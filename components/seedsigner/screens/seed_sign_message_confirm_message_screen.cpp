// seed_sign_message_confirm_message_screen
//
// LVGL port of Python's SeedSignMessageConfirmMessageScreen (seed_screens.py:1605).
// Reviews the FULL message about to be signed.
//
// The entire message is shown as ONE left-aligned, scrollable body over a bottom-pinned
// "Next" button. When the message overflows the viewport, bind_screen_navigation's
// scroll-then-buttons navigation scrolls the content (joystick down / touch swipe) until
// the Next button is revealed and selectable; a short message just shows Next at the
// bottom. The host passes the whole message — no paging, no per-page hand-off.
//
// (This replaces an earlier screen-owned paging design; per the UI decision, plain vertical
// scroll to reveal the bottom button is used instead.)
//
// cfg:
//   message (str, req.)  — the full message to review.
//   top_nav.title (str)  — default "Review Message".
//   button_list (array)  — default ["Next"].

#include "screen_scaffold.h"   // parse/scaffold/nav/load helpers (defined in screen_scaffold.cpp)
#include "seedsigner.h"        // screen_scaffold_t
#include "gui_constants.h"     // BODY_FONT, colors
#include "navigation.h"        // NAV_BODY_VERTICAL

#include "lvgl.h"

#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

// Defined in screen_helpers.cpp: the status/intro body's tight, ink-derived
// inter-line advance so multi-line body text matches the PIL reference.
void apply_body_tight_line_spacing(lv_obj_t *label);

void seed_sign_message_confirm_message_screen(void *ctx_json) {
    json cfg;
    parse_screen_json_ctx((const char *)ctx_json, cfg);

    std::string message = cfg.value("message", std::string());

    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "Review Message";
    cfg["is_bottom_list"] = true;                                  // Python is_bottom_list = True
    if (!cfg.contains("button_list")) cfg["button_list"] = json::array({ "Next" });

    // The scaffold gives a screen a separate, scrollable `upper_body` (above the buttons)
    // only when cfg["text"] is a non-empty string — it builds the structure but does NOT
    // render the text. Set it to the message to take that path, then render our OWN
    // left-aligned body label into upper_body (Python is_text_centered = False; the generic
    // intro-text path would center it). Overflow → scroll-then-buttons via the nav layer.
    cfg["text"] = message;

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, true);

    if (!message.empty() && screen.upper_body && screen.upper_body != screen.body) {
        lv_obj_t *msg = lv_label_create(screen.upper_body);
        lv_label_set_text(msg, message.c_str());
        lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(msg, lv_obj_get_content_width(screen.upper_body));
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        lv_obj_set_style_text_color(msg, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_text_font(msg, &BODY_FONT, LV_PART_MAIN);
        // Match the PIL reference's tight, ink-based line advance (same as the status /
        // intro body text) instead of LVGL's looser default.
        apply_body_tight_line_spacing(msg);
    }

    // NAV_INDEX_NONE (like the status screens): when the message FITS, Next is active; when
    // it OVERFLOWS, start UNFOCUSED at the TOP so the reader sees the message from its
    // beginning and scrolls DOWN through it to reveal + focus the Next button — rather than
    // loading pre-scrolled to the bottom with Next focused.
    bind_screen_navigation(cfg, screen, NAV_INDEX_NONE);

    load_screen_and_cleanup_previous(screen.screen);
}
