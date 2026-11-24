#include "avatar_3d_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#define PIKA_IRAM IRAM_ATTR
#else
#define PIKA_IRAM
#endif

namespace esp_pika {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kTwoPi = 6.28318530f;
constexpr float kDegToRad = kPi / 180.0f;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 operator-(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 operator*(const Vec3& a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
inline float Dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Length(const Vec3& v) {
    return std::sqrt(Dot(v, v));
}
inline Vec3 Normalize(const Vec3& v) {
    const float len2 = Dot(v, v);
    if (len2 < 1e-12f) {
        return {0.0f, 0.0f, 1.0f};
    }
    const float inv = 1.0f / std::sqrt(len2);
    return {v.x * inv, v.y * inv, v.z * inv};
}

inline float Clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

inline float SmoothStep(float e0, float e1, float x) {
    const float t = Clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

inline float SmoothMin(float a, float b, float k) {
    const float h = Clamp01(0.5f + 0.5f * (b - a) / k);
    return (b * (1.0f - h) + a * h) - k * h * (1.0f - h);
}

// Fast smooth polynomial Gaussian substitute: zero exp() calls inside pixel loops!
inline float FastFalloff(float d2) {
    const float t = 1.0f - d2 * 0.25f;
    if (t <= 0.0f) {
        return 0.0f;
    }
    const float t2 = t * t;
    return t2 * t2;
}

inline float SdTaperedCapsule(const Vec3& p, const Vec3& a, const Vec3& b, float ra, float rb) {
    const Vec3 pa = p - a;
    const Vec3 ba = b - a;
    const float baba = Dot(ba, ba);
    const float t = (baba > 1e-6f) ? Clamp01(Dot(pa, ba) / baba) : 0.0f;
    const Vec3 diff = pa - ba * t;
    return Length(diff) - (ra + (rb - ra) * t);
}

struct TuftControlPoints {
    Vec3 q0{0.20f, -0.76f, -0.02f};
    Vec3 q1{0.38f, -0.95f, -0.03f};
    Vec3 q2{0.46f, -1.13f, -0.04f};
    Vec3 q3{0.37f, -1.30f, -0.04f};
};

PIKA_IRAM float SceneSdf(const Vec3& q, const TuftControlPoints& tuft) {
    const float d_sphere = Length(q) - 1.0f;
    const float d1 = SdTaperedCapsule(q, tuft.q0, tuft.q1, 0.33f, 0.20f);
    const float d2 = SdTaperedCapsule(q, tuft.q1, tuft.q2, 0.20f, 0.105f);
    const float d3 = SdTaperedCapsule(q, tuft.q2, tuft.q3, 0.105f, 0.018f);
    const float d_tuft = SmoothMin(SmoothMin(d1, d2, 0.11f), d3, 0.09f);
    return SmoothMin(d_sphere, d_tuft, 0.28f);
}

PIKA_IRAM Vec3 SceneNormalObj(const Vec3& q, const TuftControlPoints& tuft) {
    constexpr float kEps = 0.015f;
    const Vec3 k0{1.0f, -1.0f, -1.0f};
    const Vec3 k1{-1.0f, -1.0f, 1.0f};
    const Vec3 k2{-1.0f, 1.0f, -1.0f};
    const Vec3 k3{1.0f, 1.0f, 1.0f};
    const Vec3 sum = k0 * SceneSdf(q + k0 * kEps, tuft) + k1 * SceneSdf(q + k1 * kEps, tuft) +
                     k2 * SceneSdf(q + k2 * kEps, tuft) + k3 * SceneSdf(q + k3 * kEps, tuft);
    return Normalize(sum);
}

struct Mat3 {
    float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    Vec3 Mul(const Vec3& v) const {
        return {
            m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
            m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
            m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z,
        };
    }

    Vec3 MulTranspose(const Vec3& v) const {
        return {
            m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z,
            m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z,
            m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z,
        };
    }
};

Mat3 MakeRotationMatrix(float yaw_deg, float pitch_deg, float roll_deg) {
    const float y = yaw_deg * kDegToRad;
    const float p = pitch_deg * kDegToRad;
    const float r = roll_deg * kDegToRad;

    const float cy = std::cos(y);
    const float sy = std::sin(y);
    const float cp = std::cos(p);
    const float sp = std::sin(p);
    const float cr = std::cos(r);
    const float sr = std::sin(r);

    Mat3 rx;
    rx.m[1][1] = cp;
    rx.m[1][2] = -sp;
    rx.m[2][1] = sp;
    rx.m[2][2] = cp;

    Mat3 ry;
    ry.m[0][0] = cy;
    ry.m[0][2] = sy;
    ry.m[2][0] = -sy;
    ry.m[2][2] = cy;

    Mat3 rz;
    rz.m[0][0] = cr;
    rz.m[0][1] = -sr;
    rz.m[1][0] = sr;
    rz.m[1][1] = cr;

    auto mul = [](const Mat3& a, const Mat3& b) {
        Mat3 out;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                out.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
            }
        }
        return out;
    };
    return mul(rz, mul(ry, rx));
}

struct EyeFrame {
    Vec3 center;
    Vec3 ex;
    Vec3 ey;
    float side_sign = 1.0f;
};

EyeFrame MakeEyeFrame(float x_dir, float y_dir, float tilt_rad) {
    EyeFrame f;
    f.center = Normalize({x_dir, y_dir, 0.82f});
    f.side_sign = (x_dir < 0.0f) ? -1.0f : 1.0f;
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const Vec3 ex0 = Normalize(Cross(up, f.center));
    const Vec3 ey0 = Normalize(Cross(f.center, ex0));
    const float ct = std::cos(tilt_rad);
    const float st = std::sin(tilt_rad);
    f.ex = ex0 * ct + ey0 * st;
    f.ey = ey0 * ct - ex0 * st;
    return f;
}

inline uint8_t ClampByte(float v) {
    if (v <= 0.0f) {
        return 0;
    }
    if (v >= 255.0f) {
        return 255;
    }
    return static_cast<uint8_t>(v + 0.5f);
}

inline int64_t NowUs() {
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    return 0;
#endif
}

PIKA_IRAM bool TraceTuftPixel(const Mat3& rot,
                              const TuftControlPoints& tuft,
                              float inv_r,
                              float sx,
                              float sy,
                              Vec3* out_hit,
                              Vec3* out_norm,
                              float* out_min_d) {
    const float r2_s = sx * sx + sy * sy;
    const float z_sph = (r2_s < 1.0f) ? (std::sqrt(1.0f - r2_s) + 0.36f) : 0.72f;
    float z = std::max(0.72f, z_sph);
    float min_d = 10.0f;
    for (int step = 0; step < 12; ++step) {
        const Vec3 p_w{sx, sy, z};
        const Vec3 p_o = rot.MulTranspose(p_w);
        const float d = SceneSdf(p_o, tuft);
        if (d < min_d) {
            min_d = d;
        }
        if (d < 0.003f) {
            *out_hit = p_w;
            *out_norm = rot.Mul(SceneNormalObj(p_o, tuft));
            *out_min_d = min_d;
            return true;
        }
        z -= std::max(d, 0.005f);
        if (z < -0.50f) {
            break;
        }
    }
    if (r2_s < 0.996f) {
        const float z_hit = std::sqrt(1.0f - r2_s);
        const Vec3 p_w{sx, sy, z_hit};
        *out_hit = p_w;
        *out_norm = rot.Mul(SceneNormalObj(rot.MulTranspose(p_w), tuft));
        *out_min_d = 0.0f;
        return true;
    }
    if (min_d < 7.0f * inv_r) {
        const Vec3 p_w{sx, sy, z};
        *out_hit = p_w;
        *out_norm = rot.Mul(SceneNormalObj(rot.MulTranspose(p_w), tuft));
    }
    *out_min_d = min_d;
    return false;
}

} // namespace

Avatar3DRenderer::~Avatar3DRenderer() {
#ifdef ESP_PLATFORM
    if (matcap_lut_ != nullptr) {
        heap_caps_free(matcap_lut_);
        matcap_lut_ = nullptr;
    }
    if (matcap_565_ != nullptr) {
        heap_caps_free(matcap_565_);
        matcap_565_ = nullptr;
    }
    if (rainbow_lut_ != nullptr) {
        heap_caps_free(rainbow_lut_);
        rainbow_lut_ = nullptr;
    }
    if (sin_lut_ != nullptr) {
        heap_caps_free(sin_lut_);
        sin_lut_ = nullptr;
    }
    if (tuft_grid_ != nullptr) {
        heap_caps_free(tuft_grid_);
        tuft_grid_ = nullptr;
    }
    if (cached_orb_ != nullptr) {
        heap_caps_free(cached_orb_);
        cached_orb_ = nullptr;
    }
#endif
}

bool Avatar3DRenderer::Init() {
    if (ready_) {
        return true;
    }
    const size_t mc_count = static_cast<size_t>(kMatcapSize) * kMatcapSize;
    const size_t grid_count = static_cast<size_t>(kMaxGridDim) * kMaxGridDim;
    const size_t orb_bytes = static_cast<size_t>(kMaxStageW) * kMaxStageH * sizeof(uint16_t);
#ifdef ESP_PLATFORM
    auto alloc_fast = [](size_t bytes) -> void* {
        void* p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p == nullptr) {
            p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        return p;
    };
    auto alloc_spiram_first = [](size_t bytes) -> void* {
        void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p == nullptr) {
            p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        return p;
    };
    matcap_lut_ = static_cast<Rgb8*>(alloc_spiram_first(mc_count * sizeof(Rgb8)));
    matcap_565_ = static_cast<uint16_t*>(alloc_spiram_first(mc_count * sizeof(uint16_t)));
    rainbow_lut_ = static_cast<Rgb8*>(alloc_fast(static_cast<size_t>(kRainbowSize) * sizeof(Rgb8)));
    sin_lut_ = static_cast<float*>(alloc_fast(static_cast<size_t>(kSinSize) * sizeof(float)));
    tuft_grid_ = static_cast<TuftGridNode*>(alloc_fast(grid_count * sizeof(TuftGridNode)));
    cached_orb_ = static_cast<uint16_t*>(
        heap_caps_malloc(orb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (matcap_lut_ == nullptr || matcap_565_ == nullptr || rainbow_lut_ == nullptr ||
        sin_lut_ == nullptr || tuft_grid_ == nullptr || cached_orb_ == nullptr) {
        return false;
    }
#else
    host_matcap_.resize(mc_count);
    host_matcap_565_.resize(mc_count);
    host_rainbow_.resize(kRainbowSize);
    host_sin_.resize(kSinSize);
    host_tuft_grid_.resize(grid_count);
    host_cached_orb_.resize(static_cast<size_t>(kMaxStageW) * kMaxStageH);
    matcap_lut_ = host_matcap_.data();
    matcap_565_ = host_matcap_565_.data();
    rainbow_lut_ = host_rainbow_.data();
    sin_lut_ = host_sin_.data();
    tuft_grid_ = host_tuft_grid_.data();
    cached_orb_ = host_cached_orb_.data();
    (void)orb_bytes;
#endif
    BuildMatcapLut();
    BuildRainbowLut();
    BuildSinLut();
    tuft_cache_valid_ = false;
    orb_cache_valid_ = false;
    ready_ = true;
    return true;
}

void Avatar3DRenderer::BuildMatcapLut() {
    const Vec3 l_key = Normalize({-0.56f, -0.74f, 0.37f});
    const Vec3 l_crown = Normalize({0.28f, -0.78f, 0.56f});
    const Vec3 l_side = Normalize({0.88f, -0.06f, 0.44f});
    const Vec3 l_bot = Normalize({-0.60f, 0.58f, 0.52f});
    const Vec3 view{0.0f, 0.0f, 1.0f};
    const Vec3 h_key = Normalize(l_key + view);
    const Vec3 h_crown = Normalize(l_crown + view);
    const Vec3 h_side = Normalize(l_side + view);

    for (int iy = 0; iy < kMatcapSize; ++iy) {
        const float ny = (static_cast<float>(iy) + 0.5f) / kMatcapSize * 2.0f - 1.0f;
        for (int ix = 0; ix < kMatcapSize; ++ix) {
            const float nx = (static_cast<float>(ix) + 0.5f) / kMatcapSize * 2.0f - 1.0f;
            const float r2 = nx * nx + ny * ny;
            const float nz = (r2 < 1.0f) ? std::sqrt(1.0f - r2) : 0.0f;
            const Vec3 n = Normalize({nx, ny, std::max(nz, 0.03f)});

            // Calibrated directly to measured reference obsidian RGBs: belly ~(7,11,22), nose ~(25,24,30).
            float r = 4.0f;
            float g = 6.0f;
            float b = 11.0f;

            const float diff_key = std::max(0.0f, Dot(n, l_key));
            r += diff_key * 10.0f;
            g += diff_key * 10.0f;
            b += diff_key * 14.0f;

            // Soft pearl-mauve studio softbox forehead reflection: target ~(181, 154, 186).
            const float ndh = std::max(0.0f, Dot(n, h_key));
            const float spec_broad = std::pow(ndh, 9.0f);
            const float spec_crisp = std::pow(ndh, 24.0f);
            r += spec_broad * 68.0f + spec_crisp * 88.0f;
            g += spec_broad * 54.0f + spec_crisp * 72.0f;
            b += spec_broad * 72.0f + spec_crisp * 90.0f;

            // Steel-blue crown highlight beneath tuft: target ~(84, 125, 168).
            const float crown_sheen = std::pow(std::max(0.0f, Dot(n, h_crown)), 12.0f);
            r += crown_sheen * 44.0f;
            g += crown_sheen * 80.0f;
            b += crown_sheen * 118.0f;

            // Subtle grazing silver edge on right silhouette.
            const float fresnel = std::pow(1.0f - Clamp01(nz), 3.4f);
            const float side_sheen = std::pow(std::max(0.0f, Dot(n, h_side)), 7.0f);
            r += side_sheen * fresnel * 92.0f;
            g += side_sheen * fresnel * 84.0f;
            b += side_sheen * fresnel * 104.0f;

            // Faint cool eye bounce on dark lower chin.
            const float bot_glow = std::pow(std::max(0.0f, Dot(n, l_bot)), 3.0f);
            r += bot_glow * 8.0f;
            g += bot_glow * 20.0f;
            b += bot_glow * 34.0f;

            // Tight neutral-silver Fresnel rim separating jet-black orb cleanly from #000000.
            r += fresnel * 76.0f;
            g += fresnel * 78.0f;
            b += fresnel * 92.0f;

            const uint8_t cr = ClampByte(r);
            const uint8_t cg = ClampByte(g);
            const uint8_t cb = ClampByte(b);
            const int idx = iy * kMatcapSize + ix;
            matcap_lut_[idx] = {cr, cg, cb};
            matcap_565_[idx] = MakeRgb565(cr, cg, cb);
        }
    }
}

void Avatar3DRenderer::BuildRainbowLut() {
    struct Stop {
        float pos;
        float r, g, b;
    };
    // Exact pixel-sampled 8-stop rainbow from reference: ruby top-right -> amethyst bottom-left.
    constexpr Stop kStops[] = {
        {0.00f, 172.0f, 34.0f, 52.0f},   // deep ruby-crimson
        {0.16f, 232.0f, 54.0f, 28.0f},   // scarlet red
        {0.32f, 244.0f, 126.0f, 20.0f},  // amber orange
        {0.48f, 234.0f, 208.0f, 40.0f},  // golden yellow
        {0.62f, 122.0f, 240.0f, 94.0f},  // spring lime-emerald
        {0.76f, 56.0f, 236.0f, 226.0f},  // aqua cyan
        {0.88f, 66.0f, 126.0f, 228.0f},  // cobalt blue
        {1.00f, 132.0f, 58.0f, 202.0f},  // amethyst violet
    };
    constexpr size_t kStopCount = sizeof(kStops) / sizeof(kStops[0]);

    for (int i = 0; i < kRainbowSize; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kRainbowSize - 1);
        size_t s = 0;
        while (s + 2 < kStopCount && t > kStops[s + 1].pos) {
            ++s;
        }
        const Stop& a = kStops[s];
        const Stop& b = kStops[s + 1];
        const float local = Clamp01((t - a.pos) / (b.pos - a.pos));
        const float u = 0.5f - 0.5f * std::cos(local * kPi);
        rainbow_lut_[i] = {
            ClampByte(a.r + (b.r - a.r) * u),
            ClampByte(a.g + (b.g - a.g) * u),
            ClampByte(a.b + (b.b - a.b) * u),
        };
    }
}

void Avatar3DRenderer::BuildSinLut() {
    for (int i = 0; i < kSinSize; ++i) {
        const float angle = (static_cast<float>(i) / static_cast<float>(kSinSize)) * kTwoPi;
        sin_lut_[i] = std::sin(angle);
    }
}

PIKA_IRAM inline float Avatar3DRenderer::FastSin01(float phase_0_to_1) const {
    const float wrapped = phase_0_to_1 - std::floor(phase_0_to_1);
    const int idx = static_cast<int>(wrapped * static_cast<float>(kSinSize)) & (kSinSize - 1);
    return sin_lut_[idx];
}

PIKA_IRAM inline Avatar3DRenderer::Rgb8 Avatar3DRenderer::SampleMatcap(float nx,
                                                                       float ny,
                                                                       float rim_boost) const {
    const int ix = std::clamp(static_cast<int>((0.5f + 0.495f * nx) * (kMatcapSize - 1)), 0,
                              kMatcapSize - 1);
    const int iy = std::clamp(static_cast<int>((0.5f + 0.495f * ny) * (kMatcapSize - 1)), 0,
                              kMatcapSize - 1);
    const Rgb8 base = matcap_lut_[iy * kMatcapSize + ix];
    if (std::abs(rim_boost - 1.0f) < 0.02f) {
        return base;
    }
    return {
        ClampByte(base.r * rim_boost),
        ClampByte(base.g * rim_boost),
        ClampByte(base.b * rim_boost),
    };
}

PIKA_IRAM inline uint16_t Avatar3DRenderer::SampleMatcap565Fast(float nx, float ny) const {
    const int ix = std::clamp(static_cast<int>((0.5f + 0.495f * nx) * (kMatcapSize - 1)), 0,
                              kMatcapSize - 1);
    const int iy = std::clamp(static_cast<int>((0.5f + 0.495f * ny) * (kMatcapSize - 1)), 0,
                              kMatcapSize - 1);
    return matcap_565_[iy * kMatcapSize + ix];
}

PIKA_IRAM inline Avatar3DRenderer::Rgb8 Avatar3DRenderer::SampleRainbow(float t,
                                                                        float warm_shift) const {
    const float clamped = Clamp01(t);
    const int idx = std::min(kRainbowSize - 1, static_cast<int>(clamped * (kRainbowSize - 1)));
    const Rgb8 c = rainbow_lut_[idx];
    if (warm_shift <= 0.01f) {
        return c;
    }
    const float r = c.r + warm_shift * 26.0f;
    const float g = std::min(255.0f, c.g + warm_shift * 22.0f);
    const float b = c.b * (1.0f - 0.14f * warm_shift);
    return {ClampByte(r), ClampByte(g), ClampByte(b)};
}

struct Avatar3DRenderer::ShadeCtx {
    EyeFrame eye_w[2];
    float inv_rx[2] = {1.0f, 1.0f};
    float inv_ry[2] = {1.0f, 1.0f};
    Vec3 rot_col1;
    Vec3 rot_col2;
    Vec3 l_glass;
    AvatarPose pose;
    bool fast_rim_unity = true;
};

PIKA_IRAM uint16_t Avatar3DRenderer::ShadeSurface(const ShadeCtx& ctx,
                                                  float hx,
                                                  float hy,
                                                  float hz,
                                                  float nx,
                                                  float ny,
                                                  float nz,
                                                  float edge_alpha,
                                                  uint8_t eye_mask) const {
    const Vec3 hit_world{hx, hy, hz};
    const Vec3 n_world{nx, ny, nz};
    if (eye_mask != 0) {
        const float p_obj_z = Dot(hit_world, ctx.rot_col2);
        const float p_obj_y = Dot(hit_world, ctx.rot_col1);
        if (p_obj_z > 0.12f && p_obj_y > -0.52f && p_obj_y < 0.74f) {
            int best_idx = -1;
            float best_r_socket2 = 1.15f * 1.15f;
            float best_u = 0.0f;
            float best_v = 0.0f;
            float best_v_adj = 0.0f;
            float best_su = 0.0f;
            float best_sv = 0.0f;

            for (int e_idx = 0; e_idx < 2; ++e_idx) {
                if ((eye_mask & (1U << e_idx)) == 0) {
                    continue;
                }
                const EyeFrame& ef = ctx.eye_w[e_idx];
                if (Dot(hit_world, ef.center) <= 0.16f) {
                    continue;
                }
                const float dx = Dot(hit_world, ef.ex);
                const float dy = Dot(hit_world, ef.ey);

                const float u = dx * ctx.inv_rx[e_idx];
                const float v = dy * ctx.inv_ry[e_idx];
                const float u_out = u * ef.side_sign;

                // Smooth organic oval dome with subtle outer taper tilted ~-44 deg.
                const float v_arc = v - 0.03f * u_out;
                const float taper = 1.0f + 0.10f * u_out;
                const float v_adj = v_arc * taper;

                // Sculpted deep orbital crescent shifted up-outward (squared reject avoids sqrt).
                const float su = (u - 0.17f * ef.side_sign) * (1.0f / 1.17f);
                const float sv = ((v_arc + 0.08f) * (1.0f + 0.18f * u_out)) * (1.0f / 1.15f);
                const float r_socket2 = su * su + sv * sv;

                if (r_socket2 < best_r_socket2) {
                    best_idx = e_idx;
                    best_r_socket2 = r_socket2;
                    best_u = u;
                    best_v = v;
                    best_v_adj = v_adj;
                    best_su = su;
                    best_sv = sv;
                }
            }

            if (best_idx >= 0) {
                const float r_socket = std::sqrt(best_r_socket2);
                const float r_eye = std::sqrt(best_u * best_u + best_v_adj * best_v_adj);
                const EyeFrame& ef = ctx.eye_w[best_idx];
                const float u = best_u;
                const float v = best_v;
                const float su = best_su;
                const float sv = best_sv;
                const float u_out = u * ef.side_sign;

                float out_r = 0.0f;
                float out_g = 0.0f;
                float out_b = 0.0f;

                if (r_socket >= 1.0f) {
                    const float t_bevel = Clamp01((r_socket - 1.0f) / 0.15f);
                    const float sink = 4.0f * t_bevel * (1.0f - t_bevel) * 0.42f;
                    const float inv_rs = 1.0f / std::max(r_socket, 0.01f);
                    const Vec3 dir_w =
                        (ef.ex * (su * inv_rs) + ef.ey * (sv * inv_rs)) * (-sink);
                    const Vec3 bent_n = Normalize(n_world + dir_w);
                    const Rgb8 mat = SampleMatcap(bent_n.x, bent_n.y, ctx.pose.rim_boost);
                    const float cavity_ao = 0.24f + 0.76f * SmoothStep(1.0f, 1.13f, r_socket);
                    const float spill = SmoothStep(1.14f, 1.0f, r_socket) *
                                        SmoothStep(0.10f, 0.85f, v) * 0.18f * ctx.pose.eye_brightness;
                    out_r = mat.r * cavity_ao + spill * 32.0f;
                    out_g = mat.g * cavity_ao + spill * 62.0f;
                    out_b = mat.b * cavity_ao + spill * 92.0f;
                } else if (r_eye >= 1.0f) {
                    const float shelf_blend = SmoothStep(1.0f, 0.90f, r_socket);
                    const Vec3 recess_n =
                        Normalize(n_world - (ef.ex * su + ef.ey * sv) * 0.35f);
                    const Rgb8 wall = SampleMatcap(recess_n.x, recess_n.y, ctx.pose.rim_boost);
                    const float dark = 0.11f + 0.22f * (1.0f - shelf_blend);
                    out_r = wall.r * dark;
                    out_g = wall.g * dark;
                    out_b = wall.b * dark;
                } else {
                    const float v_top = -1.04f + 2.08f * ctx.pose.upper_lid + 0.14f * u * u;
                    const float v_bot = +1.04f - 2.08f * ctx.pose.lower_lid -
                                        ctx.pose.happy_arch * (1.10f - 0.65f * u * u);

                    const float gap = v_bot - v_top;
                    const float open_d = std::min(v - v_top, v_bot - v);
                    const float open_alpha =
                        (gap <= 0.02f) ? 0.0f : SmoothStep(-0.04f, 0.05f, open_d);

                    float lid_ao = 0.36f + 0.38f * SmoothStep(1.0f, 0.45f, r_eye);
                    const float lid_curl = (v < v_top)
                                               ? 0.38f * SmoothStep(0.35f, 0.0f, v_top - v)
                                               : -0.48f * SmoothStep(0.35f, 0.0f, v - v_bot);
                    if (gap < 0.14f && std::abs(v - 0.5f * (v_top + v_bot)) < 0.08f) {
                        lid_ao *= 0.35f;
                    }
                    const Vec3 lid_n = Normalize(n_world + ef.ey * lid_curl);
                    const Rgb8 lid_col = SampleMatcap(lid_n.x, lid_n.y, ctx.pose.rim_boost);
                    const float lr = lid_col.r * lid_ao;
                    const float lg = lid_col.g * lid_ao;
                    const float lb = lid_col.b * lid_ao;

                    if (open_alpha <= 0.005f) {
                        out_r = lr;
                        out_g = lg;
                        out_b = lb;
                    } else {
                        const float drift = 0.035f * FastSin01(ctx.pose.rainbow_phase);
                        const float swirl =
                            0.14f * ctx.pose.swirl_amount *
                            FastSin01(r_eye * 0.65f - ctx.pose.rainbow_phase);
                        const float grad = 0.45f - 0.50f * u_out + 0.20f * v +
                                           ctx.pose.focus_shift_x * 0.22f * ef.side_sign +
                                           ctx.pose.focus_shift_y * 0.22f + swirl + drift;
                        const Rgb8 glow = SampleRainbow(grad, ctx.pose.warm_shift);

                        const float mesh_w = SmoothStep(0.20f, 0.78f, Clamp01(grad));
                        const float m1 = FastSin01(u * 6.5f);
                        const float m2 = FastSin01(v * 6.5f);
                        const float mesh = 1.0f - 0.06f * mesh_w * (0.5f + 0.5f * m1 * m2);

                        const Vec3 glass_n =
                            Normalize(n_world + (ef.ex * u + ef.ey * v) * 0.45f);
                        const float ndl = std::max(0.0f, Dot(glass_n, ctx.l_glass));
                        const float ndl2 = ndl * ndl;
                        const float ndl4 = ndl2 * ndl2;
                        const float soft_sheen = ndl4 * ndl4 * 26.0f;
                        const float rim_glass = SmoothStep(0.76f, 0.97f, r_eye);

                        const float sp_x = (u_out - 0.42f);
                        const float sp_y = (v + 0.18f);
                        const float spec_dot = SmoothStep(
                            0.22f, 0.82f, FastFalloff(sp_x * sp_x * 88.0f + sp_y * sp_y * 128.0f));

                        const float fx = u + ctx.pose.focus_shift_x * 0.45f;
                        const float fy = v + ctx.pose.focus_shift_y * 0.45f;
                        const float focus_mag =
                            std::abs(ctx.pose.focus_shift_x) + std::abs(ctx.pose.focus_shift_y);
                        const float caustic =
                            focus_mag * 42.0f * FastFalloff((fx * fx + fy * fy) * 8.0f);
                        const float bloom = ctx.pose.core_bloom * FastFalloff(r_eye * r_eye * 5.5f);
                        const float lens_ao = 0.52f + 0.48f * SmoothStep(1.0f, 0.60f, r_eye);

                        const float brightness = ctx.pose.eye_brightness * mesh * lens_ao;
                        const float gr = glow.r * brightness + rim_glass * 70.0f +
                                         spec_dot * 248.0f + soft_sheen * 0.8f + caustic * 0.8f +
                                         bloom * 175.0f;
                        const float gg = glow.g * brightness + rim_glass * 90.0f +
                                         spec_dot * 250.0f + soft_sheen * 0.9f + caustic * 1.0f +
                                         bloom * 225.0f;
                        const float gb = glow.b * brightness + rim_glass * 125.0f +
                                         spec_dot * 255.0f + soft_sheen * 1.1f + caustic * 1.1f +
                                         bloom * 245.0f;

                        const float rim_mask = SmoothStep(1.0f, 0.91f, r_eye) * open_alpha;
                        const float inv_mask = 1.0f - rim_mask;
                        out_r = lr * inv_mask + gr * rim_mask;
                        out_g = lg * inv_mask + gg * rim_mask;
                        out_b = lb * inv_mask + gb * rim_mask;
                    }
                }

                if (edge_alpha < 0.999f) {
                    out_r *= edge_alpha;
                    out_g *= edge_alpha;
                    out_b *= edge_alpha;
                }
                return MakeRgb565(ClampByte(out_r), ClampByte(out_g), ClampByte(out_b));
            }
        }
    }

    if (edge_alpha >= 0.999f && ctx.fast_rim_unity) {
        return SampleMatcap565Fast(n_world.x, n_world.y);
    }
    const Rgb8 body = SampleMatcap(n_world.x, n_world.y, ctx.pose.rim_boost);
    return MakeRgb565(ClampByte(body.r * edge_alpha), ClampByte(body.g * edge_alpha),
                      ClampByte(body.b * edge_alpha));
}

PIKA_IRAM UiRect Avatar3DRenderer::Render(uint16_t* fb,
                                          int fb_w,
                                          int fb_h,
                                          const AvatarPose& pose,
                                          int center_x,
                                          int center_y,
                                          float radius_px,
                                          bool fast_half_res,
                                          bool* out_changed) const {
    // Even-aligned dirty rect covering full R=132px sphere + inward-hooked tuft + shadow.
    const int stage_x0 = std::max(0, (center_x - 156) & ~1);
    const int stage_y0 = std::max(0, (center_y - 188) & ~1);
    const int stage_x1 = std::min(fb_w, ((center_x + 156) + 1) & ~1);
    const int stage_y1 = std::min(fb_h, ((center_y + 162) + 1) & ~1);
    const UiRect dirty{stage_x0, stage_y0, stage_x1 - stage_x0, stage_y1 - stage_y0};

    const int stage_w = stage_x1 - stage_x0;
    const int stage_h = stage_y1 - stage_y0;
    if (!ready_ || fb == nullptr || stage_w <= 0 || stage_w > 480 || stage_h <= 0) {
        if (out_changed != nullptr) {
            *out_changed = true;
        }
        return dirty;
    }

    const float cy = static_cast<float>(center_y) + pose.bob_y;
    const float cx = static_cast<float>(center_x);
    const int icx = static_cast<int>(std::lround(cx));
    const int icy = static_cast<int>(std::lround(cy));

    // Fast 1:1 sprite blit when stationary pose matches cached render within tolerance.
    const int bob_dy = icy - cached_orb_icy_;
    float d_phase = std::abs(pose.rainbow_phase - cached_orb_pose_.rainbow_phase);
    if (d_phase > 0.5f) {
        d_phase = 1.0f - d_phase;
    }
    const float max_phase_diff = (pose.swirl_amount < 0.05f) ? 0.18f : 0.04f;
    const bool sprite_hit =
        !fast_half_res && orb_cache_valid_ && cached_orb_ != nullptr &&
        stage_w == cached_orb_w_ && stage_h == cached_orb_h_ &&
        stage_x0 == cached_orb_x0_ && stage_y0 == cached_orb_y0_ &&
        icx == cached_orb_icx_ && std::abs(bob_dy) <= 10 &&
        std::abs(radius_px - cached_orb_radius_) <= 0.1f &&
        std::abs(pose.yaw_deg - cached_orb_pose_.yaw_deg) <= 0.15f &&
        std::abs(pose.pitch_deg - cached_orb_pose_.pitch_deg) <= 0.15f &&
        std::abs(pose.roll_deg - cached_orb_pose_.roll_deg) <= 0.15f &&
        std::abs(pose.scale - cached_orb_pose_.scale) <= 0.004f &&
        std::abs(pose.upper_lid - cached_orb_pose_.upper_lid) <= 0.01f &&
        std::abs(pose.lower_lid - cached_orb_pose_.lower_lid) <= 0.01f &&
        std::abs(pose.happy_arch - cached_orb_pose_.happy_arch) <= 0.01f &&
        std::abs(pose.eye_aspect_y - cached_orb_pose_.eye_aspect_y) <= 0.01f &&
        std::abs(pose.left_eye_scale - cached_orb_pose_.left_eye_scale) <= 0.01f &&
        std::abs(pose.right_eye_scale - cached_orb_pose_.right_eye_scale) <= 0.01f &&
        std::abs(pose.core_bloom - cached_orb_pose_.core_bloom) <= 0.015f &&
        std::abs(pose.swirl_amount - cached_orb_pose_.swirl_amount) <= 0.015f &&
        std::abs(pose.focus_shift_x - cached_orb_pose_.focus_shift_x) <= 0.015f &&
        std::abs(pose.focus_shift_y - cached_orb_pose_.focus_shift_y) <= 0.015f &&
        std::abs(pose.warm_shift - cached_orb_pose_.warm_shift) <= 0.015f &&
        std::abs(pose.rim_boost - cached_orb_pose_.rim_boost) <= 0.015f &&
        std::abs(pose.eye_brightness - cached_orb_pose_.eye_brightness) <= 0.015f &&
        std::abs(pose.shadow_tint - cached_orb_pose_.shadow_tint) <= 0.015f &&
        d_phase <= max_phase_diff &&
        std::abs(pose.tuft_lag_x - cached_orb_pose_.tuft_lag_x) <= 0.004f &&
        std::abs(pose.tuft_lag_y - cached_orb_pose_.tuft_lag_y) <= 0.004f;

    if (sprite_hit) {
        if (out_changed != nullptr && icy == last_blitted_icy_) {
            *out_changed = false;
            last_prepass_us_ = 0;
            last_rows_us_ = 0;
            last_cache_hit_ = 1;
            return dirty;
        }
        const int64_t t_blit = NowUs();
        const size_t row_bytes = static_cast<size_t>(stage_w) * sizeof(uint16_t);
        for (int py = stage_y0; py < stage_y1; ++py) {
            uint16_t* dst = fb + static_cast<size_t>(py) * fb_w + stage_x0;
            const int src_y = (py - stage_y0) - bob_dy;
            if (src_y < 0 || src_y >= cached_orb_h_) {
                std::memset(dst, 0, row_bytes);
            } else {
                std::memcpy(dst, cached_orb_ + static_cast<size_t>(src_y) * cached_orb_w_,
                            row_bytes);
            }
        }
        last_blitted_icy_ = icy;
        if (out_changed != nullptr) {
            *out_changed = true;
        }
        last_prepass_us_ = 0;
        last_rows_us_ = static_cast<uint32_t>(NowUs() - t_blit);
        last_cache_hit_ = 1;
        return dirty;
    }

    if (fast_half_res) {
        orb_cache_valid_ = false;
        last_cache_hit_ = 2;
    } else {
        last_cache_hit_ = 0;
    }

    const int64_t t_pre_start = NowUs();
    const float r_px = std::max(24.0f, radius_px * pose.scale);
    const float inv_r = 1.0f / r_px;

    const Mat3 rot = MakeRotationMatrix(pose.yaw_deg, pose.pitch_deg, pose.roll_deg);

    TuftControlPoints tuft;
    tuft.q1.x += pose.tuft_lag_x * 0.25f;
    tuft.q1.y += pose.tuft_lag_y * 0.25f;
    tuft.q2.x += pose.tuft_lag_x * 0.60f;
    tuft.q2.y += pose.tuft_lag_y * 0.60f;
    tuft.q3.x += pose.tuft_lag_x;
    tuft.q3.y += pose.tuft_lag_y;

    const bool cache_hit =
        tuft_cache_valid_ &&
        std::abs(pose.yaw_deg - last_tuft_yaw_) <= 0.12f &&
        std::abs(pose.pitch_deg - last_tuft_pitch_) <= 0.12f &&
        std::abs(pose.roll_deg - last_tuft_roll_) <= 0.12f &&
        std::abs(pose.tuft_lag_x - last_tuft_lag_x_) <= 0.003f &&
        std::abs(pose.tuft_lag_y - last_tuft_lag_y_) <= 0.003f &&
        std::abs(r_px - last_tuft_r_px_) <= 0.5f;

    if (!cache_hit && tuft_grid_ != nullptr) {
        const Vec3 tw[4] = {rot.Mul(tuft.q0), rot.Mul(tuft.q1), rot.Mul(tuft.q2), rot.Mul(tuft.q3)};
        const float tr[4] = {0.33f + 0.36f, 0.20f + 0.34f, 0.105f + 0.32f, 0.018f + 0.28f};
        float t_min_x = 10.0f;
        float t_max_x = -10.0f;
        float t_min_y = 10.0f;
        float t_max_y = -10.0f;
        for (int i = 0; i < 4; ++i) {
            t_min_x = std::min(t_min_x, tw[i].x - tr[i]);
            t_max_x = std::max(t_max_x, tw[i].x + tr[i]);
            t_min_y = std::min(t_min_y, tw[i].y - tr[i]);
            t_max_y = std::max(t_max_y, tw[i].y + tr[i]);
        }
        t_max_y = std::min(t_max_y, -0.48f);

        cached_rel_bx0_ = static_cast<int>(std::floor(t_min_x * r_px)) & ~7;
        cached_rel_bx1_ = (static_cast<int>(std::ceil(t_max_x * r_px)) + 7) & ~7;
        cached_rel_by0_ = std::max(-192, static_cast<int>(std::floor(t_min_y * r_px))) & ~7;
        cached_rel_by1_ = (static_cast<int>(std::ceil(t_max_y * r_px)) + 7) & ~7;
        cached_rel_bx1_ = std::min(cached_rel_bx1_, cached_rel_bx0_ + (kMaxGridDim - 1) * 8);
        cached_rel_by1_ = std::min(cached_rel_by1_, cached_rel_by0_ + (kMaxGridDim - 1) * 8);
        cached_gw_ = (cached_rel_bx1_ > cached_rel_bx0_) ? ((cached_rel_bx1_ - cached_rel_bx0_) / 8 + 1) : 0;
        cached_gh_ = (cached_rel_by1_ > cached_rel_by0_) ? ((cached_rel_by1_ - cached_rel_by0_) / 8 + 1) : 0;

        if (cached_gw_ >= 2 && cached_gh_ >= 2) {
            for (int gy = 0; gy < cached_gh_; ++gy) {
                const float sy = (static_cast<float>(cached_rel_by0_ + gy * 8) + 0.5f) * inv_r;
                TuftGridNode* g_row = tuft_grid_ + gy * kMaxGridDim;
                for (int gx = 0; gx < cached_gw_; ++gx) {
                    const float sx = (static_cast<float>(cached_rel_bx0_ + gx * 8) + 0.5f) * inv_r;
                    Vec3 h{};
                    Vec3 n{};
                    float md = 10.0f;
                    const bool ok = TraceTuftPixel(rot, tuft, inv_r, sx, sy, &h, &n, &md);
                    g_row[gx] = {ok, md, h.z, n.x, n.y, n.z};
                }
            }
        }
        last_tuft_yaw_ = pose.yaw_deg;
        last_tuft_pitch_ = pose.pitch_deg;
        last_tuft_roll_ = pose.roll_deg;
        last_tuft_lag_x_ = pose.tuft_lag_x;
        last_tuft_lag_y_ = pose.tuft_lag_y;
        last_tuft_r_px_ = r_px;
        tuft_cache_valid_ = true;
    }

    const int bx0 = icx + cached_rel_bx0_;
    const int bx1 = icx + cached_rel_bx1_;
    const int by0 = icy + cached_rel_by0_;
    const int by1 = icy + cached_rel_by1_;
    const int draw_bx0 = std::max(stage_x0, bx0);
    const int draw_bx1 = std::min(stage_x1, bx1);
    const int draw_by0 = std::max(stage_y0, by0);
    const int draw_by1 = std::min(stage_y1, by1);
    const int gw = cached_gw_;
    const int gh = cached_gh_;

    // Steep ~44 deg tilted almond visor frames preserving clean obsidian nose bridge.
    const EyeFrame left_eye = MakeEyeFrame(-0.55f, +0.15f, +0.76f);
    const EyeFrame right_eye = MakeEyeFrame(+0.55f, +0.15f, -0.76f);
    const Vec3 l_glass = Normalize({-0.55f, -0.60f, 0.58f});

    // Project tight 2D screen bounding boxes for both eyes to skip eye math on body pixels.
    struct EyeBox {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        bool active = false;
    };
    EyeBox eye_box[2];
    for (int e_idx = 0; e_idx < 2; ++e_idx) {
        const EyeFrame& ef = (e_idx == 0) ? left_eye : right_eye;
        const float e_scale = (e_idx == 0) ? pose.left_eye_scale : pose.right_eye_scale;
        const Vec3 cw = rot.Mul(ef.center);
        if (cw.z <= -0.35f) {
            continue;
        }
        const Vec3 exw = rot.Mul(ef.ex);
        const Vec3 eyw = rot.Mul(ef.ey);
        const float ext_u = 0.72f * e_scale;
        const float ext_v = 0.54f * e_scale * pose.eye_aspect_y;
        const float min_xw =
            std::min(cw.x, 0.16f * cw.x) - ext_u * std::abs(exw.x) - ext_v * std::abs(eyw.x);
        const float max_xw =
            std::max(cw.x, 0.16f * cw.x) + ext_u * std::abs(exw.x) + ext_v * std::abs(eyw.x);
        const float min_yw =
            std::min(cw.y, 0.16f * cw.y) - ext_u * std::abs(exw.y) - ext_v * std::abs(eyw.y);
        const float max_yw =
            std::max(cw.y, 0.16f * cw.y) + ext_u * std::abs(exw.y) + ext_v * std::abs(eyw.y);
        eye_box[e_idx].x0 =
            std::clamp(static_cast<int>(std::floor(cx + min_xw * r_px)) - 2, stage_x0, stage_x1);
        eye_box[e_idx].x1 =
            std::clamp(static_cast<int>(std::ceil(cx + max_xw * r_px)) + 2, stage_x0, stage_x1);
        eye_box[e_idx].y0 =
            std::clamp(static_cast<int>(std::floor(cy + min_yw * r_px)) - 2, stage_y0, stage_y1);
        eye_box[e_idx].y1 =
            std::clamp(static_cast<int>(std::ceil(cy + max_yw * r_px)) + 2, stage_y0, stage_y1);
        eye_box[e_idx].active =
            (eye_box[e_idx].x1 > eye_box[e_idx].x0) && (eye_box[e_idx].y1 > eye_box[e_idx].y0);
    }

    const float shadow_cy = static_cast<float>(center_y) + radius_px + 18.0f;
    const float shadow_rx = radius_px * (0.86f - pose.bob_y * 0.008f);
    const float shadow_ry = 9.0f;
    const float inv_shadow_rx2 = 1.0f / (shadow_rx * shadow_rx);
    const float inv_shadow_ry2 = 1.0f / (shadow_ry * shadow_ry);

    const float aa_step = 1.6f * inv_r;
    const float inv_aa_step = 1.0f / aa_step;
    const bool fast_rim_unity = (std::abs(pose.rim_boost - 1.0f) < 0.02f);

    const ShadeCtx shade_ctx{
        {
            {rot.Mul(left_eye.center), rot.Mul(left_eye.ex), rot.Mul(left_eye.ey),
             left_eye.side_sign},
            {rot.Mul(right_eye.center), rot.Mul(right_eye.ex), rot.Mul(right_eye.ey),
             right_eye.side_sign},
        },
        {1.0f / (0.45f * pose.left_eye_scale), 1.0f / (0.45f * pose.right_eye_scale)},
        {1.0f / (0.30f * pose.left_eye_scale * pose.eye_aspect_y),
         1.0f / (0.30f * pose.right_eye_scale * pose.eye_aspect_y)},
        {rot.m[0][1], rot.m[1][1], rot.m[2][1]},
        {rot.m[0][2], rot.m[1][2], rot.m[2][2]},
        l_glass,
        pose,
        fast_rim_unity,
    };

    auto shade_shadow = [&](int px, float sh_dy) -> uint16_t {
        const float sh_dx = static_cast<float>(px) + 0.5f - cx;
        const float sd2 = sh_dx * sh_dx * inv_shadow_rx2 + sh_dy * sh_dy * inv_shadow_ry2;
        if (sd2 >= 1.35f) {
            return 0x0000;
        }
        const float g = FastFalloff(sd2 * 2.4f) * (0.45f + 0.55f * pose.shadow_tint);
        return MakeRgb565(ClampByte((14.0f + 16.0f * pose.shadow_tint) * g),
                          ClampByte((16.0f + 46.0f * pose.shadow_tint) * g),
                          ClampByte((28.0f + 78.0f * pose.shadow_tint) * g));
    };

    const int64_t t_rows_start = NowUs();
    last_prepass_us_ = static_cast<uint32_t>(t_rows_start - t_pre_start);

    uint16_t row_buf[480];
    const float scale_mc = 0.495f * static_cast<float>(kMatcapSize - 1);
    const int dix_fp = static_cast<int>(scale_mc * inv_r * 65536.0f);
    const int step = fast_half_res ? 2 : 1;
    const size_t row_bytes = static_cast<size_t>(stage_w) * sizeof(uint16_t);

    for (int py = stage_y0; py < stage_y1; py += step) {
        const float sy = (static_cast<float>(py) + 0.5f - cy) * inv_r;
        const float sy2 = sy * sy;
        const float sh_dy = static_cast<float>(py) + 0.5f - shadow_cy;
        const bool in_shadow_y = (std::abs(sh_dy) < shadow_ry * 1.4f);
        const bool row_in_tuft = (gw >= 2 && gh >= 2 && py >= draw_by0 && py < draw_by1);
        const int gy = row_in_tuft ? ((py - by0) >> 3) : 0;
        const float fy = row_in_tuft ? (static_cast<float>((py - by0) & 7) * 0.125f) : 0.0f;

        if (!row_in_tuft && sy2 >= 1.032f && !in_shadow_y) {
            uint16_t* dst_row = fb + static_cast<size_t>(py) * fb_w + stage_x0;
            std::memset(dst_row, 0, row_bytes);
            if (fast_half_res && py + 1 < stage_y1) {
                std::memset(fb + static_cast<size_t>(py + 1) * fb_w + stage_x0, 0, row_bytes);
            } else if (!fast_half_res && cached_orb_ != nullptr && stage_w <= kMaxStageW &&
                       stage_h <= kMaxStageH) {
                std::memset(cached_orb_ + static_cast<size_t>(py - stage_y0) * stage_w, 0,
                            row_bytes);
            }
            continue;
        }

        const int mc_iy = std::clamp(static_cast<int>((0.5f + 0.495f * sy) * (kMatcapSize - 1)), 0,
                                     kMatcapSize - 1);
        const uint16_t* mc_row = matcap_565_ + mc_iy * kMatcapSize;

        auto fill_bg_span = [&](int x0, int x1) {
            if (x1 <= x0) {
                return;
            }
            std::memset(row_buf + (x0 - stage_x0), 0, static_cast<size_t>(x1 - x0) * sizeof(uint16_t));
            if (in_shadow_y) {
                const float rem = std::max(0.0f, 1.35f - sh_dy * sh_dy * inv_shadow_ry2);
                const float max_sh_dx = std::sqrt(rem) * shadow_rx;
                const int sx0 = std::max(x0, static_cast<int>(std::floor(cx - max_sh_dx)));
                const int sx1 = std::min(x1, static_cast<int>(std::ceil(cx + max_sh_dx)) + 1);
                for (int px = sx0; px < sx1; px += step) {
                    const uint16_t c = shade_shadow(px, sh_dy);
                    const int off = px - stage_x0;
                    row_buf[off] = c;
                    if (fast_half_res && px + 1 < sx1) {
                        row_buf[off + 1] = c;
                    }
                }
            }
        };

        const bool e0_row = eye_box[0].active && py >= eye_box[0].y0 && py < eye_box[0].y1;
        const bool e1_row = eye_box[1].active && py >= eye_box[1].y0 && py < eye_box[1].y1;
        auto eye_mask_for_x = [&](int px) -> uint8_t {
            uint8_t m = 0;
            if (e0_row && px >= eye_box[0].x0 && px < eye_box[0].x1) {
                m |= 1U;
            }
            if (e1_row && px >= eye_box[1].x0 && px < eye_box[1].x1) {
                m |= 2U;
            }
            return m;
        };

        auto fill_fast_body = [&](int x0, int x1) {
            if (x1 <= x0) {
                return;
            }
            if (!fast_rim_unity) {
                for (int px = x0; px < x1; ++px) {
                    const float sx = (static_cast<float>(px) + 0.5f - cx) * inv_r;
                    const float z = std::sqrt(std::max(0.0f, 1.0f - (sx * sx + sy2)));
                    row_buf[px - stage_x0] =
                        ShadeSurface(shade_ctx, sx, sy, z, sx, sy, z, 1.0f, 0);
                }
                return;
            }
            const float sx0 = (static_cast<float>(x0) + 0.5f - cx) * inv_r;
            const float ix0 =
                std::clamp((0.5f + 0.495f * sx0) * static_cast<float>(kMatcapSize - 1), 0.0f,
                           static_cast<float>(kMatcapSize - 1) - 0.01f);
            int ix_fp = static_cast<int>(ix0 * 65536.0f);
            uint16_t* dst = row_buf + (x0 - stage_x0);
            const int n = x1 - x0;
            for (int i = 0; i < n; ++i) {
                dst[i] = mc_row[ix_fp >> 16];
                ix_fp += dix_fp;
            }
        };

        auto fill_eye_span = [&](int x0, int x1, uint8_t mask) {
            for (int px = x0; px < x1; px += step) {
                const float sx = (static_cast<float>(px) + 0.5f - cx) * inv_r;
                const float z = std::sqrt(std::max(0.0f, 1.0f - (sx * sx + sy2)));
                const uint16_t c = ShadeSurface(shade_ctx, sx, sy, z, sx, sy, z, 1.0f, mask);
                const int off = px - stage_x0;
                row_buf[off] = c;
                if (fast_half_res && px + 1 < x1) {
                    row_buf[off + 1] = c;
                }
            }
        };

        auto fill_aa_span = [&](int x0, int x1) {
            for (int px = x0; px < x1; ++px) {
                const float sx = (static_cast<float>(px) + 0.5f - cx) * inv_r;
                const float r2 = sx * sx + sy2;
                const float r = std::sqrt(r2);
                const float z = (r < 1.0f) ? std::sqrt(1.0f - r2) : 0.0f;
                const float edge = Clamp01((1.0f - r) * inv_aa_step * 0.5f + 0.5f);
                const Vec3 nw = Normalize({sx, sy, z});
                row_buf[px - stage_x0] =
                    ShadeSurface(shade_ctx, sx, sy, z, nw.x, nw.y, nw.z, edge, eye_mask_for_x(px));
            }
        };

        if (!row_in_tuft) {
            if (sy2 >= 1.032f) {
                fill_bg_span(stage_x0, stage_x1);
            } else {
                const float max_sx_out = std::sqrt(1.032f - sy2);
                const int x_out0 = std::clamp(static_cast<int>(std::floor(cx - max_sx_out * r_px)),
                                              stage_x0, stage_x1);
                const int x_out1 = std::clamp(static_cast<int>(std::ceil(cx + max_sx_out * r_px)) + 1,
                                              x_out0, stage_x1);

                fill_bg_span(stage_x0, x_out0);
                if (sy2 < 0.968f) {
                    const float max_sx_in = std::sqrt(0.968f - sy2);
                    const int x_in0 = std::clamp(static_cast<int>(std::ceil(cx - max_sx_in * r_px)),
                                                 x_out0, x_out1);
                    const int x_in1 = std::clamp(static_cast<int>(std::floor(cx + max_sx_in * r_px)) + 1,
                                                 x_in0, x_out1);

                    fill_aa_span(x_out0, x_in0);
                    struct Span {
                        int x0;
                        int x1;
                        uint8_t mask;
                    };
                    Span spans[2];
                    int n_spans = 0;
                    if (e0_row) {
                        const int s0 = std::max(x_in0, eye_box[0].x0);
                        const int s1 = std::min(x_in1, eye_box[0].x1);
                        if (s1 > s0) {
                            spans[n_spans++] = {s0, s1, 1U};
                        }
                    }
                    if (e1_row) {
                        const int s0 = std::max(x_in0, eye_box[1].x0);
                        const int s1 = std::min(x_in1, eye_box[1].x1);
                        if (s1 > s0) {
                            spans[n_spans++] = {s0, s1, 2U};
                        }
                    }
                    if (n_spans == 2) {
                        if (spans[1].x0 < spans[0].x0) {
                            std::swap(spans[0], spans[1]);
                        }
                        if (spans[1].x0 < spans[0].x1) {
                            spans[0].x1 = std::max(spans[0].x1, spans[1].x1);
                            spans[0].mask = 3U;
                            n_spans = 1;
                        }
                    }
                    int cur = x_in0;
                    for (int i = 0; i < n_spans; ++i) {
                        fill_fast_body(cur, spans[i].x0);
                        fill_eye_span(spans[i].x0, spans[i].x1, spans[i].mask);
                        cur = spans[i].x1;
                    }
                    fill_fast_body(cur, x_in1);
                    fill_aa_span(x_in1, x_out1);
                } else {
                    fill_aa_span(x_out0, x_out1);
                }
                fill_bg_span(x_out1, stage_x1);
            }
        } else {
            // Tuft band rows: mix analytic sphere outside draw_bx0..draw_bx1 with 8x8 tile grid inside.
            for (int px = stage_x0; px < stage_x1; px += step) {
                const float sx = (static_cast<float>(px) + 0.5f - cx) * inv_r;
                const float r2 = sx * sx + sy2;
                const bool in_tuft = (px >= draw_bx0 && px < draw_bx1);
                uint16_t px_col = 0x0000;
                if (!in_tuft) {
                    if (r2 >= 1.032f) {
                        px_col = in_shadow_y ? shade_shadow(px, sh_dy) : 0x0000;
                    } else if (r2 < 0.968f) {
                        if (fast_rim_unity && eye_mask_for_x(px) == 0) {
                            px_col = SampleMatcap565Fast(sx, sy);
                        } else {
                            const float z = std::sqrt(1.0f - r2);
                            px_col = ShadeSurface(shade_ctx, sx, sy, z, sx, sy, z, 1.0f,
                                                  eye_mask_for_x(px));
                        }
                    } else {
                        const float r = std::sqrt(r2);
                        const float z = (r < 1.0f) ? std::sqrt(1.0f - r2) : 0.0f;
                        const float edge = Clamp01((1.0f - r) * inv_aa_step * 0.5f + 0.5f);
                        const Vec3 nw = Normalize({sx, sy, z});
                        px_col = ShadeSurface(shade_ctx, sx, sy, z, nw.x, nw.y, nw.z, edge,
                                              eye_mask_for_x(px));
                    }
                } else {
                    // Inside compact crown tuft 8x8 tile grid.
                    const int gx = (px - bx0) >> 3;
                    const TuftGridNode& c00 = tuft_grid_[gy * kMaxGridDim + gx];
                    const TuftGridNode& c10 = tuft_grid_[gy * kMaxGridDim + gx + 1];
                    const TuftGridNode& c01 = tuft_grid_[(gy + 1) * kMaxGridDim + gx];
                    const TuftGridNode& c11 = tuft_grid_[(gy + 1) * kMaxGridDim + gx + 1];

                    if (!c00.hit && !c10.hit && !c01.hit && !c11.hit &&
                        c00.min_d > 14.5f * inv_r && c10.min_d > 14.5f * inv_r &&
                        c01.min_d > 14.5f * inv_r && c11.min_d > 14.5f * inv_r) {
                        px_col = in_shadow_y ? shade_shadow(px, sh_dy) : 0x0000;
                    } else if (c00.hit && c10.hit && c01.hit && c11.hit &&
                               c00.nz > 0.24f && c10.nz > 0.24f && c01.nz > 0.24f &&
                               c11.nz > 0.24f) {
                        const float fx = static_cast<float>((px - bx0) & 7) * 0.125f;
                        const float w00 = (1.0f - fx) * (1.0f - fy);
                        const float w10 = fx * (1.0f - fy);
                        const float w01 = (1.0f - fx) * fy;
                        const float w11 = fx * fy;
                        const float z = w00 * c00.z + w10 * c10.z + w01 * c01.z + w11 * c11.z;
                        const Vec3 nw = Normalize({
                            w00 * c00.nx + w10 * c10.nx + w01 * c01.nx + w11 * c11.nx,
                            w00 * c00.ny + w10 * c10.ny + w01 * c01.ny + w11 * c11.ny,
                            w00 * c00.nz + w10 * c10.nz + w01 * c01.nz + w11 * c11.nz,
                        });
                        px_col = ShadeSurface(shade_ctx, sx, sy, z, nw.x, nw.y, nw.z, 1.0f,
                                              eye_mask_for_x(px));
                    } else {
                        Vec3 hw{};
                        Vec3 nw{};
                        float md = 10.0f;
                        const bool hit = TraceTuftPixel(rot, tuft, inv_r, sx, sy, &hw, &nw, &md);
                        if (hit || md < aa_step) {
                            const float edge =
                                hit ? (0.5f + 0.5f * Clamp01((0.5f * nw.z * nw.z) * inv_aa_step))
                                    : (0.5f * Clamp01(1.0f - md * inv_aa_step));
                            px_col = ShadeSurface(shade_ctx, hw.x, hw.y, hw.z, nw.x, nw.y, nw.z,
                                                  edge, eye_mask_for_x(px));
                        } else if (in_shadow_y) {
                            px_col = shade_shadow(px, sh_dy);
                        }
                    }
                }
                const int off = px - stage_x0;
                row_buf[off] = px_col;
                if (fast_half_res && px + 1 < stage_x1) {
                    row_buf[off + 1] = px_col;
                }
            }
        }

        std::memcpy(fb + static_cast<size_t>(py) * fb_w + stage_x0, row_buf, row_bytes);
        if (fast_half_res) {
            if (py + 1 < stage_y1) {
                std::memcpy(fb + static_cast<size_t>(py + 1) * fb_w + stage_x0, row_buf, row_bytes);
            }
        } else if (cached_orb_ != nullptr && stage_w <= kMaxStageW && stage_h <= kMaxStageH) {
            std::memcpy(cached_orb_ + static_cast<size_t>(py - stage_y0) * stage_w, row_buf,
                        row_bytes);
        }
    }

    last_rows_us_ = static_cast<uint32_t>(NowUs() - t_rows_start);
    last_blitted_icy_ = fast_half_res ? -9999 : icy;
    if (out_changed != nullptr) {
        *out_changed = true;
    }
    if (!fast_half_res && cached_orb_ != nullptr && stage_w <= kMaxStageW &&
        stage_h <= kMaxStageH) {
        cached_orb_w_ = stage_w;
        cached_orb_h_ = stage_h;
        cached_orb_x0_ = stage_x0;
        cached_orb_y0_ = stage_y0;
        cached_orb_icx_ = icx;
        cached_orb_icy_ = icy;
        cached_orb_radius_ = radius_px;
        cached_orb_pose_ = pose;
        orb_cache_valid_ = true;
    }

    return dirty;
}

} // namespace esp_pika
