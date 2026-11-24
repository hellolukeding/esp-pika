#include "afe_pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_wn_iface.h"
#include "model_path.h"
#endif

namespace esp_pika {
namespace {

#ifdef ESP_PLATFORM
constexpr const char* kTag = "afe_pipeline";
#endif

float ComputeRms(const int16_t* pcm, size_t samples) {
    if (pcm == nullptr || samples == 0) {
        return 0.0f;
    }
    double sum_sq = 0.0;
    for (size_t i = 0; i < samples; ++i) {
        double v = static_cast<double>(pcm[i]);
        sum_sq += v * v;
    }
    return static_cast<float>(std::sqrt(sum_sq / static_cast<double>(samples)));
}

} // namespace

PcmPreRollBuffer::PcmPreRollBuffer(size_t capacity_samples)
    : capacity_(capacity_samples > 0 ? capacity_samples : 1) {
#ifdef ESP_PLATFORM
    storage_ = static_cast<int16_t*>(
        heap_caps_malloc(capacity_ * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (storage_ != nullptr) {
        std::memset(storage_, 0, capacity_ * sizeof(int16_t));
        owns_spiram_ = true;
        return;
    }
#endif
    host_storage_.assign(capacity_, 0);
    storage_ = host_storage_.data();
}

PcmPreRollBuffer::~PcmPreRollBuffer() {
#ifdef ESP_PLATFORM
    if (owns_spiram_ && storage_ != nullptr) {
        heap_caps_free(storage_);
        storage_ = nullptr;
    }
#endif
}

void PcmPreRollBuffer::PushSamples(const int16_t* pcm, size_t count) {
    if (pcm == nullptr || count == 0 || storage_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    for (size_t i = 0; i < count; ++i) {
        storage_[write_pos_] = pcm[i];
        write_pos_ = (write_pos_ + 1) % capacity_;
        if (filled_ < capacity_) {
            ++filled_;
        }
    }
}

void PcmPreRollBuffer::SnapshotRecent(size_t max_samples, std::vector<int16_t>* out) const {
    if (out == nullptr || storage_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    size_t n = std::min(max_samples, filled_);
    out->resize(n);
    if (n == 0) {
        return;
    }
    size_t start = (write_pos_ + capacity_ - n) % capacity_;
    for (size_t i = 0; i < n; ++i) {
        (*out)[i] = storage_[(start + i) % capacity_];
    }
}

void PcmPreRollBuffer::Clear() {
    std::lock_guard<std::mutex> lock(mu_);
    write_pos_ = 0;
    filled_ = 0;
}

size_t PcmPreRollBuffer::Size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return filled_;
}

AfePipeline::~AfePipeline() {
#ifdef ESP_PLATFORM
    if (mn_handle_ != nullptr && mn_iface_ != nullptr) {
        esp_mn_commands_free();
        auto* mn_iface = static_cast<const esp_mn_iface_t*>(mn_iface_);
        mn_iface->destroy(static_cast<model_iface_data_t*>(mn_handle_));
        mn_handle_ = nullptr;
        mn_iface_ = nullptr;
    }
    if (afe_handle_ != nullptr && afe_iface_ != nullptr) {
        auto* iface = static_cast<const esp_afe_sr_iface_t*>(afe_iface_);
        iface->destroy(static_cast<esp_afe_sr_data_t*>(afe_handle_));
        afe_handle_ = nullptr;
        afe_iface_ = nullptr;
    }
#endif
}

bool AfePipeline::Init(const AfePipelineConfig& cfg) {
    cfg_ = cfg;
    const size_t max_ms = std::max(cfg_.pre_roll_ms, cfg_.wake_vp_window_ms);
    const size_t cap_samples = (cfg_.sample_rate * max_ms) / 1000;
    pre_roll_ = std::make_unique<PcmPreRollBuffer>(cap_samples);
    speaking_ = false;
    hangover_elapsed_ms_ = 0;
    noise_floor_rms_ = 80.0f;

#ifdef ESP_PLATFORM
    srmodel_list_t* models = esp_srmodel_init("model");
    if (cfg_.enable_wakenet && !cfg_.wake_pinyin.empty() && models != nullptr) {
        char* mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
        if (mn_name != nullptr) {
            esp_mn_iface_t* mn_iface = esp_mn_handle_from_name(mn_name);
            if (mn_iface != nullptr) {
                model_iface_data_t* mn_data = mn_iface->create(mn_name, 5500);
                if (mn_data != nullptr) {
                    mn_iface->set_det_threshold(mn_data, 0.25f);
                    esp_mn_commands_alloc(mn_iface, mn_data);
                    esp_mn_commands_clear();
                    esp_mn_commands_add(1, cfg_.wake_pinyin.c_str());
                    esp_mn_error_t* mn_err = esp_mn_commands_update();
                    if (mn_err == nullptr) {
                        mn_iface_ = mn_iface;
                        mn_handle_ = mn_data;
                        int mn_chunk = mn_iface->get_samp_chunksize(mn_data);
                        mn_chunksize_ = (mn_chunk > 0) ? static_cast<size_t>(mn_chunk) : 512;
                        mn_hangover_chunks_ = 0;
                        mn_carry_.clear();
                        ESP_LOGI(kTag,
                                 "ESP-SR MultiNet wake initialized (model=%s pinyin='%s' chunk=%u)",
                                 mn_name, cfg_.wake_pinyin.c_str(),
                                 static_cast<unsigned>(mn_chunksize_));
                    } else {
                        ESP_LOGW(kTag, "MultiNet command update failed (%d errors); falling back",
                                 static_cast<int>(mn_err->num));
                        esp_mn_commands_free();
                        mn_iface->destroy(mn_data);
                    }
                }
            }
        }
    }

    const char* input_fmt = cfg_.enable_aec ? "MR" : "M";
    afe_config_t* afe_cfg = afe_config_init(input_fmt, models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (afe_cfg == nullptr) {
        ESP_LOGW(kTag, "esp-sr afe_config_init unavailable; falling back to energy VAD");
        initialized_ = true;
        return true;
    }
    afe_cfg->aec_init = cfg_.enable_aec;
    afe_cfg->ns_init = cfg_.enable_ns;
    afe_cfg->vad_init = cfg_.enable_vadnet;
    afe_cfg->vad_mode = VAD_MODE_3;
    afe_cfg->vad_min_speech_ms = 120;
    afe_cfg->vad_min_noise_ms = 320;
    afe_cfg->vad_energy_threshold = -42.0f;
    afe_cfg->agc_init = true;
    afe_cfg->agc_mode = AFE_AGC_MODE_WEBRTC;
    afe_cfg->agc_compression_gain_db = 6;
    afe_cfg->agc_target_level_dbfs = 6;
    afe_cfg->afe_linear_gain = 1.0f;
    afe_cfg->wakenet_init = (mn_handle_ == nullptr) ? cfg_.enable_wakenet : false;
    afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe_cfg->afe_perferred_core = 0;

    const esp_afe_sr_iface_t* iface = esp_afe_handle_from_config(afe_cfg);
    if (iface != nullptr) {
        ESP_LOGI(kTag, "creating ESP-SR AFE (free_int=%u free_psram=%u)",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        esp_afe_sr_data_t* handle = iface->create_from_config(afe_cfg);
        if (handle != nullptr) {
            afe_iface_ = iface;
            afe_handle_ = handle;
            int chunk = (iface->get_feed_chunksize != nullptr) ? iface->get_feed_chunksize(handle) : 0;
            feed_chunksize_ = (chunk > 0) ? static_cast<size_t>(chunk) : 512;
            feed_carry_.clear();
            ESP_LOGI(kTag, "ESP-SR AFE initialized (aec=%d vad=%d wake=%d feed_chunksize=%u)",
                     cfg_.enable_aec, cfg_.enable_vadnet, cfg_.enable_wakenet,
                     static_cast<unsigned>(feed_chunksize_));
        }
    }
    afe_config_free(afe_cfg);
#endif
    initialized_ = true;
    return true;
}

float AfePipeline::UpdateNoiseAndComputeSnr(const int16_t* pcm, size_t samples, float* out_rms) {
    float rms = ComputeRms(pcm, samples);
    if (out_rms != nullptr) {
        *out_rms = rms;
    }
    if (rms < cfg_.vad_energy_threshold) {
        noise_floor_rms_ = 0.95f * noise_floor_rms_ + 0.05f * std::max(rms, 20.0f);
    }
    float ratio = std::max(rms, 1.0f) / std::max(noise_floor_rms_, 10.0f);
    last_snr_db_ = 20.0f * std::log10(ratio);
    return last_snr_db_;
}

bool AfePipeline::ProcessFrame(const int16_t* mic_pcm,
                               const int16_t* ref_pcm,
                               size_t samples,
                               AfeStepResult* out) {
    if (!initialized_ || mic_pcm == nullptr || samples == 0 || out == nullptr) {
        return false;
    }
    *out = AfeStepResult{};
    pre_roll_->PushSamples(mic_pcm, samples);

    float rms = 0.0f;
    out->snr_db = UpdateNoiseAndComputeSnr(mic_pcm, samples, &rms);
    const uint32_t frame_ms = static_cast<uint32_t>((samples * 1000U) / std::max<uint32_t>(cfg_.sample_rate, 1));

#ifdef ESP_PLATFORM
    if (afe_handle_ != nullptr && afe_iface_ != nullptr) {
        auto* iface = static_cast<const esp_afe_sr_iface_t*>(afe_iface_);
        auto* handle = static_cast<esp_afe_sr_data_t*>(afe_handle_);

        // Accumulate samples and feed ESP-SR AFE in exact get_feed_chunksize() blocks.
        const size_t ch = cfg_.enable_aec ? 2 : 1;
        const size_t chunk_elems = (feed_chunksize_ > 0 ? feed_chunksize_ : 512) * ch;
        if (cfg_.enable_aec) {
            for (size_t i = 0; i < samples; ++i) {
                feed_carry_.push_back(mic_pcm[i]);
                feed_carry_.push_back((ref_pcm != nullptr) ? ref_pcm[i] : 0);
            }
        } else {
            feed_carry_.insert(feed_carry_.end(), mic_pcm, mic_pcm + samples);
        }

        while (feed_carry_.size() >= chunk_elems) {
            iface->feed(handle, feed_carry_.data());
            feed_carry_.erase(feed_carry_.begin(),
                              feed_carry_.begin() + static_cast<std::ptrdiff_t>(chunk_elems));

            afe_fetch_result_t* res = iface->fetch(handle);
            if (res == nullptr || res->ret_value == ESP_FAIL) {
                continue;
            }
            if (res->data != nullptr && res->data_size > 0) {
                const size_t clean_samples = static_cast<size_t>(res->data_size) / sizeof(int16_t);
                out->clean_pcm.insert(out->clean_pcm.end(), res->data, res->data + clean_samples);
            }
            const bool was_speaking = speaking_;
            const bool now_speaking = (res->vad_state == VAD_SPEECH);
            if (!speaking_ && now_speaking) {
                out->vad_started = true;
                const size_t pre_samples = (cfg_.sample_rate * cfg_.pre_roll_ms) / 1000;
                pre_roll_->SnapshotRecent(pre_samples, &out->pre_roll_pcm);
            } else if (speaking_ && !now_speaking) {
                out->vad_ended = true;
            }
            speaking_ = now_speaking;

            if (mn_handle_ != nullptr && mn_iface_ != nullptr) {
                auto* mn_iface = static_cast<const esp_mn_iface_t*>(mn_iface_);
                auto* mn_data = static_cast<model_iface_data_t*>(mn_handle_);
                if (!wake_enabled_.load(std::memory_order_relaxed)) {
                    if (mn_hangover_chunks_ > 0 || !mn_carry_.empty()) {
                        mn_iface->clean(mn_data);
                        mn_carry_.clear();
                        mn_hangover_chunks_ = 0;
                    }
                } else {
                    if (now_speaking) {
                        if (!was_speaking && mn_hangover_chunks_ == 0 && mn_chunksize_ > 0) {
                            std::vector<int16_t> lead;
                            pre_roll_->SnapshotRecent(mn_chunksize_, &lead);
                            mn_carry_.insert(mn_carry_.end(), lead.begin(), lead.end());
                        }
                        mn_hangover_chunks_ = 25;
                    } else if (mn_hangover_chunks_ > 0) {
                        if (--mn_hangover_chunks_ == 0) {
                            mn_iface->clean(mn_data);
                            mn_carry_.clear();
                        }
                    }

                    if ((now_speaking || mn_hangover_chunks_ > 0) && res->data != nullptr &&
                        res->data_size > 0 && mn_chunksize_ > 0) {
                        const size_t n = static_cast<size_t>(res->data_size) / sizeof(int16_t);
                        mn_carry_.insert(mn_carry_.end(), res->data, res->data + n);
                        while (mn_carry_.size() >= mn_chunksize_) {
                            esp_mn_state_t st = mn_iface->detect(mn_data, mn_carry_.data());
                            mn_carry_.erase(mn_carry_.begin(),
                                            mn_carry_.begin() + static_cast<std::ptrdiff_t>(mn_chunksize_));
                            if (st == ESP_MN_STATE_DETECTED) {
                                esp_mn_results_t* mr = mn_iface->get_results(mn_data);
                                float prob = (mr != nullptr && mr->num > 0) ? mr->prob[0] : 0.90f;
                                out->wake_detected = true;
                                out->wake_word = cfg_.wake_word;
                                out->wake_score = prob;
                                const size_t vp_samples =
                                    (cfg_.sample_rate * cfg_.wake_vp_window_ms) / 1000;
                                pre_roll_->SnapshotRecent(vp_samples, &out->wake_pcm);
                                ESP_LOGI(kTag, "MultiNet wake detected: word=%s prob=%.2f",
                                         cfg_.wake_word.c_str(), static_cast<double>(prob));
                                mn_iface->clean(mn_data);
                                mn_carry_.clear();
                                mn_hangover_chunks_ = 0;
                                break;
                            } else if (st == ESP_MN_STATE_TIMEOUT) {
                                mn_iface->clean(mn_data);
                                mn_carry_.clear();
                            }
                        }
                    }
                }
            }

            if (wake_enabled_.load(std::memory_order_relaxed) && res->wakeup_state == WAKENET_DETECTED) {
                out->wake_detected = true;
                out->wake_word = cfg_.wake_word;
                out->wake_score = 0.92f;
                const size_t vp_samples = (cfg_.sample_rate * cfg_.wake_vp_window_ms) / 1000;
                pre_roll_->SnapshotRecent(vp_samples, &out->wake_pcm);
                ESP_LOGI(kTag, "WakeNet detected: %s", cfg_.wake_word.c_str());
            }
        }
        out->vad_speaking = speaking_;
        return true;
    }
#else
    (void)ref_pcm;
#endif

    // Deterministic host / fallback acoustic VAD + pre-roll path.
    out->clean_pcm.assign(mic_pcm, mic_pcm + samples);
    const bool above = (rms >= cfg_.vad_energy_threshold);
    if (above) {
        hangover_elapsed_ms_ = 0;
        if (!speaking_) {
            speaking_ = true;
            out->vad_started = true;
            const size_t pre_samples = (cfg_.sample_rate * cfg_.pre_roll_ms) / 1000;
            pre_roll_->SnapshotRecent(pre_samples, &out->pre_roll_pcm);
        }
    } else if (speaking_) {
        hangover_elapsed_ms_ += frame_ms;
        if (hangover_elapsed_ms_ >= cfg_.vad_hangover_ms) {
            speaking_ = false;
            out->vad_ended = true;
        }
    }
    out->vad_speaking = speaking_;
    return true;
}

bool AfePipeline::TriggerManualWake(float wake_score, float snr_db, AfeStepResult* out) {
    if (!initialized_ || out == nullptr) {
        return false;
    }
    *out = AfeStepResult{};
    out->wake_detected = true;
    out->wake_word = cfg_.wake_word;
    out->wake_score = wake_score;
    out->snr_db = (snr_db != 0.0f) ? snr_db : last_snr_db_;
    const size_t vp_samples = (cfg_.sample_rate * cfg_.wake_vp_window_ms) / 1000;
    pre_roll_->SnapshotRecent(vp_samples, &out->wake_pcm);
    return true;
}

} // namespace esp_pika
