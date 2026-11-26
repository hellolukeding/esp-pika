#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "app_state_machine.h"
#include "audio_engine.h"
#include "board.h"
#include "ez_device_client.h"
#include "gateway_protocol.h"
#include "local_executor.h"
#include "mac_identity.h"
#include "net_client.h"
#include "ui_service.h"
#include "voiceprint_engine.h"
#include "voiceprint_store.h"
#include "wifi_store.h"

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
constexpr const char* kFirmwareVersion = "0.5.0-direct";

esp_pika::AudioWakeMode ToAudioWakeMode(esp_pika::EzWakeMode mode) {
    switch (mode) {
    case esp_pika::EzWakeMode::kWakeWordPlusVp: return esp_pika::AudioWakeMode::kWakeWordPlusVp;
    case esp_pika::EzWakeMode::kVoiceprintDirect: return esp_pika::AudioWakeMode::kVoiceprintDirect;
    case esp_pika::EzWakeMode::kWakeWord:
    default:
        return esp_pika::AudioWakeMode::kWakeWord;
    }
}

std::string BuildDefaultMosaicoName(const std::string& mac) {
    std::string hex;
    for (char ch : mac) {
        if (std::isxdigit(static_cast<unsigned char>(ch))) {
            hex.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        }
    }
    if (hex.size() >= 4) {
        return "Mosaico-" + hex.substr(hex.size() - 4);
    }
    return "Mosaico";
}

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

        esp_pika::WifiStore gw_probe;
        gw_probe.LoadFromNvs();
        ez_direct_mode_ = !gw_probe.GatewayOverride().enabled;

        std::string factory_mac;
        esp_pika::ReadFactoryEfuseMac(&factory_mac);
        const std::string display_name = BuildDefaultMosaicoName(factory_mac);
        ESP_LOGI(kTag, "transport mode: %s (mac=%s name=%s)",
                 ez_direct_mode_ ? "EZ_DIRECT (16k PCM16)" : "PIKA_GATEWAY (Opus 60ms)",
                 factory_mac.c_str(), display_name.c_str());

        vp_store_.LoadFromNvs();
        wake_cfg_.LoadFromNvs();
        if (vp_store_.Count() == 0 && wake_cfg_.mode != esp_pika::EzWakeMode::kWakeWord) {
            wake_cfg_.mode = esp_pika::EzWakeMode::kWakeWord;
            wake_cfg_.SaveToNvs();
        }
        esp_pika::VoiceprintConfig vp_cfg;
        vp_cfg.accept_threshold = wake_cfg_.vp_threshold;
        if (!vp_engine_.Init(&vp_store_, vp_cfg)) {
            ESP_LOGE(kTag, "voiceprint engine init failed");
            return false;
        }
        vp_engine_.StartAsyncWorker([this](const esp_pika::WakeVoiceprintOutcome& outcome) {
            HandleVoiceprintOutcome(outcome);
        });

        esp_pika::AudioEngineConfig audio_cfg;
        audio_cfg.transport_codec = ez_direct_mode_ ? esp_pika::AudioTransportCodec::kPcm16_20ms
                                                    : esp_pika::AudioTransportCodec::kOpus60ms;
        audio_cfg.afe.wake_word = wake_cfg_.wake_word;
        audio_cfg.afe.wake_pinyin = wake_cfg_.wake_pinyin;
        if (!audio_.Init(board_->Audio(), audio_cfg)) {
            ESP_LOGE(kTag, "audio engine init failed");
            return false;
        }
        audio_.SetTransportCodec(audio_cfg.transport_codec);
        audio_.SetWakeMode(ToAudioWakeMode(wake_cfg_.mode), wake_cfg_.min_speech_ms,
                           wake_cfg_.cooldown_ms);
        executor_ = std::make_unique<esp_pika::LocalExecutor>(board_.get(), &net_, &vp_store_);
        hw_ready_ = true;

        if (ez_direct_mode_) {
            esp_pika::EzDeviceClientConfig ez_cfg;
            ez_cfg.mac = factory_mac;
            ez_cfg.display_name = display_name;
            ez_cfg.firmware_version = kFirmwareVersion;
            ez_cfg.control_port = 8765;
            ez_cfg.hardware.enabled = true;
            ez_cfg.hardware.mac = factory_mac;
            ez_cfg.hardware.chip = board_->ChipName();
            ez_cfg.hardware.model = "esp32s31-mosaico";
            ez_cfg.hardware.zone = kDefaultZone;
            ez_cfg.hardware.group_id = kDefaultGroupId;
            ez_cfg.hardware.vp_version = vp_store_.Version();
            ez_cfg.hardware.multinet_ready = audio_.IsMultiNetReady();
            ez_cfg.hardware.wake_config = wake_cfg_;
            ez_cfg.hardware.capabilities = {
                "device.display_show",
                "device.audio_set_volume",
                "device.sensor_read",
                "device.status_read",
                "device.actuator_set",
                "device.voiceprint_list",
                "device.voiceprint_delete",
                "device.voiceprint_capture",
            };

            esp_pika::EzDeviceCallbacks ez_cbs;
            ez_cbs.on_state = [this](esp_pika::EzClientState s) { HandleEzState(s); };
            ez_cbs.on_interaction_state = [this](const std::string& st, const std::string& reason) {
                HandleEzInteractionState(st, reason);
            };
            ez_cbs.on_transcript = [this](const std::string& text) {
                if (!text.empty()) {
                    ESP_LOGI(kTag, "ez transcript: %s", text.c_str());
                    ui_.ShowMessage("listening", text);
                }
            };
            ez_cbs.on_text_output = [this](const std::string& output_id, const std::string& text) {
                (void)output_id;
                if (!text.empty()) {
                    ESP_LOGI(kTag, "ez assistant: %s", text.c_str());
                    ui_.ShowMessage("happy", text);
                }
            };
            ez_cbs.on_playback_start = [this](const std::string& output_id,
                                              uint32_t stream_id,
                                              const std::string& text) {
                HandleEzPlaybackStart(output_id, stream_id, text);
            };
            ez_cbs.on_playback_end = [this](const std::string& output_id,
                                            uint32_t stream_id,
                                            const std::string& reason) {
                HandleEzPlaybackEnd(output_id, stream_id, reason);
            };
            ez_cbs.on_downlink_pcm = [this](uint32_t stream_id,
                                            uint32_t sequence,
                                            const uint8_t* pcm,
                                            size_t len) {
                HandleEzDownlinkPcm(stream_id, sequence, pcm, len);
            };
            ez_cbs.on_device_command = [this](const esp_pika::EzDeviceCommandRequest& cmd) {
                HandleEzDeviceCommand(cmd);
            };
            ez_cbs.on_voiceprint_sync = [this](const std::string& raw_payload) {
                HandleEzVoiceprintSync(raw_payload);
            };
            ez_cbs.on_wake_config_sync = [this](const esp_pika::EzWakeConfigSyncRequest& sync) {
                HandleEzWakeConfigSync(sync);
            };
            ez_client_.Init(ez_cfg, std::move(ez_cbs));
            ui_.SetEzUnpairCallback([this]() { ez_client_.ResetPairing(); });
        }

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
        if (ez_direct_mode_) {
            ui_.SetEzStatus(true, ez_client_.IsPaired(), false,
                            ez_client_.KeyFingerprint(), ez_client_.PairedHost());
            ui_.SetSessionTag(ez_client_.IsPaired() ? "EZ:WAIT" : "PAIR READY");
        } else {
            ui_.SetSessionTag("SESSION-01");
        }

        audio_.Start(
            nullptr,
            [this](const esp_pika::WakeEvent& evt) { HandleWakeEvent(evt); },
            [this](bool speaking, float snr_db) { HandleVadState(speaking, snr_db); },
            [this](const esp_pika::VoiceprintCandidateEvent& cand) {
                HandleVoiceprintCandidate(cand);
            });
        audio_started_ = true;

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
            if (ez_direct_mode_) {
                net_.EnsureMdnsStarted();
                ez_client_.SetWifiConnected(true);
            } else {
                {
                    std::lock_guard<std::mutex> lock(net_mu_);
                    gateway_reconnect_needed_ = true;
                }
                TickHousekeeping();
            }
        }
        return true;
    }

    bool IsOnline() const { return ez_direct_mode_ ? ez_client_.IsOnline() : net_.IsConnected(); }

    void LogStatus() const {
        esp_pika::DeviceState cur_state;
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            cur_state = state_.State();
        }
        ESP_LOGI(kTag,
                 "status board=%s chip=%s mac=%s state=%s free_int=%u min_int=%u largest_int=%u free_psram=%u hwm=%u",
                 board_ ? board_->BoardName() : "none",
                 board_ ? board_->ChipName() : "none",
                 net_.DeviceMac().c_str(),
                 esp_pika::DeviceStateName(cur_state),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    }

    void TickFast(uint32_t dt_ms) {
        if (ez_direct_mode_) {
            ez_client_.TickFast(dt_ms);
            PendingDirectTurn pending_turn;
            bool run_pending_turn = false;
            {
                std::lock_guard<std::mutex> lock(speech_mu_);
                if (has_pending_direct_turn_) {
                    pending_turn = std::move(pending_direct_turn_);
                    has_pending_direct_turn_ = false;
                    run_pending_turn = true;
                }
            }
            if (run_pending_turn) {
                const bool started = StartDirectListenTurn(
                    pending_turn.wake_source, pending_turn.speaker_id, pending_turn.speaker_role,
                    pending_turn.speaker_score, pending_turn.play_chime,
                    pending_turn.accept_spec_gen);
                if (!started && pending_turn.accept_spec_gen > 0) {
                    audio_.RejectSpeculativeWake(pending_turn.accept_spec_gen);
                }
            }
        } else {
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
                    (pending_vad_shadow_.load(std::memory_order_relaxed) != 0) ? "speaking"
                                                                                : "idle";
                net_.SendText(esp_pika::BuildShadowUpdateJson(shadow));
            }
        }

        if (!IsUplinkActive()) {
            return;
        }
        bool is_started = false;
        uint32_t ez_sid = 0;
        {
            std::lock_guard<std::mutex> lock(speech_mu_);
            is_started = speech_started_;
            ez_sid = ez_uplink_stream_id_;
        }
        if (!is_started) {
            return;
        }
        if (ez_direct_mode_ && !ez_client_.IsOnline()) {
            audio_.SetUplinkEnabled(false);
            audio_.ClearQueues();
            {
                std::lock_guard<std::mutex> lock(speech_mu_);
                speech_started_ = false;
                ez_uplink_stream_id_ = 0;
            }
            return;
        }

        bool tx_aborted = false;
        const size_t flushed = audio_.FlushUplinkChecked(
            [this, ez_sid](const uint8_t* frame, size_t len) -> bool {
                if (ez_direct_mode_) {
                    if (len == 640 && ez_sid != 0) {
                        uint32_t seq = 0;
                        {
                            std::lock_guard<std::mutex> lock(speech_mu_);
                            seq = ez_uplink_seq_;
                        }
                        if (!ez_client_.SendUplinkPcmFrame(ez_sid, seq, frame)) {
                            ESP_LOGW(kTag,
                                     "uplink PCM send failed sid=%lu seq=%lu; aborting turn",
                                     static_cast<unsigned long>(ez_sid),
                                     static_cast<unsigned long>(seq));
                            return false;
                        }
                        {
                            std::lock_guard<std::mutex> lock(speech_mu_);
                            ez_uplink_seq_ = seq + 1;
                        }
                    }
                    return true;
                }
                net_.SendBinary(frame, len);
                return true;
            },
            &tx_aborted);
        if (tx_aborted) {
            audio_.SetUplinkEnabled(false);
            audio_.ClearQueues();
            {
                std::lock_guard<std::mutex> lock(speech_mu_);
                speech_started_ = false;
                heard_speech_ = false;
                speech_active_ms_ = 0;
                silence_after_speech_ms_ = 0;
                ez_uplink_stream_id_ = 0;
                ez_uplink_seq_ = 0;
            }
            if (ez_client_.IsOnline() && ez_sid != 0) {
                ez_client_.CancelListen(ez_sid);
            }
            {
                std::lock_guard<std::mutex> lock(state_mu_);
                state_.OnSpeechStopped();
                SyncUiEmotionLocked();
            }
            return;
        }
        const bool chime_playing = audio_.IsWakeChimePlaying();
        uint32_t core0_voiced_ms = 0;
        uint32_t core0_silence_ms = 0;
        audio_.GetUplinkVadState(&core0_voiced_ms, &core0_silence_ms);
        uint32_t end_silence_cfg_ms = 500U;
        {
            std::lock_guard<std::mutex> lock(wake_mu_);
            end_silence_cfg_ms = wake_cfg_.speech_end_silence_ms;
        }
        // ESP-SR AFE holds VAD_SPEECH for vad_min_noise_ms (200ms) after voice drops; subtract it
        // so total user-perceived pause matches the configured speech_end_silence_ms.
        constexpr uint32_t kAfeVadMinNoiseMs = 200U;
        const uint32_t extra_silence_target_ms =
            (end_silence_cfg_ms > kAfeVadMinNoiseMs)
                ? std::max<uint32_t>(end_silence_cfg_ms - kAfeVadMinNoiseMs, 60U)
                : 60U;
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
                speech_active_ms_ = std::max(speech_active_ms_, core0_voiced_ms);
                silence_after_speech_ms_ = core0_silence_ms;
                if (speech_active_ms_ >= 120U) {
                    heard_speech_ = true;
                }
                if (clean_elapsed_ms >= 40 && heard_speech_ &&
                    silence_after_speech_ms_ >= extra_silence_target_ms) {
                    finish_reason = "vad_end";
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

        if (ez_direct_mode_) {
            return;
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
        ui_.SetNetworkStatus(ws.wifi_connected,
                             ez_direct_mode_ ? ez_client_.IsOnline() : net_.IsConnected(),
                             ws.mac);
        if (ez_direct_mode_) {
            ui_.SetEzStatus(true, ez_client_.IsPaired(), ez_client_.IsOnline(),
                            ez_client_.KeyFingerprint(), ez_client_.PairedHost());
        }

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
                    gateway_reconnect_needed_ = !ez_direct_mode_;
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
                if (ez_direct_mode_) {
                    net_.EnsureMdnsStarted();
                    ez_client_.SetWifiConnected(true);
                }
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
                ez_uplink_stream_id_ = 0;
                ez_uplink_seq_ = 0;
            }
            audio_.SetUplinkEnabled(false);
            if (ez_direct_mode_) {
                {
                    std::lock_guard<std::mutex> lock(ez_mu_);
                    ez_active_playback_output_id_.clear();
                    ez_active_playback_stream_id_ = 0;
                }
                ez_client_.SetWifiConnected(false);
            }
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
        uint32_t ez_sid = 0;
        uint32_t ez_seq_count = 0;
        uint32_t logged_voiced_ms = 0;
        uint32_t logged_silence_ms = 0;
        {
            std::lock_guard<std::mutex> lock(speech_mu_);
            if (!speech_started_) {
                return;
            }
            speech_started_ = false;
            heard_speech_ = false;
            logged_voiced_ms = speech_active_ms_;
            logged_silence_ms = silence_after_speech_ms_;
            speech_active_ms_ = 0;
            silence_after_speech_ms_ = 0;
            sent_frames = turn_sent_frames_;
            ez_sid = ez_uplink_stream_id_;
            ez_uplink_stream_id_ = 0;
            ez_seq_count = ez_uplink_seq_;
        }
        audio_.SetUplinkEnabled(false);
        bool tx_aborted = false;
        const size_t rem = audio_.FlushUplinkChecked(
            [this, ez_sid, &ez_seq_count](const uint8_t* frame, size_t len) -> bool {
                if (ez_direct_mode_) {
                    if (len == 640 && ez_sid != 0) {
                        if (!ez_client_.SendUplinkPcmFrame(ez_sid, ez_seq_count, frame)) {
                            ESP_LOGW(kTag,
                                     "final uplink PCM send failed sid=%lu seq=%lu; cancelling",
                                     static_cast<unsigned long>(ez_sid),
                                     static_cast<unsigned long>(ez_seq_count));
                            return false;
                        }
                        ++ez_seq_count;
                    }
                    return true;
                }
                net_.SendBinary(frame, len);
                return true;
            },
            &tx_aborted);
        if (tx_aborted) {
            audio_.ClearQueues();
        }
        sent_frames += rem;
        {
            std::lock_guard<std::mutex> lock(speech_mu_);
            turn_sent_frames_ = sent_frames;
            ez_uplink_seq_ = ez_seq_count;
        }
        ESP_LOGI(kTag,
                 "finishing speech uplink (%s); sent_frames=%u voiced_ms=%lu silence_ms=%lu aborted=%d",
                 reason, static_cast<unsigned>(sent_frames),
                 static_cast<unsigned long>(logged_voiced_ms),
                 static_cast<unsigned long>(logged_silence_ms),
                 tx_aborted ? 1 : 0);
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            state_.OnSpeechStopped();
            SyncUiEmotionLocked();
        }
        if (ez_direct_mode_) {
            if (ez_sid != 0) {
                if (!tx_aborted && ez_seq_count > 0) {
                    ez_client_.StopListen(ez_sid, ez_seq_count - 1);
                } else {
                    ez_client_.CancelListen(ez_sid);
                }
            }
        } else {
            net_.SendText(esp_pika::BuildSpeechStopJson());
        }
    }

    bool StartDirectListenTurn(const std::string& wake_source,
                               const std::string& speaker_id,
                               int speaker_role,
                               float speaker_score,
                               bool play_chime,
                               uint32_t accept_spec_gen) {
        if (!ez_client_.IsOnline()) {
            ui_.ShowMessage(ez_client_.IsPaired() ? "sleep" : "curious",
                            ez_client_.IsPaired() ? "EZ ASSISTANT OFFLINE"
                                                  : "CONNECT IN EZ ASSISTANT");
            return false;
        }
        if (!ez_client_.IsAsrReady()) {
            ui_.ShowMessage("surprised", "ASR NOT CONFIGURED");
            return false;
        }
        std::string active_out;
        uint32_t active_sid = 0;
        {
            std::lock_guard<std::mutex> lock(ez_mu_);
            active_out = std::move(ez_active_playback_output_id_);
            active_sid = ez_active_playback_stream_id_;
            ez_active_playback_output_id_.clear();
            ez_active_playback_stream_id_ = 0;
        }
        if (active_sid != 0 && !active_out.empty()) {
            ez_client_.CancelPlayback(active_out, active_sid);
        }
        audio_.ClearDownlinkQueue();
        if (accept_spec_gen == 0) {
            audio_.ClearQueues();
        }
        if (play_chime) {
            audio_.PlayWakeChime();
        }

        uint32_t new_sid = 0;
        if (!ez_client_.StartListenWithContext(&new_sid, wake_source, speaker_id, speaker_role,
                                               speaker_score)) {
            ui_.ShowMessage("surprised", "LISTEN START FAILED");
            return false;
        }
        if (accept_spec_gen > 0) {
            if (!audio_.AcceptSpeculativeWake(accept_spec_gen)) {
                ez_client_.CancelListen(new_sid);
                return false;
            }
        }
        uint32_t init_voiced_ms = 0;
        if (accept_spec_gen > 0) {
            std::lock_guard<std::mutex> lock(wake_mu_);
            init_voiced_ms = wake_cfg_.min_speech_ms;
        }
        audio_.ResetUplinkVadState(init_voiced_ms);
        {
            std::lock_guard<std::mutex> lock(speech_mu_);
            speech_started_ = true;
            heard_speech_ = (accept_spec_gen > 0);
            speech_active_ms_ = init_voiced_ms;
            silence_after_speech_ms_ = 0;
            turn_sent_frames_ = 0;
            logged_first_rx_audio_ = false;
            ez_uplink_stream_id_ = new_sid;
            ez_uplink_seq_ = 0;
            uplink_started_ms_ = NowMillis();
        }
        if (accept_spec_gen == 0) {
            audio_.SetUplinkEnabled(true);
        }
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            state_.OnLocalWakeTriggered();
            state_.OnWakeAccept();
            SyncUiEmotionLocked();
        }
        return true;
    }

    void HandleVoiceprintCandidate(const esp_pika::VoiceprintCandidateEvent& cand) {
        if (!ez_direct_mode_ || vp_store_.Count() == 0) {
            audio_.RejectSpeculativeWake(cand.generation);
            return;
        }
        esp_pika::WakeVoiceprintJob job;
        job.wake_word = "voiceprint_direct";
        job.wake_source = "voiceprint_direct";
        job.generation = cand.generation;
        job.wake_score = 1.0f;
        job.snr_db = cand.snr_db;
        job.pcm_16k = cand.pcm_16k;
        if (!vp_engine_.SubmitWakeUtteranceAsync(std::move(job))) {
            audio_.RejectSpeculativeWake(cand.generation);
        }
    }

    void QueueDirectListenTurn(const std::string& wake_source,
                               const std::string& speaker_id,
                               int speaker_role,
                               float speaker_score,
                               bool play_chime,
                               uint32_t accept_spec_gen) {
        uint32_t superseded_gen = 0;
        {
            std::lock_guard<std::mutex> lock(speech_mu_);
            if (has_pending_direct_turn_ && pending_direct_turn_.accept_spec_gen > 0) {
                superseded_gen = pending_direct_turn_.accept_spec_gen;
            }
            pending_direct_turn_.wake_source = wake_source;
            pending_direct_turn_.speaker_id = speaker_id;
            pending_direct_turn_.speaker_role = speaker_role;
            pending_direct_turn_.speaker_score = speaker_score;
            pending_direct_turn_.play_chime = play_chime;
            pending_direct_turn_.accept_spec_gen = accept_spec_gen;
            has_pending_direct_turn_ = true;
        }
        if (superseded_gen > 0 && superseded_gen != accept_spec_gen) {
            audio_.RejectSpeculativeWake(superseded_gen);
        }
    }

    void HandleWakeEvent(const esp_pika::WakeEvent& evt) {
        ESP_LOGI(kTag, "wake triggered word=%s score=%.2f snr=%.1f",
                 evt.wake_word.c_str(),
                 static_cast<double>(evt.wake_score),
                 static_cast<double>(evt.snr_db));
        if (ez_direct_mode_) {
            esp_pika::EzWakeConfig cur_wake;
            {
                std::lock_guard<std::mutex> lock(wake_mu_);
                cur_wake = wake_cfg_;
            }
            if (cur_wake.mode == esp_pika::EzWakeMode::kWakeWordPlusVp &&
                vp_store_.Count() > 0 && !evt.wake_pcm_16k.empty()) {
                esp_pika::WakeVoiceprintJob job;
                job.wake_word = evt.wake_word;
                job.wake_source = "wake_word_plus_vp";
                job.wake_score = evt.wake_score;
                job.snr_db = evt.snr_db;
                job.pcm_16k = evt.wake_pcm_16k;
                vp_engine_.SubmitWakeUtteranceAsync(std::move(job));
                return;
            }
            if (!ez_client_.IsOnline()) {
                ui_.ShowMessage(ez_client_.IsPaired() ? "sleep" : "curious",
                                ez_client_.IsPaired() ? "EZ ASSISTANT OFFLINE"
                                                      : "CONNECT IN EZ ASSISTANT");
                return;
            }
            if (!ez_client_.IsAsrReady()) {
                ui_.ShowMessage("surprised", "ASR NOT CONFIGURED");
                return;
            }
            // Start wake chime immediately on Core 0, then offload voiceprint inference and
            // TLS/WSS listen_start strictly to Core 1's internal-DRAM stack (AGENTS.md §3.1).
            audio_.ClearDownlinkQueue();
            audio_.ClearQueues();
            audio_.PlayWakeChime();
            if (vp_store_.Count() > 0 && !evt.wake_pcm_16k.empty()) {
                esp_pika::WakeVoiceprintJob job;
                job.wake_word = evt.wake_word;
                job.wake_source = "wake_word";
                job.wake_score = evt.wake_score;
                job.snr_db = evt.snr_db;
                job.pcm_16k = evt.wake_pcm_16k;
                if (vp_engine_.SubmitWakeUtteranceAsync(std::move(job))) {
                    return;
                }
            }
            QueueDirectListenTurn("wake_word", "", 0, 0.0f, false, 0);
            return;
        }
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
        ESP_LOGI(kTag, "voiceprint result src=%s speaker=%s role=%s score=%.3f latency=%lldus",
                 outcome.job.wake_source.empty() ? "gateway" : outcome.job.wake_source.c_str(),
                 outcome.match.speaker_id.c_str(),
                 esp_pika::SpeakerRoleName(outcome.match.role),
                 static_cast<double>(outcome.match.score),
                 static_cast<long long>(outcome.inference_us));
        if (ez_direct_mode_) {
            esp_pika::EzWakeConfig cur_wake;
            {
                std::lock_guard<std::mutex> lock(wake_mu_);
                cur_wake = wake_cfg_;
            }
            const bool verified =
                outcome.match.identified && (outcome.match.score >= cur_wake.vp_threshold);
            if (outcome.job.wake_source == "wake_word") {
                const std::string spk_id = outcome.match.identified ? outcome.match.speaker_id : "";
                const int spk_role =
                    outcome.match.identified ? static_cast<int>(outcome.match.role) : 0;
                const float spk_score = outcome.match.identified ? outcome.match.score : 0.0f;
                QueueDirectListenTurn("wake_word", spk_id, spk_role, spk_score, false, 0);
                return;
            }
            if (outcome.job.wake_source == "voiceprint_direct") {
                if (verified) {
                    QueueDirectListenTurn("voiceprint_direct", outcome.match.speaker_id,
                                          static_cast<int>(outcome.match.role), outcome.match.score,
                                          cur_wake.vp_wake_chime, outcome.job.generation);
                } else {
                    audio_.RejectSpeculativeWake(outcome.job.generation);
                }
                return;
            }
            if (outcome.job.wake_source == "wake_word_plus_vp") {
                if (verified) {
                    QueueDirectListenTurn("wake_word_plus_vp", outcome.match.speaker_id,
                                          static_cast<int>(outcome.match.role),
                                          outcome.match.score, true, 0);
                } else {
                    ui_.ShowMessage("curious", "声纹未匹配");
                }
                return;
            }
            return;
        }
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
        if (ez_direct_mode_) {
            return;
        }
        pending_vad_shadow_.store(speaking ? 1 : 0, std::memory_order_relaxed);
        shadow_dirty_.store(true, std::memory_order_release);
    }

    void HandleEzState(esp_pika::EzClientState s) {
        const bool wifi_up = net_.IsWifiConnected();
        const bool online = (s == esp_pika::EzClientState::kOnline);
        ui_.SetNetworkStatus(wifi_up, online, net_.DeviceMac());
        ui_.SetEzStatus(true, ez_client_.IsPaired(), online,
                        ez_client_.KeyFingerprint(), ez_client_.PairedHost());

        switch (s) {
        case esp_pika::EzClientState::kOnline:
            ui_.SetSessionTag("EZ:ONLINE");
            {
                std::lock_guard<std::mutex> lock(state_mu_);
                state_.OnWsConnected();
                state_.OnDeviceWelcome();
                SyncUiEmotionLocked();
            }
            ui_.ShowMessage("happy", "EZ Ready");
            break;
        case esp_pika::EzClientState::kPairing:
            ui_.SetSessionTag("EZ:PAIR");
            break;
        case esp_pika::EzClientState::kUnpairedReady:
            ui_.SetSessionTag("PAIR READY");
            {
                uint32_t drop_spec_gen = 0;
                {
                    std::lock_guard<std::mutex> lock(speech_mu_);
                    speech_started_ = false;
                    ez_uplink_stream_id_ = 0;
                    ez_uplink_seq_ = 0;
                    if (has_pending_direct_turn_) {
                        drop_spec_gen = pending_direct_turn_.accept_spec_gen;
                        has_pending_direct_turn_ = false;
                    }
                }
                if (drop_spec_gen > 0) {
                    audio_.RejectSpeculativeWake(drop_spec_gen);
                }
            }
            audio_.SetUplinkEnabled(false);
            audio_.ClearQueues();
            {
                std::lock_guard<std::mutex> lock(ez_mu_);
                ez_active_playback_output_id_.clear();
                ez_active_playback_stream_id_ = 0;
            }
            {
                std::lock_guard<std::mutex> lock(state_mu_);
                if (wifi_up) {
                    state_.OnBoot();
                } else {
                    state_.OnWifiLost();
                }
                SyncUiEmotionLocked();
            }
            break;
        case esp_pika::EzClientState::kConnecting:
            ui_.SetSessionTag("EZ:WAIT");
            {
                uint32_t drop_spec_gen = 0;
                {
                    std::lock_guard<std::mutex> lock(speech_mu_);
                    speech_started_ = false;
                    ez_uplink_stream_id_ = 0;
                    ez_uplink_seq_ = 0;
                    if (has_pending_direct_turn_) {
                        drop_spec_gen = pending_direct_turn_.accept_spec_gen;
                        has_pending_direct_turn_ = false;
                    }
                }
                if (drop_spec_gen > 0) {
                    audio_.RejectSpeculativeWake(drop_spec_gen);
                }
            }
            audio_.SetUplinkEnabled(false);
            audio_.ClearQueues();
            {
                std::lock_guard<std::mutex> lock(ez_mu_);
                ez_active_playback_output_id_.clear();
                ez_active_playback_stream_id_ = 0;
            }
            {
                std::lock_guard<std::mutex> lock(state_mu_);
                state_.OnWsDisconnected();
                SyncUiEmotionLocked();
            }
            break;
        case esp_pika::EzClientState::kOffline:
        default:
            ui_.SetSessionTag("EZ:OFF");
            {
                uint32_t drop_spec_gen = 0;
                {
                    std::lock_guard<std::mutex> lock(speech_mu_);
                    speech_started_ = false;
                    ez_uplink_stream_id_ = 0;
                    ez_uplink_seq_ = 0;
                    if (has_pending_direct_turn_) {
                        drop_spec_gen = pending_direct_turn_.accept_spec_gen;
                        has_pending_direct_turn_ = false;
                    }
                }
                if (drop_spec_gen > 0) {
                    audio_.RejectSpeculativeWake(drop_spec_gen);
                }
            }
            audio_.SetUplinkEnabled(false);
            audio_.ClearQueues();
            {
                std::lock_guard<std::mutex> lock(ez_mu_);
                ez_active_playback_output_id_.clear();
                ez_active_playback_stream_id_ = 0;
            }
            {
                std::lock_guard<std::mutex> lock(state_mu_);
                state_.OnWifiLost();
                SyncUiEmotionLocked();
            }
            break;
        }
    }

    void HandleEzInteractionState(const std::string& st, const std::string& reason) {
        if (st == "thinking") {
            ui_.SetBaseEmotion(esp_pika::AvatarEmotion::kThinking);
        } else if (st == "listening") {
            ui_.SetBaseEmotion(esp_pika::AvatarEmotion::kListening);
        } else if (st == "speaking") {
            ui_.SetBaseEmotion(esp_pika::AvatarEmotion::kSpeaking);
        } else if (st == "error") {
            ui_.ShowMessage("surprised", reason.empty() ? "Error" : reason);
        } else if (st == "idle") {
            std::lock_guard<std::mutex> lock(state_mu_);
            if (state_.State() != esp_pika::DeviceState::kListening &&
                state_.State() != esp_pika::DeviceState::kSpeaking) {
                state_.OnDeviceWelcome();
                SyncUiEmotionLocked();
            }
        }
    }

    void HandleEzPlaybackStart(const std::string& output_id,
                               uint32_t stream_id,
                               const std::string& text) {
        {
            std::lock_guard<std::mutex> lock(ez_mu_);
            ez_active_playback_output_id_ = output_id;
            ez_active_playback_stream_id_ = stream_id;
        }
        {
            std::lock_guard<std::mutex> lock(speech_mu_);
            logged_first_rx_audio_ = false;
        }
        audio_.SetUplinkEnabled(false);
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            state_.OnDownlinkAudioStart();
            SyncUiEmotionLocked();
        }
        if (!text.empty()) {
            ui_.ShowMessage("speaking", text);
        }
    }

    void HandleEzDownlinkPcm(uint32_t stream_id,
                             uint32_t sequence,
                             const uint8_t* pcm,
                             size_t len) {
        (void)sequence;
        bool allow = false;
        {
            std::lock_guard<std::mutex> lock(ez_mu_);
            allow = (ez_active_playback_stream_id_ != 0 && stream_id == ez_active_playback_stream_id_);
        }
        {
            std::lock_guard<std::mutex> lock(speech_mu_);
            if (speech_started_) {
                allow = false;
            } else if (allow && !logged_first_rx_audio_) {
                ESP_LOGI(kTag, "received downlink PCM16 stream=%lu bytes=%u",
                         static_cast<unsigned long>(stream_id), static_cast<unsigned>(len));
                logged_first_rx_audio_ = true;
            }
        }
        if (allow) {
            audio_.EnqueueDownlinkPcm16(pcm, len);
        }
    }

    void HandleEzPlaybackEnd(const std::string& output_id,
                             uint32_t stream_id,
                             const std::string& reason) {
        (void)output_id;
        bool matched = false;
        {
            std::lock_guard<std::mutex> lock(ez_mu_);
            if (ez_active_playback_stream_id_ == stream_id) {
                ez_active_playback_stream_id_ = 0;
                ez_active_playback_output_id_.clear();
                matched = true;
            }
        }
        if (matched && reason == "cancelled") {
            audio_.ClearDownlinkQueue();
        }
    }

    void HandleEzDeviceCommand(const esp_pika::EzDeviceCommandRequest& cmd) {
        ESP_LOGI(kTag, "ez device_command id=%s cap=%s action=%s",
                 cmd.command_id.c_str(), cmd.capability.c_str(), cmd.action.c_str());
        if (cmd.capability == "device.status_read" || cmd.capability == "status.read") {
            const auto ws = net_.GetWifiStatus();
            const int volume = (board_ && board_->Audio()) ? board_->Audio()->Volume() : -1;
            const unsigned free_int =
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            const unsigned long long vp_ver =
                static_cast<unsigned long long>(vp_store_.Version());
            double battery_pct = -1.0;
            double battery_mv = -1.0;
            double temp_c = -999.0;
            if (board_ && board_->Sensors()) {
                const auto b = board_->Sensors()->Read("battery");
                if (b.ok) {
                    battery_pct = b.value;
                }
                const auto v = board_->Sensors()->Read("battery_voltage");
                if (v.ok) {
                    battery_mv = v.value;
                }
                const auto t = board_->Sensors()->Read("temperature");
                if (t.ok) {
                    temp_c = t.value;
                }
            }
            char buf[320] = {};
            std::string json = "{";
            std::snprintf(buf, sizeof(buf),
                          "\"chip\":\"%s\",\"mac\":\"%s\",\"rssi\":%d,\"volume\":%d,"
                          "\"free_int\":%u,\"vp_version\":%llu,\"vp_count\":%u",
                          board_ ? board_->ChipName() : "esp32s31",
                          net_.DeviceMac().c_str(),
                          ws.rssi,
                          volume,
                          free_int,
                          vp_ver,
                          static_cast<unsigned>(vp_store_.Count()));
            json += buf;
            if (battery_pct >= 0.0) {
                std::snprintf(buf, sizeof(buf), ",\"battery_pct\":%.1f", battery_pct);
                json += buf;
            }
            if (battery_mv >= 0.0) {
                std::snprintf(buf, sizeof(buf), ",\"battery_mv\":%.0f", battery_mv);
                json += buf;
            }
            if (temp_c > -100.0) {
                std::snprintf(buf, sizeof(buf), ",\"temperature_c\":%.1f", temp_c);
                json += buf;
            }
            json.push_back('}');
            ez_client_.SendDeviceCommandResult(cmd.command_id, true, json);
            return;
        }

        if (cmd.capability == "device.display_show" || cmd.capability == "lcd.display" ||
            cmd.capability == "display.lcd" || cmd.capability == "display.status") {
            std::string emotion = "neutral";
            std::string text;
            esp_pika::ExtractJsonStringField(cmd.params_json, "emotion", &emotion);
            esp_pika::ExtractJsonStringField(cmd.params_json, "text", &text);
            ui_.ShowMessage(emotion, text);
            std::string res = "{\"ok\":true,\"emotion\":\"" +
                              esp_pika::EscapeJsonString(emotion) + "\",\"text\":\"" +
                              esp_pika::EscapeJsonString(text) + "\"}";
            ez_client_.SendDeviceCommandResult(cmd.command_id, true, res);
            return;
        }

        if (cmd.capability == "device.voiceprint_capture" || cmd.capability == "voiceprint.capture") {
            int64_t duration_ms = 3000;
            int64_t step = 1;
            int64_t total = 3;
            esp_pika::ExtractJsonIntField(cmd.params_json, "duration_ms", &duration_ms);
            esp_pika::ExtractJsonIntField(cmd.params_json, "step", &step);
            esp_pika::ExtractJsonIntField(cmd.params_json, "total", &total);
            char prompt_buf[64] = {};
            std::snprintf(prompt_buf, sizeof(prompt_buf), "录声纹 %lld/%lld 请说话",
                          static_cast<long long>(step), static_cast<long long>(total));
            ui_.ShowMessage("listening", prompt_buf);

            std::vector<int16_t> pcm;
            uint32_t voiced_ms = 0;
            float snr_db = 0.0f;
            if (!audio_.CaptureSpeechSample(static_cast<uint32_t>(duration_ms), &pcm, &voiced_ms,
                                            &snr_db) ||
                pcm.size() < 4000 || voiced_ms < 450) {
                ui_.ShowMessage("curious", "声音太小，请重试");
                ez_client_.SendDeviceCommandResult(
                    cmd.command_id, false, "", "", "AUDIO_TOO_SHORT",
                    "未采集到足够有效人声（至少 0.5 秒），请靠近 Mosaico 重新朗读。");
                return;
            }
            std::vector<float> emb;
            if (!vp_engine_.ExtractEmbedding(pcm.data(), pcm.size(), &emb) || emb.empty()) {
                ui_.ShowMessage("curious", "声纹提取失败");
                ez_client_.SendDeviceCommandResult(cmd.command_id, false, "", "",
                                                   "EMBEDDING_FAILED", "端侧声纹特征提取失败");
                return;
            }
            double sum_sq = 0.0;
            for (int16_t s : pcm) {
                const double v = static_cast<double>(s) / 32768.0;
                sum_sq += v * v;
            }
            const float rms = static_cast<float>(std::sqrt(sum_sq / static_cast<double>(pcm.size())));
            const uint32_t actual_ms = static_cast<uint32_t>((pcm.size() * 1000U) / 16000U);

            std::string json;
            json.reserve(emb.size() * 12 + 128);
            char hdr[160] = {};
            std::snprintf(hdr, sizeof(hdr),
                          "{\"ok\":true,\"duration_ms\":%lu,\"voiced_ms\":%lu,\"snr_db\":%.2f,"
                          "\"rms\":%.4f,\"embedding\":[",
                          static_cast<unsigned long>(actual_ms),
                          static_cast<unsigned long>(voiced_ms),
                          static_cast<double>(snr_db),
                          static_cast<double>(rms));
            json += hdr;
            for (size_t i = 0; i < emb.size(); ++i) {
                if (i > 0) {
                    json.push_back(',');
                }
                char fbuf[20] = {};
                std::snprintf(fbuf, sizeof(fbuf), "%.6f", static_cast<double>(emb[i]));
                json += fbuf;
            }
            json += "]}";
            ui_.ShowMessage("happy", "声纹采集完成");
            ez_client_.SendDeviceCommandResult(cmd.command_id, true, json);
            return;
        }

        if (!executor_) {
            ez_client_.SendDeviceCommandResult(cmd.command_id, false, "", "",
                                               "EXECUTOR_UNAVAILABLE",
                                               "local executor not initialized");
            return;
        }
        esp_pika::GatewayCommand gw_cmd;
        gw_cmd.command_id = cmd.command_id;
        gw_cmd.capability = cmd.capability;
        gw_cmd.action = cmd.action;
        gw_cmd.params_json = cmd.params_json;
        const esp_pika::CommandResultPayload res = executor_->Execute(gw_cmd);
        if (res.ok && (cmd.capability == "device.voiceprint_delete" ||
                       cmd.capability == "voiceprint.delete")) {
            ez_client_.SetVoiceprintVersion(vp_store_.Version());
        }
        ez_client_.SendDeviceCommandResult(res.command_id, res.ok, res.result_json,
                                           res.media_url, res.error_code, res.error_message);
    }

    void HandleEzVoiceprintSync(const std::string& raw_payload) {
        esp_pika::VoiceprintSyncPayload sync;
        if (!esp_pika::ParseVoiceprintSyncJson(raw_payload, &sync)) {
            ESP_LOGW(kTag, "failed to parse ez voiceprint_sync payload");
            ez_client_.SendVoiceprintSyncAck(vp_store_.Version(),
                                             static_cast<uint32_t>(vp_store_.Count()),
                                             false, "parse_failed");
            return;
        }
        if (!vp_store_.ApplySync(sync)) {
            ez_client_.SendVoiceprintSyncAck(vp_store_.Version(),
                                             static_cast<uint32_t>(vp_store_.Count()),
                                             false, "apply_failed");
            return;
        }
        const bool saved = vp_store_.SaveToNvs();
        ez_client_.SetVoiceprintVersion(vp_store_.Version());
        if (vp_store_.Count() == 0) {
            bool fell_back = false;
            esp_pika::EzWakeConfig fallback_cfg;
            {
                std::lock_guard<std::mutex> lock(wake_mu_);
                if (wake_cfg_.mode != esp_pika::EzWakeMode::kWakeWord) {
                    wake_cfg_.mode = esp_pika::EzWakeMode::kWakeWord;
                    fallback_cfg = wake_cfg_;
                    fell_back = true;
                }
            }
            if (fell_back) {
                audio_.SetWakeMode(esp_pika::AudioWakeMode::kWakeWord, fallback_cfg.min_speech_ms,
                                   fallback_cfg.cooldown_ms);
                fallback_cfg.SaveToNvs();
                ez_client_.SetWakeConfig(fallback_cfg, audio_.IsMultiNetReady());
                ESP_LOGW(kTag, "voiceprints empty; auto-fallback to wake_word mode");
            }
        }
        ESP_LOGI(kTag, "ez voiceprint_sync applied vp_version=%llu count=%zu saved=%d",
                 static_cast<unsigned long long>(vp_store_.Version()),
                 vp_store_.Count(),
                 saved ? 1 : 0);
        ez_client_.SendVoiceprintSyncAck(vp_store_.Version(),
                                         static_cast<uint32_t>(vp_store_.Count()),
                                         saved, saved ? "" : "nvs_save_failed");
    }

    void HandleEzWakeConfigSync(const esp_pika::EzWakeConfigSyncRequest& sync) {
        esp_pika::EzWakeConfig next = sync.config;
        if (vp_store_.Count() == 0 && sync.enrolled_speaker_count == 0 &&
            next.mode != esp_pika::EzWakeMode::kWakeWord) {
            next.mode = esp_pika::EzWakeMode::kWakeWord;
        }
        esp_pika::EzWakeConfig prev;
        {
            std::lock_guard<std::mutex> lock(wake_mu_);
            prev = wake_cfg_;
        }
        if (next.wake_pinyin != prev.wake_pinyin || next.wake_word != prev.wake_word) {
            std::string err;
            if (!audio_.UpdateWakePinyin(next.wake_word, next.wake_pinyin, &err)) {
                ESP_LOGW(kTag, "MultiNet hot-update rejected pinyin='%s': %s",
                         next.wake_pinyin.c_str(), err.c_str());
                ez_client_.SendWakeConfigAck(sync.sync_id, false, &prev, err);
                return;
            }
        }
        vp_engine_.SetAcceptThreshold(next.vp_threshold);
        audio_.SetWakeMode(ToAudioWakeMode(next.mode), next.min_speech_ms, next.cooldown_ms);
        {
            std::lock_guard<std::mutex> lock(wake_mu_);
            wake_cfg_ = next;
        }
        const bool saved = next.SaveToNvs();
        ez_client_.SetWakeConfig(next, audio_.IsMultiNetReady());
        ESP_LOGI(kTag,
                 "ez wake_config_sync applied mode=%s word='%s' pinyin='%s' th=%.2f end_silence=%lums commit_delay=%lums saved=%d",
                 esp_pika::EzWakeModeWireName(next.mode), next.wake_word.c_str(),
                 next.wake_pinyin.c_str(), static_cast<double>(next.vp_threshold),
                 static_cast<unsigned long>(next.speech_end_silence_ms),
                 static_cast<unsigned long>(next.turn_commit_delay_ms),
                 saved ? 1 : 0);
        ez_client_.SendWakeConfigAck(sync.sync_id, saved, &next, saved ? "" : "nvs_save_failed");
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
            audio_.ResetUplinkVadState(0);
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
    bool ez_direct_mode_ = true;

    mutable std::mutex net_mu_;
    bool prev_wifi_connected_ = false;
    bool gateway_reconnect_needed_ = false;
    bool ws_started_ = false;
    int discovery_cooldown_ticks_ = 0;
    std::string connected_ssid_;
    std::string connected_ip_;
    esp_pika::GatewayEndpoint endpoint_;

    struct PendingDirectTurn {
        std::string wake_source;
        std::string speaker_id;
        int speaker_role = 0;
        float speaker_score = 0.0f;
        bool play_chime = false;
        uint32_t accept_spec_gen = 0;
    };

    mutable std::mutex speech_mu_;
    bool speech_started_ = false;
    bool has_pending_direct_turn_ = false;
    PendingDirectTurn pending_direct_turn_{};
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
    uint32_t ez_uplink_stream_id_ = 0;
    uint32_t ez_uplink_seq_ = 0;

    mutable std::mutex ez_mu_;
    std::string ez_active_playback_output_id_;
    uint32_t ez_active_playback_stream_id_ = 0;

    mutable std::mutex wake_mu_;
    esp_pika::EzWakeConfig wake_cfg_;

    mutable std::mutex state_mu_;
    esp_pika::AppStateMachine state_;

    esp_pika::NetClientConfig net_cfg_;
    std::unique_ptr<esp_pika::Board> board_;
    esp_pika::VoiceprintStore vp_store_;
    esp_pika::VoiceprintEngine vp_engine_;
    esp_pika::AudioEngine audio_;
    esp_pika::NetClient net_;
    esp_pika::EzDeviceClient ez_client_;
    std::unique_ptr<esp_pika::LocalExecutor> executor_;
    esp_pika::UiService ui_;
};
#endif

} // namespace

#ifdef ESP_PLATFORM
extern "C" void app_main(void) {
    // Keep app_main (Core 1 state machine & WSS uplink pump) above UiRenderTask (prio 3)
    // so 3D avatar rendering never stalls PCM streaming or VAD housekeeping.
    vTaskPrioritySet(nullptr, 4);
    ESP_ERROR_CHECK(InitNvs());

    static FirmwareApp app;
    if (!app.InitAndRun()) {
        ESP_LOGE(kTag, "hardware init failed; halting");
        return;
    }
    app.LogStatus();

    int64_t last_tick_ms = NowMillis();
    int64_t last_housekeeping_ms = last_tick_ms;
    int seconds = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(20));
        const int64_t now_ms = NowMillis();
        const uint32_t dt_ms = static_cast<uint32_t>(std::clamp<int64_t>(now_ms - last_tick_ms, 1, 250));
        last_tick_ms = now_ms;
        app.TickFast(dt_ms);
        if (now_ms - last_housekeeping_ms >= 1000) {
            last_housekeeping_ms = now_ms;
            app.TickHousekeeping();
            if (!app.IsOnline() && (++seconds % 10) == 0) {
                app.LogStatus();
            }
        }
    }
}
#endif
