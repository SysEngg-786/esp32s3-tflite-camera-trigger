// File: inference_engine.cpp
// Path: esp32s3-tflite-camera-trigger/main/inference/inference_engine.cpp
// Role: Inference module implementation — TFLite Micro pipeline for person detection.
//
// Pipeline (per frame):
//   JPEG (320×240) → RGB888 decode → grayscale + resize to 96×96 → INT8 tensor
//   → TFLite Micro Invoke() → dequantize output → inference_result_t
//
// Model: person_detect (MobileNet INT8, 96×96 grayscale, 2-class output)
//   Source: espressif/esp-tflite-micro examples/person_detection/
//   Array:  g_person_detect_model_data / g_person_detect_model_data_len
//   Ops required: DepthwiseConv2D, Conv2D, AveragePool2D, Reshape, Softmax (5 total)
//
// Memory (training document data point — PSRAM consumers in this module):
//   Tensor arena:        200 KB  (INFERENCE_ARENA_SIZE)
//   RGB888 decode buf:  ~225 KB  (320 × 240 × 3 bytes, QVGA camera output)
//   Total this module:  ~425 KB PSRAM
//
// Op resolver: MicroMutableOpResolver<5> — specific ops only, not AllOpsResolver.
//   Rationale: AllOpsResolver adds ~700KB flash. Specific resolver adds only
//   the ops this model uses. Community note: list the 5 ops; do not guess —
//   a missing op produces AllocateTensors() failure with no clear error message.
//
// MVP note: single model, single interpreter instance, no runtime swap.
//   Benchmark pass: model swap requires updating inference_engine.h constants
//   and the op resolver list — nothing in camera module or main.cpp changes.

#include "inference_engine.h"
#include "person_detect_model_data.h"   // g_person_detect_model_data array

// TFLite Micro core headers
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

// ESP-IDF headers
#include "esp_log.h"
#include "esp_heap_caps.h"        // heap_caps_malloc — PSRAM allocation
#include "img_converters.h"       // fmt2rgb888 — JPEG decode (esp32-camera component)

// Module log tag
static const char* TAG = "inference";

// ── Module-level state ────────────────────────────────────────────────────
// All objects are static — lifetime is the full device run.
// TFLite Micro objects must not be destroyed while inference is running.

// Verified TFLite model pointer — set once in inference_init()
static const tflite::Model* s_model = nullptr;

// Op resolver — 5 ops used by person detection model (MobileNet INT8).
// Template parameter must equal the number of AddX() calls below.
// Community note: derive this list from the model using Netron or
// `tflite::PrintModelInfo()` — do not guess, a missing op is silent until
// AllocateTensors() is called.
static tflite::MicroMutableOpResolver<5> s_resolver;

// Interpreter — owns the tensor arena and drives Invoke()
static tflite::MicroInterpreter* s_interpreter = nullptr;

// Input tensor pointer — cached after AllocateTensors() for fast per-frame access
static TfLiteTensor* s_input_tensor = nullptr;

// PSRAM tensor arena — allocated once, held for device lifetime
static uint8_t* s_tensor_arena = nullptr;

// PSRAM RGB888 decode buffer — allocated once, reused every inference_run() call.
// Size: QVGA 320×240×3 = 230,400 bytes (~225 KB).
// Training document data point: this is the second-largest PSRAM consumer
// in this module after the tensor arena.
static uint8_t* s_rgb_buf = nullptr;

// Camera capture dimensions — must match sdkconfig / camera_init FRAMESIZE_QVGA
static constexpr int CAM_WIDTH  = 320;
static constexpr int CAM_HEIGHT = 240;

// ── inference_init ─────────────────────────────────────────────────────────
// Loads the model from the C array, registers ops, allocates PSRAM arena,
// builds the TFLite Micro interpreter, and caches the input tensor pointer.
// Must be called once at boot after PSRAM is confirmed available.
esp_err_t inference_init(void)
{
    ESP_LOGI(TAG, "inference_init: loading person detection model from C array");

    // ── Step 1: allocate PSRAM tensor arena ───────────────────────────────
    // heap_caps_malloc with MALLOC_CAP_SPIRAM forces allocation into OPI PSRAM.
    // Internal SRAM cannot hold this allocation — the Xiao S3 has ~512KB SRAM.
    s_tensor_arena = static_cast<uint8_t*>(
        heap_caps_malloc(INFERENCE_ARENA_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    if (s_tensor_arena == nullptr) {
        ESP_LOGE(TAG, "inference_init: failed to allocate %d byte tensor arena in PSRAM",
                 INFERENCE_ARENA_SIZE);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "tensor arena: %d KB allocated in PSRAM", INFERENCE_ARENA_SIZE / 1024);

    // ── Step 2: allocate PSRAM RGB888 decode buffer ───────────────────────
    // fmt2rgb888() decodes JPEG into this buffer. Size = width × height × 3.
    const size_t rgb_buf_size = CAM_WIDTH * CAM_HEIGHT * 3;
    s_rgb_buf = static_cast<uint8_t*>(
        heap_caps_malloc(rgb_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    if (s_rgb_buf == nullptr) {
        ESP_LOGE(TAG, "inference_init: failed to allocate %u byte RGB decode buffer in PSRAM",
                 rgb_buf_size);
        heap_caps_free(s_tensor_arena);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "RGB decode buffer: %u KB allocated in PSRAM", rgb_buf_size / 1024);

    // ── Step 3: load and verify model ────────────────────────────────────
    // tflite::GetModel() maps the flatbuffer in flash — zero copy, zero parse time.
    s_model = tflite::GetModel(g_person_detect_model_data);
    if (s_model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "inference_init: model schema version %lu does not match "
                 "TFLite Micro schema version %d",
                 s_model->version(), TFLITE_SCHEMA_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }
    ESP_LOGI(TAG, "model loaded: schema version %lu, %d bytes",
             s_model->version(), g_person_detect_model_data_len);

    // ── Step 4: register ops ──────────────────────────────────────────────
    // Person detection model (MobileNet INT8) requires exactly these 5 ops.
    // Community note: derive the op list from the model using Netron viewer
    // (https://netron.app) or tflite::PrintModelInfo(). A missing op causes
    // AllocateTensors() to fail — the error message names the missing op.
    s_resolver.AddDepthwiseConv2D();  // depthwise separable convolution layers
    s_resolver.AddConv2D();           // pointwise convolution (1×1 conv)
    s_resolver.AddAveragePool2D();    // global average pooling before classifier
    s_resolver.AddReshape();          // flatten before softmax
    s_resolver.AddSoftmax();          // final class probability layer

    // ── Step 5: build interpreter ─────────────────────────────────────────
    // MicroInterpreter is placed in static memory. The arena backs all tensors.
    static tflite::MicroInterpreter static_interpreter(
        s_model, s_resolver, s_tensor_arena, INFERENCE_ARENA_SIZE);
    s_interpreter = &static_interpreter;

    // AllocateTensors() lays out all tensors within the arena.
    // Failure here means the arena is too small or an op is missing.
    TfLiteStatus alloc_status = s_interpreter->AllocateTensors();
    if (alloc_status != kTfLiteOk) {
        ESP_LOGE(TAG, "inference_init: AllocateTensors() failed — "
                 "arena too small or missing op");
        return ESP_FAIL;
    }

    // Log actual arena usage — training document data point.
    // This is the measured arena consumption, not the allocated size.
    ESP_LOGI(TAG, "tensors allocated — arena used: %u bytes of %d KB",
             s_interpreter->arena_used_bytes(), INFERENCE_ARENA_SIZE / 1024);

    // ── Step 6: cache input tensor pointer ───────────────────────────────
    // Cached once to avoid repeated lookups in the inference loop.
    s_input_tensor = s_interpreter->input(0);

    ESP_LOGI(TAG, "inference_init: complete — input tensor %d×%d×%d type=%d",
             s_input_tensor->dims->data[1],   // height
             s_input_tensor->dims->data[2],   // width
             s_input_tensor->dims->data[3],   // channels
             s_input_tensor->type);

    return ESP_OK;
}

// ── inference_run ──────────────────────────────────────────────────────────
// Full per-frame pipeline: JPEG decode → grayscale resize → tensor fill →
// Invoke() → dequantize output → populate result.
//
// jpeg_data: raw JPEG buffer (camera_fb_t->buf)
// jpeg_len:  JPEG buffer length in bytes (camera_fb_t->len)
// result:    detection result — always populated on ESP_OK, even if no detection
esp_err_t inference_run(const uint8_t*      jpeg_data,
                        size_t              jpeg_len,
                        inference_result_t* result)
{
    // ── Step 1: decode JPEG to RGB888 ─────────────────────────────────────
    // fmt2rgb888() uses the ESP32-S3 hardware-assisted JPEG decoder via the
    // esp32-camera component. Decodes directly into the pre-allocated PSRAM buffer.
    // PIXFORMAT_JPEG tells the converter the source format.
    bool decode_ok = fmt2rgb888(jpeg_data, jpeg_len, PIXFORMAT_JPEG, s_rgb_buf);
    if (!decode_ok) {
        ESP_LOGW(TAG, "inference_run: JPEG decode failed — skipping frame");
        result->detection_valid = false;
        return ESP_FAIL;
    }

    // ── Step 2: fill input tensor — grayscale + nearest-neighbor resize ──
    // The model expects 96×96×1 INT8 (grayscale, quantized).
    // Single-pass: for each output pixel, find the source pixel via nearest-
    // neighbor mapping, convert RGB to grayscale, quantize to INT8.
    //
    // Quantization: input_zero_point and input_scale from tensor quantization
    // parameters are used for correct INT8 conversion. For the person detection
    // model, zero_point=128 and scale≈0.0078, so subtracting 128 from the
    // uint8 grayscale value is equivalent to the standard quantization step.
    int8_t* tensor_data   = s_input_tensor->data.int8;
    const float x_scale   = static_cast<float>(CAM_WIDTH)  / INFERENCE_INPUT_WIDTH;
    const float y_scale   = static_cast<float>(CAM_HEIGHT) / INFERENCE_INPUT_HEIGHT;
    const int   zp        = s_input_tensor->params.zero_point;  // typically 128

    for (int y = 0; y < INFERENCE_INPUT_HEIGHT; y++) {
        // Map output row to source row (nearest neighbor)
        const int src_y = static_cast<int>(y * y_scale);

        for (int x = 0; x < INFERENCE_INPUT_WIDTH; x++) {
            // Map output column to source column (nearest neighbor)
            const int src_x = static_cast<int>(x * x_scale);

            // Source pixel offset in RGB888 buffer (3 bytes per pixel)
            const int src_offset = (src_y * CAM_WIDTH + src_x) * 3;
            const uint8_t r = s_rgb_buf[src_offset    ];
            const uint8_t g = s_rgb_buf[src_offset + 1];
            const uint8_t b = s_rgb_buf[src_offset + 2];

            // RGB → grayscale (ITU-R BT.601 luma coefficients, integer arithmetic)
            // gray = 0.299R + 0.587G + 0.114B  ≈  (77R + 150G + 29B) >> 8
            const uint8_t gray = static_cast<uint8_t>(
                (77 * r + 150 * g + 29 * b) >> 8);

            // Quantize uint8 grayscale to INT8: subtract zero_point
            // This matches the model's input quantization convention.
            tensor_data[y * INFERENCE_INPUT_WIDTH + x] =
                static_cast<int8_t>(static_cast<int>(gray) - zp);
        }
    }

    // ── Step 3: run inference ─────────────────────────────────────────────
    TfLiteStatus invoke_status = s_interpreter->Invoke();
    if (invoke_status != kTfLiteOk) {
        ESP_LOGE(TAG, "inference_run: Invoke() failed");
        result->detection_valid = false;
        return ESP_FAIL;
    }

    // ── Step 4: read and dequantize output tensor ─────────────────────────
    // Output tensor: 2 INT8 scores — index 0 = no person, index 1 = person.
    // Dequantize: float_val = (int8_val - zero_point) * scale
    TfLiteTensor* output = s_interpreter->output(0);
    const float   out_scale = output->params.scale;
    const int     out_zp    = output->params.zero_point;

    const float no_person_score =
        (static_cast<float>(output->data.int8[INFERENCE_SCORE_NO_PERSON]) - out_zp) * out_scale;
    const float person_score    =
        (static_cast<float>(output->data.int8[INFERENCE_SCORE_PERSON])    - out_zp) * out_scale;

    ESP_LOGD(TAG, "scores — person: %.3f  no_person: %.3f",
             person_score, no_person_score);

    // ── Step 5: populate result ───────────────────────────────────────────
    result->class_id        = INFERENCE_SCORE_PERSON;
    result->confidence      = person_score;
    result->detection_valid = (person_score >= INFERENCE_CONFIDENCE_THRESHOLD);

    return ESP_OK;
}
