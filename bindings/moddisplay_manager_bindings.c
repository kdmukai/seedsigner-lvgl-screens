#include "py/obj.h"
#include "py/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "display_manager.h"
#ifdef __cplusplus
}
#endif

// dm.init()
STATIC mp_obj_t mp_dm_init(void) {
    init();
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(dm_init_obj, mp_dm_init);

// dm.render_demo_ui()
STATIC mp_obj_t mp_dm_render_demo_ui(void) {
    render_demo_ui();
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(dm_render_demo_ui_obj, mp_dm_render_demo_ui);

STATIC const mp_rom_map_elem_t dm_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_dm) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&dm_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_render_demo_ui), MP_ROM_PTR(&dm_render_demo_ui_obj) },
};
STATIC MP_DEFINE_CONST_DICT(dm_module_globals, dm_module_globals_table);

const mp_obj_module_t dm_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&dm_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_dm, dm_user_cmodule);
