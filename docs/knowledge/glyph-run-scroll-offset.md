# Glyph-run labels can scroll — honoring `label->offset.x` in the mask draw

**Scope:** `glyph_run_draw_cb` (components/seedsigner/glyph_runs.cpp). This is the
"Task 0" foundational fix from `.claude/plans/pre-upstream-pr-hardening.md`: make the
shaped (hi / th / ur) text mask follow LVGL's scroll machinery so a marquee/scroll
animation actually moves the glyphs, instead of the mask sitting still.

## The problem

Shaped locales don't draw their text the normal way. The label's codepoint text is
made transparent (`text_opa = TRANSP`) and a pre-baked A8 alpha mask of the whole
shaped run is painted from a `LV_EVENT_DRAW_MAIN_END` callback (`glyph_run_draw_cb`).
That callback positioned the mask purely from the label's content coords + the run's
advance — it **ignored `label->offset.x`**.

`label->offset.x` is the field LVGL animates for `LONG_SCROLL` / `LONG_SCROLL_CIRCULAR`
marquees (and `offset.y` for vertical scroll). The codepoint draw path adds it; the
glyph-run path did not. So a shaped title set to `LONG_SCROLL_CIRCULAR` had a *running*
offset animation that moved nothing — the mask stayed put and an overflowing shaped
title just center-clipped to its middle.

## The fix (LTR only)

In `glyph_run_draw_cb`, for left-to-right shaping locales:

1. **Honor the offset** — translate the mask draw area by `label->offset.x` /
   `offset.y`. It is 0 for the static/fitting case, so non-scrolling labels are
   byte-identical.
2. **Start-justify overflowing scroll/clip runs** — an overflowing single-line run in
   `LONG_CLIP` / `LONG_SCROLL` / `LONG_SCROLL_CIRCULAR` is anchored to its start edge
   (left for LTR), mirroring LVGL's own `draw_main`, which forces CENTER/RIGHT → start
   when the text overflows in a scroll mode. (`LONG_DOT` headline and `LONG_WRAP` body
   are excluded.)
3. **Clip to the content box** — when an overflowing run rides a scroll/clip mode, the
   draw is clipped to the label's content box (`lv_area_intersect` of the content
   coords with the layer clip), so the scrolled-out tail and the wrap copy never bleed
   past the label edges. Matches LVGL's SCROLL/CLIP clip. The fitting case skips this
   so glyphs with side/vertical overshoot keep painting into the extended draw area
   exactly as before.
4. **Circular wrap copy** — for `LONG_SCROLL_CIRCULAR`, a second copy of the mask is
   drawn one *period* ahead so the marquee loops seamlessly (first copy scrolls off the
   start edge as the second enters from the far edge).

Once the mask honors the offset, shaped LTR titles ride LVGL's existing
`LONG_SCROLL_CIRCULAR` machinery for free. Verified in the screenshot generator's
animated-GIF capture: long hi/th titles scroll left, stay clipped between the back
button and the right edge, and wrap around cleanly.

## Three non-obvious traps

### 1. RTL seeds `offset.x` at a large NEGATIVE start — so RTL is deferred/excluded
For an RTL `LONG_SCROLL_CIRCULAR` label (`LV_USE_BIDI=1`, ur), LVGL's animation start
value is `-(size.x + gap)`, **not 0** (see `lv_label.c`, the BIDI circular branch).
At frame 0 `offset.x` is therefore a large negative number. Naively honoring it on top
of an RTL right-justify pushes the run fully off-screen to the left — the title
*disappears*. Confirmed empirically: gating everything on `!g_table.rtl` was the
difference between the ur status title vanishing and ur rendering byte-identically.

So the whole scroll/marquee path (offset, scroll-mode start-justify, wrap copy, clip)
is **LTR-only**. RTL stays on the legacy center/right path, untouched — its
codepoint-overflow right-justify for `LONG_CLIP` still works as before, and its
`SCROLL_CIRCULAR` title center-clips (no scroll). RTL marquee/start-justify framing is
deferred to Item 2c of the hardening plan.

### 2. The wrap period uses the CODEPOINT width, not the run width
The circular wrap copy is placed at `+period` where
`period = label->text_size.x + WAIT_CHAR_COUNT * space_width`. `text_size.x` is the
**codepoint** text size — the metric that drives LVGL's offset animation — so the copy
stays in lock-step with the animation reset and the loop is seamless. The *shaped* run
width (`run->layout_w`) can differ (shaping often forms narrower conjuncts/ligatures
than the sum of codepoint advances), so the visible inter-copy gap is `period -
run->layout_w` rather than a fixed N-space gap. This is acceptable (typically a slightly
larger gap); perfecting shaped marquee geometry is tracked in plan Items 2b/2c. Do NOT
"fix" this by using `run->layout_w` for the period — that desyncs the copy from the
animation and produces a jump at the wrap.

### 3. `label->offset` / `text_size` have no public getter
They live in `struct _lv_label_t` (`src/widgets/label/lv_label_private.h`). There is no
`lv_label_get_offset()`. glyph_runs.cpp already reaches into LVGL internals
(`src/misc/lv_text_ap.h`), so it includes the private label header and casts the obj to
`lv_label_t*`. `lv_area_intersect` is likewise private
(`src/misc/lv_area_private.h`).

## Verification recipe

The screenshot generator emits an animated GIF for any scenario with
`"animated": true` by advancing LVGL ticks (`maybe_write_scroll_gif`). Existing
localized `long_title` scenarios use a German (Latin/subset) title, so they don't
exercise the shaped path. To verify shaped scroll, render a scenario whose
`top_nav.title` is a long hi/th string with `animated: true`, then
`magick <gif> -coalesce` and inspect a vertical strip of title-region crops across
frames — the run should march left and (for a moderate overflow + long
`animation_seconds`) wrap around with a clean gap.

Byte-identical proof for the static gallery: render before/after for en/de/fa/hi/th/ur
and `diff -rq` the `img/` trees. Expect en/de/fa/hi/ur identical and th changed only by
a ~2px content-box clip of one title glyph's right overshoot (visually identical).

## Still deferred after Task 0 (consumers)
Task 0 is the *capability*. The consumers are separate:
- Long status **headlines** (currently `LONG_DOT`) and **top-nav titles** auto-scroll —
  plan Items 2b / 2c.
- Touch long-press-to-scroll — Item 3.
- A shaped **button** marquee — `button_set_label_marquee` still early-returns for
  glyph-run locales (see `hardware-focus-label-marquee.md` §5). Flipping that on is a
  future item; Task 0 only removes the draw-layer blocker for LTR.
