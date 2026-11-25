#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "ez_protocol.h"

#ifdef ESP_PLATFORM
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_websocket_client.h"
#endif

namespace esp_pika {

/// High-level lifecycle state of direct EZ Assistant pairing & connection.
enum class EzClientState {
    kOffline = 0,
    kUnpairedReady,
    kPairing,
    kConnecting,
    kOnline,
};

struct EzDeviceClientConfig {
    std::string mac;
    std::string display_name = "Mosaico";
    std::string firmware_version = "0.4.0-direct";
    uint16_t control_port = 8765;
    EzHardwareProfile hardware;
};

struct EzDeviceCallbacks {
    std::function<void(EzClientState state)> on_state;
    std::function<void(const std::string& state, const std::string& reason)> on_interaction_state;
    std::function<void(const std::string& text)> on_transcript;
    std::function<void(const std::string& output_id, const std::string& text)> on_text_output;
    std::function<void(const std::string& output_id, uint32_t stream_id, const std::string& text)>
        on_playback_start;
    std::function<void(const std::string& output_id, uint32_t stream_id, const std::string& reason)>
        on_playback_end;
    std::function<void(uint32_t stream_id, uint32_t sequence, const uint8_t* pcm, size_t len)>
        on_downlink_pcm;
    /// Invoked on Core 1 internal-DRAM task inside TickFast() so NVS/HAL operations stay safe.
    std::function<void(const EzDeviceCommandRequest& cmd)> on_device_command;
    /// Invoked on Core 1 internal-DRAM task inside TickFast() with raw `voiceprint_sync` JSON payload.
    std::function<void(const std::string& raw_payload)> on_voiceprint_sync;
};

/// Direct Device Protocol 1.0 client for EZ Assistant Desktop:
/// - Unpaired advertisement via `_ez-device._tcp` mDNS + local HTTP `:8765` (`/ez/info`, `/ez/connect`)
/// - TLS cert-pinned WSS `/device` (direct_confirm Ed25519 pairing + authenticated streaming)
/// - Passive `_ez-assistant._tcp` mDNS fallback & dynamic ephemeral port re-resolution
class EzDeviceClient {
public:
    EzDeviceClient() = default;
    ~EzDeviceClient();

    EzDeviceClient(const EzDeviceClient&) = delete;
    EzDeviceClient& operator=(const EzDeviceClient&) = delete;

    /// Loads or generates persistent Ed25519 identity in NVS `ez_pair`.
    /// Must run on a task whose stack lives in internal DRAM (`app_main`).
    bool Init(const EzDeviceClientConfig& cfg, EzDeviceCallbacks callbacks);

    /// Signals current Wi-Fi STA availability; heavy teardown/startup executes in TickFast().
    void SetWifiConnected(bool connected);

    /// Periodic pump called from Core 1 internal-DRAM task (`app_main` 20ms loop).
    void TickFast(uint32_t dt_ms);

    /// Thread-safe trigger to clear stored pairing credentials (executed in TickFast()).
    void ResetPairing();

    /// Starts a voice listen segment (`pcm_s16le`, 16kHz mono, 20ms) if authenticated and ASR ready.
    bool StartListen(uint32_t* out_stream_id);
    bool SendUplinkPcmFrame(uint32_t stream_id, uint32_t sequence, const uint8_t pcm640[640]);
    bool StopListen(uint32_t stream_id, uint32_t last_sequence);
    bool CancelListen(uint32_t stream_id);
    bool CancelPlayback(const std::string& output_id, uint32_t stream_id);
    bool SendDeviceCommandResult(const std::string& command_id,
                                 bool ok,
                                 const std::string& result_json,
                                 const std::string& media_url = "",
                                 const std::string& error_code = "",
                                 const std::string& error_message = "");
    bool SendVoiceprintSyncAck(uint64_t vp_version,
                               uint32_t count,
                               bool ok,
                               const std::string& error = "");
    void SetVoiceprintVersion(uint64_t vp_version);

    EzClientState State() const {
        return static_cast<EzClientState>(state_.load(std::memory_order_acquire));
    }
    bool IsPaired() const;
    bool IsOnline() const { return State() == EzClientState::kOnline; }
    bool IsTtsReady() const { return tts_ready_.load(std::memory_order_acquire); }
    bool IsAsrReady() const { return asr_ready_.load(std::memory_order_acquire); }
    std::string KeyFingerprint() const;
    std::string DisplayName() const;
    std::string PairedHost() const;

private:
    std::string NextMessageId();
    void SetState(EzClientState next);
    bool SendJson(const std::string& json_text);
    bool SendBinary(const uint8_t* data, size_t len);
    void HandleInboundEnvelope(const std::string& json_text);
    void HandleInboundBinary(const uint8_t* data, size_t len);

#ifdef ESP_PLATFORM
    void EnsureUnpairedDiscoveryStarted();
    void StopUnpairedDiscovery();
    bool StartWebSocketLocked(const std::string& host,
                              uint16_t port,
                              const std::string& path,
                              const std::string& cert_fingerprint_hex,
                              bool pairing_mode);
    void StopWebSocketLocked();
    bool QueryEzAssistantMdns(const std::string& filter_installation_id,
                              bool require_pairing_open,
                              EzConnectTrigger* out);

    static esp_err_t HandleHttpInfo(httpd_req_t* req);
    static esp_err_t HandleHttpConnect(httpd_req_t* req);
    static void WebSocketEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);

    httpd_handle_t httpd_ = nullptr;
    bool mdns_service_registered_ = false;
    esp_websocket_client_handle_t ws_handle_ = nullptr;
    std::string rx_text_buf_;
    std::vector<uint8_t> rx_bin_buf_;
#endif

    EzDeviceClientConfig cfg_;
    EzDeviceCallbacks callbacks_;

    mutable std::mutex mu_;
    mutable std::mutex ws_lifecycle_mu_;
    mutable std::mutex tx_mu_;

    EzPairingIdentity identity_;
    EzCapabilities caps_;
    std::vector<EzDeviceCommandRequest> pending_commands_;
    std::vector<std::string> pending_vp_syncs_;

    // Active pairing / auth handshake state protected by `mu_`
    bool ws_pairing_mode_ = false;
    EzConnectTrigger pairing_target_;
    std::string pairing_req_id_;
    std::string device_nonce_;

    std::atomic<bool> wifi_connected_{false};
    std::atomic<int> state_{static_cast<int>(EzClientState::kOffline)};
    std::atomic<bool> tts_ready_{false};
    std::atomic<bool> asr_ready_{false};

    std::atomic<bool> ws_closing_{false};
    std::atomic<int> ws_in_flight_{0};
    std::atomic<bool> ws_connected_{false};
    std::atomic<bool> ws_ended_{false};
    std::atomic<bool> pending_stop_ws_{false};
    std::atomic<bool> pending_save_identity_{false};
    std::atomic<bool> pending_reset_pairing_{false};

#ifdef ESP_PLATFORM
    int64_t pairing_started_ms_ = 0;
    bool has_pending_connect_trigger_ = false;
    EzConnectTrigger pending_connect_trigger_;
    bool wifi_applied_ = false;
    int64_t reconnect_due_ms_ = 0;
    int reconnect_attempts_ = 0;
    int64_t passive_mdns_due_ms_ = 0;
#endif
    uint32_t msg_seq_ = 0;
    uint32_t stream_seq_ = 0;
};

} // namespace esp_pika
