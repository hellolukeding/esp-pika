#pragma once

#include <cstdint>
#include <vector>

#include "avatar_types.h"

namespace esp_pika {

/// High-craft procedural 3D MatCap + pupil-less iridescent glass avatar renderer.
/// Designed for 480x480 RGB565 framebuffers (allocates precomputed LUTs in PSRAM on ESP32-S31).
class Avatar3DRenderer {
public:
    Avatar3DRenderer() = default;
    ~Avatar3DRenderer();

    Avatar3DRenderer(const Avatar3DRenderer&) = delete;
    Avatar3DRenderer& operator=(const Avatar3DRenderer&) = delete;

    bool Init();
    bool Ready() const { return ready_; }

    /// Renders the 3D avatar + soft contact shadow into a `fb_w x fb_h` native RGB565 buffer
    /// centered at `(center_x, center_y)` with base sphere radius `radius_px`.
    /// Returns the dirty rectangle that was modified (aligned for efficient QSPI flush).
    UiRect Render(uint16_t* fb,
                  int fb_w,
                  int fb_h,
                  const AvatarPose& pose,
                  int center_x = 240,
                  int center_y = 228,
                  float radius_px = 104.0f) const;

private:
    struct Rgb8 {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
    };

    void BuildMatcapLut();
    void BuildRainbowLut();
    Rgb8 SampleMatcap(float nx, float ny, float rim_boost) const;
    Rgb8 SampleRainbow(float t, float warm_shift) const;

    static constexpr int kMatcapSize = 128;
    static constexpr int kRainbowSize = 256;

    bool ready_ = false;
    Rgb8* matcap_lut_ = nullptr;
    Rgb8* rainbow_lut_ = nullptr;
#ifndef ESP_PLATFORM
    std::vector<Rgb8> host_matcap_;
    std::vector<Rgb8> host_rainbow_;
#endif
};

} // namespace esp_pika
