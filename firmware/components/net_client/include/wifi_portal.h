#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "gateway_protocol.h"

#ifdef ESP_PLATFORM
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

namespace esp_pika {

/// Builds the SoftAP SSID (`ESP-Mosaico-XXXX`) from the last 4 hex digits of the MAC address.
std::string BuildPortalApSsid(const std::string& mac);

/// Builds a wildcard DNS A-record response pointing any A query to `ipv4_be` (network byte order).
/// Returns reply byte length, or 0 if `query` is malformed or `max_len` is insufficient.
size_t BuildDnsWildcardReply(const uint8_t* query,
                             size_t query_len,
                             uint32_t ipv4_be,
                             uint8_t* out_reply,
                             size_t max_len);

/// Parses `POST /api/wifi/connect` JSON body (`{"ssid":...,"password":...,"use_saved":...}`).
bool ParsePortalConnectRequest(const std::string& body,
                               std::string* ssid,
                               std::string* password,
                               bool* use_saved);

/// Parses `POST /api/wifi/forget` JSON body (`{"ssid":...}`).
bool ParsePortalForgetRequest(const std::string& body, std::string* ssid);

/// Parses `POST /api/wifi/gateway` JSON body (`{"host":...,"port":...}`).
bool ParsePortalGatewayRequest(const std::string& body, std::string* host, uint16_t* port);

struct WifiPortalCallbacks {
    std::function<std::string()> get_status_json;
    std::function<std::string(bool trigger_scan)> get_scan_json;
    std::function<bool(const std::string& ssid,
                       const std::string& password,
                       bool use_saved,
                       std::string* out_msg)>
        on_connect;
    std::function<bool(const std::string& ssid, std::string* out_msg)> on_forget;
    std::function<bool(const std::string& host, uint16_t port, std::string* out_msg)> on_set_gateway;
};

/// Ephemeral SoftAP Captive Portal (`esp_http_server` + UDP:53 DNS wildcard -> 192.168.4.1).
class WifiPortal {
public:
    WifiPortal() = default;
    ~WifiPortal();

    WifiPortal(const WifiPortal&) = delete;
    WifiPortal& operator=(const WifiPortal&) = delete;

    bool Start(WifiPortalCallbacks callbacks);
    void Stop();
    bool IsRunning() const { return running_.load(std::memory_order_acquire); }

private:
    WifiPortalCallbacks callbacks_;
    std::atomic<bool> running_{false};

#ifdef ESP_PLATFORM
    static esp_err_t HandleIndex(httpd_req_t* req);
    static esp_err_t HandleStatus(httpd_req_t* req);
    static esp_err_t HandleScan(httpd_req_t* req);
    static esp_err_t HandleConnect(httpd_req_t* req);
    static esp_err_t HandleForget(httpd_req_t* req);
    static esp_err_t HandleGateway(httpd_req_t* req);
    static esp_err_t HandleCaptiveRedirect(httpd_req_t* req);
    static void DnsTaskEntry(void* arg);
    void RunDnsLoop();

    httpd_handle_t httpd_ = nullptr;
    TaskHandle_t dns_task_ = nullptr;
    SemaphoreHandle_t dns_done_ = nullptr;
#endif
};

} // namespace esp_pika
