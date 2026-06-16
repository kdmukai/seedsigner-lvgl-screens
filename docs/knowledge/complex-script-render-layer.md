# Complex-script glyph-run render layer (on-device half)

_How the LVGL screens draw the offline-shaped glyph runs
(`lang-packs/<loc>/runs.json`, see `complex-script-run-pipeline.md`) for
Devanagari / Thai / Nastaliq. This is the on-device counterpart to the offline
pipeline and the production realization of the de-risking spike
(`offline-harfbuzz-shaping-spike-findings.md`). Module:
`components/seedsigner/glyph_runs.{h,cpp}` + `stb_glyph_metrics.{c,h}`._

## The pipeline, on device

1. The host pushes the active locale's run table in via
   `seedsigner_set_glyph_runs(runs_json, len)` (alongside `seedsigner_set_locale`
   + the font registration seam). It is parsed into a `translated-text → glyph
   run` map.
2. After a screen is built, the single global post-pass
   `load_screen_and_cleanup_previous()` calls `apply_glyph_runs_to_labels()`
   (sibling to `apply_rtl_text_to_labels()`). It walks the label tree; for every
   label drawn with a **registered shaping-locale script font** whose text
   matches a run, it bakes the run and takes over the label's rendering.
3. Per matched label: each glyph is rasterized **by glyph-id** through the same
   tiny_ttf engine the rest of the UI uses (`lv_font_get_glyph_bitmap`, keyed on
   `gid.index`), composited (font-unit pen + GPOS offsets, the spike's proven
   placement math) into one **A8 alpha mask** of the whole block. The label's own
   codepoint text is suppressed (`text_opa = LV_OPA_TRANSP`).
4. A `LV_EVENT_DRAW_MAIN_END` callback draws the mask via `lv_draw_image` with
   `recolor =` the label's **live** text color; a `LV_EVENT_DELETE` callback frees
   it. (See "Why this shape" below.)

## Why this shape, not the others

- **Draw seam.** The plan named `lv_draw_unit_draw_letter()` (the one LVGL fn
  that draws a *caller-supplied* glyph dsc, i.e. by gid). It is **draw-unit
  internal** — it needs an `lv_draw_task_t`, which a widget draw event does not
  have. The public letter/character draw APIs only take a *codepoint*, so they
  can't express a pre-shaped gid. The realization that honors the plan's Option A
  intent (reuse LVGL clip/blend, draw by gid, **no submodule patch**) is what
  LVGL itself does for rotated glyphs (`lv_draw_sw_letter.c`): hand an **A8
  bitmap to `lv_draw_image` with `recolor`**. An A8 image is an alpha mask filled
  with `recolor` (`lv_draw_sw_img.c:240`), exactly a glyph blit.
- **Bake once, recolor at draw time.** The mask is color-agnostic (coverage
  only), baked once at screen-load. Recoloring happens in the draw event from
  `lv_obj_get_style_text_color(label, MAIN)` — which resolves the label's current
  (inherited, state-aware) color — so **focus highlighting still works** without
  re-baking.
- **Glyph-cache lifetime is a non-issue** because we copy each glyph's A8 into our
  *own* mask buffer and `release` the tiny_ttf cache entry immediately; the mask
  is owned by the label and freed on delete. (Drawing the cached glyph buffers
  directly via async image tasks would pin/race the cache — avoided.)
- **Layout is untouched.** The label still lays out from its (wrong, codepoint)
  text, so flex sizing/scroll/centering all work; we only paint over its box and
  align the block within it via the label's own `text_align`.

## Glyph bounding boxes: `stb_glyph_metrics`

`lv_font_get_glyph_bitmap` returns the A8 bitmap + size but **not** the box's
baseline-relative offset (tiny_ttf only exposes that through its codepoint
lookup — the path we bypass). Recovered with a **metrics-only `stbtt_fontinfo`
re-init over the same resident subset bytes** (`stb_glyph_metrics.c`,
`STBTT_STATIC` so its stb copy stays private to the TU). Same stb, same bytes,
same `stbtt_ScaleForMappingEmToPixels(px)` scale tiny_ttf uses → the box lines up
with the cached bitmap exactly. The render layer gets the bytes + px from
`font_registry.h` (`seedsigner_registered_font_{bytes,px}`), which now retains the
caller's subset buffer per registration.

## The Arabic presentation-form gotcha (the ur tofu bug)

`LV_USE_ARABIC_PERSIAN_CHARS` rewrites Arabic label text into **presentation
forms** (U+FExx) at `lv_label_set_text` time — so a finished Urdu label *stores*
`ہﻭﻡ`, not the logical `ہوم` the run table is keyed by. Naive keying silently
missed every RTL label, leaving them on the codepoint path, which renders the
gid-shaped Nastaliq subset as **tofu** (the subset's cmap has no presentation
forms). Fix: key the run map through **the same transform** (`lv_text_ap_proc`,
`glyph_runs.cpp::ap_form`). It's a no-op for non-Arabic scripts (Devanagari/Thai/
Latin pass through), so one keying path matches what every locale's labels
actually hold. (Relies on `LV_USE_ARABIC_PERSIAN_CHARS` being on — it is, for the
`fa` presentation-form path.)

## A subtle self-inflicted bug worth remembering

`lv_draw_image`'s `opa` is *separate* from the A8 mask's coverage. We suppress
the label text with `text_opa = TRANSP`; reading that same `text_opa` back into
`img.opa` drew the run at opacity 0 (invisible — looked exactly like a failed
draw). The run mask must be drawn at `opa = LV_OPA_COVER`; coverage comes from the
mask, color from `recolor`.

## Known limitations (follow-ups)

- **Long body text does not wrap.** A run is one shaped line per logical
  (`\n`-split) line. LVGL would wrap a long line to the label width, but a
  pre-shaped, visual-order run can't be naively re-wrapped, and the device width
  varies by profile so it can't be pre-wrapped offline. Long paragraphs overflow
  and clip; titles / buttons / menu items / short status (the bulk of the UI) are
  fine. The tractable fix is **device-side greedy word-wrap at space-glyph
  boundaries** — spaces are a safe break in every script (words don't join across
  them), so the flat run can be split into width-fitting lines and re-baked.
- **Segmented (`{}`-template) runs are parsed-skipped.** The device label holds
  the value-filled string, not the template, so matching needs device-side
  template matching; until then those labels fall back to the codepoint path.
  (26/337 entries in `hi`; all value insertions.)
- **Mixed Latin+script lines** shape the whole line — including embedded `BIP-39`
  — through the script subset at the bumped size, not the OpenSans baseline (the
  documented mixed-font-line tension). Renders correctly, slightly different
  metrics.
- **ESP32 footprint** is unaddressed here: `runs.json` is verbose JSON, and
  `stb_glyph_metrics` compiles a second stb copy. A compact binary run blob and
  (optionally) carrying glyph boxes in the run to drop the on-device stb are the
  on-target pass.

## Validated

Desktop screenshot generator, `--locale {hi,th,ur}` over the localized scenarios:
Devanagari conjuncts/reorder, Thai mark stacking, and the Nastaliq RTL cascade all
render correctly on real screens; ASCII (keyboard, text entry) and English are
untouched (double-gated by `seedsigner_locale_uses_glyph_runs()` + a loaded
table). Glyph placement is the spike's math (validated to ink-IoU 0.82–0.98 vs the
libraqm oracle); only the compositor changed (LVGL `lv_draw_image` vs raw blit).
