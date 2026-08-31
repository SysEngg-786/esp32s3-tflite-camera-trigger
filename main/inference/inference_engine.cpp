// File: inference_engine.cpp
// Path: esp32s3-tflite-camera-trigger/main/inference/inference_engine.cpp
// Role: Inference module — TFLite Micro pipeline for person detection.
//
// Pipeline (per frame):
//   RGB565 camera frame (PSRAM) → direct grayscale + nearest-neighbor resize
//   → 96×96 INT8 tensor (internal SRAM) → Invoke() → dequantize → result
//
// Key design decision — no intermediate RGB888 buffer:
//   Previous approach used fmt2rgb888() which wrote 57,600 bytes to a buffer.
//   Whether that buffer was in PSRAM or SRAM, the bulk write from CPU1 (detect_task)
//   into PSRAM (or the fmt2rgb888 internal PSRAM reads of the camera frame) triggered
//   esp_crosscore_isr from CPU0 WiFi PSRAM access — preempting the convolution
//   kernel on every iteration and causing Invoke() to never complete (>60s watchdog).
//
//   Solution: single-pass RGB565 → grayscale → resize with no intermediate buffer.
//   One scattered 16-bit read per output pixel (9,216 reads for 96×96).
//   Scattered reads generate far less cache pressure than 57,600-byte bulk writes.
//   All writes go to internal SRAM (tensor arena) — no cross-core coherency needed.
//
// Memory:
//   Tensor arena: 130KB internal SRAM (MALLOC_CAP_INTERNAL)
//   No RGB888 buffer — eliminated.
//   Camera frames: PSRAM, owned by camera module.
//
// Op resolver: MicroMutableOpResolver<5> — person detection MobileNet INT8.
//   DepthwiseConv2D, Conv2D, AveragePool2D, Reshape, Softmax.
//   AllOpsResolver adds ~700KB flash — not used.

#include "inference_engine.h"
#include "person_detect_model_data.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

static const char* TAG = "inference";

// ── Module-level state ────────────────────────────────────────────────────
static const tflite::Model*              s_model        = nullptr;
static tflite::MicroMutableOpResolver<5> s_resolver;
static tflite::MicroInterpreter*         s_interpreter  = nullptr;
static TfLiteTensor*                     s_input_tensor = nullptr;
static uint8_t*                          s_tensor_arena = nullptr;

// Camera frame dimensions — must match FRAMESIZE_QQVGA in camera_init.cpp
static constexpr int CAM_WIDTH  = 160;
static constexpr int CAM_HEIGHT = 120;

// ── inference_init ─────────────────────────────────────────────────────────
esp_err_t inference_init(void)
{
    ESP_LOGI(TAG, "inference_init: loading person detection model from C array");

    // ── Step 1: tensor arena in internal SRAM ─────────────────────────────
    // MALLOC_CAP_INTERNAL — on-chip SRAM, 160MHz, single-cycle access.
    // No PSRAM bus contention. No cross-core cache coherency pressure.
    // Benchmark axis: PSRAM vs SRAM arena (this = SRAM configuration).
    s_tensor_arena = static_cast<uint8_t*>(
        heap_caps_malloc(INFERENCE_ARENA_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (s_tensor_arena == nullptr) {
        ESP_LOGE(TAG, "inference_init: failed to allocate %d byte arena in internal SRAM",
                 INFERENCE_ARENA_SIZE);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "tensor arena: %d KB allocated in internal SRAM",
             INFERENCE_ARENA_SIZE / 1024);

    // ── Step 2: load and verify model ─────────────────────────────────────
    s_model = tflite::GetModel(g_person_detect_model_data);
    if (s_model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "inference_init: model schema %lu != TFLite schema %d",
                 s_model->version(), TFLITE_SCHEMA_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }
    ESP_LOGI(TAG, "model loaded: schema version %lu, %d bytes",
             s_model->version(), g_person_detect_model_data_len);

    // ── Step 3: register ops ──────────────────────────────────────────────
    // Person detection MobileNet INT8 — 5 ops derived from model via Netron.
    // Missing op → AllocateTensors() fails and names it.
    s_resolver.AddDepthwiseConv2D();
    s_resolver.AddConv2D();
    s_resolver.AddAveragePool2D();
    s_resolver.AddReshape();
    s_resolver.AddSoftmax();

    // ── Step 4: build interpreter ─────────────────────────────────────────
    static tflite::MicroInterpreter static_interpreter(
        s_model, s_resolver, s_tensor_arena, INFERENCE_ARENA_SIZE);
    s_interpreter = &static_interpreter;

    if (s_interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "inference_init: AllocateTensors() failed — "
                 "arena too small or missing op");
        return ESP_FAIL;
    }

    // Measured arena usage — training document data point
    ESP_LOGI(TAG, "tensors allocated — arena used: %u bytes of %d KB",
             s_interpreter->arena_used_bytes(), INFERENCE_ARENA_SIZE / 1024);

    // ── Step 5: cache input tensor pointer ────────────────────────────────
    s_input_tensor = s_interpreter->input(0);
    ESP_LOGI(TAG, "inference_init: complete — input tensor %d×%d×%d type=%d",
             s_input_tensor->dims->data[1],
             s_input_tensor->dims->data[2],
             s_input_tensor->dims->data[3],
             s_input_tensor->type);

    return ESP_OK;
}

// ── inference_run ──────────────────────────────────────────────────────────
// Direct single-pass pipeline: RGB565 → grayscale → 96×96 → INT8 tensor.
// No intermediate RGB888 buffer. No fmt2rgb888() call.
// frame_data: raw RGB565 buffer (camera_fb_t->buf) in PSRAM
// frame_len:  buffer length in bytes (expected: 160×120×2 = 38,400)
esp_err_t inference_run(const uint8_t*      frame_data,
                        size_t              frame_len,
                        inference_result_t* result)
{
    // ── Step 1: direct RGB565 → grayscale → 96×96 → INT8 tensor ──────────
    // Single-pass nearest-neighbor resize with inline colour conversion.
    // For each 96×96 output pixel:
    //   - Map to nearest QQVGA source pixel
    //   - Read 16-bit RGB565 value from PSRAM camera frame (scattered read)
    //   - Convert RGB565 → R8/G8/B8 (bit extraction, no multiply)
    //   - Convert RGB888 → grayscale (ITU-R BT.601 luma)
    //   - Quantize to INT8 (subtract zero_point)
    //   - Write to tensor in internal SRAM (no PSRAM write pressure)
    //
    // RGB565 byte layout (little-endian, esp32-camera DVP output):
    //   frame_data[2i]   = low byte  (GGGBBBBB)
    //   frame_data[2i+1] = high byte (RRRRRGGG)
    //   pixel16 = frame_data[2i] | (frame_data[2i+1] << 8)

    int8_t*     tensor_data = s_input_tensor->data.int8;
    const float x_scale     = static_cast<float>(CAM_WIDTH)  / INFERENCE_INPUT_WIDTH;
    const float y_scale     = static_cast<float>(CAM_HEIGHT) / INFERENCE_INPUT_HEIGHT;
    const int   zp          = s_input_tensor->params.zero_point;

    for (int y = 0; y < INFERENCE_INPUT_HEIGHT; y++) {
        const int src_y = static_cast<int>(y * y_scale);
        for (int x = 0; x < INFERENCE_INPUT_WIDTH; x++) {
            const int src_x  = static_cast<int>(x * x_scale);
            const int offset = (src_y * CAM_WIDTH + src_x) * 2;

            // Read RGB565 pixel — two bytes from PSRAM camera frame
            const uint16_t pixel =
                static_cast<uint16_t>(frame_data[offset]) |
                (static_cast<uint16_t>(frame_data[offset + 1]) << 8);

            // Extract RGB components — scale to 8-bit
            const uint8_t r = (pixel >> 8) & 0xF8;   // R5 → R8
            const uint8_t g = (pixel >> 3) & 0xFC;   // G6 → G8
            const uint8_t b = (pixel << 3) & 0xF8;   // B5 → B8

            // Grayscale: ITU-R BT.601 luma (77R + 150G + 29B) >> 8
            const uint8_t gray = static_cast<uint8_t>(
                (77u * r + 150u * g + 29u * b) >> 8);

            // Quantize to INT8 and write to tensor (internal SRAM)
            tensor_data[y * INFERENCE_INPUT_WIDTH + x] =
                static_cast<int8_t>(static_cast<int>(gray) - zp);
        }
    }

    // ── Step 2: run inference ─────────────────────────────────────────────
    if (s_interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGE(TAG, "inference_run: Invoke() failed");
        result->detection_valid = false;
        return ESP_FAIL;
    }

    // ── Step 3: dequantize output tensor ──────────────────────────────────
    // Output: 2 INT8 scores — index 0 = no person, index 1 = person
    TfLiteTensor* output    = s_interpreter->output(0);
    const float   out_scale = output->params.scale;
    const int     out_zp    = output->params.zero_point;

    const float person_score =
        (static_cast<float>(output->data.int8[INFERENCE_SCORE_PERSON]) - out_zp)
        * out_scale;

    ESP_LOGD(TAG, "scores — person: %.3f", person_score);

    // ── Step 4: populate result ───────────────────────────────────────────
    result->class_id        = INFERENCE_SCORE_PERSON;
    result->confidence      = person_score;
    result->detection_valid = (person_score >= INFERENCE_CONFIDENCE_THRESHOLD);

    return ESP_OK;
}
