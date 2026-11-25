#include "ui_service.h"

#include <algorithm>
#include <cmath>
#include <vector>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#endif

namespace esp_pika {
namespace {

#ifdef ESP_PLATFORM
constexpr const char* kTag = "ui_service";
constexpr UBaseType_t kUiCoreId = 1;
#endif

} // namespace

UiService::~UiService() {
    Stop();
}

void UiService::SetWifiCallbacks(UiWifiCallbacks callbacks) {
    std::lock_guard<std::mutex> lock(mu_);
    wifi_callbacks_ = std::move(callbacks);
}

void UiService::SetEzUnpairCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mu_);
    on_ez_unpair_ = std::move(cb);
}

bool UiService::Start(Board* board, AudioEngine* audio, std::function<void()> on_wake_trigger) {
    if (running_.load() || board == nullptr || board->Display() == nullptr) {
        return false;
    }
    DisplayHal* disp = board->Display();
    if (disp->Width() != MosaicoUi::kWidth || disp->Height() != MosaicoUi::kHeight) {
        return false;
    }

    board_ = board;
    audio_ = audio;
    on_wake_trigger_ = std::move(on_wake_trigger);

    UiWifiCallbacks wifi_cbs;
    std::function<void()> ez_unpair_cb;
    {
        std::lock_guard<std::mutex> lock(mu_);
        wifi_cbs = wifi_callbacks_;
        ez_unpair_cb = on_ez_unpair_;
    }

    UiCallbacks cbs;
    cbs.on_volume_change = [this](int vol) {
        if (board_ != nullptr && board_->Audio() != nullptr) {
            board_->Audio()->SetVolume(vol);
        }
    };
    cbs.on_brightness_change = [this](uint8_t bri) {
        if (board_ != nullptr && board_->Display() != nullptr) {
            board_->Display()->SetBrightness(bri);
        }
    };
    cbs.on_long_press_wake = [this]() {
        if (on_wake_trigger_) {
            on_wake_trigger_();
        }
    };
    cbs.on_wifi_scan = std::move(wifi_cbs.on_scan);
    cbs.on_wifi_connect = std::move(wifi_cbs.on_connect);
    cbs.on_wifi_forget = std::move(wifi_cbs.on_forget);
    cbs.on_wifi_toggle_portal = std::move(wifi_cbs.on_toggle_portal);
    cbs.on_wifi_panel_open = std::move(wifi_cbs.on_panel_open);
    cbs.on_ez_unpair = std::move(ez_unpair_cb);

    if (!ui_.Init(std::move(cbs))) {
        return false;
    }

    if (board_->Audio() != nullptr) {
        std::lock_guard<std::mutex> lock(mu_);
        snapshot_.volume = board_->Audio()->Volume();
    }

    disp->SetShowHook([this](const std::string& emotion, const std::string& text) {
        ShowMessage(emotion, text);
    });

    running_.store(true);
#ifdef ESP_PLATFORM
    // Pin 3D avatar rendering strictly to Core 1 so Core 0 audio never stalls.
    const BaseType_t created = xTaskCreatePinnedToCore(
        &UiService::RenderTaskEntry, "UiRenderTask", 8192, this, 3, &task_, kUiCoreId);
    if (created != pdPASS) {
        running_.store(false);
        disp->SetShowHook(nullptr);
        return false;
    }
    ESP_LOGI(kTag, "3D UI render task started on Core %d (480x480 @ target 40FPS)",
             static_cast<int>(kUiCoreId));
#endif
    return true;
}

void UiService::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (board_ != nullptr && board_->Display() != nullptr) {
        board_->Display()->SetShowHook(nullptr);
    }
#ifdef ESP_PLATFORM
    if (task_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(80));
        task_ = nullptr;
    }
#endif
}

void UiService::SetNetworkStatus(bool wifi_connected,
                                 bool gateway_connected,
                                 const std::string& mac) {
    std::lock_guard<std::mutex> lock(mu_);
    snapshot_.wifi_connected = wifi_connected;
    snapshot_.gateway_connected = gateway_connected;
    if (!mac.empty()) {
        snapshot_.mac = mac;
    }
}

void UiService::SetEzStatus(bool direct_mode,
                            bool paired,
                            bool online,
                            const std::string& key_fp,
                            const std::string& host) {
    std::lock_guard<std::mutex> lock(mu_);
    snapshot_.ez_direct_mode = direct_mode;
    snapshot_.ez_paired = paired;
    snapshot_.gateway_connected = online;
    snapshot_.ez_key_fp = key_fp;
    snapshot_.ez_host = host;
}

void UiService::SetWifiState(const UiWifiState& wifi_state) {
    std::lock_guard<std::mutex> lock(mu_);
    snapshot_.wifi_mode = wifi_state.mode;
    snapshot_.wifi_portal_active = wifi_state.portal_active;
    snapshot_.wifi_connected = (wifi_state.mode == WifiUiMode::kConnected);
    snapshot_.wifi_scanning = wifi_state.scanning;
    snapshot_.wifi_ssid = wifi_state.ssid;
    snapshot_.wifi_ip = wifi_state.ip;
    snapshot_.wifi_rssi = wifi_state.rssi;
    snapshot_.ap_ssid = wifi_state.ap_ssid;
    snapshot_.ap_ip = wifi_state.ap_ip;
    snapshot_.wifi_status_msg = wifi_state.status_msg;
    snapshot_.wifi_revision = wifi_state.revision;
    snapshot_.wifi_entries = wifi_state.entries;
}

void UiService::SetSessionTag(const std::string& tag) {
    std::lock_guard<std::mutex> lock(mu_);
    snapshot_.session_tag = tag;
}

void UiService::SetBaseEmotion(AvatarEmotion emotion) {
    std::lock_guard<std::mutex> lock(mu_);
    pending_base_emotion_ = emotion;
    has_pending_base_emotion_ = true;
}

void UiService::ShowMessage(const std::string& emotion, const std::string& text) {
    std::lock_guard<std::mutex> lock(mu_);
    pending_emotion_ = emotion;
    pending_text_ = text;
    has_pending_message_ = true;
}

#ifdef ESP_PLATFORM
void UiService::RenderTaskEntry(void* arg) {
    static_cast<UiService*>(arg)->RunLoop();
    vTaskDelete(nullptr);
}

void UiService::RunLoop() {
    DisplayHal* disp = board_->Display();
    TouchHal* touch = board_->Touch();
    SensorHal* sensors = board_->Sensors();

    int64_t last_us = esp_timer_get_time();
    uint32_t frame_idx = 0;
    float prev_ax = 0.0f;
    float prev_ay = 0.0f;
    float prev_az = 1.0f;
    bool have_prev_imu = false;
    float filt_ax = 0.0f;
    float filt_ay = 0.0f;
    bool have_filt_imu = false;
    float sent_tilt_ax = 0.0f;
    float sent_tilt_ay = 0.0f;
    int64_t last_shake_ms = 0;
    bool sleep_dimmed = false;
    int64_t accum_render_us = 0;
    int64_t accum_pre_us = 0;
    int64_t accum_rows_us = 0;
    int64_t accum_flush_us = 0;
    int64_t perf_window_start_us = esp_timer_get_time();
    uint32_t perf_frames = 0;
    uint32_t perf_cache_hits = 0;
    uint32_t perf_half_frames = 0;
    uint32_t perf_full_frames = 0;

    std::vector<UiRect> dirty_rects;
    dirty_rects.reserve(4);

    while (running_.load()) {
        const int64_t now_us = esp_timer_get_time();
        const int64_t now_ms = now_us / 1000;
        const float dt_sec = std::clamp(static_cast<float>(now_us - last_us) * 1e-6f, 0.01f, 0.1f);
        last_us = now_us;

        // 1. Pull pending external UI commands & telemetry snapshot cleanly.
        UiSnapshot local_state;
        bool apply_base = false;
        AvatarEmotion new_base = AvatarEmotion::kIdle;
        bool apply_msg = false;
        std::string msg_emotion;
        std::string msg_text;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (has_pending_base_emotion_) {
                apply_base = true;
                new_base = pending_base_emotion_;
                has_pending_base_emotion_ = false;
            }
            if (has_pending_message_) {
                apply_msg = true;
                msg_emotion = pending_emotion_;
                msg_text = pending_text_;
                has_pending_message_ = false;
            }
            if (board_->Audio() != nullptr) {
                snapshot_.volume = board_->Audio()->Volume();
            }
            local_state = snapshot_;
        }

        if (apply_base) {
            ui_.SetBaseEmotion(new_base);
        }
        if (apply_msg) {
            ui_.ShowMessage(msg_emotion, msg_text, 4.5f);
        }

        // 2. Poll capacitive touch (CST9220) at ~30 Hz.
        if (touch != nullptr) {
            TouchPoint tp;
            if (touch->Read(&tp)) {
                int prev_vol = local_state.volume;
                int prev_bri = local_state.brightness;
                bool prev_paired = local_state.ez_paired;
                ui_.OnTouch(tp, now_ms, &local_state);
                if (local_state.volume != prev_vol || local_state.brightness != prev_bri ||
                    local_state.ez_paired != prev_paired) {
                    std::lock_guard<std::mutex> lock(mu_);
                    snapshot_.volume = local_state.volume;
                    snapshot_.brightness = local_state.brightness;
                    snapshot_.ez_paired = local_state.ez_paired;
                }
            }
        }

        // 3. Poll BMI270 6-axis IMU (~10 Hz) for gravity tilt parallax & shake detection.
        if (sensors != nullptr && (frame_idx % 4) == 0) {
            const SensorReading rx = sensors->Read("accel_x");
            const SensorReading ry = sensors->Read("accel_y");
            const SensorReading rz = sensors->Read("accel_z");
            if (rx.ok && ry.ok && rz.ok) {
                const float ax = static_cast<float>(rx.value);
                const float ay = static_cast<float>(ry.value);
                const float az = static_cast<float>(rz.value);
                if (!have_filt_imu) {
                    filt_ax = ax;
                    filt_ay = ay;
                    have_filt_imu = true;
                } else {
                    filt_ax = 0.82f * filt_ax + 0.18f * ax;
                    filt_ay = 0.82f * filt_ay + 0.18f * ay;
                }
                if (std::abs(filt_ax - sent_tilt_ax) > 0.04f ||
                    std::abs(filt_ay - sent_tilt_ay) > 0.04f) {
                    sent_tilt_ax = filt_ax;
                    sent_tilt_ay = filt_ay;
                    ui_.SetImuTilt(sent_tilt_ax, sent_tilt_ay);
                }
                if (have_prev_imu) {
                    const float dx = ax - prev_ax;
                    const float dy = ay - prev_ay;
                    const float dz = az - prev_az;
                    const float jerk = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (jerk > 0.85f && (now_ms - last_shake_ms) > 2200) {
                        ui_.TriggerShake();
                        last_shake_ms = now_ms;
                    }
                }
                prev_ax = ax;
                prev_ay = ay;
                prev_az = az;
                have_prev_imu = true;

                std::lock_guard<std::mutex> lock(mu_);
                snapshot_.imu_valid = true;
                snapshot_.accel_x = ax;
                snapshot_.accel_y = ay;
                snapshot_.accel_z = az;
                local_state.imu_valid = true;
                local_state.accel_x = ax;
                local_state.accel_y = ay;
                local_state.accel_z = az;
            }
        }

        // 4. Poll BQ27220 battery & temperature (~0.5 Hz / ~2 s).
        if (sensors != nullptr && (frame_idx % 80) == 0) {
            const SensorReading r_bat = sensors->Read("battery");
            const SensorReading r_volt = sensors->Read("battery_voltage");
            const SensorReading r_curr = sensors->Read("battery_current");
            const SensorReading r_temp = sensors->Read("temperature");
            if (r_bat.ok) {
                std::lock_guard<std::mutex> lock(mu_);
                snapshot_.battery_valid = true;
                snapshot_.battery_percent = static_cast<int>(std::lround(r_bat.value));
                if (r_volt.ok) {
                    snapshot_.battery_voltage = static_cast<float>(r_volt.value);
                }
                if (r_curr.ok) {
                    snapshot_.battery_current_ma = static_cast<float>(r_curr.value);
                }
                if (r_temp.ok) {
                    snapshot_.temperature_c = static_cast<float>(r_temp.value);
                }
                local_state.battery_valid = snapshot_.battery_valid;
                local_state.battery_percent = snapshot_.battery_percent;
                local_state.battery_voltage = snapshot_.battery_voltage;
                local_state.battery_current_ma = snapshot_.battery_current_ma;
                local_state.temperature_c = snapshot_.temperature_c;
            }
        }

        // 5. Sample lock-free voice amplitude (Mic / Speaker RMS).
        if (audio_ != nullptr) {
            const float lvl = std::max(audio_->MicLevel(), audio_->SpeakerLevel());
            ui_.SetAudioEnergy(lvl);
        }

        // 6. Auto-dim CO5300 OLED brightness when in Sleep mode & restore on wake.
        const bool is_sleeping = (ui_.EffectiveEmotion() == AvatarEmotion::kSleep) && !ui_.IsOverlayOpen();
        if (is_sleeping != sleep_dimmed) {
            sleep_dimmed = is_sleeping;
            disp->SetBrightness(is_sleeping ? 25 : static_cast<uint8_t>(local_state.brightness));
        }

        // 7. Render into PSRAM framebuffer and QSPI-flush modified rectangles.
        const int64_t t_render_start = esp_timer_get_time();
        ui_.StepAndRender(dt_sec, local_state, &dirty_rects);
        const int64_t t_flush_start = esp_timer_get_time();
        for (const UiRect& r : dirty_rects) {
            disp->FlushRegion(r.x, r.y, r.w, r.h, ui_.Framebuffer(), MosaicoUi::kWidth);
        }
        const int64_t t_frame_end = esp_timer_get_time();
        accum_render_us += (t_flush_start - t_render_start);
        accum_pre_us += ui_.Renderer().LastPrepassUs();
        accum_rows_us += ui_.Renderer().LastRowsUs();
        const uint8_t cache_mode = ui_.Renderer().LastCacheMode();
        if (cache_mode == 1U) {
            ++perf_cache_hits;
        } else if (cache_mode == 2U) {
            ++perf_half_frames;
        } else {
            ++perf_full_frames;
        }
        accum_flush_us += (t_frame_end - t_flush_start);
        ++perf_frames;
        if (perf_frames >= 60) {
            const float avg_render_ms = static_cast<float>(accum_render_us) / (1000.0f * perf_frames);
            const float avg_pre_ms = static_cast<float>(accum_pre_us) / (1000.0f * perf_frames);
            const float avg_rows_ms = static_cast<float>(accum_rows_us) / (1000.0f * perf_frames);
            const float avg_flush_ms = static_cast<float>(accum_flush_us) / (1000.0f * perf_frames);
            const float wall_s =
                std::max(0.001f, static_cast<float>(t_frame_end - perf_window_start_us) * 1e-6f);
            ESP_LOGI(kTag,
                     "3D UI perf: render=%.2fms (pre=%.2fms rows=%.2fms hit=%u half=%u full=%u) flush=%.2fms fps=%.1f",
                     avg_render_ms,
                     avg_pre_ms,
                     avg_rows_ms,
                     static_cast<unsigned>(perf_cache_hits),
                     static_cast<unsigned>(perf_half_frames),
                     static_cast<unsigned>(perf_full_frames),
                     avg_flush_ms,
                     static_cast<float>(perf_frames) / wall_s);
            accum_render_us = 0;
            accum_pre_us = 0;
            accum_rows_us = 0;
            accum_flush_us = 0;
            perf_window_start_us = t_frame_end;
            perf_frames = 0;
            perf_cache_hits = 0;
            perf_half_frames = 0;
            perf_full_frames = 0;
        }

        ++frame_idx;
        const int64_t elapsed_ms = (t_frame_end - now_us) / 1000;
        const int64_t delay_ms = std::max<int64_t>(1, 23 - elapsed_ms);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
#endif

} // namespace esp_pika
