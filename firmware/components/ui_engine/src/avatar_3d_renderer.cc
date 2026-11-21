#include "avatar_3d_renderer.h"

#include <algorithm>
#include <cmath>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace esp_pika {
namespace {

constexpr float kPi = 3.14159265f;
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
    const float len = Length(v);
    if (len < 1e-6f) {
        return {0.0f, 0.0f, 1.0f};
    }
    const float inv = 1.0f / len;
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

float SdTaperedCapsule(const Vec3& p, const Vec3& a, const Vec3& b, float ra, float rb) {
    const Vec3 pa = p - a;
    const Vec3 ba = b - a;
    const float baba = Dot(ba, ba);
    const float t = (baba > 1e-6f) ? Clamp01(Dot(pa, ba) / baba) : 0.0f;
    const Vec3 diff = pa - ba * t;
    return Length(diff) - (ra + (rb - ra) * t);
}

struct TuftControlPoints {
    Vec3 p0{0.05f, -0.78f, -0.02f};
    Vec3 p1{0.28f, -1.03f, -0.05f};
    Vec3 p2{0.48f, -1.23f, -0.08f};
};

float SceneSdf(const Vec3& q, const TuftControlPoints& tuft) {
    // Slightly prolate obsidian sphere with silky top-crown tuft blended smoothly.
    const float d_sphere = Length({q.x * 1.00f, q.y * 1.02f, q.z * 1.00f}) - 1.0f;
    const float d_seg1 = SdTaperedCapsule(q, tuft.p0, tuft.p1, 0.34f, 0.145f);
    const float d_seg2 = SdTaperedCapsule(q, tuft.p1, tuft.p2, 0.145f, 0.022f);
    const float d_tuft = std::min(d_seg1, d_seg2);
    return SmoothMin(d_sphere, d_tuft, 0.24f);
}

Vec3 SceneNormalObj(const Vec3& q, const TuftControlPoints& tuft) {
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

    // Order: M = R_roll(z) * R_yaw(y) * R_pitch(x)
    // Pitch(+p) lifts +Z toward -Y; Yaw(+y) rotates +Z toward +X; Roll(+r) rotates -Y toward +X.
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
    f.center = Normalize({x_dir, y_dir, 0.88f});
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

} // namespace

Avatar3DRenderer::~Avatar3DRenderer() {
#ifdef ESP_PLATFORM
    if (matcap_lut_ != nullptr) {
        heap_caps_free(matcap_lut_);
        matcap_lut_ = nullptr;
    }
    if (rainbow_lut_ != nullptr) {
        heap_caps_free(rainbow_lut_);
        rainbow_lut_ = nullptr;
    }
#endif
}

bool Avatar3DRenderer::Init() {
    if (ready_) {
        return true;
    }
#ifdef ESP_PLATFORM
    matcap_lut_ = static_cast<Rgb8*>(heap_caps_malloc(
        static_cast<size_t>(kMatcapSize) * kMatcapSize * sizeof(Rgb8),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    rainbow_lut_ = static_cast<Rgb8*>(
        heap_caps_malloc(static_cast<size_t>(kRainbowSize) * sizeof(Rgb8),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (matcap_lut_ == nullptr || rainbow_lut_ == nullptr) {
        return false;
    }
#else
    host_matcap_.resize(static_cast<size_t>(kMatcapSize) * kMatcapSize);
    host_rainbow_.resize(static_cast<size_t>(kRainbowSize));
    matcap_lut_ = host_matcap_.data();
    rainbow_lut_ = host_rainbow_.data();
#endif
    BuildMatcapLut();
    BuildRainbowLut();
    ready_ = true;
    return true;
}

void Avatar3DRenderer::BuildMatcapLut() {
    const Vec3 l_key = Normalize({-0.48f, -0.58f, 0.66f});
    const Vec3 l_rim = Normalize({0.64f, 0.52f, 0.40f});
    const Vec3 view{0.0f, 0.0f, 1.0f};
    const Vec3 h_key = Normalize(l_key + view);

    for (int iy = 0; iy < kMatcapSize; ++iy) {
        const float ny = (static_cast<float>(iy) + 0.5f) / kMatcapSize * 2.0f - 1.0f;
        for (int ix = 0; ix < kMatcapSize; ++ix) {
            const float nx = (static_cast<float>(ix) + 0.5f) / kMatcapSize * 2.0f - 1.0f;
            const float r2 = nx * nx + ny * ny;
            const float nz = (r2 < 1.0f) ? std::sqrt(1.0f - r2) : 0.0f;
            const Vec3 n = Normalize({nx, ny, std::max(nz, 0.04f)});

            // Deep satin obsidian-indigo base calibrated to reference figure.
            float r = 13.0f;
            float g = 11.0f;
            float b = 22.0f;

            // Soft top-left studio fill + silky specular sheen.
            const float diff_key = std::max(0.0f, Dot(n, l_key));
            const float wrap_key = Clamp01((Dot(n, l_key) + 0.45f) / 1.45f);
            r += wrap_key * 22.0f + diff_key * 20.0f;
            g += wrap_key * 20.0f + diff_key * 18.0f;
            b += wrap_key * 36.0f + diff_key * 32.0f;

            const float ndh = std::max(0.0f, Dot(n, h_key));
            const float spec_broad = std::pow(ndh, 5.5f);
            const float spec_crisp = std::pow(ndh, 24.0f);
            r += spec_broad * 68.0f + spec_crisp * 116.0f;
            g += spec_broad * 60.0f + spec_crisp * 108.0f;
            b += spec_broad * 96.0f + spec_crisp * 145.0f;

            // Cool violet lower-right rim reflection + Fresnel edge catchlight.
            const float diff_rim = std::max(0.0f, Dot(n, l_rim));
            r += diff_rim * 34.0f;
            g += diff_rim * 22.0f;
            b += diff_rim * 58.0f;

            const float fresnel = std::pow(1.0f - Clamp01(nz), 2.35f);
            const float upper_bias = 0.68f + 0.32f * Clamp01(0.5f - ny * 0.5f + nx * 0.25f);
            r += fresnel * upper_bias * 124.0f;
            g += fresnel * upper_bias * 98.0f;
            b += fresnel * upper_bias * 198.0f;

            matcap_lut_[iy * kMatcapSize + ix] = {ClampByte(r), ClampByte(g), ClampByte(b)};
        }
    }
}

void Avatar3DRenderer::BuildRainbowLut() {
    struct Stop {
        float pos;
        float r, g, b;
    };
    constexpr Stop kStops[] = {
        {0.00f, 128.0f, 76.0f, 255.0f},   // violet
        {0.16f, 44.0f, 178.0f, 255.0f},   // electric cyan-blue
        {0.34f, 42.0f, 248.0f, 206.0f},   // neon mint-cyan
        {0.52f, 204.0f, 248.0f, 76.0f},   // luminous chartreuse-gold
        {0.68f, 255.0f, 152.0f, 48.0f},   // warm amber-orange
        {0.84f, 255.0f, 68.0f, 168.0f},   // vivid magenta
        {1.00f, 128.0f, 76.0f, 255.0f},   // wrap back to violet
    };
    constexpr size_t kStopCount = sizeof(kStops) / sizeof(kStops[0]);

    for (int i = 0; i < kRainbowSize; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kRainbowSize);
        size_t s = 0;
        while (s + 2 < kStopCount && t > kStops[s + 1].pos) {
            ++s;
        }
        const Stop& a = kStops[s];
        const Stop& b = kStops[s + 1];
        const float local = Clamp01((t - a.pos) / (b.pos - a.pos));
        // Cosine smooth interpolation between prism stops.
        const float u = 0.5f - 0.5f * std::cos(local * kPi);
        rainbow_lut_[i] = {
            ClampByte(a.r + (b.r - a.r) * u),
            ClampByte(a.g + (b.g - a.g) * u),
            ClampByte(a.b + (b.b - a.b) * u),
        };
    }
}

Avatar3DRenderer::Rgb8 Avatar3DRenderer::SampleMatcap(float nx, float ny, float rim_boost) const {
    const float u = Clamp01(0.5f + 0.5f * std::clamp(nx, -0.99f, 0.99f));
    const float v = Clamp01(0.5f + 0.5f * std::clamp(ny, -0.99f, 0.99f));
    const int ix = std::min(kMatcapSize - 1, static_cast<int>(u * (kMatcapSize - 1)));
    const int iy = std::min(kMatcapSize - 1, static_cast<int>(v * (kMatcapSize - 1)));
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

Avatar3DRenderer::Rgb8 Avatar3DRenderer::SampleRainbow(float t, float warm_shift) const {
    float wrapped = t - std::floor(t);
    if (wrapped < 0.0f) {
        wrapped += 1.0f;
    }
    const int idx = static_cast<int>(wrapped * (kRainbowSize - 1)) & (kRainbowSize - 1);
    const Rgb8 c = rainbow_lut_[idx];
    if (warm_shift <= 0.01f) {
        return c;
    }
    const float r = c.r + warm_shift * 34.0f;
    const float g = c.g + warm_shift * 14.0f;
    const float b = c.b * (1.0f - 0.10f * warm_shift) + warm_shift * 18.0f;
    return {ClampByte(r), ClampByte(g), ClampByte(b)};
}

UiRect Avatar3DRenderer::Render(uint16_t* fb,
                                int fb_w,
                                int fb_h,
                                const AvatarPose& pose,
                                int center_x,
                                int center_y,
                                float radius_px) const {
    // Fixed even-aligned stage bounds covering the full floating excursion + tuft + shadow.
    const int stage_x0 = std::max(0, (center_x - 164) & ~1);
    const int stage_y0 = std::max(0, (center_y - 180) & ~1);
    const int stage_x1 = std::min(fb_w, ((center_x + 164) + 1) & ~1);
    const int stage_y1 = std::min(fb_h, ((center_y + 160) + 1) & ~1);
    const UiRect dirty{stage_x0, stage_y0, stage_x1 - stage_x0, stage_y1 - stage_y0};

    if (!ready_ || fb == nullptr) {
        return dirty;
    }

    const float r_px = std::max(24.0f, radius_px * pose.scale);
    const float inv_r = 1.0f / r_px;
    const float cy = static_cast<float>(center_y) + pose.bob_y;
    const float cx = static_cast<float>(center_x);

    const Mat3 rot = MakeRotationMatrix(pose.yaw_deg, pose.pitch_deg, pose.roll_deg);

    TuftControlPoints tuft;
    tuft.p1.x += pose.tuft_lag_x * 0.45f;
    tuft.p1.y += pose.tuft_lag_y * 0.45f;
    tuft.p2.x += pose.tuft_lag_x;
    tuft.p2.y += pose.tuft_lag_y;

    // Project tuft bounding region to screen space so pure-sphere pixels skip raymarching.
    const Vec3 t0_w = rot.Mul(tuft.p0);
    const Vec3 t1_w = rot.Mul(tuft.p1);
    const Vec3 t2_w = rot.Mul(tuft.p2);
    const float pad = 0.62f; // r_max(0.34) + smin_k(0.24) + safety margin
    const float tuft_min_x = std::min({t0_w.x, t1_w.x, t2_w.x}) - pad;
    const float tuft_max_x = std::max({t0_w.x, t1_w.x, t2_w.x}) + pad;
    const float tuft_min_y = std::min({t0_w.y, t1_w.y, t2_w.y}) - pad;
    const float tuft_max_y = std::max({t0_w.y, t1_w.y, t2_w.y}) + pad;

    const EyeFrame left_eye = MakeEyeFrame(-0.365f, 0.045f, -0.26f);
    const EyeFrame right_eye = MakeEyeFrame(+0.365f, 0.045f, +0.26f);
    const Vec3 l_glass = Normalize({-0.55f, -0.60f, 0.58f});

    // Contact shadow center directly below orb at fixed pedestal height.
    const float shadow_cy = static_cast<float>(center_y) + radius_px + 24.0f;
    const float shadow_rx = radius_px * (0.92f - pose.bob_y * 0.008f);
    const float shadow_ry = 14.0f;
    const float inv_shadow_rx2 = 1.0f / (shadow_rx * shadow_rx);
    const float inv_shadow_ry2 = 1.0f / (shadow_ry * shadow_ry);

    const float aa_step = 1.6f * inv_r;

    for (int py = stage_y0; py < stage_y1; ++py) {
        const float sy = (static_cast<float>(py) + 0.5f - cy) * inv_r;
        const float sh_dy = static_cast<float>(py) + 0.5f - shadow_cy;
        const bool in_shadow_y = (std::abs(sh_dy) < shadow_ry * 1.4f);
        uint16_t* row = fb + static_cast<size_t>(py) * fb_w;

        for (int px = stage_x0; px < stage_x1; ++px) {
            const float sx = (static_cast<float>(px) + 0.5f - cx) * inv_r;
            const float r2 = sx * sx + sy * sy;

            const bool in_tuft_zone =
                (sx >= tuft_min_x && sx <= tuft_max_x && sy >= tuft_min_y && sy <= tuft_max_y);

            // Early reject pixels clearly outside both sphere and tuft.
            if (r2 > 1.08f && !in_tuft_zone) {
                if (in_shadow_y) {
                    const float sh_dx = static_cast<float>(px) + 0.5f - cx;
                    const float sd2 =
                        sh_dx * sh_dx * inv_shadow_rx2 + sh_dy * sh_dy * inv_shadow_ry2;
                    if (sd2 < 1.35f) {
                        const float g = std::exp(-2.8f * sd2) * (0.28f + 0.45f * pose.shadow_tint);
                        const float sr = (18.0f + 22.0f * pose.shadow_tint) * g;
                        const float sg = (16.0f + 56.0f * pose.shadow_tint) * g;
                        const float sb = (38.0f + 98.0f * pose.shadow_tint) * g;
                        row[px] = MakeRgb565(ClampByte(sr), ClampByte(sg), ClampByte(sb));
                        continue;
                    }
                }
                row[px] = 0x0000;
                continue;
            }

            float edge_alpha = 0.0f;
            Vec3 hit_world{};
            Vec3 n_world{};

            if (!in_tuft_zone && r2 < 0.90f) {
                // Fast O(1) analytic prolate sphere hit for clean core interior.
                const float y_scale = 1.0f / 1.02f;
                const float sy_e = sy * 1.02f;
                const float r2_e = sx * sx + sy_e * sy_e;
                if (r2_e >= 1.0f) {
                    row[px] = 0x0000;
                    continue;
                }
                const float z = std::sqrt(1.0f - r2_e);
                hit_world = {sx, sy, z};
                n_world = Normalize({sx, sy * 1.04f * y_scale, z});
                edge_alpha = 1.0f;
            } else {
                // Tight orthographic sphere-trace for tuft blend & anti-aliased silhouette rim.
                float z = 1.25f;
                float min_d = 10.0f;
                bool hit = false;
                for (int step = 0; step < 11; ++step) {
                    const Vec3 p_w{sx, sy, z};
                    const Vec3 p_o = rot.MulTranspose(p_w);
                    const float d = SceneSdf(p_o, tuft);
                    if (d < min_d) {
                        min_d = d;
                    }
                    if (d < 0.004f) {
                        hit = true;
                        hit_world = p_w;
                        n_world = rot.Mul(SceneNormalObj(p_o, tuft));
                        break;
                    }
                    z -= std::max(d, 0.006f);
                    if (z < -0.55f) {
                        break;
                    }
                }
                if (!hit && min_d >= aa_step) {
                    if (in_shadow_y) {
                        const float sh_dx = static_cast<float>(px) + 0.5f - cx;
                        const float sd2 =
                            sh_dx * sh_dx * inv_shadow_rx2 + sh_dy * sh_dy * inv_shadow_ry2;
                        if (sd2 < 1.35f) {
                            const float g =
                                std::exp(-2.8f * sd2) * (0.28f + 0.45f * pose.shadow_tint);
                            row[px] = MakeRgb565(
                                ClampByte((18.0f + 22.0f * pose.shadow_tint) * g),
                                ClampByte((16.0f + 56.0f * pose.shadow_tint) * g),
                                ClampByte((38.0f + 98.0f * pose.shadow_tint) * g));
                            continue;
                        }
                    }
                    row[px] = 0x0000;
                    continue;
                }
                if (!hit) {
                    // Grazing anti-alias fringe pixel outside silhouette (continuous 0.5 -> 0.0).
                    const Vec3 p_o = rot.MulTranspose({sx, sy, z});
                    hit_world = {sx, sy, z};
                    n_world = rot.Mul(SceneNormalObj(p_o, tuft));
                    edge_alpha = 0.5f * Clamp01(1.0f - min_d / aa_step);
                } else {
                    // Smooth silhouette softening near horizon (continuous 0.5 -> 1.0).
                    edge_alpha = 0.5f + 0.5f * SmoothStep(0.0f, 0.12f, n_world.z);
                }
            }

            // Map hit into rotated object coordinates for 3D eye sockets & visor glass.
            const Vec3 p_obj = rot.MulTranspose(hit_world);

            float out_r = 0.0f;
            float out_g = 0.0f;
            float out_b = 0.0f;

            bool shaded_by_eye = false;
            if (p_obj.z > 0.18f && p_obj.y > -0.45f && p_obj.y < 0.58f) {
                for (int e_idx = 0; e_idx < 2; ++e_idx) {
                    const EyeFrame& ef = (e_idx == 0) ? left_eye : right_eye;
                    if (Dot(p_obj, ef.center) <= 0.25f) {
                        continue;
                    }
                    const float dx = Dot(p_obj, ef.ex);
                    const float dy = Dot(p_obj, ef.ey);
                    const float e_scale =
                        (e_idx == 0) ? pose.left_eye_scale : pose.right_eye_scale;
                    const float rx = 0.315f * e_scale;
                    const float ry = 0.255f * e_scale * pose.eye_aspect_y;

                    const float u = dx / rx;
                    const float v = dy / ry;

                    // Subtle organic almond eye contour (slightly wider near outer upper brow).
                    const float pinch = 1.0f + 0.14f * u * ef.side_sign;
                    const float r_eye = std::sqrt(u * u + (v * pinch) * (v * pinch));

                    if (r_eye < 1.24f) {
                        // Sculpted 3D concave socket wall transition (1.0 <= r_eye < 1.24).
                        if (r_eye >= 1.0f) {
                            const float t_bevel = (r_eye - 1.0f) / 0.24f;
                            const float sink = std::sin(t_bevel * kPi) * 0.52f;
                            const float inv_re = 1.0f / std::max(r_eye, 0.01f);
                            const Vec3 dir_o =
                                (ef.ex * (u * inv_re) + ef.ey * (v * inv_re)) * (-sink);
                            const Vec3 bent_n = Normalize(n_world + rot.Mul(dir_o));
                            const Rgb8 mat = SampleMatcap(bent_n.x, bent_n.y, pose.rim_boost);
                            // Soft cavity darkening closest to inner glass wall.
                            const float cavity_ao = 0.55f + 0.45f * SmoothStep(1.0f, 1.16f, r_eye);
                            out_r = mat.r * cavity_ao;
                            out_g = mat.g * cavity_ao;
                            out_b = mat.b * cavity_ao;
                            shaded_by_eye = true;
                            break;
                        }

                        // Inside socket (r_eye < 1.0): compute obsidian eyelid closure vs glass lens.
                        const float v_top = -1.04f + 2.08f * pose.upper_lid + 0.14f * u * u;
                        const float v_bot = +1.04f - 2.08f * pose.lower_lid -
                                            pose.happy_arch * (1.12f - 0.66f * u * u);

                        const float gap = v_bot - v_top;
                        const float d_top = v - v_top;
                        const float d_bot = v_bot - v;
                        const float open_d = std::min(d_top, d_bot);
                        const float open_alpha =
                            (gap <= 0.02f) ? 0.0f : SmoothStep(-0.04f, 0.05f, open_d);

                        // Obsidian lid color (shaded via 3D MatCap with realistic lid lip bevel).
                        float lid_ao = 0.52f + 0.48f * SmoothStep(1.0f, 0.45f, r_eye);
                        float lid_curl = 0.0f;
                        if (v < v_top) {
                            lid_curl = 0.35f * SmoothStep(0.35f, 0.0f, v_top - v);
                        } else {
                            lid_curl = -0.45f * SmoothStep(0.35f, 0.0f, v - v_bot);
                        }
                        if (gap < 0.14f && std::abs(v - 0.5f * (v_top + v_bot)) < 0.08f) {
                            // Recessed lid seam when closed (Blink / Sleep).
                            lid_ao *= 0.42f;
                        }
                        const Vec3 lid_n = Normalize(n_world + rot.Mul(ef.ey * lid_curl));
                        const Rgb8 lid_col = SampleMatcap(lid_n.x, lid_n.y, pose.rim_boost);
                        const float lr = lid_col.r * lid_ao;
                        const float lg = lid_col.g * lid_ao;
                        const float lb = lid_col.b * lid_ao;

                        if (open_alpha <= 0.005f) {
                            out_r = lr;
                            out_g = lg;
                            out_b = lb;
                            shaded_by_eye = true;
                            break;
                        }

                        // Strictly PUPIL-LESS Iridescent Holographic Lens Shading!
                        const float phase =
                            0.52f * (u * ef.side_sign + pose.focus_shift_x) +
                            0.68f * (v + pose.focus_shift_y) +
                            0.24f * pose.swirl_amount *
                                std::sin(4.2f * r_eye - pose.rainbow_phase * 2.0f * kPi) +
                            pose.rainbow_phase;

                        const Rgb8 prism = SampleRainbow(phase, pose.warm_shift);

                        // Convex glass normal inside the recessed socket + glossy top-left specular.
                        const Vec3 glass_n = Normalize(
                            n_world + rot.Mul((ef.ex * u + ef.ey * v) * 0.45f));
                        const float ndl = std::max(0.0f, Dot(glass_n, l_glass));
                        const float glass_spec = std::pow(ndl, 18.0f);
                        const float edge_sheen = std::pow(Clamp01(r_eye), 3.0f) * 0.25f;

                        // Optical prism focus glow (Curious/Look) — bright caustic, NEVER black dot.
                        const float fx = u + pose.focus_shift_x * 0.45f;
                        const float fy = v + pose.focus_shift_y * 0.45f;
                        const float focus_mag =
                            std::sqrt(pose.focus_shift_x * pose.focus_shift_x +
                                      pose.focus_shift_y * pose.focus_shift_y);
                        const float caustic =
                            focus_mag * 55.0f * std::exp(-3.6f * (fx * fx + fy * fy));

                        // Core radiant bloom for Surprised — luminous cyan-white core, NEVER black.
                        const float bloom = pose.core_bloom * std::exp(-2.4f * r_eye * r_eye);

                        // Socket rim Ambient Occlusion (keeps center luminous, tucks perimeter cleanly).
                        const float lens_ao = 0.48f + 0.52f * SmoothStep(1.02f, 0.52f, r_eye);

                        float gr =
                            prism.r * lens_ao * pose.eye_brightness + glass_spec * 215.0f +
                            caustic * 0.8f + bloom * 180.0f + edge_sheen * 90.0f;
                        float gg =
                            prism.g * lens_ao * pose.eye_brightness + glass_spec * 225.0f +
                            caustic * 1.0f + bloom * 225.0f + edge_sheen * 110.0f;
                        float gb =
                            prism.b * lens_ao * pose.eye_brightness + glass_spec * 245.0f +
                            caustic * 1.15f + bloom * 245.0f + edge_sheen * 155.0f;

                        // Smooth rim blend into obsidian socket wall at r_eye -> 1.0.
                        const float rim_mask = SmoothStep(1.0f, 0.90f, r_eye) * open_alpha;
                        const float inv_mask = 1.0f - rim_mask;
                        out_r = lr * inv_mask + gr * rim_mask;
                        out_g = lg * inv_mask + gg * rim_mask;
                        out_b = lb * inv_mask + gb * rim_mask;
                        shaded_by_eye = true;
                        break;
                    }
                }
            }

            if (!shaded_by_eye) {
                const Rgb8 body = SampleMatcap(n_world.x, n_world.y, pose.rim_boost);
                out_r = body.r;
                out_g = body.g;
                out_b = body.b;
            }

            if (edge_alpha < 0.999f) {
                out_r *= edge_alpha;
                out_g *= edge_alpha;
                out_b *= edge_alpha;
            }

            row[px] = MakeRgb565(ClampByte(out_r), ClampByte(out_g), ClampByte(out_b));
        }
    }

    return dirty;
}

} // namespace esp_pika
