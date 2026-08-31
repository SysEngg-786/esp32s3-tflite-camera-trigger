// File: web_trigger.cpp
// Path: esp32s3-tflite-camera-trigger/main/trigger/web_trigger.cpp
// Role: Trigger module — WiFi station init and HTTP POST dispatch.
//
// Inference mode design — root cause of WiFi cycling fixed:
//   When detection_task calls esp_wifi_stop(), WIFI_EVENT_STA_DISCONNECTED
//   fires. The persistent handler was calling esp_wifi_connect() on every
//   disconnect — causing indefinite cycling even when WiFi was intentionally
//   stopped for inference. s_inference_mode flag suppresses this.
//
//   s_inference_mode=true: handler does NOT reconnect on disconnect or start.
//   s_inference_mode=false: handler reconnects normally (boot, detection path).
//
//   detection_task sets flag via wifi_set_inference_mode() — see main.cpp.
//
// Persistent handler — remains registered for device lifetime.
//   Handles IP assignment events on every reconnect cycle.
//   wifi_ready() reflects current IP state.

#include "web_trigger.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdio.h>
#include <string.h>

static const char* TAG = "trigger";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

#define WIFI_SSID_HARDCODED  "Virus...1"
#define WIFI_PASS_HARDCODED  "PakIstaN1"

// ── Persistent state ───────────────────────────────────────────────────────
static volatile bool      s_ip_obtained      = false;
static volatile bool      s_inference_mode   = false;  // suppresses auto-reconnect
static EventGroupHandle_t s_wifi_event_group = nullptr;
static int                s_retry_count      = 0;
static constexpr int      MAX_RETRY          = 3;

// ── wifi_ready ─────────────────────────────────────────────────────────────
bool wifi_ready(void)
{
    return s_ip_obtained;
}

// ── wifi_set_inference_mode ────────────────────────────────────────────────
// true  → handler suppresses all reconnect attempts (WiFi intentionally stopped)
// false → handler reconnects normally (boot path, detection trigger path)
void wifi_set_inference_mode(bool inference_active)
{
    s_inference_mode = inference_active;
    if (inference_active) {
        s_ip_obtained = false;   // clear IP flag — WiFi going offline
        s_retry_count = 0;       // reset retry counter
    }
}

// ── WiFi event handler — PERSISTENT ────────────────────────────────────────
static void wifi_event_handler(void*            arg,
                               esp_event_base_t event_base,
                               int32_t          event_id,
                               void*            event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // Only auto-connect when NOT in inference mode.
        // In inference mode, esp_wifi_start() is called by detection_task
        // for the boot sequence only — connect is called explicitly there.
        if (!s_inference_mode) {
            esp_wifi_connect();
        }

    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip_obtained = false;

        wifi_event_sta_disconnected_t* disconn =
            static_cast<wifi_event_sta_disconnected_t*>(event_data);
        ESP_LOGW(TAG, "disconnected — reason: %d (0x%x)",
                 disconn->reason, disconn->reason);

        if (s_inference_mode) {
            // Intentional WiFi stop for inference — do not retry.
            // detection_task controls when WiFi restarts.
            s_retry_count = 0;
            return;
        }

        // Normal disconnect — retry up to MAX_RETRY
        if (s_retry_count < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "wifi: retrying (%d/%d)", s_retry_count, MAX_RETRY);
        } else {
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
            ESP_LOGE(TAG, "wifi: association failed after %d attempt(s)", MAX_RETRY);
            s_retry_count = 0;
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(TAG, "wifi: connected — IP " IPSTR,
                 IP2STR(&event->ip_info.ip));
        s_ip_obtained = true;
        s_retry_count = 0;
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

// ── wifi_connect ───────────────────────────────────────────────────────────
esp_err_t wifi_connect(const char* ssid, const char* password)
{
    (void)ssid;
    (void)password;

    ESP_LOGI(TAG, "wifi_connect: SSID=[%s]", WIFI_SSID_HARDCODED);

    s_wifi_event_group = xEventGroupCreate();

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS erase required");
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register persistent handlers — never unregistered
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, nullptr));

    wifi_config_t wifi_config = {};
    strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid),
            WIFI_SSID_HARDCODED, sizeof(wifi_config.sta.ssid) - 1);
    strncpy(reinterpret_cast<char*>(wifi_config.sta.password),
            WIFI_PASS_HARDCODED, sizeof(wifi_config.sta.password) - 1);

    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable    = true;
    wifi_config.sta.pmf_cfg.required   = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // s_inference_mode=false at this point — handler will auto-connect on STA_START
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(CONFIG_WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "wifi_connect: connected successfully");
        return ESP_OK;
    }
    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "wifi_connect: association failed");
        return ESP_FAIL;
    }
    ESP_LOGE(TAG, "wifi_connect: timed out after %d ms",
             CONFIG_WIFI_CONNECT_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}

// ── trigger_send ───────────────────────────────────────────────────────────
esp_err_t trigger_send(const char* endpoint_url,
                       int         class_id,
                       float       confidence)
{
    char payload[64];
    snprintf(payload, sizeof(payload),
             "{\"class_id\":%d,\"confidence\":%.3f}",
             class_id, confidence);

    ESP_LOGI(TAG, "trigger_send: POST %s payload=%s", endpoint_url, payload);

    esp_http_client_config_t config = {};
    config.url    = endpoint_url;
    config.method = HTTP_METHOD_POST;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "trigger_send: esp_http_client_init failed");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload,
                                   static_cast<int>(strlen(payload)));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "trigger_send: HTTP %d",
                 esp_http_client_get_status_code(client));
    } else {
        ESP_LOGW(TAG, "trigger_send: failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}
