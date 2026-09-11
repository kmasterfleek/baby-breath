/**
 * @file display_heat.c
 * @brief "What the boards feel" heatmap strip charts (Hosyond 2.8" ILI9341).
 *
 * Layout (320x240 landscape):
 *   y 0..18      title "WHAT THE BOARDS FEEL"
 *   x 40..315    region 1 (Board 1) y 30..127, region 2 (Board 2) y 140..237
 *   x 2..38      labels "BOARD" / "1" and "BOARD" / "2"
 *
 * No framebuffer: each UDP packet is rendered as a single 1-px column with a
 * white cursor column ahead of it. Text uses a tiny 5x7 bitmap font drawn
 * through display_hal_draw_bitmap(). No LVGL.
 *
 * RAM: colour LUTs 1 KB, column buffer 196 B, cursor column 196 B, glyph
 * buffer 280 B, packet buffer 128 B, task stack 4 KB, one UDP socket.
 */

#include "display_heat.h"
#include "sdkconfig.h"

#if CONFIG_DISPLAY_ENABLE && CONFIG_DISPLAY_BOARD_HOSYOND_28_ILI9341

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "display_hal.h"

static const char *TAG = "disp_heat";

/* ---- Layout ---- */
#define SCR_W           320
#define SCR_H           240
#define TITLE_Y         2
#define REG_X           40
#define REG_W           276
#define REG_H           98
#define REG1_Y          30
#define REG2_Y          140
#define LABEL_X         4

/* ---- Task ---- */
#define HEAT_TASK_STACK     4096
#define HEAT_TASK_PRIO      1
#define HEAT_TASK_CORE      0
#define HEAT_RX_TIMEOUT_MS  500
#define HEAT_STALE_MS       3000
#define HEAT_STATS_MS       30000
#define HEAT_RX_BUF         128

/* ---- Font: 5x7, column-major, bit0 = top row. Space, '0'-'9', 'A'-'Z'. ---- */
#define FONT_W  5
#define FONT_H  7
#define FONT_MAX_SCALE 2

static const uint8_t s_font_digits[10][FONT_W] = {
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, {0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03}, {0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1E},
};
static const uint8_t s_font_alpha[26][FONT_W] = {
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
};

/* ---- Per-board state ---- */
typedef struct {
    int         y;              /* region top */
    int         cursor;         /* next data column, 0..REG_W-1 */
    TickType_t  last_rx;
    bool        waiting_drawn;  /* placeholder text currently shown */
    uint32_t    packets;
} heat_node_t;

static heat_node_t s_nodes[2];
static bool        s_ready = false;
static bool        s_started = false;
static uint32_t    s_dropped = 0;

/* ---- Buffers (internal RAM, DMA-capable) ---- */
static uint16_t s_lut[256];             /* blue->cyan->green->yellow->red */
static uint16_t s_lut_dim[256];         /* same, quarter brightness (board inactive) */
static uint16_t s_col[REG_H];           /* one data column */
static uint16_t s_cursor_col[REG_H];    /* constant white column */
static uint16_t s_glyph[FONT_W * FONT_MAX_SCALE * FONT_H * FONT_MAX_SCALE];

/* ---- Colours (panel byte order) ---- */
static uint16_t s_c_black, s_c_white, s_c_bg, s_c_frame, s_c_dim, s_c_label;

/* ---- Colormap ---- */
static void build_lut(void)
{
    static const uint8_t stops[5][3] = {
        {0, 0, 255}, {0, 255, 255}, {0, 255, 0}, {255, 255, 0}, {255, 0, 0},
    };
    for (int i = 0; i < 256; i++) {
        int seg = (i * 4) / 256;                 /* 0..3 */
        int f   = (i * 4) % 256;                 /* 0..255 within segment */
        int r = stops[seg][0] + ((stops[seg + 1][0] - stops[seg][0]) * f) / 255;
        int g = stops[seg][1] + ((stops[seg + 1][1] - stops[seg][1]) * f) / 255;
        int b = stops[seg][2] + ((stops[seg + 1][2] - stops[seg][2]) * f) / 255;
        s_lut[i]     = display_hal_rgb565((uint8_t)r, (uint8_t)g, (uint8_t)b);
        s_lut_dim[i] = display_hal_rgb565((uint8_t)(r >> 2), (uint8_t)(g >> 2), (uint8_t)(b >> 2));
    }
}

/* ---- Text ---- */
static const uint8_t *glyph_for(char c)
{
    if (c >= '0' && c <= '9') return s_font_digits[c - '0'];
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return s_font_alpha[c - 'A'];
    return NULL;   /* space and anything else: blank */
}

/* Draw one glyph at (x, y) scaled by `scale`; background is painted too. */
static void draw_char(int x, int y, char c, int scale, uint16_t fg, uint16_t bg)
{
    const uint8_t *g = glyph_for(c);
    int gw = FONT_W * scale, gh = FONT_H * scale;
    for (int row = 0; row < gh; row++) {
        for (int col = 0; col < gw; col++) {
            bool on = g && ((g[col / scale] >> (row / scale)) & 1);
            s_glyph[row * gw + col] = on ? fg : bg;
        }
    }
    display_hal_draw_bitmap(x, y, gw, gh, s_glyph);
}

static int text_width(const char *s, int scale)
{
    int n = (int)strlen(s);
    return n ? n * (FONT_W + 1) * scale - scale : 0;
}

static void draw_text(int x, int y, const char *s, int scale, uint16_t fg, uint16_t bg)
{
    if (scale < 1) scale = 1;
    if (scale > FONT_MAX_SCALE) scale = FONT_MAX_SCALE;
    for (; *s; s++) {
        draw_char(x, y, *s, scale, fg, bg);
        x += (FONT_W + 1) * scale;
    }
}

/* ---- Region drawing ---- */
static void clear_region(heat_node_t *n)
{
    display_hal_fill_rect(REG_X, n->y, REG_W, REG_H, s_c_bg);
    n->cursor = 0;
}

static void draw_waiting(heat_node_t *n)
{
    clear_region(n);
    const char *msg = "WAITING FOR SERVER";
    int w = text_width(msg, 1);
    draw_text(REG_X + (REG_W - w) / 2, n->y + (REG_H - FONT_H) / 2, msg, 1, s_c_dim, s_c_bg);
    n->waiting_drawn = true;
}

static void draw_chrome(void)
{
    display_hal_fill_rect(0, 0, SCR_W, SCR_H, s_c_black);

    const char *title = "WHAT THE BOARDS FEEL";
    draw_text((SCR_W - text_width(title, 2)) / 2, TITLE_Y, title, 2, s_c_white, s_c_black);

    const char *digits[2] = { "1", "2" };
    for (int i = 0; i < 2; i++) {
        heat_node_t *n = &s_nodes[i];
        /* 1-px frame around the region */
        display_hal_fill_rect(REG_X - 1, n->y - 1, REG_W + 2, 1, s_c_frame);
        display_hal_fill_rect(REG_X - 1, n->y + REG_H, REG_W + 2, 1, s_c_frame);
        display_hal_fill_rect(REG_X - 1, n->y - 1, 1, REG_H + 2, s_c_frame);
        display_hal_fill_rect(REG_X + REG_W, n->y - 1, 1, REG_H + 2, s_c_frame);
        /* Labels at x 2..38: "BOARD" (29 px) over a big digit */
        draw_text(LABEL_X, n->y + 36, "BOARD", 1, s_c_label, s_c_black);
        draw_text(LABEL_X + 10, n->y + 48, digits[i], 2, s_c_white, s_c_black);
        draw_waiting(n);
    }
}

/* Render one packet payload as a column and advance the cursor. */
static void draw_packet(heat_node_t *n, const uint8_t *bins, int n_bins, bool active)
{
    if (n->waiting_drawn) {
        clear_region(n);
        n->waiting_drawn = false;
    }
    const uint16_t *lut = active ? s_lut : s_lut_dim;
    for (int r = 0; r < REG_H; r++) {
        int bin = ((REG_H - 1 - r) * n_bins) / REG_H;   /* bin 0 at the bottom */
        s_col[r] = lut[bins[bin]];
    }
    display_hal_draw_bitmap(REG_X + n->cursor, n->y, 1, REG_H, s_col);
    n->cursor = (n->cursor + 1) % REG_W;
    display_hal_draw_bitmap(REG_X + n->cursor, n->y, 1, REG_H, s_cursor_col);
}

/* ---- UDP receive task ---- */
static void heat_task(void *arg)
{
    int sock = (int)(intptr_t)arg;
    uint8_t buf[HEAT_RX_BUF];
    TickType_t last_stats = xTaskGetTickCount();

    ESP_LOGI(TAG, "Heat display task running on Core %d, UDP port %d",
             xPortGetCoreID(), CONFIG_DISPLAY_HEAT_UDP_PORT);

    while (1) {
        int len = recv(sock, buf, sizeof(buf), 0);
        TickType_t now = xTaskGetTickCount();

        if (len >= DISPLAY_HEAT_HDR_LEN) {
            uint32_t magic = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                             ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
            int node_id = buf[4];
            int n_bins  = buf[5];
            bool active = (buf[6] & 0x01) != 0;
            if (magic == DISPLAY_HEAT_MAGIC && (node_id == 1 || node_id == 2) &&
                n_bins >= 1 && n_bins <= DISPLAY_HEAT_MAX_BINS &&
                len >= DISPLAY_HEAT_HDR_LEN + n_bins) {
                heat_node_t *n = &s_nodes[node_id - 1];
                draw_packet(n, buf + DISPLAY_HEAT_HDR_LEN, n_bins, active);
                n->last_rx = now;
                n->packets++;
            } else {
                s_dropped++;
            }
        } else if (len >= 0) {
            s_dropped++;
        }
        /* len < 0 with EAGAIN/EWOULDBLOCK is just the receive timeout. */

        for (int i = 0; i < 2; i++) {
            heat_node_t *n = &s_nodes[i];
            if (!n->waiting_drawn && (now - n->last_rx) >= pdMS_TO_TICKS(HEAT_STALE_MS)) {
                draw_waiting(n);
            }
        }

        if ((now - last_stats) >= pdMS_TO_TICKS(HEAT_STATS_MS)) {
            ESP_LOGI(TAG, "rows: board1=%lu board2=%lu dropped=%lu",
                     (unsigned long)s_nodes[0].packets, (unsigned long)s_nodes[1].packets,
                     (unsigned long)s_dropped);
            last_stats = now;
        }
    }
}

/* ---- Public API ---- */

esp_err_t display_heat_init(void)
{
    esp_err_t ret = display_hal_init_panel();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Display not available — running headless");
        return ret;
    }

    s_c_black = display_hal_rgb565(0, 0, 0);
    s_c_white = display_hal_rgb565(255, 255, 255);
    s_c_bg    = display_hal_rgb565(8, 8, 16);
    s_c_frame = display_hal_rgb565(64, 64, 80);
    s_c_dim   = display_hal_rgb565(96, 96, 96);
    s_c_label = display_hal_rgb565(0, 212, 255);
    build_lut();
    for (int r = 0; r < REG_H; r++) s_cursor_col[r] = s_c_white;

    memset(s_nodes, 0, sizeof(s_nodes));
    s_nodes[0].y = REG1_Y;
    s_nodes[1].y = REG2_Y;
    draw_chrome();

    s_ready = true;
    ESP_LOGI(TAG, "Heat display ready (2 x %dx%d strips)", REG_W, REG_H);
    return ESP_OK;
}

esp_err_t display_heat_start(void)
{
    if (!s_ready) {
        ESP_LOGW(TAG, "Panel not initialised — heat task not started");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_started) return ESP_OK;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return ESP_FAIL;
    }
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(CONFIG_DISPLAY_HEAT_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind(%d) failed: errno %d", CONFIG_DISPLAY_HEAT_UDP_PORT, errno);
        close(sock);
        return ESP_FAIL;
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = HEAT_RX_TIMEOUT_MS * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    BaseType_t xret = xTaskCreatePinnedToCore(
        heat_task, "disp_heat", HEAT_TASK_STACK,
        (void *)(intptr_t)sock, HEAT_TASK_PRIO, NULL, HEAT_TASK_CORE);
    if (xret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create heat display task");
        close(sock);
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "Heat display task started (Core %d, prio %d, UDP %d)",
             HEAT_TASK_CORE, HEAT_TASK_PRIO, CONFIG_DISPLAY_HEAT_UDP_PORT);
    return ESP_OK;
}

#endif /* CONFIG_DISPLAY_ENABLE && CONFIG_DISPLAY_BOARD_HOSYOND_28_ILI9341 */
