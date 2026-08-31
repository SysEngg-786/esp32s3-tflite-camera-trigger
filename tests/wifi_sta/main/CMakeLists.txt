# File: CMakeLists.txt
# Path: esp32s3-tflite-camera-trigger/tests/wifi_sta/main/CMakeLists.txt
# Role: Registers the single test source file as the main component.

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
)
