// power_off_not_required_screen
//
// LVGL port of Python's PowerOffNotRequiredScreen (screen.py:995). Title
// "Just Unplug It" over a centered body message reassuring the user that the
// device can be unplugged at any time. Has a back button (Python
// show_back_button = True); no power button.
//
// cfg:
//   top_nav.title (str) — default "Just Unplug It".
//   text (str)          — default the "safe to disconnect" message.

#include "screen_scaffold.h"   // parse/scaffold/nav/load helpers (screen_scaffold.cpp)
#include "seedsigner.h"        // screen_scaffold_t
#include "screen_helpers.h"    // make_body_text_label, apply_body_tight_line_spacing
#include "gui_constants.h"     // EDGE_PADDING (via body content width)
#include "navigation.h"        // NAV_BODY_VERTICAL, NAV_INDEX_NONE

#include "lvgl.h"

#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

void power_off_not_required_screen(void *ctx_json) {
    json cfg;
    parse_screen_json_ctx((const char *)ctx_json, cfg);

    // Top nav: Python sets show_back_button = True and inherits
    // show_power_button = False.
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "Just Unplug It";
    if (!cfg["top_nav"].contains("show_back_button"))  cfg["top_nav"]["show_back_button"]  = true;
    if (!cfg["top_nav"].contains("show_power_button")) cfg["top_nav"]["show_power_button"] = false;

    std::string text = cfg.value(
        "text", std::string("It is safe to disconnect power at any time."));

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, true);

    // No button_list -> scaffold Mode 1 (body == upper_body). One centered, wrapped
    // body label, vertically centered to match Python's full-height TextArea.
    lv_obj_t *label = make_body_text_label(screen.body, text.c_str(),
                                           lv_obj_get_content_width(screen.body));
    apply_body_tight_line_spacing(label);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    // Only the top-nav back button is focusable; no body items.
    bind_screen_navigation(cfg, screen, NULL, 0, NAV_BODY_VERTICAL, NAV_INDEX_NONE);

    load_screen_and_cleanup_previous(screen.screen);
}
