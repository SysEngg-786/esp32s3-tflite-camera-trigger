// File: camera_init.h
// Path: esp32s3-tflite-camera-trigger/main/camera/camera_init.h
// Role: Public interface for the camera module.
//       Owns: OV2640 init, frame capture, frame buffer return.
//       Does not own: frame format conversion (inference module concern).
//
// MVP note: single camera config only — no runtime reconfiguration,
//   no resolution switching, no exposure control. Fixed pipeline.

#pragma once

#include "esp_err.h"
#include "esp_camera.h"   // camera_fb_t — from espressif/esp32-camera managed component

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the OV2640 camera with pin map and frame buffer config
// for the Xiao ESP32-S3 Sense board.
// Must be called once at boot before any capture attempt.
// Returns ESP_OK on success; halts on failure (MVP: no recovery path).
esp_err_t camera_init(void);

// Capture one frame from the OV2640 into a PSRAM-backed DMA buffer.
// Returns a pointer to the populated frame buffer, or NULL on failure.
// Caller must return the buffer via camera_return_frame() after use —
// failure to do so starves the DMA pool.
camera_fb_t* camera_capture_frame(void);

// Return a frame buffer to the DMA pool after the inference module
// has finished reading it. Must be called on every non-NULL return
// from camera_capture_frame().
void camera_return_frame(camera_fb_t* fb);

#ifdef __cplusplus
}
#endif
