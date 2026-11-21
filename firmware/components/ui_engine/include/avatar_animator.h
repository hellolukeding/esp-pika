#pragma once

#include <cstdint>

#include "avatar_types.h"

namespace esp_pika {

/// Animates smooth 3D transitions, natural blinks, spring lag on the crown tuft,
/// IMU tilt parallax, touch gaze, and voice-reactive spectral modulation across all 12 expressions.
class AvatarAnimator {
public:
    AvatarAnimator() = default;

    void SetBaseEmotion(AvatarEmotion emotion);
    void TriggerTemporaryEmotion(AvatarEmotion emotion, float duration_sec);
    AvatarEmotion EffectiveEmotion() const;

    /// Applies gentle IMU gravity tilt (roll/yaw parallax in normalized g units).
    void SetImuTilt(float tilt_x_g, float tilt_y_g);

    /// Triggers an immediate startled expression upon sudden device shake/pickup.
    void TriggerShake();

    /// Steers 3D gaze toward a touched coordinate on the 480x480 screen.
    void LookAtScreenPoint(int x, int y);

    /// Updates live voice amplitude in [0.0, 1.0].
    void SetAudioEnergy(float rms_0_to_1);

    /// Advances physics & interpolation by `dt_sec` and returns the current pose.
    AvatarPose Step(float dt_sec);

    /// Returns a deterministic snapshot pose for any of the 12 canonical expressions.
    static AvatarPose PoseForSnapshot(AvatarEmotion emotion, float t_sec = 0.25f);

private:
    static AvatarPose PresetForEmotion(AvatarEmotion emotion);
    static void SmoothToward(float* value, float target, float dt_sec, float rate);

    AvatarEmotion base_emotion_ = AvatarEmotion::kIdle;
    AvatarEmotion temp_emotion_ = AvatarEmotion::kIdle;
    float temp_remaining_sec_ = 0.0f;

    float time_sec_ = 0.0f;
    float blink_cooldown_sec_ = 3.2f;
    float blink_phase_sec_ = -1.0f;
    float glance_cooldown_sec_ = 4.5f;
    float glance_yaw_deg_ = 0.0f;

    float imu_yaw_deg_ = 0.0f;
    float imu_roll_deg_ = 0.0f;
    float touch_gaze_sec_ = 0.0f;
    float touch_yaw_deg_ = 0.0f;
    float touch_pitch_deg_ = 0.0f;

    float audio_energy_ = 0.0f;

    AvatarPose current_;
    float prev_yaw_deg_ = 0.0f;
    float prev_pitch_deg_ = 0.0f;
    float prev_roll_deg_ = 0.0f;
    float tuft_vel_x_ = 0.0f;
    float tuft_vel_y_ = 0.0f;
};

} // namespace esp_pika
