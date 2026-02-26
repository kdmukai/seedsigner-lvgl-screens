#include <string.h>
#include "py/obj.h"
#include "py/runtime.h"

#include "display_axs15231b_core.h"

static mp_obj_t mp_lcdaxs_available(void) { return mp_obj_new_bool(display_axs15231b_driver_available()); }
static MP_DEFINE_CONST_FUN_OBJ_0(lcdaxs_available_obj, mp_lcdaxs_available);

static mp_obj_t mp_lcdaxs_driver_name(void) {
    const char *s = display_axs15231b_driver_name();
    return mp_obj_new_str(s, strlen(s));
}
static MP_DEFINE_CONST_FUN_OBJ_0(lcdaxs_driver_name_obj, mp_lcdaxs_driver_name);

static mp_obj_t mp_lcdaxs_bl_duty(void) { return mp_obj_new_int(display_axs15231b_backlight_default_duty()); }
static MP_DEFINE_CONST_FUN_OBJ_0(lcdaxs_bl_duty_obj, mp_lcdaxs_bl_duty);

static mp_obj_t mp_lcdaxs_init(void) { return mp_obj_new_int(display_axs15231b_init()); }
static MP_DEFINE_CONST_FUN_OBJ_0(lcdaxs_init_obj, mp_lcdaxs_init);

static mp_obj_t mp_lcdaxs_power(mp_obj_t on_in) { return mp_obj_new_int(display_axs15231b_power(mp_obj_is_true(on_in))); }
static MP_DEFINE_CONST_FUN_OBJ_1(lcdaxs_power_obj, mp_lcdaxs_power);

static mp_obj_t mp_lcdaxs_fill(mp_obj_t color_in) {
    uint16_t c = (uint16_t)mp_obj_get_int_truncated(color_in);
    return mp_obj_new_int(display_axs15231b_fill(c));
}
static MP_DEFINE_CONST_FUN_OBJ_1(lcdaxs_fill_obj, mp_lcdaxs_fill);

static mp_obj_t mp_lcdaxs_set_mirror(mp_obj_t x_in, mp_obj_t y_in) {
    return mp_obj_new_int(display_axs15231b_set_mirror(mp_obj_is_true(x_in), mp_obj_is_true(y_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(lcdaxs_set_mirror_obj, mp_lcdaxs_set_mirror);

static mp_obj_t mp_lcdaxs_set_swap_xy(mp_obj_t on_in) {
    return mp_obj_new_int(display_axs15231b_set_swap_xy(mp_obj_is_true(on_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(lcdaxs_set_swap_xy_obj, mp_lcdaxs_set_swap_xy);

static mp_obj_t mp_lcdaxs_set_invert(mp_obj_t on_in) {
    return mp_obj_new_int(display_axs15231b_set_invert(mp_obj_is_true(on_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(lcdaxs_set_invert_obj, mp_lcdaxs_set_invert);

static mp_obj_t mp_lcdaxs_set_gap(mp_obj_t x_in, mp_obj_t y_in) {
    return mp_obj_new_int(display_axs15231b_set_gap(mp_obj_get_int(x_in), mp_obj_get_int(y_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(lcdaxs_set_gap_obj, mp_lcdaxs_set_gap);


static mp_obj_t mp_lcdaxs_test_pattern(void) {
    return mp_obj_new_int(display_axs15231b_test_pattern());
}
static MP_DEFINE_CONST_FUN_OBJ_0(lcdaxs_test_pattern_obj, mp_lcdaxs_test_pattern);


static mp_obj_t mp_lcdaxs_set_madctl_raw(mp_obj_t v_in) {
    return mp_obj_new_int(display_axs15231b_set_madctl_raw((uint8_t)mp_obj_get_int(v_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(lcdaxs_set_madctl_raw_obj, mp_lcdaxs_set_madctl_raw);

static mp_obj_t mp_lcdaxs_set_colmod(mp_obj_t v_in) {
    return mp_obj_new_int(display_axs15231b_set_colmod((uint8_t)mp_obj_get_int(v_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(lcdaxs_set_colmod_obj, mp_lcdaxs_set_colmod);

static mp_obj_t mp_lcdaxs_force_mode(void) {
    return mp_obj_new_int(display_axs15231b_force_mode());
}
static MP_DEFINE_CONST_FUN_OBJ_0(lcdaxs_force_mode_obj, mp_lcdaxs_force_mode);


static mp_obj_t mp_lcdaxs_bsp_selftest(void) {
    return mp_obj_new_int(display_axs15231b_bsp_selftest());
}
static MP_DEFINE_CONST_FUN_OBJ_0(lcdaxs_bsp_selftest_obj, mp_lcdaxs_bsp_selftest);

static const mp_rom_map_elem_t lcdaxs_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_lcdaxs) },
    { MP_ROM_QSTR(MP_QSTR_available), MP_ROM_PTR(&lcdaxs_available_obj) },
    { MP_ROM_QSTR(MP_QSTR_driver_name), MP_ROM_PTR(&lcdaxs_driver_name_obj) },
    { MP_ROM_QSTR(MP_QSTR_backlight_duty_default), MP_ROM_PTR(&lcdaxs_bl_duty_obj) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&lcdaxs_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_power), MP_ROM_PTR(&lcdaxs_power_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill), MP_ROM_PTR(&lcdaxs_fill_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_mirror), MP_ROM_PTR(&lcdaxs_set_mirror_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_swap_xy), MP_ROM_PTR(&lcdaxs_set_swap_xy_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_invert), MP_ROM_PTR(&lcdaxs_set_invert_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_gap), MP_ROM_PTR(&lcdaxs_set_gap_obj) },
    { MP_ROM_QSTR(MP_QSTR_test_pattern), MP_ROM_PTR(&lcdaxs_test_pattern_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_madctl_raw), MP_ROM_PTR(&lcdaxs_set_madctl_raw_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_colmod), MP_ROM_PTR(&lcdaxs_set_colmod_obj) },
    { MP_ROM_QSTR(MP_QSTR_force_mode), MP_ROM_PTR(&lcdaxs_force_mode_obj) },
    { MP_ROM_QSTR(MP_QSTR_bsp_selftest), MP_ROM_PTR(&lcdaxs_bsp_selftest_obj) },
};
static MP_DEFINE_CONST_DICT(lcdaxs_module_globals, lcdaxs_module_globals_table);

const mp_obj_module_t lcdaxs_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&lcdaxs_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_lcdaxs, lcdaxs_user_cmodule);
