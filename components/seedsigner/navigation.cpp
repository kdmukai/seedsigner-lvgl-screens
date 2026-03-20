#include "navigation.h"
#include "components.h"
#include "input_profile.h"

#include <cmath>

#define NAV_INDEX_NONE ((size_t)-1)

extern "C" __attribute__((weak)) void seedsigner_lvgl_on_aux_key(const char *key_name) {
    (void)key_name;
}

typedef struct {
    lv_group_t   *group;
    lv_obj_t     *top_item;     // NULL when screen has no top-nav button
    lv_obj_t    **body_items;
    size_t        body_count;
    nav_zone_t    zone;
    size_t        body_index;
    nav_body_layout_t body_layout;
    nav_aux_policy_t  aux_policy;
} nav_ctx_t;


// ---------------------------------------------------------------------------
// Aux-key helpers
// ---------------------------------------------------------------------------

static bool is_aux_key(uint32_t key, int *idx_out) {
#ifdef LV_KEY_F1
    if (key == LV_KEY_F1) { *idx_out = 1; return true; }
#endif
#ifdef LV_KEY_F2
    if (key == LV_KEY_F2) { *idx_out = 2; return true; }
#endif
#ifdef LV_KEY_F3
    if (key == LV_KEY_F3) { *idx_out = 3; return true; }
#endif
    if (key == (uint32_t)'1') { *idx_out = 1; return true; }
    if (key == (uint32_t)'2') { *idx_out = 2; return true; }
    if (key == (uint32_t)'3') { *idx_out = 3; return true; }
    return false;
}

static nav_aux_action_t action_for_aux(const nav_ctx_t *ctx, int idx) {
    if (!ctx) return NAV_AUX_NOOP;
    if (idx == 1) return ctx->aux_policy.key1;
    if (idx == 2) return ctx->aux_policy.key2;
    if (idx == 3) return ctx->aux_policy.key3;
    return NAV_AUX_NOOP;
}


// ---------------------------------------------------------------------------
// Grid navigation helpers
// ---------------------------------------------------------------------------

static size_t grid_columns_for_count(size_t body_count) {
    if (body_count <= 1) return 1;
    if (body_count == 4) return 2;
    size_t c = (size_t)std::ceil(std::sqrt((double)body_count));
    return c > 0 ? c : 1;
}

static size_t grid_move(size_t current, size_t body_count, size_t cols, uint32_t key) {
    if (body_count == 0 || cols == 0 || current >= body_count) return current;

    size_t row = current / cols;
    size_t col = current % cols;

    if (key == LV_KEY_LEFT) {
        if (col == 0) return current;
        size_t target = current - 1;
        return target < body_count ? target : current;
    }
    if (key == LV_KEY_RIGHT) {
        size_t target = current + 1;
        if ((target / cols) != row) return current;
        return target < body_count ? target : current;
    }
    if (key == LV_KEY_UP) {
        if (row == 0) return current;
        size_t target = current - cols;
        return target < body_count ? target : current;
    }
    if (key == LV_KEY_DOWN) {
        size_t target = current + cols;
        return target < body_count ? target : current;
    }
    return current;
}


// ---------------------------------------------------------------------------
// Visual focus management
//
// All visual highlight changes go through this single function.
// nav_ctx_t.zone and nav_ctx_t.body_index are the authoritative source of
// truth; this function renders them.
// ---------------------------------------------------------------------------

static void update_visual_focus(nav_ctx_t *ctx) {
    if (ctx->top_item) {
        button_set_active(ctx->top_item, ctx->zone == NAV_ZONE_TOP);
    }
    if (ctx->body_items) {
        for (size_t i = 0; i < ctx->body_count; ++i) {
            if (ctx->body_items[i]) {
                button_set_active(ctx->body_items[i],
                    ctx->zone == NAV_ZONE_BODY && i == ctx->body_index);
            }
        }
        // Scroll the parent container to keep the focused body item visible.
        if (ctx->zone == NAV_ZONE_BODY &&
            ctx->body_index < ctx->body_count &&
            ctx->body_items[ctx->body_index]) {
            lv_obj_scroll_to_view(ctx->body_items[ctx->body_index], LV_ANIM_ON);
        }
    }
}

static lv_obj_t *get_focused_item(nav_ctx_t *ctx) {
    if (ctx->zone == NAV_ZONE_TOP) {
        return ctx->top_item;  // may be NULL; caller must check
    }
    if (ctx->body_items && ctx->body_index < ctx->body_count) {
        return ctx->body_items[ctx->body_index];
    }
    return NULL;
}


// ---------------------------------------------------------------------------
// Key event handler — registered on the keypad sink object only.
//
// Design: a single 1x1 transparent "sink" object is the sole member of the
// LVGL group connected to the keypad indev.  All key events route here.
// Body and top-nav items are never in the LVGL group, so LVGL auto-focus
// behavior cannot interfere with our explicit nav state.
// ---------------------------------------------------------------------------

static void nav_key_handler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;

    nav_ctx_t *ctx = (nav_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;

    uint32_t key = lv_event_get_key(e);

    // --- Aux keys (KEY1 / KEY2 / KEY3) ---
    int aux_idx = 0;
    if (is_aux_key(key, &aux_idx)) {
        nav_aux_action_t action = action_for_aux(ctx, aux_idx);
        if (action == NAV_AUX_ENTER) {
            lv_obj_t *item = get_focused_item(ctx);
            if (item && lv_obj_is_valid(item)) {
                lv_obj_send_event(item, LV_EVENT_CLICKED, NULL);
            }
        } else if (action == NAV_AUX_EMIT) {
            if (aux_idx == 1) seedsigner_lvgl_on_aux_key("KEY1");
            else if (aux_idx == 2) seedsigner_lvgl_on_aux_key("KEY2");
            else if (aux_idx == 3) seedsigner_lvgl_on_aux_key("KEY3");
        }
        // NAV_AUX_NOOP: do nothing
        return;
    }

    // --- Center click / ENTER ---
    if (key == LV_KEY_ENTER) {
        lv_obj_t *item = get_focused_item(ctx);
        if (item && lv_obj_is_valid(item)) {
            lv_obj_send_event(item, LV_EVENT_CLICKED, NULL);
        }
        return;
    }

    // --- Directional keys ---

    if (ctx->body_layout == NAV_BODY_VERTICAL) {
        if (key == LV_KEY_UP) {
            if (ctx->zone == NAV_ZONE_BODY) {
                if (ctx->body_index > 0) {
                    ctx->body_index--;
                    update_visual_focus(ctx);
                } else if (ctx->top_item) {
                    // First body item: move up into top-nav.
                    ctx->zone = NAV_ZONE_TOP;
                    update_visual_focus(ctx);
                }
            }
            return;
        }
        if (key == LV_KEY_DOWN) {
            if (ctx->zone == NAV_ZONE_TOP) {
                if (ctx->body_count > 0) {
                    ctx->zone = NAV_ZONE_BODY;
                    // body_index retains its last value.
                    update_visual_focus(ctx);
                }
            } else if (ctx->zone == NAV_ZONE_BODY) {
                if (ctx->body_index + 1 < ctx->body_count) {
                    ctx->body_index++;
                    update_visual_focus(ctx);
                }
            }
            return;
        }
        // LEFT / RIGHT: no-op in vertical body layout.
        return;
    }

    if (ctx->body_layout == NAV_BODY_GRID) {
        size_t cols = grid_columns_for_count(ctx->body_count);

        if (key == LV_KEY_DOWN && ctx->zone == NAV_ZONE_TOP) {
            if (ctx->body_count > 0) {
                ctx->zone = NAV_ZONE_BODY;
                update_visual_focus(ctx);
            }
            return;
        }

        if (ctx->zone == NAV_ZONE_BODY) {
            // UP from the top row of the grid enters top-nav (if present).
            if (key == LV_KEY_UP && ctx->body_index < cols && ctx->top_item) {
                ctx->zone = NAV_ZONE_TOP;
                update_visual_focus(ctx);
                return;
            }

            size_t next = grid_move(ctx->body_index, ctx->body_count, cols, key);
            if (next != ctx->body_index) {
                ctx->body_index = next;
                update_visual_focus(ctx);
            }
        }
    }
}


// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static void nav_cleanup_handler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;

    nav_ctx_t *ctx = (nav_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;

    // lv_group_del clears the indev's group pointer in LVGL 8, preventing
    // stale references after the screen is destroyed.
    if (ctx->group) {
        lv_group_del(ctx->group);
        ctx->group = NULL;
    }
    if (ctx->body_items) {
        lv_free(ctx->body_items);
        ctx->body_items = NULL;
    }
    lv_free(ctx);
}


// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void nav_bind(const nav_config_t *cfg) {
    if (!cfg || !cfg->screen) return;

    nav_ctx_t *ctx = (nav_ctx_t *)lv_malloc(sizeof(nav_ctx_t));
    if (!ctx) return;
    lv_memzero(ctx, sizeof(*ctx));

    // Top item: back and power are mutually exclusive; take whichever is set.
    ctx->top_item = cfg->top_back_btn ? cfg->top_back_btn : cfg->top_power_btn;

    // Copy body items array.
    ctx->body_count = cfg->body_item_count;
    if (ctx->body_count > 0 && cfg->body_items) {
        ctx->body_items = (lv_obj_t **)lv_malloc(sizeof(lv_obj_t *) * ctx->body_count);
        if (!ctx->body_items) {
            lv_free(ctx);
            return;
        }
        for (size_t i = 0; i < ctx->body_count; ++i) {
            ctx->body_items[i] = cfg->body_items[i];
        }
    }

    ctx->body_layout = cfg->body_layout;
    ctx->aux_policy  = cfg->aux_policy;
    ctx->zone        = NAV_ZONE_BODY;
    ctx->body_index  = 0;

    input_mode_t mode = cfg->has_input_mode_override
        ? cfg->input_mode_override
        : input_profile_get_mode();

    if (mode == INPUT_MODE_HARDWARE) {
        // Determine initial body focus index.
        size_t init = NAV_INDEX_NONE;
        if (cfg->initial_body_index != NAV_INDEX_NONE &&
            cfg->initial_body_index < ctx->body_count &&
            ctx->body_items && ctx->body_items[cfg->initial_body_index]) {
            init = cfg->initial_body_index;
        } else if (ctx->body_items) {
            for (size_t i = 0; i < ctx->body_count; ++i) {
                if (ctx->body_items[i]) { init = i; break; }
            }
        }

        if (init != NAV_INDEX_NONE) {
            ctx->body_index = init;
            ctx->zone = NAV_ZONE_BODY;
        } else if (ctx->top_item) {
            ctx->zone = NAV_ZONE_TOP;
        }

        // Apply initial visual highlight.
        update_visual_focus(ctx);

        // Create the keypad sink: a single 1x1 transparent object that is the
        // sole member of the LVGL group connected to the keypad indev.
        // Keeping body/top items out of the group prevents LVGL from generating
        // spurious FOCUSED/DEFOCUSED events that would fight our own state.
        ctx->group = lv_group_create();
        lv_group_set_wrap(ctx->group, false);

        lv_obj_t *sink = lv_obj_create(cfg->screen);
        lv_obj_set_size(sink, 1, 1);
        lv_obj_set_pos(sink, 0, 0);
        lv_obj_set_style_opa(sink, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sink, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(sink, 0, LV_PART_MAIN);
        lv_obj_set_style_outline_width(sink, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(sink, 0, LV_PART_MAIN);
        lv_obj_remove_flag(sink, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

        lv_group_add_obj(ctx->group, sink);
        lv_obj_add_event_cb(sink, nav_key_handler, LV_EVENT_KEY, ctx);

        // Connect all keypad/encoder indevs to this group.
        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD ||
                lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
                lv_indev_set_group(indev, ctx->group);
            }
        }
    }

    lv_obj_add_event_cb(cfg->screen, nav_cleanup_handler, LV_EVENT_DELETE, ctx);
}
