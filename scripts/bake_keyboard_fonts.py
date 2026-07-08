#!/usr/bin/env python3
"""Bake the Inconsolata fixed-width keyboard / text-entry LVGL fonts.

Regenerates the six compiled-in monospace fonts used by the keyboard, text entry, and the
monospace readouts (xpub value, review-passphrase):

    components/seedsigner/fonts/inconsolata_semibold_{22,24}_4bpp{,_133x,_200x}.c

Two size roles, each emitted at three display-profile sizes (base=100%, 133x, 200x):

  * 22 px "candidate" font — Inconsolata-SemiBold, ASCII only.
  * 24 px "keyboard" font  — Inconsolata-SemiBold ASCII + the handful of SeedSigner PUA
                             icon glyphs that appear as keyboard keys (check, chevron
                             left/right, backspace, space).

The Inconsolata range is printable ASCII only. The review-passphrase screen reveals
leading / trailing / doubled spaces by custom-drawing a solid block over each space (no
font glyph is used), so no space-marker glyph is baked. (Earlier bakes added U+2589 ▉ or
U+2423 ␣ for that reveal; ▉ inflated the font's line_height and regressed monospace layouts
like psbt_math, and a hollow box reads as a tofu rendering-error — see INCONSOLATA_RANGE.)

Post-bake, the lv_font_conv 1.5.x include preamble (a bare `#ifdef LV_LVGL_H_INCLUDE_SIMPLE`
block that needs `lvgl.h`, not `lvgl/lvgl.h`) is rewritten to the auto-detecting
`__has_include` form the rest of the repo uses — the same fixup bake_icon_fonts.py applies.

Requires `lv_font_conv` on PATH (npm i -g lv_font_conv). The .ttf/.otf sources live in the
sibling `seedsigner` repo by default; override with --font-dir.

Usage:
    python3 scripts/bake_keyboard_fonts.py [--font-dir DIR]
"""
import argparse
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(REPO, "components", "seedsigner", "fonts")

# Inconsolata printable ASCII only. The review-passphrase space-reveal marker is NOT a font
# glyph — it is a solid block custom-drawn over each space (see passphrase_space_block_cb in
# seedsigner.cpp). An earlier bake added U+2589 (▉ block) for that reveal, but its full-cell
# glyph (ink ≈ -24..+10 vs ASCII -17..+5) inflated the baked line_height 25->34 and regressed
# every layout that reads it (e.g. psbt_math row spacing). Keeping the range ASCII-only holds
# line_height at the ASCII value; no space-marker glyph is baked.
INCONSOLATA_RANGE = "0x20-0x7E"
# SeedSigner PUA icon glyphs that appear as keyboard keys (check, chevron L/R, backspace,
# space) — merged into the 24 px keyboard font only.
KEYBOARD_ICON_RANGE = "0xE905,0xE909,0xE90A,0xE922,0xE923"

# role stem, whether to merge the keyboard icons, and the (base, 133x, 200x) px sizes.
ROLES = [
    ("inconsolata_semibold_22_4bpp", False, (22, 29, 44)),
    ("inconsolata_semibold_24_4bpp", True,  (24, 32, 48)),
]
PROFILE_SUFFIXES = ["", "_133x", "_200x"]

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


def patch_preamble(path):
    src = open(path).read()
    if PREAMBLE_BARE not in src:
        raise SystemExit(f"{path}: expected lv_font_conv preamble not found")
    open(path, "w").write(src.replace(PREAMBLE_BARE, PREAMBLE_HAS_INCLUDE, 1))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--font-dir",
                    default=os.path.normpath(os.path.join(
                        REPO, "..", "seedsigner", "src", "seedsigner", "resources", "fonts")),
                    help="directory holding Inconsolata-SemiBold.ttf + seedsigner-icons.otf")
    args = ap.parse_args()

    inconsolata_ttf = os.path.join(args.font_dir, "Inconsolata-SemiBold.ttf")
    seedsigner_otf = os.path.join(args.font_dir, "seedsigner-icons.otf")
    for f in (inconsolata_ttf, seedsigner_otf):
        if not os.path.isfile(f):
            sys.exit(f"missing source font: {f}\n(pass --font-dir to point at the .ttf/.otf sources)")
    if subprocess.run(["which", "lv_font_conv"], capture_output=True).returncode != 0:
        sys.exit("lv_font_conv not found on PATH (install with: npm i -g lv_font_conv)")

    for stem, merge_icons, sizes in ROLES:
        for size, suffix in zip(sizes, PROFILE_SUFFIXES):
            name = f"{stem}{suffix}"
            out = os.path.join(OUT_DIR, f"{name}.c")
            cmd = [
                "lv_font_conv", "--bpp", "4", "--size", str(size), "--no-compress",
                "--font", inconsolata_ttf, "--range", INCONSOLATA_RANGE,
            ]
            if merge_icons:
                cmd += ["--font", seedsigner_otf, "--range", KEYBOARD_ICON_RANGE]
            cmd += ["--format", "lvgl", "--lv-font-name", name, "-o", out]
            subprocess.run(cmd, check=True)
            patch_preamble(out)
            print(f"baked {name}.c")


if __name__ == "__main__":
    main()
