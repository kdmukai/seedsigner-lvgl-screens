#include "navigation.h"
#include "seedsigner.h"      // seedsigner_lvgl_on_aux_key host hook (weak default in components.cpp)
#include "components.h"
#include "input_profile.h"

#include <cmath>

// Shared input handoff for every screen build — see navigation.h for the full
// rationale. Attaches each keypad/encoder indev to `group` and latches it with
// lv_indev_wait_release() so a key still held from the previous screen is
// ignored until released; only a fresh press on the new screen registers.
void attach_keypad_indevs_to_group(lv_group_t *group) {
    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL) {
        lv_indev_type_t type = lv_indev_get_type(indev);
        if (type == LV_INDEV_TYPE_KEYPAD || type == LV_INDEV_TYPE_ENCODER) {
            lv_indev_set_group(indev, group);

            // Suppress a carried-over hold: a key already down at the transition
            // was aimed at the OLD screen, so ignore it until it is released.
            // The next RELEASED->PRESSED edge (a genuine new press on this
            // screen) fires normally, so legitimate key exits still work.
            lv_indev_wait_release(indev);
        }
    }
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
    lv_obj_t     *scroll_obj;          // NULL unless the screen opted into scrolling
    bool          scroll_then_buttons;  // insert a NAV_ZONE_SCROLL step before the buttons
    bool          top_is_back;          // top_item is a BACK button (vs a power button)
} nav_ctx_t;


// ---------------------------------------------------------------------------
// Aux-key helpers
// ---------------------------------------------------------------------------

// Public shared recognizer (declared in navigation.h). Keep the #ifdef branches:
// LV_KEY_F1..F3 are the documented forward-compat contract for on-device keycodes,
// even though no current build defines them (they compile out everywhere today).
int nav_aux_key_index(uint32_t key) {
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
// Body scroll step (joystick-driven scrolling for NAV_ZONE_SCROLL)
//
// Scrolls ctx->scroll_obj by one step (75% of the viewport, animated) toward the
// requested direction, then reports whether MORE scrolling remains:
//   true  -> there is still content to reveal; the caller stays in NAV_ZONE_SCROLL.
//   false -> the relevant edge is within one step; the caller advances out of the
//            scroll zone on the SAME keypress — onto the first body button (DOWN)
//            or the top-nav back button (UP). Returning false on the exhausting
//            step avoids a "dead" extra press at each end: the press that finishes
//            the scroll also moves the focus.
//
// DOWN target — the FIRST focusable body item, not the content bottom. The point
// of scroll-then-buttons is to read the non-focusable upper content (status text,
// or a button list's intro text) and then hand off to item navigation. Stopping
// at the first body item is what makes this work for a MULTI-button list under
// intro text: scrolling all the way to scroll_bottom would skip past the first
// buttons to the last one. Once item 0 is reached the caller focuses it and
// update_visual_focus()'s scroll_to_view() walks the rest of the list. For a
// single bottom-pinned button (large_icon_status_screen) item 0 sits at the very
// bottom, so "first item in view" coincides with scrolling to the bottom — that
// screen's behavior is unchanged.
//
// UP target — the top of the content; exhausting it surfaces the top-nav.
//
// LVGL scroll-by convention: a NEGATIVE dy reveals lower content ("scroll down").
// LVGL clamps to the content bounds, so every step is safe; lv_obj_get_scroll_
// top/bottom report the range above / below the current viewport.
// ---------------------------------------------------------------------------

static bool nav_scroll_step(nav_ctx_t *ctx, bool down) {
    if (!ctx || !ctx->scroll_obj) return false;

    int32_t step = (lv_obj_get_height(ctx->scroll_obj) * 3) / 4;
    if (step < 1) step = 1;

    if (!down) {
        // Scroll up toward the top. Clamp the last step to the exact remaining
        // range so it lands precisely at the top (no elastic over-scroll gap).
        int32_t range = lv_obj_get_scroll_top(ctx->scroll_obj);
        if (range <= 0) return false;   // already at the top: nothing scrolled
        int32_t move = step < range ? step : range;
        lv_obj_scroll_by(ctx->scroll_obj, 0, move, LV_ANIM_ON);
        return range > step;
    }

    int32_t range = lv_obj_get_scroll_bottom(ctx->scroll_obj);
    if (range <= 0) return false;   // already at the bottom: nothing scrolled

    // How far below the viewport bottom does the first body item's top edge still
    // sit? When that gap is within one step, the next step would reveal the item:
    // stop scrolling and hand off, letting the caller focus item 0 and
    // scroll_to_view() land it precisely (which also lands the lone status-screen
    // button exactly at the bottom, no over-scroll gap).
    int32_t needed = range;   // fallback (no items): scroll to the content bottom
    if (ctx->body_items && ctx->body_count > 0 && ctx->body_items[0]) {
        lv_area_t item_coords, view_coords;
        lv_obj_get_coords(ctx->body_items[0], &item_coords);
        lv_obj_get_coords(ctx->scroll_obj, &view_coords);
        needed = item_coords.y1 - view_coords.y2;
        if (needed < 0) needed = 0;   // item already in (or above) the viewport
    }
    if (needed <= step) return false;

    int32_t move = step < range ? step : range;
    lv_obj_scroll_by(ctx->scroll_obj, 0, -move, LV_ANIM_ON);
    return true;
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
                bool focused = ctx->zone == NAV_ZONE_BODY && i == ctx->body_index;
                button_set_active(ctx->body_items[i], focused);
                // A focused too-wide button label marquee-scrolls; the rest clip to
                // their start edge. Body items are out of the LVGL focus group, so
                // LVGL never emits the FOCUSED/DEFOCUSED that would drive this — the
                // nav layer drives it explicitly here.
                button_set_label_marquee(ctx->body_items[i], focused);
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
    // Only NAV_ZONE_BODY has a focusable item. NAV_ZONE_SCROLL is mid-scroll with
    // nothing highlighted, so ENTER / aux-enter are no-ops there.
    //
    // This no-op is intentional, not a dropped keypress. In hardware nav a button
    // must be active (focused/highlighted) before it can be selected, so while the
    // overflowing upper content is still being scrolled the (not-yet-revealed)
    // bottom button is deliberately unselectable. The user scrolls all the way
    // down — which reveals the button and drops focus onto it (see the LV_KEY_DOWN
    // handoff below) — and only then can ENTER select it. Returning NULL here keeps
    // the "must be active before selectable" rule consistent across all screens.
    if (ctx->zone == NAV_ZONE_BODY && ctx->body_items && ctx->body_index < ctx->body_count) {
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
    int aux_idx = nav_aux_key_index(key);
    if (aux_idx != 0) {
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
        // Does this screen insert a scroll step between the top-nav and the body
        // buttons? Only when it opted in AND there is something to scroll.
        bool scroll_enabled = ctx->scroll_then_buttons && ctx->scroll_obj;

        if (key == LV_KEY_DOWN) {
            // Scroll path: from the top-nav OR mid-scroll, each DOWN scrolls the
            // body one step. The first DOWN already moves (no dead "enter the zone"
            // press), and the step that reaches the bottom drops focus straight onto
            // the first button on the SAME press.
            if (scroll_enabled &&
                (ctx->zone == NAV_ZONE_TOP || ctx->zone == NAV_ZONE_SCROLL)) {
                ctx->zone = NAV_ZONE_SCROLL;
                if (!nav_scroll_step(ctx, /*down=*/true)) {
                    ctx->zone = NAV_ZONE_BODY;
                    ctx->body_index = 0;
                }
                update_visual_focus(ctx);
                return;
            }

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

        if (key == LV_KEY_UP) {
            // Mid-scroll: each UP scrolls the body up one step; the step that
            // reaches the top surfaces into the top-nav back button on the same
            // press.
            if (scroll_enabled && ctx->zone == NAV_ZONE_SCROLL) {
                if (!nav_scroll_step(ctx, /*down=*/false)) {
                    if (ctx->top_item) ctx->zone = NAV_ZONE_TOP;
                }
                update_visual_focus(ctx);
                return;
            }

            if (ctx->zone == NAV_ZONE_BODY) {
                if (ctx->body_index > 0) {
                    ctx->body_index--;
                    update_visual_focus(ctx);
                } else if (scroll_enabled) {
                    // First body button: drop back into the scroll region (now at
                    // the bottom of the content) and scroll up immediately, so the
                    // first UP already moves rather than just changing zones.
                    ctx->zone = NAV_ZONE_SCROLL;
                    if (!nav_scroll_step(ctx, /*down=*/false)) {
                        if (ctx->top_item) ctx->zone = NAV_ZONE_TOP;
                    }
                    update_visual_focus(ctx);
                } else if (ctx->top_item) {
                    // First body item: move up into top-nav.
                    ctx->zone = NAV_ZONE_TOP;
                    update_visual_focus(ctx);
                }
            }
            return;
        }
        // LEFT: shortcut up to the top-nav BACK button (Python parity — jump
        // straight to Back from any body item). Only when a body item is focused and
        // the top item is actually a back button (not a lone power button). RIGHT:
        // no-op in vertical body layout.
        if (key == LV_KEY_LEFT &&
            ctx->zone == NAV_ZONE_BODY &&
            ctx->top_item && ctx->top_is_back) {
            ctx->zone = NAV_ZONE_TOP;
            update_visual_focus(ctx);
        }
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
    ctx->top_is_back = (cfg->top_back_btn != NULL);

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
    ctx->scroll_obj          = cfg->scroll_obj;
    ctx->scroll_then_buttons = cfg->scroll_then_buttons;

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

        // When the body overflows, two behaviors are possible — selected by whether
        // the caller supplied a CONCRETE initial body index:
        //
        //  - Concrete index (button lists / menus pass 0; a settings re-render passes
        //    the row to restore): KEEP that item focused and scroll it into view, so a
        //    button is always active — there is no "nothing selected" on load.
        //  - NAV_INDEX_NONE (status / warning screens): START UNFOCUSED at the top so
        //    the user scrolls through the content before any button becomes active; an
        //    UP at the top then surfaces the back button. (When such a screen instead
        //    FITS, the fallback above already focused its first button — so it's only
        //    unfocused while scrolling is genuinely required.)
        if (ctx->scroll_then_buttons && ctx->scroll_obj) {
            bool keep_focus = cfg->initial_body_index != NAV_INDEX_NONE &&
                              ctx->zone == NAV_ZONE_BODY &&
                              ctx->body_items &&
                              ctx->body_index < ctx->body_count &&
                              ctx->body_items[ctx->body_index];
            if (keep_focus) {
                lv_obj_scroll_to_view(ctx->body_items[ctx->body_index], LV_ANIM_OFF);
            } else {
                ctx->zone = NAV_ZONE_SCROLL;
                lv_obj_scroll_to_y(ctx->scroll_obj, 0, LV_ANIM_OFF);
            }
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

        // Connect all keypad/encoder indevs to this group, latching each so a key
        // still held from the previous screen can't bleed onto this one.
        attach_keypad_indevs_to_group(ctx->group);
    } else {
        // Touch mode has no persistent active/selected highlight — a row only looks
        // active while a finger is on it — so we deliberately focus and activate
        // NOTHING here (that also keeps e.g. AddressExplorer's rows truncated until
        // tapped, matching Python's "reveal only while selected"). But a caller-supplied
        // initial_selected_index still means "reveal this row on load" (e.g.
        // AddressExplorer resuming at the revealed row after a Next-N page-forward). The
        // active-highlight UI language doesn't fit touch, yet the SCROLL POSITION still
        // should — so scroll that row into view WITHOUT highlighting it.
        //
        // Restricted to plain vertical item-scrolling lists (button lists, menus):
        //  - scroll_then_buttons screens (status / warning / verify: read-only content
        //    above the buttons) must start at the TOP so that content is visible;
        //    scrolling their first button into view would hide it. They already load at
        //    scroll_y = 0 (touch does nothing else here), which is exactly what we want.
        //  - a default index 0 scrolls the top row into view == a no-op, so ordinary
        //    lists are unaffected; only a real non-zero resume index actually moves.
        if (ctx->body_layout == NAV_BODY_VERTICAL &&
            !ctx->scroll_then_buttons &&
            cfg->initial_body_index != NAV_INDEX_NONE &&
            cfg->initial_body_index < ctx->body_count &&
            ctx->body_items && ctx->body_items[cfg->initial_body_index]) {
            lv_obj_update_layout(cfg->screen);   // resolve geometry pre-load so the jump lands
            lv_obj_scroll_to_view(ctx->body_items[cfg->initial_body_index], LV_ANIM_OFF);
        }
    }

    lv_obj_add_event_cb(cfg->screen, nav_cleanup_handler, LV_EVENT_DELETE, ctx);
}
