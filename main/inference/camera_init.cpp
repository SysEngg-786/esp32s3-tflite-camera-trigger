// File: camera_init.cpp
// Path: esp32s3-tflite-camera-trigger/main/camera/camera_init.cpp
// Role: Camera module implementation — OV3660 init and frame capture
//       for the Seeed Xiao ESP32-S3 Sense board.
//
// Sensor:      OV3660 (auto-detected by esp_camera driver over I2C)
// Interface:   DVP parallel, 14 GPIO pins via ribbon FPC connector
// Pin map:     Seeed Xiao ESP32-S3 Sense fixed board wiring.
//              Source: Seeed Studio official wiki + camera repo.
//
// Current pipeline configuration — QQVGA + RGB565:
//   Pixel format: RGB565 — raw pixel data, deterministic size.
//                 160×120×2 = 38,400 bytes exact. No overflow possible.
//                 JPEG was replaced because driver buffer allocation
//                 underestimated compressed output size for real scenes,
//                 causing persistent FB-OVF regardless of quality setting.
//   Frame size:   QQVGA (160×120) — larger than 96×96 model input in both
//                 dimensions. Scale ratio 1.67x, adequate for nearest-neighbor
//                 resize. 4x smaller than QVGA.
//   Frame buffers: 2 in PSRAM — double buffering, DMA-backed.
//   Grab mode:    CAMERA_GRAB_LATEST — always newest frame discarding stale.
//
// Benchmark variables (post-MVP controlled experiments):
//   - pixel_format: RGB565 (current) vs JPEG
//   - frame_size:   QQVGA (current) vs QVGA vs VGA
//   - fb_location:  PSRAM (current) vs DRAM
//   See PARAMETER-CHANGE-LOG.md for full change history and rationale.

#include "camera_init.h"
#include "esp_log.h"
#include "esp_camera.h"

static const char* TAG = "camera";

// ── Xiao ESP32-S3 Sense pin map ────────────────────────────────────────────
// Fixed by board hardware. Source: Seeed Studio wiki.
// PWDN and RESET not wired on this board (-1 = not connected).
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    10
#define CAM_PIN_SIOD    40   // SCCB data
#define CAM_PIN_SIOC    39   // SCCB clock
#define CAM_PIN_D7      48
#define CAM_PIN_D6      11
#define CAM_PIN_D5      12
#define CAM_PIN_D4      14
#define CAM_PIN_D3      16
#define CAM_PIN_D2      18
#define CAM_PIN_D1      17
#define CAM_PIN_D0      15
#define CAM_PIN_VSYNC   38
#define CAM_PIN_HREF    47
#define CAM_PIN_PCLK    13

// ── camera_init ────────────────────────────────────────────────────────────
esp_err_t camera_init(void)
{
    ESP_LOGI(TAG, "camera_init: configuring OV3660 on Xiao ESP32-S3 Sense");

    camera_config_t config = {};

    // LEDC generates XCLK. Channel 0 / Timer 0 reserved for camera.
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;

    // Pin assignments — Xiao S3 Sense fixed wiring
    config.pin_pwdn     = CAM_PIN_PWDN;
    config.pin_reset    = CAM_PIN_RESET;
    config.pin_xclk     = CAM_PIN_XCLK;
    config.pin_sccb_sda = CAM_PIN_SIOD;
    config.pin_sccb_scl = CAM_PIN_SIOC;
    config.pin_d7       = CAM_PIN_D7;
    config.pin_d6       = CAM_PIN_D6;
    config.pin_d5       = CAM_PIN_D5;
    config.pin_d4       = CAM_PIN_D4;
    config.pin_d3       = CAM_PIN_D3;
    config.pin_d2       = CAM_PIN_D2;
    config.pin_d1       = CAM_PIN_D1;
    config.pin_d0       = CAM_PIN_D0;
    config.pin_vsync    = CAM_PIN_VSYNC;
    config.pin_href     = CAM_PIN_HREF;
    config.pin_pclk     = CAM_PIN_PCLK;

    // XCLK: 20MHz — stable for OV3660 on this board.
    // Set here in code; CONFIG_CAMERA_XCLK_FREQ_HZ in sdkconfig.defaults
    // is not a recognised Kconfig key and has no effect.
    config.xclk_freq_hz = 20000000;

    // Pixel format: RGB565 — deterministic 38,400 bytes per frame.
    // JPEG replaced due to driver DMA buffer allocation underestimation
    // causing FB-OVF regardless of quality setting. See change log.
    config.pixel_format = PIXFORMAT_RGB565;

    // Frame size: QQVGA (160×120).
    // Inference module converts directly RGB565→grayscale→96×96 in one pass.
    config.frame_size   = FRAMESIZE_QQVGA;

    // jpeg_quality not applicable for RGB565 raw format.

    // Two PSRAM-backed DMA frame buffers — double buffering.
    config.fb_count    = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;

    // Always return newest frame — discard stale frames from inference delay.
    config.grab_mode   = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x (%s)",
                 err, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "camera_init: OV3660 initialised — QQVGA RGB565, 2 PSRAM buffers");
    return ESP_OK;
}

// ── camera_capture_frame ───────────────────────────────────────────────────
camera_fb_t* camera_capture_frame(void)
{
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) {
        ESP_LOGW(TAG, "camera_capture_frame: esp_camera_fb_get returned NULL");
        return nullptr;
    }
    ESP_LOGD(TAG, "frame captured: %ux%u %zu bytes", fb->width, fb->height, fb->len);
    return fb;
}

// ── camera_return_frame ────────────────────────────────────────────────────
void camera_return_frame(camera_fb_t* fb)
{
    if (fb == nullptr) {
        ESP_LOGW(TAG, "camera_return_frame: called with NULL — ignored");
        return;
    }
    esp_camera_fb_return(fb);
}
