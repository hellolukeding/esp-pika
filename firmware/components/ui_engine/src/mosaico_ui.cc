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
constexpr uint16_t kColorAccentDanger = MakeRgb565(248, 113, 113);
constexpr uint16_t kColorDrawerBg = MakeRgb565(11, 13, 21);
constexpr uint16_t kColorCardBg = MakeRgb565(20, 24, 38);
constexpr uint16_t kColorKeyBg = MakeRgb565(26, 32, 50);
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

struct HitRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool Contains(int px, int py) const {
        return px >= x && px < (x + w) && py >= y && py < (y + h);
    }
};

constexpr HitRect kDrawerUnpairBtn{228, 144, 90, 50};
constexpr HitRect kDrawerWifiBtn{326, 144, 104, 50};
constexpr HitRect kWifiScanBtn{52, 128, 116, 36};
constexpr HitRect kWifiHotspotBtn{178, 128, 140, 36};
constexpr HitRect kWifiPageBadge{328, 128, 100, 36};
constexpr HitRect kWifiPrevBtn{118, 402, 54, 38};
constexpr HitRect kWifiCloseBtn{182, 402, 116, 38};
constexpr HitRect kWifiNextBtn{308, 402, 54, 38};
constexpr int kWifiRowsPerPage = 4;
constexpr int kWifiRowX = 52;
constexpr int kWifiRowW = 376;
constexpr int kWifiRowY0 = 176;
constexpr int kWifiRowStep = 54;
constexpr int kWifiRowH = 48;
constexpr int kWifiDelX = 366;
constexpr int kWifiDelW = 62;

constexpr HitRect kKbClearBtn{356, 62, 44, 36};
constexpr HitRect kKbCancelBtn{76, 342, 150, 44};
constexpr HitRect kKbPortalBtn{254, 342, 150, 44};

enum class KbKeyAction {
    kChar = 0,
    kShift,
    kBackspace,
    kModeNext,
    kJoin,
};

struct KbKey {
    HitRect rect;
    std::string label;
    char ch = 0;
    KbKeyAction action = KbKeyAction::kChar;
    bool accent = false;
    bool active = false;
};

template <typename Layer>
std::vector<KbKey> BuildKeyboardKeys(Layer layer) {
    // Compare enum via int to keep helper clean without exposing nested enum type.
    const int l = static_cast<int>(layer);
    static constexpr const char* kRow0[4] = {
        "qwertyuiop",
        "QWERTYUIOP",
        "1234567890",
        "[]{}#%^*+=",
    };
    static constexpr const char* kRow1[4] = {
        "asdfghjkl",
        "ASDFGHJKL",
        "-/:;()$&@",
        "_|~<>`!?\\",
    };
    static constexpr const char* kRow2[4] = {
        "zxcvbnm",
        "ZXCVBNM",
        ".,?!'\"_",
        ".,~+=/'",
    };

    std::vector<KbKey> keys;
    keys.reserve(34);

    // Row 0: 10 keys, width 36, gap 4 -> x0 = 42, y = 118..164.
    for (int i = 0; i < 10; ++i) {
        const char c = kRow0[l][i];
        keys.push_back({{42 + i * 40, 118, 36, 46}, std::string(1, c), c, KbKeyAction::kChar, false, false});
    }

    // Row 1: 9 keys, width 40, gap 4 -> x0 = 44, y = 170..216.
    for (int i = 0; i < 9; ++i) {
        const char c = kRow1[l][i];
        keys.push_back({{44 + i * 44, 170, 40, 46}, std::string(1, c), c, KbKeyAction::kChar, false, false});
    }

    // Row 2: SHF/SYM key + 7 keys + DEL, y = 222..268.
    const bool alpha = (l == 0 || l == 1);
    keys.push_back({{36, 222, 56, 46}, alpha ? "SHF" : "ABC", 0, KbKeyAction::kShift, false, l == 1});
    for (int i = 0; i < 7; ++i) {
        const char c = kRow2[l][i];
        keys.push_back({{97 + i * 42, 222, 38, 46}, std::string(1, c), c, KbKeyAction::kChar, false, false});
    }
    keys.push_back({{392, 222, 52, 46}, "DEL", 0, KbKeyAction::kBackspace, false, false});

    // Row 3: Mode toggle + SPACE + JOIN, y = 274..320.
    const char* mode_lbl = (l == 2) ? "SYM" : (l == 3) ? "ABC" : "123";
    keys.push_back({{44, 274, 70, 46}, mode_lbl, 0, KbKeyAction::kModeNext, false, l >= 2});
    keys.push_back({{120, 274, 174, 46}, "SPACE", ' ', KbKeyAction::kChar, false, false});
    keys.push_back({{300, 274, 136, 46}, "JOIN", 0, KbKeyAction::kJoin, true, false});

    return keys;
}

std::string SafeSsidLabel(const std::string& raw, size_t max_chars) {
    std::string clean = ui_gfx::NormalizeAsciiText(raw);
    if (clean.empty() && !raw.empty()) {
        clean = "<UTF8-WIFI>";
    }
    if (clean.size() > max_chars && max_chars >= 3) {
        clean = clean.substr(0, max_chars - 2) + "..";
    }
    return clean;
}

std::string TruncAscii(const std::string& raw, size_t max_chars) {
    std::string clean = ui_gfx::NormalizeAsciiText(raw);
    if (clean.size() > max_chars && max_chars >= 3) {
        clean = clean.substr(0, max_chars - 2) + "..";
    }
    return clean;
}

std::string ResolveTopRightPill(const UiSnapshot& state,
                                AvatarEmotion active_emotion,
                                bool* is_alert_pill = nullptr) {
    if (is_alert_pill != nullptr) {
        *is_alert_pill = false;
    }
    if (!state.status_tag.empty()) {
        return state.status_tag;
    }
    if (state.wifi_mode == WifiUiMode::kPortal) {
        if (is_alert_pill != nullptr) {
            *is_alert_pill = true;
        }
        return "WIFI SETUP";
    }
    if (state.ez_direct_mode && state.wifi_connected && !state.ez_paired &&
        active_emotion == AvatarEmotion::kIdle) {
        if (is_alert_pill != nullptr) {
            *is_alert_pill = true;
        }
        return "PAIR READY";
    }
    if (active_emotion == AvatarEmotion::kIdle && !state.session_tag.empty()) {
        return state.session_tag;
    }
    return std::string(AvatarEmotionLabel(active_emotion));
}

void DrawSignalBars(uint16_t* fb, int fb_w, int fb_h, int x, int y, int rssi, uint16_t active_col, uint16_t muted_col) {
    int bars = 0;
    if (rssi > -99) {
        if (rssi >= -60) bars = 3;
        else if (rssi >= -75) bars = 2;
        else bars = 1;
    }
    for (int i = 0; i < 3; ++i) {
        const int bh = 4 + i * 3;
        const int bx = x + i * 5;
        const int by = y + (10 - bh);
        const uint16_t col = (i < bars) ? active_col : muted_col;
        ui_gfx::FillRect(fb, fb_w, fb_h, bx, by, 3, bh, col, 255);
    }
}

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

void MosaicoUi::OpenWifiMenu() {
    drawer_open_ = false;
    drawer_progress_ = 0.0f;
    last_drawer_progress_ = 0.0f;
    view_ = UiView::kWifiMenu;
    wifi_page_ = 0;
    force_full_refresh_ = true;
    overlay_dirty_ = true;
    if (callbacks_.on_wifi_panel_open) {
        callbacks_.on_wifi_panel_open();
    }
}

void MosaicoUi::CloseWifiOverlay() {
    view_ = UiView::kAvatar;
    kb_flash_key_ = -1;
    force_full_refresh_ = true;
    overlay_dirty_ = true;
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

        if (view_ == UiView::kWifiKeyboard) {
            if (!was_tap) {
                return;
            }
            if (kKbClearBtn.Contains(cur_x_, cur_y_)) {
                kb_password_.clear();
                overlay_dirty_ = true;
                return;
            }
            if (kKbCancelBtn.Contains(cur_x_, cur_y_)) {
                view_ = UiView::kWifiMenu;
                kb_flash_key_ = -1;
                force_full_refresh_ = true;
                overlay_dirty_ = true;
                return;
            }
            if (kKbPortalBtn.Contains(cur_x_, cur_y_)) {
                if (callbacks_.on_wifi_toggle_portal) {
                    callbacks_.on_wifi_toggle_portal(true);
                }
                OpenWifiMenu();
                return;
            }
            const auto keys = BuildKeyboardKeys(kb_layer_);
            for (size_t i = 0; i < keys.size(); ++i) {
                if (!keys[i].rect.Contains(cur_x_, cur_y_)) {
                    continue;
                }
                kb_flash_key_ = static_cast<int>(i);
                kb_flash_until_ms_ = now_ms + 160;
                overlay_dirty_ = true;
                switch (keys[i].action) {
                case KbKeyAction::kChar:
                    if (kb_password_.size() < 64) {
                        kb_password_.push_back(keys[i].ch);
                    }
                    break;
                case KbKeyAction::kShift:
                    if (kb_layer_ == KbLayer::kLower) kb_layer_ = KbLayer::kUpper;
                    else if (kb_layer_ == KbLayer::kUpper) kb_layer_ = KbLayer::kLower;
                    else kb_layer_ = KbLayer::kLower;
                    break;
                case KbKeyAction::kBackspace:
                    if (!kb_password_.empty()) {
                        kb_password_.pop_back();
                    }
                    break;
                case KbKeyAction::kModeNext:
                    if (kb_layer_ == KbLayer::kLower || kb_layer_ == KbLayer::kUpper) {
                        kb_layer_ = KbLayer::kDigits;
                    } else if (kb_layer_ == KbLayer::kDigits) {
                        kb_layer_ = KbLayer::kSymbols;
                    } else {
                        kb_layer_ = KbLayer::kLower;
                    }
                    break;
                case KbKeyAction::kJoin:
                    if (callbacks_.on_wifi_connect && !kb_ssid_.empty()) {
                        callbacks_.on_wifi_connect(kb_ssid_, kb_password_, false);
                    }
                    view_ = UiView::kWifiMenu;
                    kb_flash_key_ = -1;
                    force_full_refresh_ = true;
                    break;
                }
                return;
            }
            return;
        }

        if (view_ == UiView::kWifiMenu) {
            if (!was_tap) {
                return;
            }
            if (kWifiScanBtn.Contains(cur_x_, cur_y_)) {
                if (callbacks_.on_wifi_scan) {
                    callbacks_.on_wifi_scan();
                }
                overlay_dirty_ = true;
                return;
            }
            if (kWifiHotspotBtn.Contains(cur_x_, cur_y_)) {
                const bool currently_active =
                    (mutable_state != nullptr) &&
                    (mutable_state->wifi_portal_active || mutable_state->wifi_mode == WifiUiMode::kPortal);
                if (callbacks_.on_wifi_toggle_portal) {
                    callbacks_.on_wifi_toggle_portal(!currently_active);
                }
                overlay_dirty_ = true;
                return;
            }
            const size_t total_entries = (mutable_state != nullptr) ? mutable_state->wifi_entries.size() : 0;
            const int total_pages = std::max(1, static_cast<int>((total_entries + kWifiRowsPerPage - 1) / kWifiRowsPerPage));
            if (wifi_page_ >= total_pages) {
                wifi_page_ = total_pages - 1;
            }
            if (kWifiPrevBtn.Contains(cur_x_, cur_y_)) {
                if (wifi_page_ > 0) {
                    --wifi_page_;
                    overlay_dirty_ = true;
                }
                return;
            }
            if (kWifiNextBtn.Contains(cur_x_, cur_y_)) {
                if (wifi_page_ + 1 < total_pages) {
                    ++wifi_page_;
                    overlay_dirty_ = true;
                }
                return;
            }
            if (kWifiCloseBtn.Contains(cur_x_, cur_y_)) {
                CloseWifiOverlay();
                return;
            }
            if (mutable_state != nullptr) {
                const size_t start_idx = static_cast<size_t>(wifi_page_) * kWifiRowsPerPage;
                for (int row = 0; row < kWifiRowsPerPage; ++row) {
                    const size_t idx = start_idx + static_cast<size_t>(row);
                    if (idx >= mutable_state->wifi_entries.size()) {
                        break;
                    }
                    const int ry = kWifiRowY0 + row * kWifiRowStep;
                    const HitRect row_rect{kWifiRowX, ry, kWifiRowW, kWifiRowH};
                    if (!row_rect.Contains(cur_x_, cur_y_)) {
                        continue;
                    }
                    const auto& entry = mutable_state->wifi_entries[idx];
                    const HitRect del_rect{kWifiDelX, ry, kWifiDelW, kWifiRowH};
                    if (entry.saved && del_rect.Contains(cur_x_, cur_y_)) {
                        if (callbacks_.on_wifi_forget) {
                            callbacks_.on_wifi_forget(entry.ssid);
                        }
                        overlay_dirty_ = true;
                        return;
                    }
                    if (entry.saved) {
                        if (callbacks_.on_wifi_connect) {
                            callbacks_.on_wifi_connect(entry.ssid, "", true);
                        }
                        overlay_dirty_ = true;
                    } else if (!entry.secure) {
                        if (callbacks_.on_wifi_connect) {
                            callbacks_.on_wifi_connect(entry.ssid, "", false);
                        }
                        overlay_dirty_ = true;
                    } else {
                        kb_ssid_ = entry.ssid;
                        kb_password_.clear();
                        kb_layer_ = KbLayer::kLower;
                        kb_flash_key_ = -1;
                        view_ = UiView::kWifiKeyboard;
                        force_full_refresh_ = true;
                        overlay_dirty_ = true;
                    }
                    return;
                }
            }
            return;
        }

        if (drawer_open_) {
            if (dy < -45 || (was_tap && cur_y_ > 426)) {
                drawer_open_ = false;
                force_full_refresh_ = true;
            } else if (was_tap && mutable_state != nullptr && mutable_state->ez_direct_mode &&
                       mutable_state->ez_paired && kDrawerUnpairBtn.Contains(cur_x_, cur_y_)) {
                if (callbacks_.on_ez_unpair) {
                    callbacks_.on_ez_unpair();
                }
                mutable_state->ez_paired = false;
                mutable_state->gateway_connected = false;
                drawer_open_ = false;
                force_full_refresh_ = true;
                ShowMessage("curious", "UNPAIRED // READY", 3.0f);
            } else if (was_tap && kDrawerWifiBtn.Contains(cur_x_, cur_y_)) {
                OpenWifiMenu();
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
            const HitRect hud_wifi_zone{16, 0, 100, 52};
            const bool portal_bottom_tap =
                (mutable_state != nullptr && mutable_state->wifi_mode == WifiUiMode::kPortal && cur_y_ >= 390);
            if (hud_wifi_zone.Contains(cur_x_, cur_y_) || portal_bottom_tap) {
                OpenWifiMenu();
                return;
            }
            const bool near_orb = (std::abs(cur_x_ - 240) < 105 && std::abs(cur_y_ - 228) < 105);
            if (near_orb && callbacks_.on_tap_barge_in && callbacks_.on_tap_barge_in()) {
                last_tap_ms_ = 0;
                return;
            }
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

    if (view_ != UiView::kAvatar) {
        return;
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

    const uint16_t mono_col = MakeRgb565(138, 145, 160);
    uint16_t wifi_col = mono_col;
    if (state.wifi_mode == WifiUiMode::kPortal || state.wifi_mode == WifiUiMode::kConnecting) {
        wifi_col = kColorAccentAmber;
    } else if (!state.wifi_connected) {
        wifi_col = kColorHudMuted;
    }
    ui_gfx::DrawWifiIcon(framebuffer_, kWidth, kHeight, 36, 15, state.wifi_connected, wifi_col);

    const uint16_t link_col = state.gateway_connected ? mono_col : MakeRgb565(76, 82, 96);
    ui_gfx::DrawLinkIcon(framebuffer_, kWidth, kHeight, 62, 15, state.gateway_connected, link_col);

    const bool charging = state.battery_current_ma > 20.0f;
    uint16_t bat_col = mono_col;
    if (state.battery_valid && state.battery_percent < 20) {
        bat_col = kColorAccentDanger;
    } else if (charging) {
        bat_col = kColorAccentMint;
    }
    ui_gfx::DrawBatteryIcon(framebuffer_, kWidth, kHeight, 88, 15,
                            state.battery_valid ? state.battery_percent : 100, charging,
                            mono_col, bat_col);

    bool alert_pill = false;
    const std::string right_text = ResolveTopRightPill(state, active_emotion, &alert_pill);
    const std::string pill = "[ " + right_text + " ]";
    const int scale = 2;
    const int text_w = static_cast<int>(pill.size()) * (6 * scale) - scale;
    const int rx = std::max(210, 442 - text_w);
    uint16_t pill_col = MakeRgb565(146, 152, 168);
    if (alert_pill || active_emotion == AvatarEmotion::kSurprised) {
        pill_col = kColorAccentAmber;
    } else if (active_emotion == AvatarEmotion::kListening) {
        pill_col = kColorAccentCyan;
    } else if (active_emotion == AvatarEmotion::kSpeaking || active_emotion == AvatarEmotion::kHappy) {
        pill_col = kColorAccentMint;
    } else if (active_emotion == AvatarEmotion::kThinking || active_emotion == AvatarEmotion::kCurious) {
        pill_col = kColorAccentCyan;
    }
    ui_gfx::DrawText(framebuffer_, kWidth, kHeight, rx, 15, pill, scale, pill_col, 225);
}

void MosaicoUi::RenderBottomCaption(const UiSnapshot& state, float audio_energy) {
    ui_gfx::FillRect(framebuffer_, kWidth, kHeight, 24, 390, 432, 86, kColorBlack, 255);

    if (audio_energy > 0.04f) {
        const int bar_w = std::clamp(static_cast<int>(40.0f + audio_energy * 220.0f), 40, 260);
        const int x0 = 240 - bar_w / 2;
        ui_gfx::FillRoundedRect(framebuffer_, kWidth, kHeight, x0, 394, bar_w, 3, 1,
                                kColorAccentCyan, static_cast<uint8_t>(110 + audio_energy * 145));
    }

    if (caption_alpha_ <= 0.02f || caption_text_.empty()) {
        if (audio_energy <= 0.04f) {
            if (state.wifi_mode == WifiUiMode::kPortal) {
                const std::string ap_name =
                    state.ap_ssid.empty() ? "ESP-Mosaico" : TruncAscii(state.ap_ssid, 20);
                const std::string portal_line = "AP: " + ap_name + " // " + state.ap_ip;
                ui_gfx::DrawCenteredText(framebuffer_, kWidth, kHeight, 240, 424,
                                         portal_line, 1, kColorAccentAmber, 235);
                ui_gfx::DrawCenteredText(framebuffer_, kWidth, kHeight, 240, 444,
                                         "TAP HERE TO CONFIG WIFI", 1, kColorHudMuted, 210);
            } else if (state.ez_direct_mode && state.wifi_connected && !state.ez_paired) {
                const std::string fp =
                    state.ez_key_fp.empty() ? "----" : TruncAscii(state.ez_key_fp, 12);
                const std::string pair_line = "PAIR READY // FP: " + fp;
                ui_gfx::DrawCenteredText(framebuffer_, kWidth, kHeight, 240, 424,
                                         pair_line, 1, kColorAccentCyan, 235);
                ui_gfx::DrawCenteredText(framebuffer_, kWidth, kHeight, 240, 444,
                                         "OPEN EZ ASSISTANT -> CONNECT", 1, kColorHudMuted, 210);
            } else {
                ui_gfx::DrawCenteredText(framebuffer_, kWidth, kHeight, 240, 436,
                                         "TAP TO TALK // MOSAICO", 2, MakeRgb565(92, 98, 114), 210);
            }
        }
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

    ui_gfx::DrawCenteredText(framebuffer_, kWidth, kHeight, 240, 22, "MOSAICO CONTROL & TELEMETRY", 2,
                             kColorAccentViolet, 240);

    // Card 1: Power & Thermal (BQ27220) + Motion (BMI270) + Network / WIFI button.
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

    std::string net_line;
    if (state.wifi_connected) {
        const std::string ssid_s = SafeSsidLabel(state.wifi_ssid, 15);
        net_line = "WIFI: " + (ssid_s.empty() ? "OK" : ssid_s) +
                   (state.wifi_ip.empty() ? "" : (" " + state.wifi_ip));
    } else if (state.wifi_mode == WifiUiMode::kPortal || state.wifi_portal_active) {
        net_line = "AP: " + (state.ap_ssid.empty() ? "SETUP" : TruncAscii(state.ap_ssid, 18));
    } else if (state.wifi_mode == WifiUiMode::kConnecting) {
        net_line = "WIFI: CONNECTING...";
    } else {
        net_line = "WIFI: OFFLINE";
    }
    ui_gfx::DrawText(framebuffer_, kWidth, kHeight, 54, 126, TruncAscii(net_line, 40), 1,
                     state.wifi_connected ? kColorAccentMint : kColorAccentAmber, 230);

    if (state.ez_direct_mode) {
        const std::string fp = state.ez_key_fp.empty() ? "----" : TruncAscii(state.ez_key_fp, 10);
        if (state.ez_paired) {
            const std::string ez_line =
                std::string("EZ:") + (state.gateway_connected ? "OK" : "WAIT") + " FP:" + fp;
            ui_gfx::DrawText(framebuffer_, kWidth, kHeight, 54, 156, TruncAscii(ez_line, 26), 1,
                             state.gateway_connected ? kColorAccentMint : kColorHudMuted, 225);

            ui_gfx::FillRoundedRect(framebuffer_, kWidth, kHeight, kDrawerUnpairBtn.x,
                                    kDrawerUnpairBtn.y, kDrawerUnpairBtn.w, kDrawerUnpairBtn.h, 12,
                                    MakeRgb565(46, 20, 26), 255);
            ui_gfx::StrokeRoundedRect(framebuffer_, kWidth, kHeight, kDrawerUnpairBtn.x,
                                      kDrawerUnpairBtn.y, kDrawerUnpairBtn.w, kDrawerUnpairBtn.h,
                                      12, kColorAccentDanger, 255);
            ui_gfx::DrawCenteredText(framebuffer_, kWidth, kHeight,
                                     kDrawerUnpairBtn.x + kDrawerUnpairBtn.w / 2,
                                     kDrawerUnpairBtn.y + 18, "UNPAIR", 2, kColorAccentDanger, 245);
        } else {
            const std::string ez_line = "EZ:PAIR FP:" + fp + " " + TruncAscii(state.mac, 17);
            ui_gfx::DrawText(framebuffer_, kWidth, kHeight, 54, 156, TruncAscii(ez_line, 42), 1,
                             kColorAccentCyan, 225);
        }
    } else {
        char line4[64] = {};
        std::snprintf(line4, sizeof(line4), "GW=%s  MAC=%s", state.gateway_connected ? "OK" : "OFF",
                      state.mac.c_str());
        ui_gfx::DrawText(framebuffer_, kWidth, kHeight, 54, 156, TruncAscii(line4, 42), 1,
                         kColorHudMuted, 225);
    }

    // Interactive WIFI > button in Card 1 bottom-right.
    ui_gfx::FillRoundedRect(framebuffer_, kWidth, kHeight, kDrawerWifiBtn.x, kDrawerWifiBtn.y,
                            kDrawerWifiBtn.w, kDrawerWifiBtn.h, 12, kColorKeyBg, 255);
    ui_gfx::StrokeRoundedRect(framebuffer_, kWidth, kHeight, kDrawerWifiBtn.x, kDrawerWifiBtn.y,
                              kDrawerWifiBtn.w, kDrawerWifiBtn.h, 12, kColorAccentCyan, 255);
    ui_gfx::DrawCenteredText(framebuffer_, kWidth, kHeight, kDrawerWifiBtn.x + kDrawerWifiBtn.w / 2,
                             kDrawerWifiBtn.y + 18, "WIFI >", 2, kColorAccentCyan, 245);

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

void MosaicoUi::RenderWifiMenu(const UiSnapshot& state) {
    ClearFramebuffer();
    ui_gfx::DrawCenteredText(framebuffer_, kWidth, kHeight, 240, 22, "WI-FI NETWORKS", 2,
                             kColorAccentCyan, 245);

    // Status card inside circular bezel.
    ui_gfx::FillRoundedRect(framebuffer_, kWidth, kHeight, 76, 56, 328, 62, 14, kColorCardBg, 255);
    ui_gfx::StrokeRoundedRect(framebuffer_, kWidth, kHeight, 76, 56, 328, 62, 14, kColorPillBorder, 255);

    std::string st_line1;
    uint16_t st_col = kColorHudMuted;
    if (state.wifi_connected) {
        st_line1 = "CONN: " + SafeSsidLabel(state.wifi_ssid, 22) + " (" +
                   std::to_string(state.wifi_rssi) + "dBm)";
        st_col = kColorAccentMint;
    } else if (state.wifi_mode == WifiUiMode::kConnecting) {
        st_line1 = "JOINING: " + SafeSsidLabel(state.wifi_ssid, 26);
        st_col = kColorAccentAmber;
    } else if (state.wifi_portal_active || state.wifi_mode == WifiUiMode::kPortal) {
        st_line1 = "HOTSPOT: " + SafeSsidLabel(state.ap_ssid.empty() ? "ESP-Mosaico" : state.ap_ssid, 24);
        st_col = kColorAccentAmber;
    } else {
        st_line1 = "WIFI DISCONNECTED";
    }
    ui_gfx::DrawText(framebuffer_, kWidth, kHeight, 92, 66, TruncAscii(st_line1, 48), 1, st_col, 245);

    std::string st_line2;
    if (state.wifi_portal_active || state.wifi_mode == WifiUiMode::kPortal) {
        st_line2 = "SETUP: HTTP://" + state.ap_ip;
        if (!state.wifi_ip.empty()) {
            st_line2 += "  STA:" + state.wifi_ip;
        }
    } else if (state.wifi_connected) {
        st_line2 = "IP: " + (state.wifi_ip.empty() ? "--" : state.wifi_ip) + "  GW: " +
                   (state.gateway_connected ? "OK" : "WAIT");
    } else {
        st_line2 = "TAP NETWORK TO JOIN OR USE HOTSPOT";
    }
    ui_gfx::DrawText(framebuffer_, kWidth, kHeight, 92, 84, TruncAscii(st_line2, 48), 1,
                     kColorHudBright, 225);

    if (!state.wifi_status_msg.empty()) {
        ui_gfx::DrawText(framebuffer_, kWidth, kHeight, 92, 100,
                         TruncAscii(state.wifi_status_msg, 46), 1, kColorAccentAmber, 235);
    }

    // Action bar.
    auto draw_btn = [&](const HitRect& r, const std::string& label, uint16_t bg, uint16_t bd,
                        uint16_t fg) {
        ui_gfx::FillRoundedRect(framebuffer_, kWidth, kHeight, r.x, r.y, r.w, r.h, 10, bg, 255);
        ui_gfx::StrokeRoundedRect(framebuffer_, kWidth, kHeight, r.x, r.y, r.w, r.h, 10, bd, 255);
        ui_gfx::DrawCenteredText(framebuffer_, kWidth, kHeight, r.x + r.w / 2, r.y + (r.h - 7) / 2,
                                 label, 1, fg, 245);
    };

    draw_btn(kWifiScanBtn, state.wifi_scanning ? "SCANNING..." : "SCAN", kColorKeyBg,
             state.wifi_scanning ? kColorAccentCyan : kColorPillBorder, kColorHudBright);

    const bool ap_on = state.wifi_portal_active || (state.wifi_mode == WifiUiMode::kPortal);
    draw_btn(kWifiHotspotBtn, ap_on ? "STOP AP" : "HOTSPOT",
             ap_on ? kColorAccentAmber : kColorKeyBg,
             ap_on ? kColorAccentAmber : kColorPillBorder,
             ap_on ? kColorBlack : kColorHudBright);

    const int total_pages = std::max(
        1, static_cast<int>((state.wifi_entries.size() + kWifiRowsPerPage - 1) / kWifiRowsPerPage));
    const int page_clamped = std::clamp(wifi_page_, 0, total_pages - 1);
    char page_buf[24] = {};
    std::snprintf(page_buf, sizeof(page_buf), "PAGE %d/%d", page_clamped + 1, total_pages);
    draw_btn(kWifiPageBadge, page_buf, kColorCardBg, kColorPillBorder, kColorHudMuted);

    // 4 visible Wi-Fi rows.
    const size_t start_idx = static_cast<size_t>(page_clamped) * kWifiRowsPerPage;
    if (state.wifi_entries.empty()) {
        ui_gfx::DrawCenteredText(framebuffer_, kWidth, kHeight, 240, 266,
                                 state.wifi_scanning ? "SCANNING 2.4GHZ AP..." : "NO WIFI FOUND - TAP SCAN",
                                 1, kColorHudMuted, 220);
    } else {
        for (int row = 0; row < kWifiRowsPerPage; ++row) {
            const size_t idx = start_idx + static_cast<size_t>(row);
            if (idx >= state.wifi_entries.size()) {
                break;
            }
            const auto& entry = state.wifi_entries[idx];
            const int ry = kWifiRowY0 + row * kWifiRowStep;
            const uint16_t row_bd = entry.connected ? kColorAccentMint
                                    : entry.saved   ? kColorAccentCyan
                                                    : kColorPillBorder;
            ui_gfx::FillRoundedRect(framebuffer_, kWidth, kHeight, kWifiRowX, ry, kWifiRowW, kWifiRowH,
                                    12, kColorCardBg, 255);
            ui_gfx::StrokeRoundedRect(framebuffer_, kWidth, kHeight, kWifiRowX, ry, kWifiRowW,
                                      kWifiRowH, 12, row_bd, 255);

            const std::string ssid_lbl = SafeSsidLabel(entry.ssid, 26);
            ui_gfx::DrawText(framebuffer_, kWidth, kHeight, kWifiRowX + 14, ry + 10, ssid_lbl, 1,
                             entry.connected ? kColorAccentMint : kColorHudBright, 245);

            DrawSignalBars(framebuffer_, kWidth, kHeight, kWifiRowX + 14, ry + 26, entry.rssi,
                           entry.connected ? kColorAccentMint : kColorAccentCyan, kColorTrackBg);

            char meta[40] = {};
            if (entry.rssi > -99) {
                std::snprintf(meta, sizeof(meta), "%d dBm  %s", entry.rssi,
                              entry.secure ? "WPA" : "OPEN");
            } else {
                std::snprintf(meta, sizeof(meta), "OFFLINE  %s", entry.secure ? "WPA" : "OPEN");
            }
            ui_gfx::DrawText(framebuffer_, kWidth, kHeight, kWifiRowX + 34, ry + 28, meta, 1,
                             kColorHudMuted, 225);

            const char* tag = entry.connected ? "CUR"
                              : entry.saved   ? "SAVED"
                              : entry.secure  ? "LOCK"
                                              : "OPEN";
            const uint16_t tag_col = entry.connected ? kColorAccentMint
                                     : entry.saved   ? kColorAccentCyan
                                                     : kColorHudMuted;
            const int tag_x = entry.saved ? (kWifiDelX - 48) : (kWifiRowX + kWifiRowW - 48);
            ui_gfx::DrawText(framebuffer_, kWidth, kHeight, tag_x, ry + 19, tag, 1, tag_col, 240);

            if (entry.saved) {
                const HitRect del_btn{kWifiDelX, ry + 6, kWifiDelW - 6, kWifiRowH - 12};
                draw_btn(del_btn, "DEL", MakeRgb565(46, 20, 26), kColorAccentDanger,
                         kColorAccentDanger);
            }
        }
    }

    // Bottom navigation.
    draw_btn(kWifiPrevBtn, "<", kColorKeyBg, kColorPillBorder,
             page_clamped > 0 ? kColorHudBright : kColorHudMuted);
    draw_btn(kWifiCloseBtn, "CLOSE", kColorKeyBg, kColorPillBorder, kColorHudBright);
    draw_btn(kWifiNextBtn, ">", kColorKeyBg, kColorPillBorder,
             (page_clamped + 1 < total_pages) ? kColorHudBright : kColorHudMuted);
}

void MosaicoUi::RenderWifiKeyboard(const UiSnapshot& /*state*/, int64_t now_ms) {
    ClearFramebuffer();
    const std::string title = "JOIN: " + SafeSsidLabel(kb_ssid_, 22);
    ui_gfx::DrawCenteredText(framebuffer_, kWidth, kHeight, 240, 22, title, 1, kColorAccentCyan, 245);

    // Input box + CLR.
    ui_gfx::FillRoundedRect(framebuffer_, kWidth, kHeight, 76, 56, 328, 48, 12, kColorCardBg, 255);
    ui_gfx::StrokeRoundedRect(framebuffer_, kWidth, kHeight, 76, 56, 328, 48, 12, kColorAccentCyan, 255);

    std::string shown;
    bool placeholder = false;
    if (kb_password_.empty()) {
        shown = "PASSWORD_";
        placeholder = true;
    } else {
        constexpr size_t kMaxShown = 20;
        if (kb_password_.size() > kMaxShown) {
            shown = kb_password_.substr(kb_password_.size() - kMaxShown);
        } else {
            shown = kb_password_;
        }
        shown.push_back('_');
    }
    ui_gfx::DrawText(framebuffer_, kWidth, kHeight, 90, 73,
                     TruncAscii(shown, 22), 2, placeholder ? kColorHudMuted : kColorHudBright, 245);

    ui_gfx::FillRoundedRect(framebuffer_, kWidth, kHeight, kKbClearBtn.x, kKbClearBtn.y,
                            kKbClearBtn.w, kKbClearBtn.h, 8, kColorKeyBg, 255);
    ui_gfx::StrokeRoundedRect(framebuffer_, kWidth, kHeight, kKbClearBtn.x, kKbClearBtn.y,
                              kKbClearBtn.w, kKbClearBtn.h, 8, kColorPillBorder, 255);
    ui_gfx::DrawCenteredText(framebuffer_, kWidth, kHeight, kKbClearBtn.x + kKbClearBtn.w / 2,
                             kKbClearBtn.y + 14, "CLR", 1, kColorHudMuted, 240);

    // Soft keyboard keys.
    const auto keys = BuildKeyboardKeys(kb_layer_);
    const bool flash_active = (kb_flash_key_ >= 0) && (now_ms <= kb_flash_until_ms_);
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto& k = keys[i];
        const bool flashed = flash_active && (static_cast<int>(i) == kb_flash_key_);
        uint16_t bg = kColorKeyBg;
        uint16_t bd = kColorPillBorder;
        uint16_t fg = kColorHudBright;
        if (k.accent) {
            bg = kColorAccentCyan;
            bd = kColorAccentCyan;
            fg = kColorBlack;
        } else if (flashed || k.active) {
            bg = kColorAccentViolet;
            bd = kColorAccentViolet;
            fg = kColorBlack;
        }
        ui_gfx::FillRoundedRect(framebuffer_, kWidth, kHeight, k.rect.x, k.rect.y, k.rect.w,
                                k.rect.h, 8, bg, 255);
        ui_gfx::StrokeRoundedRect(framebuffer_, kWidth, kHeight, k.rect.x, k.rect.y, k.rect.w,
                                  k.rect.h, 8, bd, 255);
        const int scale = (k.label.size() == 1) ? 2 : 1;
        const int ty = k.rect.y + (k.rect.h - 7 * scale) / 2;
        ui_gfx::DrawCenteredText(framebuffer_, kWidth, kHeight, k.rect.x + k.rect.w / 2, ty,
                                 k.label, scale, fg, 250);
    }

    // Bottom buttons: CANCEL & USE PHONE AP.
    ui_gfx::FillRoundedRect(framebuffer_, kWidth, kHeight, kKbCancelBtn.x, kKbCancelBtn.y,
                            kKbCancelBtn.w, kKbCancelBtn.h, 10, kColorKeyBg, 255);
    ui_gfx::StrokeRoundedRect(framebuffer_, kWidth, kHeight, kKbCancelBtn.x, kKbCancelBtn.y,
                              kKbCancelBtn.w, kKbCancelBtn.h, 10, kColorPillBorder, 255);
    ui_gfx::DrawCenteredText(framebuffer_, kWidth, kHeight, kKbCancelBtn.x + kKbCancelBtn.w / 2,
                             kKbCancelBtn.y + 18, "CANCEL", 1, kColorHudBright, 245);

    ui_gfx::FillRoundedRect(framebuffer_, kWidth, kHeight, kKbPortalBtn.x, kKbPortalBtn.y,
                            kKbPortalBtn.w, kKbPortalBtn.h, 10, kColorCardBg, 255);
    ui_gfx::StrokeRoundedRect(framebuffer_, kWidth, kHeight, kKbPortalBtn.x, kKbPortalBtn.y,
                              kKbPortalBtn.w, kKbPortalBtn.h, 10, kColorAccentAmber, 255);
    ui_gfx::DrawCenteredText(framebuffer_, kWidth, kHeight, kKbPortalBtn.x + kKbPortalBtn.w / 2,
                             kKbPortalBtn.y + 18, "USE PHONE AP", 1, kColorAccentAmber, 245);
}

void MosaicoUi::StepAndRender(float dt_sec, const UiSnapshot& state, std::vector<UiRect>* out_dirty) {
    if (!ready_ || out_dirty == nullptr) {
        return;
    }
    out_dirty->clear();
    overlay_clock_ms_ += static_cast<int64_t>(std::max(1.0f, dt_sec * 1000.0f));

    if (view_ == UiView::kWifiMenu || view_ == UiView::kWifiKeyboard) {
        renderer_.InvalidateFrameCache();
        const bool rev_changed = (state.wifi_revision != last_overlay_wifi_rev_);
        const bool flash_expired = (kb_flash_key_ >= 0) && (overlay_clock_ms_ > kb_flash_until_ms_);
        if (flash_expired) {
            kb_flash_key_ = -1;
        }
        if (force_full_refresh_ || overlay_dirty_ || rev_changed || flash_expired) {
            if (view_ == UiView::kWifiMenu) {
                RenderWifiMenu(state);
            } else {
                RenderWifiKeyboard(state, overlay_clock_ms_);
            }
            out_dirty->push_back({0, 0, kWidth, kHeight});
            force_full_refresh_ = false;
            overlay_dirty_ = false;
            last_overlay_wifi_rev_ = state.wifi_revision;
        }
        return;
    }

    const AvatarPose pose = animator_.Step(dt_sec);
    const AvatarEmotion active_emotion = animator_.EffectiveEmotion();

    if (caption_hold_sec_ > 0.0f) {
        caption_hold_sec_ = std::max(0.0f, caption_hold_sec_ - dt_sec);
        caption_alpha_ = std::min(1.0f, caption_alpha_ + dt_sec * 5.0f);
    } else {
        caption_alpha_ = std::max(0.0f, caption_alpha_ - dt_sec * 2.5f);
    }

    const float drawer_target = drawer_open_ ? 1.0f : 0.0f;
    drawer_progress_ += (drawer_target - drawer_progress_) * std::min(1.0f, dt_sec * 12.0f);
    if (std::abs(drawer_target - drawer_progress_) < 0.01f) {
        drawer_progress_ = drawer_target;
    }

    const bool drawer_animating = std::abs(drawer_progress_ - last_drawer_progress_) > 0.005f;
    if (force_full_refresh_ || drawer_animating || drawer_progress_ > 0.01f) {
        renderer_.InvalidateFrameCache();
    }
    if (force_full_refresh_) {
        ClearFramebuffer();
    }

    if (drawer_progress_ >= 0.99f && !force_full_refresh_ && !drawer_animating) {
        RenderDrawerOverlay(state, kHeight);
        out_dirty->push_back({0, 0, kWidth, 340});
        last_drawer_progress_ = drawer_progress_;
        return;
    }

    const std::string right_pill = ResolveTopRightPill(state, active_emotion);
    const bool charging = state.battery_current_ma > 20.0f;
    const bool ez_changed = (state.ez_direct_mode != last_ez_direct_) ||
                            (state.ez_paired != last_ez_paired_) ||
                            (state.ez_key_fp != last_ez_fp_);
    const bool hud_changed =
        force_full_refresh_ || (state.wifi_connected != last_wifi_) ||
        (state.gateway_connected != last_gateway_) || (state.wifi_mode != last_wifi_mode_) ||
        (state.battery_percent != last_bat_pct_) || (charging != last_charging_) ||
        (right_pill != last_right_pill_) || ez_changed;

    if (hud_changed || drawer_animating) {
        RenderTopHud(state, active_emotion);
        last_wifi_ = state.wifi_connected;
        last_gateway_ = state.gateway_connected;
        last_wifi_mode_ = state.wifi_mode;
        last_bat_pct_ = state.battery_percent;
        last_charging_ = charging;
        last_ez_direct_ = state.ez_direct_mode;
        last_ez_paired_ = state.ez_paired;
        last_ez_fp_ = state.ez_key_fp;
        last_right_pill_ = right_pill;
        if (!force_full_refresh_ && !drawer_animating) {
            out_dirty->push_back({0, 0, kWidth, 44});
        }
    }

    const bool fast_moving =
        has_prev_pose_ &&
        (std::abs(pose.yaw_deg - prev_pose_.yaw_deg) +
                 std::abs(pose.pitch_deg - prev_pose_.pitch_deg) +
                 std::abs(pose.roll_deg - prev_pose_.roll_deg) >
             0.06f ||
         std::abs(pose.upper_lid - prev_pose_.upper_lid) +
                 std::abs(pose.lower_lid - prev_pose_.lower_lid) >
             0.005f ||
         std::abs(pose.scale - prev_pose_.scale) > 0.002f ||
         std::abs(pose.happy_arch - prev_pose_.happy_arch) > 0.005f ||
         std::abs(pose.core_bloom - prev_pose_.core_bloom) > 0.01f ||
         std::abs(pose.tuft_lag_x - prev_pose_.tuft_lag_x) +
                 std::abs(pose.tuft_lag_y - prev_pose_.tuft_lag_y) >
             0.002f ||
         pose.swirl_amount > 0.15f);
    prev_pose_ = pose;
    has_prev_pose_ = true;
    bool avatar_changed = true;
    const UiRect avatar_rect = renderer_.Render(
        RawFb(), kWidth, kHeight, pose, 240, 228, 132.0f, fast_moving, &avatar_changed);
    if (!force_full_refresh_ && !drawer_animating && avatar_changed) {
        out_dirty->push_back(avatar_rect);
    }

    const bool caption_active =
        (caption_alpha_ > 0.01f) || (pose.audio_energy > 0.04f) ||
        (state.wifi_mode != last_wifi_mode_);
    if (force_full_refresh_ || caption_active || caption_visible_last_ || drawer_animating ||
        hud_changed) {
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
    renderer_.Render(RawFb(), kWidth, kHeight, pose, 240, 228, 132.0f);
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

void MosaicoUi::RenderPreviewWifiMenu(const UiSnapshot& state) {
    if (!ready_) {
        return;
    }
    RenderWifiMenu(state);
}

void MosaicoUi::RenderPreviewWifiKeyboard(const std::string& ssid,
                                          const std::string& password,
                                          const UiSnapshot& state) {
    if (!ready_) {
        return;
    }
    kb_ssid_ = ssid;
    kb_password_ = password;
    RenderWifiKeyboard(state, overlay_clock_ms_);
}

} // namespace esp_pika
