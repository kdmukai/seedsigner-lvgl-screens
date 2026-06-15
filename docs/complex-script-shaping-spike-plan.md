# Offline HarfBuzz shaping for the LVGL screens — spike-first plan

_Status: **SPIKE EXECUTED — SUCCEEDED (2026-06-15).** The de-risking spike below
was implemented and validated end-to-end (Devanagari + Nastaliq-Urdu + Thai + the
`fa` oracle + a word-boundary insertion), with **no LVGL submodule patch**. See
`docs/knowledge/offline-harfbuzz-shaping-spike-findings.md` for the result, the
resolved risks, and a non-obvious subset-closure constraint discovered. The
full-subsystem roadmap (below) is now unblocked. Concrete, finalized successor to
`docs/complex-script-shaping-plan.md` (the "agreed direction + open questions" notes).
Companion to `docs/font-farsi-rtl-plan.md`, `docs/font-tiering-plan.md`,
`docs/font-and-i18n-rendering.md`. Paths below are repo-relative._

## Context

The shared LVGL screens (this repo, consumed by Pi Zero via `seedsigner-raspi-lvgl` and
ESP32-S3 via `seedsigner-micropython-builder`) render text through `lv_tiny_ttf` (stb),
which has **no general text shaper** — only `LV_USE_BIDI` and a basic Arabic
presentation-form path (`LV_USE_ARABIC_PERSIAN_CHARS`). That presentation-form trick is
the only reason Farsi works today, and it **does not generalize**: Devanagari (Hindi — a
hard must-have for the initial LVGL release) needs reordering + conjunct glyphs that have
no Unicode codepoints; Urdu wants Nastaliq; Thai wants GPOS mark stacking. Broad
world-language coverage requires a real shaper — **HarfBuzz**.

**Decision (already made, see `docs/complex-script-shaping-plan.md`):** run HarfBuzz
**offline / at build time**, never on the signing device. Deliver **pre-shaped glyph
runs** — `(glyph_id, x_offset, y_offset, x_advance)` in final visual order — plus the
subset TTF. The device rasterizes those glyph-ids at display size (stb already does this
internally) and places them by offset. Resolution-independent; keeps the heavy,
parser-rich shaper off the device behind signed packs.

**Two findings from the design exploration de-risk the approach:**
1. **The on-device half is nearly free.** LVGL already has a glyph-id draw seam:
   `lv_draw_unit_draw_letter()` (`third_party/lvgl/src/draw/lv_draw_label.c:595`) draws
   from a caller-supplied `lv_font_glyph_dsc_t` when `dsc->g != NULL`, skipping the
   codepoint lookup; tiny_ttf's rasterizer + caches are **already keyed by glyph-id**
   (`lv_tiny_ttf.c:357/582`, `stbtt_MakeGlyphBitmap`). We bypass exactly one function
   (`ttf_get_glyph_dsc_cb`, the `stbtt_FindGlyphIndex` call). No new rasterizer, no new
   cache, and likely **no submodule patch**.
2. **Runtime variable insertion is tractable.** A full inventory of the `seedsigner/`
   app found **zero** open-ended translated-text splices (Class D), only **3** closed-set
   translated-word patterns (change/self-transfer, btc/sats, 3 network names), and the
   rest numeric/ASCII at word boundaries. Offline shaping is viable.

**Intended outcome of this plan:** a **de-risking spike** that proves the full path
end-to-end — offline HarfBuzz shape → run table → on-device glyph-id render → validated
against a libraqm reference — across the hardest scripts, before committing to the
production subsystem. The full subsystem is sketched as the follow-on roadmap.

## Scope decisions

- **Spike-first**: prove the path; do NOT build the production pipeline/plumbing yet.
- **Spike covers Devanagari (hi) + Nastaliq Urdu (ur) + Thai (th)** — maximal coverage
  (reordering, conjuncts, mark stacking, and the Nastaliq cascade — the extreme GPOS case).
- **Migrate `fa` to HarfBuzz runs as part of the spike.** `fa` is the one script that
  already renders correctly today (presentation-form path), so re-deriving it through the
  new HarfBuzz-run path gives a **built-in regression oracle**: the new path must match
  the existing `fa` render.

## Architecture (the two halves the spike validates)

### Offline half — `tools/i18n/` (Python: `uharfbuzz` + `fontTools`)
Generalizes the existing Arabic-only `tools/i18n/shaper/lv_shape.c` (LVGL shaper →
codepoints) into a HarfBuzz shaper (→ glyph runs). Python, not C: the C-tool rationale
for `lv_shape` was "emit exactly what LVGL's own shaper emits" — that constraint
evaporates here (the device won't run HarfBuzz). The pipeline is already Python and
already shells to `fontTools`.

- **Subset-then-shape** resolves the glyph-id chicken-and-egg: subset the source TTF to
  the corpus codepoints with **layout closure preserved** (`drop_layout=False` — the
  opposite of the CJK path; keeps GSUB/GPOS/GDEF + all conjunct/positional glyphs), then
  shape every needed string **against that exact subset font** so glyph-ids match what
  on-device stb sees by construction. Same mechanism the Python/raqm side relies on.
- **Shaper contract** (`tools/i18n/hb_shaper.py`, new): `(text, subset TTFont, script,
  lang, direction) → {glyphs:[{gid, cluster, x_advance, y_advance, x_offset, y_offset}],
  direction, units_per_em}` in visual order, integers in font design units. Direction is
  per-segment, determined by script (no FriBidi needed for single-direction strings;
  guard mixed-direction units with a loud error — expected: none given the inventory).

### On-device half — `components/seedsigner/`
- **Render path = Option A**: a custom draw-walk that drives the **existing**
  `lv_draw_unit_draw_letter()` per glyph-id (reusing LVGL's A8 blit, clipping, LRU
  caches). NOT a hand-rolled blitter, NOT a PUA-codepoint remap (B — can't express GPOS
  offsets, wrong for Nastaliq + mark stacking), NOT a first-class lv_font run mode (C —
  needs a submodule patch + lv_label changes).
- **Glyph-id → glyph dsc shim**: prefer the **zero-submodule-edit** route — re-init a
  `stbtt_fontinfo` over the already-resident font buffer in the render layer, compute
  box/metrics + `stbtt_ScaleForMappingEmToPixels`, fill `lv_font_glyph_dsc_t` (set
  `gid.index`, `format=A8`, `box_*`, `ofs_*`, `resolved_font`), and still fetch the
  **bitmap** from tiny_ttf's cache via the public `lv_font_get_glyph_bitmap`. Fall back
  to a ≤2-function additive `lv_tiny_ttf` shim (small tracked patch) only if metrics are
  awkward that way. The load-bearing call per glyph: `gd.g = &g; lv_draw_unit_draw_letter(
  t, &gd, &pos, font, 0, cb)`, folding GPOS `dy` into `g.ofs_y` to fit the existing y
  formula (`lv_draw_label.c:624-627`).
- **Coexistence**: Latin/CJK stay on `lv_label` unchanged. A locale flag gates a glyph-run
  pass hooked into the **single existing global screen-load hook**
  `load_screen_and_cleanup_previous()` (`seedsigner.cpp:114`), sibling to
  `apply_rtl_text_to_labels()` — builders stay shaping-agnostic. (Full-subsystem detail;
  the spike shortcuts this with a dedicated spike screen.)

## The spike (the executable deliverable)

Throwaway-grade, desktop-only, in the `screenshot_generator` harness. **Do NOT** build
the run-table-in-pack plumbing, locale-table flag, global post-pass, or manifest hashing
— those come after the render path is proven.

### Step 0 — vendor fonts + deps
- Vendor into `components/seedsigner/assets/`: **NotoSansDevanagari-Regular.ttf**
  (absent), **NotoNastaliqUrdu-Regular.ttf** (absent). `NotoSansTH` and `NotoSansAR` are
  already vendored.
- Add `tools/i18n/requirements.txt` pinning `fonttools` (4.57.0, present) + **`uharfbuzz`**
  (not installed — must add; its wheel bundles its own libharfbuzz, which pins the shaping
  engine for reproducibility). Record `hb.HARFBUZZ_VERSION_STRING` in the spike output.

### Step 1 — offline shaper (throwaway) `tools/i18n/spike_shape.py`
- Inputs: hardcoded strings exercising the hard cases —
  - **hi (Devanagari):** e.g. `हिन्दी` (pre-base i-matra reorder + `न्द` conjunct).
  - **ur (Nastaliq):** a hardcoded cascade word, e.g. `اردو` / `بٹ کوائن` (no `ur`
    catalog in the submodule → hardcoded).
  - **th (Thai):** a word with stacked vowel/tone marks (pull from `th.po`).
  - **fa (oracle):** a string already in `fa.po` (re-derive via HarfBuzz).
  - **one insertion case:** a segmented numeric template, e.g. `Seed Words: {}/{}` —
    shape the translated segments around the hole, leave the numeric value to the
    simple path (proves the word-boundary segmentation mechanism).
- Per string: `pyftsubset` the source font to its chars **keeping GSUB/GPOS**
  (`drop_layout=False`); `uharfbuzz` shape against the **subset** font with explicit
  script/lang/direction; emit (i) `spike_runs.bin` in the on-device run format (font-unit
  offsets/advances, glyph-ids into the subset TTF), and (ii) a **PIL+libraqm reference
  PNG** per string at one target px (the oracle — libraqm = the production Python app's
  exact engine).

### Step 2 — on-device-equivalent render (desktop) in `screenshot_gen`
- Add a `--shape-spike` flag / hidden spike screen that: registers each subset font via
  the existing tiny_ttf path; loads `spike_runs.bin`; for each string draws the glyph run
  via the on-device shim + draw-walk into the framebuffer; writes a PNG via the existing
  `write_png_rgb24`.
- Implement the glyph-id shim as the zero-submodule-edit route first.

### Step 3 — validation
- **Pixel-diff** each device PNG against its libraqm reference (reuse/adapt
  `tools/apps/screenshot_generator/compare_screenshots.py`). Success = same visual order,
  marks stacked at correct offsets, **Nastaliq baseline cascade present** (not flat
  Naskh), Thai marks positioned. Require structural match + small AA tolerance (stb ≠
  FreeType), plus a human eyeball side-by-side.
- **fa regression check**: the new HarfBuzz-run render of `fa` must match the existing
  presentation-form render of the same string — proves the new path doesn't regress the
  one script that works today.
- **Negative control**: render the Urdu string through the current
  presentation-form/`lv_label` path → flat Naskh / wrong, demonstrating why the run path
  is needed and that the spike actually changed the output.

## Full-subsystem roadmap (sketch — only if the spike succeeds)

1. **Productionize the shaper**: `tools/i18n/hb_shaper.py` (shaper contract) +
   `tools/i18n/shape_inventory.py` (`.po` → shaping units: plain msgstr / `CLASS_A_ENUM`
   enumerated combinations / Class B-C word-boundary segments; CI assert every
   `{}`-bearing msgid is classified, assert holes sit at word boundaries).
2. **Run table + signing**: per-locale `lang-packs/<locale>/runs.json` (keyed by English
   msgid; `kind: plain|enumerated|segmented`), sha256 in `manifest.json`, embed
   `font_sha256` so a font/run mismatch is caught on-device.
3. **`build_fontpacks.py` integration**: new `needs_complex_shaping` gate (script-block
   table: Arabic, Devanagari `U+0900-097F`, Thai `U+0E00-0E7F`, …) routing to the
   keep-layout subset + HarfBuzz path; CJK/block-range paths unchanged.
4. **Render layer**: `components/seedsigner/glyph_runs.{h,cpp}` (run-blob parse,
   `key→RunHandle` map, `glyph_run_measure`, `glyph_run_draw`); `LocaleFontEntry` gains a
   `shaping` flag + `seedsigner_locale_uses_glyph_runs()`; `apply_glyph_runs_to_labels()`
   added to `load_screen_and_cleanup_previous()` (coordinated with the RTL pass so both
   don't claim the same label); pack loaders fetch `runs.json` next to the `.ttf`.
5. **Add `hi`/`th`/`ur` to `locale_fonts.cpp`; migrate `fa`** off the presentation-form
   path onto runs.
6. **ESP32 footprint pass** (separate, on-target): flash (subset TTF + run table), PSRAM
   glyph-cache growth keyed by glyph-id (watch the documented cache-OOM spin,
   `docs/knowledge/tiny-ttf-cache-spin-root-cause.md`), incremental code size, per-string
   draw time at PX_MULTIPLIER 100/150/200.
7. **Reproducibility/signing**: pin `uharfbuzz`/`fonttools`; treat a HarfBuzz bump as a
   reviewed re-shaping event; CI rebuilds a pack twice and asserts byte-identical output.

## Risks / decisions the spike resolves
- **Glyph-id shim feasibility** without a submodule patch (zero-edit stbtt re-init vs a
  ≤2-function additive `lv_tiny_ttf` patch). The spike picks the route.
- **GPOS offset fidelity** through the `lv_draw_unit_draw_letter` y-formula (folding `dy`
  into `ofs_y`) — Nastaliq is the proof.
- **HarfBuzz↔stb visual parity** at AA tolerance (different rasterizers).
- **Bidi double-reorder** if the original UTF-8 is left in a matched label under RTL —
  the run is already in visual order, so the matched label's text must be emptied/
  suppressed (full-subsystem concern; spike uses a dedicated screen).

## Critical files
- `third_party/lvgl/src/draw/lv_draw_label.c` — `lv_draw_unit_draw_letter` glyph-id seam (~595)
- `third_party/lvgl/src/libs/tiny_ttf/lv_tiny_ttf.c` — glyph-id raster + caches (~277/357/582)
- `components/seedsigner/seedsigner.cpp` — `load_screen_and_cleanup_previous` / `apply_rtl_text_to_labels` global hook (~103-125)
- `components/seedsigner/locale_fonts.{h,cpp}` — locale table / shaping flag / `--dump-locales` manifest
- `components/seedsigner/font_registry.cpp` — `seedsigner_register_font` / resident TTF buffer (~95)
- `tools/apps/screenshot_generator/screenshot_gen.cpp` — spike host + pack loader; `compare_screenshots.py` for diffing
- `tools/i18n/build_fontpacks.py`, `po_catalog.py`, `shaper/lv_shape.c` — the pipeline this generalizes; new: `spike_shape.py` (throwaway), later `hb_shaper.py` + `shape_inventory.py`
- `components/seedsigner/assets/` — vendor NotoSansDevanagari + NotoNastaliqUrdu
