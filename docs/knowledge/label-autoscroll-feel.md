# Tuning an LVGL label's auto-scroll feel (initial hold + true px/sec + per-loop hold)

**Scope:** `label_set_line_autoscroll()` in components.cpp — the helper that makes an
overflowing single-line label (top-nav title; long status headline) marquee-scroll with
a deliberate feel: hold start-justified for a beat, scroll at a true constant px/sec,
hold again each time it wraps back to the start. This is Items 2c/2d of
`.claude/plans/pre-upstream-pr-hardening.md`. Several of the mechanics are not exposed
through LVGL's public label API and were non-obvious; capture here so the next change
(headlines, a shaped button marquee, speed tuning) doesn't have to re-derive them.

## What the helper does

```
lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);   // continuous wrap
lv_obj_set_style_anim_duration(label, explicit_ms, LV_PART_MAIN); // true px/sec (see below)
// + a static template anim set via lv_obj_set_style_anim() carrying the holds
```

The label must already be width-constrained + start-aligned by the caller (top_nav's
overflow branch does this); the helper only adds the scroll behavior.

## The four traps

### 1. There is no API for a scroll *start delay* — use a template anim with negative act_time
LVGL gives `LONG_SCROLL`/`LONG_SCROLL_CIRCULAR` an end/repeat delay
(`LV_LABEL_SCROLL_DELAY`) but **no initial hold** before the first scroll. To add one,
set the label's style "anim" to a template (`lv_obj_set_style_anim(label, &tmpl, …)`)
whose `act_time` is negative — `lv_anim_set_delay(&tmpl, ms)` sets `act_time = -ms`, and
a negative act_time is LVGL's representation of "wait this long before animating."
During the wait the value stays at `start` (offset 0 = start-justified).

The label's `lv_label_refr_text` copies fields out of this template via
`overwrite_anim_property` (lv_label.c). It only copies `act_time` **when the
destination anim's act_time <= 0** — true on first creation (the new anim's act_time is
0), false while the scroll is already running. So the initial hold fires once on show
and is NOT restarted by later layout refreshes. 

### 2. `overwrite_anim_property` copies a fixed field set — and `repeat_cnt` is in it
The template is not a full anim; the label copies only specific fields, and the set
**differs by long_mode**:
- `LONG_SCROLL`: `act_time` (if dest<=0), `repeat_cnt`, `repeat_delay`, `completed_cb`, `reverse_delay`
- `LONG_SCROLL_CIRCULAR`: `act_time` (if dest<=0), `repeat_cnt`, `repeat_delay`, `completed_cb` (**no reverse_delay** — circular has no reverse phase)

`repeat_cnt` is copied, and `lv_anim_init()` defaults it to **1**. So the template MUST
set `repeat_cnt = LV_ANIM_REPEAT_INFINITE`, or the copy clobbers the label's infinite
loop and the title scrolls exactly once then freezes. (This is easy to miss because the
template "looks" like it only carries the delays.)

### 3. The px/sec *speed encoding* caps duration at ~10 s — use an explicit duration for a true rate
`lv_anim_speed_clamped(speed, min_time, max_time)` encodes a px/sec speed that LVGL
later resolves to a duration via `lv_anim_resolve_speed` (`time = distance*100/speed10`,
clamped to [min,max]). But the encoding **truncates max_time to ~10230 ms**, so any line
long enough to need >10 s at the target rate scrolls *faster* than the target (the
60-char German stress title came out ~60 px/sec instead of 40). The default
`LV_LABEL_DEF_SCROLL_SPEED = lv_anim_speed_clamped(40, 300, 10000)` has this built in.

For a TRUE constant rate at any length, skip the encoding and set an explicit duration
(plain ms) via `lv_obj_set_style_anim_duration`:
`duration_ms = (scroll_distance) * 1000 / px_per_sec`, where for `LONG_SCROLL_CIRCULAR`
the scroll distance is `line_width + LV_LABEL_WAIT_CHAR_COUNT * space_glyph_width` — the
same `-(size.x + gap)` the offset animation travels before it wraps. The helper measures
`line_width` itself with `lv_text_get_size(lv_label_get_text(label), font, letter_space,
…)` so it doesn't depend on LVGL having laid the label out yet. Floor it
(`LINE_SCROLL_MIN_MS`) so a tiny overflow can't produce a sub-300 ms jitter; no max
(honoring the true rate is the point).

### 4. For CIRCULAR, "pause each time it returns to the start" == `repeat_delay`
The circular loop animates offset `0 → -(size.x+gap)` then resets to 0 and repeats. The
reset is exactly the moment the line is back at the start (first copy left-justified;
the wrap copy that filled the right edge slides off). So `repeat_delay` — the wait
before each repeat — lands the hold precisely at the start position. Set it equal to the
initial hold for a consistent feel. (No `reverse_delay`: circular never reverses.)

## Speed scaling and tunables
Constants in gui_constants.h: `LINE_SCROLL_PX_PER_SEC` (40, matches the Python PIL
screens' `horizontal_scroll_speed`), `LINE_SCROLL_BEGIN_HOLD_MS` (1000), and
`LINE_SCROLL_MIN_MS` (300 floor). Speed is scaled by `active_profile().px_multiplier/100`
so the visual rate is constant across display sizes (a 2× display scrolls a 2× line in
the same wall-clock time). The holds are wall-clock and do not scale.

## Why the static screenshots stayed byte-identical
The screenshot generator captures each scenario near tick 0. Because the initial hold
keeps offset at 0 for the first second, the captured frame is reliably the
start-justified line — identical to the pre-change tip (which was also at offset 0 at
t≈0). So adding the scroll changed only the animated GIFs, not the static PNG gallery.

## Model choice (for the record)
The Python screens ping-pong (scroll out, hold, scroll back). We use a continuous
**circular** wrap instead (the user preferred it), keeping only Python's *initial + per-
loop start hold* and its 40 px/sec rate. If a future requirement wants the ping-pong,
`LONG_SCROLL` + `reverse_delay` is the LVGL equivalent (and Task 0's shaped wrap-copy
would then go unused for that label).

## Reach
Shaped (hi/th) titles ride this same offset animation because the glyph-run draw honors
`label->offset.x` (Task 0, `glyph-run-scroll-offset.md`); RTL (ur) is excluded at that
draw layer for now. The helper is locale-agnostic — it just configures the LVGL label;
the glyph-run draw layer does the rest for shaped scripts.

## Consumers
- **Top-nav title** (`top_nav()`, Items 2c/2d) — overflow branch start-justifies + calls
  the helper.
- **Status headline** (`large_icon_status_screen`, Item 2b) — overflow branch does the
  same. Two headline-specific notes: (1) the fit test measures the label's STORED text
  (presentation forms) and is taken against the FULL screen width, so only genuinely
  too-wide headlines scroll; a fitting headline stays centered + `LONG_DOT` (byte-
  identical). (2) the status screen zeroes the body's horizontal padding so the hero icon
  spans full width, so an overflowing headline must be re-clamped to the body's text
  column (`text_edge_padding_multiplier * EDGE_PADDING` per side) — otherwise it scrolls
  edge-to-edge and under the pulsing warning border. The `upper_body` flex centers the
  narrower label, giving equal gutters; LVGL's clip (and the glyph-run content-box clip)
  then keep the marquee inside them. RTL headlines are gated out (legacy `LONG_DOT`),
  matching the title and the draw layer.
