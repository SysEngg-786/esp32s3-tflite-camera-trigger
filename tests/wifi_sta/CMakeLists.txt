# File: CMakeLists.txt
# Path: esp32s3-tflite-camera-trigger/tests/wifi_sta/CMakeLists.txt
# Role: Root build entry point for the isolated WiFi station test project.
#
# Purpose: Standalone ESP-IDF project that tests WiFi STA connectivity
#   using Espressif's reference pattern verbatim — no camera, no TFLite,
#   no trigger code. Used to isolate WiFi stack behaviour from the main
#   project during the WiFi association diagnostic pass.
#
# Build: Run from THIS directory, not the repo root.
#   cd tests/wifi_sta
#   idf.py set-target esp32s3
#   idf.py menuconfig   (set SSID and password)
#   idf.py build flash monitor
#
# Training document reference:
#   This test was created during the WiFi association diagnostic pass
#   (2026-08-29). If the reference pattern connects and the main project
#   does not — the issue is in the main project WiFi init sequence.
#   If both fail — the issue is environmental (AP policy, MAC block, band).

cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wifi_sta_test)
