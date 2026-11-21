#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace esp_pika {

/// Circular PCM buffer (PSRAM-backed on ESP32-S31) retaining recent microphone audio
/// for both 300-500ms VAD pre-roll flush and 1.0-1.5s text-dependent wake voiceprint extraction.
class PcmPreRollBuffer {
public:
    explicit PcmPreRollBuffer(size_t capacity_samples = 24000); // 1.5s @ 16kHz
    ~PcmPreRollBuffer();

    PcmPreRollBuffer(const PcmPreRollBuffer&) = delete;
    PcmPreRollBuffer& operator=(const PcmPreRollBuffer&) = delete;

    void PushSamples(const int16_t* pcm, size_t count);
    void SnapshotRecent(size_t max_samples, std::vector<int16_t>* out) const;
    void Clear();

    size_t Size() const;
    size_t Capacity() const { return capacity_; }

private:
    size_t capacity_ = 0;
    int16_t* storage_ = nullptr;
#ifdef ESP_PLATFORM
    bool owns_spiram_ = false;
#endif
    std::vector<int16_t> host_storage_;
    size_t write_pos_ = 0;
    size_t filled_ = 0;
    mutable std::mutex mu_;
};

struct AfePipelineConfig {
    uint32_t sample_rate = 16000;
    bool enable_aec = true;
    bool enable_ns = true;
    bool enable_vadnet = true;
    bool enable_wakenet = true;
    uint32_t pre_roll_ms = 400;
    uint32_t wake_vp_window_ms = 1500;
    std::string wake_word = "hi_esp";
    float vad_energy_threshold = 450.0f;
    uint32_t vad_hangover_ms = 400;
};

struct AfeStepResult {
    bool vad_speaking = false;
    bool vad_started = false;
    bool vad_ended = false;
    bool wake_detected = false;
    std::string wake_word;
    float wake_score = 0.0f;
    float snr_db = 0.0f;
    std::vector<int16_t> clean_pcm;
    std::vector<int16_t> pre_roll_pcm;
    std::vector<int16_t> wake_pcm;
};

/// Acoustic Front-End wrapper combining ESP-SR (AEC/NS/VADNet/WakeNet) with PSRAM pre-roll
/// and deterministic SNR/VAD fallback for host verification and Lite platforms.
class AfePipeline {
public:
    AfePipeline() = default;
    ~AfePipeline();

    AfePipeline(const AfePipeline&) = delete;
    AfePipeline& operator=(const AfePipeline&) = delete;

    bool Init(const AfePipelineConfig& cfg);

    /// Feeds one 16kHz mono mic frame (and optional speaker AEC reference) and fetches AFE output.
    bool ProcessFrame(const int16_t* mic_pcm,
                      const int16_t* ref_pcm,
                      size_t samples,
                      AfeStepResult* out);

    /// Injects a test wake event (useful for unit tests and manual push-to-talk triggers).
    bool TriggerManualWake(float wake_score, float snr_db, AfeStepResult* out);

    size_t FeedSampleRate() const { return cfg_.sample_rate; }
    bool IsSpeaking() const { return speaking_; }

private:
    float UpdateNoiseAndComputeSnr(const int16_t* pcm, size_t samples, float* out_rms);

    AfePipelineConfig cfg_;
    std::unique_ptr<PcmPreRollBuffer> pre_roll_;
    bool initialized_ = false;
    bool speaking_ = false;
    uint32_t hangover_elapsed_ms_ = 0;
    float noise_floor_rms_ = 80.0f;
    float last_snr_db_ = 0.0f;

#ifdef ESP_PLATFORM
    void* afe_handle_ = nullptr;
    const void* afe_iface_ = nullptr;
#endif
};

} // namespace esp_pika
