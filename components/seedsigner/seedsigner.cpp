#include "seedsigner.h"
#include "components.h"
#include "gui_constants.h"
#include "navigation.h"
#include "input_profile.h"
#include "font_registry.h"
#include "glyph_runs.h"
#include "locale_loader.h"   // ss_reap_retired() after the old screen is deleted
#include "overlay_manager.h" // SS_OBJ_FLAG_NO_SCREENSAVER (per-screen saver policy)

#include "lvgl.h"

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>
#include <cstring>
#include <set>

using json = nlohmann::json;

// The SeedSigner logo + HRF partner-logo images are declared (gated per display
// height) in gui_constants.h; pick the active-profile variant via
// seedsigner_logo_for_active_profile() / hrf_logo_for_active_profile().


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
static void parse_screen_json_ctx(const char *ctx_json, json &cfg_out) {
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

    // Per-screen screensaver policy: normalize to the single system default
    // (allowed). This is the ONLY place allow_screensaver's default is set — the
    // view owns the value, and every downstream consumer sees an explicit bool.
    cfg_out["allow_screensaver"] = cfg_out.value("allow_screensaver", true);
}

// Switch to a newly built LVGL screen and dispose of the old one.
//
// Rationale:
// - Every screen render path allocates a fresh root screen (`lv_obj_create(NULL)`).
// - If we do not delete the previous root, those widget trees remain allocated and
//   accumulate over repeated navigations/renders.
// - We only delete after `lv_scr_load(new_screen)` so LVGL has already switched the
//   active screen; this avoids deleting the currently active screen too early.
// - The `old_screen != new_screen` guard is a safety check for accidental reuse.
// Recursively force RTL text direction on every text LABEL in a finished screen
// tree (for RTL locales). This is the ONE global place direction is applied, so
// the screen/component builders stay direction-agnostic — no per-label or
// per-builder maintenance.
//
// Why labels only (not the screen root): LVGL's base_dir is a single inherited
// property that drives BOTH bidi text direction AND element layout (flex order,
// and the coordinate origin for lv_obj_set_pos / lv_obj_align). Setting it RTL at
// the root would mirror the layout too — the Scan tile, the nav buttons, the
// passphrase cursor all flip. Scoping it to lv_label objects gives correct RTL
// text while every container keeps its physical left-to-right arrangement.
//
// lv_textarea subtrees are skipped: the passphrase entry is always ASCII/LTR, so
// its box and cursor must stay left-to-right. The on-screen keyboard is a
// buttonmatrix (no child labels), so it is unaffected.
static void apply_rtl_text_to_labels(lv_obj_t *obj) {
    if (lv_obj_check_type(obj, &lv_textarea_class)) return;  // user input stays LTR
    if (lv_obj_check_type(obj, &lv_label_class)) {
        lv_obj_set_style_base_dir(obj, LV_BASE_DIR_RTL, 0);
    }
    uint32_t child_count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_count; ++i) {
        apply_rtl_text_to_labels(lv_obj_get_child(obj, i));
    }
}

static void load_screen_and_cleanup_previous(lv_obj_t *new_screen) {
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


// Build the standard "body" container used by screens beneath TopNav.
// Most screens share the same layout/styling shell (size, alignment, padding, background,
// border, and scrollbar baseline behavior). This function encapsulates that common
// boilerplate.
static lv_obj_t* create_standard_body_content(lv_obj_t *screen, lv_obj_t *top_nav_obj, bool scrollable) {
    lv_obj_t* body_content = lv_obj_create(screen);

    // The body sits directly below the full top_nav (TOP_NAV_HEIGHT tall) and clips
    // at TOP_NAV_HEIGHT. The top_nav vertically centers its buttons, so the space
    // between a button's bottom edge and TOP_NAV_HEIGHT is a visual buffer OWNED BY
    // THE TOP_NAV: scrolling body content clips at TOP_NAV_HEIGHT and never renders
    // up against the back/power buttons — the buffer stays between the nav and the
    // moving content. (An earlier revision pushed this buffer down into the body so
    // a screen could overlap an element up into the nav, but that let scrolled
    // content collide with the nav buttons; the buffer belongs to the top_nav.)
    lv_obj_set_size(body_content, lv_obj_get_width(screen),
                    lv_obj_get_height(screen) - TOP_NAV_HEIGHT);
    lv_obj_align_to(body_content, top_nav_obj, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(body_content, lv_color_hex(BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_pad_left(body_content, EDGE_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_right(body_content, EDGE_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_top(body_content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(body_content, COMPONENT_PADDING, LV_PART_MAIN);
    lv_obj_set_style_border_width(body_content, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(body_content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body_content, scrollable ? LV_SCROLLBAR_MODE_AUTO : LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_right(body_content, 0, LV_PART_SCROLLBAR);
    return body_content;
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

static void nav_mode_override_from_cfg(const json &cfg, bool &has_override, input_mode_t &mode_override) {
    has_override = false;
    mode_override = INPUT_MODE_TOUCH;

    if (!(cfg.contains("input") && cfg["input"].is_object() && cfg["input"].contains("mode") && cfg["input"]["mode"].is_string())) {
        return;
    }

    std::string mode = cfg["input"]["mode"].get<std::string>();
    if (mode == "touch") {
        has_override = true;
        mode_override = INPUT_MODE_TOUCH;
    } else if (mode == "hardware") {
        has_override = true;
        mode_override = INPUT_MODE_HARDWARE;
    }
}

static size_t nav_initial_index_from_cfg(const json &cfg, size_t default_index) {
    if (cfg.contains("initial_selected_index") && cfg["initial_selected_index"].is_number_integer()) {
        int idx = cfg["initial_selected_index"].get<int>();
        if (idx >= 0) {
            return (size_t)idx;
        }
    }
    return default_index;
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
static void bind_screen_navigation(const json &cfg,
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

// Read a button_list array from cfg into a vector of label strings.
// Accepts either bare strings or `[label, ...]` arrays whose first element is a
// string label (Python's ButtonOption tuple shape). Returns true if the key was
// present and well-formed; false (with `out` cleared) if the key is missing.
// Throws on a malformed entry so screens fail fast on bad JSON.
static bool read_button_list_labels(const json &cfg, std::vector<std::string> &out) {
    out.clear();
    if (!cfg.contains("button_list")) {
        return false;
    }
    if (!cfg["button_list"].is_array()) {
        throw std::runtime_error("button_list must be an array");
    }

    for (const auto &entry : cfg["button_list"]) {
        if (entry.is_string()) {
            out.push_back(entry.get<std::string>());
        } else if (entry.is_array() && !entry.empty() && entry[0].is_string()) {
            out.push_back(entry[0].get<std::string>());
        } else {
            throw std::runtime_error("button_list entries must be string or array with string label at index 0");
        }
    }
    return true;
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
static screen_scaffold_t create_top_nav_screen_scaffold(const json &cfg, bool scrollable, const lv_font_t *title_font = nullptr) {
    screen_scaffold_t out = {0};

    out.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(out.screen, lv_color_hex(BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_radius(out.screen, 0, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(out.screen, BODY_LINE_SPACING, LV_PART_MAIN);
    lv_obj_set_style_pad_all(out.screen, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(out.screen, 0, LV_PART_MAIN);

    // Per-screen screensaver policy (view-owned, carried on the screen object):
    // a view that set allow_screensaver=false gets the saver opt-out stamped onto
    // this root, where the overlay dispatcher reads it off lv_scr_act(). Absent/
    // true leaves the flag off = saver allowed. parse_screen_json_ctx normalizes
    // the key, so this is the one shared stamp for every top-nav screen.
    if (!cfg.value("allow_screensaver", true)) {
        lv_obj_add_flag(out.screen, SS_OBJ_FLAG_NO_SCREENSAVER);
    }
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

    out.top_nav = top_nav(out.screen, title.c_str(), show_back, show_power, &out.top_back_btn, &out.top_power_btn, title_font);
    out.body = create_standard_body_content(out.screen, out.top_nav, scrollable);

    // Decide which scaffold mode applies based on cfg.
    std::vector<std::string> button_labels;
    bool has_button_list = read_button_list_labels(cfg, button_labels);
    bool is_bottom_list = false;
    if (cfg.contains("is_bottom_list")) {
        if (!cfg["is_bottom_list"].is_boolean()) {
            throw std::runtime_error("is_bottom_list must be a boolean");
        }
        is_bottom_list = cfg["is_bottom_list"].get<bool>();
    }

    if (!has_button_list) {
        // Mode 1: legacy. body == upper_body, no scaffold buttons.
        out.upper_body = out.body;
        return out;
    }

    if (button_labels.size() > SEEDSIGNER_SCAFFOLD_MAX_BUTTONS) {
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
        items.reserve(button_labels.size());
        for (const auto &label : button_labels) {
            button_list_item_t item = { .label = label.c_str(), .value = NULL };
            items.push_back(item);
        }

        button_list(out.body, items.data(), items.size());

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

    for (size_t i = 0; i < button_labels.size(); ++i) {
        // `button()`'s second arg is unused under flex layout — flex
        // overrides any align_to. Pass NULL for clarity.
        lv_obj_t *btn = button(out.body, button_labels[i].c_str(), NULL);
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


// Create a standard wrapped body-text label in `parent`: WRAP, fixed `width`,
// centered, BODY_FONT in BODY_FONT_COLOR. Shared by the button_list_screen intro
// text and the status-screen body (which then layers on its inset width, tight
// line spacing, and centering). Caller owns any further styling.
static lv_obj_t *make_body_text_label(lv_obj_t *parent, const char *text, int32_t width) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &BODY_FONT, LV_PART_MAIN);
    return label;
}


// Render a screen whose body is a vertical list of buttons, optionally preceded
// by an intro-text block (`cfg["text"]`).
//
// The scaffold builds the buttons from `cfg["button_list"]`; when `cfg["text"]`
// is present it also gives us a separate `upper_body` (above the buttons) for the
// text. This function validates the required key, renders any intro text, wires
// navigation, and loads the screen. When intro text overflows the viewport,
// bind_screen_navigation auto-enables scroll-then-buttons joystick navigation:
// the text scrolls into view before the first button takes focus.
void button_list_screen(void *ctx_json)
{
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array()) {
        throw std::runtime_error("button_list is required and must be an array");
    }

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, true);

    // Optional intro text above the buttons. The scaffold gave us a separate
    // upper_body (Mode 4) whenever cfg["text"] is a non-empty string; render the
    // text into it. Wraps to the upper_body content width and uses the standard
    // body font/color; line spacing is the screen's inherited default.
    if (cfg.contains("text") && cfg["text"].is_string()) {
        std::string text = cfg["text"].get<std::string>();
        if (!text.empty() && screen.upper_body && screen.upper_body != screen.body) {
            make_body_text_label(screen.upper_body, text.c_str(),
                                 lv_obj_get_content_width(screen.upper_body));
        }
    }

    bind_screen_navigation(
        cfg,
        screen,
        screen.button_list_count > 0 ? screen.button_list : NULL,
        screen.button_list_count,
        NAV_BODY_VERTICAL,
        (size_t)-1
    );

    load_screen_and_cleanup_previous(screen.screen);
}


// ---------------------------------------------------------------------------
// large_icon_status_screen
// ---------------------------------------------------------------------------
//
// Ports Python's `LargeIconStatusScreen` family — Success / Warning /
// DireWarning / Error — to LVGL. The Python class hierarchy collapses into a
// single function with a `status_type` enum.
//
// Layout (built on `create_top_nav_screen_scaffold` with cfg.button_list +
// cfg.is_bottom_list = true):
//
//   ┌────────────────────────────────┐
//   │ TopNav (title; optional back)  │
//   ├────────────────────────────────┤
//   │  [hero icon, status_color]     │
//   │  [headline, status_color]      │
//   │  [body text, body color]       │
//   │  ┄ ┄ ┄ flex spacer ┄ ┄ ┄ ┄ ┄  │
//   │  [ OK / I understand ]         │
//   └────────────────────────────────┘
//
// When `cfg.warning_edges` is true a pulsing colored border is rendered on top
// of the screen (see `add_warning_edges_overlay`).
//
// `large_icon_status_screen` itself only adds the icon, headline, and body
// text into `scaffold.upper_body`; the scaffold owns the spacer and the
// button.

enum class status_type_t { SUCCESS, WARNING, DIRE_WARNING, ERROR };

// Per-status defaults. Python keeps these as class attributes; here we keep
// them in one place so JSON-driven configuration can override any field.
struct status_type_defaults_t {
    const char *icon;
    int color;
    const char *default_top_nav_title;
    const char *default_button_label;
    bool warning_edges_default;
    int text_edge_padding_multiplier;  // 1 for success, 2 for warning variants
};

static status_type_defaults_t defaults_for_status_type(status_type_t st) {
    switch (st) {
        case status_type_t::SUCCESS:
            return { SeedSignerIconConstants::SUCCESS, SUCCESS_COLOR,
                     "Success!",   "OK",            false, 1 };
        case status_type_t::WARNING:
            return { SeedSignerIconConstants::WARNING, WARNING_COLOR,
                     "Caution",    "I understand",  true,  2 };
        case status_type_t::DIRE_WARNING:
            return { SeedSignerIconConstants::WARNING, DIRE_WARNING_COLOR,
                     "Caution",    "I understand",  true,  2 };
        case status_type_t::ERROR:
            return { SeedSignerIconConstants::ERROR,   ERROR_COLOR,
                     "Error",      "I understand",  true,  2 };
    }
    // Unreachable; silences -Wreturn-type on some compilers.
    return { SeedSignerIconConstants::SUCCESS, SUCCESS_COLOR, "", "", false, 1 };
}

static status_type_t parse_status_type(const json &cfg) {
    if (!cfg.contains("status_type") || !cfg["status_type"].is_string()) {
        throw std::runtime_error("status_type is required and must be one of "
                                 "\"success\", \"warning\", \"dire_warning\", \"error\"");
    }
    std::string value = cfg["status_type"].get<std::string>();
    if (value == "success")      return status_type_t::SUCCESS;
    if (value == "warning")      return status_type_t::WARNING;
    if (value == "dire_warning") return status_type_t::DIRE_WARNING;
    if (value == "error")        return status_type_t::ERROR;
    throw std::runtime_error("status_type must be one of \"success\", "
                             "\"warning\", \"dire_warning\", \"error\"");
}

// Inject status-type defaults into cfg before the scaffold reads it.
//
// Mirrors the Python class hierarchy: `top_nav.title`, `button_list`, and
// `is_bottom_list` get sensible per-status defaults if the JSON omits them.
// `is_bottom_list` is forced to true for status screens, matching
// LargeIconStatusScreen.__post_init__.
static void apply_status_type_defaults(json &cfg, const status_type_defaults_t &defaults) {
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) {
        cfg["top_nav"] = json::object();
    }
    if (!cfg["top_nav"].contains("title")) {
        cfg["top_nav"]["title"] = defaults.default_top_nav_title;
    }
    if (!cfg.contains("button_list")) {
        cfg["button_list"] = json::array({ defaults.default_button_label });
    }

    // LargeIconStatusScreen always pins the button to the bottom.
    cfg["is_bottom_list"] = true;
}

// Pulsing colored border overlay for warning-class screens.
//
// Python paints five concentric borders per frame, ramping each border's
// brightness from 0 to full and back. LVGL gives us a single border-style
// with a width and an opacity, which captures the perceptual "breathing"
// effect with one widget and one animation.
//
// The overlay sits on top of `screen` (not `body`), so it covers the TopNav
// too and does not scroll with content — matching Python's behavior.
//
// LVGL automatically frees the animation when its target object is deleted,
// so no explicit teardown is required.
static void add_warning_edges_overlay(lv_obj_t *screen, int status_color) {
    lv_obj_t *edge = lv_obj_create(screen);
    lv_obj_set_size(edge, lv_pct(100), lv_pct(100));
    lv_obj_align(edge, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(edge, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    lv_obj_set_style_bg_opa(edge, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(edge, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(edge, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(edge, 0, LV_PART_MAIN);

    lv_obj_set_style_border_color(edge, lv_color_hex(status_color), LV_PART_MAIN);
    lv_obj_set_style_border_width(edge, EDGE_PADDING, LV_PART_MAIN);
    lv_obj_set_style_border_side(edge, LV_BORDER_SIDE_FULL, LV_PART_MAIN);

    // Pulse: opacity 255 -> 0 -> 255. The edges REST at full color and breathe
    // OUT to fully off, then hold at full color before the next breath. This
    // mirrors Python's WarningEdgesThread, which rests at inhale_factor=0 (full
    // color), holds there for 8 frames, and ramps the brightness OUT toward black
    // — so the resting/held state is bright, and the trough is (near) off. (The
    // earlier 64->255 inverted this: it rested dim and never reached full off.)
    //
    // LVGL v9 names: `set_duration` is the forward leg (full -> off),
    // `set_reverse_duration` the back-to-start leg (off -> full), and
    // `set_repeat_delay` the pause between iterations — held at `start` (full
    // color), since each iteration ends back at `start`. Python's ~10fps hold of
    // 8 trough frames (~800 ms) maps to a 400 ms repeat delay here.
    lv_anim_t pulse;
    lv_anim_init(&pulse);
    lv_anim_set_var(&pulse, edge);
    lv_anim_set_values(&pulse, 255, 0);
    lv_anim_set_duration(&pulse, 500);
    lv_anim_set_reverse_duration(&pulse, 500);
    lv_anim_set_repeat_delay(&pulse, 400);
    lv_anim_set_repeat_count(&pulse, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&pulse, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&pulse, [](void *obj, int32_t v) {
        lv_obj_set_style_border_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_MAIN);
    });
    lv_anim_start(&pulse);
}

// ---------------------------------------------------------------------------
// Tight, ink-based inter-line spacing.
//
// Walk `text` (UTF-8) and return the maximum ink ascent and descent — the tallest
// distance above and the deepest below the baseline — over every visible glyph in
// `font`. Whitespace and absent/inkless glyphs are skipped. Both tight_line_space
// and text_top_leading derive their metrics from this single pass (text_top_leading
// uses only the ascent). Either out-param may be null.
static void measure_text_ink_extents(const lv_font_t *font, const char *text,
                                     int32_t *out_max_ascent, int32_t *out_max_descent) {
    int32_t max_ascent  = 0;   // ink above the baseline
    int32_t max_descent = 0;   // ink below the baseline

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

        if (cp == '\n' || cp == '\r' || cp == ' ') {
            continue;
        }

        lv_font_glyph_dsc_t d;
        if (!lv_font_get_glyph_dsc(font, &d, cp, 0) || d.box_h == 0) {
            continue;   // absent or inkless (whitespace)
        }

        int32_t ascent  = (int32_t)d.ofs_y + (int32_t)d.box_h;  // top, above baseline
        int32_t descent = -(int32_t)d.ofs_y;                    // bottom, below baseline
        if (ascent  > max_ascent)  max_ascent  = ascent;
        if (descent > max_descent) max_descent = descent;
    }

    if (out_max_ascent)  *out_max_ascent  = max_ascent;
    if (out_max_descent) *out_max_descent = max_descent;
}

// LVGL's declared font line_height carries loose leading and varies wildly
// across scripts (Arabic/Farsi fonts reserve large vertical space for stacked
// marks), so using it as the line advance leaves multi-line body text far
// looser than the PIL/Python reference. Instead we derive the advance from the
// ACTUAL ink extents of the glyphs present in `text` -- the tallest ascender
// plus the deepest descender -- and add only a small visual `gap`. The result
// is the value to hand lv_obj_set_style_text_line_space() (the rendered advance
// is font line_height + this), so it is usually NEGATIVE (tightening).
//
// Worst-case safe for a run of text: the maximum ascender (which may land on a
// lower line) and the maximum descender (which may land on the line above it)
// are kept at least `gap` px apart. Complex cursive scripts with a cascading
// baseline (Urdu Nastaliq) are not well served by a single constant advance and
// render through their own (shaped) path, not this one.
static int32_t tight_line_space(const lv_font_t *font, const char *text, int32_t gap) {
    if (!font || !text) {
        return 0;
    }

    int32_t max_ascent = 0, max_descent = 0;
    measure_text_ink_extents(font, text, &max_ascent, &max_descent);

    if (max_ascent + max_descent <= 0) {
        return 0;   // measured nothing; leave the label's default spacing alone
    }

    int32_t line_h  = (int32_t)lv_font_get_line_height(font);
    int32_t advance = max_ascent + max_descent + gap;
    int32_t space   = advance - line_h;

    // Safety floor on how far we tighten. Per-codepoint measurement is exact for
    // simple LTR scripts but can UNDER-count scripts whose on-screen glyphs differ
    // from their source codepoints (Arabic/Farsi presentation-form shaping):
    // there the measured ink is far too small and an unclamped advance would
    // collapse the lines on top of each other. Never remove more than a quarter
    // of the font's declared line_height — enough to strip loose leading on
    // well-measured fonts, but a guard against collapse on the rest.
    // NOTE: callers in large_icon_status_screen now pass lv_label_get_text() (the
    // AP-processed presentation forms actually drawn), so fa measures correctly and
    // no longer hits this floor — it is now insurance against future divergence,
    // not the active correction it once was for fa.
    int32_t min_space = -(line_h / 4);
    if (space < min_space) {
        space = min_space;
    }
    return space;
}

// Empty vertical space between a label's box top and the VISIBLE top of its text
// — the font's ascent minus the text's real ink ascent. LVGL anchors text by the
// font's ascent (which includes leading above the caps); PIL/Python anchors the
// visible glyph top. Subtract this from a top margin so the visible text lands
// where Python places it, instead of the label's (taller) box.
int32_t text_top_leading(const lv_font_t *font, const char *text) {
    if (!font || !text) {
        return 0;
    }
    int32_t max_ascent = 0;
    measure_text_ink_extents(font, text, &max_ascent, nullptr);
    if (max_ascent <= 0) {
        return 0;
    }
    int32_t ascent  = (int32_t)lv_font_get_line_height(font) - (int32_t)font->base_line;
    int32_t leading = ascent - max_ascent;
    return leading > 0 ? leading : 0;
}

// Balanced wrap for a codepoint-rendered (subset/Latin) wrapped label: shrink its
// column to the SMALLEST width that still produces the same number of lines —
// floored at half the original width — so greedy wrapping fills the lines more
// evenly and a lone trailing word gets pulled up. Width-only: the line count, and
// therefore the label's height, is unchanged, so this composes with the vertical
// centering / reclaim done by the caller. The search is pure metric arithmetic
// over already-loaded glyph advances (lv_text_get_size: no rasterization, no
// re-shaping), run once at screen build — negligible cost. Shaped glyph-run
// locales (hi/th) wrap in the render layer (glyph_runs.cpp) and are balanced
// there instead; this path must not touch them (its codepoint measurement would
// not match their shaped line-breaking).
static void balance_wrapped_label_column(lv_obj_t *label) {
    if (!label) return;
    const char *text = lv_label_get_text(label);
    if (!text || !text[0]) return;

    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    int32_t letter_space  = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
    int32_t line_space    = lv_obj_get_style_text_line_space(label, LV_PART_MAIN);
    int32_t w0            = lv_obj_get_width(label);
    if (!font || w0 < 2) return;

    // Reuse the shared line-count binary search (balanced_wrap_width, glyph_runs.h) —
    // the shaped path feeds it glyph-run advances; here the per-trial measure is
    // lv_text_get_size over the codepoint text. LVGL stacks N wrapped lines to height
    // N*(line_h + line_space) - line_space, so the visual line count is exactly
    // (sz.y + line_space) / (line_h + line_space) — the same quantity the old height
    // comparison used, expressed as the count the shared search expects.
    const int32_t line_h = (int32_t)lv_font_get_line_height(font);
    int best = balanced_wrap_width((int)w0, [&](int w, size_t *nlines, int *max_line_w) {
        lv_point_t sz;
        lv_text_get_size(&sz, text, font, letter_space, line_space, w, LV_TEXT_FLAG_NONE);
        *nlines     = (size_t)((sz.y + line_space) / (line_h + line_space));
        *max_line_w = (int)sz.x;
    });
    if (best < (int)w0) lv_obj_set_width(label, (int32_t)best);
}

void large_icon_status_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    status_type_t status = parse_status_type(cfg);
    status_type_defaults_t defaults = defaults_for_status_type(status);
    apply_status_type_defaults(cfg, defaults);

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, true);

    // Status screens use the full screen width for icon / headline / body text
    // (Python's LargeIconStatusScreen positions these on the canvas without a
    // margin). Zero out the body's horizontal padding so upper_body spans
    // the full screen width; re-apply the matching margin to the scaffold's
    // bottom-list buttons so they keep their EDGE_PADDING side gutters.
    lv_obj_set_style_pad_left(screen.body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_right(screen.body, 0, LV_PART_MAIN);
    for (size_t i = 0; i < screen.button_list_count; ++i) {
        lv_obj_set_width(screen.button_list[i],
                         lv_obj_get_width(screen.body) - 2 * EDGE_PADDING);
    }

    // Most status screens fit the viewport and never scroll. But a tall body
    // (long warning text on a small display) can push the bottom button off-screen,
    // so KEEP the body scrollable (the scaffold already set scroll_dir=VER +
    // SCROLLBAR_MODE_AUTO): touch gets native drag-to-scroll, and bind_screen_
    // navigation auto-detects overflow to opt the joystick nav into scroll-then-
    // button stepping. When content fits, scroll_bottom == 0 — the scrollbar stays
    // hidden and the flow is the plain one.

    // Zero upper_body's flex row-gap so the icon->headline spacing is ONLY the
    // headline's COMPONENT_PADDING/2 top margin (matching Python's
    // next_y = icon_bottom + COMPONENT_PADDING/2); any inherited row-gap inflates it.
    lv_obj_set_style_pad_row(screen.upper_body, 0, LV_PART_MAIN);

    // Hero icon — colored, centered, sized from the active display profile.
    // upper_body's flex layout (column, cross-axis center) handles centering. The
    // icon needs no margin: the body's default pad_top is 0, so the icon renders at
    // the first available body pixel — just below the top_nav's bottom buffer — and
    // the glyph fits its label box exactly (box_h == line_height), so it renders
    // whole (no clip). This sits ~COMPONENT_PADDING/2 below Python's anchor
    // (top_nav.height - COMPONENT_PADDING/2); the trade keeps the top_nav buffer
    // visible while the body scrolls beneath it.
    lv_obj_t *icon = lv_label_create(screen.upper_body);
    lv_label_set_text(icon, defaults.icon);
    lv_obj_set_style_text_font(icon, &ICON_PRIMARY_SCREEN_FONT__SEEDSIGNER, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, lv_color_hex(defaults.color), LV_PART_MAIN);
    lv_obj_set_style_margin_top(icon, 0, LV_PART_MAIN);

    // Strip default label padding so the box matches the font's line_height
    // (cap-height; the icon font has base_line=0), so the icon's bottom edge
    // anchors the headline spacing exactly where Python places it.
    lv_obj_set_style_pad_all(icon, 0, LV_PART_MAIN);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // upper_body now spans the full screen width (body padding zeroed above).
    const int32_t upper_body_content_width = lv_obj_get_width(screen.screen);

    // Horizontal inset for the readable text column: the body text, and an
    // overflowing headline when it scrolls, both sit inside this gutter so they
    // share one left/right edge and clear the pulsing warning border (warning
    // variants double it; success uses a single EDGE_PADDING). The hero icon and
    // a centered (fitting) headline still span the full width, matching Python.
    const int32_t text_inset = defaults.text_edge_padding_multiplier * EDGE_PADDING;

    // Optional headline — colored to match status. Single-line (never wraps),
    // matching Python's auto_line_break=False. A headline that FITS centers on the
    // full width and is byte-identical to before (LONG_DOT). A headline that
    // OVERFLOWS start-justifies (LTR=left; shaped RTL via the glyph-run draw) and
    // auto-scrolls within the text column to reveal the tail — a NEW capability
    // (Python truncates with an ellipsis; we scroll instead). Overflow is rare
    // (the fit test is against the full width) but long localized strings hit it.
    if (cfg.contains("status_headline") && cfg["status_headline"].is_string()) {
        std::string headline = cfg["status_headline"].get<std::string>();
        if (!headline.empty()) {
            lv_obj_t *headline_label = lv_label_create(screen.upper_body);
            lv_label_set_text(headline_label, headline.c_str());
            lv_obj_set_style_text_color(headline_label, lv_color_hex(defaults.color), LV_PART_MAIN);
            lv_obj_set_style_text_font(headline_label, &BODY_FONT, LV_PART_MAIN);

            // Measure the rendered width like top_nav()/A11 do: the label's STORED
            // (presentation-form) text, so the overflow test matches the painted
            // glyphs (see label_subset_text_width). Single-line headline; shaped
            // hi/th still mis-measure here (codepoint width) — a known low-impact gap.
            if (label_subset_text_width(headline_label, &BODY_FONT) > upper_body_content_width &&
                !seedsigner_locale_is_rtl()) {
                // LTR overflow: clamp the label to the text column (so it scrolls
                // within the same gutters as the body, never up to the screen edge
                // or under the warning border), then start-justify (LEFT) +
                // continuous marquee with the initial/per-wrap hold + true 40 px/sec
                // (shared with the top_nav title). The upper_body flex centers the
                // narrower label, yielding equal `text_inset` gutters. Shaped hi/th
                // ride Task 0's offset-aware glyph-run draw. RTL (ur) is gated out
                // here to match Task 0 / glyph_run_draw_cb (the offset, scroll
                // start-justify, and content-box clip are all LTR-only for now); an
                // overflowing ur headline keeps the legacy LONG_DOT path below, so ur
                // stays byte-identical and its RTL scroll lands with the ur RTL track.
                int32_t scroll_width = upper_body_content_width - 2 * text_inset;
                if (scroll_width < 16) {
                    scroll_width = 16;
                }
                lv_obj_set_width(headline_label, scroll_width);
                lv_obj_set_style_text_align(headline_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
                label_set_line_autoscroll(headline_label, LINE_SCROLL_BEGIN_HOLD_MS, LINE_SCROLL_BEGIN_HOLD_MS);
            } else {
                // Fits (any locale) or RTL overflow: centered on the full width +
                // ellipsis-capable, exactly as before (byte-identical).
                lv_obj_set_width(headline_label, upper_body_content_width);
                lv_label_set_long_mode(headline_label, LV_LABEL_LONG_DOT);
                lv_obj_set_style_text_align(headline_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            }
            // Python's gap is CP/2 between the icon's visible bottom and the
            // headline's VISIBLE top. The label adds top leading above the caps
            // that PIL does not, so subtract it — otherwise the gap reads ~2x too
            // big and cascades down, jamming the bottom button against the edge.
            // Measure the label's STORED text, not the logical `headline`: with
            // LV_USE_ARABIC_PERSIAN_CHARS, lv_label_set_text rewrote Arabic/Persian
            // into presentation forms (the glyphs actually drawn, and the only ones
            // present in the subset font). Measuring the logical codepoints
            // under-counts the ink → an over-large leading → the fa headline pulls
            // UP into the hero icon (the A11 collision). en is unaffected (AP is a
            // no-op there, so the stored text == the logical text).
            int32_t hl_lead = text_top_leading(&BODY_FONT, lv_label_get_text(headline_label));
            lv_obj_set_style_margin_top(headline_label, COMPONENT_PADDING / 2 - hl_lead, LV_PART_MAIN);
        }
    }

    // Body text — wraps inside the upper_body width minus the
    // status-type-appropriate edge inset. Warning-class screens use
    // 2 * EDGE_PADDING so text never sits under the pulsing border.
    // Hoisted to function scope so the fits-case vertical centering below can
    // reference it after the full layout settles.
    lv_obj_t *body_label = nullptr;
    if (cfg.contains("text") && cfg["text"].is_string()) {
        std::string text = cfg["text"].get<std::string>();
        if (!text.empty()) {
            // Inset the body text by `text_inset` on each side (computed above,
            // shared with the headline's scroll column) so it never sits under the
            // pulsing warning border.
            int32_t text_width = upper_body_content_width - 2 * text_inset;
            if (text_width < 16) {
                text_width = 16;
            }

            body_label = make_body_text_label(screen.upper_body, text.c_str(), text_width);

            // Python places the body immediately after the headline
            // (body_top = headline_bottom; no extra gap) — so NO top margin here.
            // A CP/2 margin (as before) made the headline->body gap visibly looser
            // than the Python reference.
            lv_obj_set_style_margin_top(body_label, 0, LV_PART_MAIN);

            // Tight, ink-based inter-line spacing (see tight_line_space): derive
            // the line advance from THIS text's real glyph ink extents (max
            // ascender + max descender) plus a tiny gap, rather than the font's
            // loose declared line_height. Matches the PIL reference and tames
            // scripts whose font metrics over-reserve vertical space (Farsi).
            // The gap is intentionally small and profile-scaled.
            // As with the headline above, measure the label's STORED text so the
            // Arabic/Persian presentation forms (what's actually drawn) are what we
            // measure — measuring the logical codepoints under-counts the ink and
            // pins fa to NotoSansAR's over-reserved declared line_height (too loose).
            int32_t line_gap = LIST_ITEM_PADDING / 2;
            lv_obj_set_style_text_line_space(
                body_label,
                tight_line_space(&BODY_FONT, lv_label_get_text(body_label), line_gap),
                LV_PART_MAIN);
        }
    }

    // Optional pulsing border. Default per status_type (true for warning /
    // dire_warning / error), explicitly overridable from JSON.
    bool warning_edges = defaults.warning_edges_default;
    if (cfg.contains("warning_edges")) {
        if (!cfg["warning_edges"].is_boolean()) {
            throw std::runtime_error("warning_edges must be a boolean");
        }
        warning_edges = cfg["warning_edges"].get<bool>();
    }
    if (warning_edges) {
        add_warning_edges_overlay(screen.screen, defaults.color);
    }

    // A13/Item1: for shaped (glyph-run) locales the body's mask is drawn TALLER than
    // the lv_label widget box — the run lays out at the font's full line_height,
    // while the codepoint box is sized at the tighter tight_line_space advance. Bake
    // the runs NOW (normally deferred to the post-load pass) and GROW the body label
    // to the run's true drawn height, so the codepoint box no longer under-reports
    // the painted extent. Every lv_obj_get_scroll_bottom() decision below — reclaim,
    // the fits gate, the centering, and bind_screen_navigation's scroll auto-detect —
    // then measures the real height on ONE code path, so a tall shaped body
    // reclaims/scrolls instead of clipping at the bottom with no scroll path. Mirror
    // the post-load order (RTL flip, then attach); the post-load pass re-runs both as
    // no-ops (RTL idempotent; attach skips already-attached labels). No-op for
    // en/subset (apply_glyph_runs_to_labels self-gates on the active locale; run
    // height returns -1 so no min-height is set).
    //
    // NOTE: this fix is specific to the status body, which sets a TIGHT line_space
    // (tight_line_space, below) so its codepoint box is SHORTER than the run. Plain
    // body labels (make_body_text_label, e.g. button_list_screen's intro text) keep
    // the screen's generous default BODY_LINE_SPACING, so their codepoint box already
    // meets/exceeds the run height and needs no such fix.
    if (seedsigner_locale_uses_glyph_runs()) {
        lv_obj_update_layout(screen.body);
        if (seedsigner_locale_is_rtl()) {
            apply_rtl_text_to_labels(screen.screen);
        }
        apply_glyph_runs_to_labels(screen.screen);
        if (body_label) {
            // run height is content-relative (painted from the content top), so size
            // the box to run_h PLUS the label's vertical padding to keep the content
            // area >= the painted run regardless of any theme padding.
            int32_t run_h = seedsigner_label_run_drawn_height(body_label);
            if (run_h >= 0) {
                int32_t pad_v = lv_obj_get_style_pad_top(body_label, LV_PART_MAIN) +
                                lv_obj_get_style_pad_bottom(body_label, LV_PART_MAIN);
                lv_obj_set_style_min_height(body_label, run_h + pad_v, LV_PART_MAIN);
            }
        }
    }

    // Reclaim-only-as-needed: if the content overflows by no more than the top_nav's
    // bottom buffer (tn_gap), pull the whole body up by exactly that overflow so it
    // FITS without scrolling — the icon/headline/text rise a few px into the buffer
    // region while the bottom button and its padding stay put. The body keeps its
    // bottom edge, so only the top is reclaimed. Because we pull up by the exact
    // overflow, the result fits (scroll_bottom -> 0) and never scrolls, so scrolled
    // content can never collide with the nav. A larger overflow (> tn_gap can't be
    // hidden in the buffer) is left alone: the icon stays at the default position and
    // the screen scrolls cleanly under the full buffer (bind_screen_navigation then
    // enables scroll-then-buttons). The two cases are mutually exclusive.
    lv_obj_update_layout(screen.body);
    int32_t overflow = lv_obj_get_scroll_bottom(screen.body);
    int32_t tn_gap = (TOP_NAV_HEIGHT - TOP_NAV_BUTTON_SIZE) / 2;
    if (overflow > 0 && overflow <= tn_gap) {
        lv_obj_set_height(screen.body, lv_obj_get_height(screen.body) + overflow);
        lv_obj_align_to(screen.body, screen.top_nav, LV_ALIGN_OUT_BOTTOM_MID, 0, -overflow);
    }

    // Balanced wrap: when the body fits, even out the body text's lines by
    // narrowing its column (keeping the line count). Only the codepoint-rendered
    // (subset/Latin) locales are handled here; shaped glyph-run locales (hi/th)
    // wrap in the render layer and are balanced there. Done before the vertical
    // centering below; it changes only the column width, not the height, so the
    // centering math is unaffected.
    //
    // KNOWN ASYMMETRY (intentional): this subset/Latin balancing is SCOPED to this
    // screen (called only here), while the shaped balancing in glyph_runs.cpp is
    // GLOBAL (every wrapped shaped label, app-wide). So on a NON-status screen with
    // wrapped body text (e.g. a button_list with intro text), shaped locales get
    // balanced but subset/Latin ones do not. The status screen — the original
    // motivation — is covered for all locales. If full cross-locale parity on other
    // screens is ever needed, promote this to a global post-load pass (walk the
    // tree, balance LONG_WRAP non-shaped labels) mirroring apply_glyph_runs_to_labels.
    lv_obj_update_layout(screen.body);
    bool body_fits = lv_obj_get_scroll_bottom(screen.body) == 0;
    if (body_fits && body_label && !seedsigner_locale_uses_glyph_runs()) {
        balance_wrapped_label_column(body_label);
    }

    // Vertically center the body text in the gap between the headline and the
    // bottom button when the content FITS with slack to spare. By default the body
    // text sits directly under the headline (gap 0) and the whole flex-grow spacer
    // sits BELOW it, hard against the button — so the text reads as top-biased.
    // Moving the text down by HALF the below-gap puts equal space above and below
    // it; the spacer (which the shift comes out of) keeps the button pinned, so the
    // button and its bottom padding never move. Skipped when the screen scrolls or
    // only just fits (spacer smaller than the shift): the gate keeps it a no-op
    // there, so reclaimed/overflowing screens are unaffected.
    if (body_label && screen.button_list_count > 0 && screen.button_list_spacer) {
        lv_obj_update_layout(screen.body);
        lv_area_t text_area, button_area;
        lv_obj_get_coords(body_label, &text_area);
        lv_obj_get_coords(screen.button_list[0], &button_area);

        // text bottom -> button top. For a shaped (glyph-run) body the run paints
        // a different height than the codepoint label box, so use the run's true
        // drawn bottom (content top + drawn block height); -1 => no run (en/subset
        // locales), in which case the label box bottom is already correct. (A13)
        int32_t text_bottom = text_area.y2;
        int32_t run_h = seedsigner_label_run_drawn_height(body_label);
        if (run_h >= 0) {
            lv_area_t cc;
            lv_obj_get_content_coords(body_label, &cc);
            text_bottom = cc.y1 + run_h;
        }
        int32_t below_gap = button_area.y1 - text_bottom;
        int32_t spacer_height = lv_obj_get_height(screen.button_list_spacer);

        // Shift the body down by half the below-gap to balance the space above and
        // below it. The shift comes out of the flex-grow spacer, so the button stays
        // pinned — but we can never take more than the spacer holds, or the button
        // would move. On TIGHT screens (e.g. a 3-line shaped body at 240x240) the
        // ideal half-gap exceeds the small spacer; CLAMP to the spacer for a partial
        // centering rather than skipping it entirely (which left the body hugging the
        // headline — the reported hi/th symptom). (A13)
        int32_t shift = below_gap / 2;
        if (shift > spacer_height) {
            shift = spacer_height;
        }
        if (shift > 0) {
            lv_obj_set_style_margin_top(body_label, shift, LV_PART_MAIN);
        }
    }

    // bind_screen_navigation auto-detects any remaining body overflow (a long
    // warning that even a full reclaim can't fit) and enables scroll-then-buttons
    // joystick navigation. After a successful reclaim the content fits, so nothing
    // scrolls and the flow is the plain top-nav<->button one.
    bind_screen_navigation(
        cfg,
        screen,
        screen.button_list_count > 0 ? screen.button_list : NULL,
        screen.button_list_count,
        NAV_BODY_VERTICAL,
        0
    );

    load_screen_and_cleanup_previous(screen.screen);
}


// ---------------------------------------------------------------------------
// seed_add_passphrase_screen
// ---------------------------------------------------------------------------
//
// The first text-entry screen of the LVGL port. Rather than re-implementing
// SeedSigner's hand-built button-grid keyboard, this is built on LVGL's native
// `lv_keyboard` (a `lv_buttonmatrix` underneath) + `lv_textarea`. Pixel parity
// with the Python screen is intentionally NOT a goal here — a clean native
// keyboard experience is preferred.
//
// Layout diverges by input mode (matching the rest of the codebase):
//   - Touch (landscape ESP32 boards): full-width keyboard, mouse-driven, with
//     tap-to-position cursor plus in-grid cursor keys.
//   - Hardware/joystick (Pi Zero): the keyboard is the LVGL-group-focused
//     object so the buttonmatrix's own directional navigation moves the key
//     selection; a small key filter hands focus up to the top-nav back button
//     from the keyboard's top row and back down again.
//
// Character inventory is an exact match of the Python screen — the same 94
// printable characters (a-z, A-Z, 0-9, and two symbol sets) plus space —
// enforced by `audit_passphrase_charset()`. Because the native default keymaps
// have a different inventory, every mode uses a custom map via
// `lv_keyboard_set_map`.
//
// Switching/confirm in v1 uses the native in-grid control keys ("abc"/"ABC"
// shift, "1#" symbols, OK), which `lv_keyboard_def_event_cb` handles for free
// in both modes. (The dedicated right-side KEY1/KEY2/KEY3 hardware panel from
// the plan is a documented fast-follow; the in-grid keys are reachable by both
// touch and joystick today.)

extern "C" __attribute__((weak)) void seedsigner_lvgl_on_text_entered(const char *text) {
    (void)text;
}

// Static-render mode: when enabled, screens render without animations that would
// make a still capture non-deterministic — currently the text-entry cursor is
// shown without blinking. Off by default; the screenshot generator turns it on.
// Live use leaves it off so the cursor blinks normally.
static bool g_static_render = false;
extern "C" void seedsigner_lvgl_set_static_render(bool enabled) {
    g_static_render = enabled;
}

// Exact character inventory — source of truth mirrored from the Python app
// (seedsigner/src/seedsigner/gui/screens/seed_screens.py:664-673). These five
// disjoint sets total 94 distinct printable characters; the keymaps below must
// reproduce exactly this set (audited at screen build time).
static const char *PASSPHRASE_CHARSET_LOWER  = "abcdefghijklmnopqrstuvwxyz";
static const char *PASSPHRASE_CHARSET_UPPER  = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char *PASSPHRASE_CHARSET_DIGITS = "0123456789";
static const char *PASSPHRASE_CHARSET_SYM1   = "!@#$%&();:,.-+='\"?";
static const char *PASSPHRASE_CHARSET_SYM2   = "^*[]{}_\\|<>/`~";

// Button-matrix control entries. `KBW(n)` is a plain character key of relative
// width n; `KBC(n)` adds the control-key flags (NO_REPEAT | CLICK_TRIG |
// CHECKED) for the control labels below — "abc"/"ABC" (case shift), "1#"
// (symbols), and the LV_SYMBOL_* glyphs — which `lv_keyboard_def_event_cb`
// handles natively. Casts are required because `lv_buttonmatrix_ctrl_t` is an
// enum and C++ (unlike LVGL's own C maps) won't implicitly narrow int → enum.
#define KBW(n) ((lv_buttonmatrix_ctrl_t)(n))
#define KBC(n) ((lv_buttonmatrix_ctrl_t)(LV_KEYBOARD_CTRL_BUTTON_FLAGS | (n)))
// Hidden spacer key: reserves a row's height but is not drawn or navigable. Used
// to add an "empty row" so a short page (digits) keeps normal key proportions
// instead of stretching to fill the keyboard height.
#define KBH(n) ((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_HIDDEN | LV_BUTTONMATRIX_CTRL_DISABLED | (n)))

// Mode-switch key labels. Our custom keyboard handler
// (passphrase_kb_value_changed) maps each to a target mode, so we choose
// readable labels (the native def_event_cb, which forces "abc"/"ABC"/"1#", is
// removed): ABC→uppercase, abc→lowercase, 123→digits, !?&→symbols.
#define SYM_LABEL "!?&"
#define NUM_LABEL "123"
#define ABC_LABEL "abc"
#define UPPER_LABEL "ABC"

// ===========================================================================
// QWERTY maps — used at >240px height (320/480), where there is room for a full
// 10-wide QWERTY plus a persistent digit row, so digits need no separate page.
// In-grid controls (shift / symbols / OK); these serve both touch and (the
// unlikely) joystick input at >240, which has no side panel.
// ===========================================================================

// Lowercase letters page: digit row, QWERTY letters, shift-to-uppercase ("ABC"),
// backspace, symbols toggle ("!?&"), cursor keys, space, and OK.
static const char * const passphrase_kb_map_lower[] = {
    "1","2","3","4","5","6","7","8","9","0","\n",
    "q","w","e","r","t","y","u","i","o","p","\n",
    "a","s","d","f","g","h","j","k","l","\n",
    UPPER_LABEL,"z","x","c","v","b","n","m",SeedSignerIconConstants::DELETE,"\n",
    SYM_LABEL,SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::CHECK,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_lower[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBC(2),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBC(2),
    KBC(2),KBW(6),KBC(1),KBC(1),KBC(2)
};

// Uppercase letters page: mirror of lowercase; "abc" shifts back to lowercase.
static const char * const passphrase_kb_map_upper[] = {
    "1","2","3","4","5","6","7","8","9","0","\n",
    "Q","W","E","R","T","Y","U","I","O","P","\n",
    "A","S","D","F","G","H","J","K","L","\n",
    ABC_LABEL,"Z","X","C","V","B","N","M",SeedSignerIconConstants::DELETE,"\n",
    SYM_LABEL,SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::CHECK,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_upper[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBC(2),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBC(2),
    KBC(2),KBW(6),KBC(1),KBC(1),KBC(2)
};

// Symbols page (LV_KEYBOARD_MODE_SPECIAL): all 32 symbols on one page; "abc"
// returns to the lowercase letters page.
static const char * const passphrase_kb_map_special[] = {
    "!","@","#","$","%","&","(",")","\n",
    ";",":",",",".","-","+","=","'","\n",
    "\"","?","^","*","[","]","{","}","\n",
    "_","\\","|","<",">","/","`","~","\n",
    "abc",SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::DELETE,SeedSignerIconConstants::CHECK,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_special[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBC(2),KBW(4),KBC(1),KBC(1),KBC(2),KBC(2)
};

// ===========================================================================
// 240px maps — digits get their OWN page (NUMBER mode) rather than a top row
// that shrinks the letters (matching the Python rationale). Letter ORDERING
// depends on input mode (the user's call): touch favors QWERTY (thumb-typing
// muscle memory), joystick favors ALPHABETICAL for two reasons:
//   1. Navigation: stepping a joystick cursor to a known letter is easier when
//      the letters are in a predictable A-Z order than scattered QWERTY.
//   2. Key size: a small 240px screen doesn't have the WIDTH for QWERTY, whose
//      top row needs 10 columns (q-w-e-r-t-y-u-i-o-p) and so squeezes the keys
//      narrow. Alphabetical lets us choose the column count — 26 letters over
//      4 rows of ~7 columns — trading a row of height (which we have) for key
//      width (which we don't). This is also why the joystick keyboard matches
//      the Pi Zero Python layout, which is alphabetical for the same reason.
// Pages: letters (lower/upper), digits, symbols — cycled by 123 / !?& / abc.
//
//   *_240    — touch: QWERTY letters, in-grid mode/OK control keys.
//   *_240hw  — joystick: alphabetical letters, NO in-grid mode/OK keys (the side
//              KEY1/KEY2/KEY3 panel switches charset + confirms); cursor / space
//              / backspace stay in-grid.
// ===========================================================================

// --- 240 touch (QWERTY, in-grid controls) ---
static const char * const passphrase_kb_map_lower_240[] = {
    "q","w","e","r","t","y","u","i","o","p","\n",
    "a","s","d","f","g","h","j","k","l","\n",
    UPPER_LABEL,"z","x","c","v","b","n","m",SeedSignerIconConstants::DELETE,"\n",
    NUM_LABEL,SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::CHECK,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_lower_240[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBC(2),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBC(2),
    KBC(2),KBW(3),KBC(1),KBC(1),KBC(2)
};

static const char * const passphrase_kb_map_upper_240[] = {
    "Q","W","E","R","T","Y","U","I","O","P","\n",
    "A","S","D","F","G","H","J","K","L","\n",
    ABC_LABEL,"Z","X","C","V","B","N","M",SeedSignerIconConstants::DELETE,"\n",
    NUM_LABEL,SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::CHECK,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_upper_240[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBC(2),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBC(2),
    KBC(2),KBW(3),KBC(1),KBC(1),KBC(2)
};

static const char * const passphrase_kb_map_digits_240[] = {
    "1","2","3","4","\n",
    "5","6","7","8","\n",
    "9","0",SeedSignerIconConstants::DELETE,"\n",
    ABC_LABEL,SYM_LABEL,SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::CHECK,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_digits_240[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBC(2),
    KBC(2),KBC(2),KBW(3),KBC(1),KBC(1),KBC(2)
};

static const char * const passphrase_kb_map_symbols_240[] = {
    "!","@","#","$","%","&","(",")","\n",
    ";",":",",",".","-","+","=","'","\n",
    "\"","?","^","*","[","]","{","}","\n",
    "_","\\","|","<",">","/","`","~","\n",
    ABC_LABEL,NUM_LABEL,SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::DELETE,SeedSignerIconConstants::CHECK,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_symbols_240[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBC(2),KBC(2),KBW(3),KBC(1),KBC(1),KBC(2),KBC(2)
};

// --- 240 joystick (no in-grid mode/OK keys; side panel handles those) ---
static const char * const passphrase_kb_map_lower_240hw[] = {
    "a","b","c","d","e","f","g","\n",
    "h","i","j","k","l","m","n","\n",
    "o","p","q","r","s","t","u","\n",
    "v","w","x","y","z",SeedSignerIconConstants::DELETE,"\n",
    SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_lower_240hw[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBC(2),
    KBW(4),KBC(1),KBC(1)
};

static const char * const passphrase_kb_map_upper_240hw[] = {
    "A","B","C","D","E","F","G","\n",
    "H","I","J","K","L","M","N","\n",
    "O","P","Q","R","S","T","U","\n",
    "V","W","X","Y","Z",SeedSignerIconConstants::DELETE,"\n",
    SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_upper_240hw[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBC(2),
    KBW(4),KBC(1),KBC(1)
};

static const char * const passphrase_kb_map_digits_240hw[] = {
    "1","2","3","4","\n",
    "5","6","7","8","\n",
    "9","0",SeedSignerIconConstants::DELETE,"\n",
    SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_digits_240hw[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBC(2),
    KBW(4),KBC(1),KBC(1)
};

static const char * const passphrase_kb_map_symbols_240hw[] = {
    "!","@","#","$","%","&","(",")","\n",
    ";",":",",",".","-","+","=","'","\n",
    "\"","?","^","*","[","]","{","}","\n",
    "_","\\","|","<",">","/","`","~","\n",
    SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::DELETE,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_symbols_240hw[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(5),KBC(1),KBC(1),KBC(2)
};

// Verify (once, at screen build) that the union of single-character keys across
// the given maps is exactly the 94-char Python inventory — no additions, no
// omissions, no duplicates. Guards against silent drift if a map is edited.
// Run for whichever map set (touch or hardware) the screen is about to use.
static void audit_passphrase_charset(const char * const * const *maps, size_t map_count) {
    std::set<char> expected;
    for (const char *s : { PASSPHRASE_CHARSET_LOWER, PASSPHRASE_CHARSET_UPPER,
                           PASSPHRASE_CHARSET_DIGITS, PASSPHRASE_CHARSET_SYM1,
                           PASSPHRASE_CHARSET_SYM2 }) {
        for (; *s; ++s) expected.insert(*s);
    }

    std::set<char> actual;
    for (size_t m = 0; m < map_count; ++m) {
        const char * const *map = maps[m];
        for (size_t i = 0; map[i][0] != '\0'; ++i) {
            // Single-byte tokens that aren't the row separator or space are the
            // typeable characters; multi-byte LV_SYMBOL_* glyphs and the
            // "abc"/"ABC"/"1#" mode labels are >1 byte and thus skipped.
            if (std::strlen(map[i]) == 1 && map[i][0] != '\n' && map[i][0] != ' ') {
                actual.insert(map[i][0]);
            }
        }
    }

    if (actual != expected) {
        throw std::runtime_error("passphrase keyboard charset does not match the Python inventory");
    }
}

// Hardware/joystick screen state. Lives for the screen's lifetime; freed in
// passphrase_cleanup_cb. Holds what the KEY1/KEY2/KEY3 handlers need plus the
// remembered letter case for returning from the symbols page.
typedef struct {
    lv_obj_t          *kb;
    lv_obj_t          *ta;
    lv_obj_t          *back_btn;
    lv_obj_t          *key1_label;   // KEY1 (case toggle) label
    lv_obj_t          *key2_label;   // KEY2 (symbols toggle) label
    lv_obj_t          *key1_btn;     // KEY1/2/3 side buttons, for the press flash
    lv_obj_t          *key2_btn;
    lv_obj_t          *key3_btn;
    lv_group_t        *group;
    lv_keyboard_mode_t letter_mode;  // LOWER/UPPER to restore when leaving symbols
} passphrase_ctx_t;

// Number of keys in the keyboard's current top row (used for the UP→back-button
// handoff). Counts buttons in the active map until the first row break, so it
// works for any page (alphabetic 7, digits 5, symbols 8, QWERTY 10).
static size_t passphrase_top_row_count(lv_obj_t *kb) {
    const char * const *map = lv_keyboard_get_map_array(kb);
    size_t n = 0;
    while (map && map[n] && map[n][0] != '\0' && std::strcmp(map[n], "\n") != 0) {
        n++;
    }
    return n;
}

// Find the [first, last] button indices of the map row that contains button
// `sel`, so LEFT/RIGHT can wrap within the current row instead of spilling onto
// the adjacent row (matches the Python keyboard's auto_wrap). Assumes the map
// has no hidden buttons (the passphrase maps don't).
static void passphrase_row_bounds(lv_obj_t *kb, uint32_t sel,
                                  uint32_t *first, uint32_t *last) {
    const char * const *map = lv_keyboard_get_map_array(kb);
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

// Button index of a single-character key in the current map (-1 if absent).
// Used to pre-select the last-typed key for the joystick selection highlight.
static int passphrase_find_button(lv_obj_t *kb, char ch) {
    const char * const *map = lv_keyboard_get_map_array(kb);
    int id = 0;
    for (size_t i = 0; map && map[i] && map[i][0] != '\0'; ++i) {
        if (std::strcmp(map[i], "\n") == 0) continue;  // row break: not a button
        if (std::strlen(map[i]) == 1 && map[i][0] == ch) return id;
        id++;
    }
    return -1;
}

// Default joystick selection when arriving at a page fresh — a central key, so
// the first move is short on average: 'k' on the alphabetical letter grid, '6'
// on the digit pad, '.' on the symbol page. Returns a button index (0 if the
// chosen key isn't found in the current map).
static uint32_t passphrase_default_key(lv_obj_t *kb, lv_keyboard_mode_t mode) {
    char ch = 0;
    // The letter layouts differ: the 240 joystick keyboard is alphabetical
    // (a-g / h-n / ...), so 'k' is its center; the QWERTY keyboards (240 touch
    // and the larger profiles) center on 'g' — the middle of the home row.
    // Detect by whether 'a'/'A' is the first key.
    bool alphabetical = (passphrase_find_button(kb, 'a') == 0) ||
                        (passphrase_find_button(kb, 'A') == 0);
    switch (mode) {
        case LV_KEYBOARD_MODE_TEXT_LOWER: ch = alphabetical ? 'k' : 'g'; break;
        case LV_KEYBOARD_MODE_TEXT_UPPER: ch = alphabetical ? 'K' : 'G'; break;
        case LV_KEYBOARD_MODE_NUMBER:     ch = '6'; break;
        case LV_KEYBOARD_MODE_SPECIAL:    ch = '.'; break;
        default: break;
    }
    int idx = ch ? passphrase_find_button(kb, ch) : -1;
    return idx >= 0 ? (uint32_t)idx : 0;
}

// Refresh the KEY1/KEY2 side-panel labels to reflect what pressing them does
// next (mirrors the Python passphrase screen's right-panel labels).
static void passphrase_update_labels(passphrase_ctx_t *c) {
    lv_keyboard_mode_t m = lv_keyboard_get_mode(c->kb);
    bool on_letters = (m == LV_KEYBOARD_MODE_TEXT_LOWER || m == LV_KEYBOARD_MODE_TEXT_UPPER);
    if (c->key1_label) {
        // KEY1: on letters, the case to switch TO; otherwise, back to letters.
        const char *t = !on_letters ? ABC_LABEL
                        : (m == LV_KEYBOARD_MODE_TEXT_UPPER ? ABC_LABEL : UPPER_LABEL);
        lv_label_set_text(c->key1_label, t);
    }
    if (c->key2_label) {
        // KEY2 cycles digits <-> symbols: on digits show "!?&", otherwise "123".
        lv_label_set_text(c->key2_label, (m == LV_KEYBOARD_MODE_NUMBER) ? SYM_LABEL : NUM_LABEL);
    }
}

// Switch the keyboard charset/page. lv_keyboard_set_mode leaves btn_id_sel
// unchanged. Lowercase and uppercase share the same layout, so a case swap keeps
// the active key (pressing shift on "g" lands on "G"). For any other page
// (digits/symbols) the index could fall past the end of the shorter map — no
// visible active key, and the joystick would have to wander to find it — so we
// reset the selection to the first key.
static void passphrase_switch_mode(passphrase_ctx_t *c, lv_keyboard_mode_t mode) {
    lv_keyboard_mode_t old = lv_keyboard_get_mode(c->kb);
    bool case_swap =
        (old  == LV_KEYBOARD_MODE_TEXT_LOWER || old  == LV_KEYBOARD_MODE_TEXT_UPPER) &&
        (mode == LV_KEYBOARD_MODE_TEXT_LOWER || mode == LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_keyboard_set_mode(c->kb, mode);
    if (!case_swap) {
        lv_buttonmatrix_set_selected_button(c->kb, passphrase_default_key(c->kb, mode));
    }
}

// KEY1 — on a letters page, toggle case; on the digits/symbols page, return to
// the remembered letters page.
static void passphrase_key1_case(passphrase_ctx_t *c) {
    lv_keyboard_mode_t m = lv_keyboard_get_mode(c->kb);
    if (m == LV_KEYBOARD_MODE_TEXT_LOWER || m == LV_KEYBOARD_MODE_TEXT_UPPER) {
        c->letter_mode = (m == LV_KEYBOARD_MODE_TEXT_UPPER)
                         ? LV_KEYBOARD_MODE_TEXT_LOWER : LV_KEYBOARD_MODE_TEXT_UPPER;
    }
    passphrase_switch_mode(c, c->letter_mode);
    passphrase_update_labels(c);
}

// KEY2 — cycle letters → digits → symbols → digits (the way back to letters is
// KEY1). Remembers the letters case when leaving a letters page.
static void passphrase_key2_cycle(passphrase_ctx_t *c) {
    lv_keyboard_mode_t m = lv_keyboard_get_mode(c->kb);
    if (m == LV_KEYBOARD_MODE_NUMBER) {
        passphrase_switch_mode(c, LV_KEYBOARD_MODE_SPECIAL);
    } else if (m == LV_KEYBOARD_MODE_SPECIAL) {
        passphrase_switch_mode(c, LV_KEYBOARD_MODE_NUMBER);
    } else {
        c->letter_mode = m;
        passphrase_switch_mode(c, LV_KEYBOARD_MODE_NUMBER);
    }
    passphrase_update_labels(c);
}

// Momentary "pressed" flash on a side button when its physical key is hit. The
// side buttons aren't clickable, so we drive the PRESSED state by hand: add it,
// then clear it after a short beat via a one-shot timer (matches Python, which
// shows the button react before its state updates).
static void passphrase_flash_clear_cb(lv_timer_t *t) {
    lv_obj_t *btn = (lv_obj_t *)lv_timer_get_user_data(t);
    if (btn && lv_obj_is_valid(btn)) lv_obj_remove_state(btn, LV_STATE_PRESSED);
    lv_timer_delete(t);
}
static void passphrase_flash_side_button(lv_obj_t *btn) {
    if (!btn || !lv_obj_is_valid(btn)) return;
    lv_obj_add_state(btn, LV_STATE_PRESSED);
    lv_timer_t *t = lv_timer_create(passphrase_flash_clear_cb, 120, btn);
    lv_timer_set_repeat_count(t, 1);
}

// Hardware-mode key filter on the keyboard. Handles the KEY1/KEY2/KEY3 aux keys
// (case / symbols / confirm) and the top-nav handoff: when the selection is on
// the top row and UP is pressed, focus the back button. The buttonmatrix does
// not wrap UP off the top row, so this is the seam between the keyboard's
// internal navigation and the top-nav zone.
static void passphrase_kb_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    passphrase_ctx_t *c = (passphrase_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;

    uint32_t key = lv_event_get_key(e);

    // KEY1/KEY2/KEY3 (ASCII '1'/'2'/'3'; see is_aux_key in navigation.cpp) only
    // act when the side panel is present (240 joystick). At >240 there is no
    // panel — switching/confirm is via the in-grid mode/OK keys.
    if (c->key2_label) {
        if (key == (uint32_t)'1') { passphrase_flash_side_button(c->key1_btn); passphrase_key1_case(c); return; }
        if (key == (uint32_t)'2') { passphrase_flash_side_button(c->key2_btn); passphrase_key2_cycle(c); return; }
        if (key == (uint32_t)'3') {
            passphrase_flash_side_button(c->key3_btn);
            if (c->ta && lv_obj_is_valid(c->ta)) {
                seedsigner_lvgl_on_text_entered(lv_textarea_get_text(c->ta));
            }
            return;
        }
    }

    if (key == LV_KEY_UP) {
        if (!c->back_btn || !lv_obj_is_valid(c->back_btn)) return;
        uint32_t sel = lv_keyboard_get_selected_button(c->kb);
        if (sel == LV_BUTTONMATRIX_BUTTON_NONE) return;
        if (sel < passphrase_top_row_count(c->kb)) {
            lv_group_focus_obj(c->back_btn);
        }
    }

    // LEFT/RIGHT wrap within the current row rather than spilling onto the
    // adjacent row (matches the Python keyboard's auto_wrap). As a PREPROCESS
    // handler we see the selection before the buttonmatrix moves it, set the
    // wrapped target ourselves, and stop the event so its default linear move
    // (which would cross rows) does not run.
    if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
        uint32_t sel = lv_keyboard_get_selected_button(c->kb);
        if (sel == LV_BUTTONMATRIX_BUTTON_NONE) return;  // let the default enter the grid
        uint32_t first, last;
        passphrase_row_bounds(c->kb, sel, &first, &last);
        uint32_t target = (key == LV_KEY_RIGHT) ? (sel >= last  ? first : sel + 1)
                                                : (sel <= first ? last  : sel - 1);
        lv_buttonmatrix_set_selected_button(c->kb, target);
        lv_event_stop_processing(e);
    }
}

// Hardware-mode key filter on the back button: DOWN returns focus to the
// keyboard.
static void passphrase_back_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    if (lv_event_get_key(e) != LV_KEY_DOWN) return;

    passphrase_ctx_t *c = (passphrase_ctx_t *)lv_event_get_user_data(e);
    if (c && c->kb && lv_obj_is_valid(c->kb)) {
        lv_group_focus_obj(c->kb);
    }
}

// Tear down the group + ctx when the screen is destroyed (mirrors
// nav_cleanup_handler / screensaver_cleanup_handler). lv_group_del clears the
// indev's group pointer so no stale reference survives the screen swap.
static void passphrase_cleanup_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    passphrase_ctx_t *c = (passphrase_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    if (c->group) {
        lv_group_del(c->group);
    }
    lv_free(c);
}

// Build one right-side panel button (KEY1/KEY2/KEY3 indicator). These are
// display-only — they show what the physical keys do; they are not
// joystick-navigable targets.
// `clipped_right` is how many px of the button run off the right screen edge.
// The label is centered within the VISIBLE portion (full width minus the clipped
// strip), not the full button — matching Python, which re-centers the text for
// what remains on-screen. Pass 0 for a fully-visible button.
static lv_obj_t *passphrase_side_button(lv_obj_t *parent, int32_t x, int32_t y,
                                        int32_t w, int32_t h, const char *text,
                                        const lv_font_t *font, int color,
                                        int32_t clipped_right,
                                        lv_obj_t **out_label) {
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
    // the KEY1/KEY2/KEY3 handlers. Glyph color is set on the button (not the
    // label) so the PRESSED selector applies and the label inherits the color.
    lv_obj_set_style_bg_color(btn, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btn, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_color(btn, lv_color_hex(BUTTON_SELECTED_FONT_COLOR), LV_PART_MAIN | LV_STATE_PRESSED);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);

    // A single SeedSigner icon glyph (the confirm check) is line-centered by the
    // label box, but its ink can sit off-center within that box (glyph metrics
    // vary — the back chevron happens to be centered, the check is not). Nudge it
    // by the gap between the glyph's ink center and the line center.
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

// Custom keyboard handler, installed in place of lv_keyboard_def_event_cb so we
// control the in-grid control-key labels (notably the symbols toggle, which the
// native handler would force to "1#"). Handles touch-mode in-grid control keys
// plus character insertion / backspace / cursor for both modes.
static void passphrase_kb_value_changed(lv_event_t *e) {
    lv_obj_t *kb = lv_event_get_target_obj(e);
    passphrase_ctx_t *c = (passphrase_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;

    uint32_t id = lv_keyboard_get_selected_button(kb);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *txt = lv_keyboard_get_button_text(kb, id);
    if (!txt) return;

    lv_obj_t *ta = c->ta;

    // In-grid mode-switch keys (present in the touch maps). Each label maps to a
    // target mode; the custom handler owns this since def_event_cb was removed.
    if (std::strcmp(txt, UPPER_LABEL) == 0) {
        c->letter_mode = LV_KEYBOARD_MODE_TEXT_UPPER;
        passphrase_switch_mode(c, LV_KEYBOARD_MODE_TEXT_UPPER);
        passphrase_update_labels(c);
        return;
    }
    if (std::strcmp(txt, ABC_LABEL) == 0) {
        c->letter_mode = LV_KEYBOARD_MODE_TEXT_LOWER;
        passphrase_switch_mode(c, LV_KEYBOARD_MODE_TEXT_LOWER);
        passphrase_update_labels(c);
        return;
    }
    if (std::strcmp(txt, NUM_LABEL) == 0) {
        passphrase_switch_mode(c, LV_KEYBOARD_MODE_NUMBER);
        passphrase_update_labels(c);
        return;
    }
    if (std::strcmp(txt, SYM_LABEL) == 0) {
        passphrase_switch_mode(c, LV_KEYBOARD_MODE_SPECIAL);
        passphrase_update_labels(c);
        return;
    }
    if (std::strcmp(txt, SeedSignerIconConstants::CHECK) == 0) {
        if (ta && lv_obj_is_valid(ta)) {
            seedsigner_lvgl_on_text_entered(lv_textarea_get_text(ta));
        }
        return;
    }

    if (!ta || !lv_obj_is_valid(ta)) return;

    // Editing keys + character insertion (both modes).
    if (std::strcmp(txt, SeedSignerIconConstants::DELETE) == 0) { lv_textarea_delete_char(ta); return; }
    if (std::strcmp(txt, SeedSignerIconConstants::CHEVRON_LEFT) == 0)  { lv_textarea_cursor_left(ta); return; }
    if (std::strcmp(txt, SeedSignerIconConstants::CHEVRON_RIGHT) == 0) { lv_textarea_cursor_right(ta); return; }
    // The space key displays the seedsigner space glyph; insert a real space.
    if (std::strcmp(txt, SeedSignerIconConstants::SPACE) == 0) { lv_textarea_add_text(ta, " "); return; }
    lv_textarea_add_text(ta, txt);
}

// Per-key restyling of the SeedSigner control-icon keys (CHECK, the two
// CHEVRON cursors, DELETE, SPACE). These glyphs are merged into the keyboard
// text font for layout, but they need two fixes the buttonmatrix can't do per
// key, done here at draw time (it tags each label task with the button index in
// base.id1):
//
//   - Color: the confirm CHECK is green; SPACE is muted gray (it enters a real
//     character); the cursor + backspace controls are SeedSigner-orange so they
//     read as actions, not enterable glyphs (clearest on the symbol page).
//   - Vertical centering: lv_keyboard line-centers key text by the font line
//     height, but these icons are bottom-anchored and as tall as the ascent
//     (they were designed for a dedicated icon font, then merged onto the text
//     baseline), so they would sit at the top of the key. Nudge each down by the
//     gap between its ink center and the line-box center — computed from the
//     font so it holds at any size, with no edits to the generated font data.
static void passphrase_kb_draw_cb(lv_event_t *e) {
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
    // active text color (BUTTON_SELECTED_FONT_COLOR, black) so the glyph stays
    // visible on the orange highlight — exactly like the letter keys. Only
    // recolor the resting state:
    //   CHECK confirms (green); SPACE enters a real character (muted gray, like
    //   Python); the cursor + backspace keys are controls, kept SeedSigner-orange
    //   so they read as actions, not enterable glyphs — clearest on the symbol
    //   page where every surrounding key IS enterable.
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

// Apply SeedSigner styling to the keyboard: black panel, dark keys with light
// text, control keys marked in accent orange, and the selected/pressed key
// highlighted in SeedSigner orange (matches button_set_active elsewhere).
static void passphrase_style_keyboard(lv_obj_t *kb) {
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
    // Tight inter-key gaps + modest rounding so the keys read as large hit
    // targets (the default theme padding/radius made them look small).
    lv_obj_set_style_pad_row(kb, COMPONENT_PADDING / 4, LV_PART_MAIN);
    lv_obj_set_style_pad_column(kb, COMPONENT_PADDING / 4, LV_PART_MAIN);

    // Keys: fixed-width font (matches the text-entry box), dark fill, light text.
    lv_obj_set_style_text_font(kb, &KEYBOARD_FONT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(BUTTON_BACKGROUND_COLOR), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(BUTTON_FONT_COLOR), LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, BUTTON_RADIUS / 2, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);

    // Control keys are flagged CHECKED; the theme would draw them light. Force
    // the same dark fill as every other key (dark-mode throughout) and mark them
    // only with accent-orange text so they read as actions, not a light key.
    lv_obj_set_style_bg_color(kb, lv_color_hex(BUTTON_BACKGROUND_COLOR), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, lv_color_hex(ACCENT_COLOR), LV_PART_ITEMS | LV_STATE_CHECKED);

    // Selected key: SeedSigner orange, FULL opacity, black text. Joystick
    // navigation marks the key FOCUSED/FOCUS_KEY; touch press and our static
    // screenshot highlight use PRESSED. bg_opa COVER is set explicitly because
    // the default theme draws the focus states at partial opacity — that looked
    // like a muted/inactive dark orange. Control keys are also CHECKED, so the
    // CHECKED combos are styled too.
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

    // Recolor the OK key green at draw time (per-key color isn't a style option).
    lv_obj_add_flag(kb, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(kb, passphrase_kb_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
}

// Parse cfg["initial_mode"] into a starting keyboard mode (for screenshots /
// deep-links into a specific charset page). Defaults to lowercase letters.
static lv_keyboard_mode_t passphrase_initial_mode(const json &cfg) {
    if (cfg.contains("initial_mode") && cfg["initial_mode"].is_string()) {
        std::string m = cfg["initial_mode"].get<std::string>();
        if (m == "upper")   return LV_KEYBOARD_MODE_TEXT_UPPER;
        if (m == "digits")  return LV_KEYBOARD_MODE_NUMBER;
        if (m == "symbols") return LV_KEYBOARD_MODE_SPECIAL;
    }
    return LV_KEYBOARD_MODE_TEXT_LOWER;
}

void seed_add_passphrase_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Default the title if the caller didn't supply top_nav (keeps minimal
    // scenarios terse).
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) {
        cfg["top_nav"] = json::object();
    }
    if (!cfg["top_nav"].contains("title")) {
        cfg["top_nav"]["title"] = "Enter Passphrase";
    }

    // Resolve input mode early — it selects letter ordering and the layout.
    bool has_mode_override = false;
    input_mode_t mode_override = INPUT_MODE_TOUCH;
    nav_mode_override_from_cfg(cfg, has_mode_override, mode_override);
    bool hardware = (has_mode_override ? mode_override : input_profile_get_mode()) == INPUT_MODE_HARDWARE;

    // Layout decisions:
    //  - is_240: at 240px there is no room for a number row, so digits get their
    //    own page (keeps letter keys from shrinking); >240 keeps the number row.
    //  - use_side_panel: only at 240 + joystick — the Pi Zero, the one device
    //    with physical KEY1/KEY2/KEY3. Larger screens never show the panel.
    const bool is_240 = (active_profile().height == 240);
    const bool use_side_panel = hardware && is_240;
    const bool has_digits_page = is_240;

    // Keymap set:
    //  - >240 (touch or joystick): QWERTY + number row, in-grid controls.
    //  - 240 touch:   QWERTY (no number row) + digits page, in-grid controls.
    //  - 240 joystick: alphabetical (no number row) + digits page, NO in-grid
    //                  controls (the side panel switches charset + confirms).
    const char * const *map_lower;
    const char * const *map_upper;
    const char * const *map_special;
    const char * const *map_digits = nullptr;
    const lv_buttonmatrix_ctrl_t *ctrl_lower;
    const lv_buttonmatrix_ctrl_t *ctrl_upper;
    const lv_buttonmatrix_ctrl_t *ctrl_special;
    const lv_buttonmatrix_ctrl_t *ctrl_digits = nullptr;
    if (!is_240) {
        map_lower = passphrase_kb_map_lower;       ctrl_lower = passphrase_kb_ctrl_lower;
        map_upper = passphrase_kb_map_upper;       ctrl_upper = passphrase_kb_ctrl_upper;
        map_special = passphrase_kb_map_special;   ctrl_special = passphrase_kb_ctrl_special;
    } else if (use_side_panel) {
        map_lower = passphrase_kb_map_lower_240hw;     ctrl_lower = passphrase_kb_ctrl_lower_240hw;
        map_upper = passphrase_kb_map_upper_240hw;     ctrl_upper = passphrase_kb_ctrl_upper_240hw;
        map_digits = passphrase_kb_map_digits_240hw;   ctrl_digits = passphrase_kb_ctrl_digits_240hw;
        map_special = passphrase_kb_map_symbols_240hw; ctrl_special = passphrase_kb_ctrl_symbols_240hw;
    } else {
        map_lower = passphrase_kb_map_lower_240;     ctrl_lower = passphrase_kb_ctrl_lower_240;
        map_upper = passphrase_kb_map_upper_240;     ctrl_upper = passphrase_kb_ctrl_upper_240;
        map_digits = passphrase_kb_map_digits_240;   ctrl_digits = passphrase_kb_ctrl_digits_240;
        map_special = passphrase_kb_map_symbols_240; ctrl_special = passphrase_kb_ctrl_symbols_240;
    }

    // Fail fast if a future edit ever breaks the exact-inventory guarantee.
    if (has_digits_page) {
        const char * const * audit_maps[] = { map_lower, map_upper, map_digits, map_special };
        audit_passphrase_charset(audit_maps, 4);
    } else {
        const char * const * audit_maps[] = { map_lower, map_upper, map_special };
        audit_passphrase_charset(audit_maps, 3);
    }

    // No button_list: the body is custom (textarea + keyboard). upper_body == body.
    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);
    lv_obj_t *body = screen.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

    const int32_t content_w = lv_obj_get_content_width(body);
    const int32_t content_h = lv_obj_get_content_height(body);

    // Reserve a right-side strip for the KEY1/KEY2/KEY3 panel only when it is
    // shown (240 + joystick). Python's right_panel_buttons_width = 56, scaled per
    // profile. The text strip and keyboard occupy the remaining width.
    const int32_t panel_w = use_side_panel ? (56 * active_profile().px_multiplier / 100) : 0;
    // The side panel deliberately runs off the right screen edge to line up with
    // the three physical Waveshare buttons (mirrors Python, whose hw buttons
    // overshoot canvas_width by COMPONENT_PADDING). Reclaiming the right
    // EDGE_PADDING gutter widens the keyboard + text-entry strip by that much.
    const int32_t main_w = use_side_panel ? (content_w - panel_w + EDGE_PADDING) : content_w;

    // Per-screen state (both modes). Freed in passphrase_cleanup_cb.
    passphrase_ctx_t *c = (passphrase_ctx_t *)lv_malloc(sizeof(passphrase_ctx_t));
    lv_memzero(c, sizeof(*c));

    // Text-entry strip: one-line, cleartext. Font matches the keyboard for a
    // consistent look; SeedSigner dark fill with an accent-orange border/cursor.
    // cursor_click_pos (on by default) lets touch tap to position; the in-grid
    // cursor keys are the precise fallback.
    const int32_t ta_border = 2;
    lv_obj_t *ta = lv_textarea_create(body);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, "");
    lv_obj_set_width(ta, main_w);
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
    // Cursor: a thin light-gray I-bar (Python's #ccc bar), drawn as a left border
    // so it sits between characters as an insert/position cursor — the default
    // block fill reads as "overwrite the character". The blink is desirable in
    // live use; disable it only in static-render mode so screenshots reliably
    // capture the cursor (anim_duration 0 -> cursor shown without blinking).
    // The default theme already styles the cursor as a thin left-border I-bar,
    // but with a DARK border (color_text) and on the LV_PART_CURSOR|FOCUSED
    // selector — so when the box is focused that dark border wins over a base-part
    // override and the cursor looks dark grey. Override on BOTH the base and the
    // focused selector with an opaque white border so it's clearly visible.
    const int32_t cur_w = 2 * active_profile().px_multiplier / 100;
    // Box vertical-centering padding (computed up here because the cursor inset
    // below is derived from it). The box height under LV_SIZE_CONTENT is
    // line_height + 2*pad + 2*border, so size the padding (minus the border) to
    // land near BUTTON_HEIGHT.
    int32_t ta_pad_v = (BUTTON_HEIGHT - (int32_t)lv_font_get_line_height(&KEYBOARD_FONT)) / 2 - ta_border;
    if (ta_pad_v < 0) ta_pad_v = 0;
    // Keep a constant small gap (>=1px, scaled) between the cursor bar and the
    // top/bottom of the text box at every size. refr_cursor_area makes the bar
    // letter_h + 2*(pad + cur_w) tall while the box interior is letter_h +
    // 2*ta_pad_v, so a top gap of cur_gap needs pad = ta_pad_v - cur_w - cur_gap.
    // 2px at the base profile, widening as the display scales up (taller screens
    // have the room): 100x -> 2, 150x -> 4, 200x -> 6.
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
    if (g_static_render) {
        lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR);
    } else {
        // The textarea is never the group's focused object (the keyboard is), so
        // the cursor blink that normally kicks off on LV_EVENT_FOCUSED never
        // fires. Trigger it manually so the cursor blinks while waiting for the
        // first input, not just after text exists. The textarea's FOCUSED handler
        // does nothing but start_cursor_blink, so this has no other side effects.
        lv_obj_send_event(ta, LV_EVENT_FOCUSED, NULL);
    }
    // One-line entry: never show a (vertical) scrollbar. Horizontal overflow is
    // handled by the textarea scrolling its content to follow the cursor.
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
    // Vertically center the text in the box via the symmetric top/bottom padding
    // computed above.
    lv_obj_set_style_pad_top(ta, ta_pad_v, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(ta, ta_pad_v, LV_PART_MAIN);

    std::string initial_text;
    if (cfg.contains("initial_text") && cfg["initial_text"].is_string()) {
        initial_text = cfg["initial_text"].get<std::string>();
        lv_textarea_set_text(ta, initial_text.c_str());
    }
    if (cfg.contains("max_length") && cfg["max_length"].is_number_integer()) {
        int max_length = cfg["max_length"].get<int>();
        if (max_length > 0) {
            lv_textarea_set_max_length(ta, (uint32_t)max_length);
        }
    }
    lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);

    // Native keyboard with our custom maps. Replace the default event handler
    // with ours so we control the control-key labels (e.g. the symbols toggle).
    lv_obj_t *kb = lv_keyboard_create(body);
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, map_lower, ctrl_lower);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, map_upper, ctrl_upper);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, map_special, ctrl_special);
    if (has_digits_page) {
        lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_NUMBER, map_digits, ctrl_digits);
    }
    lv_keyboard_set_textarea(kb, ta);
    passphrase_style_keyboard(kb);

    lv_keyboard_mode_t start_mode = passphrase_initial_mode(cfg);
    // The digits page only exists at 240; elsewhere fall back to letters.
    if (start_mode == LV_KEYBOARD_MODE_NUMBER && !has_digits_page) {
        start_mode = LV_KEYBOARD_MODE_TEXT_LOWER;
    }
    lv_keyboard_set_mode(kb, start_mode);

    c->kb = kb;
    c->ta = ta;
    c->back_btn = screen.top_back_btn;
    c->letter_mode = (start_mode == LV_KEYBOARD_MODE_TEXT_UPPER)
                     ? LV_KEYBOARD_MODE_TEXT_UPPER : LV_KEYBOARD_MODE_TEXT_LOWER;

    lv_obj_add_event_cb(kb, passphrase_kb_value_changed, LV_EVENT_VALUE_CHANGED, c);

    const int32_t kb_top = BUTTON_HEIGHT + COMPONENT_PADDING;
    const int32_t kb_h = content_h - kb_top;

    // Keyboard fills the area left of any reserved panel strip (main_w ==
    // content_w when there is no panel). lv_obj_align (not set_pos):
    // lv_keyboard_create sets a default BOTTOM_MID anchor in its constructor, so
    // a bare set_pos would offset from the bottom; TOP_LEFT resets the anchor.
    lv_obj_set_size(kb, main_w, kb_h);
    lv_obj_align(kb, LV_ALIGN_TOP_LEFT, 0, kb_top);

    if (use_side_panel) {
        // KEY1/KEY2/KEY3 indicator buttons. Their vertical positions must line up
        // with the device's physical buttons, so they replicate the Python
        // SeedAddPassphraseScreen layout: KEY2 centered on the full canvas,
        // KEY1/KEY3 offset by (3*COMPONENT_PADDING + BUTTON_HEIGHT). Those offsets
        // are canvas-relative; `body` sits TOP_NAV_HEIGHT below the canvas top, so
        // subtract it to get body-local y.
        const int32_t btn_h = BUTTON_HEIGHT;
        const int32_t screen_h = lv_obj_get_height(screen.screen);
        const int32_t spacing = 3 * COMPONENT_PADDING + btn_h;
        const int32_t center_y = (screen_h - btn_h) / 2 - TOP_NAV_HEIGHT;
        // Buttons sit COMPONENT_PADDING right of the keyboard and overshoot the
        // right screen edge by COMPONENT_PADDING (clipped at the body boundary) so
        // they read as aligned with the physical hardware keys.
        const int32_t px = content_w + EDGE_PADDING + COMPONENT_PADDING - panel_w;
        // ABC/123 use the fixed-width keyboard font (Inconsolata), matching the
        // keys and the Python side panel (FIXED_WIDTH_EMPHASIS_FONT_NAME), not the
        // OpenSans body button font.
        const int32_t clipped = COMPONENT_PADDING;  // overshoot off the right edge
        c->key1_btn = passphrase_side_button(body, px, center_y - spacing, panel_w, btn_h,
                               UPPER_LABEL, &KEYBOARD_FONT, BUTTON_FONT_COLOR, clipped, &c->key1_label);
        c->key2_btn = passphrase_side_button(body, px, center_y, panel_w, btn_h,
                               NUM_LABEL, &KEYBOARD_FONT, BUTTON_FONT_COLOR, clipped, &c->key2_label);
        c->key3_btn = passphrase_side_button(body, px, center_y + spacing, panel_w, btn_h,
                               SeedSignerIconConstants::CHECK, &ICON_FONT__SEEDSIGNER, SUCCESS_COLOR, clipped, NULL);
        passphrase_update_labels(c);
    }

    if (hardware) {
        // Localized LVGL-native focus (this screen does NOT use nav_bind): the
        // keyboard is the group-focused object so the buttonmatrix's own
        // directional navigation drives key selection; the back button is the
        // other group member for the top-nav handoff. Used at any size for
        // joystick input; at >240 there is no side panel, so the in-grid mode/OK
        // keys (reached by navigating to them) do the switching/confirming.
        c->group = lv_group_create();
        lv_group_set_wrap(c->group, false);
        lv_group_add_obj(c->group, kb);
        if (screen.top_back_btn) {
            lv_group_add_obj(c->group, screen.top_back_btn);
            // PREPROCESS: run before the buttonmatrix's own key handler so we read
            // the selection BEFORE it moves. Otherwise an UP from the 2nd row first
            // moves the selection into the top row, then our handler sees a top-row
            // selection and wrongly jumps to the back button.
            lv_obj_add_event_cb(kb, passphrase_kb_key_cb,
                                (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), c);
            lv_obj_add_event_cb(screen.top_back_btn, passphrase_back_key_cb, LV_EVENT_KEY, c);
        }
        lv_group_focus_obj(kb);

        // Connect all keypad/encoder indevs to this group.
        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD ||
                lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
                lv_indev_set_group(indev, c->group);
            }
        }

        // Pre-select an initial key so the joystick selection is visible from the
        // start. Otherwise btn_id_sel is NONE and it takes an arrow press just to
        // "enter" the keyboard, with no visible cursor until then. Prefer the
        // last-typed key (prefilled, e.g. the "i" of satoshi), else the page's
        // central default key (k / 6). The highlight shows via the focused-key
        // style in live use; static-render (screenshots) has no indev to apply
        // that focus state, so add PRESSED to make the highlight show in the still.
        int sel = initial_text.empty() ? -1 : passphrase_find_button(kb, initial_text.back());
        if (sel < 0) sel = (int)passphrase_default_key(kb, start_mode);
        lv_buttonmatrix_set_selected_button(kb, (uint32_t)sel);
        if (g_static_render) {
            lv_obj_add_state(kb, LV_STATE_PRESSED);
        }
    }

    // Free ctx (+ group, if any) when the screen is destroyed.
    lv_obj_add_event_cb(screen.screen, passphrase_cleanup_cb, LV_EVENT_DELETE, c);

    load_screen_and_cleanup_previous(screen.screen);
}


void main_menu_screen(void *ctx_json)
{
    // The home menu's structure is fixed (a 2x2 grid of four icon buttons), but
    // its DISPLAY TEXT — the top-nav title and the four button labels — must
    // localize. So those come from the JSON context (translated upstream by the
    // scenario localizer / Python view layer); the four icons never translate.
    //
    // Defaults below reproduce the original English home menu, so the screen
    // still renders correctly when called with no context (ctx_json == NULL).
    json cfg = {
        {"top_nav", {{"title", "Home"}, {"show_back_button", false}, {"show_power_button", true}}},
    };

    // Merge any provided context over the defaults (RFC 7396 merge-patch): a
    // caller can override just the keys it cares about (e.g. only button_list).
    const char *json_str = (const char *)ctx_json;
    if (json_str) {
        json incoming;
        try {
            incoming = json::parse(json_str);
        } catch (...) {
            throw std::runtime_error("invalid JSON syntax");
        }
        if (!incoming.is_object()) {
            throw std::runtime_error("screen config must be a JSON object");
        }
        cfg.merge_patch(incoming);
    }

    // Button labels come from cfg["button_list"] when it supplies exactly the
    // four the grid needs; otherwise fall back to the English defaults. (The
    // grid is a fixed 2x2, so a mismatched count means an ill-formed context —
    // defaulting keeps the screen legible rather than rendering blanks.)
    static const char *default_labels[] = {"Scan", "Seeds", "Tools", "Settings"};
    std::vector<std::string> labels(default_labels, default_labels + 4);
    {
        std::vector<std::string> from_cfg;
        if (read_button_list_labels(cfg, from_cfg) && from_cfg.size() == 4) {
            labels = std::move(from_cfg);
        }
    }

    // Drop button_list before building the scaffold: this screen lays out its own
    // 2x2 large-icon grid, so the scaffold must stay in its no-button_list mode.
    // Leaving the key in would make the scaffold ALSO stack a hidden text-button
    // list in the body, which bleeds through the gaps behind the grid.
    cfg.erase("button_list");

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false, &MAIN_MENU_TITLE_FONT);
    lv_obj_t *scr = screen.screen;
    lv_obj_t *body_content = screen.body;

    static const char *icons[] = {
        SeedSignerIconConstants::SCAN,
        SeedSignerIconConstants::SEEDS,
        SeedSignerIconConstants::TOOLS,
        SeedSignerIconConstants::SETTINGS,
    };

    const int32_t available_w = lv_obj_get_content_width(body_content);
    const int32_t screen_h = lv_obj_get_height(scr);

    // Match the Python LargeButtonScreen button sizing:
    //   button_height = int((canvas_height - top_nav.height - 2*COMPONENT_PADDING - EDGE_PADDING) / 2)
    int32_t button_h = (screen_h - TOP_NAV_HEIGHT - 2 * COMPONENT_PADDING - EDGE_PADDING) / 2;
    int32_t button_w = (available_w - COMPONENT_PADDING) / 2;

    // Cap the button width to the 320x240 profile's button proportions so wider-
    // than-4:3 displays don't stretch the 2x2 grid buttons too far. Keep the full
    // button HEIGHT (the grid still fills the screen vertically) but limit the
    // WIDTH to the reference aspect, then center the grid (below) so the body
    // pillar-boxes symmetrically. The top_nav spans the full width regardless, so
    // its power button stays pinned to the far right.
    //
    // Reference = the 320x240 buttons (the widest small/4:3 profile), computed from
    // that profile's geometry. 320x240 renders at PX_MULTIPLIER_100, so these are
    // the unscaled base constants (EDGE_PADDING=8, COMPONENT_PADDING=8,
    // top_nav_height=48):
    //   ref_h = (240 - 48 - 2*8 - 8) / 2          = 84
    //   ref_w = (320 - 2*8 [body pad] - 8) / 2    = 148
    // 240x240 (ref_w would be 108) and 320x240 (exactly 148) stay below the cap, so
    // both are left byte-identical; only 480/800 narrow + pillar-box.
    constexpr int32_t REF_BTN_W = 148;   // 320x240 button width
    constexpr int32_t REF_BTN_H = 84;    // 320x240 button height
    int32_t max_button_w = button_h * REF_BTN_W / REF_BTN_H;
    if (button_w > max_button_w) {
        button_w = max_button_w;
    }

    // Vertically center the 2x2 grid, matching the Python LargeButtonScreen:
    //   button_start_y = top_nav_h + (canvas_h - (top_nav_h + CP) - 2*button_h - CP) / 2
    // Computed relative to the body origin (which sits at top_nav bottom).
    int32_t below_nav = screen_h - TOP_NAV_HEIGHT;
    int32_t y_offset = (below_nav - COMPONENT_PADDING - 2 * button_h - COMPONENT_PADDING) / 2;

    // Horizontally center the (possibly width-capped) grid within the body so wide
    // displays pillar-box symmetrically; x_offset is 0 when the grid fills the width.
    int32_t grid_w = 2 * button_w + COMPONENT_PADDING;
    int32_t x_offset = (available_w - grid_w) / 2;
    if (x_offset < 0) x_offset = 0;

    lv_obj_t *buttons[4] = {NULL, NULL, NULL, NULL};
    for (uint32_t i = 0; i < 4; ++i) {
        lv_obj_t *btn = large_icon_button(body_content, icons[i], labels[i].c_str(), NULL);
        lv_obj_set_size(btn, button_w, button_h);
        buttons[i] = btn;
    }

    // first row
    lv_obj_set_pos(buttons[0], x_offset, y_offset);
    lv_obj_set_pos(buttons[1], x_offset + button_w + COMPONENT_PADDING, y_offset);

    // second row
    lv_obj_set_pos(buttons[2], x_offset, y_offset + button_h + COMPONENT_PADDING);
    lv_obj_set_pos(buttons[3], x_offset + button_w + COMPONENT_PADDING, y_offset + button_h + COMPONENT_PADDING);

    // Bind shared nav behavior using this screen's body focusables/layout.
    bind_screen_navigation(
        cfg,
        screen,
        buttons,
        4,
        NAV_BODY_GRID,
        0
    );

    load_screen_and_cleanup_previous(scr);
}


// ---------------------------------------------------------------------------
// screensaver_screen
// ---------------------------------------------------------------------------

extern "C" void seedsigner_lvgl_on_button_selected(uint32_t index, const char *label);

typedef struct {
    lv_obj_t   *screen;
    lv_obj_t   *logo_img;
    lv_timer_t *timer;
    lv_group_t *group;
    float       center_x;  // logo center, float for sub-pixel accuracy
    float       center_y;
    float       vel_x;     // pixels per millisecond
    float       vel_y;
    uint32_t    last_tick;
    int32_t     screen_w;
    int32_t     screen_h;
    int32_t     logo_w;    // displayed width after zoom
    int32_t     logo_h;    // displayed height after zoom
    bool        route_dismiss;  // true: input fires a host dismiss result (legacy
                                // path); false: the overlay manager's idle-watch
                                // dismisses, so input is not host-routed here.
} screensaver_ctx_t;

// Speed range: 0.07 – 0.18 pixels/ms  (70 – 180 px/s).
static constexpr float SAVER_SPEED_MIN = 0.07f;
static constexpr float SAVER_SPEED_MAX = 0.18f;

// Minimum angle from the wall surface on departure (degrees).
// Prevents the logo from hugging a wall at a shallow grazing angle.
static constexpr float SAVER_MIN_WALL_ANGLE_RAD = 25.0f * 3.14159265f / 180.0f;

// Returns a random float in [lo, hi).
static float saver_randf(float lo, float hi) {
    uint32_t r = lv_rand(0, 0x7fffffffu);
    return lo + (hi - lo) * ((float)r / (float)0x7fffffffu);
}

// Pick a random departure angle within the half-plane defined by 'normal_angle'
// (the inward wall normal), clamped so the angle is at least SAVER_MIN_WALL_ANGLE
// away from either wall surface edge.  This eliminates wall-hugging trajectories.
static float saver_bounce_angle(float normal_angle) {
    float max_offset = (3.14159265f / 2.0f) - SAVER_MIN_WALL_ANGLE_RAD;
    float offset = saver_randf(-max_offset, max_offset);
    return normal_angle + offset;
}

static void screensaver_timer_cb(lv_timer_t *timer) {
    screensaver_ctx_t *ctx = (screensaver_ctx_t *)lv_timer_get_user_data(timer);

    // Legacy (Python-driven) path only: poll pointer devices and route a dismiss
    // to the host on touch. In overlay-manager mode (route_dismiss == false) the
    // manager's idle-watch handles dismissal — any input resets
    // lv_display_get_inactive_time() — so the screensaver does not route input.
    if (ctx->route_dismiss) {
        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER &&
                lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
                seedsigner_lvgl_on_button_selected(SEEDSIGNER_RET_SCREENSAVER_DISMISS, "screensaver_dismiss");
                return;
            }
        }
    }

    uint32_t now     = lv_tick_get();
    uint32_t elapsed = now - ctx->last_tick;
    ctx->last_tick   = now;

    // Clamp elapsed to avoid huge jumps after screen switches or pauses.
    if (elapsed > 200) elapsed = 200;

    ctx->center_x += ctx->vel_x * (float)elapsed;
    ctx->center_y += ctx->vel_y * (float)elapsed;

    bool bounced_x = false;
    bool bounced_y = false;
    bool hit_left  = false;
    bool hit_top   = false;

    if (ctx->center_x < 0.0f) {
        ctx->center_x = 0.0f;
        bounced_x = true; hit_left = true;
    } else if (ctx->center_x > (float)ctx->screen_w) {
        ctx->center_x = (float)ctx->screen_w;
        bounced_x = true;
    }

    if (ctx->center_y < 0.0f) {
        ctx->center_y = 0.0f;
        bounced_y = true; hit_top = true;
    } else if (ctx->center_y > (float)ctx->screen_h) {
        ctx->center_y = (float)ctx->screen_h;
        bounced_y = true;
    }

    if (bounced_x || bounced_y) {
        // Inward normal angle for the wall(s) hit.
        // Screen coords: +x = right, +y = down.
        // Left wall  normal: 0          Right wall normal: π
        // Top wall   normal: π/2 (down) Bottom wall normal: -π/2 (up)
        float normal;
        if (bounced_x && bounced_y) {
            // Corner: diagonal normal pointing toward screen interior.
            normal = hit_left
                ? (hit_top ? (3.14159265f / 4.0f)        // top-left  → SE
                           : (-3.14159265f / 4.0f))       // bot-left  → NE
                : (hit_top ? (3.0f * 3.14159265f / 4.0f) // top-right → SW
                           : (-3.0f * 3.14159265f / 4.0f)); // bot-right → NW
        } else if (bounced_x) {
            normal = hit_left ? 0.0f : 3.14159265f;
        } else {
            normal = hit_top ? (3.14159265f / 2.0f) : (-3.14159265f / 2.0f);
        }

        float speed     = saver_randf(SAVER_SPEED_MIN, SAVER_SPEED_MAX);
        float new_angle = saver_bounce_angle(normal);
        ctx->vel_x = speed * cosf(new_angle);
        ctx->vel_y = speed * sinf(new_angle);
    }

    lv_obj_set_pos(ctx->logo_img,
                   (int32_t)(ctx->center_x - ctx->logo_w / 2.0f),
                   (int32_t)(ctx->center_y - ctx->logo_h / 2.0f));
}

static void screensaver_key_handler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    seedsigner_lvgl_on_button_selected(SEEDSIGNER_RET_SCREENSAVER_DISMISS, "screensaver_dismiss");
}

static void screensaver_cleanup_handler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;

    screensaver_ctx_t *ctx = (screensaver_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;

    if (ctx->timer) {
        lv_timer_del(ctx->timer);
        ctx->timer = NULL;
    }
    if (ctx->group) {
        lv_group_del(ctx->group);
        ctx->group = NULL;
    }
    lv_free(ctx);
}

// Build the screensaver screen (bouncing logo) WITHOUT loading it — the caller
// loads it. `route_dismiss_to_host` selects how the screensaver gets dismissed:
//   true  — legacy Python-driven path: a key/touch fires
//           SEEDSIGNER_RET_SCREENSAVER_DISMISS (via seedsigner_lvgl_on_button_selected)
//           and the host runner restores the saved screen.
//   false — overlay-manager path: the manager's idle-watch dismisses on any
//           input (lv_display_get_inactive_time() resets), so input is NOT
//           host-routed here. The keypad sink + group are still installed so the
//           wake keypress is swallowed rather than actuating the restored screen.
static lv_obj_t *ss_build_screensaver_impl(bool route_dismiss_to_host) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
    int32_t screen_h = lv_display_get_vertical_resolution(NULL);

    // Display the logo at native resolution (no zoom). The image is pre-scaled
    // by png_to_lvgl.py and selected per display profile (px_multiplier) by
    // seedsigner_logo_for_active_profile().
    const lv_image_dsc_t *logo = seedsigner_logo_for_active_profile();
    int32_t logo_w = (int32_t)logo->header.w;
    int32_t logo_h = (int32_t)logo->header.h;

    lv_obj_t *logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, logo);
    lv_obj_set_size(logo_img, logo_w, logo_h);

    // Allocate and initialise animation context.
    screensaver_ctx_t *ctx = (screensaver_ctx_t *)lv_malloc(sizeof(screensaver_ctx_t));
    lv_memzero(ctx, sizeof(*ctx));
    ctx->screen   = scr;
    ctx->logo_img = logo_img;
    ctx->screen_w = screen_w;
    ctx->screen_h = screen_h;
    ctx->logo_w   = logo_w;
    ctx->logo_h   = logo_h;
    ctx->route_dismiss = route_dismiss_to_host;

    // Start at screen center.
    ctx->center_x = screen_w / 2.0f;
    ctx->center_y = screen_h / 2.0f;

    // Random initial direction and speed.
    float init_speed = saver_randf(SAVER_SPEED_MIN, SAVER_SPEED_MAX);
    float init_angle = saver_randf(0.0f, 2.0f * 3.14159265f);
    ctx->vel_x = init_speed * cosf(init_angle);
    ctx->vel_y = init_speed * sinf(init_angle);

    ctx->last_tick = lv_tick_get();

    // Place logo at starting position.
    lv_obj_set_pos(logo_img,
                   (int32_t)(ctx->center_x - logo_w / 2.0f),
                   (int32_t)(ctx->center_y - logo_h / 2.0f));

    ctx->timer = lv_timer_create(screensaver_timer_cb, 16, ctx);

    // Keypad sink: any key press dismisses the screensaver.
    if (input_profile_get_mode() == INPUT_MODE_HARDWARE) {
        lv_obj_t *sink = lv_obj_create(scr);
        lv_obj_set_size(sink, 1, 1);
        lv_obj_set_pos(sink, 0, 0);
        lv_obj_set_style_opa(sink, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sink, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(sink, 0, LV_PART_MAIN);
        lv_obj_set_style_outline_width(sink, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(sink, 0, LV_PART_MAIN);
        lv_obj_remove_flag(sink, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

        ctx->group = lv_group_create();
        lv_group_add_obj(ctx->group, sink);
        // Only the legacy path fires a host dismiss on keypress. In manager mode
        // the sink still swallows the wake keypress (no handler installed) while
        // the idle-watch performs the dismiss.
        if (route_dismiss_to_host) {
            lv_obj_add_event_cb(sink, screensaver_key_handler, LV_EVENT_KEY, ctx);
        }

        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD ||
                lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
                lv_indev_set_group(indev, ctx->group);
            }
        }
    }

    lv_obj_add_event_cb(scr, screensaver_cleanup_handler, LV_EVENT_DELETE, ctx);
    return scr;
}

// Internal shared builder (declared in seedsigner.h) — used by the overlay
// manager to build a manager-dismissed screensaver.
extern "C" lv_obj_t *ss_build_screensaver_obj(bool route_dismiss_to_host) {
    return ss_build_screensaver_impl(route_dismiss_to_host);
}

void screensaver_screen(void * /*ctx_json*/) {
    // Legacy entry point: build with host-routed dismiss and load WITHOUT
    // destroying the previous screen (the caller save/restores via
    // save_screen/restore_screen).
    lv_scr_load(ss_build_screensaver_impl(true));
}


// ---------------------------------------------------------------------------
// splash_screen — opening splash (parity with Python OpeningSplashScreen)
// ---------------------------------------------------------------------------
// Like every screen here it BUILDS-and-RETURNS; the timed reveal is driven by an
// lv_anim (logo fade) + an lv_timer (hold/reveal sequence), and completion is
// emitted via seedsigner_lvgl_on_button_selected(SEEDSIGNER_RET_SPLASH_COMPLETE)
// — the screensaver_screen() async pattern. The screenshot generator renders a
// single static frame, so the FINAL frame is composed at full opacity up front
// and the fade/timer/hidden-band are gated behind !g_static_render.
//
// MicroPython passes logo_already_shown=true: the firmware holds the C-boot logo
// on the display through boot, so the splash shows the logo SOLID from frame 0
// (no fade) and animates only the version + partner band — a seamless handoff.

typedef enum {
    SPLASH_PHASE_INTRO = 0,  // logo fading/sliding in; version + band hidden
    SPLASH_PHASE_LOGO,       // logo settled, version visible, holding
    SPLASH_PHASE_PARTNER,    // partner band revealed, holding
    SPLASH_PHASE_DONE,
} splash_phase_t;

typedef struct {
    lv_obj_t      *screen;
    lv_obj_t      *version_label;  // revealed once the logo intro completes (NULL if no version)
    lv_obj_t      *partner_band;   // transparent full-screen container, NULL if no partner
    lv_timer_t    *timer;
    lv_group_t    *group;
    splash_phase_t phase;
    bool           dismissible;
    bool           show_partner;
    bool           emitted;
    uint32_t       phase_start;    // lv_tick at phase entry
    uint32_t       intro_ms;       // logo fade/slide-in duration (0 = nothing to animate)
    uint32_t       hold_logo_ms;
    uint32_t       hold_final_ms;
} splash_ctx_t;

static void splash_emit_complete(splash_ctx_t *ctx) {
    if (ctx->emitted) return;
    ctx->emitted = true;
    ctx->phase = SPLASH_PHASE_DONE;
    if (ctx->timer) { lv_timer_del(ctx->timer); ctx->timer = NULL; }
    seedsigner_lvgl_on_button_selected(SEEDSIGNER_RET_SPLASH_COMPLETE, "splash_complete");
}

static void splash_logo_opa_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_MAIN);
}

// Animate the logo's vertical position (center-relative offset). Used for the
// MicroPython partner handoff: the held boot logo is centered, so slide it up to
// the partner-band offset rather than letting it jump.
static void splash_logo_y_cb(void *obj, int32_t v) {
    lv_obj_align((lv_obj_t *)obj, LV_ALIGN_CENTER, 0, v);
}

static void splash_timer_cb(lv_timer_t *timer) {
    splash_ctx_t *ctx = (splash_ctx_t *)lv_timer_get_user_data(timer);

    // Early dismiss on touch: poll pointer devices directly (like the screensaver).
    if (ctx->dismissible) {
        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER &&
                lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
                splash_emit_complete(ctx);
                return;
            }
        }
    }

    uint32_t elapsed = lv_tick_get() - ctx->phase_start;

    switch (ctx->phase) {
    case SPLASH_PHASE_INTRO:
        // Logo fading and/or sliding into position. Once it settles, bring in the
        // version text — Python draws the version only after the logo fade.
        if (elapsed >= ctx->intro_ms) {
            if (ctx->version_label) lv_obj_remove_flag(ctx->version_label, LV_OBJ_FLAG_HIDDEN);
            ctx->phase = SPLASH_PHASE_LOGO;
            ctx->phase_start = lv_tick_get();
        }
        break;
    case SPLASH_PHASE_LOGO: {
        // Version visible; hold a beat, then reveal the partner band (if any).
        uint32_t hold = ctx->show_partner ? ctx->hold_logo_ms : ctx->hold_final_ms;
        if (elapsed >= hold) {
            if (ctx->show_partner && ctx->partner_band) {
                lv_obj_remove_flag(ctx->partner_band, LV_OBJ_FLAG_HIDDEN);
                ctx->phase = SPLASH_PHASE_PARTNER;
                ctx->phase_start = lv_tick_get();
            } else {
                splash_emit_complete(ctx);
            }
        }
        break;
    }
    case SPLASH_PHASE_PARTNER:
        if (elapsed >= ctx->hold_final_ms) splash_emit_complete(ctx);
        break;
    case SPLASH_PHASE_DONE:
    default:
        break;
    }
}

static void splash_key_handler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    splash_ctx_t *ctx = (splash_ctx_t *)lv_event_get_user_data(e);
    if (ctx && ctx->dismissible) splash_emit_complete(ctx);
}

static void splash_cleanup_handler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    splash_ctx_t *ctx = (splash_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->timer) { lv_timer_del(ctx->timer); ctx->timer = NULL; }
    if (ctx->group) { lv_group_del(ctx->group); ctx->group = NULL; }
    lv_free(ctx);
}

void splash_screen(void *ctx_json) {
    // Defaults reproduce the English opening splash; a caller overrides only the
    // keys it cares about (RFC 7396 merge-patch), like main_menu_screen.
    json cfg = {
        {"version", ""},
        {"show_partner_logos", false},
        {"sponsor_text", "With support from:"},
        {"logo_already_shown", false},
        {"boot_logo_only", false},
        {"dismissible", true},
    };
    const char *json_str = (const char *)ctx_json;
    if (json_str) {
        json incoming;
        try { incoming = json::parse(json_str); }
        catch (...) { throw std::runtime_error("invalid JSON syntax"); }
        if (!incoming.is_object()) throw std::runtime_error("screen config must be a JSON object");
        cfg.merge_patch(incoming);
    }

    const std::string version      = cfg.value("version", std::string(""));
    const std::string sponsor_text = cfg.value("sponsor_text", std::string("With support from:"));
    const bool show_partner        = cfg.value("show_partner_logos", false);
    const bool logo_already_shown  = cfg.value("logo_already_shown", false);
    const bool boot_logo_only      = cfg.value("boot_logo_only", false);
    const bool dismissible         = cfg.value("dismissible", true);

    uint32_t fade_in_ms = 1200, slide_in_ms = 600, hold_logo_ms = 1000, hold_final_ms = 2000;
    if (cfg.contains("durations") && cfg["durations"].is_object()) {
        const auto &d = cfg["durations"];
        fade_in_ms    = d.value("fade_in_ms", fade_in_ms);
        slide_in_ms   = d.value("slide_in_ms", slide_in_ms);
        hold_logo_ms  = d.value("hold_logo_ms", hold_logo_ms);
        hold_final_ms = d.value("hold_final_ms", hold_final_ms);
    }

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t screen_h = lv_display_get_vertical_resolution(NULL);
    const int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
    const int32_t px_mult  = active_profile().px_multiplier;

    // Centered logo, shifted up to make room for the partner band when shown.
    // (-56 is the Python offset at the 240px reference; scale it by px_multiplier.)
    const lv_image_dsc_t *logo = seedsigner_logo_for_active_profile();
    const int32_t logo_w = (int32_t)logo->header.w;
    const int32_t logo_h = (int32_t)logo->header.h;
    const int32_t logo_offset_y = (show_partner && !boot_logo_only) ? -(56 * px_mult / 100) : 0;

    lv_obj_t *logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, logo);
    lv_obj_set_size(logo_img, logo_w, logo_h);
    lv_obj_align(logo_img, LV_ALIGN_CENTER, 0, logo_offset_y);

    // Boot-logo preview: only the centered logo (matches the firmware C-boot
    // logo's position). No version, partner band, animation, or completion.
    if (boot_logo_only) {
        load_screen_and_cleanup_previous(scr);
        return;
    }

    // Version label (accent color), COMPONENT_PADDING below the logo's bottom.
    // Mirrors Python: version_y = canvas_h/2 + logo_h/2 + logo_offset_y + CP, drawn
    // top-anchored; subtract text_top_leading so the visible text lands like PIL.
    lv_obj_t *version_label = NULL;
    if (!version.empty()) {
        version_label = lv_label_create(scr);
        lv_label_set_text(version_label, version.c_str());
        lv_obj_set_style_text_font(version_label, &TOP_NAV_TITLE_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(version_label, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN);
        const int32_t version_top = screen_h / 2 + logo_h / 2 + logo_offset_y + COMPONENT_PADDING;
        const int32_t lead = text_top_leading(&TOP_NAV_TITLE_FONT, version.c_str());
        lv_obj_align(version_label, LV_ALIGN_TOP_MID, 0, version_top - lead);
    }

    // Partner band: "With support from:" + HRF logo, pinned to the bottom.
    // Built in a transparent full-screen container so the whole band reveals as a
    // unit. Layout from the bottom up (mirrors Python): CP bottom margin, HRF logo,
    // CP/2 gap, sponsor text.
    lv_obj_t *partner_band = NULL;
    if (show_partner) {
        const lv_image_dsc_t *hrf = hrf_logo_for_active_profile();
        const int32_t hrf_h = (int32_t)hrf->header.h;

        partner_band = lv_obj_create(scr);
        lv_obj_remove_style_all(partner_band);
        lv_obj_set_size(partner_band, screen_w, screen_h);
        lv_obj_set_pos(partner_band, 0, 0);
        lv_obj_remove_flag(partner_band, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

        lv_obj_t *hrf_img = lv_image_create(partner_band);
        lv_image_set_src(hrf_img, hrf);
        lv_obj_set_size(hrf_img, (int32_t)hrf->header.w, hrf_h);
        const int32_t hrf_y = screen_h - COMPONENT_PADDING - hrf_h;
        lv_obj_align(hrf_img, LV_ALIGN_TOP_MID, 0, hrf_y);

        lv_obj_t *sponsor = lv_label_create(partner_band);
        lv_label_set_text(sponsor, sponsor_text.c_str());
        lv_obj_set_style_text_font(sponsor, &BODY_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(sponsor, lv_color_hex(0xcccccc), LV_PART_MAIN);
        const int32_t line_h = lv_font_get_line_height(&BODY_FONT);
        lv_obj_align(sponsor, LV_ALIGN_TOP_MID, 0, hrf_y - (COMPONENT_PADDING / 2) - line_h);
    }

    // Static render (screenshot generator): the final frame at full opacity, no
    // animation or timing — exactly what a single still should capture.
    if (g_static_render) {
        load_screen_and_cleanup_previous(scr);
        return;
    }

    // Live entrance — unified "center, then up": the logo always enters CENTERED
    // (CPython fades it in there; MicroPython's held C-boot logo is already there),
    // then, when a partner band is shown, it slides UP into its raised slot. The
    // version and partner band stay hidden until the logo settles — Python brings
    // the version in only after the logo finishes, and the band follows a beat
    // later. (This diverges from Python, which fades the logo in already-raised:
    // the slide gives both platforms one consistent motion and makes the
    // boot->splash handoff seamless on the MCU.)
    if (version_label) lv_obj_add_flag(version_label, LV_OBJ_FLAG_HIDDEN);
    if (partner_band)  lv_obj_add_flag(partner_band,  LV_OBJ_FLAG_HIDDEN);

    lv_obj_align(logo_img, LV_ALIGN_CENTER, 0, 0);  // enter centered (where the boot logo sits)

    const uint32_t fade_ms  = logo_already_shown ? 0u : fade_in_ms;     // no fade if already on screen
    const uint32_t slide_ms = (logo_offset_y != 0) ? slide_in_ms : 0u;  // slide only when a band raises it

    if (fade_ms > 0) {
        lv_obj_set_style_opa(logo_img, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_anim_t fade;
        lv_anim_init(&fade);
        lv_anim_set_var(&fade, logo_img);
        lv_anim_set_exec_cb(&fade, splash_logo_opa_cb);
        lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&fade, fade_ms);
        lv_anim_set_path_cb(&fade, lv_anim_path_ease_out);
        lv_anim_start(&fade);
    }
    if (slide_ms > 0) {
        lv_anim_t slide;
        lv_anim_init(&slide);
        lv_anim_set_var(&slide, logo_img);
        lv_anim_set_exec_cb(&slide, splash_logo_y_cb);
        lv_anim_set_values(&slide, 0, logo_offset_y);
        lv_anim_set_duration(&slide, slide_ms);
        lv_anim_set_delay(&slide, fade_ms);  // slide begins once the fade (if any) finishes
        lv_anim_set_path_cb(&slide, lv_anim_path_ease_in_out);
        lv_anim_start(&slide);
    }

    splash_ctx_t *ctx = (splash_ctx_t *)lv_malloc(sizeof(splash_ctx_t));
    lv_memzero(ctx, sizeof(*ctx));
    ctx->screen        = scr;
    ctx->version_label = version_label;
    ctx->partner_band  = partner_band;
    ctx->phase         = SPLASH_PHASE_INTRO;
    ctx->dismissible   = dismissible;
    ctx->show_partner  = show_partner;
    ctx->intro_ms      = fade_ms + slide_ms;
    ctx->hold_logo_ms  = hold_logo_ms;
    ctx->hold_final_ms = hold_final_ms;
    ctx->phase_start   = lv_tick_get();
    ctx->timer = lv_timer_create(splash_timer_cb, 50, ctx);

    // Keypad sink: any key press dismisses (hardware input mode), like the screensaver.
    if (dismissible && input_profile_get_mode() == INPUT_MODE_HARDWARE) {
        lv_obj_t *sink = lv_obj_create(scr);
        lv_obj_set_size(sink, 1, 1);
        lv_obj_set_pos(sink, 0, 0);
        lv_obj_set_style_opa(sink, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sink, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(sink, 0, LV_PART_MAIN);
        lv_obj_set_style_outline_width(sink, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(sink, 0, LV_PART_MAIN);
        lv_obj_remove_flag(sink, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

        ctx->group = lv_group_create();
        lv_group_add_obj(ctx->group, sink);
        lv_obj_add_event_cb(sink, splash_key_handler, LV_EVENT_KEY, ctx);

        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD ||
                lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
                lv_indev_set_group(indev, ctx->group);
            }
        }
    }

    lv_obj_add_event_cb(scr, splash_cleanup_handler, LV_EVENT_DELETE, ctx);
    load_screen_and_cleanup_previous(scr);
}


void lv_seedsigner_screen_close(void)
{
    /*Delete all animation*/
    lv_anim_del(NULL, NULL);

    // lv_timer_del(meter2_timer);
    // meter2_timer = NULL;

    lv_obj_clean(lv_scr_act());

    // lv_style_reset(&style_text_muted);
    // lv_style_reset(&style_title);
    // lv_style_reset(&style_icon);
    // lv_style_reset(&style_bullet);
}


