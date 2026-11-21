#include "mosaico_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#include "ui_font.h"

namespace esp_pika {
namespace {

constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorHudMuted = MakeRgb565(138, 148, 174);
constexpr uint16_t kColorHudBright = MakeRgb565(226, 232, 248);
constexpr uint16_t kColorPillBg = MakeRgb565(16, 18, 28);
constexpr uint16_t kColorPillBorder = MakeRgb565(48, 54, 82);
constexpr uint16_t kColorAccentViolet = MakeRgb565(156, 128, 245);
constexpr uint16_t kColorAccentCyan = MakeRgb565(56, 224, 240);
constexpr uint16_t kColorAccentMint = MakeRgb565(52, 211, 153);
constexpr uint16_t kColorAccentAmber = MakeRgb565(245, 180, 62);
constexpr uint16_t kColorDrawerBg = MakeRgb565(11, 13, 21);
constexpr uint16_t kColorCardBg = MakeRgb565(20, 24, 38);
constexpr uint16_t kColorTrackBg = MakeRgb565(34, 40, 60);

struct QuickExprButton {
    const char* label;
    AvatarEmotion emotion;
};

constexpr QuickExprButton kQuickButtons[] = {
    {"IDLE", AvatarEmotion::kIdle},       {"HAPPY", AvatarEmotion::kHappy},
    {"THINK", AvatarEmotion::kThinking},  {"CURIOUS", AvatarEmotion::kCurious},
    {"ALERT", AvatarEmotion::kSurprised}, {"SLEEP", AvatarEmotion::kSleep},
};

} // namespace

MosaicoUi::~MosaicoUi() {
#ifdef ESP_PLATFORM
    if (framebuffer_ != nullptr) {
        heap_caps_free(framebuffer_);
        framebuffer_ = nullptr;
    }
#endif
}

bool MosaicoUi::Init(UiCallbacks callbacks) {
    if (ready_) {
        return true;
    }
    callbacks_ = std::move(callbacks);
    if (!renderer_.Init()) {
        return false;
    }
    const size_t px_count = static_cast<size_t>(kWidth) * kHeight;
#ifdef ESP_PLATFORM
    framebuffer_ = static_cast<uint16_t*>(
        heap_caps_malloc(px_count * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (framebuffer_ == nullptr) {
        return false;
    }
#else
    host_fb_.assign(px_count, kColorBlack);
    framebuffer_ = host_fb_.data();
#endif
    ClearFramebuffer();
    force_full_refresh_ = true;
    ready_ = true;
    return true;
}

void MosaicoUi::ClearFramebuffer() {
    if (framebuffer_ != nullptr) {
        std::fill_n(framebuffer_, static_cast<size_t>(kWidth) * kHeight, kColorBlack);
    }
}

void MosaicoUi::SetBaseEmotion(AvatarEmotion emotion) {
    animator_.SetBaseEmotion(emotion);
}

void MosaicoUi::ShowMessage(const std::string& emotion, const std::string& text, float hold_sec) {
    const AvatarEmotion parsed = ParseAvatarEmotion(emotion);
    animator_.TriggerTemporaryEmotion(parsed, std::max(2.5f, hold_sec));
    const std::string clean = ui_gfx::NormalizeAsciiText(text);
    if (!clean.empty()) {
        caption_text_ = clean;
        caption_hold_sec_ = hold_sec;
    }
}

void MosaicoUi::SetImuTilt(float accel_x, float accel_y) {
    animator_.SetImuTilt(accel_x, accel_y);
}

void MosaicoUi::TriggerShake() {
    animator_.TriggerShake();
    caption_text_ = "WHOA! EASY THERE";
    caption_hold_sec_ = 1.6f;
}

void MosaicoUi::SetAudioEnergy(float rms_0_to_1) {
    animator_.SetAudioEnergy(rms_0_to_1);
}

void MosaicoUi::OnTouch(const TouchPoint& tp, int64_t now_ms, UiSnapshot* mutable_state) {
    if (!tp.pressed) {
        if (!touch_down_) {
            return;
        }
        touch_down_ = false;
        const int dy = cur_y_ - down_y_;
        const int dx = cur_x_ - down_x_;
        const bool was_tap =
            !long_press_fired_ && !swipe_handled_ && (std::abs(dx) < 20 && std::abs(dy) < 20);

        if (drawer_open_) {
            if (dy < -45 || (was_tap && cur_y_ > 426)) {
                drawer_open_ = false;
                force_full_refresh_ = true;
            } else if (was_tap && cur_y_ >= 344 && cur_y_ <= 416) {
                const int col = std::clamp((cur_x_ - 36) / 138, 0, 2);
                const int row = (cur_y_ >= 382) ? 1 : 0;
                const size_t idx = static_cast<size_t>(row * 3 + col);
                if (idx < sizeof(kQuickButtons) / sizeof(kQuickButtons[0])) {
                    const AvatarEmotion chosen = kQuickButtons[idx].emotion;
                    animator_.SetBaseEmotion(chosen);
                    if (callbacks_.on_quick_emotion) {
                        callbacks_.on_quick_emotion(chosen);
                    }
                    drawer_open_ = false;
                    force_full_refresh_ = true;
                }
            }
        } else if (was_tap) {
            const bool near_orb = (std::abs(cur_x_ - 240) < 105 && std::abs(cur_y_ - 228) < 105);
            const bool is_double_tap =
                near_orb && (last_tap_ms_ > 0) && (now_ms - last_tap_ms_ <= 380) &&
                (std::abs(cur_x_ - last_tap_x_) < 60 && std::abs(cur_y_ - last_tap_y_) < 60);
            if (is_double_tap) {
                animator_.TriggerTemporaryEmotion(AvatarEmotion::kHappy, 2.2f);
                last_tap_ms_ = 0;
            } else {
                animator_.LookAtScreenPoint(cur_x_, cur_y_);
                last_tap_ms_ = now_ms;
                last_tap_x_ = cur_x_;
                last_tap_y_ = cur_y_;
            }
        }
        return;
    }

    cur_x_ = tp.x;
    cur_y_ = tp.y;

    if (!touch_down_) {
        touch_down_ = true;
        down_x_ = tp.x;
        down_y_ = tp.y;
        down_ms_ = now_ms;
        long_press_fired_ = false;
        swipe_handled_ = false;
    }

    const int dy = cur_y_ - down_y_;
    if (!drawer_open_) {
        if (down_y_ < 92 && dy > 44) {
            drawer_open_ = true;
            swipe_handled_ = true;
            force_full_refresh_ = true;
            return;
        }
        const bool near_orb = (std::abs(down_x_ - 240) < 100 && std::abs(down_y_ - 228) < 100);
        if (!long_press_fired_ && near_orb && std::abs(dy) < 22 && (now_ms - down_ms_ >= 650)) {
            long_press_fired_ = true;
            if (callbacks_.on_long_press_wake) {
                callbacks_.on_long_press_wake();
            }
        } else if (!long_press_fired_ && std::abs(dy) < 35) {
            animator_.LookAtScreenPoint(cur_x_, cur_y_);
        }
    } else if (drawer_progress_ > 0.85f) {
        if (dy < -54) {
            drawer_open_ = false;
            swipe_handled_ = true;
            force_full_refresh_ = true;
            return;
        }
        // Live slider adjustments inside control drawer (x track: 108..424).
        constexpr int kTrackX0 = 108;
        constexpr int kTrackX1 = 424;
        if (cur_y_ >= 224 && cur_y_ <= 264) {
            const int pct = std::clamp(((cur_x_ - kTrackX0) * 100) / (kTrackX1 - kTrackX0), 0, 100);
            if (mutable_state != nullptr) {
                mutable_state->volume = pct;
            }
            if (callbacks_.on_volume_change) {
                callbacks_.on_volume_change(pct);
            }
            swipe_handled_ = true;
        } else if (cur_y_ >= 278 && cur_y_ <= 318) {
            const int pct = std::clamp(
                15 + ((cur_x_ - kTrackX0) * 85) / (kTrackX1 - kTrackX0), 15, 100);
            if (mutable_state != nullptr) {
                mutable_state->brightness = pct;
            }
            if (callbacks_.on_brightness_change) {
                callbacks_.on_brightness_change(static_cast<uint8_t>(pct));
            }
            swipe_handled_ = true;
        }
    }
}

void MosaicoUi::RenderTopHud(const UiSnapshot& state, AvatarEmotion active_emotion) {
    ui_gfx::FillRect(framebuffer_, kWidth, kHeight, 0, 0, kWidth, 44, kColorBlack, 255);

    // Subtle inset for circular/rounded-corner AMOLED bezels.
    const uint16_t wifi_col = state.wifi_connected ? kColorHudBright : kColorHudMuted;
    ui_gfx::DrawWifiIcon(framebuffer_, kWidth, kHeight, 44, 15, state.wifi_connected, wifi_col);

    const uint16_t link_col = state.gateway_connected ? kColorAccentCyan : kColorHudMuted;
    ui_gfx::DrawLinkIcon(framebuffer_, kWidth, kHeight, 70, 15, state.gateway_connected, link_col);

    const bool charging = state.battery_current_ma > 20.0f;
    uint16_t bat_col = kColorAccentMint;
    if (state.battery_valid && state.battery_percent < 20) {
        bat_col = MakeRgb565(248, 113, 113);
    } else if (state.battery_valid && state.battery_percent < 40) {
        bat_col = kColorAccentAmber;
    }
    ui_gfx::DrawBatteryIcon(framebuffer_, kWidth, kHeight, 94, 15,
                            state.battery_valid ? state.battery_percent : 100, charging,
                            kColorHudMuted, bat_col);
    if (state.battery_valid) {
        char pct_buf[12] = {};
        std::snprintf(pct_buf, sizeof(pct_buf), "%d%%", std::clamp(state.battery_percent, 0, 100));
        ui_gfx::DrawText(framebuffer_, kWidth, kHeight, 126, 18, pct_buf, 1, kColorHudMuted, 220);
    }

    // Right status badge matching user's `[SESSION-01]` style cleanly inside safe bezel.
    std::string right_text = state.status_tag;
    if (right_text.empty()) {
        if (active_emotion == AvatarEmotion::kIdle && !state.session_tag.empty()) {
            right_text = state.session_tag;
        } else {
            right_text = AvatarEmotionLabel(active_emotion);
        }
    }
    const std::string pill = "[" + right_text + "]";
    const int scale = 2;
    const int text_w = static_cast<int>(pill.size()) * (6 * scale) - scale;
    const int rx = std::max(230, 434 - text_w);
    uint16_t pill_col = kColorHudMuted;
    if (active_emotion == AvatarEmotion::kListening) {
        pill_col = kColorAccentCyan;
    } else if (active_emotion == AvatarEmotion::kSpeaking || active_emotion == AvatarEmotion::kHappy) {
        pill_col = kColorAccentMint;
    } else if (active_emotion == AvatarEmotion::kThinking || active_emotion == AvatarEmotion::kCurious) {
        pill_col = kColorAccentViolet;
    } else if (active_emotion == AvatarEmotion::kSurprised) {
        pill_col = kColorAccentAmber;
    }
    ui_gfx::DrawText(framebuffer_, kWidth, kHeight, rx, 14, pill, scale, pill_col, 225);
}

void MosaicoUi::RenderBottomCaption(const UiSnapshot& /*state*/, float audio_energy) {
    ui_gfx::FillRect(framebuffer_, kWidth, kHeight, 24, 390, 432, 86, kColorBlack, 255);

    // Subtle 2px voice activity shimmer bar right beneath the avatar.
    if (audio_energy > 0.04f) {
        const int bar_w = std::clamp(static_cast<int>(40.0f + audio_energy * 220.0f), 40, 260);
        const int x0 = 240 - bar_w / 2;
        ui_gfx::FillRoundedRect(framebuffer_, kWidth, kHeight, x0, 394, bar_w, 3, 1,
                                kColorAccentCyan, static_cast<uint8_t>(110 + audio_energy * 145));
    }

    if (caption_alpha_ <= 0.02f || caption_text_.empty()) {
        return;
    }
    const uint8_t alpha = static_cast<uint8_t>(std::clamp(caption_alpha_, 0.0f, 1.0f) * 255.0f);
    ui_gfx::FillRoundedRect(framebuffer_, kWidth, kHeight, 44, 404, 392, 54, 16, kColorPillBg, alpha);
    ui_gfx::StrokeRoundedRect(framebuffer_, kWidth, kHeight, 44, 404, 392, 54, 16, kColorPillBorder,
                              alpha);

    const auto lines = ui_gfx::WrapAsciiText(caption_text_, 28, 2);
    const int scale = 2;
    const int line_h = 18;
    const int block_h = static_cast<int>(lines.size()) * line_h - 4;
    int y = 404 + std::max(8, (54 - block_h) / 2);
    for (const auto& line : lines) {
        ui_gfx::DrawCenteredText(framebuffer_, kWidth, kHeight, 240, y, line, scale, kColorHudBright,
                                 alpha);
        y += line_h;
    }
}

void MosaicoUi::RenderDrawerOverlay(const UiSnapshot& state, int height_px) {
    if (height_px <= 8) {
        return;
    }
    const int h = std::min(kHeight, height_px);
    ui_gfx::FillRect(framebuffer_, kWidth, kHeight, 0, 0, kWidth, h, kColorDrawerBg, 255);
    if (h < 180) {
        return;
    }

    // Header strip.
    ui_gfx::DrawCenteredText(framebuffer_, kWidth, kHeight, 240, 22, "MOSAICO CONTROL & TELEMETRY", 2,
                             kColorAccentViolet, 240);

    // Card 1: Power & Thermal (BQ27220) + Motion (BMI270).
    ui_gfx::FillRoundedRect(framebuffer_, kWidth, kHeight, 36, 54, 408, 152, 16, kColorCardBg, 255);
    ui_gfx::StrokeRoundedRect(framebuffer_, kWidth, kHeight, 36, 54, 408, 152, 16, kColorPillBorder,
                              255);

    char line1[64] = {};
    if (state.battery_valid) {
        std::snprintf(line1, sizeof(line1), "BAT: %d%%  %.2fV  %.0fmA  TEMP: %.1fC",
                      state.battery_percent, state.battery_voltage, state.battery_current_ma,
                      state.temperature_c);
    } else {
        std::snprintf(line1, sizeof(line1), "BAT: USB POWER / N/A");
    }
    ui_gfx::DrawText(framebuffer_, kWidth, kHeight, 54, 72, line1, 1, kColorHudBright, 240);

    char line2[64] = {};
    if (state.imu_valid) {
        std::snprintf(line2, sizeof(line2), "IMU: X=%+.2fg  Y=%+.2fg  Z=%+.2fg", state.accel_x,
                      state.accel_y, state.accel_z);
    } else {
        std::snprintf(line2, sizeof(line2), "IMU: BMI270 STANDBY");
    }
    ui_gfx::DrawText(framebuffer_, kWidth, kHeight, 54, 96, line2, 1, kColorAccentCyan, 230);

    // Small live IMU tilt bubble indicator on right side of telemetry card.
    ui_gfx::StrokeRoundedRect(framebuffer_, kWidth, kHeight, 378, 72, 48, 48, 24, kColorPillBorder,
                              255);
    const int dot_x = 402 + std::clamp(static_cast<int>(state.accel_x * 16.0f), -16, 16);
    const int dot_y = 96 + std::clamp(static_cast<int>(state.accel_y * 16.0f), -16, 16);
    ui_gfx::FillCircle(framebuffer_, kWidth, kHeight, dot_x, dot_y, 5, kColorAccentCyan, 255);

    char line3[64] = {};
    std::snprintf(line3, sizeof(line3), "NET: WIFI=%s  GW=%s  MAC=%s",
                  state.wifi_connected ? "OK" : "OFF", state.gateway_connected ? "OK" : "OFF",
                  state.mac.c_str());
    ui_gfx::DrawText(framebuffer_, kWidth, kHeight, 54, 128, line3, 1, kColorHudMuted, 225);

    ui_gfx::DrawText(framebuffer_, kWidth, kHeight, 54, 154, "AGENT: DEEPSEEK-FLASH // MCP: ACTIVE",
                     1, kColorAccentMint, 220);

    // Sliders: Speaker Volume + AMOLED Brightness.
    constexpr int kTrackX = 108;
    constexpr int kTrackW = 316;
    auto draw_slider = [&](int y, const char* val_txt, int pct, uint16_t fill_col,
                           bool is_volume) {
        if (is_volume) {
            ui_gfx::DrawVolumeIcon(framebuffer_, kWidth, kHeight, 52, y + 6, kColorHudBright);
        } else {
            ui_gfx::DrawBrightnessIcon(framebuffer_, kWidth, kHeight, 52, y + 6, kColorHudBright);
        }
        ui_gfx::FillRoundedRect(framebuffer_, kWidth, kHeight, kTrackX, y + 8, kTrackW, 14, 7,
                                kColorTrackBg, 255);
        const int fill_w = std::max(14, (kTrackW * std::clamp(pct, 0, 100)) / 100);
        ui_gfx::FillRoundedRect(framebuffer_, kWidth, kHeight, kTrackX, y + 8, fill_w, 14, 7,
                                fill_col, 255);
        ui_gfx::FillCircle(framebuffer_, kWidth, kHeight, kTrackX + fill_w - 4, y + 15, 10,
                           kColorHudBright, 255);
        ui_gfx::DrawText(framebuffer_, kWidth, kHeight, 434, y + 11, val_txt, 1, kColorHudMuted,
                         220);
    };

    char vol_buf[8] = {};
    std::snprintf(vol_buf, sizeof(vol_buf), "%d", state.volume);
    draw_slider(228, vol_buf, state.volume, kColorAccentViolet, true);

    char bri_buf[8] = {};
    std::snprintf(bri_buf, sizeof(bri_buf), "%d", state.brightness);
    draw_slider(282, bri_buf, state.brightness, kColorAccentCyan, false);

    // Quick expression preset buttons (2 rows x 3 columns).
    for (size_t i = 0; i < sizeof(kQuickButtons) / sizeof(kQuickButtons[0]); ++i) {
        const int col = static_cast<int>(i % 3);
        const int row = static_cast<int>(i / 3);
        const int bx = 42 + col * 136;
        const int by = 344 + row * 38;
        const bool active = (animator_.EffectiveEmotion() == kQuickButtons[i].emotion);
        ui_gfx::FillRoundedRect(framebuffer_, kWidth, kHeight, bx, by, 124, 30, 10,
                                active ? kColorAccentViolet : kColorCardBg, 255);
        ui_gfx::StrokeRoundedRect(framebuffer_, kWidth, kHeight, bx, by, 124, 30, 10,
                                  kColorPillBorder, 255);
        ui_gfx::DrawCenteredText(framebuffer_, kWidth, kHeight, bx + 62, by + 11,
                                 kQuickButtons[i].label, 1,
                                 active ? kColorBlack : kColorHudBright, 245);
    }

    // Bottom swipe-up close pill.
    ui_gfx::FillRoundedRect(framebuffer_, kWidth, kHeight, 190, 444, 100, 6, 3, kColorHudMuted, 180);
    ui_gfx::DrawCenteredText(framebuffer_, kWidth, kHeight, 240, 458, "SWIPE UP TO CLOSE", 1,
                             kColorHudMuted, 190);
}

void MosaicoUi::StepAndRender(float dt_sec, const UiSnapshot& state, std::vector<UiRect>* out_dirty) {
    if (!ready_ || out_dirty == nullptr) {
        return;
    }
    out_dirty->clear();

    const AvatarPose pose = animator_.Step(dt_sec);
    const AvatarEmotion active_emotion = animator_.EffectiveEmotion();

    // Update caption hold & smooth fade.
    if (caption_hold_sec_ > 0.0f) {
        caption_hold_sec_ = std::max(0.0f, caption_hold_sec_ - dt_sec);
        caption_alpha_ = std::min(1.0f, caption_alpha_ + dt_sec * 5.0f);
    } else {
        caption_alpha_ = std::max(0.0f, caption_alpha_ - dt_sec * 2.5f);
    }

    // Update swipe-down drawer animation.
    const float drawer_target = drawer_open_ ? 1.0f : 0.0f;
    drawer_progress_ += (drawer_target - drawer_progress_) * std::min(1.0f, dt_sec * 12.0f);
    if (std::abs(drawer_target - drawer_progress_) < 0.01f) {
        drawer_progress_ = drawer_target;
    }

    const bool drawer_animating = std::abs(drawer_progress_ - last_drawer_progress_) > 0.005f;
    if (force_full_refresh_) {
        ClearFramebuffer();
    }

    if (drawer_progress_ >= 0.99f && !force_full_refresh_ && !drawer_animating) {
        RenderDrawerOverlay(state, kHeight);
        out_dirty->push_back({0, 0, kWidth, 340});
        last_drawer_progress_ = drawer_progress_;
        return;
    }

    // Check top HUD changes.
    std::string right_pill = state.status_tag;
    if (right_pill.empty()) {
        right_pill = (active_emotion == AvatarEmotion::kIdle && !state.session_tag.empty())
                         ? state.session_tag
                         : std::string(AvatarEmotionLabel(active_emotion));
    }
    const bool charging = state.battery_current_ma > 20.0f;
    const bool hud_changed =
        force_full_refresh_ || (state.wifi_connected != last_wifi_) ||
        (state.gateway_connected != last_gateway_) ||
        (state.battery_percent != last_bat_pct_) || (charging != last_charging_) ||
        (right_pill != last_right_pill_);

    if (hud_changed || drawer_animating) {
        RenderTopHud(state, active_emotion);
        last_wifi_ = state.wifi_connected;
        last_gateway_ = state.gateway_connected;
        last_bat_pct_ = state.battery_percent;
        last_charging_ = charging;
        last_right_pill_ = right_pill;
        if (!force_full_refresh_ && !drawer_animating) {
            out_dirty->push_back({0, 0, kWidth, 44});
        }
    }

    // Render 3D digital life avatar in central stage.
    const UiRect avatar_rect = renderer_.Render(RawFb(), kWidth, kHeight, pose, 240, 224, 102.0f);
    if (!force_full_refresh_ && !drawer_animating) {
        out_dirty->push_back(avatar_rect);
    }

    // Bottom caption + audio bar.
    const bool caption_active = (caption_alpha_ > 0.01f) || (pose.audio_energy > 0.04f);
    if (force_full_refresh_ || caption_active || caption_visible_last_ || drawer_animating) {
        RenderBottomCaption(state, pose.audio_energy);
        caption_visible_last_ = caption_active;
        if (!force_full_refresh_ && !drawer_animating) {
            out_dirty->push_back({24, 390, 432, 86});
        }
    }

    if (drawer_progress_ > 0.01f) {
        RenderDrawerOverlay(state, static_cast<int>(drawer_progress_ * kHeight));
    }

    if (force_full_refresh_ || drawer_animating) {
        out_dirty->clear();
        out_dirty->push_back({0, 0, kWidth, kHeight});
        force_full_refresh_ = false;
    }
    last_drawer_progress_ = drawer_progress_;
}

void MosaicoUi::RenderPreviewExpression(AvatarEmotion emotion,
                                        const UiSnapshot& state,
                                        const std::string& caption) {
    if (!ready_) {
        return;
    }
    ClearFramebuffer();
    const AvatarPose pose = AvatarAnimator::PoseForSnapshot(emotion, 0.25f);
    RenderTopHud(state, emotion);
    renderer_.Render(RawFb(), kWidth, kHeight, pose, 240, 224, 102.0f);
    caption_text_ = ui_gfx::NormalizeAsciiText(caption);
    caption_alpha_ = caption_text_.empty() ? 0.0f : 1.0f;
    RenderBottomCaption(state, pose.audio_energy);
}

void MosaicoUi::RenderPreviewDrawer(const UiSnapshot& state) {
    if (!ready_) {
        return;
    }
    ClearFramebuffer();
    RenderDrawerOverlay(state, kHeight);
}

} // namespace esp_pika
