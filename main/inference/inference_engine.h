// File: inference_engine.h
// Path: esp32s3-tflite-camera-trigger/main/inference/inference_engine.h
// Role: Public interface for the inference module.
//       Owns: SPIFFS mount, model load into PSRAM arena, TFLite Micro
//             interpreter lifecycle, frame preprocessing, inference run.
//       Does not own: frame capture (camera module) or HTTP dispatch (trigger module).
//
// Model: MobileNetV2 SSD COCO INT8, 224×224 RGB input.
// Framework: TFLite Micro (espressif/esp-tflite-micro managed component).
//
// MVP note: single model, single interpreter instance, no hot-swap,
//   no model versioning. One result per inference call — no temporal
//   smoothing across frames (post-MVP refinement).

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// SPIFFS mount point — model partition defined in partitions.csv
#define INFERENCE_SPIFFS_MOUNT_POINT   "/spiffs"

// Full path to the quantized model binary on the SPIFFS partition.
// The model file is flashed separately via: idf.py spiffsgen + flash
#define INFERENCE_MODEL_PATH           "/spiffs/model.tflite"

// Input tensor dimensions for MobileNetV2 SSD COCO INT8
#define INFERENCE_INPUT_WIDTH          224
#define INFERENCE_INPUT_HEIGHT         224
#define INFERENCE_INPUT_CHANNELS       3     // RGB

// Detection confidence threshold — results below this are discarded.
// MVP default: 0.6 (60%). Tunable without code change in post-MVP config.
#define INFERENCE_CONFIDENCE_THRESHOLD 0.6f

// Inference result — populated by inference_run() on each call.
typedef struct {
    int   class_id;         // COCO class index of the top detection
    float confidence;       // Detection confidence in range [0.0, 1.0]
    bool  detection_valid;  // True only when confidence >= threshold
} inference_result_t;

// Mount SPIFFS, load the model from INFERENCE_MODEL_PATH into the PSRAM
// inference arena, and initialise the TFLite Micro interpreter.
// Must be called once at boot after PSRAM is confirmed available.
// Returns ESP_OK on success; caller should halt on failure (MVP: no recovery).
esp_err_t inference_init(void);

// Preprocess one RGB frame to 224×224 INT8 tensor, run MobileNetV2
// inference, and populate result. The frame data must be RGB888 format —
// format conversion from JPEG/YUV is the caller's responsibility (camera module).
// Returns ESP_OK if inference completed; result->detection_valid indicates
// whether the top result cleared the confidence threshold.
esp_err_t inference_run(const uint8_t* rgb_data,
                        size_t         data_len,
                        inference_result_t* result);

#ifdef __cplusplus
}
#endif
