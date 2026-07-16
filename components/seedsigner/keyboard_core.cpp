// keyboard_core — shared LVGL keyboard/text-entry mechanics. See keyboard_core.h
// for the rationale and the reusable-vs-screen-specific split. The bodies here
// were lifted verbatim from the original passphrase keyboard (only generalizing
// the map source, the key font, and the static-render flag into parameters) so
// existing screens render pixel-identically after the extraction.

#include "keyboard_core.h"
#include "gui_constants.h"
#include "navigation.h"       // attach_keypad_indevs_to_group (held-key-safe input handoff)

#include "lvgl.h"

#include <cstring>
#include <cstdint>

// ===========================================================================
// Button-matrix map inspection
// ===========================================================================

size_t kb_top_row_count(const char * const *map) {
    size_t n = 0;
    while (map && map[n] && map[n][0] != '\0' && std::strcmp(map[n], "\n") != 0) {
        n++;
    }
    return n;
}

void kb_row_bounds(const char * const *map, uint32_t sel,
                   uint32_t *first, uint32_t *last) {
    uint32_t id = 0, row_first = 0;
    for (size_t i = 0; map && map[i] && map[i][0] != '\0'; ++i) {
        if (std::strcmp(map[i], "\n") == 0) {       // end of a row [row_first, id-1]
            if (sel < id) { *first = row_first; *last = id - 1; return; }
            row_first = id;
            continue;
        }
        id++;
    }
    // Last row (terminated by "" rather than "\n").
    *first = row_first;
    *last  = (id > 0) ? id - 1 : 0;
}

int kb_find_button(const char * const *map, char ch) {
    int id = 0;
    for (size_t i = 0; map && map[i] && map[i][0] != '\0'; ++i) {
        if (std::strcmp(map[i], "\n") == 0) continue;  // row break: not a button
        if (std::strlen(map[i]) == 1 && map[i][0] == ch) return id;
        id++;
    }
    return -1;
}

// ===========================================================================
// Text-entry box
// ===========================================================================

lv_obj_t *kb_make_text_entry(lv_obj_t *parent, int32_t width, bool static_render) {
    // One-line, cleartext. Font matches the keyboard for a consistent look;
    // SeedSigner dark fill with an accent-orange border/cursor. cursor_click_pos
    // (on by default) lets touch tap to position; the in-grid cursor keys are the
    // precise fallback.
    const int32_t ta_border = 2;
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, "");
    lv_obj_set_width(ta, width);
    // NOTE: do NOT force a fixed height. one_line mode sizes the box to its
    // content (LV_SIZE_CONTENT). Forcing BUTTON_HEIGHT made the content area
    // (height - padding - border) slightly shorter than one line, so
    // scroll_to_cursor perpetually animated a vertical scroll — the box bounced.
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(BUTTON_BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_font(ta, &KEYBOARD_FONT, LV_PART_MAIN);
    lv_obj_set_style_border_color(ta, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_border_width(ta, ta_border, LV_PART_MAIN);
    lv_obj_set_style_radius(ta, BUTTON_RADIUS / 2, LV_PART_MAIN);

    // Cursor: a thin light I-bar drawn as a left border so it sits between
    // characters as an insert/position cursor (the default block fill reads as
    // "overwrite"). The default theme styles the cursor with a DARK border on the
    // CURSOR|FOCUSED selector, which wins over a base-part override when focused —
    // so override on BOTH the base and the focused selector with opaque white.
    const int32_t cur_w = 2 * active_profile().px_multiplier / 100;
    // Box vertical-centering padding (the cursor inset is derived from it). Under
    // LV_SIZE_CONTENT the box height is line_height + 2*pad + 2*border, so size
    // the padding (minus the border) to land near BUTTON_HEIGHT.
    int32_t ta_pad_v = (BUTTON_HEIGHT - (int32_t)lv_font_get_line_height(&KEYBOARD_FONT)) / 2 - ta_border;
    if (ta_pad_v < 0) ta_pad_v = 0;
    // Keep a constant small gap (>=1px, scaled) between the cursor bar and the
    // top/bottom of the box at every size. 2px at the base profile, widening as
    // the display scales up: 100x -> 2, 150x -> 4, 200x -> 6.
    int32_t cur_gap = 2 * (1 + (active_profile().px_multiplier - 100) / 50);
    if (cur_gap < 2) cur_gap = 2;
    int32_t cur_pad_v = ta_pad_v - cur_w - cur_gap;
    const lv_style_selector_t cur_sel[] = {
        LV_PART_CURSOR, (lv_style_selector_t)(LV_PART_CURSOR | LV_STATE_FOCUSED),
    };
    for (lv_style_selector_t cs : cur_sel) {
        lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, cs);
        lv_obj_set_style_border_color(ta, lv_color_hex(0xffffff), cs);
        lv_obj_set_style_border_opa(ta, LV_OPA_COVER, cs);
        lv_obj_set_style_border_width(ta, cur_w, cs);
        lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, cs);
        // The theme nudges the cursor left (pad_left = -1px) so it sits ON the
        // previous glyph; zero it so the bar sits in the gap after the character.
        lv_obj_set_style_pad_left(ta, 0, cs);
        lv_obj_set_style_pad_top(ta, cur_pad_v, cs);
        lv_obj_set_style_pad_bottom(ta, cur_pad_v, cs);
    }
    if (static_render) {
        lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR);
    } else {
        // The textarea is never the group's focused object (the keyboard is), so
        // the cursor blink that normally kicks off on LV_EVENT_FOCUSED never
        // fires. Trigger it manually so the cursor blinks while waiting for the
        // first input. The FOCUSED handler only starts the blink, no side effects.
        lv_obj_send_event(ta, LV_EVENT_FOCUSED, NULL);
    }
    // One-line entry: never show a (vertical) scrollbar. Horizontal overflow is
    // handled by the textarea scrolling its content to follow the cursor.
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
    // Vertically center the text via the symmetric top/bottom padding above.
    lv_obj_set_style_pad_top(ta, ta_pad_v, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(ta, ta_pad_v, LV_PART_MAIN);
    return ta;
}

// ===========================================================================
// Keyboard / button-matrix theming
// ===========================================================================

// Per-key restyling of SeedSigner control-icon keys (CHECK, the two CHEVRON
// cursors, DELETE, SPACE). Those glyphs are merged into the key text font for
// layout, but need two per-key fixes the buttonmatrix can't do via styles:
//   - Color: CHECK is green; SPACE is muted gray (it enters a real character);
//     the cursor + backspace controls are SeedSigner-orange so they read as
//     actions, not enterable glyphs.
//   - Vertical centering: lv_buttonmatrix line-centers key text by the font line
//     height, but these icons are bottom-anchored and as tall as the ascent, so
//     they would sit at the top of the key. Nudge each down by the gap between
//     its ink center and the line-box center — computed from the font so it holds
//     at any size, with no edits to the generated font data.
//
// Non-SeedSigner icon fonts (e.g. FontAwesome dice on keyboard_screen) have a
// different lead byte and fall through untouched.
static void kb_icon_draw_cb(lv_event_t *e) {
    lv_draw_task_t *task = lv_event_get_draw_task(e);
    lv_draw_label_dsc_t *label_dsc = lv_draw_task_get_label_dsc(task);
    if (!label_dsc) return;  // not a label draw task

    lv_draw_dsc_base_t *base = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(task);
    if (!base || base->part != LV_PART_ITEMS) return;

    lv_obj_t *kb = lv_event_get_target_obj(e);
    const char *txt = lv_buttonmatrix_get_button_text(kb, base->id1);
    if (!txt) return;

    // SeedSigner icon glyphs are the only U+Exxx keys (3-byte UTF-8, lead byte
    // 0xEE); letter and mode-label keys are ASCII and fall through untouched.
    if ((unsigned char)txt[0] != 0xEE) return;
    uint32_t cp = ((uint32_t)(txt[0] & 0x0F) << 12) |
                  ((uint32_t)(txt[1] & 0x3F) << 6) |
                  ((uint32_t)(txt[2] & 0x3F));

    // Leave the highlighted key's color alone: when a control key is selected
    // (joystick) or pressed (touch), the buttonmatrix has already applied the
    // active text color (BUTTON_SELECTED_FONT_COLOR, black). Only recolor the
    // resting state.
    if (!lv_color_eq(label_dsc->color, lv_color_hex(BUTTON_SELECTED_FONT_COLOR))) {
        uint32_t icon_color;
        if (cp == 0xE905)      icon_color = SUCCESS_COLOR;  // CHECK
        else if (cp == 0xE923) icon_color = 0x999999u;      // SPACE
        else                   icon_color = ACCENT_COLOR;   // CHEVRON_LEFT/RIGHT, DELETE
        label_dsc->color = lv_color_hex(icon_color);
    }

    lv_font_glyph_dsc_t g;
    const lv_font_t *font = label_dsc->font;
    if (font && lv_font_get_glyph_dsc(font, &g, cp, 0)) {
        int32_t line_center  = lv_font_get_line_height(font) / 2 - font->base_line;
        int32_t glyph_center = g.ofs_y + (int32_t)g.box_h / 2;
        label_dsc->ofs_y = glyph_center - line_center;
    }
}

void kb_style_matrix(lv_obj_t *kb, const lv_font_t *item_font) {
    lv_obj_set_style_bg_color(kb, lv_color_hex(BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(kb, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(kb, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(kb, 0, LV_PART_MAIN);
    // The keyboard is the focused group object in joystick mode; suppress the
    // theme's focus outline/border on the panel (we show focus per-key instead).
    lv_obj_set_style_outline_width(kb, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(kb, 0, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(kb, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(kb, 0, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    // Tight inter-key gaps + modest rounding so the keys read as large hit targets.
    lv_obj_set_style_pad_row(kb, COMPONENT_PADDING / 4, LV_PART_MAIN);
    lv_obj_set_style_pad_column(kb, COMPONENT_PADDING / 4, LV_PART_MAIN);

    // Keys: caller-supplied font, dark fill, light text.
    lv_obj_set_style_text_font(kb, item_font, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(BUTTON_BACKGROUND_COLOR), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(BUTTON_FONT_COLOR), LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, BUTTON_RADIUS / 2, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    // Flatten the default theme's button drop-shadow on the keys. lv_keyboard zeroes
    // this via its keyboard_button_bg style, but a plain lv_buttonmatrix keeps the
    // theme's LV_DPX(3) grey shadow, which reads as a 3D bevel at each key's bottom
    // edge. Zero it so buttonmatrix keyboards match the flat passphrase keyboard.
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);

    // Control keys are flagged CHECKED; the theme would draw them light. Force
    // the same dark fill as every other key and mark them only with accent-orange
    // text so they read as actions, not a light key.
    lv_obj_set_style_bg_color(kb, lv_color_hex(BUTTON_BACKGROUND_COLOR), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, lv_color_hex(ACCENT_COLOR), LV_PART_ITEMS | LV_STATE_CHECKED);

    // Selected key: SeedSigner orange, FULL opacity, black text. Joystick marks
    // the key FOCUSED/FOCUS_KEY; touch press + the static screenshot highlight
    // use PRESSED. bg_opa COVER is set explicitly because the default theme draws
    // the focus states at partial opacity (a muted/inactive dark orange). Control
    // keys are also CHECKED, so the CHECKED combos are styled too.
    const lv_state_t sel_states[] = {
        LV_STATE_PRESSED, LV_STATE_FOCUSED, LV_STATE_FOCUS_KEY,
        (lv_state_t)(LV_STATE_CHECKED | LV_STATE_PRESSED),
        (lv_state_t)(LV_STATE_CHECKED | LV_STATE_FOCUSED),
        (lv_state_t)(LV_STATE_CHECKED | LV_STATE_FOCUS_KEY),
    };
    for (lv_state_t st : sel_states) {
        lv_obj_set_style_bg_color(kb, lv_color_hex(ACCENT_COLOR), LV_PART_ITEMS | st);
        lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS | st);
        lv_obj_set_style_text_color(kb, lv_color_hex(BUTTON_SELECTED_FONT_COLOR), LV_PART_ITEMS | st);
    }

    // Recolor the SeedSigner control glyphs at draw time (per-key color isn't a
    // style option).
    lv_obj_add_flag(kb, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(kb, kb_icon_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
}

// ===========================================================================
// 240px hardware side panel
// ===========================================================================

// See keyboard_core.h: the shared KEY1/KEY2/KEY3 panel geometry. The integer
// expressions are the exact ones the side-panel screens derived inline (division
// and rounding order preserved), so the extraction is pixel-identical.
kb_side_panel_geometry_t kb_side_panel_geometry(int32_t screen_h, int32_t body_content_w) {
    kb_side_panel_geometry_t geometry;

    // Python right_panel_buttons_width = 56, scaled per display profile.
    geometry.panel_w = 56 * active_profile().px_multiplier / 100;

    // The panel sits COMPONENT_PADDING right of the keyboard strip and deliberately
    // overshoots the right screen edge by COMPONENT_PADDING (clipped at the body
    // boundary) so the buttons read as aligned with the physical hardware keys
    // (Python's hw buttons overshoot canvas_width by COMPONENT_PADDING).
    geometry.clipped = COMPONENT_PADDING;
    geometry.x       = body_content_w + EDGE_PADDING + COMPONENT_PADDING - geometry.panel_w;

    // KEY2 is a BUTTON_HEIGHT slot centered on the FULL canvas. That offset is
    // canvas-relative and the body sits TOP_NAV_HEIGHT below the canvas top, so
    // subtract it for body-local y. KEY1/KEY3 sit one
    // (3*COMPONENT_PADDING + BUTTON_HEIGHT) step above/below the KEY2 slot.
    geometry.key2_y  = (screen_h - BUTTON_HEIGHT) / 2 - TOP_NAV_HEIGHT;
    geometry.spacing = 3 * COMPONENT_PADDING + BUTTON_HEIGHT;

    return geometry;
}

lv_obj_t *kb_side_button(lv_obj_t *parent, int32_t x, int32_t y,
                         int32_t w, int32_t h, const char *text,
                         const lv_font_t *font, int color,
                         int32_t clipped_right, lv_obj_t **out_label) {
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(BUTTON_BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, BUTTON_RADIUS / 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_remove_flag(btn, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    // Pressed (flash) look: orange fill with a black glyph, matching the keyboard
    // selection. The buttons aren't clickable; the flash is driven by hand from
    // the physical-key handler. Glyph color is set on the button (not the label)
    // so the PRESSED selector applies and the label inherits the color.
    lv_obj_set_style_bg_color(btn, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btn, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_color(btn, lv_color_hex(BUTTON_SELECTED_FONT_COLOR), LV_PART_MAIN | LV_STATE_PRESSED);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);

    // A single SeedSigner icon glyph (the confirm check) is line-centered by the
    // label box, but its ink can sit off-center within that box (glyph metrics
    // vary). Nudge it by the gap between the glyph's ink center and the line
    // center.
    int32_t dy = 0;
    if ((unsigned char)text[0] == 0xEE && text[3] == '\0') {  // one 3-byte U+Exxx char
        uint32_t cp = ((uint32_t)(text[0] & 0x0F) << 12) |
                      ((uint32_t)(text[1] & 0x3F) << 6) |
                      ((uint32_t)(text[2] & 0x3F));
        lv_font_glyph_dsc_t g;
        if (lv_font_get_glyph_dsc(font, &g, cp, 0)) {
            int32_t line_center  = lv_font_get_line_height(font) / 2 - font->base_line;
            int32_t glyph_center = g.ofs_y + (int32_t)g.box_h / 2;
            dy = glyph_center - line_center;
        }
    }
    // Center within the on-screen portion: shift left by half the clipped strip.
    lv_obj_align(label, LV_ALIGN_CENTER, -clipped_right / 2, dy);

    if (out_label) *out_label = label;
    return btn;
}

static void kb_flash_clear_cb(lv_timer_t *t) {
    lv_obj_t *btn = (lv_obj_t *)lv_timer_get_user_data(t);
    if (btn && lv_obj_is_valid(btn)) lv_obj_remove_state(btn, LV_STATE_PRESSED);
    lv_timer_delete(t);
}

void kb_flash_side_button(lv_obj_t *btn) {
    if (!btn || !lv_obj_is_valid(btn)) return;
    lv_obj_add_state(btn, LV_STATE_PRESSED);
    lv_timer_t *t = lv_timer_create(kb_flash_clear_cb, 120, btn);
    lv_timer_set_repeat_count(t, 1);
}

// ===========================================================================
// Joystick directional navigation
// ===========================================================================

void kb_handle_directional(lv_event_t *e, const char * const *map,
                           lv_obj_t *matrix, lv_obj_t *back_btn) {
    uint32_t key = lv_event_get_key(e);

    // UP on the top row hands focus up to the back button. The buttonmatrix does
    // not wrap UP off the top row, so this is the seam between the keyboard's
    // internal navigation and the top-nav zone.
    if (key == LV_KEY_UP) {
        if (!back_btn || !lv_obj_is_valid(back_btn)) return;
        uint32_t sel = lv_buttonmatrix_get_selected_button(matrix);
        if (sel == LV_BUTTONMATRIX_BUTTON_NONE) return;
        if (sel < kb_top_row_count(map)) {
            lv_group_focus_obj(back_btn);
        }
    }

    // LEFT/RIGHT wrap within the current row rather than spilling onto the
    // adjacent row. As a PREPROCESS handler we see the selection before the
    // buttonmatrix moves it, set the wrapped target ourselves, and stop the event
    // so its default linear (cross-row) move does not run.
    if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
        uint32_t sel = lv_buttonmatrix_get_selected_button(matrix);
        if (sel == LV_BUTTONMATRIX_BUTTON_NONE) return;  // let the default enter the grid
        uint32_t first, last;
        kb_row_bounds(map, sel, &first, &last);
        uint32_t target = (key == LV_KEY_RIGHT) ? (sel >= last  ? first : sel + 1)
                                                : (sel <= first ? last  : sel - 1);
        lv_buttonmatrix_set_selected_button(matrix, target);
        lv_event_stop_processing(e);
    }
}

void kb_back_down_to_matrix(lv_event_t *e, lv_obj_t *matrix) {
    if (lv_event_get_key(e) != LV_KEY_DOWN) return;
    if (matrix && lv_obj_is_valid(matrix)) {
        lv_group_focus_obj(matrix);
    }
}

void kb_connect_indevs(lv_group_t *group) {
    // Shared input handoff: attaches the keypad/encoder indevs to this keyboard's
    // group and latches held keys so a key still held from the screen that opened
    // the keyboard can't bleed through as a keystroke. See navigation.h.
    attach_keypad_indevs_to_group(group);
}
