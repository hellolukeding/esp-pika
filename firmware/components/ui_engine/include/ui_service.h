#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "audio_engine.h"
#include "board.h"
#include "mosaico_ui.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace esp_pika {

struct UiWifiCallbacks {
    std::function<void()> on_scan;
    std::function<void(const std::string& ssid, const std::string& password, bool use_saved)> on_connect;
    std::function<void(const std::string& ssid)> on_forget;
    std::function<void(bool enable)> on_toggle_portal;
    std::function<void()> on_panel_open;
};

struct UiWifiState {
    WifiUiMode mode = WifiUiMode::kOffline;
    bool portal_active = false;
    bool scanning = false;
    std::string ssid;
    std::string ip;
    int rssi = -100;
    std::string ap_ssid;
    std::string ap_ip = "192.168.4.1";
    std::string status_msg;
    uint32_t revision = 0;
    std::vector<WifiUiEntry> entries;
};

/// Coordinates the Core 1 30 FPS 3D UI task, sensor fusion (BMI270 + BQ27220),
/// CST9220 touch gestures, audio reactivity, DisplayHal region flushing, and Wi-Fi UI.
class UiService {
public:
    UiService() = default;
    ~UiService();

    UiService(const UiService&) = delete;
    UiService& operator=(const UiService&) = delete;

    void SetWifiCallbacks(UiWifiCallbacks callbacks);
    bool Start(Board* board, AudioEngine* audio, std::function<void()> on_wake_trigger = nullptr);
    void Stop();

    void SetNetworkStatus(bool wifi_connected, bool gateway_connected, const std::string& mac);
    void SetWifiState(const UiWifiState& wifi_state);
    void SetSessionTag(const std::string& tag);
    void SetBaseEmotion(AvatarEmotion emotion);
    void ShowMessage(const std::string& emotion, const std::string& text);

    bool IsRunning() const { return running_.load(); }

private:
#ifdef ESP_PLATFORM
    static void RenderTaskEntry(void* arg);
    void RunLoop();
    TaskHandle_t task_ = nullptr;
#endif

    Board* board_ = nullptr;
    AudioEngine* audio_ = nullptr;
    std::function<void()> on_wake_trigger_;
    UiWifiCallbacks wifi_callbacks_;
    MosaicoUi ui_;
    std::atomic<bool> running_{false};

    mutable std::mutex mu_;
    UiSnapshot snapshot_;
    bool has_pending_base_emotion_ = false;
    AvatarEmotion pending_base_emotion_ = AvatarEmotion::kIdle;
    bool has_pending_message_ = false;
    std::string pending_emotion_;
    std::string pending_text_;
};

} // namespace esp_pika
