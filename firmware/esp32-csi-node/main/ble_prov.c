/**
 * @file ble_prov.c
 * @brief BLE WiFi Provisioning via ESP-IDF wifi_provisioning manager.
 *
 * Implements BLE-based provisioning flow:
 *   1. Initialize wifi_prov_mgr with BLE transport (NimBLE).
 *   2. Register custom endpoint "target-config" for server IP/port/node_id.
 *   3. Advertise as "MYBABY_XXXX" (last 4 hex of WiFi MAC).
 *   4. On success, write SSID/password/target config to NVS "csi_cfg".
 *   5. Deinit wifi_prov_mgr to free BLE memory.
 *
 * Security: Security 1 with proof-of-possession from Kconfig
 * (CONFIG_PROV_POP, default "mybaby123").
 */

#include "ble_prov.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "nvs_config.h"

static const char *TAG = "ble_prov";

/* ------------------------------------------------------------------ */
/*  Event bits for provisioning completion                            */
/* ------------------------------------------------------------------ */

#define PROV_DONE_BIT   BIT0
#define PROV_FAIL_BIT   BIT1
#define WIFI_GOT_IP_BIT BIT2

static EventGroupHandle_t s_prov_event_group;

/* ------------------------------------------------------------------ */
/*  Received target-config data (written to NVS after WiFi connects)  */
/* ------------------------------------------------------------------ */

static char     s_target_ip[NVS_CFG_IP_MAX];
static uint16_t s_target_port;
static uint8_t  s_node_id;
static bool     s_target_cfg_received;

/* ------------------------------------------------------------------ */
/*  Custom endpoint handler: "target-config"                          */
/* ------------------------------------------------------------------ */

/**
 * Parse a simple JSON payload for target-config.
 *
 * Expected format (whitespace-tolerant):
 *   {"target_ip":"192.168.1.20","target_port":5005,"node_id":1}
 *
 * Uses a minimal hand-rolled parser to avoid pulling in cJSON.
 */
static esp_err_t target_config_handler(uint32_t session_id,
                                       const uint8_t *inbuf, ssize_t inlen,
                                       uint8_t **outbuf, ssize_t *outlen,
                                       void *priv_data)
{
    (void)session_id;
    (void)priv_data;

    if (inbuf == NULL || inlen <= 0) {
        ESP_LOGW(TAG, "target-config: empty payload");
        const char *err_msg = "ERR: empty payload";
        *outbuf = (uint8_t *)strdup(err_msg);
        *outlen = (ssize_t)strlen(err_msg);
        return ESP_OK;
    }

    /* Null-terminate for safe string ops. */
    char *json = calloc(1, (size_t)inlen + 1);
    if (json == NULL) {
        const char *err_msg = "ERR: alloc";
        *outbuf = (uint8_t *)strdup(err_msg);
        *outlen = (ssize_t)strlen(err_msg);
        return ESP_OK;
    }
    memcpy(json, inbuf, (size_t)inlen);

    ESP_LOGI(TAG, "target-config payload: %s", json);

    /* Parse target_ip */
    const char *ip_key = "\"target_ip\"";
    char *ip_pos = strstr(json, ip_key);
    if (ip_pos) {
        /* Find the opening quote of the value. */
        ip_pos += strlen(ip_key);
        char *colon = strchr(ip_pos, ':');
        if (colon) {
            char *q1 = strchr(colon, '"');
            if (q1) {
                q1++;
                char *q2 = strchr(q1, '"');
                if (q2) {
                    size_t len = (size_t)(q2 - q1);
                    if (len < NVS_CFG_IP_MAX) {
                        memcpy(s_target_ip, q1, len);
                        s_target_ip[len] = '\0';
                    }
                }
            }
        }
    }

    /* Parse target_port (integer value) */
    const char *port_key = "\"target_port\"";
    char *port_pos = strstr(json, port_key);
    if (port_pos) {
        port_pos += strlen(port_key);
        char *colon = strchr(port_pos, ':');
        if (colon) {
            s_target_port = (uint16_t)strtoul(colon + 1, NULL, 10);
        }
    }

    /* Parse node_id (integer value) */
    const char *nid_key = "\"node_id\"";
    char *nid_pos = strstr(json, nid_key);
    if (nid_pos) {
        nid_pos += strlen(nid_key);
        char *colon = strchr(nid_pos, ':');
        if (colon) {
            s_node_id = (uint8_t)strtoul(colon + 1, NULL, 10);
        }
    }

    s_target_cfg_received = true;
    ESP_LOGI(TAG, "target-config parsed: ip=%s port=%u node_id=%u",
             s_target_ip, (unsigned)s_target_port, (unsigned)s_node_id);

    free(json);

    const char *ok_msg = "OK";
    *outbuf = (uint8_t *)strdup(ok_msg);
    *outlen = (ssize_t)strlen(ok_msg);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Event handler for provisioning + WiFi events                      */
/* ------------------------------------------------------------------ */

static void prov_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
        case WIFI_PROV_START:
            ESP_LOGI(TAG, "Provisioning started — waiting for credentials via BLE");
            break;
        case WIFI_PROV_CRED_RECV: {
            wifi_sta_config_t *sta = (wifi_sta_config_t *)event_data;
            ESP_LOGI(TAG, "Received WiFi credentials: SSID=%s", (const char *)sta->ssid);
            break;
        }
        case WIFI_PROV_CRED_FAIL: {
            wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
            ESP_LOGE(TAG, "Provisioning failed: reason=%d", (int)*reason);
            xEventGroupSetBits(s_prov_event_group, PROV_FAIL_BIT);
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "Provisioning successful — device connected to WiFi");
            xEventGroupSetBits(s_prov_event_group, PROV_DONE_BIT);
            break;
        case WIFI_PROV_END:
            ESP_LOGI(TAG, "Provisioning ended");
            break;
        default:
            break;
        }
    } else if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGI(TAG, "WiFi disconnected during provisioning, retrying...");
            esp_wifi_connect();
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_prov_event_group, WIFI_GOT_IP_BIT);
    }
}

/* ------------------------------------------------------------------ */
/*  Save provisioned credentials to NVS "csi_cfg"                    */
/* ------------------------------------------------------------------ */

static esp_err_t save_provisioned_config(void)
{
    /* Retrieve the WiFi config that wifi_prov_mgr wrote. */
    wifi_config_t wifi_cfg;
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get WiFi config: %s", esp_err_to_name(err));
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open("csi_cfg", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for write: %s", esp_err_to_name(err));
        return err;
    }

    /* Save SSID */
    nvs_set_str(handle, "ssid", (const char *)wifi_cfg.sta.ssid);

    /* Save password */
    nvs_set_str(handle, "password", (const char *)wifi_cfg.sta.password);

    ESP_LOGI(TAG, "Saved WiFi credentials to NVS (SSID=%s)", wifi_cfg.sta.ssid);

    /* Save target config if received via custom endpoint. */
    if (s_target_cfg_received) {
        if (s_target_ip[0] != '\0') {
            nvs_set_str(handle, "target_ip", s_target_ip);
            ESP_LOGI(TAG, "Saved target_ip=%s to NVS", s_target_ip);
        }
        if (s_target_port > 0) {
            nvs_set_u16(handle, "target_port", s_target_port);
            ESP_LOGI(TAG, "Saved target_port=%u to NVS", (unsigned)s_target_port);
        }
        nvs_set_u8(handle, "node_id", s_node_id);
        ESP_LOGI(TAG, "Saved node_id=%u to NVS", (unsigned)s_node_id);
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
    }
    nvs_close(handle);

    return err;
}

/* ------------------------------------------------------------------ */
/*  Build the BLE service name: "MYBABY_XXXX"                        */
/* ------------------------------------------------------------------ */

static void get_service_name(char *name, size_t max_len)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(name, max_len, "MYBABY_%02X%02X", mac[4], mac[5]);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

esp_err_t ble_prov_run(void)
{
    esp_err_t err;

    /* Reset target-config state. */
    s_target_ip[0] = '\0';
    s_target_port = 0;
    s_node_id = 0;
    s_target_cfg_received = false;

    s_prov_event_group = xEventGroupCreate();
    if (s_prov_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    /* Initialize networking stack. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    /* Register event handlers. */
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &prov_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &prov_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &prov_event_handler, NULL));

    /* Initialize provisioning manager with BLE transport. */
    wifi_prov_mgr_config_t prov_cfg = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    err = wifi_prov_mgr_init(prov_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_prov_mgr_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Register custom endpoint for target configuration. */
    err = wifi_prov_mgr_endpoint_create("target-config");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create target-config endpoint: %s",
                 esp_err_to_name(err));
        /* Non-fatal: provisioning can still work without target config. */
    }

    /* Build the BLE service name. */
    char service_name[16];
    get_service_name(service_name, sizeof(service_name));

    /* Proof of possession from Kconfig. */
#ifdef CONFIG_PROV_POP
    const char *pop = CONFIG_PROV_POP;
#else
    const char *pop = "mybaby123";
#endif

    ESP_LOGI(TAG, "Starting BLE provisioning: name=%s, pop=%s", service_name, pop);

    /* Start provisioning (Security 1 + proof of possession). */
    err = wifi_prov_mgr_start_provisioning(
        WIFI_PROV_SECURITY_1, pop, service_name, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_prov_mgr_start_provisioning failed: %s",
                 esp_err_to_name(err));
        wifi_prov_mgr_deinit();
        return err;
    }

    /* Register the target-config endpoint handler (must be after start). */
    wifi_prov_mgr_endpoint_register(
        "target-config", target_config_handler, NULL);

    /* Wait for provisioning to complete or fail. */
    ESP_LOGI(TAG, "Waiting for provisioning to complete...");
    EventBits_t bits = xEventGroupWaitBits(
        s_prov_event_group,
        PROV_DONE_BIT | PROV_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & PROV_FAIL_BIT) {
        ESP_LOGE(TAG, "Provisioning failed — restarting...");
        wifi_prov_mgr_deinit();
        vEventGroupDelete(s_prov_event_group);
        esp_restart();
        return ESP_FAIL; /* Not reached. */
    }

    /* Wait for IP address. */
    ESP_LOGI(TAG, "Provisioning succeeded, waiting for IP...");
    xEventGroupWaitBits(s_prov_event_group, WIFI_GOT_IP_BIT,
                        pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    /* Save the provisioned credentials to NVS "csi_cfg". */
    save_provisioned_config();

    /* Reload g_nvs_config so the rest of app_main sees the new values. */
    extern nvs_config_t g_nvs_config;
    nvs_config_load(&g_nvs_config);

    /* Clean up provisioning manager to free BLE memory (~60 KB). */
    wifi_prov_mgr_deinit();

    /* Unregister provisioning event handlers. */
    esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &prov_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &prov_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &prov_event_handler);

    vEventGroupDelete(s_prov_event_group);
    s_prov_event_group = NULL;

    ESP_LOGI(TAG, "BLE provisioning complete — proceeding to CSI operation");
    return ESP_OK;
}
