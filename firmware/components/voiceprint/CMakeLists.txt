set(VOICEPRINT_SRCS
    "src/fbank_extractor.cc"
    "src/voiceprint_store.cc"
    "src/voiceprint_engine.cc"
)

idf_component_register(
    SRCS ${VOICEPRINT_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES esp_common freertos net_client
    PRIV_REQUIRES nvs_flash heap log esp-dsp esp-dl
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
