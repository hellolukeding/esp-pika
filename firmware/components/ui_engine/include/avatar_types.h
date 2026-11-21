#pragma once

#include <cstdint>
#include <string>

namespace esp_pika {

/// Axis-aligned rectangle in display pixel coordinates.
struct UiRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool Empty() const { return w <= 0 || h <= 0; }
};

/// 12-expression taxonomy for the pupil-less 3D digital life avatar.
enum class AvatarEmotion : uint8_t {
    kIdle = 0,
    kThinking,
    kBlink,
    kLookLeft,
    kLookRight,
    kListening,
    kSpeaking,
    kTurn,
    kCurious,
    kHappy,
    kSurprised,
    kSleep,
};

constexpr size_t kAvatarEmotionCount = 12;

/// Maps semantic emotion strings (e.g. from DeepSeek / MCP display_show) to AvatarEmotion.
AvatarEmotion ParseAvatarEmotion(const std::string& name);

/// Returns short uppercase HUD tag matching the expression (e.g. "IDLE", "THINK", "HAPPY").
const char* AvatarEmotionLabel(AvatarEmotion emotion);

/// Returns display-friendly title label for host previews / drawer pills.
const char* AvatarEmotionDisplayName(AvatarEmotion emotion);

/// Continuous 3D rigid pose + spring secondary motion + shader parameters.
struct AvatarPose {
    float yaw_deg = 0.0f;    // Positive rotates nose right
    float pitch_deg = 0.0f;  // Positive tilts upward
    float roll_deg = 0.0f;   // Positive tilts clockwise
    float scale = 1.0f;
    float bob_y = 0.0f;      // Vertical float offset in px

    // Second-order spring lag for top curved flame tuft.
    float tuft_lag_x = 0.0f;
    float tuft_lag_y = 0.0f;

    // Asymmetric eye sizing & aperture (strictly pupil-less).
    float left_eye_scale = 1.0f;
    float right_eye_scale = 1.0f;
    float eye_aspect_y = 1.0f;
    float upper_lid = 0.0f;   // [0..1]
    float lower_lid = 0.0f;   // [0..1]
    float happy_arch = 0.0f;  // [0..1] lower cheek arch carving glowing rainbow crescents

    // Optical prism focus shift & holographic aurora controls (never black dots).
    float focus_shift_x = 0.0f;
    float focus_shift_y = 0.0f;
    float rainbow_phase = 0.0f;
    float swirl_amount = 0.0f;
    float eye_brightness = 1.0f;
    float core_bloom = 0.0f;
    float warm_shift = 0.0f;
    float rim_boost = 1.0f;
    float audio_energy = 0.0f;
    float shadow_tint = 0.0f;
};

constexpr uint16_t MakeRgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

} // namespace esp_pika
