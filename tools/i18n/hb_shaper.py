#!/usr/bin/env python3
"""
hb_shaper.py — offline HarfBuzz shaping for the LVGL screens (production).

Generalizes the de-risking spike (`spike_shape.py`, throwaway) into a reusable
shaper contract that `build_fontpacks.py` uses to pre-shape complex scripts
(Devanagari/Hindi, Thai, Nastaliq/Urdu, Arabic) into glyph-run tables. The device
rasterizes those runs BY GLYPH-ID (stb already keys on gid internally) and places
each glyph by its GPOS offset — no shaper ever ships to the signing device.

Pipeline = SUBSET-THEN-SHAPE (resolves the glyph-id chicken-and-egg):

  1. Subset the source TTF to the corpus codepoints KEEPING layout closure
     (`--layout-features=*` ⇒ GSUB/GPOS/GDEF + every conjunct/positional/ligature
     glyph, none of which have their own codepoint) PLUS HarfBuzz's pre-GSUB
     decomposition targets (see `decomposition_closure`). The subset then shapes
     byte-for-byte like the full font, and its glyph-ids match what on-device stb
     sees by construction.
  2. Shape each string against THAT EXACT subset with HarfBuzz, emitting glyph
     runs `(gid, x/y offset, x/y advance, cluster)` in final VISUAL order, in font
     design units (resolution-independent — the device scales at its chosen px).

This module is mechanism-only: callers (build_fontpacks / shape_inventory) own the
locale→(script, language, direction) policy and pass those in. See
`docs/complex-script-shaping-spike-plan.md` and
`docs/knowledge/offline-harfbuzz-shaping-spike-findings.md`.
"""

import subprocess
import sys

import uharfbuzz as hb
from fontTools.ttLib import TTFont


def harfbuzz_version():
    """The bundled libharfbuzz version — recorded in pack manifests so a shaper
    bump is a visible, reviewable re-shaping event (reproducibility / signing)."""
    return hb.version_string()


# ---------------------------------------------------------------------------
# Core shaping
# ---------------------------------------------------------------------------
def shape(font_bytes, text, script, language, direction):
    """Shape ONE single-direction segment against `font_bytes` (the subset TTF).

    Returns (glyphs, upem):
      glyphs : list of dicts in VISUAL order, positions in font design units —
               {gid, cluster, x_advance, y_advance, x_offset, y_offset}
      upem   : font units per em (the run's unit scale; device multiplies by the
               stb EM→px scale at render time).

    `script`    : ISO-15924 tag, e.g. "Deva", "Thai", "Arab".
    `language`  : BCP-47-ish tag, e.g. "hi", "th", "ur", "fa".
    `direction` : "ltr" or "rtl" (HarfBuzz emits visual order for this direction).
    """
    face = hb.Face(font_bytes)
    if text == "":
        return [], face.upem        # empty line (e.g. between two '\n') — no glyphs
    font = hb.Font(face)            # default scale == upem -> font-unit positions
    buf = hb.Buffer()
    buf.add_str(text)
    buf.direction = direction
    buf.script = script
    buf.language = language
    hb.shape(font, buf)

    glyphs = []
    for info, pos in zip(buf.glyph_infos, buf.glyph_positions):
        glyphs.append(dict(
            gid=info.codepoint,     # post-shaping "codepoint" == glyph index
            cluster=info.cluster,
            x_advance=pos.x_advance, y_advance=pos.y_advance,
            x_offset=pos.x_offset,  y_offset=pos.y_offset,
        ))
    return glyphs, face.upem


def shape_lines(font_bytes, text, script, language, direction):
    """Shape a possibly multi-line string: split on '\\n' and shape each line
    independently. A newline is a HARD line break the renderer stacks vertically;
    HarfBuzz has no glyph for it, so shaping across one would emit .notdef tofu and
    (for cursive scripts) wrongly join across the break. Returns (lines, upem)
    where `lines` is a list of glyph runs (one per line, same order as the text)."""
    lines = []
    upem = None
    for ln in text.split("\n"):
        glyphs, upem = shape(font_bytes, ln, script, language, direction)
        lines.append(glyphs)
    return lines, upem


def count_notdef(glyphs):
    """Number of .notdef (gid 0) glyphs in a run. A non-zero count means the
    subset is missing a glyph the shaper needs — a silent tofu box. Callers MUST
    treat any .notdef as a hard error rather than bake it (assert this offline)."""
    return sum(1 for g in glyphs if g["gid"] == 0)


# ---------------------------------------------------------------------------
# Subset closure (the non-obvious part — see spike findings §"decomposition")
# ---------------------------------------------------------------------------
def _gid_to_codepoint(ttf):
    """{glyph_id -> codepoint} for the glyphs that DO have a codepoint, via the
    font's best cmap + glyph order. Conjuncts/positional forms (no codepoint) are
    absent here on purpose — GSUB closure covers those during subsetting."""
    best = ttf.getBestCmap()                    # {codepoint: glyph_name}
    name_to_cp = {name: cp for cp, name in best.items()}
    order = ttf.getGlyphOrder()                 # index -> glyph_name
    return {i: name_to_cp[name] for i, name in enumerate(order) if name in name_to_cp}


def decomposition_closure(src_ttf, texts, script, language, direction):
    """Codepoints the subset must include so it shapes byte-for-byte like the full
    font: the literal text PLUS every codepoint HarfBuzz introduces via Unicode
    decomposition BEFORE GSUB.

    pyftsubset's `--layout-features=*` closure pulls in GSUB-reachable glyphs
    (conjuncts, positional/ligature forms — none with their own codepoint). But
    HarfBuzz ALSO decomposes at the Unicode level first — e.g. the Thai shaper
    rewrites SARA AM (U+0E33) into NIKHAHIT (U+0E4D) + SARA AA (U+0E32). Those
    targets HAVE codepoints but are NOT reachable by GSUB closure from U+0E33, so a
    naive subset-by-literal-text drops them and the run gets .notdef tofu.

    Recover them by shaping each `texts` entry against the FULL font and reverse-
    mapping every resulting glyph-id back to a codepoint. `texts` is the whole
    corpus (all strings that will be shaped against the subset), so the closure is
    complete for the pack.
    """
    needed = set()
    for t in texts:
        needed.update(ord(c) for c in t)

    full = TTFont(src_ttf)
    gid2cp = _gid_to_codepoint(full)

    with open(src_ttf, "rb") as fh:
        full_bytes = fh.read()
    for t in texts:
        # Shape per line (split on '\n') exactly as run generation will, so the
        # closure reflects what actually gets shaped — never the newline itself.
        for line in t.split("\n"):
            glyphs, _ = shape(full_bytes, line, script, language, direction)
            for g in glyphs:
                cp = gid2cp.get(g["gid"])
                if cp is not None:
                    needed.add(cp)
    return needed


def subset_keep_layout(src_ttf, unicodes_set, out_ttf):
    """Subset `src_ttf` to `unicodes_set` KEEPING all layout tables/features →
    `out_ttf`. The opposite of the CJK pack path (which drops layout): complex
    scripts need GSUB/GPOS/GDEF preserved so the subset shapes like the source."""
    unicodes = ",".join(f"U+{cp:04X}" for cp in sorted(unicodes_set))
    subprocess.run([
        sys.executable, "-m", "fontTools.subset", src_ttf,
        f"--unicodes={unicodes}",
        f"--output-file={out_ttf}",
        "--no-hinting", "--desubroutinize", "--notdef-outline",
        # '*' = keep ALL layout features (Indic akhn/half/pres/abvs/..., Arabic
        # init/medi/fina/isol/rlig/mark/mkmk, Thai marks).
        "--layout-features=*",
        "--glyph-names",
    ], check=True)


def subset_glyph_count(ttf_path):
    """Number of glyphs in a (subset) font — used to assert every emitted gid is
    in range for the shipped TTF before baking a run table."""
    return len(TTFont(ttf_path).getGlyphOrder())
