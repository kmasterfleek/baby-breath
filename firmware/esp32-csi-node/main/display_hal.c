/**
 * @file display_hal.c
 * @brief ADR-045: ST7789V2 SPI LCD HAL for Waveshare ESP32-S3-Touch-LCD-1.69.
 *
 * Uses ESP-IDF esp_lcd component with standard SPI (SPI2_HOST).
 * Panel: ST7789V2 240x280, standard SPI (not QSPI).
 * Touch: CST816 capacitive touch on I2C.
 *
 * Pin assignments (Waveshare ESP32-S3-Touch-LCD-1.69):
 *   LCD SPI: DC=4, CS=5, SCK=6, MOSI=7, RST=8, BL=15
 *   Touch I2C: SDA=11, SCL=10, RST=13, INT=14
 */

#include "display_hal.h"
#include "sdkconfig.h"

#if CONFIG_DISPLAY_ENABLE

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"

static const char *TAG = "disp_hal";

/* ---- LCD SPI Pin Definitions ---- */
#define LCD_PIN_DC          4
#define LCD_PIN_CS          5
#define LCD_PIN_SCK         6
#define LCD_PIN_MOSI        7
#define LCD_PIN_RST         8
#define LCD_PIN_BL          15

/* ---- Touch I2C Pin Definitions ---- */
#define TOUCH_PIN_SDA       11
#define TOUCH_PIN_SCL       10
#define TOUCH_PIN_RST       13
#define TOUCH_PIN_INT       14

/* ---- I2C config ---- */
#define I2C_MASTER_NUM      I2C_NUM_0
#define I2C_MASTER_FREQ_HZ  400000

/* ---- CST816 touch controller ---- */
#define CST816_ADDR         0x15

/* ---- Display dimensions ---- */
#define DISP_H_RES          240
#define DISP_V_RES          280

/* ST7789V2 panel offset: 240x320 panel, 240x280 visible, y offset = 20 */
#define DISP_OFFSET_X       0
#define DISP_OFFSET_Y       20

/* ---- Backlight LEDC config ---- */
#define BL_LEDC_TIMER       LEDC_TIMER_0
#define BL_LEDC_CHANNEL     LEDC_CHANNEL_0
#define BL_LEDC_SPEED_MODE  LEDC_LOW_SPEED_MODE
#define BL_LEDC_FREQ_HZ     5000
#define BL_LEDC_RESOLUTION  LEDC_TIMER_8_BIT

/* ---- SPI clock ---- */
#define LCD_SPI_CLOCK_HZ    (40 * 1000 * 1000)

/* ---- State ---- */
static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static bool s_i2c_initialized = false;
static bool s_touch_initialized = false;

/* ---- Backlight ---- */

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = BL_LEDC_SPEED_MODE,
        .duty_resolution = BL_LEDC_RESOLUTION,
        .timer_num       = BL_LEDC_TIMER,
        .freq_hz         = BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) return ret;

    ledc_channel_config_t ch_cfg = {
        .gpio_num   = LCD_PIN_BL,
        .speed_mode = BL_LEDC_SPEED_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ret = ledc_channel_config(&ch_cfg);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "Backlight PWM init OK (GPIO %d)", LCD_PIN_BL);
    return ESP_OK;
}

static void backlight_set_duty(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint32_t duty = (uint32_t)percent * 255 / 100;
    ledc_set_duty(BL_LEDC_SPEED_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_SPEED_MODE, BL_LEDC_CHANNEL);
}

/* ---- I2C helpers ---- */

static esp_err_t i2c_read_reg(uint8_t dev_addr, uint8_t reg, uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, data, len, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t init_i2c_bus(void)
{
    if (s_i2c_initialized) return ESP_OK;

    i2c_config_t i2c_cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = TOUCH_PIN_SDA,
        .scl_io_num       = TOUCH_PIN_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    esp_err_t ret = i2c_param_config(I2C_MASTER_NUM, &i2c_cfg);
    if (ret != ESP_OK) return ret;

    ret = i2c_driver_install(I2C_MASTER_NUM, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) return ret;

    s_i2c_initialized = true;
    ESP_LOGI(TAG, "I2C bus init OK (SDA=%d, SCL=%d)", TOUCH_PIN_SDA, TOUCH_PIN_SCL);
    return ESP_OK;
}

/* ---- Touch reset ---- */

static void touch_hw_reset(void)
{
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << TOUCH_PIN_RST),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_cfg);

    gpio_set_level(TOUCH_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TOUCH_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

/* ---- Public API ---- */

esp_err_t display_hal_init_panel(void)
{
    ESP_LOGI(TAG, "Initializing Waveshare LCD 1.69\" (ST7789V2 %dx%d)...",
             DISP_H_RES, DISP_V_RES);

    /* Step 1: Hardware reset via LCD_RST pin */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << LCD_PIN_RST),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_cfg);

    gpio_set_level(LCD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Step 2: Initialize backlight (off initially) */
    esp_err_t ret = backlight_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Backlight init failed: %s", esp_err_to_name(ret));
        /* Continue without PWM — we can still try the display */
    }

    /* Step 3: Initialize SPI bus */
    spi_bus_config_t bus_cfg = {
        .sclk_io_num     = LCD_PIN_SCK,
        .mosi_io_num     = LCD_PIN_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = DISP_H_RES * 40 * sizeof(uint16_t),
    };

    ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ESP_ERR_NOT_FOUND;
    }

    /* Step 4: Create panel IO (standard SPI, not QSPI) */
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num       = LCD_PIN_DC,
        .cs_gpio_num       = LCD_PIN_CS,
        .pclk_hz           = LCD_SPI_CLOCK_HZ,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
        .flags = {
            .dc_low_on_data = 0,
            .octal_mode     = 0,
        },
    };

    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &s_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel IO init failed: %s", esp_err_to_name(ret));
        spi_bus_free(SPI2_HOST);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "SPI panel IO created (%d MHz)", LCD_SPI_CLOCK_HZ / 1000000);

    /* Step 5: Create ST7789 panel */
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,  /* Already handled manually above */
        .rgb_endian     = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };

    ret = esp_lcd_new_panel_st7789(s_io_handle, &panel_config, &s_panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 panel create failed: %s", esp_err_to_name(ret));
        esp_lcd_panel_io_del(s_io_handle);
        spi_bus_free(SPI2_HOST);
        s_io_handle = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    /* Step 6: Initialize the panel */
    ret = esp_lcd_panel_reset(s_panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Panel reset failed: %s", esp_err_to_name(ret));
    }

    ret = esp_lcd_panel_init(s_panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel init failed: %s", esp_err_to_name(ret));
        esp_lcd_panel_del(s_panel_handle);
        esp_lcd_panel_io_del(s_io_handle);
        spi_bus_free(SPI2_HOST);
        s_panel_handle = NULL;
        s_io_handle = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    /* Step 7: ST7789V2 requires color inversion */
    esp_lcd_panel_invert_color(s_panel_handle, true);

    /* Step 8: Set panel gap for 240x280 in 240x320 panel */
    esp_lcd_panel_set_gap(s_panel_handle, DISP_OFFSET_X, DISP_OFFSET_Y);

    /* Step 9: Swap RGB565 bytes (big-endian for SPI) */
    esp_lcd_panel_swap_xy(s_panel_handle, false);
    esp_lcd_panel_mirror(s_panel_handle, false, false);

    /* Step 10: Turn on backlight */
    backlight_set_duty(100);

    /* Step 11: Draw test pattern — cyan bar at top, dark background */
    ESP_LOGI(TAG, "Drawing test pattern...");
    uint16_t *line_buf = heap_caps_malloc(DISP_H_RES * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (line_buf) {
        for (int y = 0; y < DISP_V_RES; y++) {
            /* Cyan bar in top 20 rows, dark gray elsewhere */
            uint16_t color = (y < 20) ? 0x07FF : 0x0841;
            for (int x = 0; x < DISP_H_RES; x++) {
                line_buf[x] = color;
            }
            esp_lcd_panel_draw_bitmap(s_panel_handle, 0, y, DISP_H_RES, y + 1, line_buf);
        }
        free(line_buf);
        ESP_LOGI(TAG, "Test pattern drawn");
    }

    ESP_LOGI(TAG, "ST7789V2 panel init OK (%dx%d, offset y=%d)",
             DISP_H_RES, DISP_V_RES, DISP_OFFSET_Y);
    return ESP_OK;
}

void display_hal_draw(int x_start, int y_start, int x_end, int y_end,
                      const void *color_data)
{
    if (!s_panel_handle) return;

    /* Clamp to display bounds */
    if (x_end > DISP_H_RES) x_end = DISP_H_RES;
    if (y_end > DISP_V_RES) y_end = DISP_V_RES;
    if (x_start >= x_end || y_start >= y_end) return;

    esp_lcd_panel_draw_bitmap(s_panel_handle, x_start, y_start, x_end, y_end, color_data);
}

esp_err_t display_hal_init_touch(void)
{
    ESP_LOGI(TAG, "Probing CST816 touch controller...");

    /* Init I2C bus if not already done */
    esp_err_t ret = init_i2c_bus();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed");
        return ESP_ERR_NOT_FOUND;
    }

    /* Hardware reset the touch controller */
    touch_hw_reset();

    /* Configure INT pin as input */
    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << TOUCH_PIN_INT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_cfg);

    /* Probe the CST816: read chip ID from register 0xA7 */
    uint8_t chip_id = 0;
    ret = i2c_read_reg(CST816_ADDR, 0xA7, &chip_id, 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "CST816 not found at 0x%02X (ret=%s)", CST816_ADDR, esp_err_to_name(ret));
        return ESP_ERR_NOT_FOUND;
    }

    if (chip_id == 0x00 || chip_id == 0xFF) {
        ESP_LOGW(TAG, "CST816 invalid chip ID: 0x%02X", chip_id);
        return ESP_ERR_NOT_FOUND;
    }

    s_touch_initialized = true;
    ESP_LOGI(TAG, "CST816 touch init OK (chip_id=0x%02X)", chip_id);
    return ESP_OK;
}

bool display_hal_touch_read(uint16_t *x, uint16_t *y)
{
    if (!s_touch_initialized) return false;

    /*
     * CST816 register map:
     *   0x01: gesture ID
     *   0x02: number of touch points
     *   0x03: X high [3:0] | event flag [7:6]
     *   0x04: X low [7:0]
     *   0x05: Y high [3:0] | touch ID [7:4]
     *   0x06: Y low [7:0]
     */
    uint8_t buf[6] = {0};
    esp_err_t ret = i2c_read_reg(CST816_ADDR, 0x01, buf, 6);
    if (ret != ESP_OK) return false;

    uint8_t num_points = buf[1] & 0x0F;
    if (num_points == 0) return false;

    *x = ((buf[2] & 0x0F) << 8) | buf[3];
    *y = ((buf[4] & 0x0F) << 8) | buf[5];

    /* Clamp to display bounds */
    if (*x >= DISP_H_RES) *x = DISP_H_RES - 1;
    if (*y >= DISP_V_RES) *y = DISP_V_RES - 1;

    return true;
}

void display_hal_set_brightness(uint8_t percent)
{
    backlight_set_duty(percent);
}

#endif /* CONFIG_DISPLAY_ENABLE */
