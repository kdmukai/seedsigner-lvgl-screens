#ifndef DISPLAY_AXS15231B_CORE_H
#define DISPLAY_AXS15231B_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

bool display_axs15231b_driver_available(void);
const char *display_axs15231b_driver_name(void);
int display_axs15231b_backlight_default_duty(void);

esp_err_t display_axs15231b_init(void);
esp_err_t display_axs15231b_power(bool on);
esp_err_t display_axs15231b_fill(uint16_t rgb565);
esp_err_t display_axs15231b_test_pattern(void);

// Runtime tuning controls
esp_err_t display_axs15231b_set_mirror(bool mirror_x, bool mirror_y);
esp_err_t display_axs15231b_set_swap_xy(bool swap_xy);
esp_err_t display_axs15231b_set_invert(bool invert);
esp_err_t display_axs15231b_set_gap(int x_gap, int y_gap);
esp_err_t display_axs15231b_set_madctl_raw(uint8_t madctl);
esp_err_t display_axs15231b_set_colmod(uint8_t colmod);
esp_err_t display_axs15231b_force_mode(void);
esp_err_t display_axs15231b_bsp_selftest(void);

#endif
