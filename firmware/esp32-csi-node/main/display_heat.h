/**
 * @file display_heat.h
 * @brief "What the boards feel" — UDP-fed heatmap strip charts for the
 *        Hosyond ESP32-S3 2.8" ILI9341 board.
 *
 * Only compiled when CONFIG_DISPLAY_BOARD_HOSYOND_28_ILI9341 is selected.
 * The header is safe to include in any build (declarations only).
 *
 * Wire format (UDP, CONFIG_DISPLAY_HEAT_UDP_PORT, sent by the sensing-server):
 *   [0..3]  u32 LE magic 0xC5110010
 *   [4]     node_id  (1 or 2)
 *   [5]     n_bins   (1..64)
 *   [6]     flags    (bit0 = board active)
 *   [7]     reserved
 *   [8..]   n_bins x u8: 0 = much weaker than usual, 128 = usual, 255 = much stronger
 *
 * Each packet becomes one 1-px column in that board's region; the cursor
 * advances oscilloscope-style and wraps. Bin 0 is drawn at the bottom.
 */

#ifndef DISPLAY_HEAT_H
#define DISPLAY_HEAT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_HEAT_MAGIC      0xC5110010u
#define DISPLAY_HEAT_MAX_BINS   64
#define DISPLAY_HEAT_HDR_LEN    8

/**
 * Initialise the panel and draw the static chrome (title, labels, frames,
 * "WAITING FOR SERVER" placeholders). Call early, before WiFi.
 *
 * @return ESP_OK, or ESP_ERR_NOT_FOUND if the panel did not come up
 *         (the node then runs headless; display_heat_start() becomes a no-op).
 */
esp_err_t display_heat_init(void);

/**
 * Bind the UDP socket and start the receiver/drawer task (Core 0, priority 1).
 * Requires the network stack to be initialised. Safe to call once.
 */
esp_err_t display_heat_start(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_HEAT_H */
