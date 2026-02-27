#ifndef SEEDSIGNER_COMPONENTS_H
#define SEEDSIGNER_COMPONENTS_H

#include "lvgl.h"
#include "seedsigner.h"

lv_obj_t* top_nav(const top_nav_ctx_t *ctx);

lv_obj_t* button(lv_obj_t* lv_parent, const char* text, lv_obj_t* align_to);
lv_obj_t* button_list(lv_obj_t* lv_parent, const button_list_item_t *items, size_t item_count);

void button_set_active(lv_obj_t* lv_button, bool active);

#endif // SEEDSIGNER_COMPONENTS_H
