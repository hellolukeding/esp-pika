set(NET_CLIENT_SRCS
    "src/mac_identity.cc"
    "src/gateway_protocol.cc"
    "src/net_client.cc"
)

idf_component_register(
    SRCS ${NET_CLIENT_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES board esp_common esp_event esp_wifi esp_http_client esp_websocket_client freertos
    PRIV_REQUIRES esp_hw_support mdns esp_netif log lwip
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
