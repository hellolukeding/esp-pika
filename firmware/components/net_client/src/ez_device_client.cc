#include "ez_device_client.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "ed25519_lite.h"
#include "gateway_protocol.h"

#ifdef ESP_PLATFORM
#include <arpa/inet.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mdns.h"
#endif

namespace esp_pika {
namespace {

void FillRandomBytes(uint8_t* out, size_t len) {
#ifdef ESP_PLATFORM
    esp_fill_random(out, len);
#else
    static uint64_t state = 0x9e3779b97f4a7c15ULL;
    for (size_t i = 0; i < len; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        out[i] = static_cast<uint8_t>(state & 0xffu);
    }
#endif
}

std::string RandomTokenB64Url(size_t num_bytes) {
    std::vector<uint8_t> raw(num_bytes, 0);
    FillRandomBytes(raw.data(), raw.size());
    char buf[128] = {};
    ez_base64url_encode(raw.data(), raw.size(), buf, sizeof(buf));
    return std::string(buf);
}

#ifdef ESP_PLATFORM
constexpr const char* kClientTag = "ez_device_client";
constexpr const char* kDeviceModel = "esp32s31-mosaico";
constexpr size_t kMaxRxTextBytes = 65536;
constexpr size_t kMaxRxBinBytes = 4096;
constexpr int64_t kPairingTimeoutMs = 30000;
constexpr int64_t kPassiveMdnsIntervalMs = 5000;

int64_t NowMsMonotonic() {
    return esp_timer_get_time() / 1000;
}

bool DecodeHexDigest32(const std::string& hex, uint8_t out[32]) {
    if (hex.size() != 64) {
        return false;
    }
    auto nibble = [](char c, uint8_t* v) -> bool {
        if (c >= '0' && c <= '9') {
            *v = static_cast<uint8_t>(c - '0');
            return true;
        }
        if (c >= 'a' && c <= 'f') {
            *v = static_cast<uint8_t>(10 + (c - 'a'));
            return true;
        }
        if (c >= 'A' && c <= 'F') {
            *v = static_cast<uint8_t>(10 + (c - 'A'));
            return true;
        }
        return false;
    };
    for (size_t i = 0; i < 32; ++i) {
        uint8_t hi = 0;
        uint8_t lo = 0;
        if (!nibble(hex[i * 2], &hi) || !nibble(hex[i * 2 + 1], &lo)) {
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

std::mutex g_pin_mu;
bool g_pin_valid = false;
uint8_t g_pin_sha256[32] = {};

bool SetPinnedCertHex(const std::string& hex_fp) {
    uint8_t digest[32] = {};
    if (!DecodeHexDigest32(hex_fp, digest)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_pin_mu);
    std::memcpy(g_pin_sha256, digest, sizeof(digest));
    g_pin_valid = true;
    return true;
}

int EzDummyCaCb(void* ctx, mbedtls_x509_crt const* child, mbedtls_x509_crt** candidate_cas) {
    (void)ctx;
    (void)child;
    if (candidate_cas != nullptr) {
        *candidate_cas = nullptr;
    }
    return 0;
}

int EzCertVerifyCb(void* data, mbedtls_x509_crt* crt, int depth, uint32_t* flags) {
    (void)data;
    if (flags == nullptr) {
        return MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
    }
    if (depth > 0) {
        *flags = 0;
        return 0;
    }
    if (crt == nullptr || crt->raw.p == nullptr || crt->raw.len == 0) {
        *flags |= MBEDTLS_X509_BADCERT_NOT_TRUSTED;
        return MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
    }

    uint8_t actual[32] = {};
    ez_sha256(crt->raw.p, crt->raw.len, actual);

    uint8_t expected[32] = {};
    bool pin_valid = false;
    {
        std::lock_guard<std::mutex> lock(g_pin_mu);
        pin_valid = g_pin_valid;
        std::memcpy(expected, g_pin_sha256, sizeof(expected));
    }
    if (!pin_valid) {
        ESP_LOGE(kClientTag, "TLS leaf cert received with no active pin");
        *flags |= MBEDTLS_X509_BADCERT_NOT_TRUSTED;
        return MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
    }

    uint8_t diff = 0;
    for (size_t i = 0; i < sizeof(actual); ++i) {
        diff |= static_cast<uint8_t>(actual[i] ^ expected[i]);
    }
    if (diff != 0) {
        ESP_LOGE(kClientTag, "TLS leaf cert SHA-256 fingerprint mismatch");
        *flags |= MBEDTLS_X509_BADCERT_NOT_TRUSTED;
        return MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
    }

    *flags = 0;
    return 0;
}

esp_err_t EzPinnedCertAttach(void* conf) {
    if (conf == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    auto* ssl_conf = static_cast<mbedtls_ssl_config*>(conf);
    mbedtls_ssl_conf_verify(ssl_conf, &EzCertVerifyCb, nullptr);
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY)
    mbedtls_ssl_conf_ca_cb(ssl_conf, &EzDummyCaCb, nullptr);
#else
    static const mbedtls_x509_crt kDummyCrt = {};
    mbedtls_ssl_conf_ca_chain(ssl_conf, const_cast<mbedtls_x509_crt*>(&kDummyCrt), nullptr);
#endif
    return ESP_OK;
}

std::string ExtractPeerIpv4(httpd_req_t* req) {
    if (req == nullptr) {
        return "";
    }
    const int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        return "";
    }
    struct sockaddr_storage addr = {};
    socklen_t len = sizeof(addr);
    if (lwip_getpeername(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) {
        return "";
    }
    if (addr.ss_family == AF_INET) {
        const auto* sin = reinterpret_cast<const struct sockaddr_in*>(&addr);
        char buf[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) != nullptr) {
            return std::string(buf);
        }
    }
#ifdef AF_INET6
    else if (addr.ss_family == AF_INET6) {
        const auto* sin6 = reinterpret_cast<const struct sockaddr_in6*>(&addr);
        const uint8_t* b = sin6->sin6_addr.s6_addr;
        static const uint8_t kV4Prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (std::memcmp(b, kV4Prefix, 12) == 0) {
            char buf[32] = {};
            std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
            return std::string(buf);
        }
    }
#endif
    return "";
}

bool LooksLikeIpv4Literal(const std::string& host) {
    struct in_addr tmp = {};
    return !host.empty() && inet_pton(AF_INET, host.c_str(), &tmp) == 1;
}

esp_err_t SendJsonResponse(httpd_req_t* req, const char* status, const std::string& body) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.data(), static_cast<ssize_t>(body.size()));
}
#endif

} // namespace

EzDeviceClient::~EzDeviceClient() {
#ifdef ESP_PLATFORM
    {
        std::lock_guard<std::mutex> ws_lock(ws_lifecycle_mu_);
        StopWebSocketLocked();
    }
    StopUnpairedDiscovery();
#endif
}

bool EzDeviceClient::Init(const EzDeviceClientConfig& cfg, EzDeviceCallbacks callbacks) {
    cfg_ = cfg;
    if (cfg_.display_name.empty()) {
        cfg_.display_name = "Mosaico";
    }
    if (cfg_.control_port == 0) {
        cfg_.control_port = 8765;
    }
    callbacks_ = std::move(callbacks);

    EzPairingIdentity loaded;
#ifdef ESP_PLATFORM
    if (!loaded.LoadFromNvs()) {
        ESP_LOGW(kClientTag, "ez_pair NVS load failed; initializing fresh identity");
        loaded = EzPairingIdentity{};
    }
#endif
    if (!loaded.has_seed) {
        FillRandomBytes(loaded.ed25519_seed, sizeof(loaded.ed25519_seed));
        ez_ed25519_public_key(loaded.ed25519_seed, loaded.ed25519_pub);
        loaded.has_seed = true;
        loaded.display_name = cfg_.display_name;
#ifdef ESP_PLATFORM
        loaded.SaveToNvs();
#endif
    } else if (loaded.display_name.empty()) {
        loaded.display_name = cfg_.display_name;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        identity_ = std::move(loaded);
    }

#ifdef ESP_PLATFORM
    ESP_LOGI(kClientTag, "initialized ez_pair paired=%d fp=%s name=%s",
             IsPaired() ? 1 : 0, KeyFingerprint().c_str(), DisplayName().c_str());
#endif
    SetState(EzClientState::kOffline);
    return true;
}

void EzDeviceClient::SetWifiConnected(bool connected) {
    wifi_connected_.store(connected, std::memory_order_release);
}

void EzDeviceClient::ResetPairing() {
    pending_reset_pairing_.store(true, std::memory_order_release);
}

bool EzDeviceClient::IsPaired() const {
    std::lock_guard<std::mutex> lock(mu_);
    return identity_.paired && !identity_.device_id.empty() && !identity_.cert_fingerprint.empty();
}

std::string EzDeviceClient::KeyFingerprint() const {
    std::lock_guard<std::mutex> lock(mu_);
    return identity_.PublicKeyFingerprint();
}

std::string EzDeviceClient::DisplayName() const {
    std::lock_guard<std::mutex> lock(mu_);
    return identity_.display_name.empty() ? cfg_.display_name : identity_.display_name;
}

std::string EzDeviceClient::PairedHost() const {
    std::lock_guard<std::mutex> lock(mu_);
    return identity_.host_address;
}

std::string EzDeviceClient::NextMessageId() {
    std::lock_guard<std::mutex> lock(mu_);
    const uint32_t seq = ++msg_seq_;
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "ez-msg-%lu", static_cast<unsigned long>(seq));
    return std::string(buf);
}

void EzDeviceClient::SetState(EzClientState next) {
    const int prev = state_.exchange(static_cast<int>(next), std::memory_order_acq_rel);
    if (prev != static_cast<int>(next) && callbacks_.on_state) {
        callbacks_.on_state(next);
    }
}

void EzDeviceClient::EnqueueJsonTx(std::string json_text) {
    if (json_text.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (pending_tx_json_.size() < 16) {
        pending_tx_json_.push_back(std::move(json_text));
    }
}

bool EzDeviceClient::SendJson(const std::string& json_text) {
#ifdef ESP_PLATFORM
    if (ws_closing_.load(std::memory_order_acquire)) {
        return false;
    }
    ws_in_flight_.fetch_add(1, std::memory_order_acq_rel);
    bool ok = false;
    {
        std::lock_guard<std::mutex> tx_lock(tx_mu_);
        esp_websocket_client_handle_t handle = ws_handle_;
        if (!ws_closing_.load(std::memory_order_acquire) && handle != nullptr &&
            esp_websocket_client_is_connected(handle)) {
            const int sent = esp_websocket_client_send_text(
                handle, json_text.data(), static_cast<int>(json_text.size()), pdMS_TO_TICKS(1000));
            ok = (sent == static_cast<int>(json_text.size()));
        }
    }
    ws_in_flight_.fetch_sub(1, std::memory_order_acq_rel);
    return ok;
#else
    (void)json_text;
    return false;
#endif
}

bool EzDeviceClient::SendBinary(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return false;
    }
#ifdef ESP_PLATFORM
    if (ws_closing_.load(std::memory_order_acquire)) {
        return false;
    }
    ws_in_flight_.fetch_add(1, std::memory_order_acq_rel);
    bool ok = false;
    {
        std::lock_guard<std::mutex> tx_lock(tx_mu_);
        esp_websocket_client_handle_t handle = ws_handle_;
        if (!ws_closing_.load(std::memory_order_acquire) && handle != nullptr &&
            esp_websocket_client_is_connected(handle)) {
            const int sent = esp_websocket_client_send_bin(
                handle, reinterpret_cast<const char*>(data), static_cast<int>(len),
                pdMS_TO_TICKS(500));
            ok = (sent == static_cast<int>(len));
        }
    }
    ws_in_flight_.fetch_sub(1, std::memory_order_acq_rel);
    return ok;
#else
    return false;
#endif
}

bool EzDeviceClient::StartListen(uint32_t* out_stream_id) {
    return StartListenWithContext(out_stream_id, "wake_word", "", 0, 0.0f);
}

bool EzDeviceClient::StartListenWithContext(uint32_t* out_stream_id,
                                            const std::string& wake_source,
                                            const std::string& speaker_id,
                                            int speaker_role,
                                            float speaker_score) {
    if (out_stream_id == nullptr || !IsOnline()) {
        return false;
    }
    if (!asr_ready_.load(std::memory_order_acquire)) {
        return false;
    }
    uint32_t sid = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        sid = ++stream_seq_;
        if (sid == 0) {
            sid = ++stream_seq_;
        }
    }
    const std::string input_id = "mosaico-in-" + std::to_string(sid) + "-" + RandomTokenB64Url(6);
    const EzOutputPreference pref =
        tts_ready_.load(std::memory_order_acquire) ? EzOutputPreference::kTextAndAudio
                                                   : EzOutputPreference::kText;
    if (!SendJson(EzBuildListenStartJson(NextMessageId(), input_id, sid, pref, wake_source,
                                         speaker_id, speaker_role, speaker_score))) {
        return false;
    }
    *out_stream_id = sid;
    return true;
}

bool EzDeviceClient::SendUplinkPcmFrame(uint32_t stream_id,
                                        uint32_t sequence,
                                        const uint8_t pcm640[640]) {
    if (!IsOnline()) {
        return false;
    }
    uint8_t frame[656] = {};
    if (!EzEncodeUplinkPcmFrame(stream_id, sequence, pcm640, frame)) {
        return false;
    }
    return SendBinary(frame, sizeof(frame));
}

bool EzDeviceClient::StopListen(uint32_t stream_id, uint32_t last_sequence) {
    if (stream_id == 0 || !IsOnline()) {
        return false;
    }
    return SendJson(EzBuildListenStopJson(NextMessageId(), stream_id, last_sequence));
}

bool EzDeviceClient::CancelListen(uint32_t stream_id) {
    if (stream_id == 0 || !IsOnline()) {
        return false;
    }
    return SendJson(EzBuildListenCancelJson(NextMessageId(), stream_id));
}

bool EzDeviceClient::CancelPlayback(const std::string& output_id, uint32_t stream_id) {
    if (output_id.empty() || stream_id == 0 || !IsOnline()) {
        return false;
    }
    return SendJson(EzBuildPlaybackCancelJson(NextMessageId(), output_id, stream_id));
}

bool EzDeviceClient::SendDeviceCommandResult(const std::string& command_id,
                                             bool ok,
                                             const std::string& result_json,
                                             const std::string& media_url,
                                             const std::string& error_code,
                                             const std::string& error_message) {
    if (command_id.empty() || !IsOnline()) {
        return false;
    }
    return SendJson(EzBuildDeviceCommandResultJson(NextMessageId(), command_id, ok, result_json,
                                                   media_url, error_code, error_message));
}

bool EzDeviceClient::SendVoiceprintSyncAck(uint64_t vp_version,
                                           uint32_t count,
                                           bool ok,
                                           const std::string& error) {
    if (!IsOnline()) {
        return false;
    }
    return SendJson(EzBuildVoiceprintSyncAckJson(NextMessageId(), vp_version, count, ok, error));
}

bool EzDeviceClient::SendWakeConfigAck(const std::string& sync_id,
                                       bool ok,
                                       const EzWakeConfig* active_config,
                                       const std::string& error) {
    if (!IsOnline()) {
        return false;
    }
    return SendJson(EzBuildWakeConfigAckJson(NextMessageId(), sync_id, ok, active_config, error));
}

void EzDeviceClient::SetVoiceprintVersion(uint64_t vp_version) {
    std::lock_guard<std::mutex> lock(mu_);
    cfg_.hardware.vp_version = vp_version;
}

void EzDeviceClient::SetWakeConfig(const EzWakeConfig& wake_config, bool multinet_ready) {
    std::lock_guard<std::mutex> lock(mu_);
    cfg_.hardware.wake_config = wake_config;
    cfg_.hardware.multinet_ready = multinet_ready;
}

void EzDeviceClient::HandleInboundEnvelope(const std::string& json_text) {
    EzInboundMessage msg;
    if (!EzParseInboundMessage(json_text, &msg)) {
#ifdef ESP_PLATFORM
        ESP_LOGW(kClientTag, "failed to parse Device Protocol 1.0 JSON");
#endif
        return;
    }

    switch (msg.type) {
    case EzInboundType::kAuthChallenge: {
        bool is_pairing = false;
        uint8_t seed[32] = {};
        uint8_t pub[32] = {};
        std::string pub_b64;
        std::string display_name;
        std::string device_id;
        std::string req_id;
        std::string dev_nonce = RandomTokenB64Url(24);
        EzCapabilities caps;
        EzHardwareProfile hw_snapshot;
        {
            std::lock_guard<std::mutex> lock(mu_);
            is_pairing = ws_pairing_mode_;
            caps = caps_;
            hw_snapshot = cfg_.hardware;
            device_nonce_ = dev_nonce;
            std::memcpy(seed, identity_.ed25519_seed, sizeof(seed));
            std::memcpy(pub, identity_.ed25519_pub, sizeof(pub));
            pub_b64 = identity_.PublicKeyBase64Url();
            display_name = identity_.display_name.empty() ? cfg_.display_name : identity_.display_name;
            device_id = identity_.device_id;
            if (is_pairing) {
                pairing_req_id_ = "pair-" + RandomTokenB64Url(12);
                req_id = pairing_req_id_;
            }
        }
        if (is_pairing) {
            EnqueueJsonTx(EzBuildPairingHelloJson(NextMessageId(), req_id, display_name, dev_nonce,
                                                  caps, pub_b64, cfg_.firmware_version));
        } else {
            const std::string sig = EzComputeAuthSignature(
                seed, pub, msg.connection_id, msg.host_nonce, device_id, dev_nonce, caps,
                EzOutputPreference::kText);
            EnqueueJsonTx(EzBuildAuthHelloJson(NextMessageId(), device_id, dev_nonce, caps,
                                               EzOutputPreference::kText, cfg_.firmware_version,
                                               sig, hw_snapshot.enabled ? &hw_snapshot : nullptr));
        }
        break;
    }
    case EzInboundType::kPairingPending:
        break;
    case EzInboundType::kPairingDirectChallenge: {
        std::string sig;
        bool valid = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (ws_pairing_mode_ && !pairing_req_id_.empty() &&
                msg.pairing_request_id == pairing_req_id_) {
                sig = EzComputePairingCommitSignature(
                    identity_.ed25519_seed, identity_.ed25519_pub, pairing_req_id_,
                    pairing_target_.installation_id, pairing_target_.certificate_fingerprint,
                    device_nonce_, msg.host_nonce, caps_, msg.device_id);
                valid = !sig.empty();
            }
        }
        if (valid) {
            EnqueueJsonTx(EzBuildPairingDirectCommitJson(NextMessageId(), msg.pairing_request_id,
                                                         msg.device_id, sig));
        }
        break;
    }
    case EzInboundType::kPairingComplete: {
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (ws_pairing_mode_ &&
                (msg.pairing_request_id.empty() || msg.pairing_request_id == pairing_req_id_)) {
                identity_.paired = true;
                identity_.device_id = msg.device_id;
                if (!msg.display_name.empty()) {
                    identity_.display_name = msg.display_name;
                }
                identity_.installation_id = pairing_target_.installation_id;
                identity_.cert_fingerprint = pairing_target_.certificate_fingerprint;
                identity_.host_address = pairing_target_.host;
                identity_.host_port = pairing_target_.port;
                identity_.host_path =
                    pairing_target_.path.empty() ? "/device" : pairing_target_.path;
                accepted = true;
            }
        }
        if (accepted) {
            pending_save_identity_.store(true, std::memory_order_release);
            pending_stop_ws_.store(true, std::memory_order_release);
        }
        break;
    }
    case EzInboundType::kHelloAck: {
        asr_ready_.store(msg.capabilities.input_pcm16_16k_mono, std::memory_order_release);
        const bool tts_ok = msg.capabilities.output_pcm16_16k_mono;
        tts_ready_.store(tts_ok, std::memory_order_release);
        SetState(EzClientState::kOnline);
        if (tts_ok && msg.output_preference != EzOutputPreference::kTextAndAudio) {
            EnqueueJsonTx(EzBuildSetOutputPreferenceJson(NextMessageId(),
                                                         EzOutputPreference::kTextAndAudio));
        }
        break;
    }
    case EzInboundType::kOutputPreferenceChanged:
        break;
    case EzInboundType::kStateChanged:
        if (callbacks_.on_interaction_state) {
            callbacks_.on_interaction_state(msg.state, msg.reason);
        }
        break;
    case EzInboundType::kTranscript:
        if (callbacks_.on_transcript) {
            callbacks_.on_transcript(msg.text);
        }
        break;
    case EzInboundType::kTextOutput:
        if (callbacks_.on_text_output) {
            callbacks_.on_text_output(msg.output_id, msg.text);
        }
        break;
    case EzInboundType::kPlaybackStart:
        if (callbacks_.on_playback_start) {
            callbacks_.on_playback_start(msg.output_id, msg.stream_id, msg.text);
        }
        break;
    case EzInboundType::kPlaybackEnd:
        if (callbacks_.on_playback_end) {
            callbacks_.on_playback_end(msg.output_id, msg.stream_id, msg.reason);
        }
        break;
    case EzInboundType::kSpeechStatusChanged: {
        const bool was_tts = tts_ready_.load(std::memory_order_acquire);
        asr_ready_.store(msg.speech_asr_ready, std::memory_order_release);
        tts_ready_.store(msg.speech_tts_ready, std::memory_order_release);
        if (!was_tts && msg.speech_tts_ready && IsOnline()) {
            EnqueueJsonTx(EzBuildSetOutputPreferenceJson(NextMessageId(),
                                                         EzOutputPreference::kTextAndAudio));
        }
        break;
    }
    case EzInboundType::kDeviceCommand: {
        std::lock_guard<std::mutex> lock(mu_);
        if (pending_commands_.size() < 8) {
            pending_commands_.push_back(std::move(msg.device_command));
        }
        break;
    }
    case EzInboundType::kVoiceprintSync: {
        std::lock_guard<std::mutex> lock(mu_);
        if (pending_vp_syncs_.size() < 4) {
            pending_vp_syncs_.push_back(std::move(msg.raw_payload));
        } else if (!pending_vp_syncs_.empty()) {
            pending_vp_syncs_.back() = std::move(msg.raw_payload);
        }
        break;
    }
    case EzInboundType::kWakeConfigSync: {
        std::lock_guard<std::mutex> lock(mu_);
        if (pending_wake_syncs_.size() < 4) {
            pending_wake_syncs_.push_back(std::move(msg.wake_config_sync));
        } else if (!pending_wake_syncs_.empty()) {
            pending_wake_syncs_.back() = std::move(msg.wake_config_sync);
        }
        break;
    }
    case EzInboundType::kPing:
        EnqueueJsonTx(EzBuildPongJson(NextMessageId(), msg.ping_nonce, msg.ping_sent_at_ms));
        break;
    case EzInboundType::kError:
#ifdef ESP_PLATFORM
        ESP_LOGW(kClientTag, "wire error code=%s recoverable=%d",
                 msg.error_code.c_str(), msg.recoverable ? 1 : 0);
#endif
        if (msg.error_code == "device_revoked" || msg.error_code == "authentication_failed") {
            if (msg.error_code == "device_revoked") {
                pending_reset_pairing_.store(true, std::memory_order_release);
            }
        }
        break;
    default:
        break;
    }
}

void EzDeviceClient::HandleInboundBinary(const uint8_t* data, size_t len) {
    uint32_t stream_id = 0;
    uint32_t sequence = 0;
    const uint8_t* pcm = nullptr;
    size_t pcm_len = 0;
    if (EzDecodeDownlinkPcmFrame(data, len, &stream_id, &sequence, &pcm, &pcm_len) &&
        callbacks_.on_downlink_pcm) {
        callbacks_.on_downlink_pcm(stream_id, sequence, pcm, pcm_len);
    }
}

#ifdef ESP_PLATFORM
void EzDeviceClient::EnsureUnpairedDiscoveryStarted() {
    if (!mdns_service_registered_) {
        std::string fp = KeyFingerprint();
        std::string name = DisplayName();
        mdns_txt_item_t txt[6] = {
            {"paired", "0"},
            {"pk_fp", fp.c_str()},
            {"name", name.c_str()},
            {"model", kDeviceModel},
            {"fw", cfg_.firmware_version.c_str()},
            {"mac", cfg_.mac.c_str()},
        };
        mdns_service_remove("_ez-device", "_tcp");
        const esp_err_t err = mdns_service_add(name.c_str(), "_ez-device", "_tcp",
                                               cfg_.control_port, txt, 6);
        if (err == ESP_OK) {
            mdns_service_registered_ = true;
            ESP_LOGI(kClientTag, "advertising _ez-device._tcp port=%u fp=%s",
                     cfg_.control_port, fp.c_str());
        } else {
            ESP_LOGW(kClientTag, "mdns_service_add(_ez-device._tcp) failed: %s",
                     esp_err_to_name(err));
        }
    }

    if (httpd_ == nullptr) {
        httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
        http_cfg.server_port = cfg_.control_port;
        http_cfg.ctrl_port = 32769;
        http_cfg.core_id = 1;
        http_cfg.stack_size = 6144;
        http_cfg.max_uri_handlers = 4;
        http_cfg.lru_purge_enable = true;

        if (httpd_start(&httpd_, &http_cfg) == ESP_OK) {
            const httpd_uri_t info_uri = {
                .uri = "/ez/info",
                .method = HTTP_GET,
                .handler = &EzDeviceClient::HandleHttpInfo,
                .user_ctx = this,
            };
            const httpd_uri_t connect_uri = {
                .uri = "/ez/connect",
                .method = HTTP_POST,
                .handler = &EzDeviceClient::HandleHttpConnect,
                .user_ctx = this,
            };
            httpd_register_uri_handler(httpd_, &info_uri);
            httpd_register_uri_handler(httpd_, &connect_uri);
            ESP_LOGI(kClientTag, "HTTP discovery nudge listener active on :%u", cfg_.control_port);
        } else {
            ESP_LOGW(kClientTag, "httpd_start(:%u) failed", cfg_.control_port);
            httpd_ = nullptr;
        }
    }
}

void EzDeviceClient::StopUnpairedDiscovery() {
    if (mdns_service_registered_) {
        mdns_service_remove("_ez-device", "_tcp");
        mdns_service_registered_ = false;
    }
    if (httpd_ != nullptr) {
        httpd_stop(httpd_);
        httpd_ = nullptr;
    }
}

esp_err_t EzDeviceClient::HandleHttpInfo(httpd_req_t* req) {
    auto* self = static_cast<EzDeviceClient*>(req->user_ctx);
    const std::string name = self->DisplayName();
    const std::string fp = self->KeyFingerprint();
    const bool paired = self->IsPaired();

    std::string body = "{";
    body += "\"name\":\"" + EscapeJsonString(name) + "\"";
    body += ",\"model\":\"" + EscapeJsonString(kDeviceModel) + "\"";
    body += ",\"fw\":\"" + EscapeJsonString(self->cfg_.firmware_version) + "\"";
    body += ",\"mac\":\"" + EscapeJsonString(self->cfg_.mac) + "\"";
    body += ",\"pk_fp\":\"" + EscapeJsonString(fp) + "\"";
    body += std::string(",\"paired\":") + (paired ? "true" : "false");
    body += "}";
    return SendJsonResponse(req, "200 OK", body);
}

esp_err_t EzDeviceClient::HandleHttpConnect(httpd_req_t* req) {
    auto* self = static_cast<EzDeviceClient*>(req->user_ctx);
    if (self->IsPaired()) {
        return SendJsonResponse(req, "409 Conflict",
                                "{\"ok\":false,\"error\":\"already_paired\"}");
    }

    if (req->content_len <= 0 || req->content_len > 1024) {
        return SendJsonResponse(req, "400 Bad Request",
                                "{\"ok\":false,\"error\":\"invalid_length\"}");
    }
    std::string body(static_cast<size_t>(req->content_len), '\0');
    size_t received = 0;
    while (received < body.size()) {
        const int r = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return SendJsonResponse(req, "400 Bad Request",
                                    "{\"ok\":false,\"error\":\"read_failed\"}");
        }
        received += static_cast<size_t>(r);
    }

    EzConnectTrigger trigger;
    if (!EzParseConnectTriggerJson(body, &trigger)) {
        return SendJsonResponse(req, "400 Bad Request",
                                "{\"ok\":false,\"error\":\"invalid_payload\"}");
    }

    const std::string peer_ip = ExtractPeerIpv4(req);
    if (trigger.host.empty() || !LooksLikeIpv4Literal(trigger.host)) {
        if (peer_ip.empty()) {
            return SendJsonResponse(req, "400 Bad Request",
                                    "{\"ok\":false,\"error\":\"missing_host\"}");
        }
        trigger.host = peer_ip;
    }

    ESP_LOGI(kClientTag, "received HTTP /ez/connect trigger host=%s:%u install=%s",
             trigger.host.c_str(), trigger.port, trigger.installation_id.c_str());

    {
        std::lock_guard<std::mutex> lock(self->mu_);
        self->pending_connect_trigger_ = std::move(trigger);
        self->has_pending_connect_trigger_ = true;
    }
    return SendJsonResponse(req, "202 Accepted", "{\"ok\":true}");
}

bool EzDeviceClient::QueryEzAssistantMdns(const std::string& filter_installation_id,
                                          bool require_pairing_open,
                                          EzConnectTrigger* out) {
    if (out == nullptr) {
        return false;
    }
    mdns_result_t* results = nullptr;
    esp_err_t err = mdns_query_ptr("_ez-assistant", "_tcp", 1200, 8, &results);
    if (err != ESP_OK || results == nullptr) {
        if (results != nullptr) {
            mdns_query_results_free(results);
        }
        return false;
    }

    bool found = false;
    for (mdns_result_t* r = results; r != nullptr; r = r->next) {
        if (r->port == 0) {
            continue;
        }
        std::string ipv4;
        for (mdns_ip_addr_t* a = r->addr; a != nullptr; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                char ip_buf[16] = {};
                esp_ip4addr_ntoa(&a->addr.u_addr.ip4, ip_buf, sizeof(ip_buf));
                if (ip_buf[0] != '\0') {
                    ipv4 = ip_buf;
                    break;
                }
            }
        }
        if (ipv4.empty()) {
            continue;
        }

        std::string install_id;
        std::string cert_fp;
        std::string path = "/device";
        std::string pairing_available;
        for (size_t i = 0; i < r->txt_count; ++i) {
            if (r->txt[i].key == nullptr) {
                continue;
            }
            const std::string key = r->txt[i].key;
            const std::string val = (r->txt[i].value != nullptr) ? r->txt[i].value : "";
            if (key == "installation_id") {
                install_id = val;
            } else if (key == "certificate_fingerprint") {
                cert_fp = val;
            } else if (key == "path" && !val.empty()) {
                path = (val[0] == '/') ? val : ("/" + val);
            } else if (key == "pairing_available") {
                pairing_available = val;
            }
        }

        std::string norm_install;
        std::string norm_fp;
        if (!EzIsValidHexLower(install_id, 32, &norm_install) ||
            !EzIsValidHexLower(cert_fp, 64, &norm_fp)) {
            continue;
        }
        if (!filter_installation_id.empty() && norm_install != filter_installation_id) {
            continue;
        }
        if (require_pairing_open && pairing_available != "true") {
            continue;
        }

        out->host = std::move(ipv4);
        out->port = r->port;
        out->path = std::move(path);
        out->installation_id = std::move(norm_install);
        out->certificate_fingerprint = std::move(norm_fp);
        found = true;
        break;
    }
    mdns_query_results_free(results);
    return found;
}

bool EzDeviceClient::StartWebSocketLocked(const std::string& host,
                                          uint16_t port,
                                          const std::string& path,
                                          const std::string& cert_fingerprint_hex,
                                          bool pairing_mode) {
    StopWebSocketLocked();
    if (host.empty() || port == 0 || !SetPinnedCertHex(cert_fingerprint_hex)) {
        return false;
    }
    const std::string norm_path = path.empty() ? "/device" : ((path[0] == '/') ? path : ("/" + path));
    char uri_buf[256] = {};
    std::snprintf(uri_buf, sizeof(uri_buf), "wss://%s:%u%s",
                  host.c_str(), port, norm_path.c_str());

    {
        std::lock_guard<std::mutex> lock(mu_);
        ws_pairing_mode_ = pairing_mode;
        pairing_req_id_.clear();
        device_nonce_.clear();
        pending_tx_json_.clear();
        if (pairing_mode) {
            pairing_started_ms_ = NowMsMonotonic();
        }
    }
    rx_text_buf_.clear();
    rx_bin_buf_.clear();
    ws_connected_.store(false, std::memory_order_release);
    ws_ended_.store(false, std::memory_order_release);
    asr_ready_.store(false, std::memory_order_release);
    tts_ready_.store(false, std::memory_order_release);

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = uri_buf;
    ws_cfg.transport = WEBSOCKET_TRANSPORT_OVER_SSL;
    ws_cfg.crt_bundle_attach = &EzPinnedCertAttach;
    ws_cfg.skip_cert_common_name_check = true;
    ws_cfg.disable_auto_reconnect = true;
    ws_cfg.task_core_id_set = true;
    ws_cfg.task_core_id = 1;
    ws_cfg.task_prio = 6;
    ws_cfg.task_stack = 8192;
    ws_cfg.buffer_size = 4096;
    ws_cfg.network_timeout_ms = 4000;

    ws_handle_ = esp_websocket_client_init(&ws_cfg);
    if (ws_handle_ == nullptr) {
        return false;
    }
    esp_websocket_register_events(ws_handle_, WEBSOCKET_EVENT_ANY,
                                  &EzDeviceClient::WebSocketEventHandler, this);
    if (esp_websocket_client_start(ws_handle_) != ESP_OK) {
        ESP_LOGW(kClientTag, "esp_websocket_client_start failed (free_int=%u largest_int=%u)",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        esp_websocket_client_destroy(ws_handle_);
        ws_handle_ = nullptr;
        return false;
    }
    ESP_LOGI(kClientTag, "connecting WSS %s (pairing=%d)", uri_buf, pairing_mode ? 1 : 0);
    return true;
}

void EzDeviceClient::StopWebSocketLocked() {
    ws_closing_.store(true, std::memory_order_release);
    ws_connected_.store(false, std::memory_order_release);
    asr_ready_.store(false, std::memory_order_release);
    tts_ready_.store(false, std::memory_order_release);
    for (int i = 0; i < 100 && ws_in_flight_.load(std::memory_order_acquire) > 0; ++i) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    esp_websocket_client_handle_t handle = ws_handle_;
    ws_handle_ = nullptr;
    if (handle != nullptr) {
        esp_websocket_client_stop(handle);
        esp_websocket_client_destroy(handle);
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        pending_tx_json_.clear();
    }
    ws_closing_.store(false, std::memory_order_release);
}

void EzDeviceClient::WebSocketEventHandler(void* arg,
                                           esp_event_base_t base,
                                           int32_t id,
                                           void* data) {
    (void)base;
    auto* self = static_cast<EzDeviceClient*>(arg);
    auto* evt = static_cast<esp_websocket_event_data_t*>(data);

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(kClientTag, "WSS connected");
        self->ws_connected_.store(true, std::memory_order_release);
        self->ws_ended_.store(false, std::memory_order_release);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(kClientTag, "WSS error type=%d tls_esp=0x%x cert_flags=0x%x http=%d",
                 evt != nullptr ? static_cast<int>(evt->error_handle.error_type) : -1,
                 evt != nullptr ? static_cast<unsigned>(evt->error_handle.esp_tls_last_esp_err) : 0u,
                 evt != nullptr ? static_cast<unsigned>(evt->error_handle.esp_tls_cert_verify_flags) : 0u,
                 evt != nullptr ? evt->error_handle.esp_ws_handshake_status_code : -1);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(kClientTag, "WSS ended id=%ld close_code=%d",
                 static_cast<long>(id),
                 evt != nullptr ? evt->close_status_code : 0);
        self->ws_connected_.store(false, std::memory_order_release);
        self->asr_ready_.store(false, std::memory_order_release);
        self->tts_ready_.store(false, std::memory_order_release);
        self->ws_ended_.store(true, std::memory_order_release);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (evt == nullptr) {
            break;
        }
        if (evt->op_code == 0x01 || (evt->op_code == 0x00 && !self->rx_text_buf_.empty())) {
            if (evt->op_code == 0x01 && evt->payload_offset == 0) {
                self->rx_text_buf_.clear();
            }
            if (evt->data_len > 0 && self->rx_text_buf_.size() < kMaxRxTextBytes) {
                const size_t room = kMaxRxTextBytes - self->rx_text_buf_.size();
                self->rx_text_buf_.append(
                    evt->data_ptr, std::min<size_t>(static_cast<size_t>(evt->data_len), room));
            }
            const bool complete =
                evt->fin &&
                (evt->payload_len <= 0 || evt->payload_offset + evt->data_len >= evt->payload_len);
            if (complete && !self->rx_text_buf_.empty()) {
                std::string json_msg = std::move(self->rx_text_buf_);
                self->rx_text_buf_.clear();
                self->HandleInboundEnvelope(json_msg);
            }
        } else if (evt->op_code == 0x02 || (evt->op_code == 0x00 && !self->rx_bin_buf_.empty())) {
            if (evt->op_code == 0x02 && evt->payload_offset == 0) {
                self->rx_bin_buf_.clear();
            }
            if (evt->data_len > 0 && self->rx_bin_buf_.size() < kMaxRxBinBytes) {
                const size_t room = kMaxRxBinBytes - self->rx_bin_buf_.size();
                const size_t copy_n = std::min<size_t>(static_cast<size_t>(evt->data_len), room);
                const auto* ptr = reinterpret_cast<const uint8_t*>(evt->data_ptr);
                self->rx_bin_buf_.insert(self->rx_bin_buf_.end(), ptr, ptr + copy_n);
            }
            const bool complete =
                evt->fin &&
                (evt->payload_len <= 0 || evt->payload_offset + evt->data_len >= evt->payload_len);
            if (complete && !self->rx_bin_buf_.empty()) {
                std::vector<uint8_t> bin_msg = std::move(self->rx_bin_buf_);
                self->rx_bin_buf_.clear();
                self->HandleInboundBinary(bin_msg.data(), bin_msg.size());
            }
        }
        break;
    default:
        break;
    }
}
#endif // ESP_PLATFORM

void EzDeviceClient::TickFast(uint32_t dt_ms) {
    (void)dt_ms;
    // Drain protocol TX replies (auth hello, pong, output preference) on Core 1's internal-DRAM
    // stack so WebSocketEventHandler never re-enters esp_websocket_client_send_* while holding
    // esp_websocket_client's internal lock (eliminates AB-BA deadlock with tx_mu_).
    {
        std::vector<std::string> tx_json;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!pending_tx_json_.empty()) {
                tx_json.swap(pending_tx_json_);
            }
        }
        for (const auto& payload : tx_json) {
            if (!SendJson(payload)) {
                break;
            }
        }
    }
    // Drain inbound WSS hardware commands, voiceprint syncs, and wake config syncs on Core 1's
    // internal-DRAM stack so NVS/Flash writes and HAL calls stay safe (AGENTS.md §3.1).
    {
        std::vector<EzDeviceCommandRequest> cmds;
        std::vector<std::string> vp_syncs;
        std::vector<EzWakeConfigSyncRequest> wake_syncs;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!pending_commands_.empty()) {
                cmds.swap(pending_commands_);
            }
            if (!pending_vp_syncs_.empty()) {
                vp_syncs.swap(pending_vp_syncs_);
            }
            if (!pending_wake_syncs_.empty()) {
                wake_syncs.swap(pending_wake_syncs_);
            }
        }
        if (callbacks_.on_device_command) {
            for (const auto& cmd : cmds) {
                callbacks_.on_device_command(cmd);
            }
        }
        if (callbacks_.on_voiceprint_sync) {
            for (const auto& raw_sync : vp_syncs) {
                callbacks_.on_voiceprint_sync(raw_sync);
            }
        }
        if (callbacks_.on_wake_config_sync) {
            for (const auto& ws : wake_syncs) {
                callbacks_.on_wake_config_sync(ws);
            }
        }
    }
#ifndef ESP_PLATFORM
    if (pending_reset_pairing_.exchange(false, std::memory_order_acq_rel)) {
        std::lock_guard<std::mutex> lock(mu_);
        identity_.ClearPairing();
        SetState(wifi_connected_.load(std::memory_order_acquire) ? EzClientState::kUnpairedReady
                                                                 : EzClientState::kOffline);
    }
#else
    const int64_t now = NowMsMonotonic();

    // 1. Handle unpair / revocation cleanly on Core 1 internal-DRAM stack.
    if (pending_reset_pairing_.exchange(false, std::memory_order_acq_rel)) {
        ESP_LOGW(kClientTag, "clearing pairing identity");
        {
            std::lock_guard<std::mutex> ws_lock(ws_lifecycle_mu_);
            StopWebSocketLocked();
        }
        EzPairingIdentity snapshot;
        {
            std::lock_guard<std::mutex> lock(mu_);
            identity_.ClearPairing();
            has_pending_connect_trigger_ = false;
            ws_pairing_mode_ = false;
            pairing_req_id_.clear();
            snapshot = identity_;
        }
        snapshot.SaveToNvs();
        pending_save_identity_.store(false, std::memory_order_release);
        pending_stop_ws_.store(false, std::memory_order_release);
        ws_ended_.store(false, std::memory_order_release);
        reconnect_attempts_ = 0;
        reconnect_due_ms_ = 0;
        passive_mdns_due_ms_ = now + 500;
    }

    // 2. Persist newly completed pairing identity & immediately transition to authenticated WSS.
    if (pending_save_identity_.exchange(false, std::memory_order_acq_rel)) {
        EzPairingIdentity snapshot;
        {
            std::lock_guard<std::mutex> lock(mu_);
            snapshot = identity_;
        }
        snapshot.SaveToNvs();
        StopUnpairedDiscovery();
        {
            std::lock_guard<std::mutex> ws_lock(ws_lifecycle_mu_);
            StopWebSocketLocked();
        }
        pending_stop_ws_.store(false, std::memory_order_release);
        ws_ended_.store(false, std::memory_order_release);
        reconnect_attempts_ = 0;
        reconnect_due_ms_ = 0;
        ESP_LOGI(kClientTag, "paired successfully as device_id=%s -> %s:%u",
                 snapshot.device_id.c_str(), snapshot.host_address.c_str(), snapshot.host_port);
    }

    if (pending_stop_ws_.exchange(false, std::memory_order_acq_rel)) {
        std::lock_guard<std::mutex> ws_lock(ws_lifecycle_mu_);
        StopWebSocketLocked();
        ws_ended_.store(false, std::memory_order_release);
    }

    // 3. Apply Wi-Fi STA transitions.
    const bool wifi_up = wifi_connected_.load(std::memory_order_acquire);
    if (wifi_up != wifi_applied_) {
        wifi_applied_ = wifi_up;
        if (!wifi_up) {
            {
                std::lock_guard<std::mutex> ws_lock(ws_lifecycle_mu_);
                StopWebSocketLocked();
            }
            StopUnpairedDiscovery();
            {
                std::lock_guard<std::mutex> lock(mu_);
                has_pending_connect_trigger_ = false;
                ws_pairing_mode_ = false;
            }
            ws_ended_.store(false, std::memory_order_release);
            SetState(EzClientState::kOffline);
            return;
        }
        reconnect_attempts_ = 0;
        reconnect_due_ms_ = 0;
        passive_mdns_due_ms_ = now + 500;
    }

    if (!wifi_up) {
        return;
    }

    // 4. Branch by paired state.
    if (IsPaired()) {
        StopUnpairedDiscovery();

        if (IsOnline()) {
            reconnect_attempts_ = 0;
        }

        if (ws_handle_ != nullptr && ws_ended_.exchange(false, std::memory_order_acq_rel)) {
            std::lock_guard<std::mutex> ws_lock(ws_lifecycle_mu_);
            StopWebSocketLocked();
            SetState(EzClientState::kConnecting);
            const int shift = std::min(reconnect_attempts_, 3);
            const int64_t delay_ms = std::min<int64_t>(10000, 1000LL << shift);
            reconnect_due_ms_ = now + delay_ms;
            ++reconnect_attempts_;
            return;
        }

        if (ws_handle_ == nullptr && now >= reconnect_due_ms_) {
            EzPairingIdentity id_copy;
            {
                std::lock_guard<std::mutex> lock(mu_);
                id_copy = identity_;
            }
            // Refresh ephemeral port / host IP from _ez-assistant._tcp on boot or alternate retries.
            if (reconnect_attempts_ == 0 || (reconnect_attempts_ % 2) == 1 ||
                id_copy.host_address.empty() || id_copy.host_port == 0) {
                EzConnectTrigger resolved;
                if (QueryEzAssistantMdns(id_copy.installation_id, false, &resolved)) {
                    const bool changed = (resolved.host != id_copy.host_address ||
                                          resolved.port != id_copy.host_port ||
                                          resolved.path != id_copy.host_path);
                    if (changed) {
                        id_copy.host_address = resolved.host;
                        id_copy.host_port = resolved.port;
                        id_copy.host_path = resolved.path;
                        {
                            std::lock_guard<std::mutex> lock(mu_);
                            identity_.host_address = resolved.host;
                            identity_.host_port = resolved.port;
                            identity_.host_path = resolved.path;
                        }
                        id_copy.SaveToNvs();
                        ESP_LOGI(kClientTag, "updated EZ Assistant endpoint via mDNS: %s:%u",
                                 id_copy.host_address.c_str(), id_copy.host_port);
                    }
                }
            }

            SetState(EzClientState::kConnecting);
            if (!id_copy.host_address.empty() && id_copy.host_port > 0) {
                std::lock_guard<std::mutex> ws_lock(ws_lifecycle_mu_);
                if (!StartWebSocketLocked(id_copy.host_address, id_copy.host_port,
                                          id_copy.host_path, id_copy.cert_fingerprint, false)) {
                    const int shift = std::min(reconnect_attempts_, 3);
                    reconnect_due_ms_ = now + std::min<int64_t>(10000, 1000LL << shift);
                    ++reconnect_attempts_;
                }
            } else {
                reconnect_due_ms_ = now + 2000;
                ++reconnect_attempts_;
            }
        }
        return;
    }

    // 5. Unpaired state: advertise _ez-device._tcp + HTTP :8765, handle direct pairing triggers.
    EnsureUnpairedDiscoveryStarted();

    if (State() == EzClientState::kPairing) {
        int64_t started_ms = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            started_ms = pairing_started_ms_;
        }
        if (ws_ended_.exchange(false, std::memory_order_acq_rel) ||
            (started_ms > 0 && now - started_ms >= kPairingTimeoutMs)) {
            ESP_LOGW(kClientTag, "pairing attempt ended or timed out");
            std::lock_guard<std::mutex> ws_lock(ws_lifecycle_mu_);
            StopWebSocketLocked();
            {
                std::lock_guard<std::mutex> lock(mu_);
                ws_pairing_mode_ = false;
            }
            SetState(EzClientState::kUnpairedReady);
        }
    }

    if (ws_handle_ == nullptr) {
        SetState(EzClientState::kUnpairedReady);

        EzConnectTrigger trigger;
        bool have_trigger = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (has_pending_connect_trigger_) {
                trigger = std::move(pending_connect_trigger_);
                has_pending_connect_trigger_ = false;
                have_trigger = true;
            }
        }

        if (!have_trigger && now >= passive_mdns_due_ms_) {
            passive_mdns_due_ms_ = now + kPassiveMdnsIntervalMs;
            if (QueryEzAssistantMdns("", true, &trigger)) {
                have_trigger = true;
                ESP_LOGI(kClientTag, "found open _ez-assistant._tcp host=%s:%u via passive mDNS",
                         trigger.host.c_str(), trigger.port);
            }
        }

        if (have_trigger) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                pairing_target_ = trigger;
            }
            SetState(EzClientState::kPairing);
            std::lock_guard<std::mutex> ws_lock(ws_lifecycle_mu_);
            if (!StartWebSocketLocked(trigger.host, trigger.port, trigger.path,
                                      trigger.certificate_fingerprint, true)) {
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    ws_pairing_mode_ = false;
                }
                SetState(EzClientState::kUnpairedReady);
            }
        }
    }
#endif // ESP_PLATFORM
}

} // namespace esp_pika
