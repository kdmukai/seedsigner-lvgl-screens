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
LV_IMG_DECLARE(seedsigner_logo_img);


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
        lv_obj_del(old_screen);
    }
}


// Build the standard "body" container used by screens beneath TopNav.
// Most screens share the same layout/styling shell (size, alignment, padding, background,
// border, and scrollbar baseline behavior). This function encapsulates that common
// boilerplate.
static lv_obj_t* create_standard_body_content(lv_obj_t *screen, lv_obj_t *top_nav_obj, bool scrollable) {
    lv_obj_t* body_content = lv_obj_create(screen);
    lv_obj_set_size(body_content, lv_obj_get_width(screen), lv_obj_get_height(screen) - TOP_NAV_HEIGHT - COMPONENT_PADDING);
    lv_obj_align_to(body_content, top_nav_obj, LV_ALIGN_OUT_BOTTOM_MID, 0, COMPONENT_PADDING);
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

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *top_nav;
    lv_obj_t *top_back_btn;
    lv_obj_t *top_power_btn;
    lv_obj_t *body;
} screen_scaffold_t;

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

// Build root screen: top nav + standard body container.
// Screen-specific code should only populate scaffold.body, then call
// load_screen_and_cleanup_previous(scaffold.screen).
static screen_scaffold_t create_top_nav_screen_scaffold(const json &cfg, bool scrollable) {
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

    out.top_nav = top_nav(out.screen, title.c_str(), show_back, show_power, &out.top_back_btn, &out.top_power_btn);
    out.body = create_standard_body_content(out.screen, out.top_nav, scrollable);
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


void button_list_screen(void *ctx_json)
{
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array()) {
        throw std::runtime_error("button_list is required and must be an array");
    }

    const auto &button_list_json = cfg["button_list"];
    std::vector<std::string> labels;
    std::vector<button_list_item_t> items;

    labels.reserve(button_list_json.size());
    items.reserve(button_list_json.size());

    for (const auto &it : button_list_json) {
        if (it.is_string()) {
            labels.push_back(it.get<std::string>());
        } else if (it.is_array() && !it.empty() && it[0].is_string()) {
            labels.push_back(it[0].get<std::string>());
        } else {
            throw std::runtime_error("button_list entries must be string or array with string label at index 0");
        }

        button_list_item_t item = {.label = labels.back().c_str(), .value = NULL};
        items.push_back(item);
    }

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, true);
    lv_obj_t *scr = screen.screen;
    lv_obj_t *body_content = screen.body;

    button_list(body_content, items.data(), items.size());

    std::vector<lv_obj_t *> body_items;
    uint32_t child_count = lv_obj_get_child_cnt(body_content);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t *child = lv_obj_get_child(body_content, i);
        if (child && lv_obj_check_type(child, &lv_btn_class)) {
            body_items.push_back(child);
        }
    }

    // Bind shared nav behavior using this screen's body focusables/layout.
    bind_screen_navigation(
        cfg,
        screen,
        body_items.empty() ? NULL : body_items.data(),
        body_items.size(),
        NAV_BODY_VERTICAL,
        (size_t)-1
    );

    load_screen_and_cleanup_previous(scr);
}

void main_menu_screen(void *ctx)
{
    // `ctx` is unused for main_menu_screen, but kept to match the shared
    // screen callback signature. Cast to void to silence unused-parameter warnings.
    (void)ctx;

    json cfg = {{"top_nav", {{"title", "Home"}, {"show_back_button", false}, {"show_power_button", true}}}};
    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);
    lv_obj_t *scr = screen.screen;
    lv_obj_t *body_content = screen.body;

    static const char *icons[] = {
        SeedSignerIconConstants::SCAN,
        SeedSignerIconConstants::SEEDS,
        SeedSignerIconConstants::TOOLS,
        SeedSignerIconConstants::SETTINGS,
    };
    static const char *labels[] = {"Scan", "Seeds", "Tools", "Settings"};

    const lv_coord_t available_w = lv_obj_get_content_width(body_content);
    const lv_coord_t available_h = lv_obj_get_content_height(body_content);

    // Max out the large button width. Note that the body already has edge padding. 
    lv_coord_t button_w = (available_w - COMPONENT_PADDING) / 2;
    lv_coord_t button_h = (available_h - COMPONENT_PADDING) / 2;

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
        0
    );
    lv_obj_set_pos(
        buttons[1],
        button_w + COMPONENT_PADDING,
        0
    );

    // second row
    lv_obj_set_pos(
        buttons[2],
        0,
        button_h + COMPONENT_PADDING
    );
    lv_obj_set_pos(
        buttons[3],
        button_w + COMPONENT_PADDING,
        button_h + COMPONENT_PADDING

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
    lv_coord_t  screen_w;
    lv_coord_t  screen_h;
    lv_coord_t  logo_w;    // displayed width after zoom
    lv_coord_t  logo_h;    // displayed height after zoom
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
    screensaver_ctx_t *ctx = (screensaver_ctx_t *)timer->user_data;

    // Check for touch dismiss: poll pointer input devices directly
    // rather than relying on LVGL's object hit-testing.
    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER &&
            indev->proc.state == LV_INDEV_STATE_PRESSED) {
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
                   (lv_coord_t)(ctx->center_x - ctx->logo_w / 2.0f),
                   (lv_coord_t)(ctx->center_y - ctx->logo_h / 2.0f));
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
    lv_mem_free(ctx);
}

void screensaver_screen(void * /*ctx_json*/) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_coord_t screen_w = lv_disp_get_hor_res(NULL);
    lv_coord_t screen_h = lv_disp_get_ver_res(NULL);

    // Display the logo at native resolution (no zoom).
    // The image is pre-scaled by png_to_lvgl.py to match the target display.
    lv_coord_t logo_w = (lv_coord_t)seedsigner_logo_img.header.w;
    lv_coord_t logo_h = (lv_coord_t)seedsigner_logo_img.header.h;

    lv_obj_t *logo_img = lv_img_create(scr);
    lv_img_set_src(logo_img, &seedsigner_logo_img);
    lv_obj_set_size(logo_img, logo_w, logo_h);

    // Allocate and initialise animation context.
    screensaver_ctx_t *ctx = (screensaver_ctx_t *)lv_mem_alloc(sizeof(screensaver_ctx_t));
    lv_memset_00(ctx, sizeof(*ctx));
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
                   (lv_coord_t)(ctx->center_x - logo_w / 2.0f),
                   (lv_coord_t)(ctx->center_y - logo_h / 2.0f));

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
        lv_obj_clear_flag(sink, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

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


