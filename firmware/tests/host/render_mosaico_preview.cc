#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "avatar_animator.h"
#include "avatar_types.h"
#include "mosaico_ui.h"
#include "ui_font.h"

namespace {

using esp_pika::AvatarEmotion;
using esp_pika::AvatarEmotionDisplayName;
using esp_pika::MakeRgb565;
using esp_pika::MosaicoUi;
using esp_pika::UiSnapshot;

bool WritePpm(const char* path, int w, int h, const uint16_t* rgb565) {
    FILE* fp = std::fopen(path, "wb");
    if (fp == nullptr) {
        std::fprintf(stderr, "failed to open %s\n", path);
        return false;
    }
    std::fprintf(fp, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> row_buf(static_cast<size_t>(w) * 3);
    for (int y = 0; y < h; ++y) {
        const uint16_t* src = rgb565 + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            const uint16_t c = src[x];
            row_buf[x * 3 + 0] = static_cast<uint8_t>(((c >> 11) & 0x1FU) * 255U / 31U);
            row_buf[x * 3 + 1] = static_cast<uint8_t>(((c >> 5) & 0x3FU) * 255U / 63U);
            row_buf[x * 3 + 2] = static_cast<uint8_t>((c & 0x1FU) * 255U / 31U);
        }
        std::fwrite(row_buf.data(), 1, row_buf.size(), fp);
    }
    std::fclose(fp);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const char* expr_out = (argc > 1) ? argv[1] : "expressions.ppm";
    const char* drawer_out = (argc > 2) ? argv[2] : "drawer.ppm";
    const char* hero_out = (argc > 3) ? argv[3] : nullptr;

    MosaicoUi ui;
    if (!ui.Init()) {
        std::fprintf(stderr, "MosaicoUi::Init failed\n");
        return 1;
    }

    UiSnapshot state;
    state.wifi_connected = true;
    state.gateway_connected = true;
    state.mac = "30:ed:a0:f4:5b:f0";
    state.session_tag = "SESSION-01";
    state.battery_valid = true;
    state.battery_percent = 94;
    state.battery_voltage = 4.18f;
    state.battery_current_ma = 0.0f;
    state.temperature_c = 29.5f;
    state.imu_valid = true;
    state.accel_x = 0.04f;
    state.accel_y = 0.01f;
    state.accel_z = 1.01f;
    state.volume = 75;
    state.brightness = 90;

    // 1. Render 4x3 grid of all 12 expressions at full 480x480 clarity.
    constexpr int kCols = 4;
    constexpr int kRows = 3;
    constexpr int kPad = 24;
    constexpr int kHeaderH = 92;
    constexpr int kTileW = MosaicoUi::kWidth;
    constexpr int kTileH = MosaicoUi::kHeight;
    constexpr int kLabelH = 44;
    constexpr int kSheetW = kPad + kCols * (kTileW + kPad);
    constexpr int kSheetH = kHeaderH + kRows * (kTileH + kLabelH + kPad);

    std::vector<uint16_t> sheet(static_cast<size_t>(kSheetW) * kSheetH, MakeRgb565(18, 20, 28));
    esp_pika::ui_gfx::DrawCenteredText(
        sheet.data(), kSheetW, kSheetH, kSheetW / 2, 26,
        "MOSAICO 3D DIGITAL LIFE - PUPIL-LESS 12 EXPRESSIONS (480X480)", 3,
        MakeRgb565(232, 236, 248));
    esp_pika::ui_gfx::DrawCenteredText(
        sheet.data(), kSheetW, kSheetH, kSheetW / 2, 58,
        "NO BLACK PUPILS // NO CARTOON LINES // MATCAP + IRIDESCENT GLASS", 2,
        MakeRgb565(142, 152, 182));

    constexpr AvatarEmotion kExpressions[12] = {
        AvatarEmotion::kIdle,      AvatarEmotion::kThinking,  AvatarEmotion::kBlink,
        AvatarEmotion::kLookLeft,  AvatarEmotion::kLookRight, AvatarEmotion::kListening,
        AvatarEmotion::kSpeaking,  AvatarEmotion::kTurn,      AvatarEmotion::kCurious,
        AvatarEmotion::kHappy,     AvatarEmotion::kSurprised, AvatarEmotion::kSleep,
    };

    for (int i = 0; i < 12; ++i) {
        const std::string caption =
            (kExpressions[i] == AvatarEmotion::kSpeaking) ? "Hello Luke, 29.5C Ready!" : "";
        ui.RenderPreviewExpression(kExpressions[i], state, caption);
        const uint16_t* tile = ui.Framebuffer();

        const int col = i % kCols;
        const int row = i / kCols;
        const int ox = kPad + col * (kTileW + kPad);
        const int oy = kHeaderH + row * (kTileH + kLabelH + kPad);

        for (int y = 0; y < kTileH; ++y) {
            const uint16_t* src = tile + static_cast<size_t>(y) * kTileW;
            uint16_t* dst = sheet.data() + static_cast<size_t>(oy + y) * kSheetW + ox;
            std::copy_n(src, kTileW, dst);
        }
        esp_pika::ui_gfx::StrokeRoundedRect(
            sheet.data(), kSheetW, kSheetH, ox - 1, oy - 1, kTileW + 2, kTileH + 2, 2,
            MakeRgb565(46, 52, 74), 255);
        esp_pika::ui_gfx::DrawCenteredText(
            sheet.data(), kSheetW, kSheetH, ox + kTileW / 2, oy + kTileH + 14,
            AvatarEmotionDisplayName(kExpressions[i]), 2, MakeRgb565(214, 222, 240));
    }

    if (!WritePpm(expr_out, kSheetW, kSheetH, sheet.data())) {
        return 1;
    }

    // 2. Render 480x480 swipe-down Control & Telemetry Drawer.
    ui.RenderPreviewDrawer(state);
    if (!WritePpm(drawer_out, MosaicoUi::kWidth, MosaicoUi::kHeight, ui.Framebuffer())) {
        return 1;
    }

    // 3. Render 1:1 native 480x480 Hero Idle preview for side-by-side comparison.
    if (hero_out != nullptr) {
        ui.RenderPreviewExpression(AvatarEmotion::kIdle, state, "");
        if (!WritePpm(hero_out, MosaicoUi::kWidth, MosaicoUi::kHeight, ui.Framebuffer())) {
            return 1;
        }
    }

    std::printf("Wrote %s (%dx%d) and %s (480x480)\n", expr_out, kSheetW, kSheetH, drawer_out);
    return 0;
}
