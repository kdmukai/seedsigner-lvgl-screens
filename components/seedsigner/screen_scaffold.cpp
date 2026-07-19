// screen_scaffold.cpp — definitions for the shared TopNav/body/navigation scaffold
// declared in screen_scaffold.h. Each per-screen TU under screens/ builds its chrome
// through these entry points (create_top_nav_screen_scaffold, bind_screen_navigation,
// load_screen_and_cleanup_previous, add_warning_edges_overlay, …).

#include "seedsigner.h"
#include "screen_scaffold.h"  // scaffold entry-point declarations (defined in this file)
#include "screen_helpers.h"   // cross-cutting helpers the scaffold calls (defined in screen_helpers.cpp)
#include "qr_core.h"          // shared QR encode/decode core + gutter close button (qr_display + transcribe)
#include "components.h"
#include "camera_preview_overlay.h"
#include "camera_entropy_overlay.h"
#include "keyboard_core.h"
#include "gui_constants.h"
#include "navigation.h"
#include "input_profile.h"
#include "font_registry.h"
#include "glyph_runs.h"
#include "locale_loader.h"   // ss_reap_retired() after the old screen is deleted
#include "locale_picker.h"   // endonym-image rows for settings_locale_picker_screen
#include "overlay_manager.h" // SS_OBJ_FLAG_NO_SCREENSAVER (per-screen saver policy)

#include "lvgl.h"

// Nayuki qrcodegen is BUNDLED inside the LVGL submodule (used by lv_qrcode). We call
// it directly (not the lv_qrcode widget) so we control ECC level (L), mode (numeric for
// SeedQR / byte for the rest), and a fixed quiet zone — see qr_display_screen below.
// Gated by LV_USE_QRCODE: qrcodegen.c only compiles when the flag is set.
#if LV_USE_QRCODE
#include "../../third_party/lvgl/src/libs/qrcode/qrcodegen.h"
#endif

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <set>
#include <map>
#include <algorithm>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>   // psram_alloc: route PSBT-overview geometry off the internal heap
#endif

using json = nlohmann::json;


// Reusable utility: build TopNav from any screen JSON config.
// Reads cfg["top_nav"] and applies defaults when missing.
static lv_obj_t* top_nav_from_screen_json(lv_obj_t* lv_parent, const json &cfg) {
    bool show_back = true;
    bool show_power = false;

    if (!cfg.is_object()) {
        throw std::runtime_error("screen config must be a JSON object");
    }
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) {
        throw std::runtime_error("top_nav object is required");
    }

    const auto &tn = cfg["top_nav"];
    if (!tn.contains("title") || !tn["title"].is_string()) {
        throw std::runtime_error("top_nav.title is required and must be a string");
    }
    std::string title = tn["title"].get<std::string>();

    {
        if (tn.contains("show_back_button")) {
            if (!tn["show_back_button"].is_boolean()) {
                throw std::runtime_error("top_nav.show_back_button must be a boolean");
            }
            show_back = tn["show_back_button"].get<bool>();
        }
        if (tn.contains("show_power_button")) {
            if (!tn["show_power_button"].is_boolean()) {
                throw std::runtime_error("top_nav.show_power_button must be a boolean");
            }
            show_power = tn["show_power_button"].get<bool>();
        }
    }

    return top_nav(lv_parent, title.c_str(), show_back, show_power, NULL, NULL);
}


// Reusable sanity check for incoming screen JSON payloads.
// Throws std::runtime_error on invalid shape/syntax.
void parse_screen_json_ctx(const char *ctx_json, json &cfg_out) {
    if (!ctx_json) {
        throw std::runtime_error("screen JSON context is required");
    }

    try {
        cfg_out = json::parse(ctx_json);
    } catch (...) {
        throw std::runtime_error("invalid JSON syntax");
    }

    if (!cfg_out.is_object()) {
        throw std::runtime_error("screen config must be a JSON object");
    }

    // NB: the per-screen screensaver policy (allow_screensaver) is intentionally NOT
    // normalized here. Its default is per-screen, not one system value — ordinary screens
    // default to allowed, but screens the saver must never cover (QR display, camera scan/
    // entropy, zoomed transcription, loading spinner) default to disallowed. That per-screen
    // default lives in apply_screensaver_policy(), which reads the view's value — present in
    // cfg only when the view set it — against the screen's own default.
}


// Optional-context variant of the parse above (boot/overlay tier: main_menu,
// opening_splash, loading_spinner, the camera overlay screens). A NULL or empty
// context parses the canonical empty object "{}" through the strict helper; every
// other input passes through verbatim — identical validation and error strings.
void parse_optional_screen_json_ctx(const char *ctx_json, json &cfg_out) {
    parse_screen_json_ctx((ctx_json && ctx_json[0]) ? ctx_json : "{}", cfg_out);
}


// Per-screen screensaver policy — see screen_scaffold.h for the contract. Stamps the
// saver opt-out flag onto `screen` when the effective allow_screensaver is false; the
// matching teardown idle-clock reset is wired off this same flag in
// load_screen_and_cleanup_previous(). The value is present in cfg only when the view set
// it (parse no longer bakes a default), so cfg.value() applies the screen's own default.
void apply_screensaver_policy(lv_obj_t *screen, const json &cfg, bool default_allow) {
    if (screen && !cfg.value("allow_screensaver", default_allow)) {
        lv_obj_add_flag(screen, SS_OBJ_FLAG_NO_SCREENSAVER);
    }
}


// LV_EVENT_DELETE handler wired by load_screen_and_cleanup_previous() onto every
// screensaver-opt-out screen: count the screen's teardown as user activity so the
// successor gets a full screensaver idle window (same primitive the overlay manager
// uses when it wakes from the saver).
static void reset_idle_clock_on_delete_cb(lv_event_t *e) {
    (void)e;
    lv_display_trigger_activity(NULL);
}


void load_screen_and_cleanup_previous(lv_obj_t *new_screen) {
    // Global RTL hook: flip text direction on the finished screen's labels for
    // RTL locales (layout stays physical; user-input widgets stay LTR).
    if (seedsigner_locale_is_rtl()) {
        apply_rtl_text_to_labels(new_screen);
    }
    // Complex-script hook: for shaping locales (Devanagari/Thai/Nastaliq), replace
    // matched labels' codepoint text with their pre-shaped glyph runs. Runs after
    // RTL: a matched label's codepoint text is suppressed (text_opa TRANSP), so the
    // base_dir set above is moot for it and the visual-order run is never re-reordered.
    // Force layout first so each label's final content width is available — the
    // glyph-run word-wrap fits long lines to it.
    lv_obj_update_layout(new_screen);
    apply_glyph_runs_to_labels(new_screen);

    // Auto-wire the screensaver-opt-out teardown reset. A screen carrying
    // SS_OBJ_FLAG_NO_SCREENSAVER (stamped by apply_screensaver_policy from
    // allow_screensaver=false) can sit for minutes with NO user input — a QR held up to a
    // camera, a paper transcription, a long signing spinner — so by teardown LVGL's idle
    // clock is stale. Without this, the overlay dispatcher would fire the screensaver over
    // the freshly-loaded SUCCESSOR the instant this screen swaps out. Counting the teardown
    // as activity gives the successor a full idle window. Derived from the flag here, at the
    // one path every screen loads through, so every opt-out screen gets it automatically —
    // no per-screen call to remember. (The flag itself only suppresses the saver WHILE this
    // screen shows; this reset covers the gap right after it.)
    if (lv_obj_has_flag(new_screen, SS_OBJ_FLAG_NO_SCREENSAVER)) {
        lv_obj_add_event_cb(new_screen, reset_idle_clock_on_delete_cb, LV_EVENT_DELETE, NULL);
    }

    lv_obj_t *old_screen = lv_scr_act();
    lv_scr_load(new_screen);
    if (old_screen && old_screen != new_screen) {
        lv_obj_delete(old_screen);

        // The old screen is gone, so any script fonts a locale switch retired
        // (detached but kept alive precisely because this screen's labels still
        // pointed at them) are now unreferenced and safe to destroy. On a switch,
        // the new screen above was built with the freshly registered fonts, never
        // the retired ones. A no-op for plain same-locale navigation. This is the
        // single point where retired fonts are reclaimed — see
        // seedsigner_clear_registered_fonts() / ss_unload_locale() for why freeing
        // them any earlier would dangle the old screen's labels.
        ss_reap_retired();
    }
}


static nav_aux_policy_t nav_aux_policy_from_cfg(const json &cfg) {
    nav_aux_policy_t aux_policy = {NAV_AUX_ENTER, NAV_AUX_ENTER, NAV_AUX_ENTER};
    if (!(cfg.contains("input") && cfg["input"].is_object() && cfg["input"].contains("keys") && cfg["input"]["keys"].is_object())) {
        return aux_policy;
    }

    const auto &keys = cfg["input"]["keys"];
    auto parse_aux = [](const json &k, const char *name, nav_aux_action_t current) {
        if (!k.contains(name) || !k[name].is_string()) return current;
        std::string s = k[name].get<std::string>();
        if (s == "enter") return NAV_AUX_ENTER;
        if (s == "noop") return NAV_AUX_NOOP;
        if (s == "emit") return NAV_AUX_EMIT;
        return current;
    };

    aux_policy.key1 = parse_aux(keys, "key1", aux_policy.key1);
    aux_policy.key2 = parse_aux(keys, "key2", aux_policy.key2);
    aux_policy.key3 = parse_aux(keys, "key3", aux_policy.key3);
    return aux_policy;
}


// Shared nav wiring helper for all screens.
// Screens provide only focusables/layout/default body index; this helper applies
// top-nav wiring, aux-key policy, mode override, and binds nav in one place.
//
// Scroll-then-buttons joystick navigation (see navigation.h) is enabled
// AUTOMATICALLY here — no per-screen opt-in. The trigger is a vertical screen with
// non-focusable upper content (a separate, populated `upper_body`) above its
// buttons whose body overflows the viewport: e.g. an overflowing
// large_icon_status_screen, or a button_list_screen with intro text. A pure button
// list (upper_body == body) is excluded — it scrolls via item-focus navigation —
// as are grid layouts (main_menu) and screens that never call this helper
// (seed_add_passphrase, screensaver).
void bind_screen_navigation(const json &cfg,
                                   const screen_scaffold_t &screen,
                                   lv_obj_t **body_items,
                                   size_t body_item_count,
                                   nav_body_layout_t body_layout,
                                   size_t default_initial_index) {
    bool has_input_mode_override = false;
    input_mode_t input_mode_override = INPUT_MODE_TOUCH;
    nav_mode_override_from_cfg(cfg, has_input_mode_override, input_mode_override);

    // Auto-detect scroll-then-buttons: a vertical screen with a separate, populated
    // upper_body (non-focusable content above the buttons) whose body overflows the
    // viewport. lv_obj_update_layout forces geometry so lv_obj_get_scroll_bottom is
    // accurate; it runs only for screens with such upper content, so pure button
    // lists (upper_body == body) and grids pay nothing and stay byte-identical.
    lv_obj_t *scroll_obj = nullptr;
    bool scroll_then_buttons = false;
    if (body_layout == NAV_BODY_VERTICAL &&
        body_item_count > 0 &&
        screen.upper_body && screen.upper_body != screen.body &&
        lv_obj_get_child_cnt(screen.upper_body) > 0) {
        lv_obj_update_layout(screen.body);
        if (lv_obj_get_scroll_bottom(screen.body) > 0) {
            scroll_obj = screen.body;
            scroll_then_buttons = true;
        }
    }

    nav_config_t nav_cfg;
    nav_cfg.screen = screen.screen;
    nav_cfg.top_back_btn = screen.top_back_btn;
    nav_cfg.top_power_btn = screen.top_power_btn;
    nav_cfg.body_items = body_items;
    nav_cfg.body_item_count = body_item_count;
    nav_cfg.body_layout = body_layout;
    nav_cfg.aux_policy = nav_aux_policy_from_cfg(cfg);
    nav_cfg.initial_body_index = nav_initial_index_from_cfg(cfg, default_initial_index);
    nav_cfg.has_input_mode_override = has_input_mode_override;
    nav_cfg.input_mode_override = input_mode_override;
    nav_cfg.scroll_obj = scroll_obj;
    nav_cfg.scroll_then_buttons = scroll_then_buttons;
    nav_bind(&nav_cfg);
}


// Scaffold-buttons convenience: vertical body list discovered by the scaffold.
// Exactly the ritual every scaffold-built button screen performed inline — same
// callee, same argument values — so it is a pure textual factoring of the call.
// The NULL-when-empty ternary is kept verbatim for byte-level argument parity,
// even though nav_bind's own count guard already treats NULL-with-count-0 and
// valid-pointer-with-count-0 identically.
void bind_screen_navigation(const json &cfg,
                            const screen_scaffold_t &screen,
                            size_t default_initial_index) {
    // button_list is a fixed array member, so under this const reference it decays
    // to `lv_obj_t *const *`; the full overload takes `lv_obj_t **` (nav_bind only
    // READS the pointers — it copies them into its own array). const_cast bridges
    // the decay without changing any argument value.
    bind_screen_navigation(cfg, screen,
                           screen.button_list_count > 0
                               ? const_cast<lv_obj_t **>(screen.button_list)
                               : NULL,
                           screen.button_list_count,
                           NAV_BODY_VERTICAL,
                           default_initial_index);
}


// Build root screen: TopNav, body container, and (if cfg["button_list"] is
// present) a flex-column body that stacks `upper_body`, an optional
// flex-grow=1 spacer, and one button per `cfg["button_list"]` label.
//
// Usage patterns:
//
// 1. No button_list (e.g. `main_menu_screen`, `screensaver_screen`):
//    the body is the existing non-flex container, `upper_body == body`,
//    no scaffold-managed buttons.
//
// 2. button_list present, no upper content (is_bottom_list omitted/false
//    AND no `cfg["text"]`): the legacy pure-list path — top-aligned
//    `button()` chain in a plain body, `upper_body == body`. Kept
//    byte-identical to the original `button_list_screen`. Such a list
//    scrolls (when it overflows) via item-focus navigation, not a
//    page-scroll step.
//
// 3. button_list present WITH upper content — i.e. is_bottom_list=true
//    (status / confirmation screens) OR `cfg["text"]` provides intro text
//    above the buttons: a vertical-flex body with a SEPARATE `upper_body`
//    (LV_SIZE_CONTENT) followed by the buttons. `is_bottom_list` adds a
//    flex-grow=1 spacer between `upper_body` and the first button so the
//    buttons pin to the viewport bottom while content fits (collapsing on
//    overflow); intro-text-only lists omit the spacer so the buttons flow
//    directly under the text. When the body overflows, bind_screen_navigation
//    auto-enables scroll-then-buttons joystick navigation (see navigation.h).
//
// Screens always populate `scaffold.upper_body` (or `scaffold.body` in
// case #1, where they're the same object) and finish with
// `load_screen_and_cleanup_previous(scaffold.screen)`.
screen_scaffold_t create_top_nav_screen_scaffold(const json &cfg, bool scrollable, const lv_font_t *title_font) {
    screen_scaffold_t out = {0};

    out.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(out.screen, lv_color_hex(BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_radius(out.screen, 0, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(out.screen, BODY_LINE_SPACING, LV_PART_MAIN);
    lv_obj_set_style_pad_all(out.screen, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(out.screen, 0, LV_PART_MAIN);

    // Per-screen screensaver policy (view-owned, carried on the screen object): a view
    // that set allow_screensaver=false gets the saver opt-out stamped onto this root,
    // where the overlay dispatcher reads it off lv_scr_act(). Ordinary top-nav screens
    // default to allowed (default_allow=true); the teardown idle-clock reset is wired
    // automatically off the flag in load_screen_and_cleanup_previous().
    apply_screensaver_policy(out.screen, cfg);
    // NB: base direction is NOT set on the screen root. A root-level RTL base_dir
    // would also mirror element LAYOUT (flex order + lv_obj_set_pos / align honor
    // base_dir), flipping the Scan tile, the nav buttons, and the passphrase
    // textarea cursor. Instead, RTL text direction is applied to text LABELS only,
    // in one global post-pass — see load_screen_and_cleanup_previous().

    bool show_back = true;
    bool show_power = false;
    const auto &tn = cfg["top_nav"];
    if (tn.contains("show_back_button") && tn["show_back_button"].is_boolean()) {
        show_back = tn["show_back_button"].get<bool>();
    }
    if (tn.contains("show_power_button") && tn["show_power_button"].is_boolean()) {
        show_power = tn["show_power_button"].get<bool>();
    }
    std::string title = tn["title"].get<std::string>();

    // Optional contextual icon beside the title (Python top_nav_icon_name /
    // top_nav_icon_color) — e.g. SeedOptionsScreen's fingerprint. The glyph string is
    // passed through verbatim (same PUA codepoints on both sides); the color defaults
    // to the body font color when omitted.
    std::string top_nav_icon;
    uint32_t top_nav_icon_color = SEEDSIGNER_ICON_COLOR_DEFAULT;
    if (tn.contains("icon")) {
        if (!tn["icon"].is_string()) {
            throw std::runtime_error("top_nav.icon must be a string");
        }
        top_nav_icon = tn["icon"].get<std::string>();
    }
    if (tn.contains("icon_color")) {
        if (!tn["icon_color"].is_string()) {
            throw std::runtime_error("top_nav.icon_color must be a string");
        }
        top_nav_icon_color = parse_hex_color(tn["icon_color"].get<std::string>());
    }

    out.top_nav = top_nav(out.screen, title.c_str(), show_back, show_power,
                          &out.top_back_btn, &out.top_power_btn, title_font,
                          top_nav_icon.empty() ? nullptr : top_nav_icon.c_str(),
                          top_nav_icon_color, &out.title_label);
    out.body = create_standard_body_content(out.screen, out.top_nav, scrollable);

    // Decide which scaffold mode applies based on cfg.
    std::vector<button_item_cfg_t> button_items;
    bool has_button_list = read_button_list_items(cfg, button_items);
    bool is_bottom_list = false;
    if (cfg.contains("is_bottom_list")) {
        if (!cfg["is_bottom_list"].is_boolean()) {
            throw std::runtime_error("is_bottom_list must be a boolean");
        }
        is_bottom_list = cfg["is_bottom_list"].get<bool>();
    }

    // Python ButtonListScreen.is_button_text_centered (default True). false →
    // left-align every button's label (used by Seed Options, the tools menu, and
    // most non-main-menu list screens). Threaded into button_list()/button_ex below.
    bool is_button_text_centered = true;
    if (cfg.contains("is_button_text_centered")) {
        if (!cfg["is_button_text_centered"].is_boolean()) {
            throw std::runtime_error("is_button_text_centered must be a boolean");
        }
        is_button_text_centered = cfg["is_button_text_centered"].get<bool>();
    }

    // Button style, mirroring Python ButtonListScreen.Button_cls: "default" |
    // "checkbox" (multi-select) | "checked_selection" (single-select radio). The
    // checked indices come from cfg["checked_buttons"] (a list of ints), stamped onto
    // each item's is_checked below. Both are screen-wide; the style is one value for
    // the whole list, matching Python.
    button_style_t button_style = BUTTON_STYLE_DEFAULT;
    if (cfg.contains("button_style")) {
        if (!cfg["button_style"].is_string()) {
            throw std::runtime_error("button_style must be a string");
        }
        std::string s = cfg["button_style"].get<std::string>();
        if (s == "default") {
            button_style = BUTTON_STYLE_DEFAULT;
        } else if (s == "checkbox") {
            button_style = BUTTON_STYLE_CHECKBOX;
        } else if (s == "checked_selection") {
            button_style = BUTTON_STYLE_CHECKED_SELECTION;
        } else {
            throw std::runtime_error("button_style must be \"default\", \"checkbox\", or \"checked_selection\"");
        }
    }

    std::set<int> checked_buttons;
    if (cfg.contains("checked_buttons")) {
        if (!cfg["checked_buttons"].is_array()) {
            throw std::runtime_error("checked_buttons must be an array of integers");
        }
        for (const auto &idx : cfg["checked_buttons"]) {
            if (!idx.is_number_integer() || idx.get<int>() < 0) {
                throw std::runtime_error("checked_buttons entries must be non-negative integers");
            }
            checked_buttons.insert(idx.get<int>());
        }
    }

    // Stamp is_checked onto each item from checked_buttons (no-op for DEFAULT style).
    for (size_t i = 0; i < button_items.size(); ++i) {
        button_items[i].is_checked = checked_buttons.count((int)i) > 0;
    }

    if (!has_button_list) {
        // Mode 1: legacy. body == upper_body, no scaffold buttons.
        out.upper_body = out.body;
        return out;
    }

    if (button_items.size() > SEEDSIGNER_SCAFFOLD_MAX_BUTTONS) {
        throw std::runtime_error("button_list exceeds SEEDSIGNER_SCAFFOLD_MAX_BUTTONS");
    }

    // Does this button list carry intro text above the buttons? If so it needs a
    // separate `upper_body` (the flex path below) even when not bottom-pinned, so
    // the text can be read by scrolling and the joystick nav can hand off to the
    // buttons. (The screen renders the text into upper_body; the scaffold only
    // builds the structure.)
    bool has_intro_text = cfg.contains("text") && cfg["text"].is_string() &&
                          !cfg["text"].get<std::string>().empty();

    // Mode 2 (pure button list — no bottom pinning, no intro text): preserve
    // byte-identical rendering with the prior `button_list_screen` implementation
    // by using the existing `button_list()` helper (top-aligned, manual
    // chain-align). `upper_body` aliases `body`. Overflow is handled by item-focus
    // navigation (scroll_to_view), so this path gets NO page-scroll step.
    if (!is_bottom_list && !has_intro_text) {
        std::vector<button_list_item_t> items;
        items.reserve(button_items.size());
        for (const auto &it : button_items) {
            button_list_item_t item = {};
            item.label = it.label.c_str();
            item.value = NULL;
            item.icon = it.icon.empty() ? nullptr : it.icon.c_str();
            item.right_icon = it.right_icon.empty() ? nullptr : it.right_icon.c_str();
            item.icon_color = it.icon_color;
            item.label_color = it.label_color;
            item.is_checked = it.is_checked;
            items.push_back(item);
        }

        button_list(out.body, items.data(), items.size(), is_button_text_centered, button_style);

        // Discover the buttons that button_list() created so navigation can
        // reach them through the scaffold.
        uint32_t child_count = lv_obj_get_child_cnt(out.body);
        for (uint32_t i = 0; i < child_count && out.button_list_count < SEEDSIGNER_SCAFFOLD_MAX_BUTTONS; ++i) {
            lv_obj_t *child = lv_obj_get_child(out.body, i);
            if (child && lv_obj_check_type(child, &lv_button_class)) {
                out.button_list[out.button_list_count++] = child;
            }
        }

        out.upper_body = out.body;
        return out;
    }

    // Modes 3 & 4: button list WITH upper content. Switch body to a vertical flex
    // column with children:
    //   [0]   upper_body (LV_SIZE_CONTENT, owned by caller)
    //   [1]   flex-grow=1 spacer — ONLY when is_bottom_list (Mode 3); pins the
    //         buttons to the viewport bottom while content fits and collapses on
    //         overflow. Intro-text-only lists (Mode 4) omit it so the buttons flow
    //         directly under the text.
    //   [...] one button per cfg.button_list label
    //
    // Row-gap of LIST_ITEM_PADDING produces the same inter-button spacing as
    // the legacy `button_list()` helper.
    lv_obj_set_layout(out.body, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(out.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(out.body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(out.body, LIST_ITEM_PADDING, LV_PART_MAIN);

    out.upper_body = lv_obj_create(out.body);
    lv_obj_set_width(out.upper_body, lv_pct(100));
    lv_obj_set_height(out.upper_body, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(out.upper_body, lv_color_hex(BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(out.upper_body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(out.upper_body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(out.upper_body, 0, LV_PART_MAIN);
    lv_obj_remove_flag(out.upper_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(out.upper_body, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(out.upper_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(out.upper_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (is_bottom_list) {
        out.button_list_spacer = lv_obj_create(out.body);
        lv_obj_set_width(out.button_list_spacer, lv_pct(100));
        lv_obj_set_height(out.button_list_spacer, 0);
        lv_obj_set_flex_grow(out.button_list_spacer, 1);
        lv_obj_set_style_bg_opa(out.button_list_spacer, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(out.button_list_spacer, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(out.button_list_spacer, 0, LV_PART_MAIN);
        lv_obj_remove_flag(out.button_list_spacer, LV_OBJ_FLAG_SCROLLABLE);
    }

    // Shared leading-icon column = the widest leading icon on THIS screen, so the
    // left-aligned labels all begin at the same x (adapts to the icons in use).
    int32_t icon_column_w = 0;
    for (const auto &it : button_items) {
        int32_t w = inline_icon_width(it.icon.empty() ? nullptr : it.icon.c_str());
        if (w > icon_column_w) icon_column_w = w;
    }

    for (size_t i = 0; i < button_items.size(); ++i) {
        // align_to is unused under flex layout — flex positions the children — so
        // pass NULL. is_text_centered carries the screen's centering choice; the
        // per-item icon fields carry any inline/right icon + color.
        const button_item_cfg_t &it = button_items[i];
        button_opts_t opts = {};
        opts.text = it.label.c_str();
        opts.align_to = NULL;
        opts.is_text_centered = is_button_text_centered;
        opts.icon = it.icon.empty() ? nullptr : it.icon.c_str();
        opts.right_icon = it.right_icon.empty() ? nullptr : it.right_icon.c_str();
        opts.icon_color = it.icon_color;
        opts.label_color = it.label_color;
        opts.style = button_style;        // screen-wide checkbox/radio/default
        opts.is_checked = it.is_checked;  // per-item checked state
        opts.icon_column_w = icon_column_w;  // shared column so labels line up
        lv_obj_t *btn = button_ex(out.body, &opts);
        out.button_list[i] = btn;
        out.button_list_count = i + 1;
    }

    // Match the legacy `button_list()` default: highlight the only button
    // when there is exactly one; otherwise leave them all unselected.
    if (out.button_list_count == 1) {
        button_set_active(out.button_list[0], true);
    }

    return out;
}


// Pulsing colored border overlay for warning-class screens.
//
// Python paints five concentric borders per frame, ramping each border's
// brightness from 0 to full and back. We reproduce the perceptual "breathing"
// with a colored perimeter that pulses its opacity.
//
// The overlay sits on top of `screen` (not `body`), so it covers the TopNav
// too and does not scroll with content — matching Python's behavior.
//
// IMPLEMENTATION: four non-overlapping edge STRIPS (not one full-screen bordered
// object). This is a performance requirement, not a style choice. A single
// full-screen object with an animated border invalidates its WHOLE bounding box
// (the entire screen) on every pulse tick, which forces LVGL to re-render every
// interior widget in the dirty area each frame — including a direct-drawn QR whose
// DRAW_MAIN_END callback re-runs its full per-module loop. On a partial-refresh SPI
// panel (ST7796) that re-render happens once PER STRIPE (~4-8x/frame) and re-flushes
// the whole frame over SPI, collapsing the pulse to a few fps (measured ~5 fps on
// the whole-QR transcribe screen). Confining the pulse to thin edge strips means the
// per-tick dirty region is edges-only: the screen interior is never invalidated, its
// draw callbacks don't re-fire, and only the strips re-flush — so the pulse animates
// at the anim rate on both DSI and SPI panels. (On a DSI panel, where a full-screen
// flush is ~free, the old single-object version looked fine, hiding the cost.)
//
// The four strips form the perimeter ring: top/bottom span the full width (incl.
// corners); left/right span only the gap BETWEEN them — so corners are single-layer
// and pulse at a uniform opacity, matching the old uniform border. LVGL automatically
// frees each strip's animation when the strip is deleted, so no explicit teardown.
void add_warning_edges_overlay(lv_obj_t *screen, int status_color) {
    const int32_t edge   = EDGE_PADDING;
    const int32_t side_h = lv_display_get_vertical_resolution(NULL) - 2 * edge;

    struct edge_strip_t { lv_align_t align; int32_t w; int32_t h; };
    const edge_strip_t strips[] = {
        { LV_ALIGN_TOP_MID,    lv_pct(100), edge   },
        { LV_ALIGN_BOTTOM_MID, lv_pct(100), edge   },
        { LV_ALIGN_LEFT_MID,   edge,        side_h },
        { LV_ALIGN_RIGHT_MID,  edge,        side_h },
    };

    for (const edge_strip_t &st : strips) {
        lv_obj_t *strip = lv_obj_create(screen);
        lv_obj_remove_style_all(strip);   // bare filled rect: no theme border/pad/scrollbar
        lv_obj_set_size(strip, st.w, st.h);
        lv_obj_align(strip, st.align, 0, 0);
        lv_obj_remove_flag(strip, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_set_style_radius(strip, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(strip, lv_color_hex(status_color), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, LV_PART_MAIN);

        // Pulse: opacity 255 -> 0 -> 255. The edges REST at full color and breathe
        // OUT to fully off, then hold at full color before the next breath (mirrors
        // Python's WarningEdgesThread: rests bright, holds ~8 trough frames, ramps
        // OUT toward black). LVGL v9: `set_duration` is the forward leg (full -> off),
        // `set_reverse_duration` the back-to-start leg (off -> full), `set_repeat_delay`
        // the pause between iterations (held at full color). All four strips start in
        // the same call/tick with identical params, so they pulse in lockstep.
        lv_anim_t pulse;
        lv_anim_init(&pulse);
        lv_anim_set_var(&pulse, strip);
        lv_anim_set_values(&pulse, 255, 0);
        lv_anim_set_duration(&pulse, 500);
        lv_anim_set_reverse_duration(&pulse, 500);
        lv_anim_set_repeat_delay(&pulse, 400);
        lv_anim_set_repeat_count(&pulse, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&pulse, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&pulse, [](void *obj, int32_t v) {
            lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_MAIN);
        });
        lv_anim_start(&pulse);
    }
}


// See screen_scaffold.h: the bottom edge of the free band above the scaffold's
// bottom button. PRECONDITION: the caller has already run lv_obj_update_layout()
// on the screen, so button_list[0]'s coordinates are final.
int32_t bottom_button_top_y(const screen_scaffold_t &screen) {
    // Display-derived fallback: where a bottom-pinned button WOULD start when the
    // scaffold has no (valid) button.
    int32_t top_y = lv_display_get_vertical_resolution(NULL) - BUTTON_HEIGHT;

    if (screen.button_list_count > 0 && lv_obj_is_valid(screen.button_list[0])) {
        lv_area_t button_area;
        lv_obj_get_coords(screen.button_list[0], &button_area);
        top_y = button_area.y1;
    }

    return top_y;
}
