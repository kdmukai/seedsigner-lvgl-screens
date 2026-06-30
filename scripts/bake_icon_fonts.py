#!/usr/bin/env python3
"""Bake the merged SeedSigner + FontAwesome inline icon font family.

Regenerates the inline/top-nav icon font used by button icons and the top-nav
back/power buttons:

    components/seedsigner/fonts/seedsigner_icons_24_4bpp{,_133x,_200x}.c

What it does, repeatably, from the canonical .otf sources:

  1. MERGE — one lv_font_conv bake combining the SeedSigner icon glyphs
     (PUA U+E900..U+E923, from seedsigner-icons.otf) with the FontAwesome glyphs
     that appear on buttons (camera/keyboard/dice, from the Font Awesome Solid otf).
     One font, so the C side renders both through ICON_FONT__SEEDSIGNER.

  2. INCLUDE PREAMBLE — lv_font_conv 1.5.x emits a bare
     `#ifdef LV_LVGL_H_INCLUDE_SIMPLE ...` block that this build can't resolve
     (it needs `lvgl.h`, not `lvgl/lvgl.h`). Rewrite it to the auto-detecting
     `__has_include` form every other font in the repo uses.

  3. INK RECENTER — the icon line box reserves descent space, but icon glyphs sit
     on the baseline (ofs_y ~ 0), so a plain box-center (lv_obj_center / *_MID)
     leaves the ink a pixel high. Rather than correct that at render time, we remove
     it here: each glyph's ofs_y is set so its INK box is vertically centered in the
     line box. After this, a plain box-center renders the icon centered, and the C
     code needs no per-glyph offset.

Idempotent: ofs_y is derived from box_h (not the previous ofs_y), so re-running
produces the same output.

Requires `lv_font_conv` on PATH (npm i -g lv_font_conv). The .otf sources live in
the sibling `seedsigner` repo by default; override with --font-dir.

Usage:
    python3 scripts/bake_icon_fonts.py [--font-dir DIR]
"""
import argparse
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(REPO, "components", "seedsigner", "fonts")

SEEDSIGNER_RANGE = "0xE900-0xE923"                  # all SeedSigner PUA icon glyphs
# camera, keyboard, die, and the six dice faces (f523-f528, used on the dice-entropy
# keyboard_screen) — all FontAwesome glyphs that appear on buttons.
FONTAWESOME_RANGE = "0xf030,0xf11c,0xf522-0xf528"

# (lv_font_conv --size, file suffix) per display profile: base=100, 133x, 200x.
VARIANTS = [(24, ""), (32, "_133x"), (48, "_200x")]

PREAMBLE_BARE = (
    '#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n'
    '#include "lvgl.h"\n'
    '#else\n'
    '#include "lvgl/lvgl.h"\n'
    '#endif'
)
PREAMBLE_HAS_INCLUDE = (
    '#ifdef __has_include\n'
    '    #if __has_include("lvgl.h")\n'
    '        #ifndef LV_LVGL_H_INCLUDE_SIMPLE\n'
    '            #define LV_LVGL_H_INCLUDE_SIMPLE\n'
    '        #endif\n'
    '    #endif\n'
    '#endif\n'
    '\n'
    '#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n'
    '    #include "lvgl.h"\n'
    '#else\n'
    '    #include "lvgl/lvgl.h"\n'
    '#endif'
)

GLYPH_DSC_RE = re.compile(
    r'\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), '
    r'\.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)'
)


def patch_preamble(path):
    src = open(path).read()
    if PREAMBLE_BARE not in src:
        raise SystemExit(f"{path}: expected lv_font_conv preamble not found")
    open(path, "w").write(src.replace(PREAMBLE_BARE, PREAMBLE_HAS_INCLUDE, 1))


def recenter_ink(path):
    """Set each glyph's ofs_y so its ink box is vertically centered in the line box."""
    src = open(path).read()
    line_height = int(re.search(r'\.line_height = (\d+)', src).group(1))
    base_line = int(re.search(r'\.base_line = (\d+)', src).group(1))

    def fix(m):
        bi, aw, bw, bh, ox, oy = (int(x) for x in m.groups())
        if bw == 0 and bh == 0:
            return m.group(0)  # reserved / empty glyph — never drawn, leave it
        # ink-center vertically: ofs_y such that (ascent - ofs_y - box_h/2) == line_h/2,
        # with ascent = line_height - base_line.
        new_oy = line_height // 2 - base_line - bh // 2
        return (f".bitmap_index = {bi}, .adv_w = {aw}, .box_w = {bw}, "
                f".box_h = {bh}, .ofs_x = {ox}, .ofs_y = {new_oy}")

    open(path, "w").write(GLYPH_DSC_RE.sub(fix, src))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--font-dir",
                    default=os.path.normpath(os.path.join(
                        REPO, "..", "seedsigner", "src", "seedsigner", "resources", "fonts")),
                    help="directory holding seedsigner-icons.otf + Font_Awesome_6_Free-Solid-900.otf")
    args = ap.parse_args()

    seedsigner_otf = os.path.join(args.font_dir, "seedsigner-icons.otf")
    fontawesome_otf = os.path.join(args.font_dir, "Font_Awesome_6_Free-Solid-900.otf")
    for f in (seedsigner_otf, fontawesome_otf):
        if not os.path.isfile(f):
            sys.exit(f"missing source font: {f}\n(pass --font-dir to point at the .otf sources)")
    if subprocess.run(["which", "lv_font_conv"], capture_output=True).returncode != 0:
        sys.exit("lv_font_conv not found on PATH (install with: npm i -g lv_font_conv)")

    for size, suffix in VARIANTS:
        name = f"seedsigner_icons_24_4bpp{suffix}"
        out = os.path.join(OUT_DIR, f"{name}.c")
        subprocess.run([
            "lv_font_conv", "--bpp", "4", "--size", str(size), "--no-compress",
            "--font", seedsigner_otf, "--range", SEEDSIGNER_RANGE,
            "--font", fontawesome_otf, "--range", FONTAWESOME_RANGE,
            "--format", "lvgl", "--lv-font-name", name, "-o", out,
        ], check=True)
        patch_preamble(out)
        recenter_ink(out)
        print(f"baked + recentered {name}.c")


if __name__ == "__main__":
    main()
