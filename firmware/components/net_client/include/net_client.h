#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "gateway_protocol.h"
#include "wifi_portal.h"
#include "wifi_store.h"

#ifdef ESP_PLATFORM
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
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

/// Wi-Fi STA / provisioning high-level connection state.
enum class WifiConnState {
    kOffline = 0,
    kConnecting,
    kConnected,
    kPortal,
};

/// Merged AP record combining live scan telemetry and NVS credential state.
struct WifiApInfo {
    std::string ssid;
    int rssi = -100;
    bool secure = true;
    bool saved = false;
    bool connected = false;
};

/// Immutable snapshot emitted on Wi-Fi status, scan, or portal state changes.
struct WifiStatusSnapshot {
    WifiConnState state = WifiConnState::kOffline;
    bool wifi_connected = false;
    bool portal_active = false;
    bool scanning = false;
    std::string mac;
    std::string ssid;
    std::string ip;
    int rssi = -100;
    std::string ap_ssid;
    std::string ap_ip = "192.168.4.1";
    std::string gateway_host;
    uint16_t gateway_port = 8080;
    std::string status_msg;
    uint32_t revision = 0;
    std::vector<WifiApInfo> aps;
};

/// Parses TXT key=value pairs advertised by gateway mDNS into a GatewayEndpoint.
void ApplyMdnsTxtRecords(const std::vector<std::pair<std::string, std::string>>& txt,
                         GatewayEndpoint* endpoint);

/// Merges saved credentials and scanned APs into a deduplicated, prioritized list (max 20).
std::vector<WifiApInfo> MergeWifiLists(const std::vector<WifiCredential>& saved,
                                       const std::vector<WifiApInfo>& scanned,
                                       const std::string& connected_ssid);

/// Selects visible saved credentials ordered by strongest scanned RSSI descending.
std::vector<WifiCredential> SelectAutoConnectCandidates(const std::vector<WifiCredential>& saved,
                                                        const std::vector<WifiApInfo>& scanned,
                                                        const std::string& exclude_ssid = "");

/// Maps an ESP-IDF Wi-Fi disconnect reason code to a concise ASCII diagnostic label.
const char* WifiReasonToMessage(int reason);

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

/// Manages multi-Wi-Fi NVS credentials, auto-connect/failover, SoftAP Captive Portal,
/// mDNS gateway discovery, strict eFuse-MAC WebSocket client, and HTTP media upload.
class NetClient final : public MediaUploader {
public:
    using TextCallback = std::function<void(const std::string& json_text)>;
    using BinaryCallback = std::function<void(const uint8_t* data, size_t len)>;
    using ConnStateCallback = std::function<void(bool connected)>;
    using WifiStateCallback = std::function<void(const WifiStatusSnapshot& status)>;

    NetClient() = default;
    ~NetClient() override;

    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    /// Registers a callback invoked on Wi-Fi status, scan, or portal state changes.
    void SetWifiStateCallback(WifiStateCallback cb);

    /// Initializes factory MAC identity, loads saved Wi-Fi configs, starts worker & auto-connect.
    bool Init(const NetClientConfig& cfg);

    /// Waits up to timeout_ms for Wi-Fi STA IP acquisition after Init() started STA.
    bool WaitForWifi(uint32_t timeout_ms);

    /// Asynchronous Wi-Fi management actions executed on the Core 1 internal-DRAM worker task.
    bool RequestScan();
    bool RequestConnect(const std::string& ssid, const std::string& password, bool use_saved = false);
    bool RequestForget(const std::string& ssid);
    bool RequestPortal(bool enable);
    bool SetGatewayOverride(const std::string& host, uint16_t port);
    WifiGatewayOverride GetGatewayOverride() const;

    WifiStatusSnapshot GetWifiStatus() const;
    bool IsWifiConnected() const { return wifi_connected_.load(std::memory_order_acquire); }
    bool IsPortalActive() const { return portal_active_.load(std::memory_order_acquire); }

    /// Guarantees the mDNS responder/client is initialized on the station interface.
    void EnsureMdnsStarted();

    /// Queries mDNS `_esp-gateway._tcp` on the local network (or applies override / fallback).
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
    bool IsConnected() const { return ws_connected_.load(std::memory_order_acquire); }

private:
    enum class WorkerCmdType {
        kAutoConnect = 0,
        kScan,
        kConnect,
        kForget,
        kTogglePortal,
        kSetGateway,
        kReconnectRuntime,
    };

    struct WorkerCmd {
        WorkerCmdType type = WorkerCmdType::kAutoConnect;
        std::string ssid;
        std::string password;
        bool flag = false;
        uint16_t port = 8080;
    };

    void EnqueueWorkerCmd(WorkerCmd cmd, bool urgent = false);
    bool PopWorkerCmd(WorkerCmd* out);
    bool HasPendingUrgentCmd() const;
    void DisconnectWebSocketLocked();
    void RefreshStatusLocked();
    void EmitWifiStatus(bool bump_revision = true);
    std::string BuildStatusJson() const;
    std::string BuildScanJson(bool trigger_scan);

    NetClientConfig cfg_;
    GatewayEndpoint endpoint_;
    std::string device_mac_;
    mutable std::mutex ws_lifecycle_mu_;
    std::atomic<bool> ws_connected_{false};
    std::atomic<bool> ws_closing_{false};
    std::atomic<int> ws_in_flight_{0};
    std::atomic<bool> wifi_connected_{false};
    std::atomic<bool> portal_active_{false};

    TextCallback on_text_;
    BinaryCallback on_binary_;
    ConnStateCallback on_state_;

    mutable std::mutex wifi_cb_mu_;
    WifiStateCallback on_wifi_state_;

    mutable std::mutex state_mu_;
    WifiStore wifi_store_;
    WifiPortal portal_;
    std::vector<WifiApInfo> scanned_aps_;
    WifiStatusSnapshot status_;

    mutable std::mutex cmd_mu_;
    std::queue<WorkerCmd> cmd_queue_;

#ifdef ESP_PLATFORM
    bool portal_manual_ = false;
    int64_t portal_auto_stop_ms_ = 0;
    void StartWorker();
    void StopWorker();
    static void WorkerTaskEntry(void* arg);
    void RunWorkerLoop();
    void HandleWorkerCmd(const WorkerCmd& cmd);
    bool RunScanInternal();
    bool ConnectSingleNetwork(const std::string& ssid,
                              const std::string& password,
                              uint32_t timeout_ms,
                              int* out_reason);
    bool RunAutoConnect(const std::string& exclude_ssid = "");
    void RunRuntimeReconnect();
    void EnsureMdnsInitialized();
    bool EnablePortalInternal(bool manual);
    void DisablePortalInternal();
    static void WifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
    static void WebSocketEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);

    bool wifi_started_ = false;
    bool mdns_started_ = false;
    esp_netif_t* sta_netif_ = nullptr;
    esp_netif_t* ap_netif_ = nullptr;
    esp_websocket_client_handle_t ws_handle_ = nullptr;
    EventGroupHandle_t wifi_events_ = nullptr;
    TaskHandle_t worker_task_ = nullptr;
    SemaphoreHandle_t worker_sem_ = nullptr;
    SemaphoreHandle_t worker_done_ = nullptr;
    std::atomic<bool> worker_running_{false};
    std::atomic<bool> connect_attempt_active_{false};
    std::atomic<bool> intentional_disconnect_{false};
    std::atomic<int> last_disconnect_reason_{0};
    std::string rx_text_buf_;
#endif
};

} // namespace esp_pika
