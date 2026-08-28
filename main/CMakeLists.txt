# File: CMakeLists.txt
# Path: esp32s3-tflite-camera-trigger/main/CMakeLists.txt
# Role: Registers all source files as a single ESP-IDF component named "main".
#
# Design decision — one component, four subdirectories:
#   The camera, inference, trigger, and model modules are tightly coupled
#   through the orchestration in main.cpp. Splitting them into independent
#   ESP-IDF components would add CMake interface overhead with no isolation
#   benefit at this scale. File-level separation (Rule 12 / SOC) is preserved;
#   build-level separation is not forced where it adds no value.
#
# MVP note: no test targets, no coverage, no component mocking defined here.
#   Those are post-MVP additions at the named seam.

idf_component_register(
    SRCS
        # Orchestration layer — wires modules together, owns the main task loop
        "main.cpp"

        # Camera module — init, frame capture, format conversion to model input
        "camera/camera_init.cpp"

        # Inference module — TFLite Micro runner
        "inference/inference_engine.cpp"

        # Person detection model — C array flatbuffer compiled directly into flash.
        # MVP pipeline verification only. Swapped for COCO model in benchmark pass.
        # Source: espressif/esp-tflite-micro managed component, person_detection example.
        # Path in component: examples/person_detection/main/person_detect_model_data.cc
        # Copied to main/model/ so it is tracked in source, not in gitignored managed_components.
        # Array names: g_person_detect_model_data / g_person_detect_model_data_len
        "model/person_detect_model_data.cc"

        # Trigger module — WiFi stack, HTTP POST dispatch on detection event
        "trigger/web_trigger.cpp"

    INCLUDE_DIRS
        # Expose all module headers to every translation unit in this component
        "."
        "camera"
        "inference"
        "trigger"
        "model"
)