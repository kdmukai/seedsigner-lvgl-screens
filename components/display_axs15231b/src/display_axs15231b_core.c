#include "display_axs15231b_core.h"
#include "display_axs15231b_board.h"

#include <stdlib.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_axs15231b.h"
#include "pmu_bridge.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "lcdaxs";

#ifndef LCD_H_RES
#define LCD_H_RES 320
#endif
#ifndef LCD_V_RES
#define LCD_V_RES 480
#endif

static bool s_inited = false;
static uint8_t s_brightness = 100;
static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static SemaphoreHandle_t s_refresh_done = NULL;

static bool lcd_on_color_trans_done(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    BaseType_t hp = pdFALSE;
    if (s_refresh_done) xSemaphoreGiveFromISR(s_refresh_done, &hp);
    return hp == pdTRUE;
}
static uint16_t *s_fill_buf = NULL;
static int s_fill_rows = 0;

// Verbatim-style init table from known working bsp_display.c
static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xBB, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5}, 8, 0},
    {0xA0, (uint8_t[]){0xC0, 0x10, 0x00, 0x02, 0x00, 0x00, 0x04, 0x3F, 0x20, 0x05, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00}, 17, 0},
    {0xA2, (uint8_t[]){0x30, 0x3C, 0x24, 0x14, 0xD0, 0x20, 0xFF, 0xE0, 0x40, 0x19, 0x80, 0x80, 0x80, 0x20, 0xf9, 0x10, 0x02, 0xff, 0xff, 0xF0, 0x90, 0x01, 0x32, 0xA0, 0x91, 0xE0, 0x20, 0x7F, 0xFF, 0x00, 0x5A}, 31, 0},
    {0xD0, (uint8_t[]){0xE0, 0x40, 0x51, 0x24, 0x08, 0x05, 0x10, 0x01, 0x20, 0x15, 0x42, 0xC2, 0x22, 0x22, 0xAA, 0x03, 0x10, 0x12, 0x60, 0x14, 0x1E, 0x51, 0x15, 0x00, 0x8A, 0x20, 0x00, 0x03, 0x3A, 0x12}, 30, 0},
    {0xA3, (uint8_t[]){0xA0, 0x06, 0xAa, 0x00, 0x08, 0x02, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x55, 0x55}, 22, 0},
    {0xC1, (uint8_t[]){0x31, 0x04, 0x02, 0x02, 0x71, 0x05, 0x24, 0x55, 0x02, 0x00, 0x41, 0x00, 0x53, 0xFF, 0xFF, 0xFF, 0x4F, 0x52, 0x00, 0x4F, 0x52, 0x00, 0x45, 0x3B, 0x0B, 0x02, 0x0d, 0x00, 0xFF, 0x40}, 30, 0},
    {0xC3, (uint8_t[]){0x00, 0x00, 0x00, 0x50, 0x03, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01}, 11, 0},
    {0xC4, (uint8_t[]){0x00, 0x24, 0x33, 0x80, 0x00, 0xea, 0x64, 0x32, 0xC8, 0x64, 0xC8, 0x32, 0x90, 0x90, 0x11, 0x06, 0xDC, 0xFA, 0x00, 0x00, 0x80, 0xFE, 0x10, 0x10, 0x00, 0x0A, 0x0A, 0x44, 0x50}, 29, 0},
    {0xC5, (uint8_t[]){0x18, 0x00, 0x00, 0x03, 0xFE, 0x3A, 0x4A, 0x20, 0x30, 0x10, 0x88, 0xDE, 0x0D, 0x08, 0x0F, 0x0F, 0x01, 0x3A, 0x4A, 0x20, 0x10, 0x10, 0x00}, 23, 0},
    {0xC6, (uint8_t[]){0x05, 0x0A, 0x05, 0x0A, 0x00, 0xE0, 0x2E, 0x0B, 0x12, 0x22, 0x12, 0x22, 0x01, 0x03, 0x00, 0x3F, 0x6A, 0x18, 0xC8, 0x22}, 20, 0},
    {0xC7, (uint8_t[]){0x50, 0x32, 0x28, 0x00, 0xa2, 0x80, 0x8f, 0x00, 0x80, 0xff, 0x07, 0x11, 0x9c, 0x67, 0xff, 0x24, 0x0c, 0x0d, 0x0e, 0x0f}, 20, 0},
    {0xC9, (uint8_t[]){0x33, 0x44, 0x44, 0x01}, 4, 0},
    {0xCF, (uint8_t[]){0x2C, 0x1E, 0x88, 0x58, 0x13, 0x18, 0x56, 0x18, 0x1E, 0x68, 0x88, 0x00, 0x65, 0x09, 0x22, 0xC4, 0x0C, 0x77, 0x22, 0x44, 0xAA, 0x55, 0x08, 0x08, 0x12, 0xA0, 0x08}, 27, 0},
    {0xD5, (uint8_t[]){0x40, 0x8E, 0x8D, 0x01, 0x35, 0x04, 0x92, 0x74, 0x04, 0x92, 0x74, 0x04, 0x08, 0x6A, 0x04, 0x46, 0x03, 0x03, 0x03, 0x03, 0x82, 0x01, 0x03, 0x00, 0xE0, 0x51, 0xA1, 0x00, 0x00, 0x00}, 30, 0},
    {0xD6, (uint8_t[]){0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x93, 0x00, 0x01, 0x83, 0x07, 0x07, 0x00, 0x07, 0x07, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x84, 0x00, 0x20, 0x01, 0x00}, 30, 0},
    {0xD7, (uint8_t[]){0x03, 0x01, 0x0b, 0x09, 0x0f, 0x0d, 0x1E, 0x1F, 0x18, 0x1d, 0x1f, 0x19, 0x40, 0x8E, 0x04, 0x00, 0x20, 0xA0, 0x1F}, 19, 0},
    {0xD8, (uint8_t[]){0x02, 0x00, 0x0a, 0x08, 0x0e, 0x0c, 0x1E, 0x1F, 0x18, 0x1d, 0x1f, 0x19}, 12, 0},
    {0xD9, (uint8_t[]){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDD, (uint8_t[]){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDF, (uint8_t[]){0x44, 0x73, 0x4B, 0x69, 0x00, 0x0A, 0x02, 0x90}, 8, 0},
    {0xE0, (uint8_t[]){0x3B, 0x28, 0x10, 0x16, 0x0c, 0x06, 0x11, 0x28, 0x5c, 0x21, 0x0D, 0x35, 0x13, 0x2C, 0x33, 0x28, 0x0D}, 17, 0},
    {0xE1, (uint8_t[]){0x37, 0x28, 0x10, 0x16, 0x0b, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x14, 0x2C, 0x33, 0x28, 0x0F}, 17, 0},
    {0xE2, (uint8_t[]){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE3, (uint8_t[]){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x32, 0x2F, 0x0F}, 17, 0},
    {0xE4, (uint8_t[]){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE5, (uint8_t[]){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0F}, 17, 0},
    {0xA4, (uint8_t[]){0x85, 0x85, 0x95, 0x82, 0xAF, 0xAA, 0xAA, 0x80, 0x10, 0x30, 0x40, 0x40, 0x20, 0xFF, 0x60, 0x30}, 16, 0},
    {0xA4, (uint8_t[]){0x85, 0x85, 0x95, 0x85}, 4, 0},
    {0xBB, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 8, 0},
    {0x13, (uint8_t[]){0x00}, 0, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x2C, (uint8_t[]){0x00, 0x00, 0x00, 0x00}, 4, 0},
};

static esp_err_t brightness_init(void) {
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LCD_BL_LEDC_MODE,
        .timer_num = LCD_BL_LEDC_TIMER,
        .duty_resolution = LCD_BL_LEDC_DUTY_RES,
        .freq_hz = LCD_BL_LEDC_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&ledc_timer), TAG, "ledc timer");

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LCD_BL_LEDC_MODE,
        .channel = LCD_BL_LEDC_CHANNEL,
        .timer_sel = LCD_BL_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = PIN_LCD_BL,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&ledc_channel);
}

static esp_err_t brightness_set(uint8_t brightness) {
    if (brightness > 100) brightness = 100;
    s_brightness = brightness;
    uint32_t duty = (brightness * (LCD_BL_LEDC_DUTY - 1)) / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL, duty), TAG, "set duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL), TAG, "update duty");
    return ESP_OK;
}



static esp_err_t alloc_fill_buffer(void) {
    if (s_fill_buf) return ESP_OK;
    const int candidates[] = {60, 40, 30, 24, 20, 16};
    for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]); ++i) {
        int rows = candidates[i];
        size_t bytes = LCD_H_RES * rows * sizeof(uint16_t);
        s_fill_buf = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (s_fill_buf) {
            s_fill_rows = rows;
            ESP_LOGI(TAG, "fill DMA buffer: %d rows (%u bytes)", rows, (unsigned)bytes);
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}
bool display_axs15231b_driver_available(void) { return true; }
const char *display_axs15231b_driver_name(void) { return "espressif/esp_lcd_axs15231b"; }
int display_axs15231b_backlight_default_duty(void) { return LCD_BL_LEDC_DUTY; }

esp_err_t display_axs15231b_init(void) {
    if (s_inited) return ESP_OK;

    esp_err_t pmu_err = lcdaxs_pmu_init();
    if (pmu_err != ESP_OK) {
        ESP_LOGW(TAG, "PMU init failed: %s", esp_err_to_name(pmu_err));
    }

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .data0_io_num = PIN_LCD_DATA0,
        .data1_io_num = PIN_LCD_DATA1,
        .data2_io_num = PIN_LCD_DATA2,
        .data3_io_num = PIN_LCD_DATA3,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi bus init");

    esp_lcd_panel_io_spi_config_t io_config = AXS15231B_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, lcd_on_color_trans_done, NULL);
    io_config.pclk_hz = LCD_PIXEL_CLOCK_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &s_io), TAG, "new panel io");

    axs15231b_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_axs15231b(s_io, &panel_config, &s_panel), TAG, "new panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");

    ESP_RETURN_ON_ERROR(brightness_init(), TAG, "brightness init");
    ESP_RETURN_ON_ERROR(brightness_set(100), TAG, "brightness set");

    if (!s_refresh_done) {
        s_refresh_done = xSemaphoreCreateBinary();
        if (!s_refresh_done) return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(alloc_fill_buffer(), TAG, "alloc fill buffer");

    // Default to landscape expectation for app-level drawing.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true), TAG, "swap_xy");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, false, false), TAG, "mirror");

    // Matches working BSP: starts off; caller can power on.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, false), TAG, "disp off");

    s_inited = true;
    ESP_LOGI(TAG, "lcdaxs init done");
    return ESP_OK;
}

esp_err_t display_axs15231b_power(bool on) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, on), TAG, "disp on/off");
    return brightness_set(on ? 100 : 0);
}

esp_err_t display_axs15231b_fill(uint16_t rgb565) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!s_fill_buf || s_fill_rows <= 0) return ESP_ERR_NO_MEM;

    const int chunk_rows = s_fill_rows;
    const int chunk_px = LCD_H_RES * chunk_rows;
    for (int i = 0; i < chunk_px; i++) s_fill_buf[i] = rgb565;

    esp_err_t err = ESP_OK;
    for (int y = 0; y < LCD_V_RES; y += chunk_rows) {
        int y2 = y + chunk_rows;
        if (y2 > LCD_V_RES) y2 = LCD_V_RES;
        err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y2, s_fill_buf);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t display_axs15231b_test_pattern(void) {
    ESP_RETURN_ON_ERROR(display_axs15231b_power(true), TAG, "power on");
    ESP_RETURN_ON_ERROR(display_axs15231b_fill(0xF800), TAG, "red");
    ESP_RETURN_ON_ERROR(display_axs15231b_fill(0x07E0), TAG, "green");
    ESP_RETURN_ON_ERROR(display_axs15231b_fill(0x001F), TAG, "blue");
    return ESP_OK;
}

esp_err_t display_axs15231b_set_mirror(bool mirror_x, bool mirror_y) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    return esp_lcd_panel_mirror(s_panel, mirror_x, mirror_y);
}
esp_err_t display_axs15231b_set_swap_xy(bool swap_xy) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    return esp_lcd_panel_swap_xy(s_panel, swap_xy);
}
esp_err_t display_axs15231b_set_invert(bool invert) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    return esp_lcd_panel_invert_color(s_panel, invert);
}
esp_err_t display_axs15231b_set_gap(int x_gap, int y_gap) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    return esp_lcd_panel_set_gap(s_panel, x_gap, y_gap);
}
esp_err_t display_axs15231b_set_madctl_raw(uint8_t madctl) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    return esp_lcd_panel_io_tx_param(s_io, 0x36, &madctl, 1);
}
esp_err_t display_axs15231b_set_colmod(uint8_t colmod) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    return esp_lcd_panel_io_tx_param(s_io, 0x3A, &colmod, 1);
}
esp_err_t display_axs15231b_force_mode(void) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    return esp_lcd_panel_disp_on_off(s_panel, true);
}


esp_err_t display_axs15231b_bsp_selftest(void) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(display_axs15231b_power(true), TAG, "power on");

    const int bit_per_pixel = 16;
    const int rows_per_band = (LCD_V_RES / bit_per_pixel);
    const size_t band_px = (size_t)LCD_H_RES * rows_per_band;
    uint16_t *color = heap_caps_calloc(band_px, sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!color) return ESP_ERR_NO_MEM;

    esp_err_t err = ESP_OK;
    for (int j = 0; j < bit_per_pixel; j++) {
        uint16_t c = (uint16_t)(1u << j);
        for (size_t i = 0; i < band_px; i++) {
            color[i] = c;
        }
        while (xSemaphoreTake(s_refresh_done, 0) == pdTRUE) {}
        err = esp_lcd_panel_draw_bitmap(s_panel, 0, j * rows_per_band, LCD_H_RES, (j + 1) * rows_per_band, color);
        if (err != ESP_OK) break;
        if (xSemaphoreTake(s_refresh_done, pdMS_TO_TICKS(500)) != pdTRUE) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
    }

    free(color);
    return err;
}
