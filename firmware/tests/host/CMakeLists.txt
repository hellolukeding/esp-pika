cmake_minimum_required(VERSION 3.16)
if(APPLE AND NOT DEFINED CMAKE_OSX_SYSROOT)
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE MACOS_SDK_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(MACOS_SDK_PATH)
        set(CMAKE_OSX_SYSROOT "${MACOS_SDK_PATH}" CACHE PATH "macOS SDK path")
    endif()
endif()
project(esp_pika_firmware_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FIRMWARE_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")

add_executable(firmware_host_test
    firmware_host_test.cc
    "${FIRMWARE_DIR}/components/board/src/board_s31_coreboard.cc"
    "${FIRMWARE_DIR}/components/board/src/board_s31_korvo.cc"
    "${FIRMWARE_DIR}/components/board/src/board_s31_mosaico.cc"
    "${FIRMWARE_DIR}/components/net_client/src/mac_identity.cc"
    "${FIRMWARE_DIR}/components/net_client/src/gateway_protocol.cc"
    "${FIRMWARE_DIR}/components/net_client/src/net_client.cc"
    "${FIRMWARE_DIR}/components/audio_engine/src/afe_pipeline.cc"
    "${FIRMWARE_DIR}/components/audio_engine/src/audio_engine.cc"
    "${FIRMWARE_DIR}/components/voiceprint/src/fbank_extractor.cc"
    "${FIRMWARE_DIR}/components/voiceprint/src/voiceprint_store.cc"
    "${FIRMWARE_DIR}/components/voiceprint/src/voiceprint_engine.cc"
    "${FIRMWARE_DIR}/components/local_executor/src/local_executor.cc"
    "${FIRMWARE_DIR}/components/ui_engine/src/ui_font.cc"
    "${FIRMWARE_DIR}/components/ui_engine/src/avatar_animator.cc"
    "${FIRMWARE_DIR}/components/ui_engine/src/avatar_3d_renderer.cc"
    "${FIRMWARE_DIR}/components/ui_engine/src/mosaico_ui.cc"
    "${FIRMWARE_DIR}/components/ui_engine/src/ui_service.cc"
    "${FIRMWARE_DIR}/main/src/app_state_machine.cc"
)

target_include_directories(firmware_host_test PRIVATE
    "${FIRMWARE_DIR}/components/board/include"
    "${FIRMWARE_DIR}/components/net_client/include"
    "${FIRMWARE_DIR}/components/audio_engine/include"
    "${FIRMWARE_DIR}/components/voiceprint/include"
    "${FIRMWARE_DIR}/components/local_executor/include"
    "${FIRMWARE_DIR}/components/ui_engine/include"
    "${FIRMWARE_DIR}/main/include"
)

if(APPLE AND EXISTS "${CMAKE_OSX_SYSROOT}/usr/include/c++/v1")
    target_compile_options(firmware_host_test PRIVATE
        -nostdinc++
        -isystem "${CMAKE_OSX_SYSROOT}/usr/include/c++/v1"
    )
endif()

target_compile_options(firmware_host_test PRIVATE
    -fno-exceptions
    -fno-rtti
    -Wall
    -Wextra
    -Werror
)

add_executable(render_mosaico_preview
    render_mosaico_preview.cc
    "${FIRMWARE_DIR}/components/ui_engine/src/ui_font.cc"
    "${FIRMWARE_DIR}/components/ui_engine/src/avatar_animator.cc"
    "${FIRMWARE_DIR}/components/ui_engine/src/avatar_3d_renderer.cc"
    "${FIRMWARE_DIR}/components/ui_engine/src/mosaico_ui.cc"
)

target_include_directories(render_mosaico_preview PRIVATE
    "${FIRMWARE_DIR}/components/board/include"
    "${FIRMWARE_DIR}/components/ui_engine/include"
)

if(APPLE AND EXISTS "${CMAKE_OSX_SYSROOT}/usr/include/c++/v1")
    target_compile_options(render_mosaico_preview PRIVATE
        -nostdinc++
        -isystem "${CMAKE_OSX_SYSROOT}/usr/include/c++/v1"
    )
endif()

target_compile_options(render_mosaico_preview PRIVATE
    -O2
    -fno-exceptions
    -fno-rtti
    -Wall
    -Wextra
    -Werror
)

enable_testing()
add_test(NAME firmware_host_test COMMAND firmware_host_test)
