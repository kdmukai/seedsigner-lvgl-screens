#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "display_manager.h"
#include "seedsigner.h"

#define SEEDSIGNER_RESULT_QUEUE_CAP 16
#define SEEDSIGNER_RESULT_LABEL_MAX 96

typedef struct {
    uint32_t index;
    char label[SEEDSIGNER_RESULT_LABEL_MAX];
} seedsigner_result_event_t;

static seedsigner_result_event_t s_result_queue[SEEDSIGNER_RESULT_QUEUE_CAP];
static uint32_t s_result_head = 0;
static uint32_t s_result_tail = 0;
static uint32_t s_result_count = 0;

void seedsigner_lvgl_on_button_selected(uint32_t index, const char *label) {
    seedsigner_result_event_t ev = {
        .index = index,
        .label = {0},
    };

    if (label) {
        strncpy(ev.label, label, SEEDSIGNER_RESULT_LABEL_MAX - 1);
        ev.label[SEEDSIGNER_RESULT_LABEL_MAX - 1] = '\0';
    }

    if (s_result_count == SEEDSIGNER_RESULT_QUEUE_CAP) {
        s_result_head = (s_result_head + 1) % SEEDSIGNER_RESULT_QUEUE_CAP;
        s_result_count--;
    }

    s_result_queue[s_result_tail] = ev;
    s_result_tail = (s_result_tail + 1) % SEEDSIGNER_RESULT_QUEUE_CAP;
    s_result_count++;
}

static mp_obj_t mp_seedsigner_lvgl_demo_screen(void) {
    if (!run_screen(demo_screen, NULL)) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("display lock unavailable"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(seedsigner_lvgl_demo_screen_obj, mp_seedsigner_lvgl_demo_screen);

static void parse_optional_top_nav(mp_obj_t top_nav_obj, top_nav_ctx_t *top_nav) {
    if (top_nav_obj == MP_OBJ_NULL || top_nav_obj == mp_const_none) {
        return;
    }
    if (!mp_obj_is_type(top_nav_obj, &mp_type_dict)) {
        mp_raise_TypeError(MP_ERROR_TEXT("top_nav must be a dict"));
    }

    mp_obj_dict_t *d = MP_OBJ_TO_PTR(top_nav_obj);

    mp_map_elem_t *e = mp_map_lookup(&d->map, MP_OBJ_NEW_QSTR(MP_QSTR_title), MP_MAP_LOOKUP);
    if (e) {
        top_nav->title = mp_obj_str_get_str(e->value);
    }

    e = mp_map_lookup(&d->map, MP_OBJ_NEW_QSTR(MP_QSTR_show_back_button), MP_MAP_LOOKUP);
    if (e) {
        top_nav->show_back_button = mp_obj_is_true(e->value);
    }

    e = mp_map_lookup(&d->map, MP_OBJ_NEW_QSTR(MP_QSTR_show_power_button), MP_MAP_LOOKUP);
    if (e) {
        top_nav->show_power_button = mp_obj_is_true(e->value);
    }
}

static mp_obj_t mp_seedsigner_lvgl_button_list_screen(mp_obj_t cfg_obj) {
    if (!mp_obj_is_type(cfg_obj, &mp_type_dict)) {
        mp_raise_TypeError(MP_ERROR_TEXT("button_list_screen expects a dict"));
    }

    button_list_screen_ctx_t ctx = {
        .top_nav = TOP_NAV_CTX_DEFAULTS,
        .button_list = NULL,
        .button_list_len = 0,
    };

    mp_obj_dict_t *cfg = MP_OBJ_TO_PTR(cfg_obj);

    // top_nav is optional
    mp_map_elem_t *e = mp_map_lookup(&cfg->map, MP_OBJ_NEW_QSTR(MP_QSTR_top_nav), MP_MAP_LOOKUP);
    if (e) {
        parse_optional_top_nav(e->value, &ctx.top_nav);
    }

    // button_list is required
    e = mp_map_lookup(&cfg->map, MP_OBJ_NEW_QSTR(MP_QSTR_button_list), MP_MAP_LOOKUP);
    if (!e) {
        mp_raise_ValueError(MP_ERROR_TEXT("button_list is required"));
    }

    mp_obj_t button_list_obj = e->value;
    size_t list_len = 0;
    mp_obj_t *list_items = NULL;
    mp_obj_get_array(button_list_obj, &list_len, &list_items);

    button_list_item_t *items = m_new(button_list_item_t, list_len);
    char **labels_alloc = m_new(char *, list_len);

    for (size_t i = 0; i < list_len; ++i) {
        labels_alloc[i] = NULL;

        size_t tuple_len = 0;
        mp_obj_t *tuple_items = NULL;
        mp_obj_get_array(list_items[i], &tuple_len, &tuple_items);
        if (tuple_len != 2) {
            mp_raise_ValueError(MP_ERROR_TEXT("button_list entries must be tuple(str, any)"));
        }

        size_t label_len = 0;
        const char *label_src = mp_obj_str_get_data(tuple_items[0], &label_len);
        char *label_copy = m_new(char, label_len + 1);
        memcpy(label_copy, label_src, label_len);
        label_copy[label_len] = '\0';

        labels_alloc[i] = label_copy;
        items[i].label = label_copy;
        items[i].value = (void *)(uintptr_t)tuple_items[1];
    }

    ctx.button_list = items;
    ctx.button_list_len = list_len;

    bool ok = run_screen(button_list_screen, &ctx);

    for (size_t i = 0; i < list_len; ++i) {
        if (labels_alloc[i]) {
            m_del(char, labels_alloc[i], strlen(labels_alloc[i]) + 1);
        }
    }
    m_del(char *, labels_alloc, list_len);
    m_del(button_list_item_t, items, list_len);

    if (!ok) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("display lock unavailable"));
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(seedsigner_lvgl_button_list_screen_obj, mp_seedsigner_lvgl_button_list_screen);

static mp_obj_t mp_seedsigner_lvgl_poll_for_result(void) {
    if (s_result_count == 0) {
        return mp_const_none;
    }

    seedsigner_result_event_t ev = s_result_queue[s_result_head];
    s_result_head = (s_result_head + 1) % SEEDSIGNER_RESULT_QUEUE_CAP;
    s_result_count--;

    mp_obj_t out[3];
    out[0] = MP_OBJ_NEW_QSTR(MP_QSTR_button_selected);
    out[1] = mp_obj_new_int_from_uint(ev.index);
    out[2] = mp_obj_new_str(ev.label, strlen(ev.label));
    return mp_obj_new_tuple(3, out);
}
static MP_DEFINE_CONST_FUN_OBJ_0(seedsigner_lvgl_poll_for_result_obj, mp_seedsigner_lvgl_poll_for_result);

static mp_obj_t mp_seedsigner_lvgl_clear_result_queue(void) {
    s_result_head = 0;
    s_result_tail = 0;
    s_result_count = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(seedsigner_lvgl_clear_result_queue_obj, mp_seedsigner_lvgl_clear_result_queue);

static const mp_rom_map_elem_t seedsigner_lvgl_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_seedsigner_lvgl) },
    { MP_ROM_QSTR(MP_QSTR_demo_screen), MP_ROM_PTR(&seedsigner_lvgl_demo_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_button_list_screen), MP_ROM_PTR(&seedsigner_lvgl_button_list_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll_for_result), MP_ROM_PTR(&seedsigner_lvgl_poll_for_result_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear_result_queue), MP_ROM_PTR(&seedsigner_lvgl_clear_result_queue_obj) },
};
static MP_DEFINE_CONST_DICT(seedsigner_lvgl_module_globals, seedsigner_lvgl_module_globals_table);

const mp_obj_module_t seedsigner_lvgl_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&seedsigner_lvgl_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_seedsigner_lvgl, seedsigner_lvgl_user_cmodule);
