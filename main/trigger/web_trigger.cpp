// File: web_trigger.cpp
// Path: esp32s3-tflite-camera-trigger/main/trigger/web_trigger.cpp
// Role: Trigger module implementation — WiFi station init and HTTP POST dispatch.
//
// wifi_connect():
//   Initialises the ESP-IDF WiFi stack in station mode, associates with the AP
//   configured via Kconfig (CONFIG_WIFI_SSID / CONFIG_WIFI_PASSWORD), and blocks
//   until an IP address is assigned or the timeout expires.
//
// trigger_send():
//   POSTs a JSON detection payload to CONFIG_TRIGGER_ENDPOINT_URL via
//   esp_http_client. Fire-and-forget at MVP — no retry, no HTTPS, no
//   response validation beyond transport success.
//
// MVP notes (named, not silent omissions):
//   - Plain HTTP only — no TLS, no certificate validation.
//   - Single connection attempt in wifi_connect() — no reconnect loop.
//   - No retry on trigger_send() failure.
//   - WiFi event handling uses a FreeRTOS EventGroup for clean blocking.
//   Post-MVP: reconnect handler, TLS, retry with backoff.

#include "web_trigger.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdio.h>       // snprintf
#include <string.h>      // strlen

// Module log tag
static const char* TAG = "trigger";

// ── WiFi event group ──────────────────────────────────────────────────────
// Two bits signal the outcome of a connection attempt.
// wifi_connect() blocks on these bits; the event handler sets them.
#define WIFI_CONNECTED_BIT  BIT0   // IP address assigned — ready to send
#define WIFI_FAIL_BIT       BIT1   // association failed — abort

static EventGroupHandle_t s_wifi_event_group = nullptr;

// Retry counter — MVP: one attempt only. Post-MVP: configurable retry count.
static int s_retry_count = 0;
static constexpr int MAX_RETRY = 1;

// ── WiFi event handler ────────────────────────────────────────────────────
// Registered with the ESP-IDF event loop. Handles station connect, disconnect,
// and IP assignment events. Sets the appropriate EventGroup bit on each outcome.
static void wifi_event_handler(void*            arg,
                               esp_event_base_t event_base,
                               int32_t          event_id,
                               void*            event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // Station started — initiate association
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < MAX_RETRY) {
            // Retry once on transient disconnect during association
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "wifi: association failed — retrying (%d/%d)",
                     s_retry_count, MAX_RETRY);
        } else {
            // Retry limit reached — signal failure to wifi_connect()
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "wifi: association failed after %d attempt(s)", MAX_RETRY);
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // IP address assigned — signal success to wifi_connect()
        ip_event_got_ip_t* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(TAG, "wifi: connected — IP " IPSTR,
                 IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// ── wifi_connect ───────────────────────────────────────────────────────────
// Initialises the ESP-IDF WiFi stack in station (STA) mode and associates
// with the AP. Blocks until connected (IP assigned) or fails.
//
// ssid / password: sourced from Kconfig via main.cpp — not from this module.
// Returns ESP_OK on successful IP assignment; ESP_FAIL on timeout or error.
esp_err_t wifi_connect(const char* ssid, const char* password)
{
    ESP_LOGI(TAG, "wifi_connect: associating with SSID \"%s\"", ssid);

    // Create the EventGroup used to block until connection outcome is known
    s_wifi_event_group = xEventGroupCreate();

    // Initialise the TCP/IP network interface layer
    ESP_ERROR_CHECK(esp_netif_init());

    // Create the default event loop (required for WiFi events)
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create the default WiFi station network interface
    esp_netif_create_default_wifi_sta();

    // Initialise the WiFi driver with default configuration
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handler for WiFi and IP events
    esp_event_handler_instance_t wifi_handler_instance;
    esp_event_handler_instance_t ip_handler_instance;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, nullptr, &wifi_handler_instance));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, nullptr, &ip_handler_instance));

    // Configure station with SSID and password from Kconfig
    wifi_config_t wifi_config = {};
    strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid),
            ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy(reinterpret_cast<char*>(wifi_config.sta.password),
            password, sizeof(wifi_config.sta.password) - 1);

    // Set station mode and apply config
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // Start the WiFi driver — WIFI_EVENT_STA_START triggers esp_wifi_connect()
    ESP_ERROR_CHECK(esp_wifi_start());

    // Block until WIFI_CONNECTED_BIT or WIFI_FAIL_BIT is set by the event handler
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,    // do not clear bits on exit
        pdFALSE,    // wait for any bit (OR)
        pdMS_TO_TICKS(CONFIG_WIFI_CONNECT_TIMEOUT_MS));

    // Unregister event handlers — no longer needed after connection outcome known
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler_instance));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler_instance));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "wifi_connect: connected successfully");
        return ESP_OK;
    }

    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "wifi_connect: association failed");
        return ESP_FAIL;
    }

    // Neither bit set — EventGroup timed out
    ESP_LOGE(TAG, "wifi_connect: timed out after %d ms",
             CONFIG_WIFI_CONNECT_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}

// ── trigger_send ───────────────────────────────────────────────────────────
// POSTs a JSON detection payload to endpoint_url via esp_http_client.
// Payload: {"class_id":<class_id>,"confidence":<confidence>}
// Fire-and-forget at MVP — no retry on failure, no response validation.
// Returns ESP_OK if the request was dispatched without transport error.
esp_err_t trigger_send(const char* endpoint_url,
                       int         class_id,
                       float       confidence)
{
    // Build JSON payload — fixed-size buffer, no heap allocation
    char payload[64];
    snprintf(payload, sizeof(payload),
             "{\"class_id\":%d,\"confidence\":%.3f}",
             class_id, confidence);

    ESP_LOGI(TAG, "trigger_send: POST %s payload=%s", endpoint_url, payload);

    // Configure HTTP client — plain HTTP at MVP (no TLS)
    // Post-MVP: set .transport_type = HTTP_TRANSPORT_OVER_SSL, add cert bundle
    esp_http_client_config_t config = {};
    config.url    = endpoint_url;
    config.method = HTTP_METHOD_POST;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "trigger_send: esp_http_client_init failed");
        return ESP_FAIL;
    }

    // Set Content-Type header and JSON body
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload,
                                   static_cast<int>(strlen(payload)));

    // Perform the request — blocks until TCP ACK or error
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "trigger_send: HTTP %d", status);
    } else {
        // MVP: log and continue — no retry
        ESP_LOGW(TAG, "trigger_send: HTTP request failed: %s",
                 esp_err_to_name(err));
    }

    // Always clean up the client handle
    esp_http_client_cleanup(client);

    return err;
}
