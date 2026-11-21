#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include "audio_engine.h"
#include "board.h"
#include "mosaico_ui.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace esp_pika {

/// Coordinates the Core 1 30 FPS 3D UI task, sensor fusion (BMI270 + BQ27220),
/// CST9220 touch gestures, audio reactivity, and DisplayHal region flushing.
class UiService {
public:
    UiService() = default;
    ~UiService();

    UiService(const UiService&) = delete;
    UiService& operator=(const UiService&) = delete;

    bool Start(Board* board, AudioEngine* audio, std::function<void()> on_wake_trigger = nullptr);
    void Stop();

    void SetNetworkStatus(bool wifi_connected, bool gateway_connected, const std::string& mac);
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
