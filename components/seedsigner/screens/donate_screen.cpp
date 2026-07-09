// donate_screen
//
// LVGL port of Python's DonateScreen (settings_screens.py:295). Title "Donate"
// over a body paragraph, then a larger, accent-colored "seedsigner.com" line.
// Back button (BaseTopNavScreen default show_back_button = True); no power button.
//
// Unlike Reset / PowerOff (which vertically center a single body), Donate is
// TOP-anchored at explicit offsets, mirroring Python's screen_y placement:
//   - paragraph at top_nav.height + 3*COMPONENT_PADDING
//   - URL line one COMPONENT_PADDING below the paragraph.
//
// The URL line is 28px. It uses the runtime seedsigner_latin_font(28) — the
// memoized OpenSans tiny_ttf rasterizer for body-relative sizes the profile
// roles don't cover — so no new font bake is needed. (Python renders this line
// with supersampling_factor=1, i.e. a plain render, which the tiny_ttf path
// matches.)
//
// cfg:
//   top_nav.title (str) — default "Donate".
//   text (str)          — default the "100% free & open source…" paragraph.
//   url  (str)          — default "seedsigner.com".

#include "screen_scaffold.h"   // parse/scaffold/nav/load helpers (screen_scaffold.cpp)
#include "seedsigner.h"        // screen_scaffold_t
#include "screen_helpers.h"    // make_body_text_label, apply_body_tight_line_spacing
#include "gui_constants.h"     // COMPONENT_PADDING, ACCENT_COLOR, seedsigner_latin_font
#include "navigation.h"        // NAV_BODY_VERTICAL, NAV_INDEX_NONE

#include "lvgl.h"

#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

void donate_screen(void *ctx_json) {
    json cfg;
    parse_screen_json_ctx((const char *)ctx_json, cfg);

    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "Donate";
    if (!cfg["top_nav"].contains("show_back_button"))  cfg["top_nav"]["show_back_button"]  = true;
    if (!cfg["top_nav"].contains("show_power_button")) cfg["top_nav"]["show_power_button"] = false;

    std::string body_text = cfg.value(
        "text", std::string("SeedSigner is 100% free & open source, funded solely by the "
                            "Bitcoin community.\n\nDonate onchain or LN at:"));
    std::string url_text = cfg.value("url", std::string("seedsigner.com"));

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, true);

    // Scaffold Mode 1 (no button_list): body == upper_body, a plain scroll container.
    const int32_t body_w = lv_obj_get_content_width(screen.body);

    // Body paragraph — centered, wrapped, top-anchored 3*COMPONENT_PADDING below the
    // top nav (Python screen_y = top_nav.height + 3*COMPONENT_PADDING).
    lv_obj_t *para = make_body_text_label(screen.body, body_text.c_str(), body_w);
    apply_body_tight_line_spacing(para);
    lv_obj_align(para, LV_ALIGN_TOP_MID, 0, 3 * COMPONENT_PADDING);

    // Accent URL line — 28px OpenSans in ACCENT_COLOR, centered, one COMPONENT_PADDING
    // below the paragraph. align_to needs `para`'s coords resolved first.
    lv_obj_update_layout(screen.body);

    lv_obj_t *url = lv_label_create(screen.body);
    lv_label_set_text(url, url_text.c_str());
    lv_obj_set_width(url, body_w);
    lv_obj_set_style_text_align(url, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(url, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_font(url, seedsigner_latin_font(28), LV_PART_MAIN);
    lv_obj_align_to(url, para, LV_ALIGN_OUT_BOTTOM_MID, 0, COMPONENT_PADDING);

    // Only the top-nav back button is focusable; no body items.
    bind_screen_navigation(cfg, screen, NULL, 0, NAV_BODY_VERTICAL, NAV_INDEX_NONE);

    load_screen_and_cleanup_previous(screen.screen);
}
