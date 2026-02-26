#include "pmu_bridge.h"
#include "bsp_i2c.h"
#include "bsp_axp2101.h"

extern "C" esp_err_t lcdaxs_pmu_init(void) {
    i2c_master_bus_handle_t bus = bsp_i2c_init();
    return bsp_axp2101_init(bus);
}
