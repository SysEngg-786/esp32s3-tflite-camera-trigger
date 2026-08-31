// File: web_trigger.h
// Path: esp32s3-tflite-camera-trigger/main/trigger/web_trigger.h
// Role: Public interface for the trigger module.
//
// Inference mode flag — wifi_set_inference_mode():
//   When inference_mode=true, the persistent WiFi event handler suppresses
//   all auto-reconnect attempts. This prevents the cycling problem where
//   esp_wifi_stop() triggers WIFI_EVENT_STA_DISCONNECTED, which the handler
//   retries — causing indefinite WiFi cycling during inference.
//   detection_task sets inference_mode=true before esp_wifi_stop() and
//   clears it just before esp_wifi_start() on detection events only.
//
// wifi_ready() — returns true when IP is assigned.
//   Persistent across all reconnect cycles.
//   Use in detection_task to confirm network is ready before trigger_send().

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialise WiFi stack and connect to AP. Call once at boot.
esp_err_t wifi_connect(const char* ssid, const char* password);

// Control inference mode — suppresses auto-reconnect when true.
// Call wifi_set_inference_mode(true) before esp_wifi_stop().
// Call wifi_set_inference_mode(false) before esp_wifi_start() on detection.
void wifi_set_inference_mode(bool inference_active);

// Returns true when WiFi connected AND IP assigned.
// Persistent across all reconnect cycles.
bool wifi_ready(void);

// Send HTTP POST trigger. Call only after wifi_ready() returns true.
esp_err_t trigger_send(const char* endpoint_url,
                       int         class_id,
                       float       confidence);

#ifdef __cplusplus
}
#endif
