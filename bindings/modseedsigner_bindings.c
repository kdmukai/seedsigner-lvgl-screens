#include <stdbool.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "display_manager.h"
#include "seedsigner.h"

static mp_obj_t mp_seedsigner_lvgl_demo_screen(void) {
    if (!run_screen(demo_screen, NULL)) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("display lock unavailable"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(seedsigner_lvgl_demo_screen_obj, mp_seedsigner_lvgl_demo_screen);

static const mp_rom_map_elem_t seedsigner_lvgl_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_seedsigner_lvgl) },
    { MP_ROM_QSTR(MP_QSTR_demo_screen), MP_ROM_PTR(&seedsigner_lvgl_demo_screen_obj) },
};
static MP_DEFINE_CONST_DICT(seedsigner_lvgl_module_globals, seedsigner_lvgl_module_globals_table);

const mp_obj_module_t seedsigner_lvgl_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&seedsigner_lvgl_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_seedsigner_lvgl, seedsigner_lvgl_user_cmodule);
