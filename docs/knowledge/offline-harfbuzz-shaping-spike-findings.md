# Offline HarfBuzz shaping — de-risking spike findings

_Outcome of executing `docs/complex-script-shaping-spike-plan.md`. The spike
**succeeded**: the full path — offline HarfBuzz shape → glyph-run table →
on-device glyph-id rasterization → validated against a libraqm oracle — works
end-to-end across the hardest scripts, with **no LVGL submodule patch**. This
doc records what was proven and the non-obvious constraints discovered, so the
production roadmap (plan §"Full-subsystem roadmap") can proceed from settled
ground._

## How to reproduce

```
.venv/bin/python tools/i18n/spike_shape.py        # offline: shape -> runs.bin + raqm refs
cmake --build tools/apps/screenshot_generator/build
tools/apps/screenshot_generator/build/screenshot_gen --shape-spike tools/i18n/spike_out
.venv/bin/python tools/i18n/spike_compare.py       # ink-IoU diff dev vs raqm
```

Artifacts land in `tools/i18n/spike_out/` (git-ignored): `spike_<name>.ttf`
subsets, `spike_runs.bin`, `spike_ref_*.png` (oracle), `spike_dev_*.png`
(device), `spike_old_*.png` (current presentation-form path), `spike_cmp_*.png`
(ref | dev | overlay).

## Result

| case | script | proves | ink IoU vs raqm |
|---|---|---|---|
| `hi` `हिन्दी` | Devanagari | pre-base matra **reorder** + `न्द` **conjunct** (glyph with no codepoint) | 0.85 |
| `ur` `بٹ کوائن` | Nastaliq | the **extreme GPOS case** — diagonal baseline cascade | 0.90 |
| `th` `ต่ำ` | Thai | **mark stacking** (mai-ek tone) + SARA AM decomposition | 0.82 |
| `fa` `زبان` | Arabic | cursive joining; **regression oracle** (script that works today) | 0.84 |
| `insertion` `पता १ जांचा जा रहा है` | Devanagari | **word-boundary segmentation** is compositionally safe | 0.98 |

All five pass the structural threshold (ink-mask IoU ≥ 0.80) at **(0,0)
alignment** — device and reference land at the *same* canvas pixel, no global
offset. The residual is pure anti-alias edge disagreement (stb vs FreeType);
IoU is lowest for the smallest-ink strings, where the AA band is a larger
fraction of total ink, exactly as expected. The Nastaliq overlay
(`spike_cmp_ur.png`) is near-uniform white — green (raqm) and magenta (device)
coincide across the whole cascade.

## Risks the spike resolved (plan §"Risks / decisions")

### 1. Glyph-id shim works with NO submodule patch — the zero-edit route wins
The on-device path needs to rasterize a chosen **glyph-id** (not a codepoint).
Two public LVGL seams make this free:

- **Raster by glyph-id:** `lv_font_get_glyph_bitmap(&g, NULL)` dispatches to
  tiny_ttf's `ttf_get_glyph_bitmap_cb`, which keys *only* on `g.gid.index` and
  `font->line_height` — the codepoint never enters. Fill an `lv_font_glyph_dsc_t`
  with `gid.index`, `resolved_font`, `format = LV_FONT_GLYPH_FORMAT_A8`, call it,
  read `lv_draw_buf_t::{data, header.{w,h,stride}}`, then
  `lv_font_glyph_release_draw_data(&g)`. The A8 bitmap is cached keyed on
  (gid, size) — the exact bypass the plan predicted.
- **Bounding-box offset:** the bitmap fetch returns size but **not** the box's
  baseline-relative offset (`ix0, iy0`) needed to place it; tiny_ttf only exposes
  that via its codepoint lookup. Recovered with a **metrics-only `stbtt_fontinfo`
  re-init** over the *same already-resident font buffer*
  (`tools/apps/screenshot_generator/stb_glyph_metrics.c`). Because it is the same
  stb, same bytes, and same `stbtt_ScaleForMappingEmToPixels(font_size)` scale
  tiny_ttf uses internally, the box lines up with the cached bitmap exactly.

Net: a small metrics-only stb re-init in the render layer is sufficient. The
`≤2-function additive lv_tiny_ttf patch` fallback was **not needed**.

### 2. GPOS offset fidelity through the y-formula
HarfBuzz emits per-glyph `(x_offset, y_offset, x_advance)` in font design units,
visual order. On device: accumulate a cursor in font units, convert with the stb
scale, place each glyph at `pen + offset + stb_box_origin`. The sign that matters:
HB `y_offset` is **y-up**, screen is **y-down**, so the glyph top is
`baseline + iy0 − round(y_offset·scale)`. With that, the Nastaliq y-offsets
(+335, +248, +299, −331 font units) reproduce the cascade pixel-for-pixel against
raqm. Mark stacking (Thai, Arabic marks) falls out of the same math.

### 3. HarfBuzz ↔ stb visual parity
Same shaper (HarfBuzz) on both sides — the *only* engine difference is the
rasterizer (stb vs FreeType). Confirmed: identical glyph ids, identical placement,
AA-only pixel deltas. The run table is **resolution-independent** (font units;
device multiplies by the stb scale at its chosen px), so one run serves every
PX_MULTIPLIER.

### 4. Word-boundary segmentation is compositionally safe
The `insertion` case shapes the translated template pieces around a mid-sentence
`{}` hole **independently** from the inserted value, concatenates the runs, and
matches the whole-string raqm shaping at IoU 0.98. Spaces flanking the hole are a
clean shaping break in every script, as assumed.

## Non-obvious constraint discovered: subset closure must include HarfBuzz's *decomposition*, not just GSUB closure

The plan's "subset-then-shape" keeps GSUB/GPOS/GDEF layout closure
(`pyftsubset --layout-features='*'`), which preserves conjuncts/positional/
ligature glyphs (none of which have codepoints). **That is not enough.**
HarfBuzz also applies **Unicode-level decomposition before GSUB**: the Thai
shaper rewrites SARA AM `U+0E33` → NIKHAHIT `U+0E4D` + SARA AA `U+0E32`. Those
targets *have* codepoints but are **not reachable by GSUB closure** from the
input, so a naive subset-by-literal-text drops their glyphs and the shaped run
gets silent `.notdef` tofu (it rendered `ต□□` in the first oracle pass).

Fix used (`spike_shape.py::closure_unicodes`): shape the text against the **full**
font first, reverse-map every resulting glyph-id back to a codepoint via the
font's cmap, and add those codepoints to the subset request. Conjuncts (no
codepoint) are still covered by GSUB closure. A `.notdef`-in-run assertion guards
against any remaining gap.

**Production implication:** `build_fontpacks.py`'s `needs_complex_shaping` path
must do this decomposition closure (today's `fa` path doesn't hit it because the
presentation-form corpus is enumerated by the real shaper). Affects Thai/Lao and
any script with shaper-internal decompositions. Likely already masked in the
production Python app only because real `.po` corpora happen to contain the
component characters elsewhere — a per-string or small-corpus subset would
regress. Cheap insurance: assert no `.notdef` in any baked run.

## Negative control + regression (plan §Step 3)

- **Negative control** (`spike_old_ur.png`): the *current* presentation-form path
  (`lv_text_ap` over Noto Sans Arabic Naskh, codepoint-driven, no GPOS) renders
  Urdu **flat and disjoint** — no cascade. Confirms the run path is necessary and
  that the spike genuinely changed the output.
- **fa regression** (`spike_old_fa.png` vs `spike_dev_fa.png` vs `spike_ref_fa.png`):
  all three agree for `زبان`. The new glyph-run path reproduces the one script
  that renders correctly today — no regression.

## Scope notes / what the spike deliberately did NOT do

- **Render integration:** the spike blits the A8 mask straight into a framebuffer
  to isolate "raster-by-gid + GPOS placement." Production should drive the same
  `lv_font_get_glyph_bitmap` raster through `lv_draw_unit_draw_letter` +
  a `draw_letter_cb`-style glyph callback (Option A) to reuse LVGL's clip/blend;
  that function is draw-unit-internal (needs an `lv_draw_task_t`), which is the
  plan's full-subsystem render-layer work, not a spike concern.
- **Per-segment fonts:** the insertion value is a Devanagari digit in the *same*
  font as the template. Production renders a Latin/ASCII value on the baked
  OpenSans baseline — a layout concern (a second font on the line), not a shaping
  one, and lower risk than what the spike covered.
- **No ESP32 footprint pass** (flash/PSRAM/draw-time) — separate, on-target.
- Everything under `tools/i18n/spike_out/` and the `--shape-spike` host code is
  throwaway scaffolding, kept only to re-run the proof.

## Key files

- `tools/i18n/spike_shape.py` — offline: subset-then-shape (+ decomposition
  closure), emit `spike_runs.bin` + raqm reference PNGs. Run format documented in
  its header.
- `tools/apps/screenshot_generator/shape_spike.cpp` — on-device render
  (`--shape-spike`): glyph-run path (new) + presentation-form path (current).
- `tools/apps/screenshot_generator/stb_glyph_metrics.{c,h}` — metrics-only stb
  re-init for glyph-id bounding boxes (the zero-edit shim).
- `tools/i18n/spike_compare.py` — ink-IoU structural diff + side-by-side composites.
- `tools/i18n/requirements.txt` — pinned `uharfbuzz==0.55.0` (libharfbuzz 14.2.1).
- `components/seedsigner/assets/NotoSansDevanagari-Regular.ttf`,
  `NotoNastaliqUrdu-Regular.ttf` — vendored sources.
