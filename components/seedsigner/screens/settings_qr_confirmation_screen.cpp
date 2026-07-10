#include "screen_scaffold.h" // shared screen-helper header (per multi-agent integration convention)
#include "seedsigner.h"      // settings_qr_confirmation_screen(), button_list_screen(), text_top_leading()
#include "gui_constants.h"   // TOP_NAV_HEIGHT, EDGE_PADDING, BODY_FONT, BODY_FONT_COLOR, active_profile()

#include "lvgl.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// settings_qr_confirmation_screen
// ---------------------------------------------------------------------------
//
// Parity target: Python `SettingsQRConfirmationScreen` (settings_screens.py:318),
// a `ButtonListScreen` (is_bottom_list=True, show_back_button=False) with a single
// "Home" button and TWO absolutely-positioned centered `TextArea`s:
//   - config_name  : text = `"<config_name>"`, screen_y = top_nav.height + 20
//   - status_message: text = <status_message>, screen_y = (config_name.screen_y + 50)
//                     — i.e. top_nav.height + 70 when a config_name is present, or
//                     top_nav.height + 20 when it is absent.
// The reference PNGs (SettingsIngestSettingsQRView_persistent / _not_persistent)
// were rendered by exactly this class: the two blocks sit at a FIXED 50 px apart,
// which a single `\n\n`-joined label (uniform line spacing) cannot reproduce.
//
// Implementation strategy — build the chrome once, overlay the two blocks:
//   1. Delegate to `button_list_screen` with an EMPTY intro text. That produces the
//      exact ButtonListScreen chrome the reference uses — top_nav (no back arrow),
//      the bottom-pinned "Home" button, joystick/touch nav wiring, and the screen
//      load — with zero duplication of the shared scaffold (screen_scaffold.cpp).
//      (Verified: the top_nav + Home button land pixel-for-pixel on the reference.)
//   2. Overlay the two centered body labels on the freshly-loaded screen at their
//      absolute Python `screen_y`, since ButtonListScreen positions these TextAreas
//      absolutely (NOT in the button-list flow).
//
// cfg contract:
//   {
//     "top_nav": { "title": "Settings QR", "show_back_button": false,
//                  "show_power_button": false },
//     "config_name":    "English noob mode",   // OPTIONAL user string (quoted, not translated)
//     "status_message": "Persistent Settings enabled. Settings saved to SD card."
//     // "button_list" OPTIONAL: host may override the action label; defaults to ["Home"].
//   }

// Ink-based tight inter-line spacing, mirroring screen_helpers.cpp's
// tight_line_space()/apply_body_tight_line_spacing() (kept screen-private here per
// the multi-agent brief). LVGL's declared font line_height carries loose leading, so
// a plain wrapped label spaces its lines ~10 px looser than the PIL/Python TextArea
// reference. Instead derive the advance from the ACTUAL glyph ink of `text` (tallest
// ascender + deepest descender + a small gap) and hand the delta to
// lv_obj_set_style_text_line_space (rendered advance = font line_height + this),
// which is usually negative (tightening). Matches the reference status paragraph.
static int32_t tight_body_line_space(const lv_font_t *font, const char *text)
{
    if (!font || !text) return 0;

    int32_t max_ascent = 0, max_descent = 0;

    // Minimal UTF-8 walk; ask the font engine for each glyph's ink box.
    for (const unsigned char *p = (const unsigned char *)text; *p; ) {
        uint32_t cp;
        if (*p < 0x80) {
            cp = *p; p += 1;
        } else if ((*p >> 5) == 0x6 && p[1]) {
            cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2;
        } else if ((*p >> 4) == 0xE && p[1] && p[2]) {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3;
        } else if ((*p >> 3) == 0x1E && p[1] && p[2] && p[3]) {
            cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F); p += 4;
        } else {
            p += 1; continue;
        }
        if (cp == '\n' || cp == '\r' || cp == ' ') continue;

        lv_font_glyph_dsc_t d;
        if (!lv_font_get_glyph_dsc(font, &d, cp, 0) || d.box_h == 0) continue;  // absent/inkless

        int32_t ascent  = (int32_t)d.ofs_y + (int32_t)d.box_h;  // ink above baseline
        int32_t descent = -(int32_t)d.ofs_y;                    // ink below baseline
        if (ascent  > max_ascent)  max_ascent  = ascent;
        if (descent > max_descent) max_descent = descent;
    }

    if (max_ascent + max_descent <= 0) return 0;  // measured nothing; leave default

    const int32_t line_gap = LIST_ITEM_PADDING / 2;   // screen_helpers.cpp uses the same gap
    const int32_t line_h   = (int32_t)lv_font_get_line_height(font);
    int32_t space = (max_ascent + max_descent + line_gap) - line_h;

    // Never tighten more than a quarter of the declared line_height (collapse guard
    // for scripts whose drawn glyphs differ from their codepoints).
    const int32_t min_space = -(line_h / 4);
    if (space < min_space) space = min_space;
    return space;
}

// Create a centered, wrapped BODY_FONT label whose VISIBLE ink top lands at the
// canvas-relative `screen_y` (Python TextArea.screen_y semantics). LVGL anchors a
// label box by the font ascent, which carries leading above the caps; subtract that
// leading so the visible text lands where PIL/Python draws it (splash/status pattern).
// Returns the label so the caller can re-center the group on the taller profiles.
static lv_obj_t *place_centered_body_text(lv_obj_t *screen, const char *text, int32_t screen_y)
{
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

    // Python TextArea defaults to canvas width and wraps at width - 2*EDGE_PADDING,
    // centering the text on the full canvas. A (canvas - 2*EDGE_PADDING)-wide box,
    // centered on the screen (TOP_MID) with centered text, is equivalent.
    lv_obj_set_width(label, lv_obj_get_width(screen) - 2 * EDGE_PADDING);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &BODY_FONT, LV_PART_MAIN);
    // Tight, ink-based inter-line spacing so a wrapped status paragraph matches the
    // PIL TextArea reference (the inherited screen-wide BODY_LINE_SPACING is ~10 px
    // too loose). No-op for single-line text (e.g. the config name).
    lv_obj_set_style_text_line_space(label, tight_body_line_space(&BODY_FONT, text), LV_PART_MAIN);

    const int32_t lead = text_top_leading(&BODY_FONT, text);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, screen_y - lead);
    return label;
}

// Depth-first search for the first LVGL button under `obj`. With no back/power
// button in the top_nav, the only button on the screen is the bottom "Home" action —
// used to bound the vertical-centering region on the taller (non-240) profiles.
static lv_obj_t *find_first_button(lv_obj_t *obj)
{
    if (obj && lv_obj_check_type(obj, &lv_button_class)) {
        return obj;
    }
    if (!obj) return nullptr;
    uint32_t n = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t *found = find_first_button(lv_obj_get_child(obj, i));
        if (found) return found;
    }
    return nullptr;
}

void settings_qr_confirmation_screen(void *ctx_json)
{
    const char *json_str = (const char *)ctx_json;
    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Pull out this screen's own fields. config_name is a user-supplied string
    // scanned from the SettingsQR — quoted, never translated. status_message is
    // already localized by the host.
    std::string config_name;
    if (cfg.contains("config_name") && cfg["config_name"].is_string()) {
        config_name = cfg["config_name"].get<std::string>();
    }
    std::string status_message;
    if (cfg.contains("status_message") && cfg["status_message"].is_string()) {
        status_message = cfg["status_message"].get<std::string>();
    }

    // 1. Build the ButtonListScreen chrome (top_nav + bottom "Home" button + nav +
    //    screen load) via button_list_screen with NO intro text. Preserve every
    //    host-provided key (top_nav / locale / input); drop this screen's own keys so
    //    button_list_screen sees a clean cfg.
    json chrome = cfg;
    chrome.erase("config_name");
    chrome.erase("status_message");
    chrome["text"] = "";                 // no flowed intro text; we overlay the blocks
    if (!chrome.contains("button_list")) {
        // Python: button_data = [ButtonOption("Home")]. Host may override.
        chrome["button_list"] = json::array({ "Home" });
    }
    chrome["is_bottom_list"] = true;

    const std::string chrome_str = chrome.dump();
    button_list_screen((void *)chrome_str.c_str());

    // 2. Overlay the two centered TextAreas on the just-loaded screen.
    lv_obj_t *screen = lv_scr_act();

    // Python screen_y offsets (20 / 50) are 240-height reference px; scale them for
    // the taller display profiles, mirroring the rest of the layout.
    const int32_t px_mult = active_profile().px_multiplier;
    int32_t start_y = TOP_NAV_HEIGHT + (20 * px_mult / 100);

    lv_obj_t *config_label = nullptr;
    lv_obj_t *status_label = nullptr;

    if (!config_name.empty()) {
        const std::string quoted = "\"" + config_name + "\"";
        config_label = place_centered_body_text(screen, quoted.c_str(), start_y);
        start_y += (50 * px_mult / 100);   // config_name_textarea.screen_y + 50
    }

    status_label = place_centered_body_text(screen, status_message.c_str(), start_y);

    // On the parity profiles (240x240 and 320x240, both px_multiplier == 100) the
    // layout is the exact Python top-anchored one — leave it. On the taller ESP32
    // profiles (px_multiplier > 100) Python has no reference, so apply the house
    // convention: vertically center the two-block group in the space between the
    // top_nav and the bottom "Home" button (feedback_lvgl_scrollable_layout).
    if (px_mult > 100) {
        lv_obj_update_layout(screen);

        lv_obj_t *top_block = config_label ? config_label : status_label;
        const int32_t group_top    = lv_obj_get_y(top_block);
        const int32_t group_bottom = lv_obj_get_y(status_label) + lv_obj_get_height(status_label);
        const int32_t group_h      = group_bottom - group_top;

        // Region: below the top_nav, above the Home button. The button is a child of
        // the body (which itself sits TOP_NAV_HEIGHT down), so lv_obj_get_y() would be
        // body-relative; use absolute coords so it shares the labels' screen frame.
        lv_obj_t *home_btn = find_first_button(screen);
        const int32_t region_top    = TOP_NAV_HEIGHT;
        int32_t region_bottom = lv_obj_get_height(screen) - TOP_NAV_HEIGHT;
        if (home_btn) {
            lv_area_t btn_coords, scr_coords;
            lv_obj_get_coords(home_btn, &btn_coords);
            lv_obj_get_coords(screen, &scr_coords);
            region_bottom = btn_coords.y1 - scr_coords.y1;
        }
        const int32_t desired_top = region_top + (region_bottom - region_top - group_h) / 2;
        const int32_t delta       = desired_top - group_top;

        if (config_label) {
            lv_obj_set_y(config_label, lv_obj_get_y(config_label) + delta);
        }
        lv_obj_set_y(status_label, lv_obj_get_y(status_label) + delta);
    }
}
