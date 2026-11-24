#pragma once

#include <cstdint>
#include <vector>

#include "avatar_types.h"

namespace esp_pika {

/// High-speed 3D MatCap + pupil-less rainbow glass avatar renderer.
/// Uses 8x8 tile-accelerated SDF evaluation (pose-cached) on the crown tuft,
/// O(1) analytic sphere spans outside eye boxes, internal SRAM LUTs/scanline staging,
/// and hardware-FPU fast-math with zero transcendental calls inside pixel loops.
class Avatar3DRenderer {
public:
    Avatar3DRenderer() = default;
    ~Avatar3DRenderer();

    Avatar3DRenderer(const Avatar3DRenderer&) = delete;
    Avatar3DRenderer& operator=(const Avatar3DRenderer&) = delete;

    bool Init();
    bool Ready() const { return ready_; }

    uint32_t LastPrepassUs() const { return last_prepass_us_; }
    uint32_t LastRowsUs() const { return last_rows_us_; }
    uint8_t LastCacheMode() const { return last_cache_hit_; }
    void InvalidateFrameCache() const { last_blitted_icy_ = -9999; }

    /// Renders the 3D avatar + soft contact shadow into a `fb_w x fb_h` native RGB565 buffer
    /// centered at `(center_x, center_y)` with base sphere radius `radius_px`.
    /// Set `fast_half_res = true` during rapid turns/blinks for 2x2 motion rendering;
    /// stationary poses hit the full 1:1 sprite cache in PSRAM with vertical bob shift.
    /// If `out_changed` is provided, `*out_changed` is set to false when the stage pixels
    /// in `fb` already match the cached 1:1 sprite verbatim (skipping redundant blits/flushes).
    /// Returns the even-aligned dirty rectangle modified.
    UiRect Render(uint16_t* fb,
                  int fb_w,
                  int fb_h,
                  const AvatarPose& pose,
                  int center_x = 240,
                  int center_y = 228,
                  float radius_px = 132.0f,
                  bool fast_half_res = false,
                  bool* out_changed = nullptr) const;

private:
    struct Rgb8 {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
    };

    struct TuftGridNode {
        bool hit = false;
        float min_d = 10.0f;
        float z = 0.0f;
        float nx = 0.0f;
        float ny = 0.0f;
        float nz = 1.0f;
    };
    struct ShadeCtx;

    void BuildMatcapLut();
    void BuildRainbowLut();
    void BuildSinLut();
    Rgb8 SampleMatcap(float nx, float ny, float rim_boost) const;
    uint16_t SampleMatcap565Fast(float nx, float ny) const;
    Rgb8 SampleRainbow(float t, float warm_shift) const;
    float FastSin01(float phase_0_to_1) const;
    uint16_t ShadeSurface(const ShadeCtx& ctx,
                          float hx,
                          float hy,
                          float hz,
                          float nx,
                          float ny,
                          float nz,
                          float edge_alpha,
                          uint8_t eye_mask) const;

    static constexpr int kMatcapSize = 96;
    static constexpr int kRainbowSize = 256;
    static constexpr int kSinSize = 256;
    static constexpr int kMaxGridDim = 32; // Supports 8x8 tuft boxes up to 248x248px
    static constexpr int kMaxStageW = 320;
    static constexpr int kMaxStageH = 360;

    bool ready_ = false;
    Rgb8* matcap_lut_ = nullptr;
    uint16_t* matcap_565_ = nullptr;
    Rgb8* rainbow_lut_ = nullptr;
    float* sin_lut_ = nullptr;
    mutable TuftGridNode* tuft_grid_ = nullptr;
    mutable uint16_t* cached_orb_ = nullptr;
    mutable bool orb_cache_valid_ = false;
    mutable int cached_orb_w_ = 0;
    mutable int cached_orb_h_ = 0;
    mutable int cached_orb_x0_ = 0;
    mutable int cached_orb_y0_ = 0;
    mutable int cached_orb_icx_ = 0;
    mutable int cached_orb_icy_ = 0;
    mutable int last_blitted_icy_ = -9999;
    mutable float cached_orb_radius_ = 0.0f;
    mutable AvatarPose cached_orb_pose_{};
    mutable uint32_t last_prepass_us_ = 0;
    mutable uint32_t last_rows_us_ = 0;
    mutable uint8_t last_cache_hit_ = 0;
    mutable bool tuft_cache_valid_ = false;
    mutable float last_tuft_yaw_ = 0.0f;
    mutable float last_tuft_pitch_ = 0.0f;
    mutable float last_tuft_roll_ = 0.0f;
    mutable float last_tuft_lag_x_ = 0.0f;
    mutable float last_tuft_lag_y_ = 0.0f;
    mutable float last_tuft_r_px_ = 0.0f;
    mutable int cached_rel_bx0_ = 0;
    mutable int cached_rel_bx1_ = 0;
    mutable int cached_rel_by0_ = 0;
    mutable int cached_rel_by1_ = 0;
    mutable int cached_gw_ = 0;
    mutable int cached_gh_ = 0;
#ifndef ESP_PLATFORM
    std::vector<Rgb8> host_matcap_;
    std::vector<uint16_t> host_matcap_565_;
    std::vector<Rgb8> host_rainbow_;
    std::vector<float> host_sin_;
    mutable std::vector<TuftGridNode> host_tuft_grid_;
    mutable std::vector<uint16_t> host_cached_orb_;
#endif
};

} // namespace esp_pika
