#include <chrono>
#include <memory>
#include <string>

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

        state_.OnBoot();
        return TryConnectNetwork(true);
    }

    bool RetryNetwork() {
        if (!hw_ready_) {
            return false;
        }
        return TryConnectNetwork(false);
    }

    bool TryConnectNetwork(bool initial) {
        const bool wifi_ok = initial ? net_.Init(net_cfg_) : net_.WaitForWifi(1000);
        if (!wifi_ok) {
            state_.OnWifiLost();
            ui_.SetNetworkStatus(false, false, net_.DeviceMac());
            SyncUiEmotion();
            ESP_LOGW(kTag, "network init offline; state=%s", esp_pika::DeviceStateName(state_.State()));
            return false;
        }
        state_.OnWifiConnected();
        wifi_connected_ = true;
        ui_.SetNetworkStatus(true, false, net_.DeviceMac());
        SyncUiEmotion();

        esp_pika::GatewayEndpoint endpoint;
        ESP_LOGI(kTag, "step: discovering gateway...");
        vTaskDelay(pdMS_TO_TICKS(80));
        if (!net_.DiscoverGateway(&endpoint)) {
            ESP_LOGW(kTag, "gateway discovery failed");
            return false;
        }
        state_.OnGatewayDiscovered();
        ESP_LOGI(kTag, "step: endpoint=%s:%u, init executor", endpoint.host.c_str(), endpoint.port);
        vTaskDelay(pdMS_TO_TICKS(80));

        if (!executor_) {
            executor_ = std::make_unique<esp_pika::LocalExecutor>(board_.get(), &net_, &vp_store_);
        }
        if (!audio_started_) {
            ESP_LOGI(kTag, "step: starting audio engine");
            vTaskDelay(pdMS_TO_TICKS(80));
            audio_.Start(
                [this](const uint8_t* frame, size_t len) {
                    if (state_.IsUplinkActive()) {
                        net_.SendBinary(frame, len);
                    }
                },
                [this](const esp_pika::WakeEvent& evt) { HandleWakeEvent(evt); },
                [this](bool speaking, float snr_db) { HandleVadState(speaking, snr_db); });
            audio_started_ = true;
        }

        ESP_LOGI(kTag, "step: connecting websocket");
        vTaskDelay(pdMS_TO_TICKS(80));
        net_.ConnectWebSocket(
            endpoint,
            [this](const std::string& msg) { HandleTextMessage(msg); },
            [this](const uint8_t* bin, size_t len) { HandleAudioFrame(bin, len); },
            [this](bool connected) { HandleConnectionState(connected); });

        return true;
    }

    void LogStatus() const {
        ESP_LOGI(kTag, "status board=%s chip=%s mac=%s state=%s",
                 board_ ? board_->BoardName() : "none",
                 board_ ? board_->ChipName() : "none",
                 net_.DeviceMac().c_str(),
                 esp_pika::DeviceStateName(state_.State()));
    }

    void TickHousekeeping() {
        if (state_.State() == esp_pika::DeviceState::kSpeaking &&
            audio_.DownlinkQueueSize() == 0 && audio_.SpeakerLevel() < 0.01f) {
            state_.OnPlaybackFinished();
            SyncUiEmotion();
        }
    }

private:
    void SyncUiEmotion() {
        switch (state_.State()) {
        case esp_pika::DeviceState::kWakePending:
        case esp_pika::DeviceState::kListening:
            ui_.SetBaseEmotion(esp_pika::AvatarEmotion::kListening);
            break;
        case esp_pika::DeviceState::kSpeaking:
            ui_.SetBaseEmotion(esp_pika::AvatarEmotion::kSpeaking);
            break;
        case esp_pika::DeviceState::kOffline:
            ui_.SetBaseEmotion(esp_pika::AvatarEmotion::kSleep);
            break;
        default:
            ui_.SetBaseEmotion(esp_pika::AvatarEmotion::kIdle);
            break;
        }
    }

    void HandleWakeEvent(const esp_pika::WakeEvent& evt) {
        state_.OnLocalWakeTriggered();
        SyncUiEmotion();

        esp_pika::WakeDetectedPayload wake;
        wake.group_id = kDefaultGroupId;
        wake.zone = kDefaultZone;
        wake.wake_word = evt.wake_word;
        wake.wake_score = evt.wake_score;
        wake.snr_db = evt.snr_db;
        wake.detected_at_ms = NowMillis();
        net_.SendText(esp_pika::BuildWakeDetectedJson(wake));

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
        if (state_.IsUplinkActive()) {
            const std::string spk = outcome.match.identified ? outcome.match.speaker_id : "";
            net_.SendText(esp_pika::BuildSpeechStartJson(spk, ""));
        }
    }

    void HandleVadState(bool speaking, float snr_db) {
        (void)snr_db;
        esp_pika::ShadowUpdatePayload shadow;
        shadow.vad_state = speaking ? "speaking" : "idle";
        net_.SendText(esp_pika::BuildShadowUpdateJson(shadow));
    }

    void HandleConnectionState(bool connected) {
        if (!connected) {
            state_.OnWsDisconnected();
            audio_.SetUplinkEnabled(false);
            ui_.SetNetworkStatus(wifi_connected_, false, net_.DeviceMac());
            SyncUiEmotion();
            if (board_ && board_->Display()) {
                board_->Display()->Show("sleep", "Reconnecting...");
            }
            return;
        }

        state_.OnWsConnected();
        ui_.SetNetworkStatus(true, true, net_.DeviceMac());
        SyncUiEmotion();
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
            state_.OnDeviceWelcome();
            SyncUiEmotion();
            if (board_->Display()) {
                board_->Display()->Show("happy", "Ready");
            }
            break;
        case esp_pika::GatewayMsgType::kWakeAccept:
            state_.OnWakeAccept();
            audio_.SetUplinkEnabled(state_.IsUplinkActive());
            SyncUiEmotion();
            break;
        case esp_pika::GatewayMsgType::kWakeSuppress:
            state_.OnWakeSuppress();
            audio_.SetUplinkEnabled(false);
            SyncUiEmotion();
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
        state_.OnDownlinkAudioStart();
        SyncUiEmotion();
        audio_.EnqueueDownlinkOpus(frame, len);
    }

    bool hw_ready_ = false;
    bool audio_started_ = false;
    bool wifi_connected_ = false;
    esp_pika::NetClientConfig net_cfg_;
    std::unique_ptr<esp_pika::Board> board_;
    esp_pika::VoiceprintStore vp_store_;
    esp_pika::VoiceprintEngine vp_engine_;
    esp_pika::AudioEngine audio_;
    esp_pika::NetClient net_;
    std::unique_ptr<esp_pika::LocalExecutor> executor_;
    esp_pika::AppStateMachine state_;
    esp_pika::UiService ui_;
};
#endif

} // namespace

#ifdef ESP_PLATFORM
extern "C" void app_main(void) {
    ESP_ERROR_CHECK(InitNvs());

    static FirmwareApp app;
    bool online = app.InitAndRun();
    app.LogStatus();

    int seconds = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        app.TickHousekeeping();
        if (!online && (++seconds % 5) == 0) {
            online = app.RetryNetwork();
            app.LogStatus();
        }
    }
}
#endif
