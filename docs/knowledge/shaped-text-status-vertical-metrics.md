# Shaped-text vertical metrics on `large_icon_status_screen` (fa headline/body; hi/th/ur centering)

Two non-obvious traps when measuring text height/position for complex scripts on the status screen.
Both were fixed in the `feat/screen-layout-parity` work (A11 = `e89ef1a`, A13 = `74671d1`). Companion to
[`large-icon-status-screen-parity.md`](large-icon-status-screen-parity.md) (the A6 layout/spacing parity
write-up) and [`complex-script-render-layer.md`](complex-script-render-layer.md) (the glyph-run renderer).

---

## 1. Measure `lv_label_get_text()`, NOT the logical string, for per-glyph metrics on Arabic/Persian (fa)

**Symptom (A11):** the fa status **headline** rendered too high and collided with the hero icon; fa
**body** interline spacing was looser than the Python/PIL reference (~28px vs ~22px). en was correct.

**Root cause.** fa is a *subset* locale (not a glyph-run locale): it renders through the normal LVGL
`lv_label` codepoint path with a NotoSansAR subset font. That subset holds Arabic/Persian **presentation
forms** (U+FExx), and with `LV_USE_ARABIC_PERSIAN_CHARS` enabled, `lv_label_set_text()` rewrites the
logical base-letter codepoints into those presentation forms and **stores the rewritten string** (see
`third_party/lvgl/src/widgets/label/lv_label.c` `copy_text_to_label()` → `lv_text_ap_proc`). The drawn
glyphs are the presentation forms; the only glyphs present in the subset font are the presentation forms.

The two helpers in `seedsigner.cpp` — `tight_line_space()` (body interline) and `text_top_leading()`
(headline top offset) — walk a string codepoint-by-codepoint calling `lv_font_get_glyph_dsc()` to measure
ink. They were being handed the **logical source string**. Measuring logical base letters against a
presentation-form-only subset under-counts the ink (glyphs absent → skipped; or the isolated form differs
from the joined form). Consequences:
- headline: leading computed too large → `margin_top = COMPONENT_PADDING/2 − leading` went negative →
  headline pulled UP into the icon.
- body: measured advance fell below the `−line_height/4` safety clamp → body pinned to NotoSansAR's
  over-reserved *declared* `line_height` → too loose.

**Fix.** Feed both helpers `lv_label_get_text(label)` — the stored, AP-processed presentation forms, i.e.
exactly what is drawn and what the subset font contains. No offline-baked metric, no manifest change.

**The general rule:** after `lv_label_set_text()`, `lv_label_get_text()` returns the **shaped** bytes for
Arabic/Persian. Any per-glyph measurement (height, advance, leading, overflow) must use the stored text,
never the logical input. This was already the established pattern at `components.cpp:413-416` and `:536`
(the A4/A2 button-label work); the status-screen call sites had simply missed it. For non-Arabic/Persian
locales `lv_text_ap_proc` is identity, so `lv_label_get_text()` == the input and the change is a no-op
(en + subset locales render byte-identical).

**Note:** this makes the `−line_height/4` clamp in `tight_line_space()` insurance against future
divergence rather than the active fa correction it once was.

---

## 2. A glyph-run body draws `nlines·line_height` — taller than its codepoint label box (hi/th/ur)

**Symptom (A13):** the status body text for the glyph-run locales (hi/th/ur) was vertically mis-centered
between the headline and the bottom button — biased low in some scenarios, and (the originally reported
case) biased *high*, hugging the headline, on a tight 3-line Hindi `warning` at 240×240. en centered fine.

**Root cause A — the run is a different height than the widget box.** For a glyph-run locale,
`apply_glyph_runs_to_labels()` overlays a pre-shaped A8 **mask** on the label and suppresses its codepoint
text, but it does **not** change the `lv_label` widget's size. So the flex layout — and the A10 centering
math, which read `lv_obj_get_coords(body_label)` — see the **codepoint box** height, while the user sees
the **mask**. Those differ:
- the run lays out at the font's full `lv_font_get_line_height` → drawn block height = `nlines·line_height`
  (mask is `nlines·line_height + 2·margin`, the `margin = line_height` being transparent bearing slack,
  symmetric, so it does not shift the visible ink — see `bake_run`/`bake_segmented` in `glyph_runs.cpp`).
- the codepoint box is sized from the codepoint text at the **tightened** `tight_line_space` advance
  (≈ `0.75·line_height` after the ¼ clamp for shaped scripts).

So measuring the box mis-locates the shaped body's true bottom by ≈ `(nlines−1)·0.25·line_height`.

**Fix A.** Expose the run's drawn height: `seedsigner_label_run_drawn_height(label)` returns
`mask->header.h − 2·margin` (== `nlines·line_height`), found via `find_label_run()` (which scans the
label's event descriptors for `glyph_run_draw_cb` and returns its `LabelRun*` user_data), or `−1` for a
plain codepoint label. The centering uses `content_top + run_height` as the body bottom when a run is
present. Because the accessor needs the run attached, `large_icon_status_screen` now bakes runs **early**
(mirroring the post-load RTL→attach order); a **double-attach guard** in `attach_runs()` makes the global
post-load pass a no-op for already-attached labels, so every other screen is unchanged.

**Root cause B — the centering gate was all-or-nothing.** Even with the right height, the 240×240 Hindi
case stayed top-biased. A10 only applied the shift `if (shift <= spacer_height)`. On a tight screen the
ideal half-gap can exceed the small flex spacer (measured: `shift=8 > spacer=5`), so it applied **nothing**
and the body hugged the headline.

**Fix B.** Clamp the shift to `spacer_height` instead of skipping — partial centering, moving the body down
as far as the free spacer allows (the button stays pinned because the shift comes out of the flex-grow
spacer). On tight screens the achievable shift is small (the body + icon + headline + button nearly fill
the display), but it uses all the available slack.

**Scope / safety.** en and subset locales have no run (accessor → −1 → original math) and no en status
screen hits the clamp, so en renders byte-identical. Only `large_icon_status_screen` changes, only for
shaped or tight-fit cases. Overflowing screens (spacer ≈ 0) never trigger the shift, so the A1/A9
scroll-then-buttons path is untouched.
