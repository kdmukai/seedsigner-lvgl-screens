#ifndef SEEDSIGNER_NAVIGATION_H
#define SEEDSIGNER_NAVIGATION_H

#include "lvgl.h"
#include "input_profile.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NAV_ZONE_TOP = 0,
    NAV_ZONE_BODY = 1,
    // Body content (not a focusable item) is scrolling under joystick control.
    // Used only by screens that opt into scroll-then-buttons navigation (e.g. an
    // overflowing large_icon_status_screen): DOWN walks the body down a step at a
    // time, then drops into NAV_ZONE_BODY to focus the bottom button; UP reverses.
    // No item is highlighted while in this zone.
    NAV_ZONE_SCROLL = 2,
} nav_zone_t;

typedef enum {
    NAV_BODY_VERTICAL = 0,
    NAV_BODY_GRID = 1,
} nav_body_layout_t;

typedef enum {
    NAV_AUX_ENTER = 0,
    NAV_AUX_NOOP = 1,
    NAV_AUX_EMIT = 2,
} nav_aux_action_t;

typedef struct {
    nav_aux_action_t key1;
    nav_aux_action_t key2;
    nav_aux_action_t key3;
} nav_aux_policy_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *top_back_btn;
    lv_obj_t *top_power_btn;
    lv_obj_t **body_items;
    size_t body_item_count;
    nav_body_layout_t body_layout;
    nav_aux_policy_t aux_policy;
    size_t initial_body_index;
    bool has_input_mode_override;
    input_mode_t input_mode_override;

    // Opt-in joystick scrolling (default off: scroll_obj == NULL). When a screen's
    // body content overflows the viewport, it passes the scrollable body here with
    // scroll_then_buttons = true: the nav state machine inserts a NAV_ZONE_SCROLL
    // step so DOWN scrolls the body progressively before focusing the bottom button,
    // and UP scrolls back up before entering the top-nav. Screens whose content fits
    // (or that never opt in) leave these unset and keep the plain TOP<->BODY flow.
    lv_obj_t *scroll_obj;
    bool scroll_then_buttons;
} nav_config_t;

void nav_bind(const nav_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif // SEEDSIGNER_NAVIGATION_H
