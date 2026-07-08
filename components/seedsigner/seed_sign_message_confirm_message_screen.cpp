// seed_sign_message_confirm_message_screen
//
// LVGL port of Python's SeedSignMessageConfirmMessageScreen
// (seedsigner/gui/screens/seed_screens.py:1605). Reviews ONE page of a
// message that is about to be signed.
//
// Layout (Python parity):
//   - ButtonListScreen chrome: top_nav title + a single bottom-pinned "Next"
//     button (is_bottom_list = True).
//   - Title: "Review Message" when the message is a single page, or
//     "Message (pt N/M)" for a multi-page message. The host owns the reflow
//     into pages and passes THIS page's text + the already-resolved title; the
//     screen renders exactly one page's text block.
//   - A LEFT-aligned (NOT centered) TextArea of the page's message positioned at
//     screen_y = TOP_NAV_HEIGHT + COMPONENT_PADDING, wrapping to
//     canvas_width - 2*EDGE_PADDING.
//
// Composition strategy: delegate the chrome (top_nav + bottom-pinned Next button
// + navigation wiring) to the PUBLIC button_list_screen(), then overlay the
// left-aligned body label onto the loaded screen. This reuses the parity-tuned
// scaffold without duplicating any of it. The scenario JSON therefore carries the
// standard button_list/top_nav/is_bottom_list keys PLUS our own "message" key
// (which button_list_screen ignores).

#include "seedsigner.h"
#include "gui_constants.h"
#include "lvgl.h"

#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

// Defined (non-static) in seedsigner.cpp: applies the status/intro body's tight,
// ink-derived inter-line advance so multi-line body text matches the PIL/Python
// reference instead of LVGL's looser declared line_height. Reused here verbatim so
// the message block stacks at the same line pitch Python's TextArea uses.
void apply_body_tight_line_spacing(lv_obj_t *label);


void seed_sign_message_confirm_message_screen(void *ctx_json) {
    // 1. Build the ButtonListScreen chrome via the public entry point: top_nav +
    //    a bottom-pinned "Next" button + navigation. button_list_screen loads the
    //    finished screen as the active screen. (It reads top_nav / button_list /
    //    is_bottom_list from the same JSON and ignores our extra "message" key.)
    button_list_screen(ctx_json);

    // 2. Pull THIS page's message text out of the same config. Host-side reflow
    //    guarantees a single page's worth of text here; the screen never touches
    //    the Controller (unlike the current Python TODO).
    std::string message;
    try {
        json cfg = json::parse(reinterpret_cast<const char *>(ctx_json));
        message = cfg.value("message", std::string());
    } catch (...) {
        // Malformed JSON already surfaced inside button_list_screen; nothing to add.
    }
    if (message.empty()) {
        return;
    }

    // 3. Overlay the LEFT-aligned message block on the loaded screen. Python's
    //    TextArea is drawn with is_text_centered=False, so its glyphs left-justify
    //    at EDGE_PADDING and wrap to canvas_width - 2*EDGE_PADDING.
    lv_obj_t *screen = lv_screen_active();
    const DisplayProfile &profile = active_profile();

    int32_t wrap_width = profile.width - 2 * EDGE_PADDING;

    lv_obj_t *message_label = lv_label_create(screen);
    lv_label_set_text(message_label, message.c_str());
    lv_label_set_long_mode(message_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(message_label, wrap_width);
    lv_obj_set_style_text_align(message_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_text_color(message_label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_font(message_label, &BODY_FONT, LV_PART_MAIN);

    // The overlay label is static content, never a scroll/focus target.
    lv_obj_remove_flag(message_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(message_label, LV_OBJ_FLAG_CLICKABLE);

    // PIL-matched line pitch: Python's TextArea advances each line by
    // (ink ascent + BODY_LINE_SPACING), which is tighter than LVGL's default
    // (declared line_height + inherited BODY_LINE_SPACING). Without this the block
    // drifts progressively lower line-by-line vs the reference.
    apply_body_tight_line_spacing(message_label);

    // Vertical placement. Python anchors the TextArea image top (≈ the VISIBLE
    // glyph top) at screen_y; LVGL anchors the font ascent (leading above the
    // caps), so subtract text_top_leading() to land the visible text where Python
    // places it. (Same correction the status/intro body labels use.)
    int32_t start_y = TOP_NAV_HEIGHT + COMPONENT_PADDING;
    int32_t leading = text_top_leading(&BODY_FONT, message.c_str());
    int32_t x = EDGE_PADDING;

    if (profile.px_multiplier == PX_MULTIPLIER_100) {
        // 240-px-height profiles (Pi Zero 240x240 reference, plus the 320x240 wide
        // variant): top-anchor at screen_y, matching Python exactly.
        lv_obj_set_pos(message_label, x, start_y - leading);
    } else {
        // Taller 320/480-px-height displays have no Python reference. Vertically
        // center the message block in the band between the top_nav (+padding) and
        // the bottom-pinned Next button so a short message is not stranded high on
        // a much taller screen.
        int32_t band_top = start_y;
        int32_t band_bottom =
            profile.height - EDGE_PADDING - BUTTON_HEIGHT - COMPONENT_PADDING;

        lv_obj_update_layout(message_label);
        int32_t label_height = lv_obj_get_height(message_label);

        int32_t y = band_top + ((band_bottom - band_top) - label_height) / 2 - leading;
        if (y < start_y - leading) {
            y = start_y - leading;  // never above the top-anchored position
        }
        lv_obj_set_pos(message_label, x, y);
    }
}
