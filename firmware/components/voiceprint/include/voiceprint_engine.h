#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "fbank_extractor.h"
#include "voiceprint_store.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

namespace esp_pika {

/// Three-tier edge/cloud speaker verification policy per architecture.md §4.3.
enum class ConfidenceTier {
    kRejectGuest = 0,     // < uncertain_threshold (default 0.65)
    kUncertainCloud = 1,  // [0.65, 0.85): verify via gateway 3D-Speaker + Qdrant
    kAcceptLocal = 2,     // >= accept_threshold (default 0.85)
};

struct VoiceprintConfig {
    size_t embedding_dim = 192;
    float accept_threshold = 0.85f;
    float uncertain_threshold = 0.65f;
    std::string model_path = "/model/voiceprint.espdl";
};

struct VoiceprintMatch {
    bool identified = false;
    std::string speaker_id;
    std::string display_name;
    SpeakerRole role = SpeakerRole::kGuest;
    float score = 0.0f;
    ConfidenceTier tier = ConfidenceTier::kRejectGuest;
};

struct WakeVoiceprintJob {
    std::string wake_word;
    float wake_score = 0.0f;
    float snr_db = 0.0f;
    std::vector<int16_t> pcm_16k;
    std::string wake_source;
    uint32_t generation = 0;
    bool during_playback = false;
};

struct WakeVoiceprintOutcome {
    WakeVoiceprintJob job;
    VoiceprintMatch match;
    int64_t inference_us = 0;
};

/// On-device INT8/FBank speaker recognition engine executing asynchronously on Core 1.
class VoiceprintEngine {
public:
    using AsyncOutcomeCallback = std::function<void(const WakeVoiceprintOutcome& outcome)>;

    VoiceprintEngine() = default;
    ~VoiceprintEngine();

    VoiceprintEngine(const VoiceprintEngine&) = delete;
    VoiceprintEngine& operator=(const VoiceprintEngine&) = delete;

    bool Init(VoiceprintStore* store, const VoiceprintConfig& cfg = {});

    /// Extracts an L2-normalized embedding vector (192-D default) from 16kHz mono PCM.
    bool ExtractEmbedding(const int16_t* pcm, size_t samples, std::vector<float>* out_embedding) const;

    /// Registers or updates a speaker template by averaging 1..N enrollment utterances.
    bool EnrollFromSamples(const std::string& speaker_id,
                           const std::string& display_name,
                           SpeakerRole role,
                           const std::vector<std::vector<int16_t>>& pcm_samples,
                           uint64_t version);

    /// Identifies the closest enrolled speaker using cosine similarity and 3-tier gating.
    bool Identify(const int16_t* pcm, size_t samples, VoiceprintMatch* out) const;

    /// Dynamically updates the local acceptance cosine threshold (clamped to [0.55, 0.95]).
    void SetAcceptThreshold(float threshold);
    float AcceptThreshold() const { return accept_threshold_.load(std::memory_order_relaxed); }

    /// Starts the Core 1 async inference worker (internal DRAM stack safe for NVS access).
    bool StartAsyncWorker(AsyncOutcomeCallback cb);
    void StopAsyncWorker();

    /// Enqueues a 1.0-1.5s wake-word PCM utterance for Core 1 inference without blocking Core 0.
    bool SubmitWakeUtteranceAsync(WakeVoiceprintJob job);

    /// Drains and processes one queued job synchronously (for deterministic host tests).
    bool ProcessOnePendingForTest();

    static float CosineSimilarity(const std::vector<float>& a, const std::vector<float>& b);
    static void L2Normalize(std::vector<float>* vec);

private:
    bool ComputePooledFbankEmbedding(const std::vector<float>& fbank,
                                     size_t num_frames,
                                     std::vector<float>* out_embedding) const;

    VoiceprintStore* store_ = nullptr;
    VoiceprintConfig cfg_;
    std::atomic<float> accept_threshold_{0.85f};
    FbankExtractor fbank_;
    bool initialized_ = false;

    mutable std::mutex job_mu_;
    std::deque<WakeVoiceprintJob> pending_jobs_;
    AsyncOutcomeCallback on_outcome_;
    std::atomic<bool> worker_running_{false};

#ifdef ESP_PLATFORM
    bool espdl_loaded_ = false;
    void* dl_model_ = nullptr;
    TaskHandle_t worker_task_ = nullptr;
    SemaphoreHandle_t job_sem_ = nullptr;
    static void WorkerTaskEntry(void* arg);
    void RunWorkerLoop();
#endif
};

} // namespace esp_pika
