#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "gateway_protocol.h"

#ifdef ESP_PLATFORM
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#endif

namespace esp_pika {

/// Gateway network endpoint resolved via mDNS (_esp-gateway._tcp) or fallback config.
struct GatewayEndpoint {
    std::string host;
    uint16_t port = 8080;
    bool tls = false;
    std::string group_id = "home-default";
    std::string ws_path = "/ws/device";
    std::string media_path = "/api/v1/media/upload";
};

/// Parses TXT key=value pairs advertised by gateway mDNS into a GatewayEndpoint.
void ApplyMdnsTxtRecords(const std::vector<std::pair<std::string, std::string>>& txt,
                         GatewayEndpoint* endpoint);

/// Abstract interface for independent media-plane HTTP POST uploads (JPEG/sensor blobs).
class MediaUploader {
public:
    virtual ~MediaUploader() = default;
    virtual bool UploadJpeg(const uint8_t* data, size_t len, std::string* out_media_url) = 0;
};

struct NetClientConfig {
    std::string wifi_ssid;
    std::string wifi_password;
    std::string fallback_host;
    uint16_t fallback_port = 8080;
    uint32_t mdns_timeout_ms = 4000;
};

/// Manages Wi-Fi STA, mDNS gateway discovery, strict eFuse-MAC WebSocket client, and HTTP media upload.
class NetClient final : public MediaUploader {
public:
    using TextCallback = std::function<void(const std::string& json_text)>;
    using BinaryCallback = std::function<void(const uint8_t* data, size_t len)>;
    using ConnStateCallback = std::function<void(bool connected)>;

    NetClient() = default;
    ~NetClient() override;

    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    /// Initializes factory MAC identity, network stack, and connects to Wi-Fi STA.
    bool Init(const NetClientConfig& cfg);

    /// Waits up to timeout_ms for Wi-Fi STA IP acquisition after Init() started STA.
    bool WaitForWifi(uint32_t timeout_ms);

    /// Queries mDNS `_esp-gateway._tcp` on the local network (or applies configured fallback).
    bool DiscoverGateway(GatewayEndpoint* out_endpoint);

    /// Connects `/ws/device` with mandatory `X-Device-MAC` handshake header.
    bool ConnectWebSocket(const GatewayEndpoint& endpoint,
                          TextCallback on_text,
                          BinaryCallback on_binary,
                          ConnStateCallback on_state);

    void DisconnectWebSocket();

    bool SendText(const std::string& json_text);
    bool SendBinary(const uint8_t* data, size_t len);

    bool UploadJpeg(const uint8_t* data, size_t len, std::string* out_media_url) override;

    const std::string& DeviceMac() const { return device_mac_; }
    bool IsConnected() const { return ws_connected_; }

private:
    NetClientConfig cfg_;
    GatewayEndpoint endpoint_;
    std::string device_mac_;
    bool ws_connected_ = false;
    TextCallback on_text_;
    BinaryCallback on_binary_;
    ConnStateCallback on_state_;

#ifdef ESP_PLATFORM
    bool wifi_started_ = false;
    bool mdns_started_ = false;
    esp_websocket_client_handle_t ws_handle_ = nullptr;
    EventGroupHandle_t wifi_events_ = nullptr;
    std::string rx_text_buf_;

    static void WifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
    static void WebSocketEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
#endif
};

} // namespace esp_pika
