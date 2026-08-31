/* File: main.c
 * Path: esp32s3-tflite-camera-trigger/tests/wifi_sta/main/main.c
 * Role: Isolated WiFi station connectivity test.
 *
 * PURPOSE OF THIS TEST:
 *   Verify ESP32-S3 WiFi STA connectivity using Espressif's reference
 *   pattern verbatim — no camera, no TFLite, no custom code beyond the
 *   standard WiFi station bring-up sequence.
 *
 * DIAGNOSTIC VALUE:
 *   Created during the main project WiFi association diagnostic pass
 *   (2026-08-29) to isolate whether the WiFi failure was in our code
 *   or in the environment (AP policy, MAC filtering, band steering).
 *
 *   Result interpretation:
 *     This test connects, main project does not
 *       → Bug is in main project WiFi init sequence. Compare the two.
 *     Both fail with same error code
 *       → Environmental issue (AP policy, MAC block, PMF requirement).
 *     Both connect
 *       → WiFi stack is fine. Recheck main project.
 *
 * CREDENTIALS:
 *   Hardcoded directly — menuconfig bypassed entirely to eliminate
 *   any encoding variable from Kconfig. Update WIFI_PASS before building.
 *   Restore CONFIG_ macros after connectivity is confirmed.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#define WIFI_SSID    "zkb"
#define WIFI_PASS    "test1234"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"

/* ── HARDCODED CREDENTIALS — update WIFI_PASS before building ────────────
 * Menuconfig bypassed entirely. Zero encoding risk, zero Kconfig involvement.
 * Remove this block and restore CONFIG_ macros after connectivity confirmed. */
//#define WIFI_SSID    "Virus...1"
//#define WIFI_PASS    "PakIstaN"
//#define WIFI_SSID    "zkb"
//#define WIFI_PASS    "test1234"
#define WIFI_RETRIES  5

static const char* TAG = "wifi_sta_test";

/* EventGroup bits */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

/* ── Event handler ────────────────────────────────────────────────────────
 * Verbatim Espressif reference pattern.
 * Logs reason code on every disconnect — key diagnostic output.
 */
static void event_handler(void*            arg,
                          esp_event_base_t event_base,
                          int32_t          event_id,
                          void*            event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {

        wifi_event_sta_disconnected_t* disconn =
            (wifi_event_sta_disconnected_t*)event_data;

        /* Log the disconnect reason code — key diagnostic output */
        ESP_LOGW(TAG, "disconnected — reason: %d (0x%x)",
                 disconn->reason, disconn->reason);

        if (s_retry_num < WIFI_RETRIES) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry %d/%d", s_retry_num, WIFI_RETRIES);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "all retries exhausted — giving up");
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "connected — IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ── wifi_init_sta ────────────────────────────────────────────────────────
 * Espressif reference WiFi station init with explicit PMF and authmode.
 */
static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        &event_handler, NULL, &instance_any_id));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASS,
            /* Explicit WPA2 authmode threshold */
           //.threshold.authmode = WIFI_AUTH_WPA2_PSK,
           .threshold.authmode = WIFI_AUTH_WPA2_WPA3_PSK,
            /* PMF capable but not required —
             * handles WPA2/WPA3 mixed-mode APs and
             * modern phone hotspots that enable PMF */
            .pmf_cfg = {
                .capable  = true,
                .required = false,
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta: started — SSID=[%s] retries=%d",
             WIFI_SSID, WIFI_RETRIES);

    /* Block until connected or all retries exhausted — no timeout */
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "RESULT: CONNECTED — WiFi stack and AP are working");
        ESP_LOGI(TAG, "If main project fails: compare wifi_connect() "
                 "against this file for differences");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "RESULT: FAILED — check AP admin page:");
        ESP_LOGE(TAG, "  MAC: check if 20:6e:f1:88:13:8c is blocked");
        ESP_LOGE(TAG, "  Security: WPA2/WPA3 mode, PMF policy");
        ESP_LOGE(TAG, "  Band: confirm 2.4GHz is enabled");
        ESP_LOGE(TAG, "  Password: verify character by character");
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

/* ── app_main ─────────────────────────────────────────────────────────────
 * Minimal boot — NVS init then WiFi test only.
 * No camera, no TFLite, no HTTP. Isolation is the point.
 */
void app_main(void)
{
    ESP_LOGI(TAG, "wifi_sta_test — boot");
    ESP_LOGI(TAG, "Target SSID: [%s]", WIFI_SSID);

    /* NVS init — required before esp_wifi_init() */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS erase required");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialised");

    wifi_init_sta();

    ESP_LOGI(TAG, "test complete — monitor output above shows result");
}
