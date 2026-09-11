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

/* ---- Direct-draw API (implemented by display_hal_ili9341.c, Hosyond board) ---- */

/**
 * Pack an 8-bit RGB triple into RGB565 in panel byte order (big-endian on
 * the wire, so byte-swapped in ESP32 memory). Use for both draw_bitmap and
 * fill_rect pixel values.
 */
static inline uint16_t display_hal_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((c << 8) | (c >> 8));
}

/**
 * Draw a w x h RGB565 bitmap at (x, y). Blocks until the SPI transfer
 * completes, so the caller may reuse the buffer on return.
 *
 * @param rgb565  w*h pixels in panel byte order (see display_hal_rgb565).
 * @return ESP_OK, ESP_ERR_INVALID_STATE if no panel, ESP_ERR_INVALID_ARG
 *         if the rectangle is empty or off-screen.
 */
esp_err_t display_hal_draw_bitmap(int x, int y, int w, int h, const void *rgb565);

/**
 * Fill a w x h rectangle at (x, y) with one colour (panel byte order).
 */
esp_err_t display_hal_fill_rect(int x, int y, int w, int h, uint16_t color);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_HAL_H */
