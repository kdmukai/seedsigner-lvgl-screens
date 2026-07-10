// settings_qr_confirmation_screen
//
// Python provenance: SettingsQRConfirmationScreen (settings_screens.py)
//
// Post-scan confirmation for the SettingsQR ingest flow: ButtonListScreen chrome
// (is_bottom_list=True; Python's __post_init__ forces show_back_button=False)
// with a single bottom "Home" action, plus TWO absolutely-positioned centered
// TextAreas:
//   1. the scanned config's name, quoted, at screen_y = top_nav.height + 20
//      (only when a config_name is present);
//   2. the localized status message 50 px below it — i.e. top_nav.height + 70
//      when a config_name is present, or top_nav.height + 20 when it is absent.
// The reference PNGs (SettingsIngestSettingsQRView_persistent / _not_persistent)
// were rendered by exactly this class: the two blocks sit a FIXED 50 px apart,
// which a single "\n\n"-joined label (uniform line spacing) cannot reproduce.
//
// COMPOSE-AND-OVERLAY technique (spec §8 — RESERVED, rollout decision pending;
// do not add a third consumer): this file and tools_calc_final_word_screen build
// their chrome by mutating a COPY of cfg and delegating to the public
// button_list_screen() — which builds the scaffold, binds navigation, AND loads
// the screen — then overlay their body labels on lv_scr_act() afterwards.
// (Verified: the top_nav + Home button land pixel-for-pixel on the reference.)
// KNOWN FLAGGED VIOLATION (load-tail invariant, spec §5): the overlay labels are
// created AFTER load_screen_and_cleanup_previous has already run inside the
// delegate, so they never receive the RTL / glyph-run post-passes — a localized
// status_message renders with LTR base direction / unshaped codepoints on RTL
// (ur/fa) and shaped (hi/th) locales. Flagged in the bug ledger, not fixed here.
//
// Layout notes: the Python screen_y offsets (20 / 50) are 240-height reference
// px, scaled by px_multiplier for the taller profiles. On the parity profiles
// (240x240 / 320x240, px_multiplier == 100) the block pair keeps Python's exact
// top anchoring; on taller profiles Python has no reference, so the pair is
// vertically re-centered in the free band between the top_nav and the Home
// button (house convention for taller-than-reference profiles; the vertical-
// slack policy of spec §11 is still open, so this site keeps its exact current
// centering math). Deviation from Python: __post_init__ forces
// show_back_button=False, while this port leaves the flag host-supplied (the
// reference cfg passes false) with the scaffold's write-if-absent default of true.
//
// Lifecycle: Tier 1 (stateless) — no heap ctx, no timers, no cleanup callback;
// the overlay labels die with the screen. Forced (non-defaulted) chrome
// overrides: text="" (suppresses the delegate's flowed intro block) and
// is_bottom_list=true (Python: is_bottom_list = True).
//
// cfg:
//   status_message            (string, required)     localized status paragraph
//            (Python: _(status_message) — content arrives localized from the host).
//   config_name               (string, optional)     user-supplied config name
//            scanned from the SettingsQR — rendered quoted, never translated
//            (Python: config_name=None default skips the block entirely).
//   top_nav.title             (string, required)     localized screen title (read
//            by the scaffold via the delegated button_list_screen; enforced here
//            via require_top_nav_title so the throw carries this screen's name).
//   top_nav.show_back_button  (bool, default true)   structural write-if-absent
//            (the scaffold's implicit default; the reference cfg passes false —
//            see the layout-notes deviation above).
//   top_nav.show_power_button (bool, default false)  structural write-if-absent.
//   button_list               (array, default ["Home"])  the bottom action row
//            (Python: button_data = [ButtonOption("Home")]). English content
//            default KEPT and flagged: the scenarios omit the key, so the §5
//            required-throw conversion cannot be applied yet.
//   All remaining keys (top_nav.icon / icon_color, is_button_text_centered,
//   button_style, checked_buttons, initial_selected_index, input.mode,
//   input.keys.*, allow_screensaver) pass through unchanged to the delegated
//   button_list_screen / scaffold / navigation layer — see button_list_screen's
//   banner for their contract.

#include "screen_scaffold.h"  // parse_screen_json_ctx
#include "seedsigner.h"       // settings_qr_confirmation_screen decl, button_list_screen (delegated chrome), text_top_leading
#include "gui_constants.h"    // TOP_NAV_HEIGHT, EDGE_PADDING, LIST_ITEM_PADDING, BODY_FONT, BODY_FONT_COLOR, active_profile
#include "screen_helpers.h"   // tight_line_space, ensure_top_nav_structure, require_top_nav_title

#include "lvgl.h"             // lv_label + absolute align/coords (post-load overlay placement)

#include <nlohmann/json.hpp>  // json (cfg reads + chrome-copy mutation)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string

using json = nlohmann::json;


namespace {

// Create a centered, wrapped BODY_FONT label whose VISIBLE ink top lands at the
// canvas-relative `screen_y` (Python TextArea.screen_y semantics). LVGL anchors a
// label box by the font ascent, which carries leading above the caps; subtract that
// leading so the visible text lands where PIL/Python draws it (splash/status pattern).
// Returns the label so the caller can re-center the group on the taller profiles.
lv_obj_t *settings_qr_confirmation_place_centered_body_text(lv_obj_t *screen,
                                                            const char *text,
                                                            int32_t screen_y) {
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
    // NOTE (flagged, kept as-is): this measures the RAW `text` argument, not the
    // label's STORED text — under LV_USE_ARABIC_PERSIAN_CHARS the stored text is
    // rewritten into presentation forms, so fa under-measures and falls to the
    // spacing clamp. Behavioral; recorded in the bug ledger.
    lv_obj_set_style_text_line_space(label,
                                     tight_line_space(&BODY_FONT, text, LIST_ITEM_PADDING / 2),
                                     LV_PART_MAIN);

    const int32_t lead = text_top_leading(&BODY_FONT, text);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, screen_y - lead);
    return label;
}

// Depth-first search for the first LVGL button under `obj`. The delegated
// button_list_screen returns no object handles, so the bottom "Home" action must
// be re-discovered from the widget tree (a symptom of the RESERVED §8 technique —
// kept as-is pending the rollout decision). ASSUMES the reference chrome
// (show_back_button=false, show_power_button=false): with no top-nav buttons the
// first button in tree order IS the Home action. A cfg that shows a top-nav
// button would be found first and skew the centering region below — flagged in
// the bug ledger, kept as-is.
lv_obj_t *settings_qr_confirmation_find_first_button(lv_obj_t *obj) {
    if (obj && lv_obj_check_type(obj, &lv_button_class)) {
        return obj;
    }
    if (!obj) return nullptr;
    uint32_t child_count = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t *found = settings_qr_confirmation_find_first_button(lv_obj_get_child(obj, i));
        if (found) return found;
    }
    return nullptr;
}

}  // namespace


void settings_qr_confirmation_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required field: status_message is user-visible CONTENT, which always arrives
    // localized from the host view layer (a string literal baked here would be
    // English-only by construction). One throw, before any LVGL work (the delegate
    // has not run yet), so no throw path can leak LVGL objects.
    if (!cfg.contains("status_message") || !cfg["status_message"].is_string()) {
        throw std::runtime_error("settings_qr_confirmation_screen: status_message is required and must be a string");
    }
    std::string status_message = cfg["status_message"].get<std::string>();

    // config_name is a user-supplied string scanned from the SettingsQR — quoted,
    // never translated, and genuinely OPTIONAL (Python: config_name=None default
    // skips its TextArea entirely).
    std::string config_name;
    if (cfg.contains("config_name") && cfg["config_name"].is_string()) {
        config_name = cfg["config_name"].get<std::string>();
    }

    // Structural defaults (write-if-absent, never user-visible text), matching the
    // scaffold's implicit fallbacks (show_back=true / show_power=false) so the
    // writes are representation-only; the reference cfg passes show_back_button=
    // false (Python forces it — see the banner deviation note). The localized
    // title itself is content and must come from the host; validating it HERE
    // (before the delegate) makes the throw carry this screen's name.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "settings_qr_confirmation_screen");

    // --- Chrome (delegated scaffold + navigation + load, spec §8) ---

    // Build the ButtonListScreen chrome (top_nav + bottom "Home" button + nav +
    // screen load) via button_list_screen with NO intro text. Preserve every
    // host-provided key (top_nav / locale / input); drop this screen's own keys so
    // button_list_screen sees a clean cfg. The delegate runs the ENTIRE §5 tail —
    // bind_screen_navigation + load_screen_and_cleanup_previous — which is why the
    // body overlay below happens post-load (the flagged §5 violation in the banner).
    json chrome = cfg;
    chrome.erase("config_name");
    chrome.erase("status_message");
    chrome["text"] = "";                 // forced — no flowed intro text; we overlay the blocks
    if (!chrome.contains("button_list")) {
        // Python: button_data = [ButtonOption("Home")]. Host may override. English
        // content default KEPT (flagged): the scenarios omit button_list, so the §5
        // required-throw conversion cannot be applied yet.
        chrome["button_list"] = json::array({ "Home" });
    }
    chrome["is_bottom_list"] = true;     // forced, not defaulted — Python: is_bottom_list = True

    const std::string chrome_str = chrome.dump();
    button_list_screen((void *)chrome_str.c_str());

    // --- Body (post-load overlay) ---

    // Overlay the two centered TextAreas on the just-loaded screen, in the Python
    // build order (config_name first, then status_message).
    lv_obj_t *screen = lv_scr_act();

    // Python screen_y offsets (20 / 50) are 240-height reference px; scale them for
    // the taller display profiles, mirroring the rest of the layout.
    const int32_t px_multiplier = active_profile().px_multiplier;
    int32_t start_y = TOP_NAV_HEIGHT + (20 * px_multiplier / 100);

    lv_obj_t *config_label = nullptr;
    lv_obj_t *status_label = nullptr;

    // 1. The quoted config name (skipped when absent — Python's `if self.config_name`).
    if (!config_name.empty()) {
        const std::string quoted = "\"" + config_name + "\"";
        config_label = settings_qr_confirmation_place_centered_body_text(screen, quoted.c_str(), start_y);
        start_y += (50 * px_multiplier / 100);   // config_name_textarea.screen_y + 50
    }

    // 2. The localized status message.
    status_label = settings_qr_confirmation_place_centered_body_text(screen, status_message.c_str(), start_y);

    // --- Geometry (taller-profile vertical recenter) ---

    // On the parity profiles (240x240 and 320x240, both px_multiplier == 100) the
    // layout is the exact Python top-anchored one — leave it. On the taller ESP32
    // profiles (px_multiplier > 100) Python has no reference, so apply the house
    // convention for taller-than-reference profiles: vertically center the
    // two-block group in the space between the top_nav and the bottom "Home"
    // button (the spec §11 vertical-slack policy is still open; this site keeps
    // its exact current centering math until that ruling).
    if (px_multiplier > 100) {
        lv_obj_update_layout(screen);

        lv_obj_t *top_block = config_label ? config_label : status_label;
        const int32_t group_top    = lv_obj_get_y(top_block);
        const int32_t group_bottom = lv_obj_get_y(status_label) + lv_obj_get_height(status_label);
        const int32_t group_height = group_bottom - group_top;

        // Region: below the top_nav, above the Home button. The button is a child of
        // the body (which itself sits TOP_NAV_HEIGHT down), so lv_obj_get_y() would be
        // body-relative; use absolute coords so it shares the labels' screen frame.
        lv_obj_t *home_button = settings_qr_confirmation_find_first_button(screen);
        const int32_t region_top    = TOP_NAV_HEIGHT;
        int32_t region_bottom = lv_obj_get_height(screen) - TOP_NAV_HEIGHT;
        if (home_button) {
            lv_area_t button_coords, screen_coords;
            lv_obj_get_coords(home_button, &button_coords);
            lv_obj_get_coords(screen, &screen_coords);
            region_bottom = button_coords.y1 - screen_coords.y1;
        }
        const int32_t desired_top = region_top + (region_bottom - region_top - group_height) / 2;
        const int32_t delta       = desired_top - group_top;

        if (config_label) {
            lv_obj_set_y(config_label, lv_obj_get_y(config_label) + delta);
        }
        lv_obj_set_y(status_label, lv_obj_get_y(status_label) + delta);
    }
}
