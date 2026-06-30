#ifndef KEYBOARD_CORE_H
#define KEYBOARD_CORE_H

// keyboard_core — shared LVGL keyboard/text-entry mechanics.
//
// These helpers were extracted from the original one-off passphrase keyboard so
// that all keyboard-style screens (seed_add_passphrase, keyboard_screen,
// seed_mnemonic_entry) share one implementation of the fiddly bits: the
// cursor-styled text-entry box, the SeedSigner key theming + per-key icon
// recolor, the 240px hardware side-panel buttons, and the joystick directional
// navigation (top-row → back-button handoff + LEFT/RIGHT row-wrap).
//
// Everything here is platform-agnostic C++ (no passphrase semantics). Screen-
// specific behavior — charset/page switching, aux-key (KEY1/KEY2/KEY3) meaning,
// completion routing, live word matching — stays in each screen.
//
// The map-inspection helpers take a button-matrix map array directly (rather
// than the widget) so they work for both `lv_keyboard` (multi-mode: pass
// `lv_keyboard_get_map_array(kb)`) and a plain single-map `lv_buttonmatrix`
// (pass the map you built). All other helpers operate on the widget via the
// `lv_buttonmatrix_*` API, which applies to `lv_keyboard` too (it subclasses
// buttonmatrix).

#include "lvgl.h"

// --- Button-matrix map inspection ------------------------------------------

// Number of buttons in the map's top row — used for the UP → back-button
// handoff. Counts entries until the first row break ("\n") or terminator ("").
size_t kb_top_row_count(const char * const *map);

// [first, last] button indices of the row that contains button `sel`, so
// LEFT/RIGHT can wrap within the current row instead of crossing onto the
// adjacent row. Assumes the map has no hidden buttons.
void kb_row_bounds(const char * const *map, uint32_t sel,
                   uint32_t *first, uint32_t *last);

// Button index of a single-character key in the map (-1 if absent). Used to
// pre-select the last-typed key for the joystick highlight.
int kb_find_button(const char * const *map, char ch);

// --- Text-entry box ---------------------------------------------------------

// Build the standard one-line, cursor-styled SeedSigner text-entry box (dark
// fill, accent border, thin white insert-cursor I-bar, vertically centered) and
// align it to the parent's top-left. The caller sets the initial text /
// max-length / cursor position. `static_render` true disables the cursor blink
// so screenshots reliably capture the cursor.
lv_obj_t *kb_make_text_entry(lv_obj_t *parent, int32_t width, bool static_render);

// --- Keyboard / button-matrix theming --------------------------------------

// Apply SeedSigner styling to a keyboard or button-matrix: black panel, dark
// keys with `item_font` light text, control keys (CHECKED) marked accent-orange,
// selected/pressed key highlighted orange with black text. Also installs the
// per-key icon-recolor + vertical-centering draw callback (CHECK → green,
// SPACE → gray, other SeedSigner control glyphs → orange).
void kb_style_matrix(lv_obj_t *matrix, const lv_font_t *item_font);

// --- 240px hardware side panel (KEY1/KEY2/KEY3 indicators) ------------------

// Build one display-only side-panel button (not joystick-navigable). The label
// is centered within the VISIBLE portion (full width minus `clipped_right`, the
// px that run off the right screen edge). Pass 0 for a fully-visible button.
lv_obj_t *kb_side_button(lv_obj_t *parent, int32_t x, int32_t y,
                         int32_t w, int32_t h, const char *text,
                         const lv_font_t *font, int color,
                         int32_t clipped_right, lv_obj_t **out_label);

// Momentary "pressed" flash on a side button (they aren't clickable, so the
// PRESSED state is driven by hand from the physical-key handler).
void kb_flash_side_button(lv_obj_t *btn);

// --- Joystick directional navigation ---------------------------------------

// Generic part of a keyboard's hardware key handler, to be called from a
// screen's own LV_EVENT_KEY|PREPROCESS callback AFTER it has handled its own
// aux keys (KEY1/KEY2/KEY3). Implements: UP on the top row → focus `back_btn`;
// LEFT/RIGHT wrap within the current row (consuming the event so the default
// cross-row move does not run). `map` is the matrix's current map.
void kb_handle_directional(lv_event_t *e, const char * const *map,
                           lv_obj_t *matrix, lv_obj_t *back_btn);

// Back-button key handler body: DOWN returns focus to `matrix`. Call from the
// back button's LV_EVENT_KEY callback.
void kb_back_down_to_matrix(lv_event_t *e, lv_obj_t *matrix);

// Connect every keypad/encoder input device to `group` (joystick navigation).
void kb_connect_indevs(lv_group_t *group);

#endif // KEYBOARD_CORE_H
