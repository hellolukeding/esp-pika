set(AUDIO_ENGINE_SRCS
    "src/afe_pipeline.cc"
    "src/audio_engine.cc"
)

idf_component_register(
    SRCS ${AUDIO_ENGINE_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES board esp_common freertos
    PRIV_REQUIRES esp_audio_codec esp-sr heap log
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
