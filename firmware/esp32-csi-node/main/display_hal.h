/**
 * @file display_hal.h
 * @brief ADR-045: ST7789V2 SPI LCD + CST816 touch HAL.
 *
 * Hardware abstraction for the Waveshare ESP32-S3-Touch-LCD-1.69 panel.
 * Probes hardware at boot; returns ESP_ERR_NOT_FOUND if absent.
 */

#ifndef DISPLAY_HAL_H
#define DISPLAY_HAL_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Display resolution */
#define DISP_HAL_H_RES  240
#define DISP_HAL_V_RES  280

/**
 * Probe and initialize the ST7789V2 SPI LCD panel.
 *
 * Configures SPI bus, creates esp_lcd panel, enables backlight,
 * and draws a test pattern to confirm the display works.
 * Returns ESP_ERR_NOT_FOUND if the panel does not respond.
 *
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no display detected.
 */
esp_err_t display_hal_init_panel(void);

/**
 * Draw a rectangle of pixels to the LCD.
 * Uses esp_lcd_panel_draw_bitmap() internally.
 *
 * @param x_start  Left column (inclusive).
 * @param y_start  Top row (inclusive).
 * @param x_end    Right column (exclusive).
 * @param y_end    Bottom row (exclusive).
 * @param color_data  RGB565 pixel data, (x_end-x_start)*(y_end-y_start) pixels.
 */
void display_hal_draw(int x_start, int y_start, int x_end, int y_end,
                      const void *color_data);

/**
 * Probe and initialize the CST816 capacitive touch controller.
 *
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no touch IC detected.
 */
esp_err_t display_hal_init_touch(void);

/**
 * Read touch point (non-blocking).
 *
 * @param[out] x  Touch X coordinate (0..239).
 * @param[out] y  Touch Y coordinate (0..279).
 * @return true if touch is active, false if released.
 */
bool display_hal_touch_read(uint16_t *x, uint16_t *y);

/**
 * Set LCD backlight brightness via LEDC PWM.
 *
 * @param percent  Brightness 0-100.
 */
void display_hal_set_brightness(uint8_t percent);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_HAL_H */
