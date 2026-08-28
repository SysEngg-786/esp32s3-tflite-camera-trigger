// File: main.cpp
// Path: esp32s3-tflite-camera-trigger/main/main.cpp
// Role: Orchestration only — owns the main task loop and wires the three
//       modules together in sequence. Contains no module logic of its own.
//
// Boot sequence:
//   1. wifi_connect()     — network up before inference loop starts
//   2. camera_init()      — OV3660 and DMA pipeline ready
//   3. inference_init()   — model loaded, PSRAM arena allocated, interpreter ready
//   4. Detection loop     — capture → infer → trigger if detection_valid
//
// Configuration: WiFi credentials and endpoint URL are set via
//   `idf.py menuconfig` → "ESP32S3 TFLite Camera Trigger Configuration".
//   Values live in sdkconfig (gitignored) — never in source files.
//
// MVP note: on any init failure the system logs and halts — no recovery loop.
//   The detection loop runs indefinitely — no sleep, no power management.
//   Post-MVP: light-sleep between frames, configurable poll interval.

#include "camera_init.h"
#include "inference_engine.h"
#include "web_trigger.h"

#include "esp_log.h"

// Credentials and endpoint from Kconfig — set via idf.py menuconfig.
// CONFIG_ defines are generated from Kconfig.projbuild into sdkconfig.
// sdkconfig is gitignored — credentials never enter the repository.
#define WIFI_SSID        CONFIG_WIFI_SSID
#define WIFI_PASSWORD    CONFIG_WIFI_PASSWORD
#define TRIGGER_ENDPOINT CONFIG_TRIGGER_ENDPOINT_URL

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
    // Credentials sourced from Kconfig — not from source code.
    ESP_LOGI(TAG, "step 1: wifi_connect");
    if (wifi_connect(WIFI_SSID, WIFI_PASSWORD) != ESP_OK) {
        ESP_LOGE(TAG, "wifi_connect failed — halting");
        return;
    }

    // ── Step 2: Camera ──────────────────────────────────────────────────────
    // OV3660 + DMA pipeline init. Must succeed before any capture attempt.
    ESP_LOGI(TAG, "step 2: camera_init");
    if (camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "camera_init failed — halting");
        return;
    }

    // ── Step 3: Inference engine ────────────────────────────────────────────
    // Load model from C array, allocate PSRAM arena, build TFLite interpreter.
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
            continue;
        }

        // 4b. Run inference on the captured frame.
        // frame->buf is raw JPEG bytes — inference_run() owns the full
        // decode pipeline: JPEG → grayscale → 96×96 → INT8 tensor → Invoke().
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
    }
}
