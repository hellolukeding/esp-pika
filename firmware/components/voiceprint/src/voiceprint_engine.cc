#include "voiceprint_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#ifdef ESP_PLATFORM
#include "dl_model_base.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#endif

namespace esp_pika {
namespace {

#ifdef ESP_PLATFORM
constexpr const char* kTag = "vp_engine";
constexpr UBaseType_t kVoiceprintCoreId = 1; // Core 1 per AGENTS.md §3.1
#endif
constexpr size_t kMaxPendingJobs = 4;

int64_t NowMicros() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

VoiceprintEngine::~VoiceprintEngine() {
    StopAsyncWorker();
#ifdef ESP_PLATFORM
    if (dl_model_ != nullptr) {
        delete static_cast<dl::Model*>(dl_model_);
        dl_model_ = nullptr;
    }
    if (job_sem_ != nullptr) {
        vSemaphoreDelete(job_sem_);
        job_sem_ = nullptr;
    }
#endif
}

bool VoiceprintEngine::Init(VoiceprintStore* store, const VoiceprintConfig& cfg) {
    if (store == nullptr || cfg.embedding_dim == 0) {
        return false;
    }
    store_ = store;
    cfg_ = cfg;
    accept_threshold_.store(std::clamp(cfg.accept_threshold, 0.55f, 0.95f), std::memory_order_relaxed);
    if (!fbank_.Init()) {
        return false;
    }

#ifdef ESP_PLATFORM
    if (!cfg_.model_path.empty()) {
        FILE* f = std::fopen(cfg_.model_path.c_str(), "rb");
        if (f != nullptr) {
            std::fclose(f);
            auto* model = new (std::nothrow) dl::Model(
                cfg_.model_path.c_str(), fbs::MODEL_LOCATION_IN_FLASH_RODATA);
            if (model != nullptr) {
                dl_model_ = model;
                espdl_loaded_ = true;
                ESP_LOGI(kTag, "loaded INT8 ESP-DL model: %s", cfg_.model_path.c_str());
            }
        } else {
            ESP_LOGW(kTag, "model %s not flashed; using FBank statistics pooling backend",
                     cfg_.model_path.c_str());
        }
    }
    if (job_sem_ == nullptr) {
        job_sem_ = xSemaphoreCreateCounting(kMaxPendingJobs, 0);
    }
#endif

    initialized_ = true;
    return true;
}

void VoiceprintEngine::L2Normalize(std::vector<float>* vec) {
    if (vec == nullptr || vec->empty()) {
        return;
    }
    double norm_sq = 0.0;
    for (float v : *vec) {
        norm_sq += static_cast<double>(v) * static_cast<double>(v);
    }
    if (norm_sq <= 1e-12) {
        return;
    }
    const float inv_norm = static_cast<float>(1.0 / std::sqrt(norm_sq));
    for (float& v : *vec) {
        v *= inv_norm;
    }
}

float VoiceprintEngine::CosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || a.size() != b.size()) {
        return 0.0f;
    }
    double dot = 0.0;
    double na = 0.0;
    double nb = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double va = static_cast<double>(a[i]);
        const double vb = static_cast<double>(b[i]);
        dot += va * vb;
        na += va * va;
        nb += vb * vb;
    }
    if (na <= 1e-12 || nb <= 1e-12) {
        return 0.0f;
    }
    return static_cast<float>(dot / (std::sqrt(na) * std::sqrt(nb)));
}

bool VoiceprintEngine::ComputePooledFbankEmbedding(const std::vector<float>& fbank,
                                                  size_t num_frames,
                                                  std::vector<float>* out_embedding) const {
    const size_t bins = fbank_.Config().num_mel_bins;
    if (num_frames == 0 || fbank.size() != num_frames * bins || out_embedding == nullptr) {
        return false;
    }

    std::vector<float> mean(bins, 0.0f);
    std::vector<float> stddev(bins, 0.0f);
    std::vector<float> delta_rms(bins, 0.0f);
    std::vector<float> spectral_contrast(bins, 0.0f);

    for (size_t f = 0; f < num_frames; ++f) {
        const float* row = fbank.data() + f * bins;
        for (size_t m = 0; m < bins; ++m) {
            mean[m] += row[m];
        }
    }
    for (size_t m = 0; m < bins; ++m) {
        mean[m] /= static_cast<float>(num_frames);
    }

    for (size_t f = 0; f < num_frames; ++f) {
        const float* row = fbank.data() + f * bins;
        for (size_t m = 0; m < bins; ++m) {
            const float diff = row[m] - mean[m];
            stddev[m] += diff * diff;
            if (f > 0) {
                const float d = row[m] - fbank[(f - 1) * bins + m];
                delta_rms[m] += d * d;
            }
            if (m + 1 < bins) {
                spectral_contrast[m] += (row[m + 1] - row[m]);
            }
        }
    }
    for (size_t m = 0; m < bins; ++m) {
        stddev[m] = std::sqrt(stddev[m] / static_cast<float>(num_frames));
        delta_rms[m] = (num_frames > 1) ? std::sqrt(delta_rms[m] / static_cast<float>(num_frames - 1)) : 0.0f;
        spectral_contrast[m] /= static_cast<float>(num_frames);
    }

    // Construct deterministic 192-D speaker embedding combining band stddev, deltas, contrasts, and sub-band ratios.
    out_embedding->assign(cfg_.embedding_dim, 0.0f);
    for (size_t i = 0; i < cfg_.embedding_dim; ++i) {
        const size_t band = i % bins;
        const size_t grp = i / bins;
        float val = 0.0f;
        switch (grp) {
        case 0: val = stddev[band]; break;
        case 1: val = delta_rms[band]; break;
        case 2: val = spectral_contrast[band]; break;
        case 3: val = stddev[band] - stddev[(band + bins / 2) % bins]; break;
        default: val = delta_rms[band] * 0.5f + spectral_contrast[(band + 3) % bins] * 0.5f; break;
        }
        (*out_embedding)[i] = val;
    }
    L2Normalize(out_embedding);
    return true;
}

bool VoiceprintEngine::ExtractEmbedding(const int16_t* pcm,
                                        size_t samples,
                                        std::vector<float>* out_embedding) const {
    if (!initialized_ || pcm == nullptr || samples == 0 || out_embedding == nullptr) {
        return false;
    }
    std::vector<float> fbank;
    size_t num_frames = 0;
    if (!fbank_.Extract(pcm, samples, &fbank, &num_frames)) {
        return false;
    }

#ifdef ESP_PLATFORM
    if (espdl_loaded_ && dl_model_ != nullptr) {
        auto* model = static_cast<dl::Model*>(dl_model_);
        auto inputs = model->get_inputs();
        if (!inputs.empty()) {
            dl::TensorBase* in_tensor = inputs.begin()->second;
            if (in_tensor != nullptr && in_tensor->size > 0) {
                const size_t copy_elems = std::min<size_t>(fbank.size(), static_cast<size_t>(in_tensor->size));
                auto* dst = static_cast<float*>(in_tensor->data);
                if (dst != nullptr && in_tensor->dtype == dl::DATA_TYPE_FLOAT) {
                    std::copy_n(fbank.data(), copy_elems, dst);
                }
            }
            model->run();
            auto outputs = model->get_outputs();
            if (!outputs.empty()) {
                dl::TensorBase* out_tensor = outputs.begin()->second;
                if (out_tensor != nullptr && out_tensor->data != nullptr &&
                    out_tensor->dtype == dl::DATA_TYPE_FLOAT && out_tensor->size >= static_cast<int>(cfg_.embedding_dim)) {
                    const auto* src = static_cast<const float*>(out_tensor->data);
                    out_embedding->assign(src, src + cfg_.embedding_dim);
                    L2Normalize(out_embedding);
                    return true;
                }
            }
        }
    }
#endif

    return ComputePooledFbankEmbedding(fbank, num_frames, out_embedding);
}

bool VoiceprintEngine::EnrollFromSamples(const std::string& speaker_id,
                                         const std::string& display_name,
                                         SpeakerRole role,
                                         const std::vector<std::vector<int16_t>>& pcm_samples,
                                         uint64_t version) {
    if (!initialized_ || store_ == nullptr || speaker_id.empty() || pcm_samples.empty()) {
        return false;
    }
    std::vector<float> avg_emb(cfg_.embedding_dim, 0.0f);
    size_t valid_count = 0;
    for (const auto& utt : pcm_samples) {
        if (utt.empty()) {
            continue;
        }
        std::vector<float> emb;
        if (!ExtractEmbedding(utt.data(), utt.size(), &emb) || emb.size() != cfg_.embedding_dim) {
            continue;
        }
        for (size_t d = 0; d < cfg_.embedding_dim; ++d) {
            avg_emb[d] += emb[d];
        }
        ++valid_count;
    }
    if (valid_count == 0) {
        return false;
    }
    for (float& v : avg_emb) {
        v /= static_cast<float>(valid_count);
    }
    L2Normalize(&avg_emb);

    VoiceprintTemplate tpl;
    tpl.speaker_id = speaker_id;
    tpl.display_name = display_name;
    tpl.role = role;
    tpl.embedding = std::move(avg_emb);
    tpl.version = version;
    return store_->Upsert(std::move(tpl));
}

void VoiceprintEngine::SetAcceptThreshold(float threshold) {
    accept_threshold_.store(std::clamp(threshold, 0.55f, 0.95f), std::memory_order_relaxed);
}

bool VoiceprintEngine::Identify(const int16_t* pcm, size_t samples, VoiceprintMatch* out) const {
    if (!initialized_ || store_ == nullptr || out == nullptr) {
        return false;
    }
    *out = VoiceprintMatch{};

    std::vector<float> query;
    if (!ExtractEmbedding(pcm, samples, &query)) {
        return false;
    }

    const auto templates = store_->Snapshot();
    if (templates.empty()) {
        out->tier = ConfidenceTier::kRejectGuest;
        out->role = SpeakerRole::kGuest;
        return true;
    }

    float best_score = -1.0f;
    const VoiceprintTemplate* best_tpl = nullptr;
    for (const auto& tpl : templates) {
        const float score = CosineSimilarity(query, tpl.embedding);
        if (score > best_score) {
            best_score = score;
            best_tpl = &tpl;
        }
    }

    out->score = best_score;
    if (best_tpl == nullptr) {
        return true;
    }
    const size_t hash_pos = best_tpl->speaker_id.find('#');
    out->speaker_id = (hash_pos != std::string::npos) ? best_tpl->speaker_id.substr(0, hash_pos)
                                                      : best_tpl->speaker_id;
    out->display_name = best_tpl->display_name;

    const float accept_th = accept_threshold_.load(std::memory_order_relaxed);
    const float uncertain_th = std::min(cfg_.uncertain_threshold, accept_th);
    if (best_score >= accept_th) {
        out->identified = true;
        out->tier = ConfidenceTier::kAcceptLocal;
        out->role = best_tpl->role;
    } else if (best_score >= uncertain_th) {
        out->identified = false;
        out->tier = ConfidenceTier::kUncertainCloud;
        out->role = SpeakerRole::kGuest;
    } else {
        out->identified = false;
        out->tier = ConfidenceTier::kRejectGuest;
        out->speaker_id.clear();
        out->display_name.clear();
        out->role = SpeakerRole::kGuest;
    }
    return true;
}

bool VoiceprintEngine::StartAsyncWorker(AsyncOutcomeCallback cb) {
    if (!initialized_) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(job_mu_);
        on_outcome_ = std::move(cb);
    }
    if (worker_running_.load()) {
        return true;
    }
    worker_running_.store(true);
#ifdef ESP_PLATFORM
    // Core 1 internal-DRAM stack task so encrypted NVS reads/writes never violate Flash cache rules.
    BaseType_t ok = xTaskCreatePinnedToCore(&VoiceprintEngine::WorkerTaskEntry, "VoiceprintTask",
                                            8192, this, 4, &worker_task_, kVoiceprintCoreId);
    if (ok != pdPASS) {
        worker_running_.store(false);
        return false;
    }
#endif
    return true;
}

void VoiceprintEngine::StopAsyncWorker() {
    worker_running_.store(false);
#ifdef ESP_PLATFORM
    if (job_sem_ != nullptr) {
        xSemaphoreGive(job_sem_);
    }
    if (worker_task_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(50));
        worker_task_ = nullptr;
    }
#endif
}

bool VoiceprintEngine::SubmitWakeUtteranceAsync(WakeVoiceprintJob job) {
    if (!initialized_ || job.pcm_16k.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(job_mu_);
        if (pending_jobs_.size() >= kMaxPendingJobs) {
            pending_jobs_.pop_front();
        }
        pending_jobs_.push_back(std::move(job));
    }
#ifdef ESP_PLATFORM
    if (job_sem_ != nullptr) {
        xSemaphoreGive(job_sem_);
    }
#endif
    return true;
}

bool VoiceprintEngine::ProcessOnePendingForTest() {
    WakeVoiceprintJob job;
    AsyncOutcomeCallback cb;
    {
        std::lock_guard<std::mutex> lock(job_mu_);
        if (pending_jobs_.empty()) {
            return false;
        }
        job = std::move(pending_jobs_.front());
        pending_jobs_.pop_front();
        cb = on_outcome_;
    }

    const int64_t t0 = NowMicros();
    VoiceprintMatch match;
    if (!Identify(job.pcm_16k.data(), job.pcm_16k.size(), &match)) {
        return false;
    }
    WakeVoiceprintOutcome outcome;
    outcome.job = std::move(job);
    outcome.match = std::move(match);
    outcome.inference_us = NowMicros() - t0;
    if (cb) {
        cb(outcome);
    }
    return true;
}

#ifdef ESP_PLATFORM
void VoiceprintEngine::WorkerTaskEntry(void* arg) {
    static_cast<VoiceprintEngine*>(arg)->RunWorkerLoop();
    vTaskDelete(nullptr);
}

void VoiceprintEngine::RunWorkerLoop() {
    while (worker_running_.load()) {
        if (job_sem_ != nullptr && xSemaphoreTake(job_sem_, pdMS_TO_TICKS(250)) != pdTRUE) {
            continue;
        }
        while (ProcessOnePendingForTest()) {
        }
    }
}
#endif

} // namespace esp_pika
