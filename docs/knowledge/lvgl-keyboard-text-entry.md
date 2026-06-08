# LVGL native keyboard / text-entry integration

Notes from building `seed_add_passphrase_screen` (`components/seedsigner/seedsigner.cpp`)
on LVGL v9.5.0's native `lv_keyboard` + `lv_textarea`. These are the non-obvious
LVGL behaviors a future text-entry screen (seed word entry, derivation path, etc.)
will hit. None of them are documented clearly in the LVGL docs.

## A keyboard renders ALL keys with one font — so merge the font

`lv_keyboard` is a `lv_buttonmatrix`; the whole matrix uses a single font
(`LV_PART_ITEMS`). There is no per-button font. To get fixed-width letters AND the
`LV_SYMBOL_*` control glyphs (backspace, cursor, OK) in one keyboard, **merge glyph
ranges into one font** with `lv_font_conv`:

```
lv_font_conv --bpp 4 --size <N> --no-compress \
  --font Inconsolata-SemiBold.ttf --range 0x20-0x7E \
  --font <lvgl>/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff \
  --range 0xF00C,0xF053,0xF054,0xF55A \
  --format lvgl -o inconsolata_semibold_18_4bpp.c
```

`LV_SYMBOL_*` are FontAwesome 5 codepoints: OK `0xF00C`, LEFT(chevron) `0xF053`,
RIGHT `0xF054`, BACKSPACE `0xF55A`. The Inconsolata TTFs live in the Python repo
(`seedsigner/src/seedsigner/resources/fonts/`); FontAwesome5 ships inside the LVGL
submodule. Per-key recolor (we want OK green, not the orange of other control keys)
is done at draw time via `LV_EVENT_DRAW_TASK_ADDED` — the buttonmatrix tags each
label draw task with the button index in `base->id1`.

**lv_font_conv 1.5.3 gotcha:** it emits a bare `#ifdef LV_LVGL_H_INCLUDE_SIMPLE` /
`#else #include "lvgl/lvgl.h"` block but omits the `#ifdef __has_include` auto-detect
block the existing repo fonts have — so the generated `.c` falls back to
`lvgl/lvgl.h` and won't compile. Re-add the `__has_include` block (copy from any
existing `opensans_*_4bpp.c`). Font `.c` files are listed explicitly (NOT globbed)
in `tools/screen_runner/CMakeLists.txt` and `tools/screenshot_generator/CMakeLists.txt`.

## lv_keyboard_create sets a BOTTOM_MID anchor in its constructor

So a bare `lv_obj_set_pos(kb, x, y)` is treated as an **offset from the bottom**
(pushes the keyboard off-screen / clips it). Use
`lv_obj_align(kb, LV_ALIGN_TOP_LEFT, x, y)` to reset the anchor.

## def_event_cb only switches lower / upper / SPECIAL

`lv_keyboard_def_event_cb` (installed by the constructor) recognizes only three
mode-switch control buttons — `"abc"`, `"ABC"`, `"1#"` — plus OK/close. There is **no
native switch button for NUMBER or the USER_1-4 modes**, and the labels are fixed.
To use readable mode labels (`123` / `!?&`) and reach NUMBER mode, remove the default
handler and install your own:

```c
lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
lv_obj_add_event_cb(kb, my_value_changed_cb, LV_EVENT_VALUE_CHANGED, ctx);
```

In the custom handler, switch modes yourself with `lv_keyboard_set_mode()` (which can
reach any mode), and insert chars with `lv_textarea_add_text/delete_char/cursor_*`.
Custom per-mode maps go in via `lv_keyboard_set_map(kb, mode, map, ctrl_map)`. In C++
the `ctrl_map` entries must be cast — `lv_buttonmatrix_ctrl_t` is an enum and C++ (unlike
LVGL's own C maps) won't implicitly narrow `int`. `LV_BUTTONMATRIX_CTRL_HIDDEN |
DISABLED` makes a spacer key that reserves a row's height but isn't drawn or navigable
(used to stop a short page's keys from stretching tall).

## Navigation: lv_keyboard fights the shared keypad-sink nav system

`navigation.cpp` / `nav_bind` deliberately avoids LVGL group focus: a single
transparent 1x1 "sink" is the sole group member, and key events are routed manually so
LVGL's auto-focus never fights the custom highlight state. `lv_keyboard`'s built-in
directional navigation requires the keyboard to **be** the focused group object
receiving `LV_KEY_*` — fundamentally incompatible. Forwarding keys from the sink also
fails on insertion: the buttonmatrix fires its insert on PRESSED/RELEASED keyed off
the arrow-set `btn_id_sel`, not on a synthesized `CLICKED`.

Resolution: this screen **bypasses `nav_bind`** and wires localized LVGL-native group
focus — create a group with the keyboard + back button, focus the keyboard, connect the
keypad/encoder indevs (same loop `nav_bind` uses). The top-nav handoff is two one-line
`LV_EVENT_KEY` filters: UP off the keyboard's top row → `lv_group_focus_obj(back_btn)`,
DOWN on the back button → focus the keyboard. The two nav models coexist via the one
seam they share (the indevs); existing screens are untouched.

**Event ordering gotcha (UP→back):** added `LV_EVENT_KEY` callbacks run AFTER the
widget's class handler, so the buttonmatrix moves the selection first. A plain UP
handler that checks "is the selection in the top row?" then fires on an UP from the
*second* row too — the buttonmatrix already moved it into the top row before the
handler reads it, so it wrongly jumps to the back button. Register the handler with
`LV_EVENT_KEY | LV_EVENT_PREPROCESS` (the `0x8000` flag) so it runs BEFORE the class
handler and reads the pre-move selection; then a top-row check correctly means "already
at the top, exit upward."

## Forcing a selected-key highlight (e.g. for screenshots)

`buttonmatrix` draw applies PRESSED/FOCUSED/FOCUS_KEY to `btn_id_sel` based on the
keyboard object's **overall** state. To highlight a key without real input:
`lv_buttonmatrix_set_selected_button(kb, id)` + `lv_obj_add_state(kb, LV_STATE_PRESSED)`.
Use PRESSED, not FOCUS_KEY — FOCUS_KEY also draws the theme's focus outline on the
panel (LV_PART_MAIN). Zero that outline regardless (`outline_width 0` for
`LV_STATE_FOCUSED`/`LV_STATE_FOCUS_KEY` on MAIN), because the joystick focusing the
keyboard in normal use would otherwise show a stray blue panel outline.

## A one-line textarea bounces if you force a fixed height

`lv_textarea_set_one_line(true)` sets the box height to `LV_SIZE_CONTENT`. If you
then force a fixed height (`lv_obj_set_height(ta, BUTTON_HEIGHT)`) and add padding +
a border, the **content area** (`height - padding - border`) can end up slightly
shorter than one line height. `lv_textarea_scroll_to_cusor_pos` (called on cursor
moves / style changes) then thinks the cursor is clipped and animates a vertical
scroll **every frame** — the box visibly bounces up and down. Fix: don't force a
fixed height; let one-line size the box to content and use symmetric top/bottom
padding (sized from `lv_font_get_line_height` minus the border) to land near the
target height. The content then always fits → no scroll → no bounce.

## Selected-key highlight color: focus states draw at partial opacity

The default theme draws buttonmatrix items in FOCUSED / FOCUS_KEY at a **partial
bg_opa**, so setting only `bg_color` on those states yields a muted/dark-orange key
that reads as "inactive". Set `bg_opa = LV_OPA_COVER` explicitly for the selected
states. Joystick navigation marks the selected key FOCUSED/FOCUS_KEY (managed by the
indev, persists across presses); a touch press / the static screenshot highlight use
PRESSED. Style all of them (plus the CHECKED combos, since control keys are CHECKED).
Also **pre-select an initial button** in joystick mode (`lv_buttonmatrix_set_selected_button`)
— otherwise `btn_id_sel` is NONE and it takes an arrow press just to "enter" the
keyboard with no visible cursor until then. In static-render (no indev) the focus
state isn't applied, so add `LV_STATE_PRESSED` to make the highlight show in the still.

## Textarea cursor: shape, visibility, color, and blink

- **Block vs I-bar:** the default theme already draws the cursor as a thin left-border
  I-bar (`lv_theme_default`'s `ta_cursor`: `border_side LEFT`, `border_width ~2dpx`,
  `border_color = color_text`, `pad_left = -1dpx`). It is NOT a block — but it's styled
  on the **`LV_PART_CURSOR | LV_STATE_FOCUSED`** selector, so to recolor it you must
  override on that focused selector too (a base-`LV_PART_CURSOR` override loses to it
  when the box is focused → cursor renders in the dark `color_text`). Override on BOTH
  `LV_PART_CURSOR` and `LV_PART_CURSOR | LV_STATE_FOCUSED`: set `border_color` (light) +
  `border_opa = COVER` (else partial opacity = muted/dark-grey), a thin `border_width`,
  and `pad_left = 0` (the theme's `-1dpx` nudges the bar onto the previous glyph).
  Setting `bg_opa = TRANSP` keeps it a bar, not a block.

- `draw_cursor` gates **only** on `ta->cursor.show` — no focus required. So an unfocused
  textarea still shows its cursor if `cursor.show == 1`. It draws `LV_PART_CURSOR` as a
  rect (bg + border) at the cursor cell, so the cursor renders whatever that part's
  style specifies — a `bg_opa` fill (block) or a single-side border (I-bar). With no
  visible bg/border style it draws nothing.
- Blink is controlled by `anim_duration` on `LV_PART_CURSOR`. Setting it to **0** makes
  `start_cursor_blink` delete the blink animation and force `cursor.show = 1` (always
  on). The screen keeps the blink for live use and only disables it in static-render
  mode (`seedsigner_lvgl_set_static_render(true)`, set by the screenshot generator) so
  stills deterministically capture the cursor. `set_cursor_pos` / a style change
  re-triggers `start_cursor_blink`, so set `anim_duration 0` before the final cursor-pos
  call.
- `lv_textarea` natively places the cursor at the tapped character
  (`cursor_click_pos`, on by default) — touch tap-to-position is free.

## Build / tooling notes

- A new screen must be registered in BOTH desktop tools' `k_screen_registry`
  (`tools/screen_runner/screen_runner.cpp`, `tools/screenshot_generator/screenshot_gen.cpp`)
  and have scenarios in `tools/scenarios.json`.
- Screen results return via weak callbacks (`seedsigner_lvgl_on_text_entered`,
  `seedsigner_lvgl_on_button_selected`, `seedsigner_lvgl_on_aux_key`) — desktop tools
  provide strong overrides.
- The screen runner maps SDL number keys `1/2/3` to `LV_KEY_ENTER`; `F1/F2/F3` were added
  to emit the aux codes `'1'/'2'/'3'` so KEY1/KEY2/KEY3 side-panel actions are testable.
- The screenshot generator renders each resolution in its native input mode
  (240px → joystick, larger → touch) so the base scenario reflects the real device.
