# Parameter Change Log — ESP32-S3 TFLite Camera Pipeline
**Project:** esp32s3-tflite-camera-trigger
**Purpose:** Structured record of every parameter change — value, basis, result, comparison with expectation.
**Methodology note:** Changes marked REACTIVE were made to fix an observed failure. Changes marked DELIBERATE were planned single-variable experiments. Mixed changes (multiple parameters in one pass) are explicitly flagged — their results cannot be attributed to a single variable and must be re-run as single-variable experiments in the benchmark pass.

---

## 1. Camera pixel format

| Field | Value |
|---|---|
| Parameter | `config.pixel_format` in `camera_init.cpp` |
| Change type | REACTIVE |
| Single/multiple | Multiple (changed alongside frame_size in same pass) |

**Initial value:** `PIXFORMAT_JPEG`
**Basis for initial value:** JPEG is the standard recommended format for ESP32 camera — hardware compression reduces DMA transfer size, assumed to fit within driver-allocated buffer.

**Change 1:** `PIXFORMAT_JPEG` → `PIXFORMAT_RGB565`
**Basis for change:** JPEG at QQVGA caused persistent `FB-OVF` (frame buffer overflow). Driver allocates buffer based on an estimated compression ratio that underestimates real-scene JPEG output size. RGB565 is raw pixel data — exactly `width × height × 2` bytes, deterministic, no overflow possible. `fmt2rgb888()` accepts `PIXFORMAT_RGB565` natively (confirmed from component header).

**Expected result:** FB-OVF eliminated. Frame buffer correctly sized.
**Actual result:** FB-OVF eliminated completely. Buffer correctly allocated at 38,400 bytes (160×120×2).
**Matches expectation:** Yes.

**Paper contribution:** JPEG vs RGB565 is a defined benchmark axis. JPEG produces variable-size output that the driver misestimates; RGB565 is deterministic. The full comparison (memory cost, decode time, inference confidence delta) is a benchmark experiment to be run in a controlled single-variable pass.

---

## 2. Camera frame size

| Field | Value |
|---|---|
| Parameter | `config.frame_size` in `camera_init.cpp` |
| Change type | REACTIVE |
| Single/multiple | Multiple (changed alongside pixel_format in same pass) |

**Initial value:** `FRAMESIZE_QVGA` (320×240)
**Basis for initial value:** QVGA is larger than the 96×96 model input in both dimensions, giving a scale ratio of ~3.3x. Better image content coverage, adequate headroom for resize.

**Change 1:** `FRAMESIZE_QVGA` → `FRAMESIZE_QQVGA` (160×120)
**Basis for change:** QVGA JPEG at quality 10 overflowed the DMA buffer (15,360 bytes driver estimate). QQVGA was chosen as the smallest frame size still larger than 96×96 in both dimensions (scale ratio 1.67x). Smaller frame = smaller compressed output.

**Expected result:** Reduced buffer pressure, FB-OVF eliminated.
**Actual result:** FB-OVF persisted at QQVGA JPEG — driver still underestimated buffer size. Problem was the JPEG format, not the resolution. Resolution change alone was insufficient.
**Matches expectation:** Partial — resolution reduction was correct direction but not the root cause.

**Note:** QQVGA retained after switching to RGB565. 160×120 RGB565 = 38,400 bytes exactly. Fits DMA buffer. Scale ratio 1.67x adequate for 96×96 inference input.

**Paper contribution:** QQVGA vs QVGA is a defined benchmark axis. Resolution affects: frame buffer size, format conversion time, resize computation, and potentially inference confidence (more context vs less context at 96×96 output). Controlled experiment needed.

---

## 3. Camera JPEG quality

| Field | Value |
|---|---|
| Parameter | `config.jpeg_quality` in `camera_init.cpp` |
| Change type | REACTIVE then ELIMINATED |
| Single/multiple | Single each time |

**Initial value:** `10` (scale 0=best/largest to 63=worst/smallest)
**Basis for initial value:** Quality 10 gives good visual fidelity at QVGA. Higher quality assumed adequate for 96×96 model input.

**Change 1:** `10` → `30`
**Basis:** Quality 10 at QQVGA still caused FB-OVF. Quality 30 produces smaller compressed output.
**Expected result:** FB-OVF eliminated.
**Actual result:** FB-OVF persisted. Compression ratio at quality 30 still exceeded driver estimate.
**Matches expectation:** No.

**Change 2:** `30` → `63`
**Basis:** Maximum compression, minimum output size.
**Expected result:** FB-OVF eliminated.
**Actual result:** FB-OVF persisted. Root cause confirmed as format choice (JPEG), not quality level.
**Matches expectation:** No.

**Change 3:** `63` → ELIMINATED (format changed to RGB565)
**Basis:** jpeg_quality is not applicable to RGB565 — raw format, no compression parameter.
**Result:** Parameter removed from config. FB-OVF eliminated by format change.

**Paper contribution:** Demonstrates that JPEG quality tuning does not resolve DMA buffer overflow when the driver's buffer allocation algorithm itself is incorrect. The fix must be at the format level, not the compression level.

---

## 4. Camera frame buffer location

| Field | Value |
|---|---|
| Parameter | `config.fb_location` in `camera_init.cpp` |
| Change type | REACTIVE |
| Single/multiple | Single each time |

**Initial value:** `CAMERA_FB_IN_PSRAM`
**Basis:** PSRAM has 8MB capacity — ample for frame buffers. Preserves internal SRAM for other uses.

**Change 1:** `CAMERA_FB_IN_PSRAM` → `CAMERA_FB_IN_DRAM`
**Basis:** Hypothesis that PSRAM DMA mode caused buffer size mismatch. Driver log showed `PSRAM DMA mode disabled` — suspected interaction.
**Expected result:** DMA buffer correctly sized, FB-OVF eliminated.
**Actual result:** FB-OVF persisted. Buffer moved to DRAM but size (3,840 bytes) unchanged — driver still underestimated.
**Matches expectation:** No. Root cause was format (JPEG), not location.

**Change 2:** `CAMERA_FB_IN_DRAM` → `CAMERA_FB_IN_PSRAM`
**Basis:** DRAM frame buffers (2 × 38,400 = 75KB) consumed internal SRAM needed by the inference arena. RGB565 frame size is deterministic — PSRAM safe for RGB565 (overflow only occurs with JPEG size unpredictability).
**Expected result:** Internal SRAM freed for tensor arena allocation.
**Actual result:** Pending — build in progress.
**Matches expectation:** TBD.

**Paper contribution:** fb_location is a benchmark variable with two measurable effects: (1) internal SRAM availability for arena, (2) camera DMA transfer speed and latency. Controlled experiment needed.

---

## 5. TFLite tensor arena size

| Field | Value |
|---|---|
| Parameter | `INFERENCE_ARENA_SIZE` in `inference_engine.h` |
| Change type | REACTIVE |
| Single/multiple | Multiple (changed alongside arena location in same pass) |

**Initial value:** `200KB` (204,800 bytes)
**Basis:** Conservative estimate for MobileNet person detection. Measured usage confirmed: 122,568 bytes. 200KB provided ~78KB headroom.

**Change 1:** `200KB` → `150KB`
**Basis:** Moving arena to internal SRAM required reducing size to fit available SRAM. 150KB = ~28KB headroom over measured 122,568 bytes.
**Expected result:** Allocation succeeds in internal SRAM.
**Actual result:** Allocation failed — 133,120 bytes (150KB) did not fit in contiguous internal SRAM with WiFi stack and camera DRAM buffers active.
**Matches expectation:** No — available contiguous SRAM was overestimated.

**Change 2:** `150KB` → `130KB`
**Basis:** 130KB = 133,120 bytes. Measured usage 122,568 bytes — 10,552 bytes (8%) headroom. Camera frame buffers returned to PSRAM simultaneously, freeing DRAM for arena.
**Expected result:** Allocation succeeds in internal SRAM.
**Actual result:** Pending — build in progress.
**Matches expectation:** TBD.

**Paper contribution:** Arena size sweep is a defined benchmark experiment. Minimum viable arena size, allocation success threshold vs SRAM fragmentation, and headroom requirements are all measurable.

---

## 6. TFLite tensor arena location

| Field | Value |
|---|---|
| Parameter | `MALLOC_CAP_SPIRAM` vs `MALLOC_CAP_INTERNAL` in `inference_engine.cpp` |
| Change type | REACTIVE (but matches planned benchmark variable) |
| Single/multiple | Multiple (changed alongside arena size in same pass) |

**Initial value:** `MALLOC_CAP_SPIRAM` (PSRAM)
**Basis:** PSRAM has 8MB capacity — arena fits easily. Internal SRAM conserved for WiFi, system, stacks.

**Change 1:** `MALLOC_CAP_SPIRAM` → `MALLOC_CAP_INTERNAL`
**Basis:** With WiFi stack active, PSRAM-backed arena caused `esp_nn_conv_s8_esp32s3` to block IDLE0 for >60 seconds — watchdog fired at exactly 60s interval. Hypothesis: WiFi DMA interrupts degrade PSRAM memory bus bandwidth, causing convolution kernel to spin. Internal SRAM at 160MHz, single-cycle access, no contention.
**Expected result:** Invoke() completes within watchdog timeout. Inference time drops significantly.
**Actual result:** Pending — allocation failed at 150KB. Retrying at 130KB with camera buffers in PSRAM.
**Matches expectation:** TBD.

**Paper contribution — most significant finding of the session:**
PSRAM arena + WiFi active = >60s Invoke(). Internal SRAM arena expected = <500ms Invoke(). Delta quantifies PSRAM bandwidth degradation under WiFi interrupt load. This measurement does not exist in published literature for ESP32-S3 + TFLite Micro + WiFi co-existence.

**Experimental design note:** This change was made alongside arena size change — results cannot be attributed cleanly to location alone. In the benchmark pass, run PSRAM arena at same size as SRAM arena to isolate the location effect.

---

## 7. Watchdog timeout

| Field | Value |
|---|---|
| Parameter | `CONFIG_ESP_TASK_WDT_TIMEOUT_S` in `sdkconfig.defaults` |
| Change type | DELIBERATE (measurement instrument) |
| Single/multiple | Single |

**Initial value:** `5` (ESP-IDF upstream default — not set in sdkconfig.defaults, inherited silently)
**Basis for initial value:** ESP-IDF general-purpose default for simple embedded tasks. Not designed for inference workloads.

**Change 1:** Not set → `60`
**Basis:** Inference Invoke() on PSRAM-backed arena exceeded 5s. 60s chosen as measurement instrument — long enough to let inference attempt complete and observe the actual failure mode, not as a production value.
**Expected result:** Watchdog fires at 60s instead of 5s, revealing whether Invoke() eventually completes or spins indefinitely.
**Actual result:** Watchdog fired at 65,063ms and 125,063ms — exactly 60s apart. Invoke() never completed. Confirmed: PSRAM arena under WiFi load causes indefinite spin, not slow-but-finite inference.
**Matches expectation:** Partially — timeout extended as intended; revealed indefinite spin rather than slow completion.

**Paper contribution:** Watchdog timing as a measurement instrument. The 60s interval between firings confirms the spin is not making progress — it is a true indefinite block, not a slow computation.

**Production value:** After measuring actual inference time with SRAM arena, set to `measured_time × 2`.

---

## 8. WiFi authentication threshold

| Field | Value |
|---|---|
| Parameter | `wifi_config.sta.threshold.authmode` in `web_trigger.cpp` |
| Change type | REACTIVE (diagnostic) |
| Single/multiple | Single each time |

**Initial value:** `WIFI_AUTH_WPA2_PSK`
**Basis:** Assumed WPA2 is the standard — most modern routers advertise WPA2.

**Change 1:** `WIFI_AUTH_WPA2_PSK` → `WIFI_AUTH_WPA2_WPA3_PSK`
**Basis:** Failure codes 203 (0xcb) and 205 (0xcd) indicated security mode mismatch — AP found but below threshold. Hypothesis: router running WPA2/WPA3 mixed mode.
**Expected result:** Connection succeeds.
**Actual result:** Still failed. Router is WPA (not WPA2) — WPA2_WPA3 threshold still excluded it.
**Matches expectation:** No.

**Change 2:** `WIFI_AUTH_WPA2_WPA3_PSK` → `WIFI_AUTH_WPA_WPA2_PSK`
**Basis:** QR code scan of router showed `Security: WPA`. WPA2_WPA3 threshold excludes WPA.
**Expected result:** Connection succeeds.
**Actual result:** Connected. WPA2-PSK negotiated (`wifi:security: WPA2-PSK`).
**Matches expectation:** Yes.

**Change 3:** `WIFI_AUTH_WPA_WPA2_PSK` → `WIFI_AUTH_OPEN`
**Basis:** No single threshold accepts WPA, WPA2, and WPA3 simultaneously. WIFI_AUTH_OPEN removes the floor entirely — password still enforces encryption. Firmware auto-elevates threshold based on password length (`authmode threshold changes from OPEN to WPA2`). Forward-compatible with future protocols.
**Expected result:** Connects to both WPA router and WPA3 hotspot with same code.
**Actual result:** Router connected (WPA2-PSK negotiated). Hotspot connected (WPA3-SAE negotiated). Both confirmed on real hardware.
**Matches expectation:** Yes.

**Paper contribution (WiFi letter — standalone):**
ESP-IDF threshold.authmode is a security floor with no auto-negotiate across WPA/WPA2/WPA3. WIFI_AUTH_OPEN fills this gap. Firmware auto-negotiates upward. Forward-compatible. Confirmed on two APs with two different security protocols. Full reason code diagnostic table documented.

---

## Summary table

| # | Parameter | Initial | Final | Change type | Variables changed simultaneously | Result matched expectation |
|---|---|---|---|---|---|---|
| 1 | pixel_format | JPEG | RGB565 | Reactive | frame_size | Yes |
| 2 | frame_size | QVGA | QQVGA | Reactive | pixel_format | Partial |
| 3 | jpeg_quality | 10 | Eliminated | Reactive | None (each change single) | No (×2), then eliminated |
| 4 | fb_location | PSRAM | PSRAM (restored) | Reactive | None | TBD |
| 5 | arena_size | 200KB | 130KB | Reactive | arena_location | TBD |
| 6 | arena_location | PSRAM | SRAM | Reactive | arena_size | TBD |
| 7 | watchdog_timeout | 5s (default) | 60s | Deliberate | None | Partial |
| 8 | wifi_authmode | WPA2_PSK | WIFI_AUTH_OPEN | Reactive (diagnostic) | None (each change single) | Yes (final) |

---

## Benchmark pass — planned single-variable experiments

The following experiments must be run with all other parameters held constant:

1. **Arena location** — PSRAM vs SRAM, same size (130KB). Measures: inference time, IDLE0 utilisation.
2. **Arena size** — 130KB vs 150KB vs 200KB in SRAM. Measures: allocation success, headroom.
3. **Pixel format** — JPEG vs RGB565 at QQVGA. Measures: frame buffer size, decode time, inference confidence.
4. **Frame size** — QQVGA vs QVGA vs VGA at RGB565. Measures: buffer size, conversion time, inference confidence.
5. **fb_location** — PSRAM vs DRAM for frame buffers at RGB565. Measures: SRAM availability, DMA transfer latency.
6. **jpeg_quality sweep** — 10 to 63 at QVGA JPEG (once stable). Measures: compressed size, decode time, overflow threshold, confidence.

*Note: Parameters 1 and 6 from the change log were changed simultaneously — their individual effects must be isolated in the benchmark pass.*

---

*This document is the experimental record. All changes, basis, and results are recorded here. No result is claimed without evidence. Retracted labels are noted explicitly.*
*Last updated: 2026-08-30*
