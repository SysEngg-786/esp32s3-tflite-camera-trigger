// File: main.cpp
// Path: esp32s3-tflite-camera-trigger/main/main.cpp
// Role: Orchestration only — owns the main task loop and wires the three
//       modules together in sequence. Contains no module logic of its own.
//
// Boot sequence (stub pass — no real work done yet):
//   1. wifi_connect()     — network up before inference loop starts
//   2. camera_init()      — OV2640 and DMA pipeline ready
//   3. inference_init()   — SPIFFS mounted, model loaded into PSRAM arena
//   4. Detection loop     — capture → infer → trigger if detection_valid
//
// MVP note: on any init failure the system logs and halts (no recovery loop).
//   The detection loop runs indefinitely — no sleep, no power management.
//   Post-MVP: light-sleep between frames, configurable poll interval.

#include "camera_init.h"
#include "inference_engine.h"
#include "web_trigger.h"

#include "esp_log.h"

// ── User configuration ─────────────────────────────────────────────────────
// MVP: credentials and endpoint supplied here as string literals.
// Post-MVP: move to Kconfig (menuconfig) so they are not in source control.
#define WIFI_SSID        "YOUR_SSID"
#define WIFI_PASSWORD    "YOUR_PASSWORD"
#define TRIGGER_ENDPOINT "http://your-server/detect"

// Module log tag
static const char* TAG = "main";

// ── app_main ───────────────────────────────────────────────────────────────
// ESP-IDF entry point — called by the scheduler after system init.
// Runs as the main task; must not return.
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "esp32s3-tflite-camera-trigger — boot");

    // ── Step 1: WiFi ────────────────────────────────────────────────────────
    // Network must be up before the detection loop can fire the trigger.
    ESP_LOGI(TAG, "step 1: wifi_connect");
    if (wifi_connect(WIFI_SSID, WIFI_PASSWORD) != ESP_OK) {
        ESP_LOGE(TAG, "wifi_connect failed — halting");
        return;   // MVP: halt; post-MVP: retry with backoff
    }

    // ── Step 2: Camera ──────────────────────────────────────────────────────
    // OV2640 + DMA pipeline init. Must succeed before any capture attempt.
    ESP_LOGI(TAG, "step 2: camera_init");
    if (camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "camera_init failed — halting");
        return;
    }

    // ── Step 3: Inference engine ────────────────────────────────────────────
    // Mount SPIFFS, load model into PSRAM arena, build TFLite interpreter.
    ESP_LOGI(TAG, "step 3: inference_init");
    if (inference_init() != ESP_OK) {
        ESP_LOGE(TAG, "inference_init failed — halting");
        return;
    }

    ESP_LOGI(TAG, "all modules initialised — entering detection loop");

    // ── Step 4: Detection loop ──────────────────────────────────────────────
    // Capture → infer → trigger. Runs indefinitely.
    while (true) {

        // 4a. Grab one frame from the DMA pool
        camera_fb_t* frame = camera_capture_frame();
        if (frame == nullptr) {
            ESP_LOGW(TAG, "frame capture failed — skipping cycle");
            continue;   // try again next cycle; do not halt on transient miss
        }

        // 4b. Run inference on the captured frame
        // MVP: frame->buf is assumed RGB888 here — format conversion
        //   from JPEG/YUV will be added inside inference_run() in the
        //   inference implementation pass.
        inference_result_t result = {};
        esp_err_t infer_ret = inference_run(frame->buf, frame->len, &result);

        // 4c. Return the frame buffer immediately after inference reads it —
        //   holding it longer starves the DMA pool on the next capture
        camera_return_frame(frame);

        if (infer_ret != ESP_OK) {
            ESP_LOGW(TAG, "inference_run error — skipping trigger");
            continue;
        }

        // 4d. Fire HTTP POST only when a confident detection is present
        if (result.detection_valid) {
            ESP_LOGI(TAG, "detection: class=%d confidence=%.2f — sending trigger",
                     result.class_id, result.confidence);

            esp_err_t trig_ret = trigger_send(TRIGGER_ENDPOINT,
                                              result.class_id,
                                              result.confidence);
            if (trig_ret != ESP_OK) {
                // MVP: log and continue — no retry
                ESP_LOGW(TAG, "trigger_send failed (ignored at MVP)");
            }
        }
        // No detection — silent continue; log volume would be excessive
    }
}
