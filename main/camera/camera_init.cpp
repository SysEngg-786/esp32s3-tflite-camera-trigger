// File: camera_init.cpp
// Path: esp32s3-tflite-camera-trigger/main/camera/camera_init.cpp
// Role: Camera module implementation — OV2640 init and frame capture.
//       STUB: all functions return safe defaults. No hardware is touched.
//       Real implementation replaces the stub bodies in the next pass.
//
// MVP note: Xiao ESP32-S3 Sense pin map is hardcoded here — single board
//   target, no runtime board selection needed.

#include "camera_init.h"
#include "esp_log.h"

// Module log tag — all camera log lines are prefixed with this
static const char* TAG = "camera";

// ── camera_init ────────────────────────────────────────────────────────────
// STUB: will configure the esp_camera driver with the Xiao S3 Sense
//   OV2640 pin map, set JPEG output at VGA resolution, allocate two
//   PSRAM-backed frame buffers, and start the DMA pipeline.
esp_err_t camera_init(void)
{
    ESP_LOGI(TAG, "camera_init: stub — no hardware touched");
    return ESP_OK;
}

// ── camera_capture_frame ───────────────────────────────────────────────────
// STUB: will call esp_camera_fb_get() to grab a populated DMA frame buffer.
//   Returns NULL here so the caller can handle it without dereferencing.
camera_fb_t* camera_capture_frame(void)
{
    ESP_LOGI(TAG, "camera_capture_frame: stub — returning NULL");
    return nullptr;
}

// ── camera_return_frame ────────────────────────────────────────────────────
// STUB: will call esp_camera_fb_return() to release the DMA buffer
//   back to the pool for the next capture cycle.
void camera_return_frame(camera_fb_t* fb)
{
    // Guard against NULL in the stub — real implementation inherits this guard
    if (fb == nullptr) { return; }
    ESP_LOGI(TAG, "camera_return_frame: stub — buffer not returned to pool");
}
