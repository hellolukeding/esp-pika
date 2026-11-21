set(LOCAL_EXECUTOR_SRCS
    "src/local_executor.cc"
)

idf_component_register(
    SRCS ${LOCAL_EXECUTOR_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES board net_client voiceprint esp_common
    PRIV_REQUIRES log
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
