idf_component_register(
    SRCS
        "src/ui_font.cc"
        "src/avatar_animator.cc"
        "src/avatar_3d_renderer.cc"
        "src/mosaico_ui.cc"
        "src/ui_service.cc"
    INCLUDE_DIRS "include"
    REQUIRES board
    PRIV_REQUIRES audio_engine esp_timer freertos heap log
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -ffast-math -fno-math-errno)
