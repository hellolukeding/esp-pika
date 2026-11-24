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

/// High-level Wi-Fi connection / provisioning mode displayed by MosaicoUi.
enum class WifiUiMode {
    kOffline = 0,
    kConnecting,
    kConnected,
    kPortal,
};

/// Single Wi-Fi row displayed in the Mosaico on-screen Wi-Fi manager.
struct WifiUiEntry {
    std::string ssid;
    int rssi = -100;
    bool secure = true;
    bool saved = false;
    bool connected = false;
};

/// Snapshot of system, audio, power, sensor, and Wi-Fi telemetry displayed by MosaicoUi.
struct UiSnapshot {
    bool wifi_connected = false;
    bool gateway_connected = false;
    WifiUiMode wifi_mode = WifiUiMode::kOffline;
    bool wifi_portal_active = false;
    std::string mac = "00:00:00:00:00:00";
    std::string wifi_ssid;
    std::string wifi_ip;
    int wifi_rssi = -100;
    std::string ap_ssid;
    std::string ap_ip = "192.168.4.1";
    std::string wifi_status_msg;
    bool wifi_scanning = false;
    uint32_t wifi_revision = 0;
    std::vector<WifiUiEntry> wifi_entries;

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
    std::function<void()> on_wifi_scan;
    std::function<void(const std::string& ssid, const std::string& password, bool use_saved)>
        on_wifi_connect;
    std::function<void(const std::string& ssid)> on_wifi_forget;
    std::function<void(bool enable)> on_wifi_toggle_portal;
    std::function<void()> on_wifi_panel_open;
};

/// 480x480 ESP-Mosaico compositor combining minimal HUD, 3D avatar stage,
/// auto-fading bottom captions, CST9220 swipe-down telemetry drawer, and native Wi-Fi UI.
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

    void OpenWifiMenu();
    void CloseWifiOverlay();

    bool IsDrawerOpen() const { return drawer_open_; }
    bool IsOverlayOpen() const { return drawer_open_ || view_ != UiView::kAvatar; }
    bool IsWifiMenuOpen() const { return view_ == UiView::kWifiMenu; }
    bool IsWifiKeyboardOpen() const { return view_ == UiView::kWifiKeyboard; }
    AvatarEmotion EffectiveEmotion() const { return animator_.EffectiveEmotion(); }
    const Avatar3DRenderer& Renderer() const { return renderer_; }

    /// Advances animation & renders modified regions into `framebuffer_`.
    void StepAndRender(float dt_sec, const UiSnapshot& state, std::vector<UiRect>* out_dirty);

    /// Deterministic single-frame helpers for host testing & visual preview rendering.
    void RenderPreviewExpression(AvatarEmotion emotion,
                                 const UiSnapshot& state,
                                 const std::string& caption = "");
    void RenderPreviewDrawer(const UiSnapshot& state);
    void RenderPreviewWifiMenu(const UiSnapshot& state);
    void RenderPreviewWifiKeyboard(const std::string& ssid,
                                     const std::string& password,
                                     const UiSnapshot& state);

private:
    enum class UiView {
        kAvatar = 0,
        kWifiMenu,
        kWifiKeyboard,
    };

    enum class KbLayer {
        kLower = 0,
        kUpper,
        kDigits,
        kSymbols,
    };

    void RenderTopHud(const UiSnapshot& state, AvatarEmotion active_emotion);
    void RenderBottomCaption(const UiSnapshot& state, float audio_energy);
    void RenderDrawerOverlay(const UiSnapshot& state, int top_y);
    void RenderWifiMenu(const UiSnapshot& state);
    void RenderWifiKeyboard(const UiSnapshot& state, int64_t now_ms);
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
    bool has_prev_pose_ = false;
    AvatarPose prev_pose_{};

    // Caption fade state.
    std::string caption_text_;
    float caption_hold_sec_ = 0.0f;
    float caption_alpha_ = 0.0f;
    bool caption_visible_last_ = false;

    // HUD diff tracking so static top bar isn't needlessly re-flushed every frame.
    bool last_wifi_ = false;
    bool last_gateway_ = false;
    WifiUiMode last_wifi_mode_ = WifiUiMode::kOffline;
    int last_bat_pct_ = -1;
    bool last_charging_ = false;
    std::string last_right_pill_;

    // Swipe-down drawer animation state.
    bool drawer_open_ = false;
    float drawer_progress_ = 0.0f; // 0.0 = closed, 1.0 = full open
    float last_drawer_progress_ = 0.0f;

    // Native Wi-Fi configuration / touch keyboard overlay state.
    UiView view_ = UiView::kAvatar;
    int wifi_page_ = 0;
    std::string kb_ssid_;
    std::string kb_password_;
    KbLayer kb_layer_ = KbLayer::kLower;
    int kb_flash_key_ = -1;
    int64_t kb_flash_until_ms_ = 0;
    bool overlay_dirty_ = false;
    uint32_t last_overlay_wifi_rev_ = 0;
    int64_t overlay_clock_ms_ = 0;

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
