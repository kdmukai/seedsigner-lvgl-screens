#ifndef SEEDSIGNER_COMPONENTS_H
#define SEEDSIGNER_COMPONENTS_H

#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>

lv_obj_t* top_nav(lv_obj_t* lv_parent, const char *title, bool show_back_button, bool show_power_button, lv_obj_t **out_back_btn, lv_obj_t **out_power_btn);

typedef struct {
    const char *label;
    void *value;
} button_list_item_t;

lv_obj_t* button(lv_obj_t* lv_parent, const char* text, lv_obj_t* align_to);
lv_obj_t* large_icon_button(lv_obj_t* lv_parent, const char* icon, const char* text, lv_obj_t* align_to);
lv_obj_t* button_list(lv_obj_t* lv_parent, const button_list_item_t *items, size_t item_count);

void button_set_active(lv_obj_t* lv_button, bool active);

// Suppress exactly one upcoming body button CLICKED event (used by nav layer
// to prevent top-nav ENTER from falling through to parked body selection).
void suppress_next_body_button_click(void);

// Nav layer sets whether virtual top-nav zone is currently active.
// Body button callbacks use this to hard-block CLICKED handling while top-nav
// owns input focus.
void set_nav_top_zone_active(bool active);

#endif // SEEDSIGNER_COMPONENTS_H
