// reset_screen
//
// LVGL port of Python's ResetScreen (screen.py:980). Shown while SeedSigner
// restarts and wipes its in-memory data. A plain title over a centered body
// message, with NO back button and NO power button — the app restarts on its
// own, so there is nothing for the user to dismiss.
//
// cfg:
//   top_nav.title (str) — default "Restarting".
//   text (str)          — default the restart/wipe message.

#include "screen_scaffold.h"   // parse/scaffold/nav/load helpers (screen_scaffold.cpp)
#include "seedsigner.h"        // screen_scaffold_t
#include "screen_helpers.h"    // make_body_text_label, apply_body_tight_line_spacing
#include "gui_constants.h"     // EDGE_PADDING (via body content width)
#include "navigation.h"        // NAV_BODY_VERTICAL, NAV_INDEX_NONE

#include "lvgl.h"

#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

void reset_screen(void *ctx_json) {
    json cfg;
    parse_screen_json_ctx((const char *)ctx_json, cfg);

    // Top nav: Python ResetScreen sets show_back_button = False and inherits
    // show_power_button = False — so no nav buttons are shown at all.
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "Restarting";
    if (!cfg["top_nav"].contains("show_back_button"))  cfg["top_nav"]["show_back_button"]  = false;
    if (!cfg["top_nav"].contains("show_power_button")) cfg["top_nav"]["show_power_button"] = false;

    std::string text = cfg.value(
        "text", std::string("SeedSigner is restarting.\n\nAll in-memory data will be wiped."));

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, true);

    // No button_list -> scaffold Mode 1: body == upper_body, a plain (non-flex)
    // scroll container spanning the area below the top nav. Render one centered,
    // wrapped body label and vertically center it in that area — matching Python's
    // full-height TextArea, which vertically centers a body that fits
    // (is_text_centered=True; text_offset_y = (height - total_text_height)/2).
    lv_obj_t *label = make_body_text_label(screen.body, text.c_str(),
                                           lv_obj_get_content_width(screen.body));
    apply_body_tight_line_spacing(label);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    // No focusable body items; nav just wires the (here absent) top-nav buttons.
    bind_screen_navigation(cfg, screen, NULL, 0, NAV_BODY_VERTICAL, NAV_INDEX_NONE);

    load_screen_and_cleanup_previous(screen.screen);
}
