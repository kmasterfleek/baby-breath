/**
 * @file ble_prov.h
 * @brief BLE WiFi Provisioning via ESP-IDF wifi_provisioning manager.
 *
 * When no WiFi SSID is configured in NVS, the firmware enters BLE
 * provisioning mode.  The device advertises as "MYBABY_XXXX" (last 4
 * hex digits of the WiFi MAC address) and accepts credentials from a
 * companion app using Security 1 with proof-of-possession.
 *
 * A custom endpoint "target-config" is registered to accept the
 * aggregator target_ip, target_port, and node_id in JSON.
 */

#ifndef BLE_PROV_H
#define BLE_PROV_H

#include "esp_err.h"

/**
 * Run BLE WiFi provisioning.
 *
 * Blocks until the device has received WiFi credentials and connected
 * successfully.  On completion the credentials and any target-config
 * data are written to NVS namespace "csi_cfg" so that subsequent boots
 * use the stored values directly.
 *
 * Calls wifi_prov_mgr_deinit() before returning to free BLE memory.
 *
 * @return ESP_OK on success, or an error code.
 */
esp_err_t ble_prov_run(void);

#endif /* BLE_PROV_H */
