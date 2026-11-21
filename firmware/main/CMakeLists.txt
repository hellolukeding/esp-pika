idf_component_register(
    SRCS
        "src/app_state_machine.cc"
        "src/app_main.cc"
    INCLUDE_DIRS "include"
    REQUIRES board net_client audio_engine local_executor voiceprint ui_engine
    PRIV_REQUIRES nvs_flash esp_netif esp_event log freertos
)

target_compile_features(${COMPONENT_TARGET} PUBLIC cxx_std_20)
