# PagerOS device firmware — top-level project file.
#
# Target hardware: LILYGO T-LoRa Pager (ESP32-S3 N16R8, 16 MB flash, 8 MB OPI PSRAM).
# Partition layout: A/B OTA + factory recovery + bundled fonts + storage (see partitions.csv).
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pageros)
