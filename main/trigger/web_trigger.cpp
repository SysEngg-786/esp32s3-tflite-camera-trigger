// File: web_trigger.cpp
// Path: esp32s3-tflite-camera-trigger/main/trigger/web_trigger.cpp
// Role: Trigger module implementation — WiFi init and HTTP POST dispatch.
//       STUB: all functions return safe defaults. No WiFi is started,
//             no HTTP request is sent.
//       Real implementation replaces stub bodies in the trigger pass.
//
// Real implementation will:
//   wifi_connect()  — esp_netif_init(), esp_event_loop_create_default(),
//                     esp_wifi_init/set_config/start/connect(), block on
//                     IP_EVENT_STA_GOT_IP with a connection timeout.
//   trigger_send()  — esp_http_client_init() with endpoint_url, build
//                     JSON body {"class_id":N,"confidence":F}, perform(),
//                     cleanup(). MVP: no retry on failure.

#include "web_trigger.h"
#include "esp_log.h"

// Module log tag
static const char* TAG = "trigger";

// ── wifi_connect ───────────────────────────────────────────────────────────
// STUB: will initialise the WiFi stack in STA mode, associate with the AP,
//   and block until an IP address is assigned or the connection times out.
esp_err_t wifi_connect(const char* ssid, const char* password)
{
    // Silence unused-parameter warnings in the stub build
    (void)ssid;
    (void)password;

    ESP_LOGI(TAG, "wifi_connect: stub — no WiFi started");
    return ESP_OK;
}

// ── trigger_send ───────────────────────────────────────────────────────────
// STUB: will POST {"class_id":<class_id>,"confidence":<confidence>}
//   to endpoint_url via esp_http_client. Fire-and-forget at MVP.
esp_err_t trigger_send(const char* endpoint_url,
                       int         class_id,
                       float       confidence)
{
    // Silence unused-parameter warnings in the stub build
    (void)endpoint_url;
    (void)class_id;
    (void)confidence;

    ESP_LOGI(TAG, "trigger_send: stub — no HTTP request sent");
    return ESP_OK;
}