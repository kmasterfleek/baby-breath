/**
 * @file display_hal_ili9341.c
 * @brief ILI9341 SPI LCD HAL for the Hosyond ESP32-S3 2.8" board.
 *
 * Implements the display_hal.h API for the Hosyond board. Compiled only when
 * CONFIG_DISPLAY_BOARD_HOSYOND_28_ILI9341 is selected (see main/CMakeLists.txt);
 * the Waveshare board keeps display_hal.c untouched.
 *
 * Panel: ILI9341 240x320 driven in landscape (320x240), 4-wire SPI on
 * SPI2_HOST, RGB565. No touch controller is initialised on this board.
 *
 * Pins are fixed here rather than in Kconfig: the DISPLAY_QSPI_* / TOUCH_*
 * Kconfig ints in Kconfig.projbuild are dead options from the RM67162 era
 * and display_hal.c also hardcodes its pins, so this follows that convention.
 *
 *   LCD SPI: CS=10, DC=46, SCLK=12, MOSI=11, MISO=13, RST tied to EN, BL=45
 *   Touch I2C (NOT initialised): SDA=16, SCL=15, RST=18, INT=17
 */

#include "display_hal.h"
#include "sdkconfig.h"

#if CONFIG_DISPLAY_ENABLE && CONFIG_DISPLAY_BOARD_HOSYOND_28_ILI9341

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

static const char *TAG = "disp_ili";

/* ---- LCD SPI pins (Hosyond ESP32-S3 2.8" ILI9341) ---- */
#define LCD_PIN_CS          10
#define LCD_PIN_DC          46
#define LCD_PIN_SCLK        12
#define LCD_PIN_MOSI        11
#define LCD_PIN_MISO        13
#define LCD_PIN_BL          45   /* strapping pin: driven only after boot */

#define LCD_SPI_HOST        SPI2_HOST
/* 40 MHz per spec; drop to 20 MHz if the panel shows tearing/garbage. */
#define LCD_SPI_CLOCK_HZ    (40 * 1000 * 1000)

/* Landscape 320x240 */
#define LCD_H_RES           320
#define LCD_V_RES           240

/* Orientation: swap_xy + these mirrors give MADCTL MV|BGR (0x28), the usual
 * "rotation 1" landscape for 2.8" ILI9341 modules. If the picture appears
 * mirrored or upside-down on real hardware, flip these two flags. */
#define LCD_MIRROR_X        false
#define LCD_MIRROR_Y        false
#define LCD_INVERT_COLOR    false

/* Scratch buffer for fill_rect: LCD_H_RES x FILL_ROWS pixels (2560 bytes). */
#define FILL_ROWS           4

/* ---- State ---- */
static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;
static SemaphoreHandle_t s_tx_done = NULL;
static uint16_t *s_fill_buf = NULL;

/* Called from the SPI ISR when a colour transfer finishes. */
static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                          esp_lcd_panel_io_event_data_t *edata,
                                          void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_tx_done, &woken);
    return woken == pdTRUE;
}

static void backlight_set(bool on)
{
    gpio_config_t bl_cfg = {
        .pin_bit_mask = (1ULL << LCD_PIN_BL),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(LCD_PIN_BL, on ? 1 : 0);
}

static void teardown(void)
{
    if (s_panel) { esp_lcd_panel_del(s_panel); s_panel = NULL; }
    if (s_io)    { esp_lcd_panel_io_del(s_io); s_io = NULL; }
    spi_bus_free(LCD_SPI_HOST);
}

/* ---- Public API ---- */

esp_err_t display_hal_init_panel(void)
{
    ESP_LOGI(TAG, "Initializing Hosyond 2.8\" ILI9341 (%dx%d landscape)...",
             LCD_H_RES, LCD_V_RES);

    if (!s_tx_done) {
        s_tx_done = xSemaphoreCreateBinary();
        if (!s_tx_done) return ESP_ERR_NO_MEM;
    }
    if (!s_fill_buf) {
        s_fill_buf = heap_caps_malloc(LCD_H_RES * FILL_ROWS * sizeof(uint16_t),
                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_fill_buf) return ESP_ERR_NO_MEM;
    }

    spi_bus_config_t bus_cfg = {
        .sclk_io_num     = LCD_PIN_SCLK,
        .mosi_io_num     = LCD_PIN_MOSI,
        .miso_io_num     = LCD_PIN_MISO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * FILL_ROWS * sizeof(uint16_t),
    };
    esp_err_t ret = spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ESP_ERR_NOT_FOUND;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num         = LCD_PIN_CS,
        .dc_gpio_num         = LCD_PIN_DC,
        .spi_mode            = 0,
        .pclk_hz             = LCD_SPI_CLOCK_HZ,
        .trans_queue_depth   = 4,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx            = NULL,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &s_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel IO init failed: %s", esp_err_to_name(ret));
        teardown();
        return ESP_ERR_NOT_FOUND;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,               /* RST tied to EN: software reset */
        .rgb_endian     = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_ili9341(s_io, &panel_cfg, &s_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ILI9341 panel create failed: %s", esp_err_to_name(ret));
        teardown();
        return ESP_ERR_NOT_FOUND;
    }

    esp_lcd_panel_reset(s_panel);
    ret = esp_lcd_panel_init(s_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ILI9341 init failed: %s", esp_err_to_name(ret));
        teardown();
        return ESP_ERR_NOT_FOUND;
    }
    esp_lcd_panel_invert_color(s_panel, LCD_INVERT_COLOR);
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, LCD_MIRROR_X, LCD_MIRROR_Y);
    esp_lcd_panel_disp_on_off(s_panel, true);

    /* Clear to black before the backlight comes on. */
    display_hal_fill_rect(0, 0, LCD_H_RES, LCD_V_RES, 0x0000);
    backlight_set(true);

    ESP_LOGI(TAG, "ILI9341 panel init OK (%d MHz SPI, BL on GPIO %d)",
             LCD_SPI_CLOCK_HZ / 1000000, LCD_PIN_BL);
    return ESP_OK;
}

esp_err_t display_hal_draw_bitmap(int x, int y, int w, int h, const void *rgb565)
{
    if (!s_panel) return ESP_ERR_INVALID_STATE;
    if (w <= 0 || h <= 0 || x < 0 || y < 0 ||
        x + w > LCD_H_RES || y + h > LCD_V_RES || !rgb565) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, rgb565);
    if (ret != ESP_OK) return ret;
    /* Block until the DMA transfer has finished so the caller may reuse the buffer. */
    if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "SPI colour transfer timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t display_hal_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_panel || !s_fill_buf) return ESP_ERR_INVALID_STATE;
    if (w <= 0 || h <= 0 || x < 0 || y < 0 ||
        x + w > LCD_H_RES || y + h > LCD_V_RES) {
        return ESP_ERR_INVALID_ARG;
    }
    int rows_per_chunk = (LCD_H_RES * FILL_ROWS) / w;
    if (rows_per_chunk > h) rows_per_chunk = h;
    for (int i = 0; i < w * rows_per_chunk; i++) s_fill_buf[i] = color;

    for (int row = 0; row < h; row += rows_per_chunk) {
        int n = (h - row < rows_per_chunk) ? (h - row) : rows_per_chunk;
        esp_err_t ret = display_hal_draw_bitmap(x, y + row, w, n, s_fill_buf);
        if (ret != ESP_OK) return ret;
    }
    return ESP_OK;
}

/* ---- Legacy display_hal.h API (kept so the header stays board-agnostic) ---- */

void display_hal_draw(int x_start, int y_start, int x_end, int y_end,
                      const void *color_data)
{
    display_hal_draw_bitmap(x_start, y_start, x_end - x_start, y_end - y_start, color_data);
}

esp_err_t display_hal_init_touch(void)
{
    /* Touch controller on this board is unidentified; deliberately not probed. */
    return ESP_ERR_NOT_SUPPORTED;
}

bool display_hal_touch_read(uint16_t *x, uint16_t *y)
{
    (void)x; (void)y;
    return false;
}

void display_hal_set_brightness(uint8_t percent)
{
    /* Backlight is a plain GPIO on this board: on/off only. */
    backlight_set(percent > 0);
}

#endif /* CONFIG_DISPLAY_ENABLE && CONFIG_DISPLAY_BOARD_HOSYOND_28_ILI9341 */
