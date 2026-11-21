#include "avatar_animator.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace esp_pika {
namespace {

std::string LowerAscii(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

} // namespace

AvatarEmotion ParseAvatarEmotion(const std::string& name) {
    const std::string key = LowerAscii(name);
    if (key.empty() || key == "idle" || key == "neutral" || key == "ready" || key == "normal") {
        return AvatarEmotion::kIdle;
    }
    if (key == "think" || key == "thinking" || key == "processing") {
        return AvatarEmotion::kThinking;
    }
    if (key == "blink" || key == "wink") {
        return AvatarEmotion::kBlink;
    }
    if (key == "look_left" || key == "left" || key == "lookleft") {
        return AvatarEmotion::kLookLeft;
    }
    if (key == "look_right" || key == "right" || key == "lookright") {
        return AvatarEmotion::kLookRight;
    }
    if (key == "listen" || key == "listening" || key == "wake" || key == "wakepending") {
        return AvatarEmotion::kListening;
    }
    if (key == "speak" || key == "speaking" || key == "talk" || key == "talking") {
        return AvatarEmotion::kSpeaking;
    }
    if (key == "turn" || key == "rotate" || key == "side") {
        return AvatarEmotion::kTurn;
    }
    if (key == "curious" || key == "confused" || key == "wonder" || key == "question") {
        return AvatarEmotion::kCurious;
    }
    if (key == "happy" || key == "joyful" || key == "excited" || key == "smile" || key == "joy") {
        return AvatarEmotion::kHappy;
    }
    if (key == "surprised" || key == "surprise" || key == "alert" || key == "shock" ||
        key == "sad" || key == "error") {
        return AvatarEmotion::kSurprised;
    }
    if (key == "sleep" || key == "standby" || key == "offline" || key == "dormant") {
        return AvatarEmotion::kSleep;
    }
    return AvatarEmotion::kIdle;
}

const char* AvatarEmotionLabel(AvatarEmotion emotion) {
    switch (emotion) {
    case AvatarEmotion::kIdle:
        return "IDLE";
    case AvatarEmotion::kThinking:
        return "THINK";
    case AvatarEmotion::kBlink:
        return "BLINK";
    case AvatarEmotion::kLookLeft:
        return "LOOK-L";
    case AvatarEmotion::kLookRight:
        return "LOOK-R";
    case AvatarEmotion::kListening:
        return "LISTEN";
    case AvatarEmotion::kSpeaking:
        return "SPEAK";
    case AvatarEmotion::kTurn:
        return "TURN";
    case AvatarEmotion::kCurious:
        return "CURIOUS";
    case AvatarEmotion::kHappy:
        return "HAPPY";
    case AvatarEmotion::kSurprised:
        return "ALERT";
    case AvatarEmotion::kSleep:
        return "SLEEP";
    }
    return "IDLE";
}

const char* AvatarEmotionDisplayName(AvatarEmotion emotion) {
    switch (emotion) {
    case AvatarEmotion::kIdle:
        return "01 IDLE";
    case AvatarEmotion::kThinking:
        return "02 THINKING";
    case AvatarEmotion::kBlink:
        return "03 BLINK";
    case AvatarEmotion::kLookLeft:
        return "04 LOOK LEFT";
    case AvatarEmotion::kLookRight:
        return "05 LOOK RIGHT";
    case AvatarEmotion::kListening:
        return "06 LISTENING";
    case AvatarEmotion::kSpeaking:
        return "07 SPEAKING";
    case AvatarEmotion::kTurn:
        return "08 TURN";
    case AvatarEmotion::kCurious:
        return "09 CURIOUS (NO PUPIL)";
    case AvatarEmotion::kHappy:
        return "10 HAPPY (NO LINE)";
    case AvatarEmotion::kSurprised:
        return "11 SURPRISED (BLOOM)";
    case AvatarEmotion::kSleep:
        return "12 SLEEP";
    }
    return "IDLE";
}

void AvatarAnimator::SetBaseEmotion(AvatarEmotion emotion) {
    base_emotion_ = emotion;
}

void AvatarAnimator::TriggerTemporaryEmotion(AvatarEmotion emotion, float duration_sec) {
    temp_emotion_ = emotion;
    temp_remaining_sec_ = std::max(0.1f, duration_sec);
}

AvatarEmotion AvatarAnimator::EffectiveEmotion() const {
    return (temp_remaining_sec_ > 0.0f) ? temp_emotion_ : base_emotion_;
}

void AvatarAnimator::SetImuTilt(float tilt_x_g, float tilt_y_g) {
    imu_yaw_deg_ = std::clamp(tilt_x_g * 14.0f, -10.0f, 10.0f);
    imu_roll_deg_ = std::clamp(tilt_y_g * 12.0f, -10.0f, 10.0f);
}

void AvatarAnimator::TriggerShake() {
    TriggerTemporaryEmotion(AvatarEmotion::kSurprised, 1.5f);
    tuft_vel_x_ += 1.1f;
    tuft_vel_y_ -= 0.7f;
}

void AvatarAnimator::LookAtScreenPoint(int x, int y) {
    const float nx = std::clamp((static_cast<float>(x) - 240.0f) / 180.0f, -1.0f, 1.0f);
    const float ny = std::clamp((static_cast<float>(y) - 228.0f) / 180.0f, -1.0f, 1.0f);
    touch_yaw_deg_ = nx * 26.0f;
    touch_pitch_deg_ = -ny * 14.0f;
    touch_gaze_sec_ = 1.8f;
    if (blink_phase_sec_ < 0.0f) {
        blink_phase_sec_ = 0.0f;
    }
}

void AvatarAnimator::SetAudioEnergy(float rms_0_to_1) {
    audio_energy_ = std::clamp(rms_0_to_1, 0.0f, 1.0f);
}

void AvatarAnimator::SmoothToward(float* value, float target, float dt_sec, float rate) {
    const float alpha = 1.0f - std::exp(-rate * std::clamp(dt_sec, 0.001f, 0.1f));
    *value += (target - *value) * alpha;
}

AvatarPose AvatarAnimator::PresetForEmotion(AvatarEmotion emotion) {
    AvatarPose p;
    switch (emotion) {
    case AvatarEmotion::kIdle:
        break;
    case AvatarEmotion::kThinking:
        p.yaw_deg = -9.0f;
        p.pitch_deg = 8.5f;
        p.roll_deg = -6.0f;
        p.upper_lid = 0.16f;
        p.lower_lid = 0.12f;
        p.swirl_amount = 1.0f;
        p.focus_shift_x = -0.22f;
        p.focus_shift_y = -0.24f;
        break;
    case AvatarEmotion::kBlink:
        p.upper_lid = 0.62f;
        p.lower_lid = 0.44f;
        break;
    case AvatarEmotion::kLookLeft:
        p.yaw_deg = -26.0f;
        p.pitch_deg = 2.0f;
        p.roll_deg = -3.0f;
        p.focus_shift_x = -0.34f;
        break;
    case AvatarEmotion::kLookRight:
        p.yaw_deg = 26.0f;
        p.pitch_deg = 2.0f;
        p.roll_deg = 3.0f;
        p.focus_shift_x = 0.34f;
        break;
    case AvatarEmotion::kListening:
        p.scale = 1.045f;
        p.pitch_deg = -4.0f;
        p.eye_aspect_y = 1.08f;
        p.eye_brightness = 1.18f;
        p.shadow_tint = 0.55f;
        break;
    case AvatarEmotion::kSpeaking:
        p.scale = 1.02f;
        p.pitch_deg = 1.5f;
        p.swirl_amount = 0.35f;
        p.eye_brightness = 1.15f;
        p.shadow_tint = 0.50f;
        break;
    case AvatarEmotion::kTurn:
        p.yaw_deg = -37.0f;
        p.pitch_deg = -5.0f;
        p.roll_deg = -4.0f;
        break;
    case AvatarEmotion::kCurious:
        // Strictly pupil-less: asymmetry + tilted head + bright prism refraction focal shift.
        p.yaw_deg = 7.0f;
        p.pitch_deg = 4.0f;
        p.roll_deg = 13.5f;
        p.left_eye_scale = 1.18f;
        p.right_eye_scale = 0.84f;
        p.eye_aspect_y = 1.06f;
        p.focus_shift_x = 0.42f;
        p.focus_shift_y = -0.14f;
        p.eye_brightness = 1.18f;
        p.rim_boost = 1.12f;
        break;
    case AvatarEmotion::kHappy:
        // Strictly no cartoon black lines: 3D obsidian cheeks arch up into glowing rainbow crescents.
        p.pitch_deg = 3.0f;
        p.happy_arch = 0.88f;
        p.lower_lid = 0.06f;
        p.warm_shift = 0.88f;
        p.eye_brightness = 1.24f;
        p.rim_boost = 1.10f;
        break;
    case AvatarEmotion::kSurprised:
        // Strictly no black pupils: wide dilated sockets + radiant cyan-white luminous core bloom.
        p.scale = 1.07f;
        p.pitch_deg = 7.0f;
        p.left_eye_scale = 1.13f;
        p.right_eye_scale = 1.13f;
        p.eye_aspect_y = 1.24f;
        p.core_bloom = 0.88f;
        p.eye_brightness = 1.32f;
        p.rim_boost = 1.42f;
        break;
    case AvatarEmotion::kSleep:
        p.pitch_deg = -13.0f;
        p.roll_deg = 4.0f;
        p.upper_lid = 0.62f;
        p.lower_lid = 0.46f;
        p.rim_boost = 0.56f;
        break;
    }
    return p;
}

AvatarPose AvatarAnimator::PoseForSnapshot(AvatarEmotion emotion, float t_sec) {
    AvatarPose p = PresetForEmotion(emotion);
    p.rainbow_phase = std::fmod(t_sec * 0.25f, 1.0f);
    if (emotion == AvatarEmotion::kSpeaking) {
        p.audio_energy = 0.72f;
        p.bob_y = -4.0f;
        p.eye_brightness += 0.20f;
        p.shadow_tint = 0.82f;
    } else if (emotion == AvatarEmotion::kListening) {
        p.audio_energy = 0.65f;
        p.eye_brightness += 0.22f;
        p.shadow_tint = 0.85f;
    }
    return p;
}

AvatarPose AvatarAnimator::Step(float dt_sec) {
    const float dt = std::clamp(dt_sec, 0.001f, 0.1f);
    time_sec_ += dt;

    if (temp_remaining_sec_ > 0.0f) {
        temp_remaining_sec_ = std::max(0.0f, temp_remaining_sec_ - dt);
    }
    if (touch_gaze_sec_ > 0.0f) {
        touch_gaze_sec_ = std::max(0.0f, touch_gaze_sec_ - dt);
    }

    const AvatarEmotion emotion = EffectiveEmotion();
    AvatarPose target = PresetForEmotion(emotion);

    // Subtle organic breathing and ambient life motion.
    if (emotion == AvatarEmotion::kSleep) {
        target.bob_y = std::sin(time_sec_ * 1.25f) * 2.5f;
    } else if (emotion == AvatarEmotion::kHappy) {
        target.bob_y = -std::abs(std::sin(time_sec_ * 5.2f)) * 7.5f;
    } else if (emotion == AvatarEmotion::kSpeaking) {
        target.bob_y = std::sin(time_sec_ * 4.5f) * 3.0f - audio_energy_ * 6.5f;
        target.eye_brightness += audio_energy_ * 0.30f;
        target.shadow_tint = std::max(target.shadow_tint, 0.35f + audio_energy_ * 0.65f);
    } else if (emotion == AvatarEmotion::kListening) {
        target.bob_y = std::sin(time_sec_ * 2.2f) * 3.5f;
        target.eye_brightness += audio_energy_ * 0.36f;
        target.shadow_tint = std::max(target.shadow_tint, 0.40f + audio_energy_ * 0.60f);
    } else {
        target.bob_y = std::sin(time_sec_ * 1.85f) * 4.5f;
    }
    target.audio_energy = audio_energy_;

    // Subtle idle micro-glance every few seconds when calm.
    if (emotion == AvatarEmotion::kIdle) {
        glance_cooldown_sec_ -= dt;
        if (glance_cooldown_sec_ <= 0.0f) {
            const float cyc = std::sin(time_sec_ * 2.3f);
            glance_yaw_deg_ = (cyc > 0.3f) ? 6.5f : ((cyc < -0.3f) ? -6.5f : 0.0f);
            glance_cooldown_sec_ = 4.2f;
        }
    } else {
        glance_yaw_deg_ = 0.0f;
    }

    // Add IMU parallax & active touch gaze.
    if (emotion != AvatarEmotion::kTurn && emotion != AvatarEmotion::kSleep) {
        target.yaw_deg += imu_yaw_deg_ * 0.65f + glance_yaw_deg_;
        target.roll_deg += imu_roll_deg_ * 0.55f;
    }
    if (touch_gaze_sec_ > 0.0f && emotion != AvatarEmotion::kSleep) {
        target.yaw_deg = std::clamp(target.yaw_deg + touch_yaw_deg_, -36.0f, 36.0f);
        target.pitch_deg = std::clamp(target.pitch_deg + touch_pitch_deg_, -18.0f, 18.0f);
        target.focus_shift_x = std::clamp(touch_yaw_deg_ / 65.0f, -0.35f, 0.35f);
    }

    // Natural autonomous blinks in open-eye states.
    if (emotion != AvatarEmotion::kBlink && emotion != AvatarEmotion::kSleep &&
        emotion != AvatarEmotion::kHappy) {
        blink_cooldown_sec_ -= dt;
        if (blink_cooldown_sec_ <= 0.0f && blink_phase_sec_ < 0.0f) {
            blink_phase_sec_ = 0.0f;
            const float jitter = 0.5f + 0.5f * std::sin(time_sec_ * 1.73f);
            blink_cooldown_sec_ = 3.4f + jitter * 2.4f;
        }
        if (blink_phase_sec_ >= 0.0f) {
            blink_phase_sec_ += dt;
            constexpr float kCloseDur = 0.11f;
            constexpr float kHoldDur = 0.04f;
            constexpr float kOpenDur = 0.14f;
            float blink_amt = 0.0f;
            if (blink_phase_sec_ < kCloseDur) {
                blink_amt = blink_phase_sec_ / kCloseDur;
            } else if (blink_phase_sec_ < kCloseDur + kHoldDur) {
                blink_amt = 1.0f;
            } else if (blink_phase_sec_ < kCloseDur + kHoldDur + kOpenDur) {
                blink_amt = 1.0f - (blink_phase_sec_ - kCloseDur - kHoldDur) / kOpenDur;
            } else {
                blink_phase_sec_ = -1.0f;
            }
            target.upper_lid = std::max(target.upper_lid, blink_amt * 0.62f);
            target.lower_lid = std::max(target.lower_lid, blink_amt * 0.44f);
        }
    }

    // Continuous holographic prism phase flow (faster when thinking).
    const float rainbow_speed = (emotion == AvatarEmotion::kThinking) ? 0.42f : 0.10f;
    current_.rainbow_phase = std::fmod(current_.rainbow_phase + rainbow_speed * dt, 1.0f);

    const float fast_rate = 14.0f;
    const float body_rate = 9.5f;
    SmoothToward(&current_.yaw_deg, target.yaw_deg, dt, body_rate);
    SmoothToward(&current_.pitch_deg, target.pitch_deg, dt, body_rate);
    SmoothToward(&current_.roll_deg, target.roll_deg, dt, body_rate);
    SmoothToward(&current_.scale, target.scale, dt, body_rate);
    SmoothToward(&current_.bob_y, target.bob_y, dt, fast_rate);
    SmoothToward(&current_.left_eye_scale, target.left_eye_scale, dt, fast_rate);
    SmoothToward(&current_.right_eye_scale, target.right_eye_scale, dt, fast_rate);
    SmoothToward(&current_.eye_aspect_y, target.eye_aspect_y, dt, fast_rate);
    SmoothToward(&current_.upper_lid, target.upper_lid, dt, 26.0f);
    SmoothToward(&current_.lower_lid, target.lower_lid, dt, 26.0f);
    SmoothToward(&current_.happy_arch, target.happy_arch, dt, fast_rate);
    SmoothToward(&current_.focus_shift_x, target.focus_shift_x, dt, fast_rate);
    SmoothToward(&current_.focus_shift_y, target.focus_shift_y, dt, fast_rate);
    SmoothToward(&current_.swirl_amount, target.swirl_amount, dt, 8.0f);
    SmoothToward(&current_.eye_brightness, target.eye_brightness, dt, fast_rate);
    SmoothToward(&current_.core_bloom, target.core_bloom, dt, 18.0f);
    SmoothToward(&current_.warm_shift, target.warm_shift, dt, 10.0f);
    SmoothToward(&current_.rim_boost, target.rim_boost, dt, 8.0f);
    SmoothToward(&current_.audio_energy, target.audio_energy, dt, 20.0f);
    SmoothToward(&current_.shadow_tint, target.shadow_tint, dt, 10.0f);

    // Secondary underdamped spring lag for crown flame tuft driven by angular motion.
    const float d_yaw = (current_.yaw_deg - prev_yaw_deg_) / dt;
    const float d_roll = (current_.roll_deg - prev_roll_deg_) / dt;
    const float d_pitch = (current_.pitch_deg - prev_pitch_deg_) / dt;
    prev_yaw_deg_ = current_.yaw_deg;
    prev_pitch_deg_ = current_.pitch_deg;
    prev_roll_deg_ = current_.roll_deg;

    const float target_lag_x = std::clamp(-d_yaw * 0.0028f - d_roll * 0.0022f, -0.18f, 0.18f);
    const float target_lag_y = std::clamp(d_pitch * 0.0025f, -0.14f, 0.14f);
    const float kSpring = 52.0f;
    const float kDamp = 9.0f;
    const float ax = kSpring * (target_lag_x - current_.tuft_lag_x) - kDamp * tuft_vel_x_;
    const float ay = kSpring * (target_lag_y - current_.tuft_lag_y) - kDamp * tuft_vel_y_;
    tuft_vel_x_ += ax * dt;
    tuft_vel_y_ += ay * dt;
    current_.tuft_lag_x = std::clamp(current_.tuft_lag_x + tuft_vel_x_ * dt, -0.22f, 0.22f);
    current_.tuft_lag_y = std::clamp(current_.tuft_lag_y + tuft_vel_y_ * dt, -0.16f, 0.16f);

    return current_;
}

} // namespace esp_pika
