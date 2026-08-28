// File: inference_engine.h
// Path: esp32s3-tflite-camera-trigger/main/inference/inference_engine.h
// Role: Public interface for the inference module.
//       Owns: model load from C array, TFLite Micro interpreter lifecycle,
//             frame preprocessing (JPEG → model input format), inference run.
//       Does not own: frame capture (camera module) or HTTP dispatch (trigger module).
//
// MVP model: Person detection — MobileNet INT8, 96×96 grayscale input.
//   Sourced from espressif/esp-tflite-micro managed component,
//   examples/person_detection/main/person_detect_model_data.cc
//   Copied to main/model/ for source tracking.
//
// Design decision — inference module owns the full preprocessing chain:
//   Preprocessing is model-specific. Person detection needs grayscale 96×96;
//   COCO SSD needs RGB 224×224. The camera module passes raw JPEG bytes and
//   does not need to know what the model expects. When the model changes in
//   the benchmark pass, only this module changes — nothing in camera or main.
//
// MVP note: single model, single interpreter, no hot-swap.
//   One result per call — no temporal smoothing across frames.

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Model input dimensions — person detection model ───────────────────────
// These constants change when the model changes in the benchmark pass.
// All preprocessing targets these dimensions — nothing in the caller changes.
#define INFERENCE_INPUT_WIDTH     96    // model input width  (pixels)
#define INFERENCE_INPUT_HEIGHT    96    // model input height (pixels)
#define INFERENCE_INPUT_CHANNELS  1     // grayscale — 1 channel

// ── PSRAM arena size ──────────────────────────────────────────────────────
// Person detection model requires ~150KB. 200KB provides headroom.
// Training document data point: arena size is one of the four memory
// contributors tracked in the flash/PSRAM build-up table.
#define INFERENCE_ARENA_SIZE      (200 * 1024)

// ── Person detection output indices ──────────────────────────────────────
#define INFERENCE_SCORE_NO_PERSON  0   // output tensor index — no person class
#define INFERENCE_SCORE_PERSON     1   // output tensor index — person class

// ── Confidence threshold ──────────────────────────────────────────────────
// Scores are dequantized to [0.0, 1.0] float before comparison.
// 0.6 = 60% confidence required for detection_valid = true.
// Tunable without recompile — adjust here and rebuild.
#define INFERENCE_CONFIDENCE_THRESHOLD  0.6f

// ── Inference result ──────────────────────────────────────────────────────
// Populated by inference_run() on each call.
typedef struct {
    int   class_id;         // detected class index (INFERENCE_SCORE_PERSON = 1)
    float confidence;       // dequantized confidence in range [0.0, 1.0]
    bool  detection_valid;  // true only when confidence >= INFERENCE_CONFIDENCE_THRESHOLD
} inference_result_t;

// Load model from C array, allocate PSRAM arena, build TFLite Micro interpreter.
// Must be called once at boot. Returns ESP_OK on success.
// Caller halts on failure (MVP behaviour — no recovery).
esp_err_t inference_init(void);

// Full per-frame pipeline:
//   JPEG decode → grayscale → resize to 96×96 → INT8 tensor → Invoke() → result.
// jpeg_data: raw JPEG buffer from camera_capture_frame()->buf
// jpeg_len:  buffer length in bytes (camera_capture_frame()->len)
// result:    populated on ESP_OK return; detection_valid indicates threshold cleared.
esp_err_t inference_run(const uint8_t*      jpeg_data,
                        size_t              jpeg_len,
                        inference_result_t* result);

#ifdef __cplusplus
}
#endif
