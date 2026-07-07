#include "seedsigner.h"
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
#include "locale_picker.h"   // endonym-image rows for locale_picker_screen
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
        } else if (entry.is_object() && entry.contains("label") && entry["label"].is_string()) {
            // Object form (per-button icons) — the label is all this reader needs.
            out.push_back(entry["label"].get<std::string>());
        } else {
            throw std::runtime_error("button_list entries must be string, array, or object with a string label");
        }
    }
    return true;
}

// Parse a JSON color string ("#RRGGBB", "0xRRGGBB", or "RRGGBB") to a 0xRRGGBB int.
// Throws on a malformed value so bad cfg fails fast (same convention as the rest of
// the screen-config parser).
static uint32_t parse_hex_color(const std::string &s) {
    std::string h = s;
    if (h.rfind("#", 0) == 0) {
        h = h.substr(1);
    } else if (h.rfind("0x", 0) == 0 || h.rfind("0X", 0) == 0) {
        h = h.substr(2);
    }
    if (h.size() != 6) {
        throw std::runtime_error("color must be a 6-digit hex string like \"#30D158\"");
    }
    for (char c : h) {
        if (!std::isxdigit((unsigned char)c)) {
            throw std::runtime_error("color must be a 6-digit hex string like \"#30D158\"");
        }
    }
    return (uint32_t)std::strtoul(h.c_str(), nullptr, 16);
}

// Richer button_list reader: parallels read_button_list_labels but also captures the
// per-item inline icon / right icon / icon color when an entry is an OBJECT. Accepts:
//   - "Label"                                                  → label only
//   - ["Label", ...]                                           → label at index 0
//   - {"label","icon"?,"icon_color"?,"right_icon"?}            → label + inline icons
// Returns true if "button_list" was present and well-formed; false (out cleared) if
// missing. Throws on a malformed entry so screens fail fast.
struct button_item_cfg_t {
    std::string label;
    std::string icon;        // empty = none
    std::string right_icon;  // empty = none
    uint32_t    icon_color = SEEDSIGNER_ICON_COLOR_DEFAULT;
    uint32_t    label_color = SEEDSIGNER_ICON_COLOR_DEFAULT;
    bool        is_checked = false;  // set from cfg["checked_buttons"] by the scaffold
};

static bool read_button_list_items(const json &cfg, std::vector<button_item_cfg_t> &out) {
    out.clear();
    if (!cfg.contains("button_list")) {
        return false;
    }
    if (!cfg["button_list"].is_array()) {
        throw std::runtime_error("button_list must be an array");
    }

    for (const auto &entry : cfg["button_list"]) {
        button_item_cfg_t item;
        if (entry.is_string()) {
            item.label = entry.get<std::string>();
        } else if (entry.is_array() && !entry.empty() && entry[0].is_string()) {
            item.label = entry[0].get<std::string>();
        } else if (entry.is_object()) {
            if (!entry.contains("label") || !entry["label"].is_string()) {
                throw std::runtime_error("button_list object entry requires a string \"label\"");
            }
            item.label = entry["label"].get<std::string>();
            if (entry.contains("icon")) {
                if (!entry["icon"].is_string()) {
                    throw std::runtime_error("button_list \"icon\" must be a string");
                }
                item.icon = entry["icon"].get<std::string>();
            }
            if (entry.contains("right_icon")) {
                if (!entry["right_icon"].is_string()) {
                    throw std::runtime_error("button_list \"right_icon\" must be a string");
                }
                item.right_icon = entry["right_icon"].get<std::string>();
            }
            if (entry.contains("icon_color")) {
                if (!entry["icon_color"].is_string()) {
                    throw std::runtime_error("button_list \"icon_color\" must be a string");
                }
                item.icon_color = parse_hex_color(entry["icon_color"].get<std::string>());
            }
            if (entry.contains("label_color")) {
                if (!entry["label_color"].is_string()) {
                    throw std::runtime_error("button_list \"label_color\" must be a string");
                }
                item.label_color = parse_hex_color(entry["label_color"].get<std::string>());
            }
        } else {
            throw std::runtime_error("button_list entries must be a string, array, or object with a string \"label\"");
        }
        out.push_back(item);
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


// Forward decl: tight body line-spacing helper (defined after tight_line_space).
static void apply_body_tight_line_spacing(lv_obj_t *label);

// Create a standard wrapped body-text label in `parent`: WRAP, fixed `width`,
// centered, BODY_FONT in BODY_FONT_COLOR. Shared by the button_list_screen intro
// text and the status-screen body (which then layer on tight line spacing via
// apply_body_tight_line_spacing(), plus any inset width / centering). Caller owns
// any further styling.
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
    // body font/color, with the SAME tight, ink-based line spacing as the
    // large_icon_status_screen body (not the screen-wide loose default) — otherwise
    // the taller block can tip a short prompt into a marginal overflow that wrongly
    // trips scroll-then-buttons, leaving no button highlighted on load.
    if (cfg.contains("text") && cfg["text"].is_string()) {
        std::string text = cfg["text"].get<std::string>();
        if (!text.empty() && screen.upper_body && screen.upper_body != screen.body) {
            lv_obj_t *intro_label = make_body_text_label(
                screen.upper_body, text.c_str(),
                lv_obj_get_content_width(screen.upper_body));
            apply_body_tight_line_spacing(intro_label);
        }
    }

    bind_screen_navigation(
        cfg,
        screen,
        screen.button_list_count > 0 ? screen.button_list : NULL,
        screen.button_list_count,
        NAV_BODY_VERTICAL,
        // Default to button index 0 selected when nothing is passed in (an explicit
        // initial_selected_index still overrides). A concrete index keeps a button
        // active even when intro text makes the list overflow — a menu/list always
        // has a selection. (cf. status screens, which pass NAV_INDEX_NONE.)
        0
    );

    load_screen_and_cleanup_previous(screen.screen);
}


// ---------------------------------------------------------------------------
// locale_picker_screen
// ---------------------------------------------------------------------------
//
// The language-selection screen (SETTING__LOCALE). Unlike a generic
// button_list_screen it must show every onboard language's name in its OWN native
// script on one screen — which the native path cannot do as live text (one active
// locale, one set of role fonts, and keeping N script fonts resident would blow
// the ESP32 glyph-cache pool). So each row is EITHER:
//   - live text (the always-resident baseline font) — for Latin-script endonyms
//     covered by the baked floor (English, Español, …); OR
//   - a pre-rendered A8 endonym image (native script, zero runtime font) — for
//     every language that ships a font pack (CJK, Arabic/Persian, Devanagari, …).
// A row is an image row iff its cfg carries an "image" filename; the host fetches
// that blob through the endonym image provider (locale_picker_set_image_provider,
// the same seam as ss_load_locale). Otherwise it is a live text row.
//
// Selection uses the standard body-button result path: clicking / entering a row
// fires seedsigner_lvgl_on_button_selected(row_index, ...); the host maps the
// index back to the locale it placed at that position (and persists / re-renders).
//
// Each row is a SINGLE line: the English name, then the native name — live text
// for a Latin-script native (e.g. "Spanish  Español"), or the pre-rendered image
// drawn right after the English text for a non-Latin script (e.g. "Hindi  हिन्दी").
// A row that overflows scrolls like any button-list row. The host orders the rows
// (English pinned first, the rest alphabetical by English name).
//
// cfg:
//   { "top_nav": {"title": "...", "show_back_button": true},
//     "active_locale": "<code>",              // radio-checked + initially focused
//     "rows": [ {"locale":"en","english":"English","native":"English"},            // live
//               {"locale":"es","english":"Spanish","native":"Español"},            // live
//               {"locale":"hi","english":"Hindi","native":"हिन्दी","image":true}, // image
//               ... ] }
void locale_picker_screen(void *ctx_json)
{
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);
    if (!cfg.contains("rows") || !cfg["rows"].is_array()) {
        throw std::runtime_error("locale_picker_screen: \"rows\" is required and must be an array");
    }
    const json &rows = cfg["rows"];
    if (rows.size() > SEEDSIGNER_SCAFFOLD_MAX_BUTTONS) {
        throw std::runtime_error("locale_picker_screen: rows exceed SEEDSIGNER_SCAFFOLD_MAX_BUTTONS");
    }

    const std::string active_locale = cfg.value("active_locale", std::string());

    // No "button_list" key ⇒ scaffold Mode 1: top_nav + a plain scrollable body
    // (upper_body == body). We build the row buttons into that body ourselves so
    // each can host live text OR an endonym image. A pure list, so navigation uses
    // item-focus scrolling (no scroll-then-buttons hand-off).
    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, true);

    std::vector<lv_obj_t *> items;
    items.reserve(rows.size());
    lv_obj_t *prev = nullptr;
    size_t active_index = 0;

    for (size_t i = 0; i < rows.size(); ++i) {
        const json &row = rows[i];
        if (!row.is_object() || !row.contains("locale") || !row["locale"].is_string()) {
            throw std::runtime_error("locale_picker_screen: each row needs a string \"locale\"");
        }
        const std::string locale  = row["locale"].get<std::string>();
        const std::string english = row.value("english", std::string());
        const std::string native  = row.value("native",  std::string());
        const bool is_active = (!active_locale.empty() && locale == active_locale);

        // Non-Latin natives ship a pre-rendered image; name it explicitly
        // ("image":"endonym_480.bin") or let the screen derive it for the active
        // profile height ("image":true → endonym_<height>.bin, so one scenario
        // renders at every resolution).
        std::string image_file;
        if (row.contains("image")) {
            if (row["image"].is_string()) {
                image_file = row["image"].get<std::string>();
            } else if (row["image"].is_boolean() && row["image"].get<bool>()) {
                image_file = "endonym_" + std::to_string(active_profile().height) + ".bin";
            }
        }

        // Row text = English name, a "|" separator, then the native name on the same
        // line. A non-Latin native is drawn as its image just after the separator, so
        // its label ends at "English |". A Latin native is appended as live text.
        // English itself (native == English) shows once, with no separator.
        const std::string primary = !english.empty() ? english : native;
        std::string label_text = primary;
        if (!image_file.empty()) {
            label_text = primary + " |";                 // native image drawn after the pipe
        } else if (!native.empty() && native != primary) {
            label_text = primary + " | " + native;       // Latin native as live text
        }

        // Radio row: the current locale is CHECK-marked; every row left-aligned and
        // chained below the previous (the same geometry button_list() produces).
        button_opts_t opts = {};
        opts.text             = label_text.c_str();
        opts.align_to         = prev;
        opts.is_text_centered = false;
        opts.icon_color       = SEEDSIGNER_ICON_COLOR_DEFAULT;
        opts.label_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;
        opts.style            = BUTTON_STYLE_CHECKED_SELECTION;
        opts.is_checked       = is_active;
        lv_obj_t *btn = button_ex(screen.body, &opts);

        // Image row (native-script name, no runtime font). On any failure the row
        // still shows its English name: fail soft, never crash on a bad pack image.
        if (!image_file.empty()) {
            locale_picker_attach_endonym(btn, locale.c_str(), image_file.c_str());
        }

        items.push_back(btn);
        prev = btn;
        if (is_active) active_index = i;
    }

    // Focus (and scroll to) the current locale's row by default; an explicit
    // cfg.initial_selected_index still overrides via nav_initial_index_from_cfg.
    bind_screen_navigation(
        cfg, screen,
        items.empty() ? nullptr : items.data(), items.size(),
        NAV_BODY_VERTICAL, active_index);

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

enum class status_type_t { SUCCESS, WARNING, DIRE_WARNING, ERROR, CUSTOM };

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
        case status_type_t::CUSTOM:
            // The icon glyph, color, and title all come from cfg (filled in by
            // large_icon_status_screen); these are only fallbacks for a bare cfg. No
            // forced title (default ""), a plain "OK" button, no warning border, and the
            // 1x text inset of a non-warning screen.
            return { SeedSignerIconConstants::INFO, INFO_COLOR,
                     "",           "OK",            false, 1 };
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
    // "custom": the caller supplies the icon glyph + color at call time (Python's
    // PSBTFinalizeScreen's SIGN icon, the microSD-notification screen, etc.), so this
    // one screen covers any large-icon prompt without a bespoke entry point.
    if (value == "custom")       return status_type_t::CUSTOM;
    throw std::runtime_error("status_type must be one of \"success\", "
                             "\"warning\", \"dire_warning\", \"error\", \"custom\"");
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

// Apply the status-screen body's tight, ink-based inter-line spacing to an already-
// created body-text label: zero top margin + a per-line advance derived from THIS
// label's real glyph ink (max ascender + max descender + a small profile-scaled
// gap) via tight_line_space(), rather than the font's loose declared line_height.
//
// Shared by the large_icon_status_screen body and the button_list_screen intro
// text so both render identically and match the PIL reference. Measures the label's
// STORED text so Arabic/Persian presentation forms (what's actually drawn) are what
// we measure. Without this the intro text inherits the screen-wide (loose)
// BODY_LINE_SPACING, whose taller block can tip a short prompt into a marginal
// overflow — wrongly tripping scroll-then-buttons so no button is highlighted on
// load.
static void apply_body_tight_line_spacing(lv_obj_t *label) {
    if (!label) {
        return;
    }
    // Python places the body immediately after the headline (no extra gap) — so no
    // top margin; a margin made the headline->body gap visibly looser than Python.
    lv_obj_set_style_margin_top(label, 0, LV_PART_MAIN);

    int32_t line_gap = LIST_ITEM_PADDING / 2;
    lv_obj_set_style_text_line_space(
        label,
        tight_line_space(&BODY_FONT, lv_label_get_text(label), line_gap),
        LV_PART_MAIN);
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

    // For a "custom" status screen the hero icon glyph + color are caller-supplied
    // (raw glyph string + hex color, the same convention as button/top-nav icons), so
    // one screen renders any large-icon prompt: PSBTFinalize's SIGN icon, the microSD
    // notification, etc. custom_icon backs defaults.icon (a const char*) for the render.
    std::string custom_icon;
    if (status == status_type_t::CUSTOM) {
        if (cfg.contains("icon") && cfg["icon"].is_string()) {
            custom_icon   = cfg["icon"].get<std::string>();
            defaults.icon = custom_icon.c_str();
        }
        if (cfg.contains("icon_color") && cfg["icon_color"].is_string()) {
            defaults.color = (int)parse_hex_color(cfg["icon_color"].get<std::string>());
        }
    }

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

            // Zero top margin (Python places the body immediately after the
            // headline) + tight, ink-based inter-line spacing matching the PIL
            // reference — both applied by apply_body_tight_line_spacing(), shared
            // with the button_list_screen intro text so the two render identically.
            apply_body_tight_line_spacing(body_label);

            // A custom large-icon screen with no headline (PSBTFinalize's "Click to
            // approve this transaction" under the SIGN icon) needs the text spaced off
            // the icon; Python uses icon_bottom + 2*COMPONENT_PADDING. Restore that gap
            // that apply_body_tight_line_spacing() zeroed for the headline-adjacent case.
            bool has_headline = cfg.contains("status_headline") &&
                                cfg["status_headline"].is_string() &&
                                !cfg["status_headline"].get<std::string>().empty();
            if (status == status_type_t::CUSTOM && !has_headline) {
                lv_obj_set_style_margin_top(body_label, 2 * COMPONENT_PADDING, LV_PART_MAIN);
            }
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
        // NAV_INDEX_NONE: the single OK/ack button is active when the screen FITS, but
        // when a long warning overflows the user must scroll through it before the
        // button becomes selectable (read-first). It is NOT pre-focused under overflow.
        NAV_INDEX_NONE
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

// The button-matrix map-inspection helpers (top-row count, row bounds, find
// button) now live in keyboard_core.h (kb_top_row_count / kb_row_bounds /
// kb_find_button), taking the map array directly. Passphrase passes its current
// page's map via lv_keyboard_get_map_array(kb).

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
    const char * const *map = lv_keyboard_get_map_array(kb);
    bool alphabetical = (kb_find_button(map, 'a') == 0) ||
                        (kb_find_button(map, 'A') == 0);
    switch (mode) {
        case LV_KEYBOARD_MODE_TEXT_LOWER: ch = alphabetical ? 'k' : 'g'; break;
        case LV_KEYBOARD_MODE_TEXT_UPPER: ch = alphabetical ? 'K' : 'G'; break;
        case LV_KEYBOARD_MODE_NUMBER:     ch = '6'; break;
        case LV_KEYBOARD_MODE_SPECIAL:    ch = '.'; break;
        default: break;
    }
    int idx = ch ? kb_find_button(map, ch) : -1;
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

// The side-button press-flash (kb_flash_side_button) now lives in keyboard_core.

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
        if (key == (uint32_t)'1') { kb_flash_side_button(c->key1_btn); passphrase_key1_case(c); return; }
        if (key == (uint32_t)'2') { kb_flash_side_button(c->key2_btn); passphrase_key2_cycle(c); return; }
        if (key == (uint32_t)'3') {
            kb_flash_side_button(c->key3_btn);
            if (c->ta && lv_obj_is_valid(c->ta)) {
                seedsigner_lvgl_on_text_entered(lv_textarea_get_text(c->ta));
            }
            return;
        }
    }

    // The remaining directional keys (UP top-row → back-button handoff, LEFT/RIGHT
    // row-wrap) are the generic keyboard navigation, shared via keyboard_core.
    kb_handle_directional(e, lv_keyboard_get_map_array(c->kb), c->kb, c->back_btn);
}

// Hardware-mode key filter on the back button: DOWN returns focus to the
// keyboard.
static void passphrase_back_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    passphrase_ctx_t *c = (passphrase_ctx_t *)lv_event_get_user_data(e);
    if (c) kb_back_down_to_matrix(e, c->kb);
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

// The side-panel button factory (kb_side_button) now lives in keyboard_core.

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

// The keyboard theming + per-key SeedSigner icon recolor (kb_style_matrix and
// its draw callback) now live in keyboard_core.

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

    // Text-entry strip: the shared one-line, cursor-styled SeedSigner box.
    lv_obj_t *ta = kb_make_text_entry(body, main_w, g_static_render);

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
    kb_style_matrix(kb, &KEYBOARD_FONT);

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
        c->key1_btn = kb_side_button(body, px, center_y - spacing, panel_w, btn_h,
                               UPPER_LABEL, &KEYBOARD_FONT, BUTTON_FONT_COLOR, clipped, &c->key1_label);
        c->key2_btn = kb_side_button(body, px, center_y, panel_w, btn_h,
                               NUM_LABEL, &KEYBOARD_FONT, BUTTON_FONT_COLOR, clipped, &c->key2_label);
        c->key3_btn = kb_side_button(body, px, center_y + spacing, panel_w, btn_h,
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
        kb_connect_indevs(c->group);

        // Pre-select an initial key so the joystick selection is visible from the
        // start. Otherwise btn_id_sel is NONE and it takes an arrow press just to
        // "enter" the keyboard, with no visible cursor until then. Prefer the
        // last-typed key (prefilled, e.g. the "i" of satoshi), else the page's
        // central default key (k / 6). The highlight shows via the focused-key
        // style in live use; static-render (screenshots) has no indev to apply
        // that focus state, so add PRESSED to make the highlight show in the still.
        int sel = initial_text.empty() ? -1 : kb_find_button(lv_keyboard_get_map_array(kb), initial_text.back());
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


// ---------------------------------------------------------------------------
// keyboard_screen
// ---------------------------------------------------------------------------
//
// Generalized single-charset text/char entry (the LVGL port of Python's
// KeyboardScreen). Unlike the passphrase screen there is one static page with no
// mode switching, so this is a plain lv_buttonmatrix (built on keyboard_core for
// the text-entry box, key theming, and joystick nav). Consumers: dice-roll /
// coin-flip entropy, BIP-85 child index, custom derivation path.
//
// Because the native screen owns the input loop and only returns the final
// string, two things that live in Python's KeyboardScreen move native here: the
// per-keystroke title (via `title_keystroke_template`) and `return_after_n_chars`
// auto-completion.
//
// cfg:
//   top_nav: { title, show_back_button, show_power_button }
//   cols (int, default 10)         grid columns; rows derive from the key count
//   keys (array of strings)        the value keys, one label per cell, row-major
//   keys_to_values (object)        optional label->emitted-value map (e.g. a dice
//                                  glyph -> "1"); absent => the label IS the value
//   keyboard_font ("fixed"|"icon") key glyph font (default "fixed" = KEYBOARD_FONT;
//                                  "icon"/"fontawesome" = ICON_FONT__SEEDSIGNER)
//   return_after_n_chars (int)     auto-return once this many chars are entered
//   show_save_button (bool)        append an in-grid green CHECK confirm key
//   initial_value (string)         prefill the text entry
//   title_keystroke_template (str) e.g. "Dice Roll {n}/{total}"; {n}=next entry
//                                  index, {total}=return_after_n_chars; live-updated
//   max_length (int)               optional cap on entered length
//
// A DELETE (backspace) key is always appended; CHECK is appended when
// show_save_button. Completion routes through seedsigner_lvgl_on_text_entered(),
// the same host hook the passphrase screen uses.

// Button-matrix control entries for keyboard_screen. KSW(n): a plain value key of
// relative width n. KSC(n): a control key (DELETE/CHECK) — marked CHECKED so
// kb_style_matrix paints it as a control + the icon draw-cb recolors it, plus
// NO_REPEAT/CLICK_TRIG so a hold doesn't auto-repeat.
#define KSW(n) ((lv_buttonmatrix_ctrl_t)(n))
#define KSC(n) ((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_CHECKED | (n)))

// Per-screen state. C++ (vectors/strings/map), so new/delete rather than
// lv_malloc; freed in keyboard_cleanup_cb. The text-entry box (c->ta) is the
// source of truth for the entered string — control keys edit it at the cursor.
struct keyboard_screen_ctx_t {
    lv_obj_t   *matrix = nullptr;
    lv_obj_t   *ta = nullptr;
    lv_obj_t   *back_btn = nullptr;
    lv_obj_t   *top_nav = nullptr;       // for re-laying-out the title on each update
    lv_obj_t   *title_label = nullptr;
    bool        title_has_power = false; // power button present (for title centering)
    lv_group_t *group = nullptr;

    std::vector<std::string>           key_storage;  // backs the value-key char*s
    std::vector<const char *>          map;          // buttonmatrix map (persistent)
    std::vector<lv_buttonmatrix_ctrl_t> ctrl;        // one entry per button
    std::map<std::string, std::string> values;       // label -> emitted value

    std::string title_template;
    int  return_after = 0;   // 0 = no auto-return
    int  total = 0;          // {total} in the title template
};

// Substitute {n} / {total} in a title template.
static std::string keyboard_render_title(const std::string &tmpl, int n, int total) {
    std::string out = tmpl;
    auto rep = [&](const char *key, int v) {
        std::string s = std::to_string(v);
        size_t p;
        while ((p = out.find(key)) != std::string::npos) out.replace(p, std::strlen(key), s);
    };
    rep("{n}", n);
    rep("{total}", total);
    return out;
}

// Refresh the top-nav title from the template: {n} is the index of the entry the
// user is about to make (chars entered + 1, clamped to total).
static void keyboard_update_title(keyboard_screen_ctx_t *c) {
    if (c->title_template.empty() || !c->title_label || !lv_obj_is_valid(c->title_label)) return;
    int len = (c->ta && lv_obj_is_valid(c->ta)) ? (int)std::strlen(lv_textarea_get_text(c->ta)) : 0;
    int n = len + 1;
    if (c->total > 0 && n > c->total) n = c->total;
    lv_label_set_text(c->title_label,
                      keyboard_render_title(c->title_template, n, c->total).c_str());
    // The counter changes width as it grows, so re-run the top-nav title staging
    // (center → left-pin → scroll) against the new text instead of marquee-scrolling
    // within the slice top_nav measured for the initial value.
    top_nav_layout_title(c->top_nav, c->title_label, c->back_btn != nullptr,
                         c->title_has_power, nullptr);
}

static void keyboard_complete(keyboard_screen_ctx_t *c) {
    if (c->ta && lv_obj_is_valid(c->ta)) {
        seedsigner_lvgl_on_text_entered(lv_textarea_get_text(c->ta));
    }
}

// Buttonmatrix click handler (both input modes). The control glyphs act on the
// text-entry box directly so the cursor position is honored; any other key inserts
// its (possibly mapped) value at the cursor.
static void keyboard_value_changed_cb(lv_event_t *e) {
    lv_obj_t *m = lv_event_get_target_obj(e);
    keyboard_screen_ctx_t *c = (keyboard_screen_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    lv_obj_t *ta = c->ta;
    if (!ta || !lv_obj_is_valid(ta)) return;

    uint32_t id = lv_buttonmatrix_get_selected_button(m);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *txt = lv_buttonmatrix_get_button_text(m, id);
    if (!txt) return;

    if (std::strcmp(txt, SeedSignerIconConstants::DELETE) == 0)        { lv_textarea_delete_char(ta);  keyboard_update_title(c); return; }
    if (std::strcmp(txt, SeedSignerIconConstants::CHEVRON_LEFT) == 0)  { lv_textarea_cursor_left(ta);  return; }
    if (std::strcmp(txt, SeedSignerIconConstants::CHEVRON_RIGHT) == 0) { lv_textarea_cursor_right(ta); return; }
    if (std::strcmp(txt, SeedSignerIconConstants::CHECK) == 0) {
        if (std::strlen(lv_textarea_get_text(ta)) > 0) keyboard_complete(c);  // don't submit empty (Python parity)
        return;
    }

    // A value key: insert its mapped value (or the label) at the cursor. The
    // textarea enforces max_length natively.
    auto it = c->values.find(txt);
    lv_textarea_add_text(ta, (it != c->values.end()) ? it->second.c_str() : txt);
    keyboard_update_title(c);

    if (c->return_after > 0 && (int)std::strlen(lv_textarea_get_text(ta)) >= c->return_after) {
        keyboard_complete(c);
    }
}

// Hardware key filter: the generic top-nav handoff + row-wrap (no aux keys — the
// confirm/backspace are in-grid and joystick-navigable).
static void keyboard_kb_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    keyboard_screen_ctx_t *c = (keyboard_screen_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    kb_handle_directional(e, c->map.data(), c->matrix, c->back_btn);
}

static void keyboard_back_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    keyboard_screen_ctx_t *c = (keyboard_screen_ctx_t *)lv_event_get_user_data(e);
    if (c) kb_back_down_to_matrix(e, c->matrix);
}

static void keyboard_cleanup_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    keyboard_screen_ctx_t *c = (keyboard_screen_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    if (c->group) lv_group_del(c->group);
    delete c;
}

void keyboard_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Input mode selects the joystick group wiring (touch needs none).
    bool has_mode_override = false;
    input_mode_t mode_override = INPUT_MODE_TOUCH;
    nav_mode_override_from_cfg(cfg, has_mode_override, mode_override);
    bool hardware = (has_mode_override ? mode_override : input_profile_get_mode()) == INPUT_MODE_HARDWARE;

    // No button_list: the body is a custom textarea + keyboard. upper_body == body.
    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);
    lv_obj_t *body = screen.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

    const int32_t content_w = lv_obj_get_content_width(body);
    const int32_t content_h = lv_obj_get_content_height(body);

    keyboard_screen_ctx_t *c = new keyboard_screen_ctx_t();
    c->back_btn = screen.top_back_btn;
    c->top_nav = screen.top_nav;
    c->title_label = screen.title_label;
    c->title_has_power = (screen.top_power_btn != nullptr);

    int cols = 10;
    if (cfg.contains("cols") && cfg["cols"].is_number_integer()) cols = cfg["cols"].get<int>();
    if (cols < 1) cols = 1;

    if (!cfg.contains("keys") || !cfg["keys"].is_array() || cfg["keys"].empty()) {
        throw std::runtime_error("keyboard_screen requires a non-empty \"keys\" array");
    }
    c->key_storage.reserve(cfg["keys"].size());
    for (const auto &k : cfg["keys"]) {
        if (!k.is_string()) throw std::runtime_error("keyboard_screen \"keys\" entries must be strings");
        c->key_storage.push_back(k.get<std::string>());
    }

    if (cfg.contains("keys_to_values") && cfg["keys_to_values"].is_object()) {
        for (auto it = cfg["keys_to_values"].begin(); it != cfg["keys_to_values"].end(); ++it) {
            if (it.value().is_string()) c->values[it.key()] = it.value().get<std::string>();
        }
    }

    if (cfg.contains("return_after_n_chars") && cfg["return_after_n_chars"].is_number_integer()) {
        c->return_after = cfg["return_after_n_chars"].get<int>();
    }
    c->total = c->return_after;
    bool show_save = cfg.value("show_save_button", false);
    bool show_cursor_keys = cfg.value("show_cursor_keys", true);
    if (cfg.contains("title_keystroke_template") && cfg["title_keystroke_template"].is_string()) {
        c->title_template = cfg["title_keystroke_template"].get<std::string>();
    }
    int max_length = 0;
    if (cfg.contains("max_length") && cfg["max_length"].is_number_integer()) {
        max_length = cfg["max_length"].get<int>();
    }
    std::string initial_value = cfg.value("initial_value", std::string());

    const lv_font_t *key_font = &KEYBOARD_FONT;
    if (cfg.contains("keyboard_font") && cfg["keyboard_font"].is_string()) {
        std::string kf = cfg["keyboard_font"].get<std::string>();
        if (kf == "icon" || kf == "fontawesome") key_font = &ICON_FONT__SEEDSIGNER;
    }

    // Assemble the buttonmatrix map. Value keys wrap by `cols`; the control keys
    // (cursor left/right, backspace, optional save check) go on their OWN row
    // beneath — so a sparse value grid (e.g. coin flip) doesn't stretch the
    // controls across the width. The map char*s reference c->key_storage (fully
    // populated before any .c_str() is taken, so no realloc invalidates them) and
    // static icon/literal strings; c->ctrl has one entry per button (no "\n").
    struct cell_t { const char *txt; lv_buttonmatrix_ctrl_t ctrl; };
    std::vector<cell_t> controls;
    if (show_cursor_keys) {
        controls.push_back({SeedSignerIconConstants::CHEVRON_LEFT,  KSC(1)});
        controls.push_back({SeedSignerIconConstants::CHEVRON_RIGHT, KSC(1)});
    }
    controls.push_back({SeedSignerIconConstants::DELETE, KSC(1)});       // always a backspace
    if (show_save) controls.push_back({SeedSignerIconConstants::CHECK, KSC(1)});

    c->map.clear();
    c->ctrl.clear();
    const int value_count = (int)c->key_storage.size();
    int col = 0;
    for (int i = 0; i < value_count; ++i) {
        c->map.push_back(c->key_storage[i].c_str());
        c->ctrl.push_back(KSW(1));
        if (++col == cols && i + 1 < value_count) { c->map.push_back("\n"); col = 0; }
    }
    c->map.push_back("\n");
    for (const cell_t &cc : controls) { c->map.push_back(cc.txt); c->ctrl.push_back(cc.ctrl); }
    c->map.push_back("");

    const int value_rows = (value_count + cols - 1) / cols;
    const int total_rows = value_rows + 1;  // + the control row

    // Text-entry strip: the shared cursor-styled box (the entry's source of truth).
    lv_obj_t *ta = kb_make_text_entry(body, content_w, g_static_render);
    if (!initial_value.empty()) lv_textarea_set_text(ta, initial_value.c_str());
    if (max_length > 0) lv_textarea_set_max_length(ta, (uint32_t)max_length);
    lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
    c->ta = ta;

    // Keyboard: a plain buttonmatrix styled like the passphrase keyboard.
    lv_obj_t *matrix = lv_buttonmatrix_create(body);
    lv_buttonmatrix_set_map(matrix, c->map.data());
    lv_buttonmatrix_set_ctrl_map(matrix, c->ctrl.data());
    kb_style_matrix(matrix, key_font);
    c->matrix = matrix;

    // Optional guidance text (e.g. the coin-flip "Heads = 1 / Tails = 0" legend),
    // centered below the keyboard. The caller passes it already translated; embedded
    // newlines split it into lines (Python renders these as stacked TextAreas). Its
    // height is reserved from the keyboard's vertical budget below so it never gets
    // pushed off-screen by the (capped, but still tall) keys.
    std::string guidance_text = cfg.value("guidance_text", std::string());
    int32_t guidance_h = 0;
    if (!guidance_text.empty()) {
        int lines = 1 + (int)std::count(guidance_text.begin(), guidance_text.end(), '\n');
        guidance_h = lines * (int32_t)lv_font_get_line_height(&BODY_FONT)
                     + (lines - 1) * BODY_LINE_SPACING + 2 * COMPONENT_PADDING;
    }

    // Cap the per-key size so a sparse grid stays a tidy block of comfortably large
    // targets instead of stretching to fill the screen (e.g. the lone backspace on a
    // coin-flip keyboard). Center the grid horizontally, just below the text entry.
    const int32_t kb_top  = BUTTON_HEIGHT + COMPONENT_PADDING;
    const int32_t avail_w = content_w;
    const int32_t avail_h = content_h - kb_top - guidance_h;
    const int32_t max_key = BUTTON_HEIGHT * 2;
    int32_t key_w = std::min(avail_w / cols, max_key);
    int32_t key_h = std::min(avail_h / total_rows, max_key);
    lv_obj_set_size(matrix, key_w * cols, key_h * total_rows);
    lv_obj_align(matrix, LV_ALIGN_TOP_MID, 0, kb_top);

    lv_obj_add_event_cb(matrix, keyboard_value_changed_cb, LV_EVENT_VALUE_CHANGED, c);

    if (!guidance_text.empty()) {
        lv_obj_t *guide = lv_label_create(body);
        lv_label_set_text(guide, guidance_text.c_str());
        lv_obj_set_width(guide, content_w);
        lv_obj_set_style_text_font(guide, &BODY_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(guide, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_text_align(guide, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        // Match regular body text's line-to-line spacing for the multi-line legend.
        lv_obj_set_style_text_line_space(guide, BODY_LINE_SPACING, LV_PART_MAIN);
        lv_obj_align(guide, LV_ALIGN_TOP_MID, 0, kb_top + key_h * total_rows + COMPONENT_PADDING);
    }

    // keyboard_update_title re-stages the top-nav title for the current counter value
    // (the initial call lays it out; subsequent keystrokes keep it correct as the
    // counter widens). Static titles keep top_nav's own layout.
    keyboard_update_title(c);

    if (hardware) {
        // Joystick: the matrix is the group-focused object so its own directional
        // navigation drives key selection; the back button is the other member for
        // the top-nav handoff (mirrors the passphrase screen, sans side panel).
        c->group = lv_group_create();
        lv_group_set_wrap(c->group, false);
        lv_group_add_obj(c->group, matrix);
        if (screen.top_back_btn) {
            lv_group_add_obj(c->group, screen.top_back_btn);
            lv_obj_add_event_cb(matrix, keyboard_kb_key_cb,
                                (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), c);
            lv_obj_add_event_cb(screen.top_back_btn, keyboard_back_key_cb, LV_EVENT_KEY, c);
        }
        lv_group_focus_obj(matrix);
        kb_connect_indevs(c->group);

        // Pre-select the first key so the joystick selection is visible immediately;
        // static-render adds PRESSED so the highlight shows in screenshots.
        lv_buttonmatrix_set_selected_button(matrix, 0);
        if (g_static_render) lv_obj_add_state(matrix, LV_STATE_PRESSED);
    }

    lv_obj_add_event_cb(screen.screen, keyboard_cleanup_cb, LV_EVENT_DELETE, c);
    load_screen_and_cleanup_previous(screen.screen);
}


// ===========================================================================
// seed_mnemonic_entry_screen
// ---------------------------------------------------------------------------
//
// BIP-39 seed-word entry (the LVGL port of Python's SeedMnemonicEntryScreen,
// seed_screens.py). A 5x6 a-z + DEL `lv_buttonmatrix` drives a live prefix-match
// state machine against a cfg-passed `wordlist`: as letters are entered the
// keys that cannot continue any word are dimmed (LV_BUTTONMATRIX_CTRL_DISABLED)
// and a candidate-word panel updates. The matching logic (calc_possible_words /
// calc_possible_alphabet + the trailing-space "live slot" trick) is inherently
// per-keystroke, so it must run native here — the View cannot intervene mid-word.
//
// Two candidate panels, by input mode:
//   - hardware/240 (Python parity): a right column with a fixed highlight slot at
//     vertical center, dimmed candidates above/below, and KEY1/KEY3 scroll +
//     KEY2 accept (the physical Pi Zero buttons), via kb_side_button.
//   - touch (deliberate divergence, UX-review pending): a scrollable candidate
//     list (tap a word to highlight it in place) + a persistent green CHECK
//     button beneath, disabled until a selection exists, tap = accept. No fixed
//     slot, no modal — mirrors the passphrase confirm idiom.
//
// Returns the chosen word via seedsigner_lvgl_on_text_entered() (the same host
// hook the other keyboard screens use); BACK returns RET_CODE__BACK_BUTTON via
// the scaffold's back button. The View loops the screen 12/24x per seed.
//
// cfg:
//   top_nav: { title (e.g. "Seed Word #3"), show_back_button, show_power_button }
//   initial_letters (string | array of single-char strings): letters already
//                   entered for this word (empty/absent = fresh)
//   wordlist (array of strings): the selected language's BIP-39 words (English =
//                   2048). Required; the View owns language selection.
//   initial_selected_word (string, optional): pre-select this candidate at load
//                   (touch: highlight it + enable CHECK; hardware: move the slot to
//                   it). Mainly for static screenshots — runtime selection is a tap/
//                   scroll. No-op if the word isn't a current match.

// Per-screen state. C++ containers, so new/delete (freed in the cleanup cb). The
// `letters` buffer mirrors Python's self.letters: the locked-in letters plus a
// trailing "live slot" (a single " " or the currently-floated letter) so the
// joystick can preview a letter before it is locked in with a click.
struct mnemonic_ctx_t {
    lv_obj_t   *matrix = nullptr;
    lv_obj_t   *ta = nullptr;
    lv_obj_t   *back_btn = nullptr;
    lv_obj_t   *top_nav = nullptr;
    lv_group_t *group = nullptr;
    bool        hardware = false;

    // Keyboard map (persistent for the buttonmatrix lifetime).
    std::vector<std::string>            key_storage;
    std::vector<const char *>           map;
    std::vector<lv_buttonmatrix_ctrl_t> ctrl;

    // Matching state.
    std::vector<std::string> wordlist;
    std::vector<std::string> letters;          // locked letters + trailing live slot
    std::vector<int>         possible_words;    // indices into wordlist
    std::string              possible_alphabet; // distinct allowed next letters
    int                      selected_index = 0;

    // Hardware candidate panel.
    lv_obj_t              *hl_btn = nullptr;    // center highlight slot
    lv_obj_t              *hl_label = nullptr;
    lv_obj_t              *arrow_up = nullptr;
    lv_obj_t              *arrow_down = nullptr;
    std::vector<lv_obj_t *> dim_above;          // nearest-first (sel-1, sel-2, ...)
    std::vector<lv_obj_t *> dim_below;          // nearest-first (sel+1, sel+2, ...)

    // Touch candidate panel.
    lv_obj_t *cand_list = nullptr;              // scrollable list of candidates
    lv_obj_t *check_btn = nullptr;              // persistent green accept
    int       touch_selected = -1;              // index into possible_words
};

// --- matching state machine (ports calc_possible_words / calc_possible_alphabet)

// The current search prefix: the letters buffer joined and right-trimmed of the
// trailing live slot (Python's "".join(self.letters).strip()).
static std::string mnemonic_prefix(const mnemonic_ctx_t *c) {
    std::string s;
    for (const std::string &ch : c->letters) s += ch;
    size_t end = s.find_last_not_of(' ');
    return (end == std::string::npos) ? std::string() : s.substr(0, end + 1);
}

// The candidate-word panel stays hidden until the prefix reaches this many
// letters. A single letter matches up to ~250 BIP-39 words; on a slower display
// (notably the ESP32-P4) rebuilding that many candidate widgets on the very first
// keystroke stalls the UI for a visible beat — and a 1-letter candidate list has
// little practical value anyway (you can't meaningfully pick a word yet). So the
// panel, and the accept affordance that reads from it, wait for a second letter to
// narrow the field. NOTE: possible_words is still computed at every stage — the
// keyboard's next-letter dimming (possible_alphabet) is derived from it — so only
// the on-screen panel is deferred, not the matching itself.
static const size_t MNEMONIC_MATCH_MIN_LETTERS = 2;

static bool mnemonic_matches_shown(const mnemonic_ctx_t *c) {
    return mnemonic_prefix(c).size() >= MNEMONIC_MATCH_MIN_LETTERS;
}

// possible_words = every wordlist entry starting with the current prefix (reset
// the highlight to the top of the new list).
static void mnemonic_calc_possible_words(mnemonic_ctx_t *c) {
    std::string prefix = mnemonic_prefix(c);
    c->possible_words.clear();
    if (!prefix.empty()) {
        for (int i = 0; i < (int)c->wordlist.size(); ++i) {
            if (c->wordlist[i].rfind(prefix, 0) == 0) c->possible_words.push_back(i);
        }
    }
    c->selected_index = 0;
}

// possible_alphabet = the distinct set of letters that can follow the locked
// prefix (the (len-1)-th char of every possible word), in first-seen order.
// Recomputed only when a letter is locked in or deleted (not while floating), so
// the joystick can freely float between active letters. With <=1 letter the
// whole alphabet is active and there are no matches yet (Python parity).
static void mnemonic_calc_possible_alphabet(mnemonic_ctx_t *c) {
    if (c->letters.size() > 1) {
        size_t letter_num = c->letters.size() - 1;   // == len(search_letters)
        mnemonic_calc_possible_words(c);
        std::string alpha;
        for (int wi : c->possible_words) {
            const std::string &w = c->wordlist[wi];
            if (w.size() >= letter_num + 1) {
                char nx = w[letter_num];
                if (alpha.find(nx) == std::string::npos) alpha += nx;
            }
        }
        c->possible_alphabet = alpha;
    } else {
        c->possible_alphabet = "abcdefghijklmnopqrstuvwxyz";
        c->possible_words.clear();
        c->selected_index = 0;
    }
}

// --- view sync helpers ------------------------------------------------------

static void mnemonic_set_hidden(lv_obj_t *o, bool hidden) {
    if (!o) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Re-dim the keyboard for the current possible_alphabet. Dimming is purely
// VISUAL (mnemonic_dim_draw_cb recolors letter keys not in possible_alphabet) —
// NOT LV_BUTTONMATRIX_CTRL_DISABLED, because disabled keys are SKIPPED by the
// joystick navigation, which strands the cursor when most keys are inactive (you
// could only move along the few active keys). Python lets the cursor float freely
// over dimmed keys (they just don't register a press); keeping every key
// navigable here matches that. So this just forces a redraw.
static void mnemonic_apply_dimming(mnemonic_ctx_t *c) {
    if (c->matrix && lv_obj_is_valid(c->matrix)) lv_obj_invalidate(c->matrix);
}

// Visual key dimming (LV_EVENT_DRAW_TASK_ADDED): recolor every letter key that
// can't continue any word (not in possible_alphabet) to a recessed dark fill +
// faint text — INCLUDING the selected one. Python's Key.render dims the selected
// inactive key too, marking it only with a highlight OUTLINE (no solid fill); the
// orange border for that comes from the FOCUS/PRESSED border style added to the
// matrix, which this leaves untouched (only fill + label are recolored). Letters
// are plain ASCII, so this never collides with kb_icon_draw_cb (the DEL glyph).
static void mnemonic_dim_draw_cb(lv_event_t *e) {
    lv_draw_task_t *task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(task);
    if (!base || base->part != LV_PART_ITEMS) return;

    mnemonic_ctx_t *c = (mnemonic_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    lv_obj_t *kb = lv_event_get_target_obj(e);
    const char *txt = lv_buttonmatrix_get_button_text(kb, base->id1);
    if (!txt || std::strlen(txt) != 1 || txt[0] < 'a' || txt[0] > 'z') return;  // letters only
    if (c->possible_alphabet.find(txt[0]) != std::string::npos) return;          // active key

    lv_draw_label_dsc_t *ld = lv_draw_task_get_label_dsc(task);
    if (ld) { ld->color = lv_color_hex(0x555555); return; }   // gray text (Python "#333"-ish)
    lv_draw_fill_dsc_t *fd = lv_draw_task_get_fill_dsc(task);
    if (fd) fd->color = lv_color_hex(0x141414);                // recessed dark fill
}

static void mnemonic_update_display(mnemonic_ctx_t *c) {
    if (!c->ta || !lv_obj_is_valid(c->ta)) return;
    std::string p = mnemonic_prefix(c);
    lv_textarea_set_text(c->ta, p.c_str());
    lv_textarea_set_cursor_pos(c->ta, LV_TEXTAREA_CURSOR_LAST);
}

// Selected/normal styling for a touch candidate button (selected = SeedSigner
// orange, like the keyboard's selected key).
static void mnemonic_style_candidate(lv_obj_t *btn, bool selected) {
    lv_obj_set_style_bg_color(btn,
        lv_color_hex(selected ? ACCENT_COLOR : BUTTON_BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl) lv_obj_set_style_text_color(lbl,
        lv_color_hex(selected ? BUTTON_SELECTED_FONT_COLOR : BUTTON_FONT_COLOR), LV_PART_MAIN);
}

// Enable/disable the touch accept button (green + clickable once a candidate is
// selected; muted + inert until then).
static void mnemonic_set_check_enabled(mnemonic_ctx_t *c, bool en) {
    if (!c->check_btn || !lv_obj_is_valid(c->check_btn)) return;
    if (en) {
        lv_obj_add_flag(c->check_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(c->check_btn, lv_color_hex(SUCCESS_COLOR), LV_PART_MAIN);
        lv_obj_set_style_text_color(c->check_btn, lv_color_hex(BUTTON_SELECTED_FONT_COLOR), LV_PART_MAIN);
    } else {
        lv_obj_remove_flag(c->check_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(c->check_btn, lv_color_hex(BUTTON_BACKGROUND_COLOR), LV_PART_MAIN);
        lv_obj_set_style_text_color(c->check_btn, lv_color_hex(0x666666), LV_PART_MAIN);
    }
}

static void mnemonic_touch_candidate_cb(lv_event_t *e);  // fwd

// Hardware: refresh the fixed-slot highlight + the dimmed rows above/below it
// from possible_words[selected_index]. Hidden entirely when there are no matches.
static void mnemonic_render_matches_hw(mnemonic_ctx_t *c) {
    bool any = mnemonic_matches_shown(c) && !c->possible_words.empty();
    mnemonic_set_hidden(c->hl_btn, !any);
    mnemonic_set_hidden(c->arrow_up, !any);
    mnemonic_set_hidden(c->arrow_down, !any);
    if (!any) {
        for (lv_obj_t *l : c->dim_above) mnemonic_set_hidden(l, true);
        for (lv_obj_t *l : c->dim_below) mnemonic_set_hidden(l, true);
        return;
    }
    int count = (int)c->possible_words.size();
    int sel = c->selected_index;
    if (c->hl_label) lv_label_set_text(c->hl_label, c->wordlist[c->possible_words[sel]].c_str());
    for (int k = 0; k < (int)c->dim_above.size(); ++k) {
        int idx = sel - 1 - k;
        if (idx >= 0) { lv_label_set_text(c->dim_above[k], c->wordlist[c->possible_words[idx]].c_str()); mnemonic_set_hidden(c->dim_above[k], false); }
        else          { mnemonic_set_hidden(c->dim_above[k], true); }
    }
    for (int k = 0; k < (int)c->dim_below.size(); ++k) {
        int idx = sel + 1 + k;
        if (idx < count) { lv_label_set_text(c->dim_below[k], c->wordlist[c->possible_words[idx]].c_str()); mnemonic_set_hidden(c->dim_below[k], false); }
        else             { mnemonic_set_hidden(c->dim_below[k], true); }
    }
}

// Touch: rebuild the scrollable candidate list (one tappable button per match),
// clearing any prior selection and disabling the accept button.
static void mnemonic_render_matches_touch(mnemonic_ctx_t *c) {
    if (!c->cand_list || !lv_obj_is_valid(c->cand_list)) return;
    lv_obj_clean(c->cand_list);
    c->touch_selected = -1;
    mnemonic_set_check_enabled(c, false);

    // Deferred until the prefix is long enough (see mnemonic_matches_shown): with a
    // single letter this would build hundreds of candidate rows, stalling the P4.
    if (!mnemonic_matches_shown(c)) return;

    const int32_t row_h = (int32_t)(BUTTON_HEIGHT * 3 / 4);
    for (int k = 0; k < (int)c->possible_words.size(); ++k) {
        lv_obj_t *b = lv_obj_create(c->cand_list);
        lv_obj_set_width(b, lv_pct(100));
        lv_obj_set_height(b, row_h);
        lv_obj_set_style_radius(b, BUTTON_RADIUS / 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_left(b, COMPONENT_PADDING, LV_PART_MAIN);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(b, (void *)(intptr_t)k);
        lv_obj_add_event_cb(b, mnemonic_touch_candidate_cb, LV_EVENT_CLICKED, c);

        lv_obj_t *lbl = lv_label_create(b);
        lv_label_set_text(lbl, c->wordlist[c->possible_words[k]].c_str());
        lv_obj_set_style_text_font(lbl, &CANDIDATE_FONT, LV_PART_MAIN);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        mnemonic_style_candidate(b, false);
    }
}

static void mnemonic_render_matches(mnemonic_ctx_t *c) {
    if (c->hardware) mnemonic_render_matches_hw(c);
    else             mnemonic_render_matches_touch(c);
}

// --- state transitions ------------------------------------------------------

// Lock in `ch` (a click / KEY_PRESS on an active letter). If the live slot was
// already this letter (floated onto it) just append a fresh slot; otherwise
// (touch tap, or a click without floating) replace the live slot with the letter
// first. Then recompute the alphabet/matches and, if only one letter can follow,
// pre-select it.
static void mnemonic_lock_in(mnemonic_ctx_t *c, char ch) {
    if (c->letters.empty() || c->letters.back() != " ") {
        c->letters.push_back(" ");
    } else {
        c->letters.back() = std::string(1, ch);
        c->letters.push_back(" ");
    }
    mnemonic_calc_possible_alphabet(c);
    mnemonic_apply_dimming(c);
    if (c->possible_alphabet.size() == 1 && c->matrix) {
        int bi = kb_find_button(c->map.data(), c->possible_alphabet[0]);
        if (bi >= 0) lv_buttonmatrix_set_selected_button(c->matrix, (uint32_t)bi);
    }
    mnemonic_update_display(c);
    mnemonic_render_matches(c);
}

// Backspace: drop the last locked letter (and the live slot), restoring a fresh
// live slot, then recompute. Ports the KEY_BACKSPACE press branch.
static void mnemonic_delete(mnemonic_ctx_t *c) {
    if (c->letters.size() >= 2) c->letters.resize(c->letters.size() - 2);
    else                        c->letters.clear();
    c->letters.push_back(" ");
    mnemonic_calc_possible_alphabet(c);
    mnemonic_apply_dimming(c);
    mnemonic_update_display(c);
    mnemonic_render_matches(c);
}

// Joystick float: preview the hovered key in the live slot WITHOUT locking it in.
// A letter updates the matches (calc_possible_words, not the alphabet); hovering
// DEL clears the live slot so the display shows just the locked prefix.
static void mnemonic_float(mnemonic_ctx_t *c, const char *txt) {
    if (!txt) return;
    if (std::strcmp(txt, SeedSignerIconConstants::DELETE) == 0) {
        if (c->letters.empty()) c->letters.push_back(" ");
        else                    c->letters.back() = " ";
        mnemonic_calc_possible_words(c);
        mnemonic_update_display(c);
        mnemonic_render_matches(c);
        return;
    }
    if (std::strlen(txt) == 1 && txt[0] >= 'a' && txt[0] <= 'z') {
        char ch = txt[0];
        if (c->possible_alphabet.find(ch) == std::string::npos) return;  // dimmed
        if (c->letters.empty()) c->letters.push_back(std::string(1, ch));
        else                    c->letters.back() = std::string(1, ch);
        mnemonic_calc_possible_words(c);
        mnemonic_update_display(c);
        mnemonic_render_matches(c);
    }
}

// Scroll the hardware candidate highlight; flash the corresponding arrow.
static void mnemonic_scroll(mnemonic_ctx_t *c, int dir) {
    if (!mnemonic_matches_shown(c) || c->possible_words.empty()) return;  // panel hidden → nothing to scroll
    c->selected_index += dir;
    if (c->selected_index < 0) c->selected_index = 0;
    if (c->selected_index >= (int)c->possible_words.size()) c->selected_index = (int)c->possible_words.size() - 1;
    kb_flash_side_button(dir < 0 ? c->arrow_up : c->arrow_down);
    mnemonic_render_matches(c);
}

// Deliver the chosen word to the host (hardware KEY2 / touch CHECK).
static void mnemonic_accept_word(mnemonic_ctx_t *c, int word_index) {
    if (word_index < 0 || word_index >= (int)c->wordlist.size()) return;
    const std::string &word = c->wordlist[word_index];
    if (c->ta && lv_obj_is_valid(c->ta)) lv_textarea_set_text(c->ta, word.c_str());
    seedsigner_lvgl_on_text_entered(word.c_str());
}

// --- event callbacks --------------------------------------------------------

// KEY1/KEY2/KEY3 arrive as LV_KEY_F1..F3 on-device or ASCII '1'/'2'/'3' on the
// desktop runner (see is_aux_key in navigation.cpp). Returns 1/2/3, else 0.
static int mnemonic_aux_index(uint32_t key) {
#ifdef LV_KEY_F1
    if (key == LV_KEY_F1) return 1;
#endif
#ifdef LV_KEY_F2
    if (key == LV_KEY_F2) return 2;
#endif
#ifdef LV_KEY_F3
    if (key == LV_KEY_F3) return 3;
#endif
    if (key == (uint32_t)'1') return 1;
    if (key == (uint32_t)'2') return 2;
    if (key == (uint32_t)'3') return 3;
    return 0;
}

// Click on a key (joystick ENTER or touch tap): lock in a letter / delete.
static void mnemonic_value_changed_cb(lv_event_t *e) {
    lv_obj_t *m = lv_event_get_target_obj(e);
    mnemonic_ctx_t *c = (mnemonic_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    uint32_t id = lv_buttonmatrix_get_selected_button(m);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *txt = lv_buttonmatrix_get_button_text(m, id);
    if (!txt) return;
    if (std::strcmp(txt, SeedSignerIconConstants::DELETE) == 0) { mnemonic_delete(c); return; }
    if (std::strlen(txt) == 1 && txt[0] >= 'a' && txt[0] <= 'z') {
        char ch = txt[0];
        if (c->possible_alphabet.find(ch) == std::string::npos) return;  // dimmed key
        mnemonic_lock_in(c, ch);
    }
}

// Hardware PREPROCESS key filter: the aux keys (scroll/accept) and the top-nav
// handoff. Read before the buttonmatrix moves its selection. Directional floats
// are handled by the POST filter (after the move) so they see the new selection.
static void mnemonic_kb_preprocess_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    mnemonic_ctx_t *c = (mnemonic_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    uint32_t key = lv_event_get_key(e);

    int aux = mnemonic_aux_index(key);
    if (aux == 1) { mnemonic_scroll(c, -1); lv_event_stop_processing(e); return; }
    if (aux == 3) { mnemonic_scroll(c, +1); lv_event_stop_processing(e); return; }
    if (aux == 2) {
        // Only accept when the panel is actually shown — otherwise possible_words is
        // populated (for dimming) but no highlighted candidate is visible to accept.
        if (mnemonic_matches_shown(c) && !c->possible_words.empty()) {
            mnemonic_accept_word(c, c->possible_words[c->selected_index]);
        }
        lv_event_stop_processing(e);
        return;
    }

    // UP on the top row hands focus up to the back button (the buttonmatrix does
    // not wrap UP off the top row).
    if (key == LV_KEY_UP) {
        uint32_t sel = lv_buttonmatrix_get_selected_button(c->matrix);
        if (sel != LV_BUTTONMATRIX_BUTTON_NONE && sel < kb_top_row_count(c->map.data())
            && c->back_btn && lv_obj_is_valid(c->back_btn)) {
            lv_group_focus_obj(c->back_btn);
            lv_event_stop_processing(e);
        }
    }
}

// Hardware POST key filter: after the buttonmatrix has moved its selection,
// preview the now-selected key (live float). Ignores ENTER (a click → lock-in via
// VALUE_CHANGED) and any key while focus has handed off to the back button.
static void mnemonic_kb_post_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    mnemonic_ctx_t *c = (mnemonic_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    uint32_t key = lv_event_get_key(e);
    if (key != LV_KEY_UP && key != LV_KEY_DOWN && key != LV_KEY_LEFT && key != LV_KEY_RIGHT) return;
    if (c->group && lv_group_get_focused(c->group) != c->matrix) return;  // handed off to back
    uint32_t sel = lv_buttonmatrix_get_selected_button(c->matrix);
    if (sel == LV_BUTTONMATRIX_BUTTON_NONE) return;
    mnemonic_float(c, lv_buttonmatrix_get_button_text(c->matrix, sel));
}

static void mnemonic_back_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    mnemonic_ctx_t *c = (mnemonic_ctx_t *)lv_event_get_user_data(e);
    if (c) kb_back_down_to_matrix(e, c->matrix);
}

// Touch: tap a candidate to highlight it in place and enable the accept button.
static void mnemonic_touch_candidate_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target_obj(e);
    mnemonic_ctx_t *c = (mnemonic_ctx_t *)lv_event_get_user_data(e);
    if (!c || !c->cand_list) return;
    int k = (int)(intptr_t)lv_obj_get_user_data(btn);
    c->touch_selected = k;
    uint32_t n = lv_obj_get_child_count(c->cand_list);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t *child = lv_obj_get_child(c->cand_list, i);
        mnemonic_style_candidate(child, (int)i == k);
    }
    mnemonic_set_check_enabled(c, true);
}

// Touch: tap CHECK to accept the highlighted candidate.
static void mnemonic_check_cb(lv_event_t *e) {
    mnemonic_ctx_t *c = (mnemonic_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    if (c->touch_selected < 0 || c->touch_selected >= (int)c->possible_words.size()) return;
    mnemonic_accept_word(c, c->possible_words[c->touch_selected]);
}

static void mnemonic_cleanup_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    mnemonic_ctx_t *c = (mnemonic_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    if (c->group) lv_group_del(c->group);
    delete c;
}

void seed_mnemonic_entry_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    bool has_mode_override = false;
    input_mode_t mode_override = INPUT_MODE_TOUCH;
    nav_mode_override_from_cfg(cfg, has_mode_override, mode_override);
    bool hardware = (has_mode_override ? mode_override : input_profile_get_mode()) == INPUT_MODE_HARDWARE;

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);
    lv_obj_t *body = screen.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

    const int32_t content_w = lv_obj_get_content_width(body);
    const int32_t content_h = lv_obj_get_content_height(body);

    mnemonic_ctx_t *c = new mnemonic_ctx_t();
    c->back_btn = screen.top_back_btn;
    c->top_nav  = screen.top_nav;
    c->hardware = hardware;

    // wordlist (required).
    if (!cfg.contains("wordlist") || !cfg["wordlist"].is_array() || cfg["wordlist"].empty()) {
        delete c;
        throw std::runtime_error("seed_mnemonic_entry_screen requires a non-empty \"wordlist\" array");
    }
    c->wordlist.reserve(cfg["wordlist"].size());
    for (const auto &w : cfg["wordlist"]) {
        if (w.is_string()) c->wordlist.push_back(w.get<std::string>());
    }

    // initial_letters: accept a string ("ap") or an array (["a","p"]); normalize to
    // a list of single non-space chars.
    if (cfg.contains("initial_letters")) {
        const auto &il = cfg["initial_letters"];
        if (il.is_string()) {
            for (char ch : il.get<std::string>()) if (ch != ' ') c->letters.push_back(std::string(1, ch));
        } else if (il.is_array()) {
            for (const auto &el : il) {
                if (el.is_string()) {
                    std::string s = el.get<std::string>();
                    if (!s.empty() && s != " ") c->letters.push_back(s);
                }
            }
        }
    }

    // Mirror Python's __post_init__ seeding of the live slot: >1 letter locks them
    // all in (append a fresh live slot) and pre-computes the matches; exactly 1
    // letter sits in the live slot (no matches yet); 0 letters = a fresh slot.
    char initial_selected = 'a';
    if (c->letters.size() > 1) {
        initial_selected = c->letters.back()[0];
        c->letters.push_back(" ");
        mnemonic_calc_possible_alphabet(c);
    } else if (c->letters.size() == 1) {
        initial_selected = c->letters[0][0];
        c->possible_alphabet = "abcdefghijklmnopqrstuvwxyz";
    } else {
        c->letters.push_back(" ");
        c->possible_alphabet = "abcdefghijklmnopqrstuvwxyz";
    }

    // --- candidate-column geometry. Python sizes the column to the longest BIP-39
    // word (8 chars, "mushroom") + 2*COMPONENT_PADDING and pins it to the right
    // screen edge (matches_list_x = canvas_width - column_width). Reclaim the right
    // EDGE_PADDING gutter so the column reaches the actual screen edge (content_w +
    // EDGE_PADDING in body-local coords), pushing it as far right as possible.
    lv_point_t msz;
    lv_text_get_size(&msz, "mushroom", &CANDIDATE_FONT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int32_t matches_col_w = msz.x + 2 * COMPONENT_PADDING;
    const int32_t col_gap       = COMPONENT_PADDING;
    const int32_t matches_x     = content_w + EDGE_PADDING - matches_col_w;
    const int32_t keyboard_w    = matches_x - col_gap;

    // Text-entry strip (source of truth for the in-progress word display), sized to
    // the keyboard width.
    lv_obj_t *ta = kb_make_text_entry(body, keyboard_w, g_static_render);
    lv_obj_set_width(ta, keyboard_w);
    c->ta = ta;

    // --- keyboard map: 5x6 a-z, DEL (width 2) on the last row, hidden fillers to
    // keep the last row's columns aligned with the rows above (Python left-aligns
    // y/z/DEL with blank space to the right).
    const char *DEL = SeedSignerIconConstants::DELETE;
    c->key_storage.clear();
    for (int i = 0; i < 26; ++i) c->key_storage.push_back(std::string(1, (char)('a' + i)));
    c->map.clear();
    c->ctrl.clear();
    int col = 0;
    for (int i = 0; i < 26; ++i) {
        c->map.push_back(c->key_storage[i].c_str());
        c->ctrl.push_back((lv_buttonmatrix_ctrl_t)1);          // width-1 value key
        if (++col == 6) { c->map.push_back("\n"); col = 0; }   // 6 cols per row
    }
    // Row 5 already started after 'x' (24 letters = 4 full rows); y,z were appended
    // into the open row. Append DEL (width 2) + two hidden fillers to fill to 6.
    c->map.push_back(DEL);
    c->ctrl.push_back((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_NO_REPEAT
                       | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_CHECKED | 2));
    c->map.push_back(" ");
    c->ctrl.push_back((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_HIDDEN | 1));
    c->map.push_back(" ");
    c->ctrl.push_back((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_HIDDEN | 1));
    c->map.push_back("");

    lv_obj_t *matrix = lv_buttonmatrix_create(body);
    lv_buttonmatrix_set_map(matrix, c->map.data());
    lv_buttonmatrix_set_ctrl_map(matrix, c->ctrl.data());
    kb_style_matrix(matrix, &KEYBOARD_FONT);
    // Inactive keys are dimmed VISUALLY (mnemonic_dim_draw_cb), not via DISABLED,
    // so the joystick can still float over them (Python parity — see
    // mnemonic_apply_dimming). The matrix already sends draw-task events (enabled by
    // kb_style_matrix for its icon recolor), so just add our dimming handler.
    lv_obj_add_event_cb(matrix, mnemonic_dim_draw_cb, LV_EVENT_DRAW_TASK_ADDED, c);
    // Orange selection OUTLINE on the focused/pressed key. For an ACTIVE selected
    // key the orange fill (from kb_style_matrix) dominates and the border just
    // rims it; for an INACTIVE selected key the dim draw-cb keeps the dark fill, so
    // only this border marks the cursor — Python's "inactive + selected = highlight
    // outline" behavior.
    const lv_state_t sel_border_states[] = {
        LV_STATE_PRESSED, LV_STATE_FOCUSED, LV_STATE_FOCUS_KEY,
    };
    for (lv_state_t st : sel_border_states) {
        lv_obj_set_style_border_color(matrix, lv_color_hex(ACCENT_COLOR), LV_PART_ITEMS | st);
        lv_obj_set_style_border_opa(matrix, LV_OPA_COVER, LV_PART_ITEMS | st);
        lv_obj_set_style_border_width(matrix, 2, LV_PART_ITEMS | st);
    }
    c->matrix = matrix;

    const int32_t kb_top = BUTTON_HEIGHT + COMPONENT_PADDING;
    lv_obj_set_size(matrix, keyboard_w, content_h - kb_top);
    lv_obj_align(matrix, LV_ALIGN_TOP_LEFT, 0, kb_top);
    lv_obj_add_event_cb(matrix, mnemonic_value_changed_cb, LV_EVENT_VALUE_CHANGED, c);

    // --- candidate panel
    // Hardware soft-buttons (KEY1 up-scroll, KEY2 accept = the centered highlight,
    // KEY3 down-scroll) sit at the SAME physical Y positions as the passphrase
    // screen's KEY1/KEY2/KEY3 side panel, so they line up with the three physical
    // buttons beside the screen; the scroll arrows overshoot the right edge (clipped)
    // to reinforce that connection — exactly like seed_add_passphrase_screen. The
    // candidate list runs the full body height behind the arrows: Python shows up to
    // 3 matches above + 7 below the centered highlight (highlighted_row=3,
    // num_possible_rows=11), overflowing past the arrows and clipping at the screen
    // edge, with the arrows drawn ON TOP.
    const int32_t line_h = (int32_t)lv_font_get_line_height(&CANDIDATE_FONT);
    // Row pitch ≈ Python's matches_list_row_height (word cap height + padding).
    const int32_t row_h  = line_h * 3 / 4 + COMPONENT_PADDING / 2;
    const int32_t hl_h   = (int32_t)(BUTTON_HEIGHT * 3 / 4);

    if (hardware) {
        // Passphrase-aligned anchor slots (body-local). The KEY2 slot is a
        // BUTTON_HEIGHT box centered on the full screen; KEY1/KEY3 are one
        // (3*COMPONENT_PADDING + BUTTON_HEIGHT) step above/below it.
        const int32_t screen_h  = lv_obj_get_height(screen.screen);
        const int32_t spacing   = 3 * COMPONENT_PADDING + BUTTON_HEIGHT;
        const int32_t key2_top  = (screen_h - BUTTON_HEIGHT) / 2 - TOP_NAV_HEIGHT;
        const int32_t key1_top  = key2_top - spacing;
        const int32_t key3_top  = key2_top + spacing;
        const int32_t hl_center = key2_top + BUTTON_HEIGHT / 2;

        // Dimmed candidate rows: up to 3 above + 7 below the highlight, spaced by
        // row_h off its center. They overflow past the arrows and clip at the body
        // edge — created FIRST so the highlight and (last) the arrows draw on top.
        for (int k = 0; k < 3; ++k) {
            lv_obj_t *l = lv_label_create(body);
            lv_obj_set_style_text_font(l, &CANDIDATE_FONT, LV_PART_MAIN);
            lv_obj_set_style_text_color(l, lv_color_hex(0xdddddd), LV_PART_MAIN);
            lv_obj_set_pos(l, matches_x + COMPONENT_PADDING, hl_center - (k + 1) * row_h - line_h / 2);
            mnemonic_set_hidden(l, true);
            c->dim_above.push_back(l);
        }
        for (int k = 0; k < 7; ++k) {
            lv_obj_t *l = lv_label_create(body);
            lv_obj_set_style_text_font(l, &CANDIDATE_FONT, LV_PART_MAIN);
            lv_obj_set_style_text_color(l, lv_color_hex(0xdddddd), LV_PART_MAIN);
            lv_obj_set_pos(l, matches_x + COMPONENT_PADDING, hl_center + (k + 1) * row_h - line_h / 2);
            mnemonic_set_hidden(l, true);
            c->dim_below.push_back(l);
        }

        // Highlight slot (KEY2): the selected candidate, centered in the KEY2 slot,
        // left-aligned fixed-width word in the (on-screen) matches column.
        // The highlight runs COMPONENT_PADDING off the right screen edge on purpose
        // (Python's matches_list_highlight_button width = column + COMPONENT_PADDING):
        // its rounded right corner is clipped at the edge, reinforcing — like the
        // overshooting arrows — that this is the joystick-mode selection.
        lv_obj_t *hl = lv_obj_create(body);
        lv_obj_set_size(hl, matches_col_w + COMPONENT_PADDING, hl_h);
        lv_obj_set_pos(hl, matches_x, hl_center - hl_h / 2);
        lv_obj_set_style_bg_color(hl, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(hl, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(hl, BUTTON_RADIUS / 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(hl, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(hl, 0, LV_PART_MAIN);
        lv_obj_remove_flag(hl, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_t *hl_lbl = lv_label_create(hl);
        lv_obj_set_style_text_font(hl_lbl, &CANDIDATE_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(hl_lbl, lv_color_hex(BUTTON_SELECTED_FONT_COLOR), LV_PART_MAIN);
        lv_obj_align(hl_lbl, LV_ALIGN_LEFT_MID, COMPONENT_PADDING, 0);
        c->hl_btn = hl;
        c->hl_label = hl_lbl;

        // Scroll arrows (KEY1/KEY3), created LAST so they draw OVER the candidate
        // rows (Python parity), centered in their slots and overshooting the right
        // screen edge exactly like the passphrase side panel (same w / x / clipped).
        const int32_t side_w   = 56 * active_profile().px_multiplier / 100;
        const int32_t side_x   = content_w + EDGE_PADDING + COMPONENT_PADDING - side_w;
        const int32_t clipped  = COMPONENT_PADDING;
        const int32_t arrow_h  = (int32_t)(BUTTON_HEIGHT * 3 / 4);
        const int32_t arrow_dy = (BUTTON_HEIGHT - arrow_h) / 2;   // center within the slot
        c->arrow_up = kb_side_button(body, side_x, key1_top + arrow_dy, side_w, arrow_h,
                                     SeedSignerIconConstants::CHEVRON_UP, &ICON_FONT__SEEDSIGNER,
                                     BODY_FONT_COLOR, clipped, nullptr);
        c->arrow_down = kb_side_button(body, side_x, key3_top + arrow_dy, side_w, arrow_h,
                                       SeedSignerIconConstants::CHEVRON_DOWN, &ICON_FONT__SEEDSIGNER,
                                       BODY_FONT_COLOR, clipped, nullptr);
    } else {
        // Touch: a scrollable candidate list above a persistent green CHECK button.
        const int32_t check_h = hl_h;
        lv_obj_t *list = lv_obj_create(body);
        lv_obj_set_pos(list, matches_x, 0);
        lv_obj_set_size(list, matches_col_w, content_h - check_h - COMPONENT_PADDING);
        lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(list, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_row(list, COMPONENT_PADDING / 2, LV_PART_MAIN);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
        c->cand_list = list;

        lv_obj_t *chk = lv_obj_create(body);
        lv_obj_set_size(chk, matches_col_w, check_h);
        lv_obj_set_pos(chk, matches_x, content_h - check_h);
        lv_obj_set_style_radius(chk, BUTTON_RADIUS / 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(chk, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(chk, 0, LV_PART_MAIN);
        lv_obj_remove_flag(chk, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *chk_lbl = lv_label_create(chk);
        lv_label_set_text(chk_lbl, SeedSignerIconConstants::CHECK);
        lv_obj_set_style_text_font(chk_lbl, &ICON_FONT__SEEDSIGNER, LV_PART_MAIN);
        lv_obj_center(chk_lbl);
        lv_obj_add_event_cb(chk, mnemonic_check_cb, LV_EVENT_CLICKED, c);
        c->check_btn = chk;
        mnemonic_set_check_enabled(c, false);
    }

    // Initial dimming + the in-progress word + the candidate matches.
    mnemonic_apply_dimming(c);
    mnemonic_update_display(c);
    mnemonic_render_matches(c);

    // Optional: pre-select one of the current candidate words. Mainly for static
    // screenshots of the touch accept affordance (the selection + enabled CHECK is
    // otherwise only reachable by tapping at runtime). Applied after the initial
    // render so the candidate widgets already exist. No-op if the word isn't a
    // current match.
    std::string preselect = cfg.value("initial_selected_word", std::string());
    if (!preselect.empty()) {
        int idx = -1;
        for (int k = 0; k < (int)c->possible_words.size(); ++k) {
            if (c->wordlist[c->possible_words[k]] == preselect) { idx = k; break; }
        }
        if (idx >= 0) {
            if (hardware) {
                c->selected_index = idx;
                mnemonic_render_matches(c);
            } else {
                c->touch_selected = idx;
                if (c->cand_list && idx < (int)lv_obj_get_child_count(c->cand_list)) {
                    mnemonic_style_candidate(lv_obj_get_child(c->cand_list, idx), true);
                }
                mnemonic_set_check_enabled(c, true);
            }
        }
    }

    // Touch panels have no joystick cursor: a key is chosen by tapping it directly,
    // so NO key is pre-selected on load (the pre-highlighted letter is purely a
    // joystick affordance). LVGL already defaults a buttonmatrix to BUTTON_NONE, but
    // set it explicitly so touch is guaranteed to start blank regardless of any
    // platform default — the hardware branch below installs the cursor on
    // initial_selected. (Mirrors button_list_screen, where initial selection is
    // hardware-mode only.)
    lv_buttonmatrix_set_selected_button(matrix, LV_BUTTONMATRIX_BUTTON_NONE);

    if (hardware) {
        c->group = lv_group_create();
        lv_group_set_wrap(c->group, false);
        lv_group_add_obj(c->group, matrix);
        if (screen.top_back_btn) {
            lv_group_add_obj(c->group, screen.top_back_btn);
            lv_obj_add_event_cb(matrix, mnemonic_kb_preprocess_cb,
                                (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), c);
            lv_obj_add_event_cb(matrix, mnemonic_kb_post_cb, LV_EVENT_KEY, c);
            lv_obj_add_event_cb(screen.top_back_btn, mnemonic_back_key_cb, LV_EVENT_KEY, c);
        }
        lv_group_focus_obj(matrix);
        kb_connect_indevs(c->group);

        int sel = kb_find_button(c->map.data(), initial_selected);
        if (sel < 0) sel = 0;
        lv_buttonmatrix_set_selected_button(matrix, (uint32_t)sel);
        if (g_static_render) lv_obj_add_state(matrix, LV_STATE_PRESSED);
    }

    lv_obj_add_event_cb(screen.screen, mnemonic_cleanup_cb, LV_EVENT_DELETE, c);
    load_screen_and_cleanup_previous(screen.screen);
}


// ---------------------------------------------------------------------------
// seed_finalize_screen
// ---------------------------------------------------------------------------
//
// The "Finalize Seed" step shown immediately after a seed is loaded (the LVGL port
// of Python's SeedFinalizeScreen, seed_screens.py). Structurally it is a
// bottom-pinned ButtonListScreen (Done / BIP-39 Passphrase, no back button); its
// one special element is a centered fingerprint readout — the master fingerprint
// hex beneath a small "fingerprint" label, beside a large blue fingerprint icon.
//
// The readout ports Python's IconTextLine(is_text_centered=True,
// icon_name=FINGERPRINT): a horizontally-centered group of
//     [ fingerprint icon ] [gap] [ label (top) / value (bottom) ]
// with the label + value left-aligned to a shared x and the icon vertically
// centered against that two-line block. Python centers the whole group vertically
// between the top-nav and the first button; here the scaffold's flex body does that
// (see the upper_body grow below).
//
// cfg:
//   top_nav: { title (default "Finalize Seed"), show_power_button }. show_back_button
//            is forced false (Python SeedFinalizeScreen.show_back_button = False).
//   fingerprint (string, required): the master-fingerprint hex to display.
//   fingerprint_label (string, optional): the small label above the value; defaults
//            to "fingerprint" (translated upstream by the View / scenario localizer).
//   button_list (array): the action buttons (e.g. ["Done", "BIP-39 Passphrase"]).
//            Defaults to ["Done"] when absent.
void seed_finalize_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // fingerprint (required); the small label defaults to the (English) Python string.
    if (!cfg.contains("fingerprint") || !cfg["fingerprint"].is_string()) {
        throw std::runtime_error("seed_finalize_screen requires a \"fingerprint\" string");
    }
    std::string fingerprint       = cfg["fingerprint"].get<std::string>();
    std::string fingerprint_label = cfg.value("fingerprint_label", std::string("fingerprint"));

    // Force the SeedFinalizeScreen shape onto the scaffold cfg: a titled,
    // back-button-less, bottom-pinned button list. The View supplies the localized
    // title + button_list; default both so a bare cfg still renders.
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "Finalize Seed";
    cfg["top_nav"]["show_back_button"] = false;    // Python: show_back_button = False
    cfg["is_bottom_list"] = true;                  // Python: is_bottom_list = True
    if (!cfg.contains("button_list")) cfg["button_list"] = json::array({ "Done" });

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);

    // Python centers the fingerprint IconTextLine in the gap between the top-nav and
    // the first button. The scaffold's bottom-list body is a flex column
    // [upper_body][spacer grow=1][buttons]; make upper_body itself the grower and
    // center its child on the main (vertical) axis, then zero the scaffold spacer so
    // upper_body claims the whole gap. Result: the readout sits vertically centered
    // above the buttons.
    lv_obj_set_flex_grow(screen.upper_body, 1);
    lv_obj_set_flex_align(screen.upper_body, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (screen.button_list_spacer) lv_obj_set_flex_grow(screen.button_list_spacer, 0);

    // Fingerprint readout via the shared IconTextLine component — the SAME widget the
    // xpub-details / review-passphrase screens use, so labeled-value spacing (gap, leading
    // reclaim) is identical across every screen. Large blue fingerprint glyph, gray label
    // over the Latin body+2 hex; upper_body's center alignment centers the whole block.
    icon_text_line_opts_t fp = {};
    fp.icon_glyph   = SeedSignerIconConstants::FINGERPRINT;
    fp.icon_font    = &ICON_LARGE_BUTTON_FONT__SEEDSIGNER;   // Python icon_size = ICON_FONT_SIZE+12 (~36)
    fp.icon_color   = INFO_COLOR;
    fp.label_text   = fingerprint_label.c_str();
    fp.value_text   = fingerprint.c_str();
    fp.value_font   = seedsigner_latin_font(19);             // Python value = body+2, always Latin
    fp.label_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;
    fp.value_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;
    fp.is_text_centered = true;
    icon_text_line(screen.upper_body, &fp);

    bind_screen_navigation(
        cfg,
        screen,
        screen.button_list_count > 0 ? screen.button_list : NULL,
        screen.button_list_count,
        NAV_BODY_VERTICAL,
        0   // default the first action button (Done) selected, like button_list_screen
    );

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
// camera_preview_overlay_screen — tooling host for the camera live-preview overlay
// ---------------------------------------------------------------------------
// On device the overlay (camera_preview_overlay.{h,cpp}) is composited over LIVE
// camera pixels the camera adapter owns; the host pushes state via set_scanning()/
// set_progress(). This entry point lets the screenshot generator + runners exercise
// the SAME spec without a camera by synthesizing a placeholder square "preview"
// background and rendering the overlay onto it with a static, JSON-described state.
// The back affordance follows input_profile_get_mode(), which the tools set per
// resolution (240 = hardware → instruction text; larger = touch → gutter button).
//
// JSON config (all optional):
//   instructions_text : full hardware-mode bottom line (host composes "< back | ...")
//   scanning          : bool — show the status bar instead of the back affordance
//   progress          : 0..100 — animated-QR percent (implies scanning)
//   frame_status      : 0 none / 1 added / 2 repeated / 3 miss — status dot
//   fill_landscape    : bool — preview geometry. Default depends on resolution: the
//                       higher-res DSI panels (short dim > 240) default to a center-cut
//                       SQUARE with static side gutters (false); the Pi Zero (<= 240)
//                       fills the display (true). Set true on a DSI panel to opt into
//                       full landscape width; set false on the Pi Zero to force a square.
//   square            : {x,y,w,h} — explicit preview-square rect (overrides the above)
void camera_preview_overlay_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;

    json cfg;
    if (json_str && json_str[0]) {
        parse_screen_json_ctx(json_str, cfg);  // validates shape; every field optional
    } else {
        cfg = json::object();
    }

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
    int32_t screen_h = lv_display_get_vertical_resolution(NULL);

    // Preview geometry, defaulting per resolution:
    //   - Higher-resolution DSI panels (short dimension > 240 — the 480x320 / 800x480
    //     touch displays) DEFAULT to a landscape center-cut SQUARE, leaving static side
    //     gutters along the long axis. Those panels have per-frame update limits along
    //     their long axis, so minimizing the redrawn span there (camera writes only the
    //     square; the gutters never refresh) is the win — hence not opt-in.
    //   - The Pi Zero (<= 240 short dimension, incl. 320x240) FILLS the display, matching
    //     Python ScanScreen (render_rect defaults to the whole canvas; 240x240 square ==
    //     full anyway).
    // cfg["fill_landscape"] overrides the per-resolution default in EITHER direction
    // (true on a DSI panel opts into full landscape width; false on the Pi Zero forces
    // the square); cfg["square"] sets an explicit rect.
    int32_t short_dim = screen_w < screen_h ? screen_w : screen_h;
    bool fill_landscape = cfg.value("fill_landscape", short_dim <= 240);

    int32_t sx = 0, sy = 0, sw = screen_w, sh = screen_h;
    if (!fill_landscape) {
        // Center-cut square of the short dimension; the long axis keeps static gutters.
        sx = (screen_w - short_dim) / 2;
        sy = (screen_h - short_dim) / 2;
        sw = short_dim; sh = short_dim;
    }

    // Explicit rect overrides either default.
    if (cfg.contains("square") && cfg["square"].is_object()) {
        const auto &sq = cfg["square"];
        sx = sq.value("x", sx);
        sy = sq.value("y", sy);
        sw = sq.value("w", sw);
        sh = sq.value("h", sh);
    }

    // Placeholder "camera preview" fill so the overlay stands alone in tooling. A flat
    // mid-gray stands in for live pixels; the surrounding gutters stay black.
    lv_obj_t *preview = lv_obj_create(scr);
    lv_obj_set_size(preview, sw, sh);
    lv_obj_set_pos(preview, sx, sy);
    lv_obj_set_style_radius(preview, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(preview, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(preview, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(preview, lv_color_hex(0x3a3a3a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(preview, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    std::string instr;
    if (cfg.contains("instructions_text") && cfg["instructions_text"].is_string()) {
        instr = cfg["instructions_text"].get<std::string>();
    }

    camera_preview_overlay_spec_t spec;
    lv_memzero(&spec, sizeof(spec));
    spec.instructions_text  = instr.empty() ? nullptr : instr.c_str();
    spec.square_x = sx; spec.square_y = sy; spec.square_w = sw; spec.square_h = sh;
    spec.scanning_active    = cfg.value("scanning", false);
    spec.progress_percent   = cfg.value("progress", 0);
    spec.frame_status       = (camera_overlay_frame_status_t)cfg.value("frame_status", 0);
    if (spec.progress_percent > 0) spec.scanning_active = true;

    // Build onto the screen (gutter button in the gutter; in-square elements over the
    // preview). Tooling renders one static state, so we free the update handle right
    // away — destroy() releases only the handle struct; the widgets stay in the tree.
    // On device the host instead retains the handle to push set_scanning/set_progress.
    camera_preview_overlay_t *overlay = camera_preview_overlay_create(scr, &spec);
    camera_preview_overlay_destroy(overlay);

    load_screen_and_cleanup_previous(scr);
}


// ---------------------------------------------------------------------------
// camera_entropy_overlay_screen — tooling host for the image-entropy overlay
// ---------------------------------------------------------------------------
// Sibling of camera_preview_overlay_screen, for camera_entropy_overlay: synthesizes a
// placeholder square "preview" and renders the entropy overlay onto it in a static,
// JSON-described PHASE, so the screenshot generator + runners exercise the SAME spec
// without a camera. The back affordance/controls follow input_profile_get_mode(), which
// the tools set per resolution (240 = hardware → text; larger = touch → buttons).
//
// JSON config (all optional; on device the strings are host-provided + localized):
//   phase                : "preview" (default) | "capturing" | "confirm"
//   preview_instructions : hardware-mode PREVIEW bottom line (e.g. "< back | click a button")
//   confirm_instructions : hardware-mode CONFIRM bottom line (e.g. "< reshoot | accept >")
//   capturing_text       : accent-color transient line (e.g. "Capturing image…")
//   capture_label        : touch-mode label above the shutter
//   accept_label         : touch-mode Accept button text
//   fill_landscape       : bool — preview geometry (default: short dim <= 240 fills)
//   square               : {x,y,w,h} — explicit preview-square rect
void camera_entropy_overlay_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;

    json cfg;
    if (json_str && json_str[0]) {
        parse_screen_json_ctx(json_str, cfg);
    } else {
        cfg = json::object();
    }

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
    int32_t screen_h = lv_display_get_vertical_resolution(NULL);

    // Same per-resolution geometry default as camera_preview_overlay_screen: DSI panels
    // (short dim > 240) center-cut a SQUARE with static gutters; the Pi Zero fills.
    int32_t short_dim = screen_w < screen_h ? screen_w : screen_h;
    bool fill_landscape = cfg.value("fill_landscape", short_dim <= 240);

    int32_t sx = 0, sy = 0, sw = screen_w, sh = screen_h;
    if (!fill_landscape) {
        sx = (screen_w - short_dim) / 2;
        sy = (screen_h - short_dim) / 2;
        sw = short_dim; sh = short_dim;
    }
    if (cfg.contains("square") && cfg["square"].is_object()) {
        const auto &sq = cfg["square"];
        sx = sq.value("x", sx);
        sy = sq.value("y", sy);
        sw = sq.value("w", sw);
        sh = sq.value("h", sh);
    }

    // Placeholder "camera preview" fill (flat mid-gray); gutters stay black.
    lv_obj_t *preview = lv_obj_create(scr);
    lv_obj_set_size(preview, sw, sh);
    lv_obj_set_pos(preview, sx, sy);
    lv_obj_set_style_radius(preview, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(preview, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(preview, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(preview, lv_color_hex(0x3a3a3a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(preview, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    camera_entropy_phase_t phase = CAMERA_ENTROPY_PHASE_PREVIEW;
    if (cfg.contains("phase") && cfg["phase"].is_string()) {
        std::string p = cfg["phase"].get<std::string>();
        if (p == "capturing")    phase = CAMERA_ENTROPY_PHASE_CAPTURING;
        else if (p == "confirm") phase = CAMERA_ENTROPY_PHASE_CONFIRM;
    }

    std::string preview_instr, confirm_instr, capturing_text, capture_label, accept_label;
    auto get_str = [&](const char *k, std::string &dst) {
        if (cfg.contains(k) && cfg[k].is_string()) dst = cfg[k].get<std::string>();
    };
    std::string capture_icon;
    get_str("preview_instructions", preview_instr);
    get_str("confirm_instructions", confirm_instr);
    get_str("capturing_text",       capturing_text);
    get_str("capture_icon",         capture_icon);
    get_str("capture_label",        capture_label);
    get_str("accept_label",         accept_label);

    camera_entropy_overlay_spec_t spec;
    lv_memzero(&spec, sizeof(spec));
    spec.square_x = sx; spec.square_y = sy; spec.square_w = sw; spec.square_h = sh;
    spec.preview_instructions = preview_instr.empty()  ? nullptr : preview_instr.c_str();
    spec.confirm_instructions = confirm_instr.empty()  ? nullptr : confirm_instr.c_str();
    spec.capturing_text       = capturing_text.empty() ? nullptr : capturing_text.c_str();
    spec.capture_icon         = capture_icon.empty()   ? nullptr : capture_icon.c_str();
    camera_entropy_capture_style_t capture_style = CAMERA_ENTROPY_CAPTURE_RING;
    if (cfg.contains("capture_style") && cfg["capture_style"].is_string()) {
        std::string cs = cfg["capture_style"].get<std::string>();
        if (cs == "solid")       capture_style = CAMERA_ENTROPY_CAPTURE_SOLID;
        else if (cs == "button") capture_style = CAMERA_ENTROPY_CAPTURE_BUTTON;
    }
    spec.capture_style        = capture_style;
    spec.capture_label        = capture_label.empty()  ? nullptr : capture_label.c_str();
    spec.accept_label         = accept_label.empty()   ? nullptr : accept_label.c_str();
    spec.phase = phase;

    // Tooling renders one static state, so free the handle right away — destroy()
    // releases only the handle struct; the widgets stay in the tree.
    camera_entropy_overlay_t *overlay = camera_entropy_overlay_create(scr, &spec);
    camera_entropy_overlay_destroy(overlay);

    load_screen_and_cleanup_previous(scr);
}


// ---------------------------------------------------------------------------
// loading_screen — the animated "processing" spinner (LVGL port of Python's
// LoadingScreenThread, screen.py)
// ---------------------------------------------------------------------------
//
// Shown while the host CPU runs a long, blocking task (seed generation, PSBT
// signing, …). Python spins a background THREAD that repaints a comet arc around
// the Bitcoin logo while the main thread works. On LVGL the host thread is busy
// inside the task and can't drive frames, so the animation is SELF-DRIVEN by an
// lv_anim on the LVGL timer: as long as the platform keeps ticking lv_timer_handler
// (a dedicated display task on both the ESP32 and Pi Zero backends), the comet keeps
// rotating with zero host involvement.
//
// The screen takes no input and returns no result — it is a pure visual. The host
// shows it, does its work, then loads the next screen (which cleans this one up via
// load_screen_and_cleanup_previous). Deleting the screen auto-cancels the anim (its
// var is a child arc object), so no explicit teardown is needed.
//
// Layout (ports LoadingScreenThread): a centered Bitcoin logo (the baked
// btc_logo_* asset — orange disc + white tilted ₿) with a two-tone "comet" orbiting
// it: a bright 45° leading arc (#ff9416) trailed by a dim 45° arc (#80490b), the two
// rotating together. Optional status text sits above the spinner.
//
// cfg (all optional):
//   text : a short status line shown above the spinner (e.g. "Loading…").

// Comet spin driver. Advances by REAL elapsed time (so the rate is consistent), but
// CLAMPS the per-frame step: a plain wall-clock lv_anim would, on a starved frame, skip
// straight to the time-correct angle (a visible jump); clamping caps how far one frame
// can move, so under extreme load the spin instead visibly SLOWS DOWN — and smoothly
// speeds back up as frames recover — a natural "still working" signal, never a leap.
// Rotation is accumulated in millidegrees for smoothness at high frame rates. A lv_timer
// is NOT auto-freed with its objects, so the screen's DELETE handler tears it down.
static const uint32_t LOADING_SPIN_PERIOD_MS     = 490;    // one revolution at healthy fps (~2 rev/s)
static const uint32_t LOADING_SPIN_TICK_MS       = 33;     // update cadence (~30 fps)
static const int32_t  LOADING_SPIN_MAX_STEP_MDEG = 60000;  // clamp: at most 60° advance per frame

struct loading_spin_ctx_t {
    lv_obj_t   *bright;
    lv_obj_t   *dim;
    lv_timer_t *timer;
    uint32_t    last_tick;       // lv_tick at the previous update
    int32_t     rotation_mdeg;   // accumulated rotation, millidegrees [0, 360000)
};

static void loading_spin_timer_cb(lv_timer_t *timer) {
    loading_spin_ctx_t *c = (loading_spin_ctx_t *)lv_timer_get_user_data(timer);
    if (!c || !lv_obj_is_valid(c->bright)) return;

    uint32_t now = lv_tick_get();
    uint32_t dt  = now - c->last_tick;   // ms since last update (uint wrap-safe)
    c->last_tick = now;

    // Wall-clock advance, clamped so a long (starved) frame slows the spin rather than
    // skipping ahead. The clamp sits just above the generator's 70 ms GIF frame step, so
    // the captured GIF stays the healthy-rate loop while only real on-device stalls slow it.
    int32_t adv = (int32_t)((uint64_t)dt * 360000u / LOADING_SPIN_PERIOD_MS);
    if (adv > LOADING_SPIN_MAX_STEP_MDEG) adv = LOADING_SPIN_MAX_STEP_MDEG;
    c->rotation_mdeg = (c->rotation_mdeg + adv) % 360000;

    int32_t deg = c->rotation_mdeg / 1000;
    lv_arc_set_rotation(c->bright, deg);
    if (lv_obj_is_valid(c->dim)) lv_arc_set_rotation(c->dim, deg);
}

static void loading_cleanup_cb(lv_event_t *e) {
    loading_spin_ctx_t *c = (loading_spin_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    if (c->timer) lv_timer_delete(c->timer);
    delete c;
}

// Build one comet segment as a bare indicator-only arc (no track, no knob, not
// interactive) spanning [start,end] degrees, in `color`, at the shared orbit radius.
static lv_obj_t *loading_make_comet_arc(lv_obj_t *parent, int32_t diameter, int32_t width,
                                        int32_t start_deg, int32_t end_deg, uint32_t color) {
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, diameter, diameter);
    lv_obj_center(arc);
    lv_obj_remove_flag(arc, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_style_pad_all(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);       // hide the full-ring track
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);        // hide the knob
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);      // flat caps (Python arc)
    lv_arc_set_angles(arc, start_deg, end_deg);                       // fixed span; rotation animates it
    return arc;
}

void loading_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;

    json cfg;
    if (json_str && json_str[0]) {
        parse_screen_json_ctx(json_str, cfg);
    } else {
        cfg = json::object();
    }

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t screen_h = lv_display_get_vertical_resolution(NULL);

    // Centered Bitcoin logo (baked asset, one per profile — orange disc + white ₿).
    const lv_image_dsc_t *logo = btc_logo_for_active_profile();
    const int32_t logo_size = (int32_t)logo->header.w;   // square
    lv_obj_t *logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, logo);
    lv_obj_center(logo_img);

    // Orbit geometry (ports LoadingScreenThread): the comet rides a ring one
    // orbit_gap outside the logo, drawn COMPONENT_PADDING thick — matching Python's
    // bounding_box = logo ± orbit_gap and arc width = COMPONENT_PADDING.
    const int32_t orbit_gap   = 2 * COMPONENT_PADDING;
    const int32_t arc_width   = COMPONENT_PADDING;
    const int32_t orbit_diam  = logo_size + 2 * orbit_gap;
    const int32_t arc_sweep   = 45;                       // Python arc_sweep
    const uint32_t arc_bright = (uint32_t)BITCOIN_ORANGE; // Python arc_color "#ff9416"
    const uint32_t arc_dim    = 0x80490b;                 // Python arc_trailing_color

    // Trailing (dim) arc first so the bright head draws on top, then the bright head.
    // The dim segment sits immediately BEHIND the bright one so the pair reads as a
    // single comet (bright head [0,45], dim tail [-45,0]); a shared rotation spins them.
    lv_obj_t *dim_arc    = loading_make_comet_arc(scr, orbit_diam, arc_width,
                                                  360 - arc_sweep, 360, arc_dim);
    lv_obj_t *bright_arc = loading_make_comet_arc(scr, orbit_diam, arc_width,
                                                  0, arc_sweep, arc_bright);

    // Optional status text, centered above the spinner (Python positions it in the
    // band between the screen top and the top of the orbit).
    if (cfg.contains("text") && cfg["text"].is_string()) {
        std::string text = cfg["text"].get<std::string>();
        if (!text.empty()) {
            lv_obj_t *label = lv_label_create(scr);
            lv_label_set_text(label, text.c_str());
            lv_obj_set_style_text_font(label, &TOP_NAV_TITLE_FONT, LV_PART_MAIN);
            lv_obj_set_style_text_color(label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            // Python: screen_y = (canvas_h - orbit_bottom) / 2, i.e. centered in the
            // gap above the orbit. orbit_bottom = (canvas_h + logo)/2 + orbit_gap.
            const int32_t orbit_bottom = (screen_h + logo_size) / 2 + orbit_gap;
            lv_obj_align(label, LV_ALIGN_TOP_MID, 0, (screen_h - orbit_bottom) / 2);
        }
    }

    // Self-driven spin (see loading_spin_timer_cb): a clamped wall-clock integrator, so
    // it runs smoothly at healthy fps, slows under extreme load instead of jumping, and
    // recovers. Not gated on static render — the generator advances no ticks before the
    // still PNG, so that frame is a deterministic 0° (bright [0,45] + dim tail); the GIF
    // path advances 70 ms/frame, each unclamped (< 60° step), for a seamless 7-frame loop.
    loading_spin_ctx_t *spin = new loading_spin_ctx_t{ bright_arc, dim_arc, nullptr, lv_tick_get(), 0 };
    spin->timer = lv_timer_create(loading_spin_timer_cb, LOADING_SPIN_TICK_MS, spin);
    lv_obj_add_event_cb(scr, loading_cleanup_cb, LV_EVENT_DELETE, spin);

    load_screen_and_cleanup_previous(scr);
}


// ---------------------------------------------------------------------------
// qr_display_screen — full-bleed QR renderer (parity with Python QRDisplayScreen)
// ---------------------------------------------------------------------------
// Chrome-free full-screen QR (no top-nav), matching Python's QRDisplayScreen. Encodes
// a payload with the qrcodegen library bundled in LVGL and paints the module matrix at
// integer scale, black modules on a brightness-driven gray background.
//
// Animation is HOST-DRIVEN. The encode_qr.py cadence lives in Python (UR fountain frames
// are generated on the fly and cannot be precomputed), so the screen renders the initial
// frame from cfg["qr_data"] and the host pushes each subsequent frame via
// qr_display_set_frame() — mirroring the camera-overlay set_* live-update pattern.
//
// Brightness is a native render concern. HARDWARE: KEY_UP/DOWN raise a passive hint panel
// with up/down chevrons + translated "Brighter"/"Darker" text (physical keys do the work).
// TOUCH: tap the QR to raise a draggable slider flanked by dim/bright "sun" icons; top-right
// X to close. The final value is reported on exit via the weak seedsigner_lvgl_on_qr_brightness()
// hook so the host can persist SETTING__QR_BRIGHTNESS.
//
// The only user-facing strings are the hardware hints' brighter_text/darker_text, passed in
// ALREADY TRANSLATED (this repo's screens are locale-agnostic; translation is the host's job).

// Optional host hook (weak no-op default; a host overrides to persist the setting).
extern "C" __attribute__((weak)) void seedsigner_lvgl_on_qr_brightness(uint8_t /*brightness*/) {}

#if LV_USE_QRCODE

namespace {

enum qr_encode_mode_t { QR_ENC_NUMERIC, QR_ENC_ALNUM, QR_ENC_BYTE, QR_ENC_AUTO };

struct qr_display_ctx_t {
    lv_obj_t   *screen;
    lv_obj_t   *canvas;
    void       *canvas_buf;     // lv_malloc'd RGB565 canvas buffer (short_dim square)
    int32_t     canvas_side;    // canvas edge in px (display short dimension)
    lv_group_t *group;          // hardware keypad group (NULL in touch mode)
    lv_obj_t   *toast;          // brightness toast container (built once, hidden)
    lv_timer_t *toast_timer;    // one-shot auto-hide timer (NULL when idle)

    input_mode_t     input_mode;
    int              brightness;   // 31..255
    int              border;       // quiet-zone modules
    qr_encode_mode_t mode;
    bool             show_tips;
    bool             emitted;      // exit reported once

    // qrcodegen scratch/output, sized for version 40, allocated once. ctx->out holds
    // the CURRENT frame's encoded matrix so a brightness change can repaint without
    // re-encoding.
    uint8_t *tmp;
    uint8_t *out;
    bool     have_frame;           // ctx->out holds a valid encoded QR
};

// The single active QR screen, so qr_display_set_frame() can reach it. LVGL is
// single-threaded (host pushes frames from the same loop), so no locking is needed.
qr_display_ctx_t *g_qr_ctx = nullptr;

// Touch toast stays up longer than the hardware hint so there's time to tap it.
constexpr uint32_t QR_TOAST_MS_HARDWARE = 1200;
constexpr uint32_t QR_TOAST_MS_TOUCH    = 3000;

// brightness (31..255) -> gray, EXACT Python parity: hex(n)[2:] * 3 == (n,n,n).
lv_color_t qr_gray(int b) {
    uint32_t v = (uint32_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
    return lv_color_hex((v << 16) | (v << 8) | v);
}

bool qr_hexval(char c, int &v) {
    if (c >= '0' && c <= '9') { v = c - '0'; return true; }
    if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; return true; }
    if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; return true; }
    return false;
}

std::vector<uint8_t> qr_base64_decode(const std::string &in) {
    auto dec = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    int val = 0, bits = -8;
    for (char c : in) {
        if (c == '=') break;
        int d = dec(c);
        if (d < 0) continue;  // skip whitespace/newlines
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 0) { out.push_back((uint8_t)((val >> bits) & 0xff)); bits -= 8; }
    }
    return out;
}

// Decode the cfg["qr_data"] string (which crossed the JSON boundary) per data_encoding.
// JSON can't carry raw bytes, so binary payloads (CompactSeedQR) arrive hex/base64.
std::vector<uint8_t> qr_decode_payload(const std::string &s, const std::string &enc) {
    std::vector<uint8_t> out;
    if (enc == "hex") {
        std::string h;
        for (char c : s) if (!isspace((unsigned char)c)) h.push_back(c);
        if (h.size() % 2 != 0) throw std::runtime_error("qr_display_screen: hex payload has odd length");
        out.reserve(h.size() / 2);
        for (size_t i = 0; i < h.size(); i += 2) {
            int hi, lo;
            if (!qr_hexval(h[i], hi) || !qr_hexval(h[i + 1], lo))
                throw std::runtime_error("qr_display_screen: invalid hex payload");
            out.push_back((uint8_t)((hi << 4) | lo));
        }
    } else if (enc == "base64") {
        out = qr_base64_decode(s);
    } else {  // utf8: raw string bytes
        out.assign(s.begin(), s.end());
    }
    return out;
}

// Encode `data` into ctx->out. SeedQR stays NUMERIC (to match the SeedQR standard version);
// BBQR and similar all-uppercase payloads use ALPHANUMERIC (denser QR, how real devices show
// BBQR); everything else (incl. binary CompactSeedQR) is BYTE. "auto" picks the most compact
// mode the payload allows, mirroring Python's qrcode auto-detect. ECC=L and boostEcl=false
// mirror Python's non-boosting qrcode lib. Returns false (frame kept) if it can't fit.
bool qr_encode(qr_display_ctx_t *ctx, const uint8_t *data, size_t len) {
    qr_encode_mode_t mode = ctx->mode;
    if (mode == QR_ENC_AUTO) {
        // numeric > alphanumeric > byte. isNumeric/isAlphanumeric take a C string, so a NUL
        // in the payload (binary) disqualifies both and it falls through to byte.
        std::string s((const char *)data, len);
        bool clean = len > 0 && s.find('\0') == std::string::npos;
        if      (clean && qrcodegen_isNumeric(s.c_str()))      mode = QR_ENC_NUMERIC;
        else if (clean && qrcodegen_isAlphanumeric(s.c_str())) mode = QR_ENC_ALNUM;
        else                                                   mode = QR_ENC_BYTE;
    }

    if (mode == QR_ENC_NUMERIC) {
        std::string digits((const char *)data, len);
        if (qrcodegen_isNumeric(digits.c_str())) {
            // makeNumeric writes the segment into tmp; encodeSegmentsAdvanced then reuses
            // tmp as scratch (it's allowed to alias the segment buffer) and writes out.
            struct qrcodegen_Segment seg = qrcodegen_makeNumeric(digits.c_str(), ctx->tmp);
            return qrcodegen_encodeSegmentsAdvanced(&seg, 1, qrcodegen_Ecc_LOW, 1, 40,
                                                    qrcodegen_Mask_AUTO, false, ctx->tmp, ctx->out);
        }
        // Host mislabeled a non-digit payload as numeric — fall through to byte mode.
    }

    if (mode == QR_ENC_ALNUM) {
        std::string s((const char *)data, len);
        if (qrcodegen_isAlphanumeric(s.c_str())) {
            struct qrcodegen_Segment seg = qrcodegen_makeAlphanumeric(s.c_str(), ctx->tmp);
            return qrcodegen_encodeSegmentsAdvanced(&seg, 1, qrcodegen_Ecc_LOW, 1, 40,
                                                    qrcodegen_Mask_AUTO, false, ctx->tmp, ctx->out);
        }
        // Not actually alphanumeric; fall through to byte mode.
    }

    if (len > (size_t)qrcodegen_BUFFER_LEN_MAX) return false;
    memcpy(ctx->tmp, data, len);  // encodeBinary clobbers dataAndTemp; out must be separate
    return qrcodegen_encodeBinary(ctx->tmp, len, ctx->out, qrcodegen_Ecc_LOW, 1, 40,
                                  qrcodegen_Mask_AUTO, false);
}

// Paint the current ctx->out matrix onto the canvas: gray fill, then black scale x scale
// module blocks (row-runs coalesced into single rects), centered with a `border`-module
// quiet zone. Reused verbatim on a brightness change (only the gray fill differs).
void qr_paint(qr_display_ctx_t *ctx) {
    if (!ctx->canvas || !ctx->have_frame) return;

    int size  = qrcodegen_getSize(ctx->out);
    int total = size + 2 * ctx->border;
    int sd    = ctx->canvas_side;

    int scale = sd / total;
    if (scale < 1) scale = 1;
    int qr_px = scale * total;
    int off   = (sd - qr_px) / 2;  // center the QR within the square canvas

    lv_canvas_fill_bg(ctx->canvas, qr_gray(ctx->brightness), LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(ctx->canvas, &layer);

    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color     = lv_color_black();
    d.bg_opa       = LV_OPA_COVER;
    d.radius       = 0;
    d.border_width = 0;

    for (int my = 0; my < size; my++) {
        int mx = 0;
        while (mx < size) {
            if (!qrcodegen_getModule(ctx->out, mx, my)) { mx++; continue; }
            int run = mx;
            while (run < size && qrcodegen_getModule(ctx->out, run, my)) run++;
            lv_area_t a;
            a.x1 = off + (ctx->border + mx)  * scale;
            a.y1 = off + (ctx->border + my)  * scale;
            a.x2 = off + (ctx->border + run) * scale - 1;
            a.y2 = a.y1 + scale - 1;
            lv_draw_rect(&layer, &d, &a);
            mx = run;
        }
    }
    lv_canvas_finish_layer(ctx->canvas, &layer);
}

void qr_encode_and_paint(qr_display_ctx_t *ctx, const uint8_t *data, size_t len) {
    if (!ctx || !ctx->canvas || len == 0) return;
    if (!qr_encode(ctx, data, len)) return;  // too long to encode — keep the previous frame
    ctx->have_frame = true;
    qr_paint(ctx);
}

// --- brightness + toast ----------------------------------------------------

void qr_hide_toast(qr_display_ctx_t *ctx) {
    if (ctx->toast) lv_obj_add_flag(ctx->toast, LV_OBJ_FLAG_HIDDEN);
}

void qr_toast_timer_cb(lv_timer_t *t) {
    qr_display_ctx_t *ctx = (qr_display_ctx_t *)lv_timer_get_user_data(t);
    qr_hide_toast(ctx);
    ctx->toast_timer = NULL;  // one-shot self-deletes (repeat count 1)
}

void qr_show_toast(qr_display_ctx_t *ctx, uint32_t ms) {
    if (!ctx->toast) return;
    lv_obj_remove_flag(ctx->toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ctx->toast);
    if (ctx->toast_timer) { lv_timer_del(ctx->toast_timer); ctx->toast_timer = NULL; }
    // ms == 0 keeps the toast up persistently (the brightness_tip demo scenario / static
    // stills); a positive ms arms a one-shot auto-hide (normal brightness interaction).
    // Stills never animate the auto-hide either.
    if (!g_static_render && ms > 0) {
        ctx->toast_timer = lv_timer_create(qr_toast_timer_cb, ms, ctx);
        lv_timer_set_repeat_count(ctx->toast_timer, 1);
    }
}

void qr_set_brightness(qr_display_ctx_t *ctx, int b) {
    if (b < 31) b = 31;
    if (b > 255) b = 255;
    if (b == ctx->brightness) return;
    ctx->brightness = b;
    qr_paint(ctx);  // re-fill the gray; matrix unchanged (reuses ctx->out)
    // Real-time change signal: on a brightness change the host RESTARTS an animated sequence
    // (Python restarts the UR fountain so the valuable pure frames are re-delivered from the
    // start) and may persist the value. Fires per change; the host may debounce slider drags.
    seedsigner_lvgl_on_qr_brightness((uint8_t)ctx->brightness);
}

void qr_exit(qr_display_ctx_t *ctx) {
    if (ctx->emitted) return;
    ctx->emitted = true;
    seedsigner_lvgl_on_qr_brightness((uint8_t)ctx->brightness);
    seedsigner_lvgl_on_button_selected(SEEDSIGNER_RET_BACK_BUTTON, "qr_display_done");
}

// --- input handlers --------------------------------------------------------

// Hardware/joystick: UP/DOWN adjust brightness (+ hint toast); any other key exits.
void qr_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    qr_display_ctx_t *ctx = (qr_display_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP) {
        qr_set_brightness(ctx, ctx->brightness + 31);
        if (ctx->show_tips) qr_show_toast(ctx, QR_TOAST_MS_HARDWARE);
    } else if (key == LV_KEY_DOWN) {
        qr_set_brightness(ctx, ctx->brightness - 31);
        if (ctx->show_tips) qr_show_toast(ctx, QR_TOAST_MS_HARDWARE);
    } else {
        qr_exit(ctx);
    }
}

// Touch: tapping the QR raises the (interactive) toast.
void qr_canvas_tap_cb(lv_event_t *e) {
    qr_display_ctx_t *ctx = (qr_display_ctx_t *)lv_event_get_user_data(e);
    if (ctx && ctx->show_tips) qr_show_toast(ctx, QR_TOAST_MS_TOUCH);
}
// Touch: the brightness slider drives the gray live as it is dragged; each change also
// resets the panel's idle auto-hide so it stays up while the user is adjusting.
void qr_slider_cb(lv_event_t *e) {
    qr_display_ctx_t *ctx = (qr_display_ctx_t *)lv_event_get_user_data(e);
    lv_obj_t *slider = lv_event_get_target_obj(e);
    if (!ctx || !slider) return;
    qr_set_brightness(ctx, (int)lv_slider_get_value(slider));
    qr_show_toast(ctx, QR_TOAST_MS_TOUCH);
}
void qr_close_cb(lv_event_t *e) {
    qr_display_ctx_t *ctx = (qr_display_ctx_t *)lv_event_get_user_data(e);
    if (ctx) qr_exit(ctx);
}

void qr_cleanup_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    qr_display_ctx_t *ctx = (qr_display_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->toast_timer) lv_timer_del(ctx->toast_timer);
    if (ctx->group)       lv_group_del(ctx->group);
    if (ctx->canvas_buf)  lv_free(ctx->canvas_buf);
    if (ctx->tmp)         lv_free(ctx->tmp);
    if (ctx->out)         lv_free(ctx->out);
    if (g_qr_ctx == ctx)  g_qr_ctx = nullptr;
    lv_free(ctx);
}

// --- widget builders -------------------------------------------------------

// A small icon label (brightness "sun", chevron, ...) in `font`, body-colored.
lv_obj_t *qr_make_icon(lv_obj_t *parent, const char *glyph, const lv_font_t *font) {
    lv_obj_t *ic = lv_label_create(parent);
    lv_label_set_text(ic, glyph);
    lv_obj_set_style_text_font(ic, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(ic, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    return ic;
}

// A horizontal, content-sized flex row with the standard inter-item gap.
lv_obj_t *qr_make_row(lv_obj_t *parent) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, COMPONENT_PADDING, LV_PART_MAIN);
    return row;
}

// One hardware hint row: a chevron (which physical key) + the translated label. The
// physical KEY_UP/KEY_DOWN do the work; the row tells the user which key does what. Text
// (not an icon) so it reads as a clear instruction; the host passes it already translated.
void qr_build_hint_row(lv_obj_t *parent, const char *chevron, const std::string &text) {
    lv_obj_t *row = qr_make_row(parent);
    lv_obj_set_style_pad_ver(row, COMPONENT_PADDING / 2, LV_PART_MAIN);
    qr_make_icon(row, chevron, &ICON_FONT__SEEDSIGNER);
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text.c_str());
    lv_obj_set_style_text_font(label, &BODY_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    // Single-line hint, so force CLIP: in this content-sized [chevron][text] row the
    // default LV_LABEL_LONG_WRAP would let the shaped-locale run layer wrap a wide
    // translation and collapse it to line 0 (see glyph-run-single-line-label-wrap-collapse.md).
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
}

// The brightness panel: a rounded semi-transparent box at the bottom, raised on
// interaction. HARDWARE shows passive chevron + translated-text hint rows (physical keys
// act); TOUCH shows a draggable slider flanked by dim/bright suns (a slider is the natural
// touch affordance for a range). brighter_text/darker_text are used only by the hardware hints.
void qr_build_toast(qr_display_ctx_t *ctx, const std::string &brighter, const std::string &darker) {
    lv_obj_t *toast = lv_obj_create(ctx->screen);
    lv_obj_remove_style_all(toast);
    lv_obj_set_size(toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(toast, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(toast, 224, LV_PART_MAIN);        // Python render_brightness_tip opacity
    lv_obj_set_style_radius(toast, 8 * active_profile().px_multiplier / 100, LV_PART_MAIN);
    lv_obj_set_style_pad_all(toast, COMPONENT_PADDING, LV_PART_MAIN);
    lv_obj_set_flex_flow(toast, LV_FLEX_FLOW_COLUMN);
    // LEFT-align rows (cross axis = START) so the hardware hint chevrons line up in a column
    // (the two rows differ in width because "Brighter"/"Darker" differ in length).
    lv_obj_set_flex_align(toast, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -COMPONENT_PADDING * 2);
    lv_obj_remove_flag(toast, LV_OBJ_FLAG_SCROLLABLE);

    if (ctx->input_mode == INPUT_MODE_TOUCH) {
        // dim sun (small) | slider | bright sun (large)
        lv_obj_t *row = qr_make_row(toast);
        qr_make_icon(row, SeedSignerIconConstants::BRIGHTNESS, &ICON_FONT__SEEDSIGNER);
        lv_obj_t *slider = lv_slider_create(row);
        lv_slider_set_range(slider, 31, 255);                 // same 31..255 gray range
        lv_slider_set_value(slider, ctx->brightness, LV_ANIM_OFF);
        lv_obj_set_width(slider, ctx->canvas_side / 2);
        lv_obj_set_style_bg_color(slider, lv_color_hex(INACTIVE_COLOR), LV_PART_MAIN);
        lv_obj_set_style_bg_color(slider, lv_color_hex(ACCENT_COLOR), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, lv_color_hex(BODY_FONT_COLOR), LV_PART_KNOB);
        lv_obj_add_event_cb(slider, qr_slider_cb, LV_EVENT_VALUE_CHANGED, ctx);
        qr_make_icon(row, SeedSignerIconConstants::BRIGHTNESS, &ICON_LARGE_BUTTON_FONT__SEEDSIGNER);
    } else {
        qr_build_hint_row(toast, SeedSignerIconConstants::CHEVRON_UP,   brighter);
        qr_build_hint_row(toast, SeedSignerIconConstants::CHEVRON_DOWN, darker);
    }

    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    ctx->toast = toast;
}

// Touch-only top-right X to dismiss the chrome-free QR (parity screens exit via any key
// on hardware; touch has no keys, so it gets an explicit affordance clear of the toast).
void qr_build_close_button(qr_display_ctx_t *ctx) {
    lv_obj_t *btn = lv_button_create(ctx->screen);
    int sz = TOP_NAV_BUTTON_SIZE;
    lv_obj_set_size(btn, sz, sz);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(BUTTON_BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    // Screen top-right GUTTER (aligned to the parent, NOT the QR square): a QR's top-right
    // finder pattern sits only `border` quiet-zone modules in, so a canvas-corner button
    // would overlap it and break scannability. Touch profiles are landscape (short_dim >
    // 240) and always have side gutters — same assumption as the camera-overlay gutter
    // buttons (camera_preview_overlay.cpp back_btn).
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -EDGE_PADDING, EDGE_PADDING);

    lv_obj_t *x = lv_label_create(btn);
    lv_label_set_text(x, SeedSignerIconConstants::CLOSE);
    lv_obj_set_style_text_font(x, &ICON_FONT__SEEDSIGNER, LV_PART_MAIN);
    lv_obj_set_style_text_color(x, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    lv_obj_center(x);

    lv_obj_add_event_cb(btn, qr_close_cb, LV_EVENT_CLICKED, ctx);
}

}  // namespace

#endif  // LV_USE_QRCODE

// Push the next animated-QR frame from the host. See seedsigner.h.
extern "C" void qr_display_set_frame(const void *data, size_t len) {
#if LV_USE_QRCODE
    if (!g_qr_ctx) return;
    qr_encode_and_paint(g_qr_ctx, (const uint8_t *)data, len);
#else
    (void)data; (void)len;
#endif
}

// True while the brightness panel is on screen. The animation frame driver holds (does not
// advance) while this is true, so the tip greets the user on start and the valuable first
// frames are held until it clears. See seedsigner.h.
extern "C" bool qr_display_is_tip_active(void) {
#if LV_USE_QRCODE
    return g_qr_ctx && g_qr_ctx->toast &&
           !lv_obj_has_flag(g_qr_ctx->toast, LV_OBJ_FLAG_HIDDEN);
#else
    return false;
#endif
}

void qr_display_screen(void *ctx_json) {
#if !LV_USE_QRCODE
    // Built without the bundled QR encoder (no shipping build does this). Load a blank
    // screen so the entry point exists and navigation into it does not crash.
    (void)ctx_json;
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    load_screen_and_cleanup_previous(scr);
#else
    json cfg;
    parse_screen_json_ctx((const char *)ctx_json, cfg);

    if (!cfg.contains("qr_data") || !cfg["qr_data"].is_string() ||
        cfg["qr_data"].get<std::string>().empty()) {
        throw std::runtime_error("qr_display_screen: non-empty qr_data (string) is required");
    }
    std::string qr_data = cfg["qr_data"].get<std::string>();

    std::string mode_s = cfg.value("qr_mode", std::string("auto"));
    qr_encode_mode_t mode;
    if (mode_s == "numeric")            mode = QR_ENC_NUMERIC;
    else if (mode_s == "alphanumeric")  mode = QR_ENC_ALNUM;
    else if (mode_s == "byte")          mode = QR_ENC_BYTE;
    else if (mode_s == "auto")          mode = QR_ENC_AUTO;
    else throw std::runtime_error("qr_display_screen: qr_mode must be numeric|alphanumeric|byte|auto");

    std::string enc = cfg.value("data_encoding", std::string("utf8"));
    if (enc != "utf8" && enc != "hex" && enc != "base64")
        throw std::runtime_error("qr_display_screen: data_encoding must be utf8|hex|base64");

    int border = cfg.value("border", 2);
    if (border < 0 || border > 20)
        throw std::runtime_error("qr_display_screen: border must be 0..20");

    int brightness = cfg.value("initial_brightness", 62);
    if (brightness < 31) brightness = 31;
    if (brightness > 255) brightness = 255;

    bool show_tips = cfg.value("show_brightness_tips", true);
    std::string brighter, darker;
    if (show_tips) {
        // Hardware brightness hints use translated TEXT (touch uses an icon slider instead).
        // The host passes brighter_text/darker_text ALREADY TRANSLATED (i18n contract, no
        // strings baked in). Required when tips are on, since the same cfg may render on a
        // hardware profile.
        if (!cfg.contains("brighter_text") || !cfg["brighter_text"].is_string() ||
            !cfg.contains("darker_text")   || !cfg["darker_text"].is_string())
            throw std::runtime_error("qr_display_screen: brighter_text/darker_text (translated) "
                                     "are required when show_brightness_tips is true");
        brighter = cfg["brighter_text"].get<std::string>();
        darker   = cfg["darker_text"].get<std::string>();
    }

    bool has_override = false;
    input_mode_t mode_override = INPUT_MODE_TOUCH;
    nav_mode_override_from_cfg(cfg, has_override, mode_override);
    input_mode_t imode = has_override ? mode_override : input_profile_get_mode();

    std::vector<uint8_t> payload = qr_decode_payload(qr_data, enc);

    // Full-bleed screen, black gutters (the QR square is gray).
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    qr_display_ctx_t *ctx = (qr_display_ctx_t *)lv_malloc(sizeof(qr_display_ctx_t));
    lv_memzero(ctx, sizeof(*ctx));
    ctx->screen     = scr;
    ctx->input_mode = imode;
    ctx->brightness = brightness;
    ctx->border     = border;
    ctx->mode       = mode;
    ctx->show_tips  = show_tips;
    ctx->tmp = (uint8_t *)lv_malloc(qrcodegen_BUFFER_LEN_MAX);
    ctx->out = (uint8_t *)lv_malloc(qrcodegen_BUFFER_LEN_MAX);

    // Square canvas sized to the display's short dimension, centered.
    int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
    int32_t screen_h = lv_display_get_vertical_resolution(NULL);
    int32_t sd = screen_w < screen_h ? screen_w : screen_h;
    ctx->canvas_side = sd;
    ctx->canvas_buf  = lv_malloc(LV_CANVAS_BUF_SIZE(sd, sd, 16, LV_DRAW_BUF_STRIDE_ALIGN));
    ctx->canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(ctx->canvas, ctx->canvas_buf, sd, sd, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(ctx->canvas);
    // lv_malloc leaves the buffer uninitialized; fill gray up front so a rare encode
    // failure (payload too long) shows a clean background, not garbage pixels.
    lv_canvas_fill_bg(ctx->canvas, qr_gray(ctx->brightness), LV_OPA_COVER);

    g_qr_ctx = ctx;

    // Initial frame.
    qr_encode_and_paint(ctx, payload.data(), payload.size());

    if (show_tips) qr_build_toast(ctx, brighter, darker);

    if (imode == INPUT_MODE_HARDWARE) {
        // Keypad sink in a dedicated group: UP/DOWN = brightness, any other key exits.
        lv_obj_t *sink = lv_obj_create(scr);
        lv_obj_set_size(sink, 1, 1);
        lv_obj_set_pos(sink, 0, 0);
        lv_obj_set_style_opa(sink, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sink, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(sink, 0, LV_PART_MAIN);
        lv_obj_remove_flag(sink, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

        ctx->group = lv_group_create();
        lv_group_add_obj(ctx->group, sink);
        lv_obj_add_event_cb(sink, qr_key_cb, LV_EVENT_KEY, ctx);

        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD ||
                lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
                lv_indev_set_group(indev, ctx->group);
            }
        }
    } else {
        // Touch: tap the QR to raise the toast; explicit top-right X to close.
        lv_obj_add_flag(ctx->canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(ctx->canvas, qr_canvas_tap_cb, LV_EVENT_CLICKED, ctx);
        qr_build_close_button(ctx);
    }

    // Brightness panel visibility on load:
    //  - cfg "tips_visible" (gallery/demo aid): shown persistently (ms=0), incl. static stills.
    //  - otherwise, LIVE only: show it on START and auto-hide (Python parity). The tip greets
    //    the user (joystick brightness isn't obvious) AND — via qr_display_is_tip_active() —
    //    the frame driver HOLDS the animation on the valuable first frames until it clears
    //    (for a UR fountain those first parts are the pure, full-data frames). Static stills
    //    stay clean: g_static_render creates no auto-hide timer, so we skip the on-start show.
    if (show_tips) {
        if (cfg.value("tips_visible", false)) {
            qr_show_toast(ctx, 0);
        } else if (!g_static_render) {
            qr_show_toast(ctx, ctx->input_mode == INPUT_MODE_HARDWARE
                                   ? QR_TOAST_MS_HARDWARE : QR_TOAST_MS_TOUCH);
        }
    }

    lv_obj_add_event_cb(scr, qr_cleanup_cb, LV_EVENT_DELETE, ctx);
    load_screen_and_cleanup_previous(scr);
#endif  // LV_USE_QRCODE
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


// ===========================================================================
// psbt_overview_screen — animated transaction-flow diagram
// (LVGL port of Python PSBTOverviewScreen + TxExplorerAnimationThread)
// ===========================================================================
//
// The "Review Transaction" screen: a BtcAmount headline over a pictogram that fans
// every input, through a shared center bar, out to every output (recipients, self-
// transfers, change, OP_RETURN, and the miner fee), with an orange pulse continuously
// chasing along the curves to signal "these funds flow this way".
//
// Faithful port of the Python screen, with two deliberate substitutions the user
// approved: the diagram is drawn with native LVGL anti-aliased lines on an lv_canvas
// (Python supersampled a PIL image 4x); the curve MATH is a verbatim port of Python's
// custom quadratic Bezier (components.calc_bezier_curve), so the S-curve geometry is
// identical. The pulse animation is a direct port of TxExplorerAnimationThread: a
// segment-stepping orange head trailed by a gray eraser, driven by an lv_timer at the
// same 20 ms cadence, repainting only the changed segments onto the canvas each frame.
//
// cfg:
//   top_nav            : standard top-nav object (title defaults to "Review Transaction").
//   btc_amount         : {primary, secondary?, unit, network|icon_color} headline (see
//                        btc_amount_from_cfg / components.btc_amount). Optional.
//   num_inputs         : int >= 1.
//   destination_addresses : array of recipient address strings (may be empty).
//   num_self_transfer_outputs, num_change_outputs : ints (default 0).
//   has_op_return      : bool (default false).
//   button             : bottom-button label (default "Review details").
//   labels             : optional { input_n, one_input, recipient_1, recipient_n,
//                        ellipsis_series, ellipsis_trunc, fee, op_return, change,
//                        self_transfer } translated templates ({} = number slot).
//                        English fallbacks when omitted, so the desktop tool works as-is
//                        and per-locale scenario JSON supplies translations.

// ---- Bezier helpers (port of components.linear_interp / calc_bezier_curve). The math
// runs in FLOATING POINT and is snapped to the pixel grid only at the end (psbt_snap),
// because integer lerps floor asymmetrically: a branch and its mirror about the
// centerline would round to different rows and the fork would look lopsided.
struct psbt_fpt { float x, y; };

static psbt_fpt psbt_lerp(psbt_fpt a, psbt_fpt b, float t) {
    return { (1.0f - t) * a.x + t * b.x, (1.0f - t) * a.y + t * b.y };
}

// Cubic Bezier p0..p3 over `segments` line segments (De Casteljau); segments+1 points.
static std::vector<psbt_fpt> psbt_cubic(psbt_fpt p0, psbt_fpt p1, psbt_fpt p2, psbt_fpt p3, int segments) {
    std::vector<psbt_fpt> pts;
    pts.push_back(p0);
    float step = 1.0f / (float)segments;
    for (int i = 1; i <= segments; ++i) {
        if (i == segments) { pts.push_back(p3); break; }
        float t = step * (float)i;
        psbt_fpt a = psbt_lerp(p0, p1, t), b = psbt_lerp(p1, p2, t), c = psbt_lerp(p2, p3, t);
        psbt_fpt d = psbt_lerp(a, b, t), e = psbt_lerp(b, c, t);
        pts.push_back(psbt_lerp(d, e, t));
    }
    return pts;
}

// Snap a float point to the pixel grid, rounding the y-offset ABOUT the centerline vcf
// (lroundf is odd-symmetric, so mirror-image branches land on symmetric rows).
static lv_point_t psbt_snap(psbt_fpt p, int32_t vc, float vcf) {
    lv_point_t q;
    q.x = (int32_t)lroundf(p.x);
    q.y = vc + (int32_t)lroundf(p.y - vcf);
    return q;
}
static std::vector<lv_point_t> psbt_snap_curve(const std::vector<psbt_fpt> &f, int32_t vc, float vcf) {
    std::vector<lv_point_t> out;
    out.reserve(f.size());
    for (const auto &p : f) out.push_back(psbt_snap(p, vc, vcf));
    return out;
}

// Horizontal-tangent hold at the label end (`_TIP`) and at the convergence hub (`_HUB`),
// as fractions of the branch's horizontal span. Smaller `_HUB` lets the branch start
// bending toward its row sooner (gentler, separates from its neighbours earlier).
static const float PSBT_S_TIP = 0.9f;
static const float PSBT_S_HUB = 0.72f;

// A smooth S (sigmoid) branch from the label end `tip` to the convergence `hub`, HORIZONTAL
// at both ends (Python's flow-diagram look). Built as a single cubic so it never forces a
// vertical inflection the way the old two-quadratic did — that forced vertical was the
// "aggressive kink" on the short inner branches (fee / OP_RETURN) that opened the black
// negative-space gap. Returned tip-first.
static std::vector<psbt_fpt> psbt_branch(psbt_fpt tip, psbt_fpt hub, int segments) {
    float dx = hub.x - tip.x;   // signed span (hub right of tip for inputs, left for outputs)
    psbt_fpt c1 = { tip.x + dx * PSBT_S_TIP, tip.y };   // horizontal at the label
    psbt_fpt c2 = { hub.x - dx * PSBT_S_HUB, hub.y };   // horizontal at the hub
    return psbt_cubic(tip, c1, c2, hub, segments);
}

// Diagram palette / geometry (native px; Python worked 4x-supersampled, so its
// per-ssf values collapse to these once divided back down).
static const uint32_t PSBT_ASSOC_COLOR   = 0x666666;         // Python association_line_color "#666"
static const uint32_t PSBT_LABEL_COLOR   = 0xdddddd;         // Python chart_font_color "#ddd"
static const uint32_t PSBT_PULSE_COLOR   = (uint32_t)ACCENT_COLOR;  // Python GUIConstants.ACCENT_COLOR
// Stroke width scales with the display profile (3 px at the 240-height reference,
// ~6 px at 480-height) so the pictogram doesn't look thin on the larger panels.
static int32_t psbt_line_width() {
    int32_t w = 3 * active_profile().px_multiplier / 100;
    return w < 3 ? 3 : w;
}
static const int      PSBT_CURVE_STEPS   = 16;               // Python used 4 (hidden by its 4x supersample); we
                                                             // use a finer polyline so the native-AA curve reads smooth
// Pulse motion is WALL-CLOCK based (not per-frame), so the rate is identical whether
// lv_timer_handler runs at 30, 60, or 120 fps. The head crosses the whole
// input->center->output path in PSBT_PULSE_TRAVERSE_MS; the lit band trails it at
// PSBT_PULSE_BAND_FRAC of the path length; and the tick just sets the visual update
// cadence. A starved frame is clamped so the pulse slows rather than teleporting.
static const uint32_t PSBT_PULSE_TICK_MS     = 16;    // ~60 fps update cadence
static const float    PSBT_PULSE_TRAVERSE_MS = 1600;  // wall-clock for the head to cross the pictogram
static const float    PSBT_PULSE_BAND_FRAC   = 0.45f; // lit band length as a fraction of the path
static const float    PSBT_PULSE_GAP_FRAC    = 0.15f; // dark pause between cycles, as a fraction of the path
static const uint32_t PSBT_PULSE_MAX_STEP_MS = 100;   // clamp per-frame advance (starved-frame guard)

// One draw-line onto an already-open canvas layer, with round caps so consecutive
// segments join smoothly (Python's draw.line joint="curve").
static void psbt_line(lv_layer_t *layer, lv_point_t a, lv_point_t b, uint32_t color) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(color);
    d.width = psbt_line_width();
    d.round_start = true;
    d.round_end = true;
    d.p1.x = (lv_value_precise_t)a.x; d.p1.y = (lv_value_precise_t)a.y;
    d.p2.x = (lv_value_precise_t)b.x; d.p2.y = (lv_value_precise_t)b.y;
    lv_draw_line(layer, &d);
}

static void psbt_draw_polyline(lv_layer_t *layer, const std::vector<lv_point_t> &c, uint32_t color) {
    for (size_t i = 1; i < c.size(); ++i) psbt_line(layer, c[i - 1], c[i], color);
}

struct psbt_anim_ctx_t {
    lv_obj_t   *canvas;
    void       *canvas_buf;     // the displayed RGB565 buffer
    void       *pristine_buf;   // snapshot of the static gray diagram (fringe-free restore source)
    size_t      buf_bytes;
    lv_timer_t *timer;
    std::vector<std::vector<lv_point_t>> inputs;   // every input curve (canvas-local coords)
    std::vector<std::vector<lv_point_t>> outputs;  // every output curve
    std::vector<lv_point_t>              center_bar;  // segmented center bar
    int         total_segs;     // total path segments (input + center + output)
    int         band_segs;      // lit band length, in segments
    int         reset_at;       // head value at which a cycle restarts (total + band + dark gap)
    float       head;           // current head position along the path, in segments (float)
    uint32_t    last_tick;      // lv_tick at the previous update
};

// Light path-segment `s` (0-based, spanning input -> center -> output) in `color`. The
// fan phases draw the segment across every input/output curve at once (all funds flow
// together); the middle phase draws the single center-bar segment.
static void psbt_draw_seg(lv_layer_t *layer, const psbt_anim_ctx_t *c, int s, uint32_t color) {
    const int in_segs     = (int)c->inputs[0].size() - 1;
    const int center_segs = (int)c->center_bar.size() - 1;
    if (s < in_segs) {
        for (const auto &curve : c->inputs) psbt_line(layer, curve[s], curve[s + 1], color);
    } else if (s < in_segs + center_segs) {
        const int idx = s - in_segs;
        psbt_line(layer, c->center_bar[idx], c->center_bar[idx + 1], color);
    } else {
        const int idx = s - in_segs - center_segs;
        for (const auto &curve : c->outputs) psbt_line(layer, curve[idx], curve[idx + 1], color);
    }
}

// Wall-clock pulse: advance the head by REAL elapsed time (so the rate is identical at
// any frame rate — the loading spinner uses the same integrator), restore the pristine
// gray diagram, then repaint the current lit band over it. Restoring from the snapshot
// each frame is what keeps the orange from leaving an anti-aliased fringe behind: a
// same-width gray redraw can't fully re-cover the orange's soft edge, and a wider one
// would permanently thicken the curve — copying back the pristine pixels is exact.
static void psbt_pulse_timer_cb(lv_timer_t *timer) {
    psbt_anim_ctx_t *c = (psbt_anim_ctx_t *)lv_timer_get_user_data(timer);
    if (!c || !lv_obj_is_valid(c->canvas) || c->inputs.empty() || c->outputs.empty()) return;

    uint32_t now = lv_tick_get();
    uint32_t dt  = now - c->last_tick;
    c->last_tick = now;
    if (dt > PSBT_PULSE_MAX_STEP_MS) dt = PSBT_PULSE_MAX_STEP_MS;   // starved-frame clamp

    // Head crosses the whole path in PSBT_PULSE_TRAVERSE_MS; loop with a dark gap.
    float segs_per_ms = (float)c->total_segs / PSBT_PULSE_TRAVERSE_MS;
    c->head += (float)dt * segs_per_ms;
    if (c->head >= (float)c->reset_at) c->head -= (float)c->reset_at;

    // Restore the pristine gray diagram, then paint the lit band [head-band, head].
    memcpy(c->canvas_buf, c->pristine_buf, c->buf_bytes);

    int hi = (int)c->head;
    int lo = (int)(c->head - (float)c->band_segs);
    if (hi > c->total_segs - 1) hi = c->total_segs - 1;   // head arrived at the outputs
    if (lo < 0) lo = 0;
    if (hi >= lo && hi >= 0) {
        lv_layer_t layer;
        lv_canvas_init_layer(c->canvas, &layer);
        for (int s = lo; s <= hi; ++s) psbt_draw_seg(&layer, c, s, PSBT_PULSE_COLOR);
        lv_canvas_finish_layer(c->canvas, &layer);
    }
    lv_obj_invalidate(c->canvas);   // reflect both the restore and the repaint
}

static void psbt_cleanup_cb(lv_event_t *e) {
    psbt_anim_ctx_t *c = (psbt_anim_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    if (c->timer) lv_timer_delete(c->timer);
    if (c->canvas_buf) lv_free(c->canvas_buf);
    if (c->pristine_buf) lv_free(c->pristine_buf);
    delete c;
}

// Map a network to its accent/highlight color: Bitcoin orange (mainnet), testnet green,
// regtest blue. The JSON contract uses the Python SettingsConstants network codes
// ("M"/"T"/"R"); the legacy long names are also accepted so pre-existing scenarios keep
// working. Shared by btc_amount_from_cfg (the coin icon) and the PSBT detail screens (the
// formatted-address head/tail highlight), so a value and its address carry one color.
static uint32_t network_color(const std::string &net) {
    if (net == "T" || net == "testnet") return (uint32_t)TESTNET_COLOR;
    if (net == "R" || net == "regtest") return (uint32_t)REGTEST_COLOR;
    return (uint32_t)ACCENT_COLOR;   // "M" / "mainnet" / default
}

// The device's configured Network ("M"/"T"/"R"), from top-level cfg["network"] or echoed
// on cfg["btc_amount"]; empty if the host didn't provide it. (The legacy long names are
// also honored by network_color downstream.)
static std::string network_setting(const json &cfg) {
    if (cfg.contains("network") && cfg["network"].is_string()) {
        return cfg["network"].get<std::string>();
    }
    if (cfg.contains("btc_amount") && cfg["btc_amount"].is_object() &&
        cfg["btc_amount"].contains("network") && cfg["btc_amount"]["network"].is_string()) {
        return cfg["btc_amount"]["network"].get<std::string>();
    }
    return "";
}

// Resolve which network to COLOR a PSBT address by, as a Python network code ("M"/"T"/"R").
// The address FORMAT is prioritized over the device's Network setting: an unambiguous
// prefix decides outright, so a mainnet address on a regtest-configured device still reads
// as mainnet — a useful "this isn't the network you think" signal — rather than being
// recolored to match the setting. Only mainnet (bc1/1/3) and regtest (bcrt1) are
// unambiguous; tb1 is testnet-or-signet (no separate signet marker → testnet). The base58
// m/n/2 prefixes are shared by testnet/signet/regtest — knowing only that they are NOT
// mainnet, the device setting disambiguates testnet vs regtest, and an absent (or a
// disagreeing mainnet) setting defaults to testnet (the common case).
static std::string resolve_address_network(const json &cfg, const std::string &address) {
    if (address.rfind("bcrt1", 0) == 0) return "R";
    if (address.rfind("bc1",  0) == 0)  return "M";
    if (address.rfind("tb1",  0) == 0)  return "T";
    if (!address.empty()) {
        char c = address[0];
        if (c == '1' || c == '3') return "M";
        if (c == 'm' || c == 'n' || c == '2') {
            std::string s = network_setting(cfg);
            if (s == "R" || s == "regtest") return "R";
            if (s == "T" || s == "testnet") return "T";
            return "T";   // absent, or a mainnet setting that disagrees with the format
        }
    }
    // Unrecognized format: defer to the device setting, else mainnet.
    std::string s = network_setting(cfg);
    return s.empty() ? "M" : s;
}

// Build a components.btc_amount from a cfg object: maps network -> icon color (an
// explicit icon_color hex overrides) and forwards the host-formatted display strings.
// Shared by every amount-showing screen (PSBT overview / detail / change).
static lv_obj_t *btc_amount_from_cfg(lv_obj_t *parent, const json &j) {
    std::string primary   = j.value("primary", std::string(""));
    std::string secondary = j.value("secondary", std::string(""));
    std::string unit      = j.value("unit", std::string(""));

    btc_amount_opts_t o = {};
    o.primary       = primary.c_str();
    o.secondary     = secondary.empty() ? nullptr : secondary.c_str();
    o.unit          = unit.empty() ? nullptr : unit.c_str();
    o.primary_small = j.value("primary_small", false);

    o.icon_color = network_color(j.value("network", std::string("mainnet")));
    if (j.contains("icon_color") && j["icon_color"].is_string()) {
        o.icon_color = parse_hex_color(j["icon_color"].get<std::string>());
    }
    return btc_amount(parent, &o);
}

void psbt_overview_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;
    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // ---- Read the transaction structure (mirrors the Python dataclass fields).
    int  num_inputs        = cfg.value("num_inputs", 1);
    if (num_inputs < 1) num_inputs = 1;
    int  num_self_transfer = cfg.value("num_self_transfer_outputs", 0);
    int  num_change        = cfg.value("num_change_outputs", 0);
    bool has_op_return     = cfg.value("has_op_return", false);
    std::vector<std::string> dest_addrs;
    if (cfg.contains("destination_addresses") && cfg["destination_addresses"].is_array()) {
        for (const auto &a : cfg["destination_addresses"]) {
            if (a.is_string()) dest_addrs.push_back(a.get<std::string>());
        }
    }

    // ---- Translated labels: cfg["labels"] with English fallbacks (host owns i18n).
    const json labels = (cfg.contains("labels") && cfg["labels"].is_object()) ? cfg["labels"] : json::object();
    auto L = [&](const char *key, const char *dflt) -> std::string {
        if (labels.contains(key) && labels[key].is_string()) return labels[key].get<std::string>();
        return std::string(dflt);
    };
    auto fmt_n = [](const std::string &tmpl, long n) -> std::string {
        std::string out = tmpl;
        size_t pos = out.find("{}");
        if (pos != std::string::npos) out.replace(pos, 2, std::to_string(n));
        return out;
    };
    const std::string input_n         = L("input_n", "input {}");
    const std::string recipient_n     = L("recipient_n", "recipient {}");
    const std::string recipient_1     = L("recipient_1", "recipient 1");
    const std::string recipient_sing  = L("recipient_singular", "recipient");
    const std::string ellipsis_series = L("ellipsis_series", "[ ... ]");
    const std::string self_transfer   = L("self_transfer", "self-transfer");
    const std::string fee_label       = L("fee", "fee");
    const std::string op_return_label = L("op_return", "OP_RETURN");
    const std::string change_label    = L("change", "change");

    // ---- Scaffold: a bottom-pinned single-button list (Python is_bottom_list).
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = L("title", "Review Transaction");
    if (!cfg.contains("button_list")) {
        cfg["button_list"] = json::array({ cfg.value("button", std::string("Review details")) });
    }
    cfg["is_bottom_list"] = true;

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);

    // ---- BtcAmount headline in the upper body (Python's callout above the chart).
    lv_obj_t *headline = nullptr;
    if (cfg.contains("btc_amount") && cfg["btc_amount"].is_object() && screen.upper_body) {
        // Hug the top nav: the body already sits below the nav's own bottom buffer, so a
        // small pad here is enough. Keeping the callout high frees vertical room for the
        // pictogram (which is otherwise cramped on the many-row scenarios).
        lv_obj_set_style_pad_top(screen.upper_body, COMPONENT_PADDING / 4, LV_PART_MAIN);
        headline = btc_amount_from_cfg(screen.upper_body, cfg["btc_amount"]);
    }

    // ---- Measure the gap between the headline and the pinned button — that band is
    // the chart (Python: chart_y = below the callout; chart_height = up to the button).
    lv_obj_update_layout(screen.screen);

    const int32_t W    = lv_display_get_horizontal_resolution(NULL);
    const int32_t comp = COMPONENT_PADDING;
    const int32_t edge = EDGE_PADDING;

    int32_t chart_top;
    if (headline) {
        lv_area_t a; lv_obj_get_coords(headline, &a);
        chart_top = a.y2 + comp / 4;
    } else {
        chart_top = TOP_NAV_HEIGHT + comp;
    }
    int32_t chart_bottom = lv_display_get_vertical_resolution(NULL) - BUTTON_HEIGHT - comp;
    if (screen.button_list_count > 0 && lv_obj_is_valid(screen.button_list[0])) {
        lv_area_t ba; lv_obj_get_coords(screen.button_list[0], &ba);
        chart_bottom = ba.y1 - comp;
    }
    const int32_t chart_h = chart_bottom - chart_top;

    const int32_t text_h = lv_font_get_line_height(&BODY_FONT);

    // Only build the diagram if there is room for at least one row.
    if (chart_h >= text_h) {
        auto text_w = [&](const std::string &s) -> int32_t {
            lv_point_t sz;
            lv_text_get_size(&sz, s.c_str(), &BODY_FONT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            return sz.x;
        };

        // ---- Input column labels (Python inputs_column).
        std::vector<std::string> inputs_column;
        if (num_inputs == 1) {
            inputs_column.push_back(L("one_input", "1 input"));
        } else if (num_inputs > 5) {
            inputs_column.push_back(fmt_n(input_n, 1));
            inputs_column.push_back(fmt_n(input_n, 2));
            inputs_column.push_back(ellipsis_series);
            inputs_column.push_back(fmt_n(input_n, num_inputs - 1));
            inputs_column.push_back(fmt_n(input_n, num_inputs));
        } else {
            for (int i = 0; i < num_inputs; ++i) inputs_column.push_back(fmt_n(input_n, i + 1));
        }
        int32_t max_inputs_text_width = 0;
        for (const auto &s : inputs_column) max_inputs_text_width = std::max(max_inputs_text_width, text_w(s));

        // ---- Aesthetic geometry knobs (all scale with the display via `comp`). The
        // pictogram is kept PERFECTLY CENTERED on the screen midline: each half mirrors
        // the other (center bar straddles W/2, equal-width curve columns), and the room
        // per half is capped by the WIDER text column — so a long address on one side
        // pulls the short side inward (blank space on the short side) instead of shoving
        // the whole diagram off-center.
        const int32_t text_gap = comp / 2;      // balanced buffer between text and curve, both sides
        const int32_t cw_min   = 4 * comp;      // curve column min (Python's fixed width) ...
        const int32_t cw_max   = 6 * comp;      // ... up to ~50% wider when there's room (graceful curves)
        const int32_t cb_min   = 3 * comp / 2;  // center bar floor — kept short so the labels breathe
        const int32_t cb_max   = 5 * comp / 2;  // ... grows only a little past the floor
        const int32_t cb_floor = comp;          // hard floor when a tight screen can't center (below cb_min)
        const int32_t center_x = W / 2;

        // ---- Destination column. The pictogram conveys transaction STRUCTURE, not the
        // actual addresses (those are verified on the later review screens), so recipients
        // are shown generically: "recipient" for one, "recipient 1..N" for a few, and
        // collapsed to "recipient 1 / [ ... ] / recipient N" when there are many. (A
        // deliberate departure from the Python original, which truncated the addresses.)
        std::vector<std::string> destination_column;
        {
            const int n_recip  = (int)dest_addrs.size();
            const int total_rl = n_recip + num_self_transfer;   // recipient-like outputs
            if (total_rl <= 3) {
                if (n_recip == 1) {
                    destination_column.push_back(recipient_sing);
                } else {
                    for (int i = 0; i < n_recip; ++i) destination_column.push_back(fmt_n(recipient_n, i + 1));
                }
                for (int i = 0; i < num_self_transfer; ++i) destination_column.push_back(self_transfer);
            } else {
                destination_column.push_back(recipient_1);
                destination_column.push_back(ellipsis_series);
                destination_column.push_back(fmt_n(recipient_n, total_rl));
            }
            destination_column.push_back(fee_label);
            if (has_op_return) destination_column.push_back(op_return_label);
            for (int i = 0; i < num_change; ++i) destination_column.push_back(change_label);
        }
        int32_t dest_text_width = 0;
        for (const auto &s : destination_column) dest_text_width = std::max(dest_text_width, text_w(s));

        // ---- Solve the horizontal layout. Two regimes:
        //   Centered (the common case, and preferred): the pictogram is symmetric about
        //   the screen midline; the per-half room is capped by the WIDER text column, so
        //   the short side pulls in and leaves blank margin. Curves grow first (graceful),
        //   then the center bar, then blank.
        //   Fit fallback (tight screen / wide labels, e.g. many recipients at 240): if
        //   even the minimum centered core won't fit, GIVE UP centering to keep the labels
        //   on screen — anchor both text columns at the edges, shrink the center bar to its
        //   hard floor, and split the leftover between the two (still equal-width) curves.
        const int32_t tw_max = std::max(max_inputs_text_width, dest_text_width);
        const int32_t room   = center_x - edge - text_gap - tw_max;   // per-half budget for (cw + cb/2)

        int32_t center_bar_x, center_bar_width, dest_conj_x;
        int32_t input_start_x, inputs_text_right, output_end_x, destination_col_x;

        if (room >= cw_min + cb_min / 2) {
            // Centered regime.
            int32_t cw = room - cb_min / 2;
            if (cw > cw_max) cw = cw_max;
            if (cw < cw_min) cw = cw_min;
            int32_t cb = 2 * (room - cw);
            if (cb < cb_min) cb = cb_min;
            if (cb > cb_max) cb = cb_max;

            center_bar_x      = center_x - cb / 2;
            center_bar_width  = cb;
            dest_conj_x       = center_x + cb / 2;
            input_start_x     = center_bar_x - cw;
            inputs_text_right = input_start_x - text_gap;
            output_end_x      = dest_conj_x + cw;
            destination_col_x = output_end_x + text_gap;
        } else {
            // Fit fallback: labels at the edges, symmetric curves, minimal center bar.
            inputs_text_right = edge + max_inputs_text_width;   // input text left-justified at the edge
            destination_col_x = W - edge - dest_text_width;     // output text right-justified at the edge
            input_start_x     = inputs_text_right + text_gap;
            output_end_x      = destination_col_x - text_gap;
            int32_t avail = output_end_x - input_start_x;       // = cw + cb + cw
            int32_t cb = cb_floor;
            if (cb > avail - 2) cb = avail - 2;                 // keep room for a sliver of curve
            if (cb < 1) cb = 1;
            int32_t cw = (avail - cb) / 2;
            if (cw < 1) cw = 1;
            center_bar_x      = input_start_x + cw;
            center_bar_width  = cb;
            dest_conj_x       = center_bar_x + cb;
            output_end_x      = dest_conj_x + cw;               // re-derive so the two curves match exactly
        }

        // ---- Vertical placement: rows centered SYMMETRICALLY about the mid-line, so an
        // ODD count puts its middle row exactly on the centerline (a straight branch, no
        // bend). Pitch matches Python's even distribution (equal top/inter/bottom gaps);
        // deriving each row from the center avoids the integer-truncation drift that used
        // to nudge the whole block upward.
        const int32_t vc  = chart_h / 2;
        const float   vcf = (float)chart_h / 2.0f;
        const int n_in    = (int)inputs_column.size();
        const int n_dest  = (int)destination_column.size();

        auto row_pitch = [&](int n) -> float {
            if (n <= 1) return 0.0f;
            float natural = (float)(chart_h - n * text_h) / (float)(n + 1) + (float)text_h;
            // Never let the row block overflow the chart band (block = (n-1)*pitch + text_h
            // <= chart_h): when there are more rows than fit at the natural pitch, compress
            // so the top/bottom rows stay inside the canvas instead of poking the button.
            float max_fit = (float)(chart_h - text_h) / (float)(n - 1);
            return natural < max_fit ? natural : max_fit;
        };
        // Float row center (snapped later about vcf), symmetric about the mid-line.
        auto row_center_yf = [&](int n, int k, float pitch) -> float {
            return vcf + ((float)k - (float)(n - 1) / 2.0f) * pitch;
        };
        const float in_pitch  = row_pitch(n_in);
        const float out_pitch = row_pitch(n_dest);

        // ---- The canvas hosts the vector diagram; labels ride on top as real widgets
        // (crisp + i18n/shaping-correct, and untouched by the per-frame repaint).
        void *buf = lv_malloc(LV_CANVAS_BUF_SIZE(W, chart_h, 16, LV_DRAW_BUF_STRIDE_ALIGN));
        lv_obj_t *canvas = lv_canvas_create(screen.screen);
        lv_canvas_set_buffer(canvas, buf, W, chart_h, LV_COLOR_FORMAT_RGB565);
        lv_canvas_fill_bg(canvas, lv_color_hex(BACKGROUND_COLOR), LV_OPA_COVER);
        lv_obj_set_pos(canvas, 0, chart_top);
        lv_obj_set_style_pad_all(canvas, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(canvas, 0, LV_PART_MAIN);
        lv_obj_remove_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(canvas, LV_OBJ_FLAG_CLICKABLE);

        auto make_chart_label = [&](const std::string &s, int32_t x, int32_t y) {
            lv_obj_t *lbl = lv_label_create(canvas);
            lv_obj_set_style_pad_all(lbl, 0, LV_PART_MAIN);
            lv_obj_set_style_text_font(lbl, &BODY_FONT, LV_PART_MAIN);
            lv_obj_set_style_text_color(lbl, lv_color_hex(PSBT_LABEL_COLOR), LV_PART_MAIN);
            lv_label_set_text(lbl, s.c_str());
            lv_obj_set_pos(lbl, x, y);
        };

        // ---- Input rows: label right-justified to the balanced text gap + a two-arc
        // S-curve to the center bar (built in float, snapped symmetric about vcf).
        std::vector<std::vector<lv_point_t>> input_curves;
        for (int k = 0; k < n_in; ++k) {
            const std::string &s = inputs_column[k];
            int32_t tw     = text_w(s);
            float   row_cy = row_center_yf(n_in, k, in_pitch);
            make_chart_label(s, inputs_text_right - tw, (int32_t)lroundf(row_cy) - text_h / 2);

            psbt_fpt start_pt = { (float)input_start_x, row_cy };
            psbt_fpt conj     = { (float)center_bar_x, vcf };   // input hub = left end of the center bar

            std::vector<psbt_fpt> fcurve;
            if (num_inputs == 1) {
                // One input: a plain straight segment — no curve logic (row_cy == vcf).
                fcurve = { start_pt, conj };
            } else {
                // tip = text, hub = center; stored text-first (the pulse enters at the tip).
                fcurve = psbt_branch(start_pt, conj, 2 * PSBT_CURVE_STEPS);
            }
            input_curves.push_back(psbt_snap_curve(fcurve, vc, vcf));
        }

        // ---- Output rows: label left-justified past the balanced text gap + a two-arc
        // S-curve from the center bar.
        std::vector<std::vector<lv_point_t>> output_curves;
        for (int k = 0; k < n_dest; ++k) {
            const std::string &s = destination_column[k];
            float row_cy = row_center_yf(n_dest, k, out_pitch);
            make_chart_label(s, destination_col_x, (int32_t)lroundf(row_cy) - text_h / 2);

            psbt_fpt conj   = { (float)dest_conj_x, vcf };   // output hub = right end of the center bar
            psbt_fpt end_pt = { (float)output_end_x, row_cy };

            // Mirror of the input side: radiate from the hub out to the label. Build
            // tip-first, then reverse so the stored curve runs hub -> tip (the pulse
            // enters at the hub and travels out to the recipient).
            std::vector<psbt_fpt> fcurve = psbt_branch(end_pt, conj, 2 * PSBT_CURVE_STEPS);
            std::reverse(fcurve.begin(), fcurve.end());
            output_curves.push_back(psbt_snap_curve(fcurve, vc, vcf));
        }

        // ---- Segment the center bar so the pulse can step across it (straight, at vcf).
        psbt_fpt cbf_start = { (float)center_bar_x, vcf };
        psbt_fpt cbf_end   = { (float)dest_conj_x, vcf };
        std::vector<lv_point_t> center_bar = psbt_snap_curve(
            { cbf_start,
              psbt_lerp(cbf_start, cbf_end, 0.25f),
              psbt_lerp(cbf_start, cbf_end, 0.50f),
              psbt_lerp(cbf_start, cbf_end, 0.75f),
              cbf_end }, vc, vcf);

        // ---- Paint the static gray diagram once, then snapshot it as the pristine
        // restore source the pulse copies back each frame (fringe-free erase).
        const size_t buf_bytes = LV_CANVAS_BUF_SIZE(W, chart_h, 16, LV_DRAW_BUF_STRIDE_ALIGN);
        lv_layer_t layer;
        lv_canvas_init_layer(canvas, &layer);
        for (const auto &c : input_curves)  psbt_draw_polyline(&layer, c, PSBT_ASSOC_COLOR);
        psbt_line(&layer, { center_bar_x, vc }, { center_bar_x + center_bar_width, vc }, PSBT_ASSOC_COLOR);
        for (const auto &c : output_curves) psbt_draw_polyline(&layer, c, PSBT_ASSOC_COLOR);
        lv_canvas_finish_layer(canvas, &layer);

        void *pristine = lv_malloc(buf_bytes);
        memcpy(pristine, buf, buf_bytes);

        // ---- Hand the curves to the wall-clock pulse timer (torn down on screen delete).
        psbt_anim_ctx_t *actx = new psbt_anim_ctx_t();
        actx->canvas       = canvas;
        actx->canvas_buf   = buf;
        actx->pristine_buf = pristine;
        actx->buf_bytes    = buf_bytes;
        actx->inputs       = std::move(input_curves);
        actx->outputs      = std::move(output_curves);
        actx->center_bar   = std::move(center_bar);
        const int in_segs  = (int)actx->inputs[0].size() - 1;
        const int ctr_segs = (int)actx->center_bar.size() - 1;
        const int out_segs = (int)actx->outputs[0].size() - 1;
        actx->total_segs   = in_segs + ctr_segs + out_segs;
        actx->band_segs    = std::max(1, (int)lroundf(PSBT_PULSE_BAND_FRAC * (float)actx->total_segs));
        actx->reset_at     = actx->total_segs + actx->band_segs
                             + std::max(1, (int)lroundf(PSBT_PULSE_GAP_FRAC * (float)actx->total_segs));
        actx->head         = 0.0f;
        actx->last_tick    = lv_tick_get();
        actx->timer        = lv_timer_create(psbt_pulse_timer_cb, PSBT_PULSE_TICK_MS, actx);
        lv_obj_add_event_cb(screen.screen, psbt_cleanup_cb, LV_EVENT_DELETE, actx);
    }

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


// ============================ PSBT detail screens ============================
// The transaction-review leaf screens: the per-recipient / change-address readouts
// and the fee "math". Each is a bottom-pinned ButtonListScreen (Python parity) whose
// body composes the shared btc_amount + formatted_address components. The host formats
// every value (denomination, digit grouping, address derivation); these screens only
// lay the pieces out — the same host-formats / C-renders split as btc_amount, so the
// two platforms can never disagree on how a number rounds or an address truncates.


// PSBTAddressDetailsScreen: one recipient's amount over its full (wrapped) address,
// vertically centered between the top nav and the action button. The View supplies the
// title ("Verify Send Address" etc.) and the button label.
//
// cfg:
//   top_nav.title            — screen title (default "Address").
//   btc_amount { ... }       — the send amount (btc_amount_from_cfg contract).
//   address (string, req.)   — the full destination address.
//   button_list (array)      — action buttons (default ["Next"]).
void psbt_address_details_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;
    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    if (!cfg.contains("address") || !cfg["address"].is_string()) {
        throw std::runtime_error("psbt_address_details_screen requires an \"address\" string");
    }
    std::string address = cfg["address"].get<std::string>();

    // Bottom-pinned button-list shape (Python is_bottom_list = True).
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "Address";
    cfg["is_bottom_list"] = true;
    if (!cfg.contains("button_list")) cfg["button_list"] = json::array({ "Next" });

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);

    // Center the [amount / address] block vertically in the gap above the button, the
    // same way seed_finalize centers its fingerprint readout: grow upper_body to claim
    // the gap, center on both axes, collapse the scaffold spacer. The COMPONENT_PADDING
    // row gap matches Python's amount->address spacing.
    lv_obj_set_flex_grow(screen.upper_body, 1);
    lv_obj_set_flex_align(screen.upper_body, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen.upper_body, COMPONENT_PADDING, LV_PART_MAIN);
    if (screen.button_list_spacer) lv_obj_set_flex_grow(screen.button_list_spacer, 0);

    if (cfg.contains("btc_amount") && cfg["btc_amount"].is_object()) {
        btc_amount_from_cfg(screen.upper_body, cfg["btc_amount"]);
    }

    const int32_t W = lv_display_get_horizontal_resolution(NULL);
    formatted_address_opts_t fo = {};
    fo.address      = address.c_str();
    fo.width        = W - 2 * EDGE_PADDING;
    fo.max_lines    = 0;                                        // wrap to as many lines as needed
    fo.accent_color = network_color(resolve_address_network(cfg, address));   // head/tail = network color
    fo.base_color   = SEEDSIGNER_ICON_COLOR_DEFAULT;
    formatted_address(screen.upper_body, &fo);

    bind_screen_navigation(cfg, screen,
        screen.button_list_count > 0 ? screen.button_list : NULL,
        screen.button_list_count, NAV_BODY_VERTICAL, 0);
    load_screen_and_cleanup_previous(screen.screen);
}


// PSBTChangeDetailsScreen: the change (or self-receive) output — amount, an address-type
// label ("change address #N"), the single-line address, and an optional "Address
// verified!" confirmation centered in the space above the button. Top-anchored (Python
// pins the stack under the top nav rather than centering it).
//
// cfg:
//   top_nav.title              — screen title (default "Your Change").
//   btc_amount { ... }         — the change amount.
//   address (string, req.)     — the change/self-receive address.
//   address_type_label (str)   — host-formatted "change address #0" / "receive address #5".
//   is_verified (bool)         — show the "Address verified!" confirmation.
//   verified_text (str)        — host-localized confirmation (default "Address verified!").
//   button_list (array)        — action buttons.
void psbt_change_details_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;
    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    if (!cfg.contains("address") || !cfg["address"].is_string()) {
        throw std::runtime_error("psbt_change_details_screen requires an \"address\" string");
    }
    std::string address       = cfg["address"].get<std::string>();
    std::string type_label    = cfg.value("address_type_label", std::string("change address #0"));
    bool        is_verified   = cfg.value("is_verified", false);
    std::string verified_text = cfg.value("verified_text", std::string("Address verified!"));

    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "Your Change";
    cfg["is_bottom_list"] = true;
    if (!cfg.contains("button_list")) cfg["button_list"] = json::array({ "Done" });

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);

    const int32_t W = lv_display_get_horizontal_resolution(NULL);

    // Top-anchored stack (default upper_body flow, cross-centered). Python spacing:
    // amount COMPONENT_PADDING below the nav, the type label COMPONENT_PADDING below the
    // amount, and the address directly under the label. Zero the row gap and place the
    // label's own top margin so the two gaps differ.
    lv_obj_set_style_pad_top(screen.upper_body, COMPONENT_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_row(screen.upper_body, 0, LV_PART_MAIN);

    if (cfg.contains("btc_amount") && cfg["btc_amount"].is_object()) {
        btc_amount_from_cfg(screen.upper_body, cfg["btc_amount"]);
    }

    // Small gray "change address #N" / "receive address #N" label. Body font (locale-
    // aware) in the label color, matching the seed_finalize label precedent.
    lv_obj_t *tlabel = lv_label_create(screen.upper_body);
    lv_label_set_text(tlabel, type_label.c_str());
    lv_obj_set_style_text_font(tlabel, &BODY_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(tlabel, lv_color_hex(LABEL_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_pad_all(tlabel, 0, LV_PART_MAIN);
    lv_obj_set_style_margin_top(tlabel, COMPONENT_PADDING, LV_PART_MAIN);

    // Single-line head…tail address (Python max_lines=1).
    formatted_address_opts_t fo = {};
    fo.address      = address.c_str();
    fo.width        = W - 2 * EDGE_PADDING;
    fo.max_lines    = 1;
    fo.accent_color = network_color(resolve_address_network(cfg, address));   // head/tail = network color
    fo.base_color   = SEEDSIGNER_ICON_COLOR_DEFAULT;
    lv_obj_t *addr = formatted_address(screen.upper_body, &fo);

    // Optional "Address verified!" line — a green success glyph + the confirmation text,
    // centered in the gap between the address and the button (Python auto-centers its
    // IconTextLine within that available height). Built on the non-flex screen root and
    // positioned by measurement so it floats mid-gap rather than hugging the address.
    lv_obj_t *vrow = nullptr;
    if (is_verified) {
        vrow = lv_obj_create(screen.screen);
        lv_obj_remove_style_all(vrow);
        lv_obj_set_size(vrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_layout(vrow, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(vrow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(vrow, COMPONENT_PADDING / 2, LV_PART_MAIN);
        lv_obj_remove_flag(vrow, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(vrow, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *vic = lv_label_create(vrow);
        lv_label_set_text(vic, SeedSignerIconConstants::SUCCESS);
        lv_obj_set_style_text_font(vic, &ICON_FONT__SEEDSIGNER, LV_PART_MAIN);
        lv_obj_set_style_text_color(vic, lv_color_hex(SUCCESS_COLOR), LV_PART_MAIN);
        lv_obj_set_style_pad_all(vic, 0, LV_PART_MAIN);

        lv_obj_t *vtx = lv_label_create(vrow);
        lv_label_set_text(vtx, verified_text.c_str());
        lv_obj_set_style_text_font(vtx, &BODY_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(vtx, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_pad_all(vtx, 0, LV_PART_MAIN);
        // Force SINGLE-LINE. This is a one-line confirmation, but the default
        // LV_LABEL_LONG_WRAP makes the shaped-locale run layer wrap the run to the
        // label's content width (glyph_runs attach_runs) — and inside this tight
        // LV_SIZE_CONTENT row that width is narrow, so a long SHAPED translation
        // (e.g. Thai "ตรวจสอบที่อยู่แล้ว!") wraps and only line 0 survives in the
        // one-line-tall box. CLIP keeps wrap_width 0 so the whole run stays on one
        // line (same fix as the PSBTMath info word). The row stays content-sized,
        // so the icon+text unit still measures/centers as a tight block below.
        lv_label_set_long_mode(vtx, LV_LABEL_LONG_CLIP);
    }

    bind_screen_navigation(cfg, screen,
        screen.button_list_count > 0 ? screen.button_list : NULL,
        screen.button_list_count, NAV_BODY_VERTICAL, 0);

    // With the scaffold + content laid out, center the verified line between the address
    // bottom and the first button top.
    if (vrow) {
        lv_obj_update_layout(screen.screen);
        lv_area_t aa; lv_obj_get_coords(addr, &aa);
        int32_t gap_top    = aa.y2;
        int32_t gap_bottom = lv_display_get_vertical_resolution(NULL) - BUTTON_HEIGHT;
        if (screen.button_list_count > 0 && lv_obj_is_valid(screen.button_list[0])) {
            lv_area_t ba; lv_obj_get_coords(screen.button_list[0], &ba);
            gap_bottom = ba.y1;
        }
        int32_t vh = lv_obj_get_height(vrow);
        int32_t vw = lv_obj_get_width(vrow);
        lv_obj_set_pos(vrow, (W - vw) / 2, (gap_top + gap_bottom) / 2 - vh / 2);
    }

    load_screen_and_cleanup_previous(screen.screen);
}


// PSBTMathScreen: the fee "math" — a right-aligned, fixed-width equation of the input
// total minus recipients minus fee, ruled off, equalling the change. In btc mode the
// trailing satoshi digits dim through three grays (Python's supersampled digit zones);
// the change line's unit is called out in orange. The host passes each amount as an
// already-formatted (unpadded) number string plus the denomination flag; this screen
// pads them to a common width so the monospace columns line up, applies the +/- signs,
// and colors the zones.
//
// cfg:
//   amounts { input, spend, fee, change }  — host-formatted number strings (unpadded).
//   denomination ("btc" | "sats")          — selects the 3-zone digit dimming.
//   num_recipients (int)                    — >0 renders the "- spend" recipients row.
//   labels { inputs, recipients, fee, change } — host-localized (already pluralized) info
//                                            words drawn after each amount.
//   button_list (array)                     — default ["Review recipients"].
void psbt_math_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;
    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    const json amounts = (cfg.contains("amounts") && cfg["amounts"].is_object()) ? cfg["amounts"] : json::object();
    std::string s_input  = amounts.value("input",  std::string("0"));
    std::string s_spend  = amounts.value("spend",  std::string("0"));
    std::string s_fee    = amounts.value("fee",    std::string("0"));
    std::string s_change = amounts.value("change", std::string("0"));

    bool is_btc = cfg.value("denomination", std::string("sats")) == "btc";
    int  num_recipients = cfg.value("num_recipients", 1);

    const json labels = (cfg.contains("labels") && cfg["labels"].is_object()) ? cfg["labels"] : json::object();
    std::string l_inputs     = labels.value("inputs",     std::string("inputs"));
    std::string l_recipients = labels.value("recipients", std::string("recipients"));
    std::string l_fee        = labels.value("fee",        std::string("fee"));
    std::string l_change     = labels.value("change",     std::string("change"));

    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "Transaction Math";
    cfg["is_bottom_list"] = true;
    if (!cfg.contains("button_list")) cfg["button_list"] = json::array({ "Review recipients" });

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);

    const int32_t W = lv_display_get_horizontal_resolution(NULL);
    const lv_font_t *digit_font = &KEYBOARD_FONT;   // profile fixed-width (Inconsolata)

    // Fixed-width metrics: one monospace advance + the digit line height.
    lv_point_t sz10;
    lv_text_get_size(&sz10, "0000000000", digit_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t char_width = sz10.x / 10; if (char_width < 1) char_width = 1;
    int32_t digit_h    = lv_font_get_line_height(digit_font);

    // Left-pad all four amounts to a common width so the monospace digits right-align
    // (Python pads to the longest before prefixing the +/- sign).
    size_t longest = std::max(std::max(s_input.size(), s_spend.size()),
                              std::max(s_fee.size(), s_change.size()));
    auto pad = [&](const std::string &s) {
        return s.size() >= longest ? s : std::string(longest - s.size(), ' ') + s;
    };
    s_input = pad(s_input); s_spend = pad(s_spend); s_fee = pad(s_fee); s_change = pad(s_change);

    int32_t dgs          = LIST_ITEM_PADDING / 2; if (dgs < 1) dgs = 1;   // digit-group gap
    int32_t digits_width = (int32_t)(longest + 1) * char_width;           // sign + digits
    int32_t info_x       = digits_width + 3 * dgs;                        // info text column
    int32_t row_advance  = digit_h + BODY_LINE_SPACING;

    // Measure the equation's actual content width — the digits column (info_x) plus the
    // widest info word — so a block narrower than the body is CENTERED horizontally on
    // wide screens instead of hugging the left edge. Only the info words actually rendered
    // are measured (the recipients row is dropped on self-transfers).
    auto body_text_w = [&](const std::string &s) -> int32_t {
        lv_point_t sz;
        lv_text_get_size(&sz, s.c_str(), &BODY_FONT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        return sz.x;
    };
    int32_t max_info_w = std::max(body_text_w(l_inputs),
                                  std::max(body_text_w(l_fee), body_text_w(l_change)));
    if (num_recipients > 0) max_info_w = std::max(max_info_w, body_text_w(l_recipients));

    const int32_t content_w = info_x + max_info_w;   // digits column + info column
    const int32_t body_w    = W - 2 * EDGE_PADDING;

    // Horizontal centering: mc spans the FULL body width (so the info labels always have
    // room and a shaped script is never width-capped into a wrap), and the block is
    // centered by shifting every element right by center_off when it fits.
    int32_t center_off = (body_w - content_w) / 2;
    if (center_off < 0) center_off = 0;

    // Equation body container. Its y is a placeholder here — the final y is set below once
    // the block height is known (vertical centering). It floats over the scaffold's
    // (empty) upper_body; the button stays pinned at the bottom.
    lv_obj_t *mc = lv_obj_create(screen.screen);
    lv_obj_remove_style_all(mc);
    lv_obj_set_pos(mc, EDGE_PADDING, TOP_NAV_HEIGHT + COMPONENT_PADDING);
    lv_obj_set_width(mc, body_w);
    lv_obj_set_height(mc, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(mc, 0, LV_PART_MAIN);
    lv_obj_remove_flag(mc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(mc, LV_OBJ_FLAG_CLICKABLE);

    // Fixed-width digit label (Latin monospace, exact metrics — never wraps).
    auto add_digits = [&](uint32_t color, const std::string &text, int32_t x, int32_t y) {
        lv_obj_t *lbl = lv_label_create(mc);
        lv_obj_set_style_pad_all(lbl, 0, LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, digit_font, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
        lv_label_set_text(lbl, text.c_str());
        lv_obj_set_pos(lbl, center_off + x, y);
    };

    // Info word (locale-aware BODY_FONT). Forced SINGLE-LINE (LV_LABEL_LONG_CLIP) with an
    // explicit width = the room to its right, so a long SHAPED translation (e.g. the Thai
    // word for "fee") stays on one line instead of wrapping onto the rule-off line below
    // it. It clips only in the extreme case where even the full remaining width can't hold
    // it — a graceful failure vs. the overlapping wrap.
    auto add_info = [&](uint32_t color, const std::string &text, int32_t y) {
        lv_obj_t *lbl = lv_label_create(mc);
        lv_obj_set_style_pad_all(lbl, 0, LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &BODY_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(lbl, body_w - center_off - info_x);
        lv_label_set_text(lbl, text.c_str());
        lv_obj_set_pos(lbl, center_off + info_x, y);
    };

    // Render one equation row: the signed amount (dimmed satoshi zones in btc mode) then
    // the info word one column over.
    auto render_amount = [&](int32_t y, const std::string &amount, const std::string &info, uint32_t info_color) {
        if (is_btc && amount.size() > 6) {
            // Split the last 6 characters into two 3-digit zones, dimming each further
            // (Python's secondary #888 / tertiary #666 supersampled digit groups).
            std::string main_zone = amount.substr(0, amount.size() - 6);
            std::string mid_zone  = amount.substr(amount.size() - 6, 3);
            std::string end_zone  = amount.substr(amount.size() - 3);
            int32_t main_w = (int32_t)main_zone.size() * char_width;
            add_digits((uint32_t)BODY_FONT_COLOR, main_zone, 0, y);
            add_digits(0x888888, mid_zone, main_w + dgs, y);
            add_digits(0x666666, end_zone, main_w + dgs + 3 * char_width + dgs, y);
        } else {
            add_digits((uint32_t)BODY_FONT_COLOR, amount, 0, y);
        }
        add_info(info_color, info, y);
    };

    int32_t cur_y = 0;
    render_amount(cur_y, std::string(" ") + s_input, l_inputs, (uint32_t)BODY_FONT_COLOR);

    // The spend line is omitted on self-transfers (no external recipient).
    if (num_recipients > 0) {
        cur_y += row_advance;
        render_amount(cur_y, std::string("-") + s_spend, l_recipients, (uint32_t)BODY_FONT_COLOR);
    }

    cur_y += row_advance;
    render_amount(cur_y, std::string("-") + s_fee, l_fee, (uint32_t)BODY_FONT_COLOR);

    // Rule-off line (aligned + sized to the centered block), then the change total below.
    cur_y += row_advance;
    int32_t divider_h = LIST_ITEM_PADDING / 4; if (divider_h < 1) divider_h = 1;
    lv_obj_t *divider = lv_obj_create(mc);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, content_w, divider_h);
    lv_obj_set_style_bg_color(divider, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_pos(divider, center_off, cur_y);

    cur_y += BODY_LINE_SPACING;
    render_amount(cur_y, std::string(" ") + s_change, l_change, 0xff8c00 /* darkorange */);

    bind_screen_navigation(cfg, screen,
        screen.button_list_count > 0 ? screen.button_list : NULL,
        screen.button_list_count, NAV_BODY_VERTICAL, 0);

    // Vertical centering: now that the equation's height is known, center it in the gap
    // between the top nav and the pinned button (Python top-anchors it; here the wider
    // screens have slack to spare).
    lv_obj_update_layout(screen.screen);
    int32_t mc_h    = lv_obj_get_height(mc);
    int32_t gap_top = TOP_NAV_HEIGHT;
    int32_t gap_bot = lv_display_get_vertical_resolution(NULL) - BUTTON_HEIGHT;
    if (screen.button_list_count > 0 && lv_obj_is_valid(screen.button_list[0])) {
        lv_area_t ba; lv_obj_get_coords(screen.button_list[0], &ba);
        gap_bot = ba.y1;
    }
    int32_t mc_y = gap_top + (gap_bot - gap_top - mc_h) / 2;
    if (mc_y < gap_top + COMPONENT_PADDING) mc_y = gap_top + COMPONENT_PADDING;   // clear the nav
    lv_obj_set_y(mc, mc_y);

    load_screen_and_cleanup_previous(screen.screen);
}


// SeedExportXpubDetailsScreen: the xpub-export summary — fingerprint, derivation path,
// and the (truncated) xpub, each an IconTextLine, stacked top-anchored under the nav.
// WarningEdgesMixin frames it in pulsing YELLOW (WARNING_COLOR): an xpub leaks viewable
// transaction history — a privacy caution, not a key-material dire warning.
//
// cfg:
//   top_nav.title             — default "Xpub Details".
//   fingerprint (str, req.)   — BIP-32 master fingerprint (hex).
//   derivation_path (str)     — default "m/84'/0'/0'".
//   xpub (str, req.)          — the extended pubkey; truncated here to one line.
//   fingerprint_label / derivation_label / xpub_label (str) — host-localized field labels.
//   button_list (array)       — default ["Export xpub"].
void seed_export_xpub_details_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;
    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    if (!cfg.contains("fingerprint") || !cfg["fingerprint"].is_string()) {
        throw std::runtime_error("seed_export_xpub_details_screen requires a \"fingerprint\" string");
    }
    if (!cfg.contains("xpub") || !cfg["xpub"].is_string()) {
        throw std::runtime_error("seed_export_xpub_details_screen requires an \"xpub\" string");
    }
    std::string fingerprint       = cfg["fingerprint"].get<std::string>();
    std::string derivation_path   = cfg.value("derivation_path", std::string("m/84'/0'/0'"));
    std::string xpub              = cfg["xpub"].get<std::string>();
    std::string fingerprint_label = cfg.value("fingerprint_label", std::string("Fingerprint"));
    std::string derivation_label  = cfg.value("derivation_label",  std::string("Derivation"));
    std::string xpub_label        = cfg.value("xpub_label",        std::string("Xpub"));

    // Bottom-pinned button-list shape (Python is_bottom_list = True).
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "Xpub Details";
    cfg["is_bottom_list"] = true;
    if (!cfg.contains("button_list")) cfg["button_list"] = json::array({ "Export xpub" });

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);

    // Top-anchored, LEFT-aligned column of the three lines (Python: screen_x =
    // COMPONENT_PADDING, first line COMPONENT_PADDING below the nav). Grow upper_body to
    // claim the whole gap above the button and collapse the scaffold spacer, so the block
    // sits in a container sized to fit rather than a shrink-wrapped one that scrolls its top
    // under the nav. icon_text_line reclaims the LVGL line-height leading so three lines pack
    // at PIL density; the inter-line gap is COMPONENT_PADDING (Python's 1.5x would leave the
    // xpub line hugging the button on the 240 body).
    lv_obj_set_flex_grow(screen.upper_body, 1);
    lv_obj_set_flex_align(screen.upper_body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_top(screen.upper_body, COMPONENT_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_left(screen.upper_body, COMPONENT_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_row(screen.upper_body, COMPONENT_PADDING, LV_PART_MAIN);
    if (screen.button_list_spacer) lv_obj_set_flex_grow(screen.button_list_spacer, 0);

    // Fingerprint line — seedsigner FINGERPRINT glyph, body-font value.
    icon_text_line_opts_t fp = {};
    fp.icon_glyph   = SeedSignerIconConstants::FINGERPRINT;
    fp.icon_color   = INFO_COLOR;
    fp.label_text   = fingerprint_label.c_str();
    fp.value_text   = fingerprint.c_str();
    fp.label_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> LABEL_FONT_COLOR (gray)
    fp.value_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> BODY_FONT_COLOR
    fp.icon_width   = ICON_FONT_SIZE;                  // shared icon column -> aligned text
    icon_text_line(screen.upper_body, &fp);

    // Derivation line — seedsigner DERIVATION glyph, body-font value.
    icon_text_line_opts_t dv = {};
    dv.icon_glyph   = SeedSignerIconConstants::DERIVATION;
    dv.icon_color   = INFO_COLOR;
    dv.label_text   = derivation_label.c_str();
    dv.value_text   = derivation_path.c_str();
    dv.label_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;
    dv.value_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;
    dv.icon_width   = ICON_FONT_SIZE;
    icon_text_line(screen.upper_body, &dv);

    // Xpub line. Python's icon is FontAwesomeIconConstants.X == the ASCII letter "X" drawn
    // in the bold FontAwesome font; render it as a bold monospace "X". The value is the
    // xpub in the fixed-width font (Python FIXED_WIDTH at body+2; the baked 22 px monospace
    // here), truncated to fill one line. Truncation is measured HERE so it tracks each
    // display profile's char width — Python's num_chars math.
    const lv_font_t *xpub_font = &CANDIDATE_FONT;   // Inconsolata SemiBold, 22 px @240
    lv_point_t sz10;
    lv_text_get_size(&sz10, "0000000000", xpub_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t char_w = sz10.x / 10;
    if (char_w < 1) char_w = 1;
    const int32_t W = lv_display_get_horizontal_resolution(NULL);
    int num_chars = (int)((W - ICON_FONT_SIZE - 2 * COMPONENT_PADDING) / char_w) - 3;  // -3 = "..."
    if (num_chars < 1) num_chars = 1;
    if (num_chars > (int)xpub.size()) num_chars = (int)xpub.size();
    std::string xpub_display = xpub.substr(0, (size_t)num_chars) + "...";

    icon_text_line_opts_t xp = {};
    xp.icon_glyph   = "X";                 // Python FontAwesomeIconConstants.X = U+0058
    xp.icon_font    = &KEYBOARD_FONT;      // bold 24 px monospace X
    xp.icon_color   = INFO_COLOR;
    xp.label_text   = xpub_label.c_str();
    xp.value_text   = xpub_display.c_str();
    xp.value_font   = xpub_font;
    xp.label_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;
    xp.value_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;
    xp.icon_width   = ICON_FONT_SIZE;      // same column as fingerprint/derivation -> "X" centers, text aligns
    icon_text_line(screen.upper_body, &xp);

    // WarningEdgesMixin — pulsing yellow border.
    add_warning_edges_overlay(screen.screen, WARNING_COLOR);

    bind_screen_navigation(cfg, screen,
        screen.button_list_count > 0 ? screen.button_list : NULL,
        screen.button_list_count, NAV_BODY_VERTICAL, 0);
    load_screen_and_cleanup_previous(screen.screen);
}


// Wrap a passphrase into up to `max_lines` lines of at most `max_cpl` monospace chars.
// Two goals beyond Python's naive fixed-width char split:
//   1. Break right AFTER a special (non-alphanumeric) character — dash, punctuation,
//      space — when one sits near the balanced break point, so a human-readable passphrase
//      wraps at its natural separators ("correct-horse-" / ...) instead of mid-word.
//   2. Balance the line lengths (each break targets k*len/num_lines), like the body-text
//      balanced wrap.
// A random or all-alphabetic passphrase has no special-char breakpoints, so it falls back
// to a balanced hard split by length. EVERY character is preserved — the break character
// stays at the end of its line — since the whole point is to show the exact passphrase.
static std::vector<std::string> wrap_passphrase(const std::string& p, int max_cpl, int max_lines) {
    const int len = (int)p.size();
    if (max_cpl < 1 || len <= max_cpl) return { p };

    int num_lines = (len + max_cpl - 1) / max_cpl;   // ceil: fewest lines that can fit
    if (num_lines > max_lines) num_lines = max_lines;
    if (num_lines < 1) num_lines = 1;

    auto is_special = [](char c) {
        return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
    };
    auto dist = [](int a, int b) { int d = a - b; return d < 0 ? -d : d; };

    std::vector<int> cuts;   // start index of each new line
    int prev = 0;
    for (int k = 1; k < num_lines; ++k) {
        const int ideal = (int)((long)len * k / num_lines);   // balanced target cut
        int lo = prev + 1;                                    // ≥1 char on this line
        int hi = prev + max_cpl;                              // this line ≤ max_cpl
        // Leave room: the remaining text must fit the remaining lines, ≥1 char each.
        const int rem_lines = num_lines - k;
        if (hi > len - rem_lines)            hi = len - rem_lines;
        if (lo < len - rem_lines * max_cpl)  lo = len - rem_lines * max_cpl;
        if (lo < prev + 1)                   lo = prev + 1;
        if (hi < lo)                         hi = lo;

        // Prefer a break just after a special char, closest to `ideal`, within [lo, hi].
        int best = -1;
        for (int pos = lo; pos <= hi && pos < len; ++pos) {
            if (is_special(p[pos - 1]) && (best < 0 || dist(pos, ideal) < dist(best, ideal))) {
                best = pos;
            }
        }
        int cut = (best >= 0) ? best : std::min(std::max(ideal, lo), hi);   // else balanced hard cut
        cuts.push_back(cut);
        prev = cut;
    }

    std::vector<std::string> lines;
    int start = 0;
    for (int c : cuts) { lines.push_back(p.substr((size_t)start, (size_t)(c - start))); start = c; }
    lines.push_back(p.substr((size_t)start));
    return lines;
}


// SeedReviewPassphraseScreen: shows the entered BIP-39 passphrase (orange, fixed-width,
// centered, up to 3 lines) above a fingerprint IconTextLine that spells out how the
// passphrase CHANGES the seed's fingerprint (without >> with). No warning edges.
//
// cfg:
//   top_nav.title                 — default "Verify Passphrase".
//   passphrase (str, req.)        — the passphrase to review.
//   fingerprint_without (str,req) — fingerprint before the passphrase.
//   fingerprint_with (str, req.)  — fingerprint after the passphrase.
//   changes_fingerprint_label(str)— host-localized "changes fingerprint" label.
//   button_list (array)           — default ["Done"].
void seed_review_passphrase_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;
    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    if (!cfg.contains("passphrase") || !cfg["passphrase"].is_string()) {
        throw std::runtime_error("seed_review_passphrase_screen requires a \"passphrase\" string");
    }
    std::string passphrase = cfg["passphrase"].get<std::string>();
    std::string fp_without = cfg.value("fingerprint_without", std::string(""));
    std::string fp_with    = cfg.value("fingerprint_with",    std::string(""));
    std::string changes_label = cfg.value("changes_fingerprint_label",
                                          std::string("changes fingerprint"));

    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "Verify Passphrase";
    cfg["is_bottom_list"] = true;
    if (!cfg.contains("button_list")) cfg["button_list"] = json::array({ "Done" });

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);

    const int32_t W = lv_display_get_horizontal_resolution(NULL);

    // The fingerprint readout: FINGERPRINT glyph + "changes fingerprint" over
    // "<without> >> <with>", centered as a block (positioned by measurement below).
    std::string fp_value = fp_without + " >> " + fp_with;
    icon_text_line_opts_t fo = {};
    fo.icon_glyph   = SeedSignerIconConstants::FINGERPRINT;
    fo.icon_color   = INFO_COLOR;
    fo.label_text   = changes_label.c_str();
    fo.value_text   = fp_value.c_str();
    fo.label_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;
    fo.value_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;
    fo.is_text_centered = true;
    lv_obj_t *fp_line = icon_text_line(screen.screen, &fo);

    // The passphrase itself: orange, fixed-width, centered, up to 3 balanced lines. The
    // user fixed the size at the baked 24 px monospace (Python auto-fits a range; we don't).
    // If the passphrase has leading/trailing or doubled spaces, ALL spaces become the block
    // glyph ▉ (Python ▉) so they can't hide — matched on the raw (ASCII) string, then
    // substituted per line so the line split stays byte-safe.
    const lv_font_t *pp_font = &KEYBOARD_FONT;      // Inconsolata SemiBold, 24 px @240
    const bool show_spaces =
        (!passphrase.empty() && (passphrase.front() == ' ' || passphrase.back() == ' ')) ||
        (passphrase.find("  ") != std::string::npos);

    lv_point_t ppsz;
    lv_text_get_size(&ppsz, "0000000000", pp_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t pp_char_w = ppsz.x / 10;
    if (pp_char_w < 1) pp_char_w = 1;

    int max_cpl = (int)((W - 2 * EDGE_PADDING) / pp_char_w);
    if (max_cpl < 1) max_cpl = 1;
    std::vector<std::string> lines = wrap_passphrase(passphrase, max_cpl, 3);  // Python max_lines = 3

    // Join the wrapped lines into one '\n'-separated string, substituting the block glyph ▉
    // for spaces when the passphrase has hidden edge/doubled spaces. A SINGLE multi-line
    // label (vs one label per line) lets tight_line_space() set a uniform, Python-tight line
    // advance; per-line labels stacked at the font's loose declared line_height left too
    // much air between the wrapped lines.
    std::string joined;
    for (std::string line : lines) {
        if (show_spaces) {
            std::string sub;
            for (char c : line) sub += (c == ' ') ? "\xE2\x96\x89" /* ▉ U+2589 */ : std::string(1, c);
            line = sub;
        }
        if (!joined.empty()) joined += "\n";
        joined += line;
    }

    // Centered container holding the passphrase block (positioned by measurement below).
    lv_obj_t *pp_col = lv_obj_create(screen.screen);
    lv_obj_remove_style_all(pp_col);
    lv_obj_set_size(pp_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(pp_col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(pp_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(pp_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(pp_col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *pp_lbl = lv_label_create(pp_col);
    lv_label_set_text(pp_lbl, joined.c_str());
    lv_obj_set_style_text_font(pp_lbl, pp_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(pp_lbl, lv_color_hex(0xFFA500), LV_PART_MAIN);  // PIL "orange"
    lv_obj_set_style_text_align(pp_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);    // center each line
    lv_obj_set_style_pad_all(pp_lbl, 0, LV_PART_MAIN);
    // Uniform, Python-tight inter-line advance (ink height + 2) instead of the font's loose
    // declared line_height. Measured across the whole block, so descenders never clip.
    lv_obj_set_style_text_line_space(pp_lbl, tight_line_space(pp_font, joined.c_str(), 2),
                                     LV_PART_MAIN);

    bind_screen_navigation(cfg, screen,
        screen.button_list_count > 0 ? screen.button_list : NULL,
        screen.button_list_count, NAV_BODY_VERTICAL, 0);

    // Position both blocks now that sizes are known. The fingerprint line sits just above
    // the button (Python: button_top - COMPONENT_PADDING - body_font_size*2.5); the
    // passphrase centers in the gap between the top nav and the fingerprint line.
    lv_obj_update_layout(screen.screen);
    int32_t button_top = lv_display_get_vertical_resolution(NULL) - BUTTON_HEIGHT;
    if (screen.button_list_count > 0 && lv_obj_is_valid(screen.button_list[0])) {
        lv_area_t ba; lv_obj_get_coords(screen.button_list[0], &ba);
        button_top = ba.y1;
    }

    int32_t fp_h = lv_obj_get_height(fp_line);
    int32_t fp_w = lv_obj_get_width(fp_line);
    int32_t fp_y = button_top - COMPONENT_PADDING - (int32_t)(BODY_FONT_SIZE * 2.5);
    lv_obj_set_pos(fp_line, (W - fp_w) / 2, fp_y);

    int32_t pp_h = lv_obj_get_height(pp_col);
    int32_t pp_w = lv_obj_get_width(pp_col);
    int32_t region_top = TOP_NAV_HEIGHT;
    int32_t pp_y = region_top + (fp_y - region_top - pp_h) / 2;
    if (pp_y < region_top) pp_y = region_top;
    lv_obj_set_pos(pp_col, (W - pp_w) / 2, pp_y);

    load_screen_and_cleanup_previous(screen.screen);
}


// SeedWordsScreen: one page of a seed's words, each numbered in a rounded chip. Paginated
// by the host (one screen render per page). WarningEdgesMixin frames it in pulsing ORANGE
// (DIRE_WARNING_COLOR) — the seed itself is on screen, the most sensitive material there is.
//
// cfg:
//   top_nav.title      — default "Seed Words: {page_index+1}/{num_pages}".
//   words (array, req) — the words shown on THIS page.
//   page_index (int)   — 0-based page number (default 0), for the default numbering.
//   num_pages (int)    — total pages (default 1), for the default title.
//   start_number (int) — 1-based number of the first word on this page. Default
//                        page_index*len(words)+1 (Python's page_index*words_per_page+...).
//   button_list (array)— default ["Done"].
void seed_words_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;
    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    if (!cfg.contains("words") || !cfg["words"].is_array() || cfg["words"].empty()) {
        throw std::runtime_error("seed_words_screen requires a non-empty \"words\" array");
    }
    std::vector<std::string> words;
    for (const auto &w : cfg["words"]) words.push_back(w.get<std::string>());

    int page_index   = cfg.value("page_index", 0);
    int num_pages    = cfg.value("num_pages", 1);
    int start_number = cfg.value("start_number", page_index * (int)words.size() + 1);

    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) {
        cfg["top_nav"]["title"] = "Seed Words: " + std::to_string(page_index + 1) +
                                  "/" + std::to_string(num_pages);
    }
    cfg["is_bottom_list"] = true;
    if (!cfg.contains("button_list")) cfg["button_list"] = json::array({ "Done" });

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);

    // Fonts: word = body font at top_nav_title+2 (Python get_top_nav_title_font_size()+2);
    // number = body font at button size. Seed words are BIP-39 wordlist tokens (ASCII), so
    // the Latin body font is an exact match to Python's body font for this content.
    // seedsigner_latin_font() takes an UNSCALED base px and applies the profile multiplier
    // itself, so pass the 240-base sizes (20+2, 18) — NOT the already-scaled *_FONT_SIZE
    // macros, which would double-scale (84 px words at the 800x480 profile).
    const lv_font_t *word_font   = seedsigner_latin_font(20 + 2);
    const lv_font_t *number_font = seedsigner_latin_font(18);

    // Square number chip sized to the widest number ("24"), like Python.
    lv_point_t nsz;
    lv_text_get_size(&nsz, "24", number_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t box = nsz.x + COMPONENT_PADDING / 2;

    // The word list is one content-sized column, centered horizontally in the body and
    // top-anchored (Python top-anchors the block under the nav). Rows are LEFT-aligned so
    // every chip shares a left edge and every word starts at the same x; the block as a
    // whole centers on the widest word. Grow upper_body to claim the whole gap above the
    // button and collapse the scaffold spacer, so the block is top-anchored in a container
    // sized to fit — otherwise four rows overflow the shrink-wrapped upper_body on the 240
    // body and scroll the first word up under the nav.
    lv_obj_set_flex_grow(screen.upper_body, 1);
    lv_obj_set_flex_align(screen.upper_body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(screen.upper_body, COMPONENT_PADDING / 2, LV_PART_MAIN);
    if (screen.button_list_spacer) lv_obj_set_flex_grow(screen.button_list_spacer, 0);

    lv_obj_t *list = lv_obj_create(screen.upper_body);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    // Compressed inter-row gap (Python's 1.5*COMPONENT_PADDING reads too loose here, and
    // four rows must fit the 240 body); the word-leading reclaim below makes each row pack
    // to the chip height rather than the taller word line box.
    lv_obj_set_style_pad_row(list, COMPONENT_PADDING, LV_PART_MAIN);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    // Collected per row so we can baseline-align the words to their numbers after layout.
    std::vector<lv_obj_t*> num_labels, word_labels;

    for (size_t i = 0; i < words.size(); ++i) {
        // Row: [number chip][word], vertically centered on each other.
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_layout(row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, COMPONENT_PADDING, LV_PART_MAIN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Rounded number chip — dark button-fill, radius 5, the (INFO_COLOR) number centered.
        lv_obj_t *chip = lv_obj_create(row);
        lv_obj_remove_style_all(chip);
        lv_obj_set_size(chip, box, box);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(chip, lv_color_hex(BUTTON_BACKGROUND_COLOR), LV_PART_MAIN);
        lv_obj_set_style_radius(chip, 5, LV_PART_MAIN);
        lv_obj_set_layout(chip, LV_LAYOUT_FLEX);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *num = lv_label_create(chip);
        lv_label_set_text(num, std::to_string(start_number + (int)i).c_str());
        lv_obj_set_style_text_font(num, number_font, LV_PART_MAIN);
        lv_obj_set_style_text_color(num, lv_color_hex(INFO_COLOR), LV_PART_MAIN);
        lv_obj_set_style_pad_all(num, 0, LV_PART_MAIN);
        num_labels.push_back(num);

        // The word. Reclaim the FONT's line-height leading (uniform, not per-word) so the
        // row packs to the chip height AND every word gets the same box/baseline — a per-
        // word reclaim would align descender words (ridge, oyster) differently from plain
        // ones (muffin) against the number chip.
        lv_obj_t *word = lv_label_create(row);
        lv_label_set_text(word, words[i].c_str());
        lv_obj_set_style_text_font(word, word_font, LV_PART_MAIN);
        lv_obj_set_style_text_color(word, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_pad_all(word, 0, LV_PART_MAIN);
        reclaim_line_leading_uniform(word, word_font);
        word_labels.push_back(word);
    }

    // WarningEdgesMixin — pulsing orange border (seed on screen).
    add_warning_edges_overlay(screen.screen, DIRE_WARNING_COLOR);

    bind_screen_navigation(cfg, screen,
        screen.button_list_count > 0 ? screen.button_list : NULL,
        screen.button_list_count, NAV_BODY_VERTICAL, 0);

    // Baseline-align each word to its number: Python draws both at the same baseline_y
    // (word anchor "ls", number "ms"), so descenders drop below a shared baseline. Cross-
    // centering above aligns the BOXES, not the baselines, leaving the words sitting high;
    // shift each word down by the gap between the two text baselines (a label's baseline is
    // its box top + the font ascent = line_height - base_line).
    lv_obj_update_layout(screen.screen);
    const int32_t num_ascent  = (int32_t)lv_font_get_line_height(number_font) - number_font->base_line;
    const int32_t word_ascent = (int32_t)lv_font_get_line_height(word_font)   - word_font->base_line;
    for (size_t i = 0; i < word_labels.size() && i < num_labels.size(); ++i) {
        lv_area_t na, wa;
        lv_obj_get_coords(num_labels[i], &na);
        lv_obj_get_coords(word_labels[i], &wa);
        int32_t delta = (na.y1 + num_ascent) - (wa.y1 + word_ascent);
        if (delta != 0) lv_obj_set_style_translate_y(word_labels[i], delta, LV_PART_MAIN);
    }

    load_screen_and_cleanup_previous(screen.screen);
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


