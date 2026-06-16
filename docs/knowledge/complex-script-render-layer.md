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

## Word-wrap: offline word marks, dumb on-device fit

Long lines wrap at **offline-computed word boundaries**, keeping the
linguistic-intelligence-offline / dumb-device split. The offline pipeline runs
each line through **ICU's dictionary-aware line `BreakIterator`** (PyICU over
system libicu) — real WORD boundaries even for the no-space scripts
(Thai/Lao/Khmer/Burmese), spaces elsewhere — and emits a per-line `breaks` array
(glyph indices) in `runs.json`. ICU offsets and HarfBuzz clusters are both
codepoint indices (all BMP), so a boundary at codepoint `c` maps to the first
glyph with `cluster == c`. The `runs.json` line shape is now
`{"glyphs": [...], "breaks": [...]}`; `icu_version` is recorded in the manifest so
an ICU bump is a reviewed re-segmentation event (like `harfbuzz_version`).

On device, `bake_run` greedy-fits each logical line to the label's content width
(forced final via `lv_obj_update_layout` before the pass), cutting only at the
`breaks`, and trims a trailing SPACE glyph at the cut (`stb_metrics_glyph_index(' ')`
— ICU folds a word's following space into that word). All the language knowledge
is the integer `breaks` list; the device grew no dictionary. This *beats* the
production Python app, which only `str.split()`s on spaces — so it overflows a
long space-less Thai phrase, while we break it at real words.

Known limitations:
- **RTL (ur) is not wrapped.** A visual-order RTL run needs right-anchored
  breaking to put the first-read words on the top line, so `ur` passes
  `wrap_width = 0` and `line_break_indices` returns `[]` for RTL. Acceptable while
  `ur` is a removable stub; revisit when a real Urdu translation lands.
- **Mixed Latin+script lines** shape the whole line — including embedded `BIP-39`
  — through the script subset at the bumped size, not the OpenSans baseline (the
  documented mixed-font-line tension). Renders correctly, slightly different
  metrics.
- **ESP32 footprint** is unaddressed here: `runs.json` is verbose JSON, and
  `stb_glyph_metrics` compiles a second stb copy. A compact binary run blob and
  (optionally) carrying glyph boxes in the run to drop the on-device stb are the
  on-target pass.

## Segmented (`{}`-template) runs: device-side value insertion

A label built from a `{}`-template holds the value-FILLED string at runtime (e.g.
`सीड शब्द #5`), not the template (`सीड शब्द #{}`) the offline run table is keyed by.
So these can't be matched by whole-string lookup. The offline pipeline classifies
them `segmented` and emits ordered `parts` — shaped literal runs (`{lit, glyphs}`)
interleaved with hole tokens (`{hole}`) — at SHAPE-SAFE boundaries (a hole never
abuts a joining script letter; see `complex-script-run-pipeline.md` §4). The device
half (`glyph_runs.cpp`):

1. **Parse** segmented entries into `RunTable::segmented` (a small list — ~26 for
   `hi`), kept separate from the whole-string `by_text` map.
2. **Match** (only when `by_text` misses, **LTR only**): `match_segmented` walks the
   template's literal anchors against the label text — leftmost-greedy, byte-level
   (UTF-8 is self-synchronising and literals start/end on codepoint boundaries) —
   and extracts each hole's inserted value.
3. **Render each value two ways.** A hole value can be an integer/ASCII string OR a
   translated text snippet (`Transaction's {} address could not be verified...` is
   filled with `_("change")`/`_("self-transfer")`). So a value is first looked up in
   the **plain run table** (`by_text`): a hit means it is its own translated msgid →
   draw it SHAPED. A miss falls to the **codepoint path** (`lv_font_get_glyph_dsc`
   over the label font's fallback chain) — integers and untranslated-English values
   (e.g. a `{network}` left as `Mainnet`) render via the OpenSans floor, exactly as
   embedded English does elsewhere. This is why the same template renders correctly
   whether the snippet is translated (`मेननेट`, shaped) or not (`Mainnet`, codepoint).
4. **Flatten + wrap + bake.** Literals and values are flattened into one glyph
   sequence (`FlatGlyph`: shaped-by-gid OR codepoint-by-dsc), word-wrapped to the
   label width, and baked into the same multi-line A8 `LabelRun` the plain path
   uses — so the draw/recolor/alignment path is shared verbatim.

Why these choices:
- **`by_text`-first, segmented-as-fallback** keeps whole strings on the cheaper
  exact-match path; segmented matching only runs on the ~handful of labels that miss.
- **Concatenating independently-shaped literal + value is visually exact** because
  the offline classifier guarantees shape-safe boundaries (the spike proved
  word-boundary concatenation); no re-shaping on device.
- **Wrap marks are computed on device (break-after-space), not offline.** Segmented
  literals carry no offline `breaks` (the value widths are runtime-dependent), so
  `bake_segmented` allows a break before any glyph whose predecessor is a space —
  the word-boundary rule for the space-separated scripts (Devanagari/Latin/Arabic)
  that actually appear as segmented body text. (An earlier attempt also broke at
  every part edge; that orphaned an opening `(` before a `{network}` hole, since the
  space sits *before* the paren — break-after-space alone groups `(Mainnet)`
  correctly.) The no-space scripts (Thai/…) essentially never appear as `{}`
  templates and would simply not wrap.
- **RTL is skipped** (`!g_table.rtl`): the literal/value order is laid out
  left-to-right, and `ur` (the only RTL locale) is a removable stub with no
  segmented entries.

## Validated

Desktop screenshot generator, `--locale {hi,th,ur}` over the localized scenarios:
Devanagari conjuncts/reorder, Thai mark stacking, and the Nastaliq RTL cascade all
render correctly on real screens; ASCII (keyboard, text entry) and English are
untouched (double-gated by `seedsigner_locale_uses_glyph_runs()` + a loaded
table). Glyph placement is the spike's math (validated to ink-IoU 0.82–0.98 vs the
libraqm oracle); only the compositor changed (LVGL `lv_draw_image` vs raw blit).

Segmented runs validated on `hi` (all four resolutions) with value-filled labels:
integer holes (`सीड शब्द #5`, `सिक्का उछाल 2/3`, `2 / 3 मल्टीसिग`), translated-snippet
holes (the change/self-transfer address warnings — `चेंज`/`सेल्फ-ट्रांसफर` inserted
SHAPED via the `by_text` lookup), and the network-mismatch warning with `{network}`
both translated (`मेननेट`, shaped) and left as untranslated English (`Mainnet`,
codepoint). Long body text wraps at word boundaries; titles/headlines stay single
line. The plain path and the offline oracle gate (`validate_runs.py`, hi/th/ur)
are unchanged and still pass.
