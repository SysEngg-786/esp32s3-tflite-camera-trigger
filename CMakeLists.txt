# File: CMakeLists.txt
# Path: esp32s3-tflite-camera-trigger/CMakeLists.txt
# Role: Root build entry point — declares the project to ESP-IDF.
#       All module-level build rules live in main/CMakeLists.txt.
#
# MVP note: single factory-app build only. No OTA targets defined here —
#           that is a post-MVP addition at the seam, not a current build item.

cmake_minimum_required(VERSION 3.16)

# Pull in the ESP-IDF project infrastructure.
# IDF_PATH must be set in the environment (handled by idf.py / ESP-IDF export).
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Project name — matches the repo name deliberately so the build
# artifact is immediately identifiable.
project(esp32s3-tflite-camera-trigger)
