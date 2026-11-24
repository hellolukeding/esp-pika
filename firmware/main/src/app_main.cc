#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "app_state_machine.h"
#include "audio_engine.h"
#include "board.h"
#include "gateway_protocol.h"
#include "local_executor.h"
#include "net_client.h"
#include "ui_service.h"
#include "voiceprint_engine.h"
#include "voiceprint_store.h"

#ifdef ESP_PLATFORM
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#endif

namespace {

constexpr const char* kTag = "app_main";
constexpr const char* kFirmwareVersion = "0.2.0-m4";

#ifdef CONFIG_ESP_PIKA_GROUP_ID
constexpr const char* kDefaultGroupId = CONFIG_ESP_PIKA_GROUP_ID;
#else
constexpr const char* kDefaultGroupId = "home-default";
#endif

#ifdef CONFIG_ESP_PIKA_ZONE
constexpr const char* kDefaultZone = CONFIG_ESP_PIKA_ZONE;
#else
constexpr const char* kDefaultZone = "study";
#endif

#ifdef ESP_PLATFORM
int64_t NowMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

esp_err_t InitNvs() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), kTag, "nvs erase failed");
        ret = nvs_flash_init();
    }
    return ret;
}

esp_pika::WifiUiMode MapWifiUiMode(esp_pika::WifiConnState state) {
    switch (state) {
    case esp_pika::WifiConnState::kConnected: return esp_pika::WifiUiMode::kConnected;
    case esp_pika::WifiConnState::kConnecting: return esp_pika::WifiUiMode::kConnecting;
    case esp_pika::WifiConnState::kPortal: return esp_pika::WifiUiMode::kPortal;
    case esp_pika::WifiConnState::kOffline:
    default:
        return esp_pika::WifiUiMode::kOffline;
    }
}

esp_pika::UiWifiState ToUiWifiState(const esp_pika::WifiStatusSnapshot& ws) {
    esp_pika::UiWifiState out;
    out.mode = MapWifiUiMode(ws.state);
    out.portal_active = ws.portal_active;
    out.scanning = ws.scanning;
    out.ssid = ws.ssid;
    out.ip = ws.ip;
    out.rssi = ws.rssi;
    out.ap_ssid = ws.ap_ssid;
    out.ap_ip = ws.ap_ip;
    out.status_msg = ws.status_msg;
    out.revision = ws.revision;
    out.entries.reserve(ws.aps.size());
    for (const auto& ap : ws.aps) {
        out.entries.push_back({ap.ssid, ap.rssi, ap.secure, ap.saved, ap.connected});
    }
    return out;
}

class FirmwareApp {
public:
    bool InitAndRun() {
        board_ = esp_pika::CreateSelectedBoard();
        if (!board_ || !board_->Init()) {
            ESP_LOGE(kTag, "board init failed");
            return false;
        }
        ESP_LOGI(kTag, "board initialized: %s (%s)", board_->BoardName(), board_->ChipName());

        vp_store_.LoadFromNvs();
        if (!vp_engine_.Init(&vp_store_)) {
            ESP_LOGE(kTag, "voiceprint engine init failed");
            return false;
        }
        vp_engine_.StartAsyncWorker([this](const esp_pika::WakeVoiceprintOutcome& outcome) {
            HandleVoiceprintOutcome(outcome);
        });

        esp_pika::AudioEngineConfig audio_cfg;
        if (!audio_.Init(board_->Audio(), audio_cfg)) {
            ESP_LOGE(kTag, "audio engine init failed");
            return false;
        }
        hw_ready_ = true;

        esp_pika::UiWifiCallbacks wifi_cbs;
        wifi_cbs.on_scan = [this]() { net_.RequestScan(); };
        wifi_cbs.on_connect = [this](const std::string& ssid,
                                     const std::string& password,
                                     bool use_saved) {
            net_.RequestConnect(ssid, password, use_saved);
        };
        wifi_cbs.on_forget = [this](const std::string& ssid) { net_.RequestForget(ssid); };
        wifi_cbs.on_toggle_portal = [this](bool enable) { net_.RequestPortal(enable); };
        wifi_cbs.on_panel_open = [this]() {
            if (!net_.GetWifiStatus().scanning) {
                net_.RequestScan();
            }
        };
        ui_.SetWifiCallbacks(std::move(wifi_cbs));
        ui_.Start(board_.get(), &audio_, [this]() { audio_.TriggerWakeForTest(0.95f, 18.0f); });
        ui_.SetSessionTag("SESSION-01");

        net_cfg_ = {};
#ifdef CONFIG_ESP_PIKA_WIFI_SSID
        net_cfg_.wifi_ssid = CONFIG_ESP_PIKA_WIFI_SSID;
#endif
#ifdef CONFIG_ESP_PIKA_WIFI_PASSWORD
        net_cfg_.wifi_password = CONFIG_ESP_PIKA_WIFI_PASSWORD;
#endif
#ifdef CONFIG_ESP_PIKA_FALLBACK_GATEWAY_HOST
        net_cfg_.fallback_host = CONFIG_ESP_PIKA_FALLBACK_GATEWAY_HOST;
#endif
#ifdef CONFIG_ESP_PIKA_FALLBACK_GATEWAY_PORT
        net_cfg_.fallback_port = static_cast<uint16_t>(CONFIG_ESP_PIKA_FALLBACK_GATEWAY_PORT);
#endif

        {
            std::lock_guard<std::mutex> lock(state_mu_);
            state_.OnBoot();
            SyncUiEmotionLocked();
        }

        net_.SetWifiStateCallback([this](const esp_pika::WifiStatusSnapshot& ws) {
            HandleWifiState(ws);
        });
        net_.Init(net_cfg_);
        if (net_.IsWifiConnected()) {
            {
                std::lock_guard<std::mutex> lock(net_mu_);
                gateway_reconnect_needed_ = true;
            }
            TickHousekeeping();
        }
        return true;
    }

    bool IsOnline() const { return net_.IsConnected(); }

    void LogStatus() const {
        esp_pika::DeviceState cur_state;
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            cur_state = state_.State();
        }
        ESP_LOGI(kTag, "status board=%s chip=%s mac=%s state=%s free_int=%u min_int=%u",
                 board_ ? board_->BoardName() : "none",
                 board_ ? board_->ChipName() : "none",
                 net_.DeviceMac().c_str(),
                 esp_pika::DeviceStateName(cur_state),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
    }

    void TickFast(uint32_t dt_ms) {
        esp_pika::WakeDetectedPayload wake;
        bool send_wake = false;
        {
            std::lock_guard<std::mutex> lock(speech_mu_);
            if (has_pending_wake_) {
                wake = std::move(pending_wake_);
                has_pending_wake_ = false;
                send_wake = true;
            }
        }
        if (send_wake) {
            net_.SendText(esp_pika::BuildWakeDetectedJson(wake));
        }

        if (shadow_dirty_.exchange(false, std::memory_order_acq_rel)) {
            esp_pika::ShadowUpdatePayload shadow;
            shadow.vad_state =
                (pending_vad_shadow_.load(std::memory_order_relaxed) != 0) ? "speaking" : "idle";
            net_.SendText(esp_pika::BuildShadowUpdateJson(shadow));
        }

        if (!IsUplinkActive()) {
            return;
        }
        bool is_started = false;
        {
            std::lock_guard<std::mutex> lock(speech_mu_);
            is_started = speech_started_;
        }
        if (!is_started) {
            return;
        }

        const size_t flushed = audio_.FlushUplink(
            [this](const uint8_t* frame, size_t len) { net_.SendBinary(frame, len); });
        const bool chime_playing = audio_.IsWakeChimePlaying();
        const bool active = audio_.IsVadSpeaking() || (audio_.CleanMicLevel() >= 0.08f);
        const char* finish_reason = nullptr;
        {
            std::lock_guard<std::mutex> lock(speech_mu_);
            if (!speech_started_) {
                return;
            }
            turn_sent_frames_ += flushed;
            if (chime_playing) {
                uplink_started_ms_ = NowMillis();
                heard_speech_ = false;
                speech_active_ms_ = 0;
                silence_after_speech_ms_ = 0;
            } else {
                const int64_t clean_elapsed_ms = NowMillis() - uplink_started_ms_;
                if (clean_elapsed_ms >= 40) {
                    if (active) {
                        speech_active_ms_ += dt_ms;
                        silence_after_speech_ms_ = 0;
                        if (speech_active_ms_ >= 180) {
                            heard_speech_ = true;
                        }
                    } else if (heard_speech_) {
                        silence_after_speech_ms_ += dt_ms;
                        if (silence_after_speech_ms_ >= 260) {
                            finish_reason = "vad_end";
                        }
                    }
                }
                if (finish_reason == nullptr && !heard_speech_ && clean_elapsed_ms >= 5500) {
                    finish_reason = "no_speech_timeout";
                }
                if (finish_reason == nullptr && clean_elapsed_ms >= 12000) {
                    finish_reason = "max_duration";
                }
            }
        }
        if (finish_reason != nullptr) {
            FinishSpeechUplink(finish_reason);
        }
    }

    void TickHousekeeping() {
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            if (state_.State() == esp_pika::DeviceState::kSpeaking &&
                audio_.DownlinkQueueSize() == 0 && audio_.SpeakerLevel() < 0.01f) {
                state_.OnPlaybackFinished();
                SyncUiEmotionLocked();
            }
        }

        if (!net_.IsWifiConnected()) {
            std::lock_guard<std::mutex> lock(net_mu_);
            discovery_cooldown_ticks_ = 0;
            return;
        }

        bool need_reconnect = false;
        bool should_check_discovery = false;
        {
            std::lock_guard<std::mutex> lock(net_mu_);
            if (gateway_reconnect_needed_) {
                gateway_reconnect_needed_ = false;
                ws_started_ = false;
                discovery_cooldown_ticks_ = 0;
                need_reconnect = true;
                should_check_discovery = true;
            } else if (!net_.IsConnected()) {
                if (discovery_cooldown_ticks_ > 0) {
                    --discovery_cooldown_ticks_;
                } else {
                    discovery_cooldown_ticks_ = 8;
                    should_check_discovery = true;
                }
            } else {
                discovery_cooldown_ticks_ = 0;
            }
        }
        if (!should_check_discovery) {
            return;
        }

        esp_pika::GatewayEndpoint fresh;
        if (!net_.DiscoverGateway(&fresh)) {
            ESP_LOGW(kTag, "gateway discovery pending; will retry");
            std::lock_guard<std::mutex> lock(net_mu_);
            discovery_cooldown_ticks_ = 8;
            return;
        }

        if (!executor_) {
            executor_ = std::make_unique<esp_pika::LocalExecutor>(board_.get(), &net_, &vp_store_);
        }
        if (!audio_started_) {
            ESP_LOGI(kTag, "step: starting audio engine");
            audio_.Start(
                nullptr,
                [this](const esp_pika::WakeEvent& evt) { HandleWakeEvent(evt); },
                [this](bool speaking, float snr_db) { HandleVadState(speaking, snr_db); });
            audio_started_ = true;
        }

        bool connect_now = need_reconnect;
        {
            std::lock_guard<std::mutex> lock(net_mu_);
            if (!ws_started_ || fresh.host != endpoint_.host || fresh.port != endpoint_.port) {
                connect_now = true;
            }
            if (connect_now) {
                endpoint_ = fresh;
                ws_started_ = true;
                discovery_cooldown_ticks_ = 8;
            }
        }
        if (connect_now) {
            {
                std::lock_guard<std::mutex> lock(state_mu_);
                state_.OnGatewayDiscovered();
                SyncUiEmotionLocked();
            }
            ESP_LOGI(kTag, "connecting gateway %s:%u", fresh.host.c_str(), fresh.port);
            ConnectGatewayEndpoint(fresh);
        }
    }

private:
    bool IsUplinkActive() const {
        std::lock_guard<std::mutex> lock(state_mu_);
        return state_.IsUplinkActive();
    }

    void HandleWifiState(const esp_pika::WifiStatusSnapshot& ws) {
        ui_.SetWifiState(ToUiWifiState(ws));
        ui_.SetNetworkStatus(ws.wifi_connected, net_.IsConnected(), ws.mac);

        bool was_connected = false;
        bool became_online_or_switched = false;
        {
            std::lock_guard<std::mutex> lock(net_mu_);
            was_connected = prev_wifi_connected_;
            if (ws.wifi_connected) {
                if (!prev_wifi_connected_ || ws.ssid != connected_ssid_ || ws.ip != connected_ip_) {
                    prev_wifi_connected_ = true;
                    connected_ssid_ = ws.ssid;
                    connected_ip_ = ws.ip;
                    gateway_reconnect_needed_ = true;
                    became_online_or_switched = true;
                }
            } else {
                prev_wifi_connected_ = false;
                connected_ssid_.clear();
                connected_ip_.clear();
                gateway_reconnect_needed_ = false;
                ws_started_ = false;
                discovery_cooldown_ticks_ = 0;
            }
        }

        if (ws.wifi_connected) {
            if (became_online_or_switched) {
                std::lock_guard<std::mutex> lock(state_mu_);
                state_.OnWifiConnected();
                SyncUiEmotionLocked();
            }
            return;
        }

        if (was_connected) {
            {
                std::lock_guard<std::mutex> lock(speech_mu_);
                speech_started_ = false;
                heard_speech_ = false;
                speech_active_ms_ = 0;
                silence_after_speech_ms_ = 0;
            }
            audio_.SetUplinkEnabled(false);
        }
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            if (ws.portal_active) {
                state_.OnPortalStarted();
            } else if (state_.State() == esp_pika::DeviceState::kProvisioning) {
                state_.OnPortalStopped(false);
            } else {
                state_.OnWifiLost();
            }
            SyncUiEmotionLocked();
        }
    }

    void ConnectGatewayEndpoint(const esp_pika::GatewayEndpoint& endpoint) {
        net_.ConnectWebSocket(
            endpoint,
            [this](const std::string& msg) { HandleTextMessage(msg); },
            [this](const uint8_t* bin, size_t len) { HandleAudioFrame(bin, len); },
            [this](bool connected) { HandleConnectionState(connected); });
    }

    void SyncUiEmotionLocked() {
        switch (state_.State()) {
        case esp_pika::DeviceState::kWakePending:
        case esp_pika::DeviceState::kListening:
            ui_.SetBaseEmotion(esp_pika::AvatarEmotion::kListening);
            break;
        case esp_pika::DeviceState::kSpeaking:
            ui_.SetBaseEmotion(esp_pika::AvatarEmotion::kSpeaking);
            break;
        case esp_pika::DeviceState::kProvisioning:
            ui_.SetBaseEmotion(esp_pika::AvatarEmotion::kCurious);
            break;
        case esp_pika::DeviceState::kOffline:
            ui_.SetBaseEmotion(esp_pika::AvatarEmotion::kSleep);
            break;
        default:
            ui_.SetBaseEmotion(esp_pika::AvatarEmotion::kIdle);
            break;
        }
    }

    void FinishSpeechUplink(const char* reason) {
        size_t sent_frames = 0;
        {
            std::lock_guard<std::mutex> lock(speech_mu_);
            if (!speech_started_) {
                return;
            }
            speech_started_ = false;
            heard_speech_ = false;
            speech_active_ms_ = 0;
            silence_after_speech_ms_ = 0;
            sent_frames = turn_sent_frames_;
        }
        audio_.SetUplinkEnabled(false);
        const size_t rem = audio_.FlushUplink(
            [this](const uint8_t* frame, size_t len) { net_.SendBinary(frame, len); });
        sent_frames += rem;
        {
            std::lock_guard<std::mutex> lock(speech_mu_);
            turn_sent_frames_ = sent_frames;
        }
        ESP_LOGI(kTag, "finishing speech uplink (%s); sent_frames=%u; sending speech.stop", reason,
                 static_cast<unsigned>(sent_frames));
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            state_.OnSpeechStopped();
            SyncUiEmotionLocked();
        }
        net_.SendText(esp_pika::BuildSpeechStopJson());
    }

    void HandleWakeEvent(const esp_pika::WakeEvent& evt) {
        ESP_LOGI(kTag, "wake triggered word=%s score=%.2f snr=%.1f",
                 evt.wake_word.c_str(),
                 static_cast<double>(evt.wake_score),
                 static_cast<double>(evt.snr_db));
        esp_pika::WakeDetectedPayload wake;
        wake.group_id = kDefaultGroupId;
        wake.zone = kDefaultZone;
        wake.wake_word = evt.wake_word;
        wake.wake_score = evt.wake_score;
        wake.snr_db = evt.snr_db;
        wake.detected_at_ms = NowMillis();
        {
            std::lock_guard<std::mutex> lock(speech_mu_);
            speech_started_ = false;
            heard_speech_ = false;
            speech_active_ms_ = 0;
            silence_after_speech_ms_ = 0;
            turn_sent_frames_ = 0;
            has_vp_outcome_ = false;
            pending_wake_ = wake;
            has_pending_wake_ = true;
        }
        audio_.ClearQueues();
        audio_.PlayWakeChime();
        audio_.SetUplinkEnabled(true);
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            state_.OnLocalWakeTriggered();
            SyncUiEmotionLocked();
        }

        // Submit 1.0-1.5s pre-roll PCM asynchronously to Core 1 so Core 0 audio never stalls.
        esp_pika::WakeVoiceprintJob job;
        job.wake_word = evt.wake_word;
        job.wake_score = evt.wake_score;
        job.snr_db = evt.snr_db;
        job.pcm_16k = evt.wake_pcm_16k;
        vp_engine_.SubmitWakeUtteranceAsync(std::move(job));
    }

    void HandleVoiceprintOutcome(const esp_pika::WakeVoiceprintOutcome& outcome) {
        ESP_LOGI(kTag, "voiceprint result speaker=%s role=%s score=%.3f latency=%lldus",
                 outcome.match.speaker_id.c_str(),
                 esp_pika::SpeakerRoleName(outcome.match.role),
                 static_cast<double>(outcome.match.score),
                 static_cast<long long>(outcome.inference_us));
        const bool uplink = IsUplinkActive();
        bool send_start = false;
        {
            std::lock_guard<std::mutex> lock(speech_mu_);
            last_vp_outcome_ = outcome;
            has_vp_outcome_ = true;
            if (uplink && !speech_started_) {
                speech_started_ = true;
                send_start = true;
            }
        }
        if (send_start) {
            const std::string spk = outcome.match.identified ? outcome.match.speaker_id : "";
            net_.SendText(esp_pika::BuildSpeechStartJson(
                spk, "", static_cast<int>(outcome.match.role), outcome.match.score));
        }
    }

    void HandleVadState(bool speaking, float snr_db) {
        (void)snr_db;
        pending_vad_shadow_.store(speaking ? 1 : 0, std::memory_order_relaxed);
        shadow_dirty_.store(true, std::memory_order_release);
    }

    void HandleConnectionState(bool connected) {
        const bool wifi_up = net_.IsWifiConnected();
        if (!connected) {
            audio_.SetUplinkEnabled(false);
            ui_.SetNetworkStatus(wifi_up, false, net_.DeviceMac());
            {
                std::lock_guard<std::mutex> lock(state_mu_);
                state_.OnWsDisconnected();
                SyncUiEmotionLocked();
            }
            if (wifi_up && board_ && board_->Display()) {
                board_->Display()->Show("sleep", "Reconnecting...");
            }
            return;
        }

        ui_.SetNetworkStatus(true, true, net_.DeviceMac());
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            state_.OnWsConnected();
            SyncUiEmotionLocked();
        }
        esp_pika::DeviceHelloPayload hello;
        hello.device_id = net_.DeviceMac();
        hello.mac = net_.DeviceMac();
        hello.group_id = kDefaultGroupId;
        hello.zone = kDefaultZone;
        hello.chip = board_->ChipName();
        hello.vp_version = vp_store_.Version();
        hello.firmware_version = kFirmwareVersion;
        hello.capabilities = board_->Capabilities();
        net_.SendText(esp_pika::BuildDeviceHelloJson(hello));
    }

    void HandleTextMessage(const std::string& json_text) {
        esp_pika::ParsedGatewayMessage parsed;
        if (!esp_pika::ParseGatewayMessage(json_text, &parsed)) {
            ESP_LOGW(kTag, "failed to parse gateway message");
            return;
        }

        switch (parsed.type) {
        case esp_pika::GatewayMsgType::kDeviceWelcome:
            {
                std::lock_guard<std::mutex> lock(state_mu_);
                state_.OnDeviceWelcome();
                SyncUiEmotionLocked();
            }
            if (board_->Display()) {
                board_->Display()->Show("happy", "Ready");
            }
            break;
        case esp_pika::GatewayMsgType::kWakeAccept: {
            ESP_LOGI(kTag, "wake.accept received; starting speech.start and uplink");
            std::string spk;
            int role = 0;
            float score = 0.0f;
            {
                std::lock_guard<std::mutex> lock(speech_mu_);
                logged_first_rx_audio_ = false;
                heard_speech_ = false;
                speech_active_ms_ = 0;
                silence_after_speech_ms_ = 0;
                uplink_started_ms_ = NowMillis();
                if (has_vp_outcome_) {
                    if (last_vp_outcome_.match.identified) {
                        spk = last_vp_outcome_.match.speaker_id;
                    }
                    role = static_cast<int>(last_vp_outcome_.match.role);
                    score = last_vp_outcome_.match.score;
                }
            }
            net_.SendText(esp_pika::BuildSpeechStartJson(spk, "", role, score));
            {
                std::lock_guard<std::mutex> lock(state_mu_);
                state_.OnWakeAccept();
                SyncUiEmotionLocked();
            }
            {
                std::lock_guard<std::mutex> lock(speech_mu_);
                speech_started_ = true;
            }
            audio_.SetUplinkEnabled(true);
            break;
        }
        case esp_pika::GatewayMsgType::kWakeSuppress:
            {
                std::lock_guard<std::mutex> lock(speech_mu_);
                speech_started_ = false;
                heard_speech_ = false;
                speech_active_ms_ = 0;
                silence_after_speech_ms_ = 0;
            }
            audio_.SetUplinkEnabled(false);
            audio_.ClearQueues();
            {
                std::lock_guard<std::mutex> lock(state_mu_);
                state_.OnWakeSuppress();
                SyncUiEmotionLocked();
            }
            break;
        case esp_pika::GatewayMsgType::kVoiceprintSync: {
            esp_pika::VoiceprintSyncPayload sync;
            if (esp_pika::ParseVoiceprintSyncJson(json_text, &sync) && vp_store_.ApplySync(sync)) {
                vp_store_.SaveToNvs();
            }
            break;
        }
        case esp_pika::GatewayMsgType::kDeviceCommand:
            if (executor_) {
                esp_pika::CommandResultPayload res = executor_->Execute(parsed.command);
                net_.SendText(esp_pika::BuildCommandResultJson(res));
            }
            break;
        default:
            break;
        }
    }

    void HandleAudioFrame(const uint8_t* frame, size_t len) {
        bool ignore_stale = false;
        bool wake_pending = false;
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            wake_pending = (state_.State() == esp_pika::DeviceState::kWakePending);
        }
        {
            std::lock_guard<std::mutex> lock(speech_mu_);
            if (speech_started_ || wake_pending) {
                ignore_stale = true;
            } else if (!logged_first_rx_audio_) {
                ESP_LOGI(kTag, "received downlink opus frame bytes=%u", static_cast<unsigned>(len));
                logged_first_rx_audio_ = true;
            }
        }
        if (ignore_stale) {
            return;
        }
        audio_.SetUplinkEnabled(false);
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            state_.OnDownlinkAudioStart();
            SyncUiEmotionLocked();
        }
        audio_.EnqueueDownlinkOpus(frame, len);
    }

    bool hw_ready_ = false;
    bool audio_started_ = false;

    mutable std::mutex net_mu_;
    bool prev_wifi_connected_ = false;
    bool gateway_reconnect_needed_ = false;
    bool ws_started_ = false;
    int discovery_cooldown_ticks_ = 0;
    std::string connected_ssid_;
    std::string connected_ip_;
    esp_pika::GatewayEndpoint endpoint_;

    mutable std::mutex speech_mu_;
    bool speech_started_ = false;
    bool heard_speech_ = false;
    uint32_t speech_active_ms_ = 0;
    uint32_t silence_after_speech_ms_ = 0;
    size_t turn_sent_frames_ = 0;
    bool has_pending_wake_ = false;
    esp_pika::WakeDetectedPayload pending_wake_{};
    int64_t uplink_started_ms_ = 0;
    std::atomic<bool> shadow_dirty_{false};
    std::atomic<int> pending_vad_shadow_{0};
    bool has_vp_outcome_ = false;
    esp_pika::WakeVoiceprintOutcome last_vp_outcome_{};
    bool logged_first_rx_audio_ = false;

    mutable std::mutex state_mu_;
    esp_pika::AppStateMachine state_;

    esp_pika::NetClientConfig net_cfg_;
    std::unique_ptr<esp_pika::Board> board_;
    esp_pika::VoiceprintStore vp_store_;
    esp_pika::VoiceprintEngine vp_engine_;
    esp_pika::AudioEngine audio_;
    esp_pika::NetClient net_;
    std::unique_ptr<esp_pika::LocalExecutor> executor_;
    esp_pika::UiService ui_;
};
#endif

} // namespace

#ifdef ESP_PLATFORM
extern "C" void app_main(void) {
    ESP_ERROR_CHECK(InitNvs());

    static FirmwareApp app;
    if (!app.InitAndRun()) {
        ESP_LOGE(kTag, "hardware init failed; halting");
        return;
    }
    app.LogStatus();

    uint32_t fast_ticks = 0;
    int seconds = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(20));
        app.TickFast(20);
        if (++fast_ticks >= 50) {
            fast_ticks = 0;
            app.TickHousekeeping();
            if (!app.IsOnline() && (++seconds % 10) == 0) {
                app.LogStatus();
            }
        }
    }
}
#endif
