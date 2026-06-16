# Complex-script glyph-run pipeline — production constraints

_The offline HarfBuzz shaping pipeline (`tools/i18n/hb_shaper.py`,
`shape_inventory.py`, `build_fontpacks.py`'s complex-shaping path) that turns a
locale's `.po` catalog into a pre-shaped glyph-run pack
(`lang-packs/<loc>/{<loc>.ttf, runs.json, manifest.json}`). The de-risking spike
(`docs/knowledge/offline-harfbuzz-shaping-spike-findings.md`) proved the render
path on five hand-picked strings; productionizing it over the **whole catalog**
surfaced constraints the spike's clean inputs never exercised. This records the
non-obvious ones so they aren't rediscovered._

## 1. Shape per LINE — a newline has no glyph, and shaping across one is wrong

Real translations contain hard line breaks (`"You can remove\nthe SD card now"`).
HarfBuzz has no glyph for `U+000A`, so shaping a string containing one emits a
`.notdef` (silent tofu) for the newline — **and**, for cursive scripts, would join
across the break. The fix is structural, not a filter: split every string on `\n`
and shape each line independently (`hb_shaper.shape_lines`, and the same split
inside `decomposition_closure`). A run table entry therefore holds a LIST of line
runs; the device stacks them like any multi-line label.

Corollary: an empty line (`"a\n\nb"`) yields an empty HarfBuzz buffer whose
`glyph_infos` is `None`, not `[]` — `shape()` special-cases `text == ""`.

## 2. Subset closure must include HarfBuzz's *decomposition*, not just GSUB closure

(Carried from the spike, restated because the production builder MUST do it.)
`pyftsubset --layout-features='*'` keeps GSUB-reachable glyphs (conjuncts,
positional/ligature forms — none with codepoints). But HarfBuzz also decomposes at
the Unicode level **before** GSUB (Thai SARA AM `U+0E33` → `U+0E4D` + `U+0E32`).
Those targets have codepoints but aren't GSUB-reachable, so a literal subset drops
them → `.notdef`. `decomposition_closure()` recovers them: shape each line against
the FULL font, reverse-map every output gid back to a codepoint via the cmap, add
those to the subset request. A hard `.notdef`/gid-range assert
(`_assert_runs_clean`) gates the bake.

## 3. Pure-ASCII strings get NO run (and the mixed-font-line tension stays open)

Many msgstrs are untranslated (left English) or numeric/symbol-only — pure ASCII.
Shaping them would (wastefully, and slightly wrongly) draw Latin via the *script*
subset at the bumped script size. They need no shaping: classify as `ascii` and
emit no run, so the device renders them on the normal codepoint path (baked
OpenSans / fallback), exactly as embedded English is handled for the CJK packs.
This also fixed a spurious validation FAIL — Thai `"Seeds"` is untranslated, and
its Latin glyphs through NotoSansTH at 48px tripped the ink-IoU AA threshold.

**Still open (deferred to the render layer):** a MIXED string (`"BIP39 शब्द"`) has
non-ASCII, so it gets a run that shapes the WHOLE line — including `BIP39` — in the
script subset. That embedded Latin renders in the script font at the bumped size,
diverging from the CJK "embedded-English-at-English-size" design. Per-segment fonts
on one line (Latin run on OpenSans + script run on the subset) is the proper fix
and is a layout concern, not a shaping one (the spike flagged it lower-risk).

## 4. Placeholder shaping-safety is about *non-ASCII letters*, not "any letter"

`shape_inventory.classify()` segments a `{}`-template only when each hole sits at a
shape-safe boundary. The boundary is unsafe **only** if the adjacent character is a
non-ASCII letter (a complex-script letter that joins/conjoins) — then shaping the
two sides independently could diverge from shaping the joined result. A string
edge, whitespace, digit, punctuation, **or an ASCII/Latin letter** is safe: a
Latin `x` in `"Begin {}x{}"` shapes as a standalone glyph in a Devanagari/Thai/
Arabic run and never conjoins with the inserted numeric value. Treating "any
letter" as unsafe wrongly rejected that case; the real risks (a Devanagari ordinal
fused to its number, a Thai vowel abutting a hole) are correctly flagged
`unsupported` with a reason and reported loudly — never silently dropped.

## Run-table size note (for the later ESP32 footprint pass)

`runs.json` is verbose JSON (~0.5 MB for hi/th's ~330 runs; full per-glyph
records). Fine as a build/desktop artifact, but on-device JSON parsing + footprint
will want a compact binary blob (drop `cluster`/`y_advance`, pack gids+offsets) —
explicitly out of scope for the offline pipeline, tracked for the on-target pass.

## Reproducibility

`build_units` iterates `sorted(msgid)`; `runs.json` is serialized with compact
separators and no timestamps; the subset is deterministic for a pinned
`fonttools`. Verified: two builds of hi produce byte-identical `runs.json` AND
`hi.ttf`. `manifest.json` binds `font_sha256` + `runs.sha256` + the bundled
`harfbuzz_version`, so a font/run mismatch or a shaper bump is caught/visible.
