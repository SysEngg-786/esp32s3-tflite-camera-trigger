// File: web_trigger.h
// Path: esp32s3-tflite-camera-trigger/main/trigger/web_trigger.h
// Role: Public interface for the trigger module.
//       Owns: WiFi station init, HTTP POST dispatch on detection event.
//       Does not own: detection logic (inference module concern).
//
// MVP note: plain HTTP only — no TLS, no certificate validation,
//   no retry logic, no exponential backoff. One POST per detection event.
//   These are named post-MVP hardening items, not omissions.
//
//   WiFi credentials are passed in at call time — not hardcoded here.
//   Caller sources them from sdkconfig defines (set via menuconfig).
//   Production credential management (NVS provisioning, BLE setup) is
//   a post-MVP concern.

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the WiFi stack in station mode and connect to the AP.
// Blocks until connected or returns ESP_FAIL on timeout.
// Must be called once at boot before trigger_send().
// MVP note: single connection attempt, no reconnect loop.
esp_err_t wifi_connect(const char* ssid, const char* password);

// Send an HTTP POST to endpoint_url carrying the detection class and
// confidence as a JSON payload. Fire-and-forget — MVP does not wait
// for or validate the server response beyond the TCP ACK.
// Returns ESP_OK if the request was dispatched without transport error.
esp_err_t trigger_send(const char* endpoint_url,
                       int         class_id,
                       float       confidence);

#ifdef __cplusplus
}
#endif
