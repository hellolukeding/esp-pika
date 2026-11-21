#include "fbank_extractor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef ESP_PLATFORM
#include "dsps_fft2r.h"
#endif

namespace esp_pika {
namespace {

constexpr float kPi = 3.14159265358979323846f;

float HzToMel(float hz) {
    return 2595.0f * std::log10(1.0f + hz / 700.0f);
}

float MelToHz(float mel) {
    return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f);
}

// In-place radix-2 Cooley-Tukey FFT on interleaved complex float array [re0, im0, re1, im1, ...].
void Radix2FftComplex(float* data, size_t n) {
    size_t j = 0;
    for (size_t i = 0; i < n; ++i) {
        if (i < j) {
            std::swap(data[2 * i], data[2 * j]);
            std::swap(data[2 * i + 1], data[2 * j + 1]);
        }
        size_t bit = n >> 1;
        while (bit > 0 && (j & bit) != 0) {
            j ^= bit;
            bit >>= 1;
        }
        j |= bit;
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float angle = -2.0f * kPi / static_cast<float>(len);
        const float wlen_re = std::cos(angle);
        const float wlen_im = std::sin(angle);
        for (size_t i = 0; i < n; i += len) {
            float w_re = 1.0f;
            float w_im = 0.0f;
            for (size_t k = 0; k < len / 2; ++k) {
                const size_t even = 2 * (i + k);
                const size_t odd = 2 * (i + k + len / 2);
                const float u_re = data[even];
                const float u_im = data[even + 1];
                const float v_re = data[odd] * w_re - data[odd + 1] * w_im;
                const float v_im = data[odd] * w_im + data[odd + 1] * w_re;
                data[even] = u_re + v_re;
                data[even + 1] = u_im + v_im;
                data[odd] = u_re - v_re;
                data[odd + 1] = u_im - v_im;
                const float next_w_re = w_re * wlen_re - w_im * wlen_im;
                w_im = w_re * wlen_im + w_im * wlen_re;
                w_re = next_w_re;
            }
        }
    }
}

} // namespace

bool FbankExtractor::Init(const FbankConfig& cfg) {
    if (cfg.sample_rate == 0 || cfg.fft_size == 0 || (cfg.fft_size & (cfg.fft_size - 1)) != 0 ||
        cfg.num_mel_bins == 0) {
        return false;
    }
    cfg_ = cfg;
    win_samples_ = (cfg_.sample_rate * cfg_.window_ms) / 1000;
    hop_samples_ = (cfg_.sample_rate * cfg_.hop_ms) / 1000;
    if (win_samples_ == 0 || hop_samples_ == 0 || win_samples_ > cfg_.fft_size) {
        return false;
    }

#ifdef ESP_PLATFORM
    if (dsps_fft2r_init_fc32(nullptr, static_cast<int>(cfg_.fft_size)) != ESP_OK) {
        return false;
    }
#endif

    // Periodic Hamming window.
    window_.resize(win_samples_);
    for (size_t i = 0; i < win_samples_; ++i) {
        window_[i] = 0.54f - 0.46f * std::cos(2.0f * kPi * static_cast<float>(i) /
                                              static_cast<float>(win_samples_ - 1));
    }

    // Triangular Mel filterbank weights.
    const float mel_min = HzToMel(cfg_.f_min_hz);
    const float mel_max = HzToMel(std::min(cfg_.f_max_hz, static_cast<float>(cfg_.sample_rate) * 0.5f));
    const size_t num_points = cfg_.num_mel_bins + 2;
    std::vector<size_t> fft_bins(num_points);
    for (size_t i = 0; i < num_points; ++i) {
        const float mel = mel_min + (mel_max - mel_min) * (static_cast<float>(i) / static_cast<float>(num_points - 1));
        const float hz = MelToHz(mel);
        const size_t bin = static_cast<size_t>(
            std::floor((static_cast<float>(cfg_.fft_size + 1) * hz) / static_cast<float>(cfg_.sample_rate)));
        fft_bins[i] = std::min(bin, cfg_.fft_size / 2);
    }

    filters_.resize(cfg_.num_mel_bins);
    for (size_t m = 0; m < cfg_.num_mel_bins; ++m) {
        const size_t left = fft_bins[m];
        const size_t center = std::max(fft_bins[m + 1], left + 1);
        const size_t right = std::max(fft_bins[m + 2], center + 1);
        const size_t capped_right = std::min(right, cfg_.fft_size / 2);
        filters_[m].start_bin = left;
        filters_[m].weights.assign(capped_right - left + 1, 0.0f);
        for (size_t k = left; k <= capped_right; ++k) {
            float w = 0.0f;
            if (k < center) {
                w = static_cast<float>(k - left) / static_cast<float>(center - left);
            } else if (k == center) {
                w = 1.0f;
            } else if (right > center) {
                w = static_cast<float>(right - k) / static_cast<float>(right - center);
            }
            filters_[m].weights[k - left] = std::max(w, 0.0f);
        }
    }

    initialized_ = true;
    return true;
}

bool FbankExtractor::Extract(const int16_t* pcm,
                             size_t samples,
                             std::vector<float>* out_flat,
                             size_t* out_frames) const {
    if (!initialized_ || pcm == nullptr || samples < win_samples_ || out_flat == nullptr ||
        out_frames == nullptr) {
        return false;
    }

    const size_t num_frames = 1 + (samples - win_samples_) / hop_samples_;
    const size_t bins = cfg_.num_mel_bins;
    const size_t spec_bins = cfg_.fft_size / 2 + 1;

    out_flat->assign(num_frames * bins, 0.0f);
    *out_frames = num_frames;

    std::vector<float> fft_buf(cfg_.fft_size * 2, 0.0f);
    std::vector<float> power(spec_bins, 0.0f);

    for (size_t f = 0; f < num_frames; ++f) {
        const size_t start = f * hop_samples_;
        std::fill(fft_buf.begin(), fft_buf.end(), 0.0f);
        for (size_t i = 0; i < win_samples_; ++i) {
            const float cur = static_cast<float>(pcm[start + i]) / 32768.0f;
            const float prev = (start + i > 0) ? (static_cast<float>(pcm[start + i - 1]) / 32768.0f) : 0.0f;
            fft_buf[2 * i] = (cur - cfg_.pre_emphasis * prev) * window_[i];
        }

#ifdef ESP_PLATFORM
        dsps_fft2r_fc32(fft_buf.data(), static_cast<int>(cfg_.fft_size));
        dsps_bit_rev_fc32(fft_buf.data(), static_cast<int>(cfg_.fft_size));
#else
        Radix2FftComplex(fft_buf.data(), cfg_.fft_size);
#endif

        for (size_t k = 0; k < spec_bins; ++k) {
            const float re = fft_buf[2 * k];
            const float im = fft_buf[2 * k + 1];
            power[k] = re * re + im * im;
        }

        float* dst_row = out_flat->data() + f * bins;
        for (size_t m = 0; m < bins; ++m) {
            const auto& flt = filters_[m];
            float energy = 1e-6f;
            for (size_t j = 0; j < flt.weights.size(); ++j) {
                const size_t k = flt.start_bin + j;
                if (k < spec_bins) {
                    energy += flt.weights[j] * power[k];
                }
            }
            dst_row[m] = std::log(energy);
        }
    }

    if (cfg_.apply_cmvn && num_frames > 1) {
        for (size_t m = 0; m < bins; ++m) {
            double sum = 0.0;
            for (size_t f = 0; f < num_frames; ++f) {
                sum += static_cast<double>((*out_flat)[f * bins + m]);
            }
            const float mean = static_cast<float>(sum / static_cast<double>(num_frames));
            for (size_t f = 0; f < num_frames; ++f) {
                (*out_flat)[f * bins + m] -= mean;
            }
        }
    }

    return true;
}

} // namespace esp_pika
