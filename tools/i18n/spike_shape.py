#!/usr/bin/env python3
"""
THROWAWAY spike — offline HarfBuzz pre-shaping for the LVGL screens.

This is the OFFLINE half of the de-risking spike described in
`docs/complex-script-shaping-spike-plan.md`. It is intentionally NOT the
production pipeline (no .po sweep, no manifest hashing, no signing): it proves,
end-to-end, that we can

  1. subset a complex-script TTF *keeping* its GSUB/GPOS/GDEF layout closure,
  2. shape hard strings against that exact subset with HarfBuzz (uharfbuzz),
  3. emit pre-shaped glyph runs — (gid, x/y offset, x/y advance) in VISUAL order,
     in font design units — that the device rasterizes by glyph-id, and
  4. render a libraqm reference PNG per string (the oracle: libraqm == the
     production Python app's exact engine = FreeType + HarfBuzz + FriBidi).

The companion on-device half lives in the screenshot generator
(`tools/apps/screenshot_generator/shape_spike.cpp`, driven by `--shape-spike`):
it loads `spike_runs.bin`, rasterizes each glyph-id through the *existing*
tiny_ttf/stb engine, places it by the run's offsets, and writes a PNG that
`spike_compare.py` diffs against the reference here.

Run (from repo root, in the tooling venv):
    .venv/bin/python tools/i18n/spike_shape.py

Outputs land in tools/i18n/spike_out/ :
    spike_<name>.ttf      subset font (glyph-ids the run indexes into)
    spike_runs.bin        the compact on-device run table (see RUN FORMAT below)
    spike_runs.json       same data, human-readable (debugging only)
    spike_ref_<name>.png  libraqm reference render (the oracle)
    meta.json             engine versions + canvas params (device must match)

------------------------------------------------------------------------------
RUN FORMAT (spike_runs.bin) — little-endian; mirrored by shape_spike.cpp
------------------------------------------------------------------------------
  File header
    char[4]  magic = "SSR1"
    u32      line_count
  Per line
    u16      name_len ; char[name_len] name        (utf8, e.g. "hi")
    u16      font_len ; char[font_len] font_file    (e.g. "spike_hi.ttf")
    u16      text_len ; char[text_len] text         (utf8 source — for the
                                                      old presentation-form path)
    u16      upem                                   (font design units / em)
    u16      px                                     (reference render px size)
    u8       direction (0=LTR, 1=RTL)               (informational)
    u8       reserved (0)
    u32      glyph_count
  Per glyph (all font design units)
    u32      gid
    i32      x_advance
    i32      y_advance
    i32      x_offset
    i32      y_offset
    u32      cluster                                (informational)

Glyphs are stored in final VISUAL order (HarfBuzz emits visual order for the
buffer direction). The device walks them left-to-right accumulating a cursor in
font units and converts to pixels with stb's EM->px scale — so the run is
resolution-independent (shape once, raster at any size).
"""

import json
import os
import struct
import subprocess
import sys

import uharfbuzz as hb
from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont, features

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ASSETS = os.path.join(REPO_ROOT, "components", "seedsigner", "assets")
OUT_DIR = os.path.join(os.path.dirname(__file__), "spike_out")

# Shared canvas geometry. The on-device half (shape_spike.cpp) and the libraqm
# reference MUST agree on these so the two PNGs are pixel-comparable. Generous
# vertical room for Nastaliq's diagonal baseline cascade and Devanagari/Thai
# stacked marks. White-on-black survives RGB565 round-tripping cleanly.
CANVAS_W = 800
CANVAS_H = 240
BASELINE_Y = 160
MARGIN_X = 24
PX = 48
FG = (255, 255, 255)
BG = (0, 0, 0)


# ---------------------------------------------------------------------------
# Test corpus — the hard cases (see plan "Step 1"). Each case is one render
# line. `segments` is normally the whole string; the insertion case splits the
# translated template at its {} word-boundary holes and shapes each piece
# independently, then concatenates the runs — proving offline segmentation is
# compositionally safe at spaces. `text` is always the fully-assembled string,
# shaped as one piece for the reference oracle.
# ---------------------------------------------------------------------------
def make_cases():
    return [
        # Devanagari: pre-base short-i matra reorders BEFORE its consonant, and
        # न् + द forms the न्द conjunct (a glyph with no Unicode codepoint).
        dict(name="hi", font="NotoSansDevanagari-Regular.ttf",
             script="Deva", language="hi", direction="ltr",
             segments=["हिन्दी"], text="हिन्दी"),

        # Nastaliq Urdu: "Bitcoin". The extreme GPOS case — letters cascade down
        # a steep diagonal baseline; a Naskh/presentation-form render is flat.
        dict(name="ur", font="NotoNastaliqUrdu-Regular.ttf",
             script="Arab", language="ur", direction="rtl",
             segments=["بٹ کوائن"], text="بٹ کوائن"),

        # Thai: "ต่ำ" (low) — mai-ek tone mark stacks above the base, sara-am to
        # the right; GPOS mark positioning. Real string from th.po.
        dict(name="th", font="NotoSansTH-Regular.ttf",
             script="Thai", language="th", direction="ltr",
             segments=["ต่ำ"], text="ต่ำ"),

        # Farsi (REGRESSION ORACLE): a real fa.po string. fa already renders
        # correctly today via the presentation-form path, so the new HB-run path
        # must reproduce it. Cursive Arabic (initial/medial/final joining).
        dict(name="fa", font="NotoSansAR-Regular.ttf",
             script="Arab", language="fa", direction="rtl",
             segments=["زبان"], text="زبان"),

        # Insertion: a real Hindi template with the hole MID-SENTENCE. Offline we
        # shape the template pieces around the {} and the value separately
        # (value = Devanagari digit, a "numeric"/simple insert), concatenate the
        # runs, and diff against the whole-string reference. Spaces flank the
        # hole, so the join is a clean shaping break.
        #   msgid:  "Checking address {}"
        #   hi:     "पता {} जांचा जा रहा है"
        dict(name="insertion", font="NotoSansDevanagari-Regular.ttf",
             script="Deva", language="hi", direction="ltr",
             segments=["पता ", "१", " जांचा जा रहा है"],
             text="पता १ जांचा जा रहा है"),
    ]


# ---------------------------------------------------------------------------
# Subset-then-shape. Keep the layout closure (GSUB/GPOS/GDEF + every conjunct/
# positional glyph) so the subset shapes identically to the full font and the
# glyph-ids match what on-device stb sees by construction.
#
# pyftsubset's GSUB closure pulls in glyphs reachable from the requested
# codepoints via the layout tables (conjuncts, positional/ligature forms — none
# of which have their own codepoint). But HarfBuzz ALSO applies Unicode-level
# decomposition BEFORE GSUB — e.g. the Thai shaper rewrites SARA AM (U+0E33)
# into NIKHAHIT (U+0E4D) + SARA AA (U+0E32). Those targets DO have codepoints,
# but they are not reachable by GSUB closure from U+0E33, so a naive
# subset-by-literal-text drops them and the shaped run gets .notdef boxes.
# closure_unicodes() recovers them: shape against the FULL font, reverse-map the
# resulting glyph-ids back to codepoints, and add those to the subset request.
# ---------------------------------------------------------------------------
def closure_unicodes(src_ttf, text, script, language, direction):
    """Codepoints the subset must include so the subset shapes byte-for-byte
    like the full font: the literal text plus every codepoint HarfBuzz's
    decomposition introduces (recovered by reverse-mapping the full-font run)."""
    needed = set(ord(c) for c in text)

    full = TTFont(src_ttf)
    best = full.getBestCmap()                       # {codepoint: glyph_name}
    name_to_cp = {name: cp for cp, name in best.items()}
    order = full.getGlyphOrder()                    # index -> glyph_name

    with open(src_ttf, "rb") as fh:
        glyphs, _ = shape_segment(fh.read(), text, script, language, direction)
    for g in glyphs:
        gid = g["gid"]
        if 0 <= gid < len(order):
            cp = name_to_cp.get(order[gid])
            if cp is not None:
                needed.add(cp)
    return needed


def subset_font(src_ttf, unicodes_set, out_ttf):
    unicodes = ",".join(f"U+{cp:04X}" for cp in sorted(unicodes_set))
    subprocess.run([
        sys.executable, "-m", "fontTools.subset", src_ttf,
        f"--unicodes={unicodes}",
        f"--output-file={out_ttf}",
        "--no-hinting", "--desubroutinize", "--notdef-outline",
        # '*' = keep ALL layout features (Indic akhn/half/pres/abvs/..., Arabic
        # init/medi/fina/isol/rlig/mark/mkmk, Thai marks). The opposite of the
        # CJK pack path, which drops layout entirely.
        "--layout-features=*",
        "--glyph-names",
    ], check=True)


def shape_segment(font_bytes, text, script, language, direction):
    """Shape one single-direction segment against the subset font. Returns a
    list of glyph dicts in visual order, positions in font design units."""
    face = hb.Face(font_bytes)
    font = hb.Font(face)            # default scale == upem -> font-unit positions
    buf = hb.Buffer()
    buf.add_str(text)
    buf.direction = direction       # "ltr" / "rtl"
    buf.script = script             # ISO-15924 tag, e.g. "Deva"
    buf.language = language
    hb.shape(font, buf)

    glyphs = []
    for info, pos in zip(buf.glyph_infos, buf.glyph_positions):
        glyphs.append(dict(
            gid=info.codepoint,     # post-shaping codepoint == glyph index
            cluster=info.cluster,
            x_advance=pos.x_advance, y_advance=pos.y_advance,
            x_offset=pos.x_offset,  y_offset=pos.y_offset,
        ))
    return glyphs, face.upem


def render_reference(subset_ttf, text, direction, language, out_png):
    """The oracle: render the whole assembled string with libraqm (FreeType +
    HarfBuzz + FriBidi) — the exact engine the production Python app uses."""
    img = Image.new("RGB", (CANVAS_W, CANVAS_H), BG)
    draw = ImageDraw.Draw(img)
    fnt = ImageFont.truetype(subset_ttf, PX, layout_engine=ImageFont.Layout.RAQM)
    # anchor "ls" = horizontal-left + vertical-baseline: the text's visual-left
    # edge sits at MARGIN_X, baseline at BASELINE_Y — matching the device walk.
    draw.text((MARGIN_X, BASELINE_Y), text, font=fnt, fill=FG,
              anchor="ls", direction=direction, language=language)
    img.save(out_png)


# ---------------------------------------------------------------------------
# Binary writer (mirrors RUN FORMAT above / shape_spike.cpp reader).
# ---------------------------------------------------------------------------
def write_runs_bin(path, lines):
    with open(path, "wb") as f:
        f.write(b"SSR1")
        f.write(struct.pack("<I", len(lines)))
        for ln in lines:
            name = ln["name"].encode("utf-8")
            font = ln["font_file"].encode("utf-8")
            text = ln["text"].encode("utf-8")
            f.write(struct.pack("<H", len(name))); f.write(name)
            f.write(struct.pack("<H", len(font))); f.write(font)
            f.write(struct.pack("<H", len(text))); f.write(text)
            dir_flag = 1 if ln["direction"] == "rtl" else 0
            f.write(struct.pack("<HHBB", ln["upem"], PX, dir_flag, 0))
            f.write(struct.pack("<I", len(ln["glyphs"])))
            for g in ln["glyphs"]:
                f.write(struct.pack("<Iiiii I",
                                    g["gid"],
                                    g["x_advance"], g["y_advance"],
                                    g["x_offset"], g["y_offset"],
                                    g["cluster"]))


def main():
    if not features.check("raqm"):
        print("ERROR: Pillow is built without libraqm; cannot render the oracle.",
              file=sys.stderr)
        return 1

    os.makedirs(OUT_DIR, exist_ok=True)
    lines = []
    cases = make_cases()

    print(f"HarfBuzz {hb.version_string()}  |  uharfbuzz {hb.__version__ if hasattr(hb,'__version__') else '?'}")
    for case in cases:
        src = os.path.join(ASSETS, case["font"])
        if not os.path.exists(src):
            print(f"[{case['name']}] MISSING source font: {src}", file=sys.stderr)
            return 1

        subset_name = f"spike_{case['name']}.ttf"
        subset_path = os.path.join(OUT_DIR, subset_name)
        needed = closure_unicodes(src, case["text"], case["script"],
                                  case["language"], case["direction"])
        subset_font(src, needed, subset_path)
        with open(subset_path, "rb") as fh:
            font_bytes = fh.read()

        # Shape each segment independently, concatenate in visual order. For the
        # single-segment cases this is just one shaping; the insertion case
        # proves the pieces join cleanly at the {} word boundaries.
        glyphs = []
        upem = None
        for seg in case["segments"]:
            seg_glyphs, upem = shape_segment(font_bytes, seg, case["script"],
                                             case["language"], case["direction"])
            glyphs.extend(seg_glyphs)

        # A .notdef (gid 0) in the final run means the subset is missing a glyph
        # the shaper needs — a silent tofu box. Fail loudly rather than bake it.
        notdef = sum(1 for g in glyphs if g["gid"] == 0)
        if notdef:
            print(f"[{case['name']}] ERROR: {notdef} .notdef glyph(s) in run — "
                  f"subset closure incomplete", file=sys.stderr)
            return 1

        ref_png = os.path.join(OUT_DIR, f"spike_ref_{case['name']}.png")
        render_reference(subset_path, case["text"], case["direction"],
                         case["language"], ref_png)

        lines.append(dict(name=case["name"], font_file=subset_name, upem=upem,
                          direction=case["direction"], glyphs=glyphs,
                          segments=case["segments"], text=case["text"]))
        print(f"[{case['name']}] {len(glyphs):2d} glyphs  upem={upem}  "
              f"subset={os.path.getsize(subset_path)}B  -> {os.path.basename(ref_png)}")

    write_runs_bin(os.path.join(OUT_DIR, "spike_runs.bin"), lines)
    with open(os.path.join(OUT_DIR, "spike_runs.json"), "w") as f:
        json.dump(lines, f, ensure_ascii=False, indent=2)

    meta = dict(
        harfbuzz_version=hb.version_string(),
        px=PX, canvas_w=CANVAS_W, canvas_h=CANVAS_H,
        baseline_y=BASELINE_Y, margin_x=MARGIN_X,
        fg=FG, bg=BG,
    )
    with open(os.path.join(OUT_DIR, "meta.json"), "w") as f:
        json.dump(meta, f, indent=2)

    print(f"\nWrote {len(lines)} lines -> {os.path.relpath(OUT_DIR, REPO_ROOT)}/"
          f"{{spike_runs.bin, spike_runs.json, meta.json}}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
