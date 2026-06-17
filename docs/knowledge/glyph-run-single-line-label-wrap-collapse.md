# Glyph-run word-wrap collapsed complex-script single-line labels to their ASCII prefix

## Symptom
On a `button_list` screen in a shaping locale, a complex-script **top_nav title**
rendered only its leading ASCII characters and silently dropped the script text.
Concretely, the Thai title `1 อินพุต` (msgid `1 input`) rendered as just `1`, while
the **identical string rendered in full as a button label** on the same screen.
Devanagari (`hi`) and Urdu (`ur`) titles looked fine, so it presented as
Thai-specific — but it was not really about Thai.

## Root cause
The glyph-run render pass (`glyph_runs.cpp::attach_runs`) word-wraps a matched run
to the label's content width:

```cpp
int wrap_width = g_table.rtl ? 0 : lv_obj_get_content_width(obj);
run = bake_run(..., wrap_width);
```

`bake_run` → `wrap_line` splits the shaped run at its offline ICU break marks
whenever it overflows `wrap_width`. That is correct for genuinely wrapping body
text, but it was applied to **every** LTR label regardless of long mode.

The `top_nav` title is `LV_LABEL_LONG_SCROLL_CIRCULAR` — a **single-line** mode —
and its content box is the narrow region clipped between the back/power buttons
(`components.cpp::top_nav`). When the shaped run is wider than that narrow region,
`wrap_line` cut it at the space break into two visual lines (`1` / `อินพุต`). The
baked mask was then two lines tall, but a single-line label only shows line 0 — so
the script text on line 1 was painted below the title and never seen.

Why it looked Thai-specific:
- It only triggers when the run **overflows the title region**. The Thai run was
  wide enough to overflow; the Devanagari `1 इनपुट` run happened to fit, so `hi`
  never wrapped. `ur` is RTL, and the RTL path already passed `wrap_width = 0`.
- So the bug is "single-line label whose run overflows its box," not "Thai." Any
  script could hit it with a long enough title.

This is independent of the host platform — it reproduces identically on the desktop
native build and on the Pi Zero (ARMv6) build, because it lives entirely in the
shared screen layer. It surfaced during the first on-device i18n render test.

## Fix
Only word-wrap labels that actually wrap their codepoint text — i.e.
`LV_LABEL_LONG_WRAP`. Every other long mode (`SCROLL`, `SCROLL_CIRCULAR`, `CLIP`,
`DOT`) is single-line and must keep the run on one line:

```cpp
const bool wraps =
    (lv_label_get_long_mode(obj) == LV_LABEL_LONG_WRAP) && !g_table.rtl;
const int wrap_width = wraps ? lv_obj_get_content_width(obj) : 0;
```

`wrap_width` is then passed to both `bake_run` and `bake_segmented`. This mirrors
LVGL's own behavior: a `SCROLL_CIRCULAR`/`CLIP` label never wraps its plain text
either — it scrolls or clips a single line — so the glyph-run path should not wrap
it. The only `LV_LABEL_LONG_WRAP` label in the screens today is the multi-line body
text in `seedsigner.cpp` (the wrap feature's intended consumer), which is unchanged.

A too-long shaped single-line title now clips at the box edge instead of collapsing
to its ASCII prefix. (It does not scroll-animate — the run mask is static and the
label's own scrolling text is suppressed — but the text is at least present and
correct, matching how the plain-codepoint path degrades.)

## Validation
- Dev-box native build: `th` title renders full; `hi`/`ur` unchanged (no regression).
- Pi Zero (ARMv6) build: same — confirmed on hardware via framebuffer capture.
- The identical string as title vs. button (the original discriminator) now renders
  identically.

See also `complex-script-run-pipeline.md` (the offline shaping + on-device run model).
