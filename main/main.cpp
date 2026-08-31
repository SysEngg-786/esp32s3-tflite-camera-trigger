// File: main.cpp
// Path: esp32s3-tflite-camera-trigger/main/main.cpp
// Role: Orchestration — boots all modules, runs detection task on CPU1.
//
// WiFi lifecycle — inference mode flag coordinates with web_trigger.cpp:
//   wifi_set_inference_mode(true)  → before esp_wifi_stop()
//     Tells handler: this is intentional, do not retry on disconnect.
//   wifi_set_inference_mode(false) → before esp_wifi_start() on detection
//     Tells handler: reconnect normally, handle IP event.
//
//   This eliminates the cycling problem:
//     Previous: esp_wifi_stop() → DISCONNECTED event → handler retries → infinite cycle
//     Fixed:    esp_wifi_stop() → DISCONNECTED event → handler sees inference_mode=true → exits
//
//   On detection path:
//     inference_mode=false → esp_wifi_start() → STA_START → handler calls esp_wifi_connect()
//     No duplicate esp_wifi_connect() call from main.cpp needed.
//
// Invoke() timing:
//   esp_timer_get_time() — µs hardware timer. Delta = accurate Invoke() duration.
//   Paper measurement 8.1.

#include "camera_init.h"
#include "inference_engine.h"
#include "web_trigger.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define WIFI_SSID        CONFIG_WIFI_SSID
#define WIFI_PASSWORD    CONFIG_WIFI_PASSWORD
#define TRIGGER_ENDPOINT CONFIG_TRIGGER_ENDPOINT_URL

#define WIFI_IP_WAIT_MAX_MS  8000

static const char* TAG = "main";

// ── Detection task — CPU1 ──────────────────────────────────────────────────
static void detection_task(void* arg)
{
    ESP_LOGI(TAG, "detection_task: running on CPU%d", xPortGetCoreID());

    // Set inference mode BEFORE stopping WiFi.
    // Suppresses handler auto-reconnect on the resulting DISCONNECTED event.
    ESP_LOGI(TAG, "detection_task: stopping WiFi for inference loop");
    wifi_set_inference_mode(true);
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    while (true) {

        // 1. Capture — WiFi stopped, camera DMA unaffected
        camera_fb_t* frame = camera_capture_frame();
        if (frame == nullptr) {
            ESP_LOGW(TAG, "frame capture failed — skipping");
            continue;
        }

        // 2. Inference — WiFi stopped, Invoke() uninterrupted
        int64_t t_before = esp_timer_get_time();
        inference_result_t result = {};
        esp_err_t infer_ret = inference_run(frame->buf, frame->len, &result);
        int64_t invoke_us = esp_timer_get_time() - t_before;

        ESP_LOGI(TAG, "inference: %lld us (%lld ms) — valid=%d confidence=%.2f",
                 invoke_us, invoke_us / 1000,
                 result.detection_valid, result.confidence);

        // 3. Return frame immediately
        camera_return_frame(frame);

        if (infer_ret != ESP_OK) {
            ESP_LOGW(TAG, "inference_run error — skipping");
            continue;
        }

        // 4. No detection — WiFi stays stopped, next frame immediately
        if (!result.detection_valid) {
            continue;
        }

        // ── Detection path ─────────────────────────────────────────────────
        ESP_LOGI(TAG, "detection: class=%d confidence=%.2f — reconnecting WiFi",
                 result.class_id, result.confidence);

        // 5. Clear inference mode BEFORE starting WiFi.
        //    Handler will now auto-connect on WIFI_EVENT_STA_START.
        //    No duplicate esp_wifi_connect() call needed here.
        wifi_set_inference_mode(false);
        esp_wifi_start();
        // Handler calls esp_wifi_connect() on STA_START automatically

        // 6. Event-driven IP wait
        int64_t ip_wait_start = esp_timer_get_time();
        while (!wifi_ready()) {
            int64_t elapsed_ms = (esp_timer_get_time() - ip_wait_start) / 1000;
            if (elapsed_ms >= WIFI_IP_WAIT_MAX_MS) {
                ESP_LOGW(TAG, "IP wait timeout after %lld ms", elapsed_ms);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        int64_t ip_wait_ms = (esp_timer_get_time() - ip_wait_start) / 1000;
        ESP_LOGI(TAG, "wifi ready after %lld ms — sending trigger", ip_wait_ms);

        // 7. Send trigger
        esp_err_t trig_ret = trigger_send(TRIGGER_ENDPOINT,
                                          result.class_id,
                                          result.confidence);
        if (trig_ret != ESP_OK) {
            ESP_LOGW(TAG, "trigger_send failed (ignored at MVP)");
        }

        // 8. Set inference mode BEFORE stopping WiFi again
        ESP_LOGI(TAG, "trigger complete — resuming inference loop");
        wifi_set_inference_mode(true);
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ── app_main ───────────────────────────────────────────────────────────────
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "esp32s3-tflite-camera-trigger — boot (CPU%d)", xPortGetCoreID());

    ESP_LOGI(TAG, "step 1: wifi_connect");
    if (wifi_connect(WIFI_SSID, WIFI_PASSWORD) != ESP_OK) {
        ESP_LOGW(TAG, "wifi_connect failed — continuing without trigger capability");
    }

    ESP_LOGI(TAG, "step 2: camera_init");
    if (camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "camera_init failed — halting");
        return;
    }

    ESP_LOGI(TAG, "step 3: inference_init");
    if (inference_init() != ESP_OK) {
        ESP_LOGE(TAG, "inference_init failed — halting");
        return;
    }

    ESP_LOGI(TAG, "step 4: creating detection task on CPU1");
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        detection_task,
        "detect_task",
        8192,
        nullptr,
        5,
        nullptr,
        1
    );

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed — halting");
        return;
    }

    ESP_LOGI(TAG, "all modules initialised — detection task running on CPU1");
}
