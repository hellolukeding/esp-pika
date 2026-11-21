# ESP-IDF v6.x top-level CMake project for esp-pika ESP32-S31 firmware
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/configs/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/configs/sdkconfig.defaults.esp32s31"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_pika)
