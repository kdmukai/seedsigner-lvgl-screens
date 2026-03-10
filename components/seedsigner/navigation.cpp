#include "navigation.h"
#include "components.h"

#include <cmath>

#define NAV_INDEX_NONE ((size_t)-1)

extern "C" __attribute__((weak)) void seedsigner_lvgl_on_aux_key(const char *key_name) {
    (void)key_name;
}

typedef struct {
    lv_group_t *group;
    lv_obj_t *top_items[2];
    size_t top_count;
    lv_obj_t **body_items;
    size_t body_count;
    nav_zone_t zone;
    size_t top_virtual_index;
    size_t last_body_index;
    lv_obj_t *parked_body_obj;
    nav_body_layout_t body_layout;
    nav_aux_policy_t aux_policy;
} nav_ctx_t;


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

static void activate_focused(nav_ctx_t *ctx) {
    if (!ctx) return;

    if (ctx->zone == NAV_ZONE_TOP) {
        if (ctx->top_count == 0) return;
        size_t idx = ctx->top_virtual_index;
        if (idx >= ctx->top_count) idx = 0;
        lv_obj_t *obj = ctx->top_items[idx];
        if (!obj || !lv_obj_is_valid(obj)) return;

        // Design decision: consume one body click to prevent parked focused body
        // control from receiving ENTER fallback when top-nav virtual zone handles ENTER.
        suppress_next_body_button_click();
        lv_event_send(obj, LV_EVENT_CLICKED, NULL);
        return;
    }

    if (!ctx->group) return;
    lv_obj_t *obj = lv_group_get_focused(ctx->group);
    if (!obj) return;
    lv_event_send(obj, LV_EVENT_CLICKED, NULL);
}

static void set_top_virtual_active(nav_ctx_t *ctx, size_t idx) {
    if (!ctx) return;
    for (size_t i = 0; i < ctx->top_count; ++i) {
        if (ctx->top_items[i]) {
            button_set_active(ctx->top_items[i], i == idx);
        }
    }
}

static void set_body_active(nav_ctx_t *ctx, size_t idx) {
    if (!ctx || !ctx->body_items) return;
    for (size_t i = 0; i < ctx->body_count; ++i) {
        if (ctx->body_items[i]) {
            button_set_active(ctx->body_items[i], i == idx);
        }
    }
}

static bool focus_top(nav_ctx_t *ctx, size_t idx) {
    if (!ctx || idx >= ctx->top_count) return false;
    ctx->zone = NAV_ZONE_TOP;
    set_nav_top_zone_active(true);
    ctx->top_virtual_index = idx;
    set_top_virtual_active(ctx, idx);
    set_body_active(ctx, NAV_INDEX_NONE);

    // Prevent ENTER from activating parked body button while top-nav virtual zone is active.
    if (ctx->group) {
        lv_obj_t *f = lv_group_get_focused(ctx->group);
        if (f && lv_obj_is_valid(f)) {
            ctx->parked_body_obj = f;
            lv_obj_clear_flag(f, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    return true;
}

static bool focus_body(nav_ctx_t *ctx, size_t idx) {
    if (!ctx || idx >= ctx->body_count || !ctx->group || !ctx->body_items) return false;
    lv_obj_t *obj = ctx->body_items[idx];
    if (!obj || !lv_obj_is_valid(obj)) return false;
    ctx->zone = NAV_ZONE_BODY;
    set_nav_top_zone_active(false);
    set_top_virtual_active(ctx, NAV_INDEX_NONE);
    set_body_active(ctx, idx);
    ctx->last_body_index = idx;

    if (ctx->parked_body_obj && lv_obj_is_valid(ctx->parked_body_obj)) {
        lv_obj_add_flag(ctx->parked_body_obj, LV_OBJ_FLAG_CLICKABLE);
    }
    ctx->parked_body_obj = NULL;

    lv_group_focus_obj(obj);
    return true;
}

static int focused_index_in(nav_ctx_t *ctx, lv_obj_t **arr, size_t count) {
    if (!ctx || !ctx->group || !arr) return -1;
    lv_obj_t *f = lv_group_get_focused(ctx->group);
    if (!f) return -1;
    for (size_t i = 0; i < count; ++i) {
        if (arr[i] == f) return (int)i;
    }
    return -1;
}

static size_t first_valid_body_index(nav_ctx_t *ctx) {
    if (!ctx || !ctx->body_items) return NAV_INDEX_NONE;
    for (size_t i = 0; i < ctx->body_count; ++i) {
        if (ctx->body_items[i]) return i;
    }
    return NAV_INDEX_NONE;
}

static size_t grid_columns_for_count(size_t body_count) {
    if (body_count <= 1) return 1;
    if (body_count == 4) return 2; // explicit common case (main_menu 2x2)

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
        if (target < body_count) return target;
        return current;
    }

    return current;
}

static inline void consume_key_event(lv_event_t *e) {
    if (!e) return;
    lv_event_stop_bubbling(e);
    lv_event_stop_processing(e);
}

static void nav_key_handler(lv_event_t *e) {
    if (!e) return;
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;

    nav_ctx_t *ctx = (nav_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;

    uint32_t key = lv_event_get_key(e);

    int aux_idx = 0;
    if (is_aux_key(key, &aux_idx)) {
        nav_aux_action_t action = action_for_aux(ctx, aux_idx);
        if (action == NAV_AUX_ENTER) {
            activate_focused(ctx);
            consume_key_event(e);
        } else if (action == NAV_AUX_EMIT) {
            if (aux_idx == 1) seedsigner_lvgl_on_aux_key("KEY1");
            else if (aux_idx == 2) seedsigner_lvgl_on_aux_key("KEY2");
            else if (aux_idx == 3) seedsigner_lvgl_on_aux_key("KEY3");
            consume_key_event(e);
        }
        return;
    }

    if (key == LV_KEY_ENTER) {
        activate_focused(ctx);
        consume_key_event(e);
        return;
    }

    int top_i = (ctx->zone == NAV_ZONE_TOP) ? (int)ctx->top_virtual_index : -1;
    int body_i = focused_index_in(ctx, ctx->body_items, ctx->body_count);

    // Keep top zone authoritative while virtual top-nav is active; otherwise,
    // the still-focused body object can incorrectly force zone/body movement.
    if (ctx->zone != NAV_ZONE_TOP && body_i >= 0) {
        ctx->zone = NAV_ZONE_BODY;
    }
    if (ctx->zone == NAV_ZONE_TOP && (top_i < 0 || (size_t)top_i >= ctx->top_count)) {
        top_i = 0;
        ctx->top_virtual_index = 0;
    }

    if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
        if (ctx->zone == NAV_ZONE_TOP && ctx->top_count > 1 && top_i >= 0) {
            if (key == LV_KEY_LEFT && top_i > 0) focus_top(ctx, (size_t)(top_i - 1));
            else if (key == LV_KEY_RIGHT && (size_t)(top_i + 1) < ctx->top_count) focus_top(ctx, (size_t)(top_i + 1));
            consume_key_event(e);
            return;
        }
    }

    if (ctx->body_layout == NAV_BODY_VERTICAL) {
        if (key == LV_KEY_UP) {
            if (ctx->zone == NAV_ZONE_BODY) {
                if (body_i > 0) {
                    focus_body(ctx, (size_t)(body_i - 1));
                } else if (body_i == 0 && ctx->top_count > 0) {
                    focus_top(ctx, 0);
                }
            }
            consume_key_event(e);
            return;
        }

        if (key == LV_KEY_DOWN) {
            if (ctx->zone == NAV_ZONE_TOP) {
                if (ctx->body_count > 0) {
                    size_t restore = (ctx->last_body_index < ctx->body_count) ? ctx->last_body_index : 0;
                    focus_body(ctx, restore);
                }
            } else if (ctx->zone == NAV_ZONE_BODY && body_i >= 0 && (size_t)(body_i + 1) < ctx->body_count) {
                focus_body(ctx, (size_t)(body_i + 1));
            }
            consume_key_event(e);
            return;
        }

        if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
            consume_key_event(e);
            return; // body no-op; top-nav already handled above
        }
    }

    if (ctx->body_layout == NAV_BODY_GRID) {
        size_t cols = grid_columns_for_count(ctx->body_count);

        if (ctx->zone == NAV_ZONE_TOP && key == LV_KEY_DOWN) {
            if (ctx->body_count > 0) {
                size_t target = ctx->last_body_index;
                if (target >= ctx->body_count || !ctx->body_items || !ctx->body_items[target]) {
                    size_t target_col = (top_i >= 0) ? (size_t)top_i : 0;
                    if (target_col >= cols) target_col = cols - 1;
                    target = target_col;
                    if (target >= ctx->body_count) target = ctx->body_count - 1;
                }
                focus_body(ctx, target);
            }
            consume_key_event(e);
            return;
        }

        if (ctx->zone == NAV_ZONE_BODY && body_i >= 0) {
            if (key == LV_KEY_UP && (size_t)body_i < cols && ctx->top_count > 0) {
                size_t target_top = ((size_t)body_i < ctx->top_count) ? (size_t)body_i : 0;
                focus_top(ctx, target_top);
                consume_key_event(e);
                return;
            }

            if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT || key == LV_KEY_UP || key == LV_KEY_DOWN) {
                size_t next_i = grid_move((size_t)body_i, ctx->body_count, cols, key);
                if (next_i != (size_t)body_i) {
                    focus_body(ctx, next_i);
                }
                consume_key_event(e);
                return;
            }
        }

        consume_key_event(e);
        return;
    }
}

static void nav_cleanup_handler(lv_event_t *e) {
    if (!e) return;
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;

    nav_ctx_t *ctx = (nav_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;

    if (ctx->parked_body_obj && lv_obj_is_valid(ctx->parked_body_obj)) {
        lv_obj_add_flag(ctx->parked_body_obj, LV_OBJ_FLAG_CLICKABLE);
        ctx->parked_body_obj = NULL;
    }
    set_nav_top_zone_active(false);

    if (ctx->group) {
        lv_group_del(ctx->group);
        ctx->group = NULL;
    }
    if (ctx->body_items) {
        lv_mem_free(ctx->body_items);
        ctx->body_items = NULL;
    }
    lv_mem_free(ctx);
}

void nav_bind(const nav_config_t *cfg) {
    if (!cfg || !cfg->screen) return;

    nav_ctx_t *ctx = (nav_ctx_t *)lv_mem_alloc(sizeof(nav_ctx_t));
    if (!ctx) return;
    lv_memset_00(ctx, sizeof(*ctx));

    ctx->group = lv_group_create();
    lv_group_set_wrap(ctx->group, false);
    // Avoid re-entrant focus/state churn while group population is in progress.
    // We'll unfreeze and set explicit initial focus once all objects are added.
    lv_group_focus_freeze(ctx->group, true);
    ctx->body_count = cfg->body_item_count;
    if (ctx->body_count > 0) {
        ctx->body_items = (lv_obj_t **)lv_mem_alloc(sizeof(lv_obj_t *) * ctx->body_count);
        if (!ctx->body_items) {
            if (ctx->group) lv_group_del(ctx->group);
            lv_mem_free(ctx);
            return;
        }
        for (size_t i = 0; i < ctx->body_count; ++i) {
            ctx->body_items[i] = cfg->body_items ? cfg->body_items[i] : NULL;
        }
    }
    ctx->body_layout = cfg->body_layout;
    ctx->aux_policy = cfg->aux_policy;

    if (cfg->top_back_btn) {
        ctx->top_items[ctx->top_count++] = cfg->top_back_btn;
    }
    if (cfg->top_power_btn) {
        ctx->top_items[ctx->top_count++] = cfg->top_power_btn;
    }

    // Design decision: keep top-nav as a virtual focus zone (not in LVGL group)
    // to avoid re-entrant/state crashes observed when moving group focus into top-nav.
    for (size_t i = 0; i < ctx->body_count; ++i) {
        if (ctx->body_items[i]) lv_group_add_obj(ctx->group, ctx->body_items[i]);
    }

    // Group is now fully populated; enable normal focus behavior and apply
    // deterministic initial focus policy.
    lv_group_focus_freeze(ctx->group, false);

    input_mode_t mode = cfg->has_input_mode_override ? cfg->input_mode_override : input_profile_get_mode();
    if (mode == INPUT_MODE_HARDWARE) {
        size_t target = NAV_INDEX_NONE;
        if (cfg->initial_body_index != NAV_INDEX_NONE && cfg->initial_body_index < ctx->body_count && ctx->body_items[cfg->initial_body_index]) {
            target = cfg->initial_body_index;
        } else {
            target = first_valid_body_index(ctx);
        }

        if (target != NAV_INDEX_NONE) {
            ctx->last_body_index = target;
            focus_body(ctx, target);
        } else if (ctx->top_count > 0) {
            focus_top(ctx, 0);
        }
    }

    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD || lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
            lv_indev_set_group(indev, ctx->group);
        }
    }

    for (size_t i = 0; i < ctx->top_count; ++i) {
        if (ctx->top_items[i]) lv_obj_add_event_cb(ctx->top_items[i], nav_key_handler, LV_EVENT_KEY, ctx);
    }
    for (size_t i = 0; i < ctx->body_count; ++i) {
        if (ctx->body_items[i]) lv_obj_add_event_cb(ctx->body_items[i], nav_key_handler, LV_EVENT_KEY, ctx);
    }
    lv_obj_add_event_cb(cfg->screen, nav_cleanup_handler, LV_EVENT_DELETE, ctx);
}
