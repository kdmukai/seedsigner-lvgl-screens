#ifndef DISPLAY_AXS15231B_BOARD_H
#define DISPLAY_AXS15231B_BOARD_H

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"

// Waveshare ESP32-S3-Touch-LCD-3.5B wiring (from demo bsp_display.h)
#define LCD_PIXEL_CLOCK_HZ          (40 * 1000 * 1000)
#define LCD_SPI_HOST                SPI2_HOST

#define PIN_LCD_CS                  GPIO_NUM_12
#define PIN_LCD_SCLK                GPIO_NUM_5
#define PIN_LCD_DATA0               GPIO_NUM_1
#define PIN_LCD_DATA1               GPIO_NUM_2
#define PIN_LCD_DATA2               GPIO_NUM_3
#define PIN_LCD_DATA3               GPIO_NUM_4
#define PIN_LCD_RST                 GPIO_NUM_NC
#define PIN_LCD_BL                  GPIO_NUM_6

#define LCD_BL_LEDC_TIMER           LEDC_TIMER_0
#define LCD_BL_LEDC_MODE            LEDC_LOW_SPEED_MODE
#define LCD_BL_LEDC_CHANNEL         LEDC_CHANNEL_0
#define LCD_BL_LEDC_DUTY_RES        LEDC_TIMER_10_BIT
#define LCD_BL_LEDC_DUTY            (1024)
#define LCD_BL_LEDC_FREQUENCY       (5000)

#endif // DISPLAY_AXS15231B_BOARD_H
