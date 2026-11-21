#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "avatar_3d_renderer.h"
#include "avatar_animator.h"
#include "avatar_types.h"
#include "board.h"

namespace esp_pika {

/// Snapshot of system, audio, power, and sensor telemetry displayed by MosaicoUi.
struct UiSnapshot {
    bool wifi_connected = false;
    bool gateway_connected = false;
    std::string mac = "00:00:00:00:00:00";
    std::string session_tag = "SESSION-01";
    std::string status_tag; // If non-empty, overrides right pill (e.g. "THINK", "LISTEN")

    bool battery_valid = false;
    int battery_percent = 100;
    float battery_voltage = 0.0f;
    float battery_current_ma = 0.0f;
    float temperature_c = 0.0f;

    bool imu_valid = false;
    float accel_x = 0.0f;
    float accel_y = 0.0f;
    float accel_z = 1.0f;

    int volume = 75;
    int brightness = 90;
};

struct UiCallbacks {
    std::function<void(int volume)> on_volume_change;
    std::function<void(uint8_t brightness)> on_brightness_change;
    std::function<void()> on_long_press_wake;
    std::function<void(AvatarEmotion emotion)> on_quick_emotion;
};

/// 480x480 ESP-Mosaico compositor combining minimal HUD, 3D avatar stage,
/// auto-fading bottom captions, and CST9220 swipe-down telemetry/control drawer.
class MosaicoUi {
public:
    static constexpr int kWidth = 480;
    static constexpr int kHeight = 480;

    MosaicoUi() = default;
    ~MosaicoUi();

    MosaicoUi(const MosaicoUi&) = delete;
    MosaicoUi& operator=(const MosaicoUi&) = delete;

    bool Init(UiCallbacks callbacks = {});
    bool Ready() const { return ready_; }

    const uint16_t* Framebuffer() const { return framebuffer_; }

    /// External stimuli forwarded from Core 1 service.
    void SetBaseEmotion(AvatarEmotion emotion);
    void ShowMessage(const std::string& emotion, const std::string& text, float hold_sec = 4.5f);
    void SetImuTilt(float accel_x, float accel_y);
    void TriggerShake();
    void SetAudioEnergy(float rms_0_to_1);
    void OnTouch(const TouchPoint& tp, int64_t now_ms, UiSnapshot* mutable_state);

    bool IsDrawerOpen() const { return drawer_open_; }
    AvatarEmotion EffectiveEmotion() const { return animator_.EffectiveEmotion(); }

    /// Advances animation & renders modified regions into `framebuffer_`.
    void StepAndRender(float dt_sec, const UiSnapshot& state, std::vector<UiRect>* out_dirty);

    /// Deterministic single-frame helpers for host testing & visual preview rendering.
    void RenderPreviewExpression(AvatarEmotion emotion,
                                 const UiSnapshot& state,
                                 const std::string& caption = "");
    void RenderPreviewDrawer(const UiSnapshot& state);

private:
    void RenderTopHud(const UiSnapshot& state, AvatarEmotion active_emotion);
    void RenderBottomCaption(const UiSnapshot& state, float audio_energy);
    void RenderDrawerOverlay(const UiSnapshot& state, int top_y);
    void ClearFramebuffer();
    uint16_t* RawFb() { return framebuffer_; }

    UiCallbacks callbacks_;
    Avatar3DRenderer renderer_;
    AvatarAnimator animator_;

    uint16_t* framebuffer_ = nullptr;
#ifndef ESP_PLATFORM
    std::vector<uint16_t> host_fb_;
#endif
    bool ready_ = false;
    bool force_full_refresh_ = true;

    // Caption fade state.
    std::string caption_text_;
    float caption_hold_sec_ = 0.0f;
    float caption_alpha_ = 0.0f;
    bool caption_visible_last_ = false;

    // HUD diff tracking so static top bar isn't needlessly re-flushed every frame.
    bool last_wifi_ = false;
    bool last_gateway_ = false;
    int last_bat_pct_ = -1;
    bool last_charging_ = false;
    std::string last_right_pill_;

    // Swipe-down drawer animation state.
    bool drawer_open_ = false;
    float drawer_progress_ = 0.0f; // 0.0 = closed, 1.0 = full open
    float last_drawer_progress_ = 0.0f;

    // Touch gesture tracking.
    bool touch_down_ = false;
    int16_t down_x_ = 0;
    int16_t down_y_ = 0;
    int16_t cur_x_ = 0;
    int16_t cur_y_ = 0;
    int64_t down_ms_ = 0;
    bool long_press_fired_ = false;
    bool swipe_handled_ = false;
    int64_t last_tap_ms_ = 0;
    int16_t last_tap_x_ = 0;
    int16_t last_tap_y_ = 0;
};

} // namespace esp_pika
