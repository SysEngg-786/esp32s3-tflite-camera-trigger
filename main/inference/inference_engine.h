// File: inference_engine.h
// Path: esp32s3-tflite-camera-trigger/main/inference/inference_engine.h
// Role: Public interface for the inference module.
//       Owns: model load, TFLite Micro interpreter, frame preprocessing,
//             inference run, result population.
//
// MVP model: Person detection — MobileNet INT8, 96×96 grayscale input.
//
// Preprocessing pipeline — direct RGB565 → grayscale → 96×96 (no intermediate buffer):
//   For each 96×96 output pixel:
//     1. Nearest-neighbor map to QQVGA source pixel (160×120)
//     2. Read RGB565 pixel (2 bytes) directly from camera frame buffer
//     3. Extract R5/G6/B5, scale to R8/G8/B8
//     4. Convert to grayscale: (77R + 150G + 29B) >> 8  [ITU-R BT.601]
//     5. Quantize to INT8: gray - zero_point
//   No intermediate RGB888 buffer — eliminates 57,600 byte allocation.
//   No fmt2rgb888() call — eliminates bulk PSRAM write from CPU1.
//   Minimises cross-core cache coherency ISRs from WiFi/PSRAM interaction.
//
// Memory layout:
//   Tensor arena:  130 KB  internal SRAM  (MALLOC_CAP_INTERNAL)
//   Camera frames:  75 KB  PSRAM          (2 × 38,400 bytes, owned by camera module)
//   No RGB buffer — eliminated by direct RGB565→grayscale conversion.
//
// Core affinity:
//   inference_init() called from app_main on CPU0.
//   inference_run() called from detect_task pinned to CPU1.
//   Arena in internal SRAM — no cross-core cache pressure during Invoke().

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Model input dimensions
#define INFERENCE_INPUT_WIDTH     96
#define INFERENCE_INPUT_HEIGHT    96
#define INFERENCE_INPUT_CHANNELS  1    // grayscale

// Tensor arena — internal SRAM.
// Measured usage: 122,580 bytes. 130KB = 133,120 bytes (~10KB headroom).
#define INFERENCE_ARENA_SIZE      (130 * 1024)

// Person detection output indices
#define INFERENCE_SCORE_NO_PERSON  0
#define INFERENCE_SCORE_PERSON     1

// Confidence threshold
#define INFERENCE_CONFIDENCE_THRESHOLD  0.6f

// Inference result
typedef struct {
    int   class_id;
    float confidence;
    bool  detection_valid;
} inference_result_t;

// Load model, allocate arena, build interpreter. Call once at boot.
esp_err_t inference_init(void);

// Direct pipeline: RGB565 frame → grayscale resize → INT8 tensor → Invoke() → result.
// frame_data: raw RGB565 from camera_capture_frame()->buf
// frame_len:  bytes (expected 38,400 for QQVGA RGB565)
esp_err_t inference_run(const uint8_t*      frame_data,
                        size_t              frame_len,
                        inference_result_t* result);

#ifdef __cplusplus
}
#endif
