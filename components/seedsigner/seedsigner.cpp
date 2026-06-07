#include "seedsigner.h"
#include "components.h"
#include "gui_constants.h"
#include "navigation.h"
#include "input_profile.h"

#include "lvgl.h"

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>

using json = nlohmann::json;

// Defined in components/seedsigner/images/seedsigner_logo_img.c
LV_IMAGE_DECLARE(seedsigner_logo_img);


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
static void load_screen_and_cleanup_previous(lv_obj_t *new_screen) {
    lv_obj_t *old_screen = lv_scr_act();
    lv_scr_load(new_screen);
    if (old_screen && old_screen != new_screen) {
        lv_obj_delete(old_screen);
    }
}


// Build the standard "body" container used by screens beneath TopNav.
// Most screens share the same layout/styling shell (size, alignment, padding, background,
// border, and scrollbar baseline behavior). This function encapsulates that common
// boilerplate.
static lv_obj_t* create_standard_body_content(lv_obj_t *screen, lv_obj_t *top_nav_obj, bool scrollable) {
    lv_obj_t* body_content = lv_obj_create(screen);
    lv_obj_set_size(body_content, lv_obj_get_width(screen), lv_obj_get_height(screen) - TOP_NAV_HEIGHT);
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
static void bind_screen_navigation(const json &cfg,
                                   const screen_scaffold_t &screen,
                                   lv_obj_t **body_items,
                                   size_t body_item_count,
                                   nav_body_layout_t body_layout,
                                   size_t default_initial_index) {
    bool has_input_mode_override = false;
    input_mode_t input_mode_override = INPUT_MODE_TOUCH;
    nav_mode_override_from_cfg(cfg, has_input_mode_override, input_mode_override);

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
// Three usage patterns:
//
// 1. No button_list (e.g. `main_menu_screen`, `screensaver_screen`,
//    `demo_screen`): the body is the existing non-flex container,
//    `upper_body == body`, no scaffold-managed buttons.
//
// 2. button_list present, is_bottom_list omitted/false (legacy
//    `button_list_screen` behavior): vertical-flex body with
//    `upper_body` (LV_SIZE_CONTENT, height 0 if nothing added) followed
//    directly by buttons. Buttons stack from the top.
//
// 3. button_list present, is_bottom_list=true (status / confirmation
//    screens): same as #2 plus a flex-grow=1 spacer between
//    `upper_body` and the first button. Buttons pin to the viewport
//    bottom while content fits; the spacer collapses and content/buttons
//    flow together when content overflows.
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

    // Mode 2 (legacy button list, no bottom pinning): preserve byte-identical
    // rendering with the prior `button_list_screen` implementation by using
    // the existing `button_list()` helper (top-aligned, manual chain-align).
    // `upper_body` aliases `body` so any future caller can still add content
    // — its widgets will simply share the body container with the buttons.
    if (!is_bottom_list) {
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

    // Mode 3: bottom-pinned button list. Switch body to a vertical flex
    // column with children:
    //   [0]   upper_body (LV_SIZE_CONTENT, owned by caller)
    //   [1]   flex-grow=1 spacer (collapses when upper_body overflows)
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

    out.button_list_spacer = lv_obj_create(out.body);
    lv_obj_set_width(out.button_list_spacer, lv_pct(100));
    lv_obj_set_height(out.button_list_spacer, 0);
    lv_obj_set_flex_grow(out.button_list_spacer, 1);
    lv_obj_set_style_bg_opa(out.button_list_spacer, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(out.button_list_spacer, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(out.button_list_spacer, 0, LV_PART_MAIN);
    lv_obj_remove_flag(out.button_list_spacer, LV_OBJ_FLAG_SCROLLABLE);

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


void demo_screen(void *ctx)
{
    (void)ctx;

    json cfg = {
        {"top_nav", {
            {"title", "Home"},
            {"show_back_button", false},
            {"show_power_button", true},
        }}
    };
    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, true);
    lv_obj_t *scr = screen.screen;
    lv_obj_t *body_content = screen.body;

    // debugging
    // lv_obj_set_style_border_color(body_content, lv_color_hex(BUTTON_BACKGROUND_COLOR), LV_PART_MAIN);

    static const button_list_item_t demo_buttons[] = {
        { .label = "Language", .value = NULL },
        { .label = "Persistent Settings", .value = NULL },
        { .label = "Another option", .value = NULL },
        { .label = "Wow so many options", .value = NULL },
        { .label = "Continue", .value = NULL },
    };
    lv_obj_t* lv_seedsigner_button = button_list(body_content, demo_buttons, sizeof(demo_buttons) / sizeof(demo_buttons[0]));

    lv_obj_t* lv_body_text = lv_label_create(body_content);
    lv_obj_set_width(lv_body_text, lv_obj_get_width(body_content) - 2 * COMPONENT_PADDING);
    lv_obj_align_to(lv_body_text, lv_seedsigner_button, LV_ALIGN_OUT_BOTTOM_LEFT, 0, COMPONENT_PADDING);
    lv_obj_set_style_text_color(lv_body_text, lv_color_hex(BODY_FONT_COLOR), 0);
    lv_obj_set_style_text_font(lv_body_text, &BODY_FONT, LV_PART_MAIN);
    lv_label_set_text(lv_body_text, "Long the Paris streets, the death-carts rumble, hollow and harsh. Six tumbrils carry the day's wine to La Guillotine. All the devouring and insatiate Monsters imagined since imagination could record itself, are fused in the one realisation, Guillotine. And yet there is not in France, with its rich variety of soil and climate, a blade, a leaf, a root, a sprig, a peppercorn, which will grow to maturity under conditions more certain than those that have produced this horror. Crush humanity out of shape once more, under similar hammers, and it will twist itself into the same tortured forms. Sow the same seed of rapacious license and oppression over again, and it will surely yield the same fruit according to its kind.\n\nSix tumbrils roll along the streets. Change these back again to what they were, thou powerful enchanter, Time, and they shall be seen to be the carriages of absolute monarchs, the equipages of feudal nobles, the toilettes of flaring Jezebels, the churches that are not my Father's house but dens of thieves, the huts of millions of starving peasants! No; the great magician who majestically works out the appointed order of the Creator, never reverses his transformations. \"If thou be changed into this shape by the will of God,\" say the seers to the enchanted, in the wise Arabian stories, \"then remain so! But, if thou wear this form through mere passing conjuration, then resume thy former aspect!\" Changeless and hopeless, the tumbrils roll along.");

    load_screen_and_cleanup_previous(scr);
}


// Render a screen whose body is just a vertical list of buttons.
//
// The scaffold builds the buttons from `cfg["button_list"]`; this function
// only needs to validate the required key, wire navigation, and load the
// screen.
void button_list_screen(void *ctx_json)
{
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array()) {
        throw std::runtime_error("button_list is required and must be an array");
    }

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, true);

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

    // Pulse: opacity 64 -> 255 -> 64, then a brief hold at the trough before
    // the next inhale. Python's algorithm holds 8 of 10 trough frames at
    // ~10fps (~800 ms) — we mirror that with a 400 ms repeat delay.
    //
    // LVGL v9 names: `set_duration` controls the forward leg, the matching
    // `set_reverse_duration` controls the back-to-start leg, and
    // `set_repeat_delay` is the pause inserted between iterations (i.e. at
    // the trough, since our iteration ends back at `start`).
    lv_anim_t pulse;
    lv_anim_init(&pulse);
    lv_anim_set_var(&pulse, edge);
    lv_anim_set_values(&pulse, 64, 255);
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

    // Allow the icon's negative top margin (below) to draw above body's top
    // edge — Python overlaps the icon with the top_nav by COMPONENT_PADDING/2.
    // Without OVERFLOW_VISIBLE, LVGL would clip the overflow to body's bounds.
    // We also disable scrolling on body since these screens are sized to fit.
    lv_obj_add_flag(screen.body, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(screen.body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen.body, LV_SCROLLBAR_MODE_OFF);

    // Hero icon — colored, centered, sized from the active display profile.
    // upper_body's flex layout (column, cross-axis center) handles centering.
    // Negative top margin places the icon's top half a COMPONENT_PADDING above
    // upper_body's top, mirroring Python's `top_nav.height - COMPONENT_PADDING/2`
    // anchor. Requires OVERFLOW_VISIBLE on body (set above).
    lv_obj_t *icon = lv_label_create(screen.upper_body);
    lv_label_set_text(icon, defaults.icon);
    lv_obj_set_style_text_font(icon, &ICON_PRIMARY_SCREEN_FONT__SEEDSIGNER, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, lv_color_hex(defaults.color), LV_PART_MAIN);
    lv_obj_set_style_margin_top(icon, -(COMPONENT_PADDING / 2), LV_PART_MAIN);

    // Strip default label padding so the bounding box matches the font's
    // natural line_height (= cap_height for SeedSigner icons, since the icon
    // font has base_line=0). That makes the icon's bottom edge sit exactly at
    // the bottom of the visible glyph, mirroring Python's
    // `Icon.height = -font.getbbox(..., anchor="ls")[1]` (cap height) and
    // letting subsequent flex children land where Python places them.
    lv_obj_set_style_pad_all(icon, 0, LV_PART_MAIN);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // upper_body now spans the full screen width (body padding zeroed above).
    const int32_t upper_body_content_width = lv_obj_get_width(screen.screen);

    // Optional headline — colored to match status; truncate (don't wrap) so
    // designers know to keep it short, matching Python's auto_line_break=False.
    if (cfg.contains("status_headline") && cfg["status_headline"].is_string()) {
        std::string headline = cfg["status_headline"].get<std::string>();
        if (!headline.empty()) {
            lv_obj_t *headline_label = lv_label_create(screen.upper_body);
            lv_label_set_text(headline_label, headline.c_str());
            lv_label_set_long_mode(headline_label, LV_LABEL_LONG_DOT);
            lv_obj_set_width(headline_label, upper_body_content_width);
            lv_obj_set_style_text_align(headline_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_style_text_color(headline_label, lv_color_hex(defaults.color), LV_PART_MAIN);
            lv_obj_set_style_text_font(headline_label, &BODY_FONT, LV_PART_MAIN);
            lv_obj_set_style_margin_top(headline_label, COMPONENT_PADDING / 2, LV_PART_MAIN);
        }
    }

    // Body text — wraps inside the upper_body width minus the
    // status-type-appropriate edge inset. Warning-class screens use
    // 2 * EDGE_PADDING so text never sits under the pulsing border.
    if (cfg.contains("text") && cfg["text"].is_string()) {
        std::string text = cfg["text"].get<std::string>();
        if (!text.empty()) {
            lv_obj_t *body_label = lv_label_create(screen.upper_body);
            lv_label_set_text(body_label, text.c_str());
            lv_label_set_long_mode(body_label, LV_LABEL_LONG_WRAP);

            // Inset the body text by `text_edge_padding` on each side so it
            // never sits under the pulsing warning border (warning variants
            // double the inset; success uses a single EDGE_PADDING).
            int32_t inset = defaults.text_edge_padding_multiplier * EDGE_PADDING;
            int32_t text_width = upper_body_content_width - 2 * inset;
            if (text_width < 16) {
                text_width = 16;
            }
            lv_obj_set_width(body_label, text_width);

            lv_obj_set_style_text_align(body_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_style_text_color(body_label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
            lv_obj_set_style_text_font(body_label, &BODY_FONT, LV_PART_MAIN);
            lv_obj_set_style_margin_top(body_label, COMPONENT_PADDING / 2, LV_PART_MAIN);

            // Override the screen-level BODY_LINE_SPACING (= COMPONENT_PADDING)
            // for body status text. That global value is tuned for paragraph
            // text in the demo screen; on a status screen the extra spacing
            // pushes a 3-4 line message off the bottom of the viewport. Use
            // the font's natural line height (no extra leading) instead, which
            // matches Python's TextArea default (line_spacing=None).
            lv_obj_set_style_text_line_space(body_label, 0, LV_PART_MAIN);
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


void main_menu_screen(void *ctx)
{
    // `ctx` is unused for main_menu_screen, but kept to match the shared
    // screen callback signature. Cast to void to silence unused-parameter warnings.
    (void)ctx;

    json cfg = {{"top_nav", {{"title", "Home"}, {"show_back_button", false}, {"show_power_button", true}}}};
    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false, &MAIN_MENU_TITLE_FONT);
    lv_obj_t *scr = screen.screen;
    lv_obj_t *body_content = screen.body;

    static const char *icons[] = {
        SeedSignerIconConstants::SCAN,
        SeedSignerIconConstants::SEEDS,
        SeedSignerIconConstants::TOOLS,
        SeedSignerIconConstants::SETTINGS,
    };
    static const char *labels[] = {"Scan", "Seeds", "Tools", "Settings"};

    const int32_t available_w = lv_obj_get_content_width(body_content);
    const int32_t screen_h = lv_obj_get_height(scr);

    // Match the Python LargeButtonScreen button sizing:
    //   button_height = int((canvas_height - top_nav.height - 2*COMPONENT_PADDING - EDGE_PADDING) / 2)
    int32_t button_w = (available_w - COMPONENT_PADDING) / 2;
    int32_t button_h = (screen_h - TOP_NAV_HEIGHT - 2 * COMPONENT_PADDING - EDGE_PADDING) / 2;

    // Vertically center the 2x2 grid, matching the Python LargeButtonScreen:
    //   button_start_y = top_nav_h + (canvas_h - (top_nav_h + CP) - 2*button_h - CP) / 2
    // Computed relative to the body origin (which sits at top_nav bottom).
    int32_t grid_h = 2 * button_h + COMPONENT_PADDING;
    int32_t below_nav = screen_h - TOP_NAV_HEIGHT;
    int32_t y_offset = (below_nav - COMPONENT_PADDING - 2 * button_h - COMPONENT_PADDING) / 2;

    lv_obj_t *buttons[4] = {NULL, NULL, NULL, NULL};
    for (uint32_t i = 0; i < 4; ++i) {
        lv_obj_t *btn = large_icon_button(body_content, icons[i], labels[i], NULL);
        lv_obj_set_size(btn, button_w, button_h);
        buttons[i] = btn;
    }

    // first row
    lv_obj_set_pos(
        buttons[0],
        0,
        y_offset
    );
    lv_obj_set_pos(
        buttons[1],
        button_w + COMPONENT_PADDING,
        y_offset
    );

    // second row
    lv_obj_set_pos(
        buttons[2],
        0,
        y_offset + button_h + COMPONENT_PADDING
    );
    lv_obj_set_pos(
        buttons[3],
        button_w + COMPONENT_PADDING,
        y_offset + button_h + COMPONENT_PADDING
    );

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

    // Check for touch dismiss: poll pointer input devices directly
    // rather than relying on LVGL's object hit-testing.
    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER &&
            lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
            seedsigner_lvgl_on_button_selected(0xFFFFFFFFu, "screensaver_dismiss");
            return;
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
    seedsigner_lvgl_on_button_selected(0xFFFFFFFFu, "screensaver_dismiss");
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

void screensaver_screen(void * /*ctx_json*/) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
    int32_t screen_h = lv_display_get_vertical_resolution(NULL);

    // Display the logo at native resolution (no zoom).
    // The image is pre-scaled by png_to_lvgl.py to match the target display.
    int32_t logo_w = (int32_t)seedsigner_logo_img.header.w;
    int32_t logo_h = (int32_t)seedsigner_logo_img.header.h;

    lv_obj_t *logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &seedsigner_logo_img);
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
        lv_obj_add_event_cb(sink, screensaver_key_handler, LV_EVENT_KEY, ctx);

        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD ||
                lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
                lv_indev_set_group(indev, ctx->group);
            }
        }
    }

    lv_obj_add_event_cb(scr, screensaver_cleanup_handler, LV_EVENT_DELETE, ctx);

    // Load the screensaver WITHOUT destroying the previous screen.
    // The caller is responsible for save/restore via save_screen/restore_screen.
    lv_scr_load(scr);
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


