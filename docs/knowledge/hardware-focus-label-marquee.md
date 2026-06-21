# Hardware-focus button-label marquee — why it's driven from the nav layer, not LVGL events

When a button-list / main-menu button is focused in **hardware (joystick/KEY)** input
mode, a too-wide text label marquee-scrolls so the user can read the clipped tail;
when it loses focus the label clips back to its START edge so the beginning shows
again. Touch mode has no persistent focus, so its labels never scroll.

This sounds like a textbook use of LVGL's `LV_EVENT_FOCUSED` / `LV_EVENT_DEFOCUSED`
(and the first implementation did register `label_scroll_on_focus` /
`label_clip_on_defocus` callbacks on those events). **It never fired, and it can't
be made to fire safely.** Here is why the working version drives the long-mode swap
directly from the navigation layer instead.

## 1. Body buttons are deliberately kept OUT of the LVGL focus group

`nav_bind()` (navigation.cpp) routes the keypad indev to a single 1×1 transparent
"sink" object that is the *sole* member of the LVGL group. Body buttons and top-nav
buttons are never added to any group. All key events land on the sink's
`nav_key_handler`, which mutates our own `nav_ctx_t.zone` / `body_index` and renders
the highlight via `button_set_active()` in `update_visual_focus()`.

Consequence: LVGL's focus machinery never runs on the buttons, so it **never emits
`LV_EVENT_FOCUSED` / `LV_EVENT_DEFOCUSED`** for them. Any callback registered on
those events on a body button is dead code.

The sink design is intentional (see the comment in `nav_bind`): keeping items out of
the group prevents LVGL's auto-focus from generating spurious FOCUSED/DEFOCUSED that
would fight our explicit nav state.

## 2. You cannot just synthesize the event with `lv_obj_send_event`

Emitting `lv_obj_send_event(btn, LV_EVENT_FOCUSED, NULL)` to trigger the callback by
hand does **not** isolate the label change. The base object class event handler reacts
to FOCUSED itself (`lv_obj.c`, ~line 982):

- it calls `lv_obj_add_state(obj, LV_STATE_FOCUSED)` (plus `LV_STATE_FOCUS_KEY` for a
  keypad/encoder indev), and
- if `LV_OBJ_FLAG_SCROLL_ON_FOCUS` is set, it calls `lv_obj_scroll_to_view_recursive`.

`DEFOCUSED` likewise removes those state bits. So synthesizing the event re-introduces
exactly the state-fighting the sink design was built to avoid — the theme's
focused-state styling could override our manual `button_set_active` highlight, and the
recursive scroll could move the viewport out from under our own `scroll_to_view`.

**Therefore:** the long-mode swap is driven directly from `update_visual_focus()` via
`button_set_label_marquee(btn, focused)` — no LVGL events, no state mutation. It sits
right next to the `button_set_active()` call that already renders our highlight.

## 3. Setting `LONG_SCROLL_CIRCULAR` on a label that FITS is a no-op (no regression)

The common case is a label that fits its button. Switching a focused fitting label to
`LV_LABEL_LONG_SCROLL_CIRCULAR` does **not** shift it: in the label draw, LVGL forces
LEFT/RIGHT alignment in SCROLL modes **only when `text_size.x > width`**
(`lv_label.c`, ~line 906) — a fitting label keeps its CENTER alignment. The scroll
animation is likewise set up only on overflow (`lv_label.c`, ~line 1128). So a focused
fitting label stays centered and static, byte-identical to before. (Verified: 246
before/after screenshots across en/de/hi × 4 profiles were byte-identical.)

## 4. `lv_label_set_long_mode` is NOT idempotent — guard the swap

Every `lv_label_set_long_mode` call unconditionally deletes the scroll animation,
resets the offset to 0, and marks a text refresh — even when the mode is unchanged.
`update_visual_focus()` re-asserts *every* non-focused button on each keypress, so an
unguarded swap would re-clip and redraw the whole list every step, and re-asserting
SCROLL on the still-focused button would restart its marquee from the beginning.
`button_set_label_marquee` reads `lv_label_get_long_mode()` and only writes on an
actual change.

## 5. Shaped (glyph-run) locales are excluded — they stay LONG_CLIP

For hi / th / ur the button label is painted by `glyph_run_draw_cb` from a pre-baked
A8 alpha mask. That draw path:

- positions the mask from the label's content coords and the run's advance — it
  **ignores the label's scroll offset**, so a marquee animation would not move the
  glyphs; and
- start-justifies an overflowing run **only while the label is `LONG_CLIP`**
  (`glyph_runs.cpp`, the `align == CENTER && layout_w > content_w && long_mode ==
  LONG_CLIP` check).

So flipping a glyph-run label to `LONG_SCROLL_CIRCULAR` would gain nothing (no scroll)
and would *break* the start-justify (the run would re-center and clip to its middle).
`button_set_label_marquee` early-returns when `seedsigner_locale_uses_glyph_runs()`,
leaving shaped labels start-justified. Active-scroll for shaped scripts would require
animating the mask draw itself — out of scope for v1.

## Where this lives

- `button_set_label_marquee()` — components.cpp (uses the same `find_last_label_child`
  as `button_set_active`; in `large_icon_button` the icon label is at child index 0
  and the text label last, so the helper correctly targets the text label).
- Called from `update_visual_focus()` — navigation.cpp, in the body-item loop.
