// File: camera_init.cpp
// Path: esp32s3-tflite-camera-trigger/main/camera/camera_init.cpp
// Role: Camera module implementation — OV3660 init and frame capture
//       for the Seeed Xiao ESP32-S3 Sense board.
//
// Sensor:      OV3660 (auto-detected by esp_camera driver over I2C)
// Interface:   DVP parallel, 14 GPIO pins via ribbon FPC connector
// Pin map:     Seeed Xiao ESP32-S3 Sense fixed board wiring — not configurable
//              at runtime. Source: Seeed Studio official wiki + camera repo.
//
// Pipeline decisions (QVGA + JPEG — MVP choice):
//   Pixel format: JPEG  — sensor hardware-compresses each frame.
//                          DMA buffer stays small (~30-50 KB at QVGA).
//                          Decode to RGB888 is the inference module's concern.
//   Frame size:   QVGA (320×240) — minimum larger than the 224×224 model
//                          input in both dimensions. Scale ratio ~1.4x.
//                          4x smaller buffer than VGA with adequate content.
//   Frame buffers: 2 in PSRAM — double buffering allows capture of the next
//                          frame while inference processes the current one.
//   Grab mode:    CAMERA_GRAB_LATEST — always return the newest frame,
//                          discarding stale frames accumulated during inference.
//
// Post-MVP benchmark note: QVGA+JPEG is the MVP baseline. VGA+JPEG,
//   QVGA+RGB565, and QVGA+RGB888 are the three comparison configurations
//   for the planned benchmark pass. Frame size and pixel format are the
//   only two config lines that change between configurations.
//
// MVP note: camera_init() halts on failure — no retry, no fallback.
//   Production hardening (reconnect on cable disturbance, runtime
//   reconfiguration) is a named post-MVP item.

#include "camera_init.h"
#include "esp_log.h"
#include "esp_camera.h"

// Module log tag — all camera log lines carry this prefix
static const char* TAG = "camera";

// -- Xiao ESP32-S3 Sense pin map --------------------------------------------
// Fixed by board hardware — do not modify without a board schematic change.
// PWDN and RESET are not wired on this board (-1 = not connected).
// Source: Seeed Studio wiki + SeeedStudio-XIAO-ESP32S3-Sense-camera repo.
#define CAM_PIN_PWDN    -1   // Power down  — not connected on Xiao S3 Sense
#define CAM_PIN_RESET   -1   // Hard reset  — not connected on Xiao S3 Sense
#define CAM_PIN_XCLK    10   // Master clock to sensor
#define CAM_PIN_SIOD    40   // SCCB/I2C data  (sensor register access)
#define CAM_PIN_SIOC    39   // SCCB/I2C clock (sensor register access)
#define CAM_PIN_D7      48   // Pixel data bit 7 (MSB)
#define CAM_PIN_D6      11   // Pixel data bit 6
#define CAM_PIN_D5      12   // Pixel data bit 5
#define CAM_PIN_D4      14   // Pixel data bit 4
#define CAM_PIN_D3      16   // Pixel data bit 3
#define CAM_PIN_D2      18   // Pixel data bit 2
#define CAM_PIN_D1      17   // Pixel data bit 1
#define CAM_PIN_D0      15   // Pixel data bit 0 (LSB)
#define CAM_PIN_VSYNC   38   // Vertical sync   — marks frame boundaries
#define CAM_PIN_HREF    47   // Horizontal ref  — marks line boundaries
#define CAM_PIN_PCLK    13   // Pixel clock     — clocks each pixel byte

// -- camera_init ------------------------------------------------------------
// Configures the esp_camera driver with the Xiao S3 Sense pin map,
// sets JPEG output at QVGA resolution, allocates two PSRAM-backed DMA
// frame buffers, and starts the sensor pipeline.
//
// Call once at boot before any capture attempt.
// Returns ESP_OK on success; logs error and returns esp_err_t on failure.
// Caller (main.cpp) halts on non-OK return — MVP behaviour.
esp_err_t camera_init(void)
{
    ESP_LOGI(TAG, "camera_init: configuring OV3660 on Xiao ESP32-S3 Sense");

    // -- Populate camera_config_t -------------------------------------------
    camera_config_t config = {};

    // LEDC peripheral generates the XCLK signal for the sensor.
    // LEDC_CHANNEL_0 and LEDC_TIMER_0 are the standard camera channel pair;
    // ensure no other driver claims these before camera_init() is called.
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;

    // Pin assignments — Xiao S3 Sense fixed wiring (see defines above)
    config.pin_pwdn     = CAM_PIN_PWDN;
    config.pin_reset    = CAM_PIN_RESET;
    config.pin_xclk     = CAM_PIN_XCLK;
    config.pin_sscb_sda = CAM_PIN_SIOD;   // SCCB data  (I2C-compatible)
    config.pin_sscb_scl = CAM_PIN_SIOC;   // SCCB clock
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

    // XCLK frequency — 20 MHz is stable for OV3660 on this board.
    // Note: xclk_freq_hz is set here in code; the CONFIG_CAMERA_XCLK_FREQ_HZ
    // line in sdkconfig.defaults is not a recognised Kconfig key and has no
    // effect — this line is the authoritative XCLK setting.
    config.xclk_freq_hz = 20000000;

    // -- MVP pipeline configuration: QVGA + JPEG ---------------------------
    // Pixel format: JPEG — sensor compresses each frame in hardware.
    //   DMA buffer is small (~30-50 KB); decode is inference module concern.
    config.pixel_format = PIXFORMAT_JPEG;

    // Frame size: QVGA (320×240) — larger than 224×224 model input in both
    //   dimensions, 4x smaller buffer than VGA. MVP baseline for benchmarks.
    config.frame_size   = FRAMESIZE_QVGA;

    // JPEG quality: 10 (scale 0=best to 63=worst).
    //   Quality 10 gives good fidelity at QVGA with a small compressed size.
    //   Post-MVP benchmark: quality vs. detection accuracy is a variable to sweep.
    config.jpeg_quality = 10;

    // Frame buffer count: 2 in PSRAM — double buffering.
    //   Buffer 0 is held by inference while buffer 1 captures the next frame.
    //   Requires PSRAM; sdkconfig.defaults enables OPI PSRAM on Xiao S3 Sense.
    config.fb_count     = 2;

    // Frame buffer location: PSRAM — internal SRAM cannot hold two QVGA buffers.
    config.fb_location  = CAMERA_FB_IN_PSRAM;

    // Grab mode: CAMERA_GRAB_LATEST — always return the newest captured frame.
    //   At 1-3 FPS inference, multiple frames accumulate during each Invoke().
    //   GRAB_LATEST discards stale frames so each inference sees current reality.
    config.grab_mode    = CAMERA_GRAB_LATEST;

    // -- Initialise the driver ----------------------------------------------
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        // Log the specific error code — helps distinguish pin map errors
        // (ESP_ERR_CAMERA_NOT_DETECTED) from PSRAM errors (ESP_ERR_NO_MEM)
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x (%s)",
                 err, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "camera_init: OV3660 initialised — QVGA JPEG, 2 PSRAM buffers");
    return ESP_OK;
}

// -- camera_capture_frame ---------------------------------------------------
// Grabs one frame from the DMA pool. The driver fills the buffer from the
// most recently completed capture (GRAB_LATEST mode).
//
// Returns a pointer to the populated frame buffer, or NULL on failure.
// Caller must pass every non-NULL return to camera_return_frame() after
// the inference module finishes reading it — failure starves the DMA pool.
camera_fb_t* camera_capture_frame(void)
{
    // esp_camera_fb_get() blocks until a frame is available or times out.
    // In GRAB_LATEST mode it discards intermediate frames automatically.
    camera_fb_t* fb = esp_camera_fb_get();

    if (fb == nullptr) {
        // Transient failure — DMA pool temporarily empty or sensor not ready.
        // main.cpp handles NULL by skipping the cycle; no halt here.
        ESP_LOGW(TAG, "camera_capture_frame: esp_camera_fb_get returned NULL");
        return nullptr;
    }

    // Log frame dimensions and compressed size for the first frame captured
    // (LOGD so it does not flood the log at runtime; enable with log level DEBUG)
    ESP_LOGD(TAG, "frame captured: %ux%u %zu bytes",
             fb->width, fb->height, fb->len);

    return fb;
}

// -- camera_return_frame ----------------------------------------------------
// Returns a frame buffer to the DMA pool after the inference module has
// finished reading it. Must be called on every non-NULL return from
// camera_capture_frame() — this is the only mechanism that frees the buffer.
//
// With fb_count=2, holding both buffers simultaneously causes the next
// camera_capture_frame() call to block indefinitely — guarded with NULL
// check here but the real guard is the call discipline in main.cpp.
void camera_return_frame(camera_fb_t* fb)
{
    // Guard: never pass NULL to esp_camera_fb_return — undefined behaviour
    if (fb == nullptr) {
        ESP_LOGW(TAG, "camera_return_frame: called with NULL — ignored");
        return;
    }

    // Return the buffer to the DMA pool for reuse by the next capture cycle
    esp_camera_fb_return(fb);
}