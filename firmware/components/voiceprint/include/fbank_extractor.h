#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esp_pika {

struct FbankConfig {
    uint32_t sample_rate = 16000;
    uint16_t window_ms = 25;  // 400 samples @ 16kHz
    uint16_t hop_ms = 10;     // 160 samples @ 16kHz
    size_t fft_size = 512;
    size_t num_mel_bins = 40;
    float f_min_hz = 60.0f;
    float f_max_hz = 7600.0f;
    float pre_emphasis = 0.97f;
    bool apply_cmvn = true;
};

/// Extracts 40-dimensional log-Mel filterbank features (FBank) from 16kHz mono PCM.
/// Uses espressif/esp-dsp FFT routines on ESP32-S31 and exact radix-2 FFT on host builds.
class FbankExtractor {
public:
    FbankExtractor() = default;
    bool Init(const FbankConfig& cfg = {});

    /// Computes [out_frames x num_mel_bins] row-major FBank features.
    bool Extract(const int16_t* pcm,
                 size_t samples,
                 std::vector<float>* out_flat,
                 size_t* out_frames) const;

    const FbankConfig& Config() const { return cfg_; }

private:
    struct MelFilter {
        size_t start_bin = 0;
        std::vector<float> weights;
    };

    FbankConfig cfg_;
    size_t win_samples_ = 0;
    size_t hop_samples_ = 0;
    std::vector<float> window_;
    std::vector<MelFilter> filters_;
    bool initialized_ = false;
};

} // namespace esp_pika
