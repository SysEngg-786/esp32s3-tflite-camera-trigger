// File: inference_engine.cpp
// Path: esp32s3-tflite-camera-trigger/main/inference/inference_engine.cpp
// Role: Inference module implementation — model load and TFLite Micro runner.
//       STUB: all functions return safe defaults. No model is loaded,
//             no SPIFFS is mounted, no inference is run.
//       Real implementation replaces stub bodies in the inference pass.
//
// Real implementation will:
//   inference_init() — esp_vfs_spiffs_register(), fopen model, allocate
//                      PSRAM arena, build TFLite Micro interpreter.
//   inference_run()  — resize + normalise frame to 224×224 INT8,
//                      copy into input tensor, Invoke(), read output tensor,
//                      apply confidence threshold, populate result struct.

#include "inference_engine.h"
#include "esp_log.h"

// Module log tag
static const char* TAG = "inference";

// ── inference_init ─────────────────────────────────────────────────────────
// STUB: will mount SPIFFS at INFERENCE_SPIFFS_MOUNT_POINT, read the model
//   binary from INFERENCE_MODEL_PATH into a PSRAM-backed buffer, and
//   initialise the TFLite Micro interpreter with an arena sized for
//   MobileNetV2 SSD COCO INT8 (~2–3 MB in PSRAM).
esp_err_t inference_init(void)
{
    ESP_LOGI(TAG, "inference_init: stub — no model loaded, no SPIFFS mounted");
    return ESP_OK;
}

// ── inference_run ──────────────────────────────────────────────────────────
// STUB: will preprocess rgb_data to the 224×224 INT8 input tensor,
//   call interpreter->Invoke(), extract the top detection from the
//   output tensor, and populate result.
esp_err_t inference_run(const uint8_t*      rgb_data,
                        size_t              data_len,
                        inference_result_t* result)
{
    // Silence unused-parameter warnings in the stub build
    (void)rgb_data;
    (void)data_len;

    // Safe default — no detection, no trigger fired
    result->class_id        = -1;
    result->confidence      = 0.0f;
    result->detection_valid = false;

    ESP_LOGI(TAG, "inference_run: stub — detection_valid=false");
    return ESP_OK;
}
