#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "afe_pipeline.h"
#include "board.h"
#include "ring_frame_queue.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace esp_pika {

struct AudioEngineConfig {
    uint32_t uplink_sample_rate = 16000;
    uint32_t downlink_sample_rate = 24000;
    uint16_t frame_ms = 60;
    size_t max_send_frames = 40;    // 40 * 60ms = 2.4s cap per architecture.md
    size_t max_decode_frames = 512; // Absorb burst DeepSeek WS TTS packets (~10.2s buffer)
    AfePipelineConfig afe;
};

struct WakeEvent {
    std::string wake_word;
    float wake_score = 0.0f;
    float snr_db = 0.0f;
    std::vector<int16_t> wake_pcm_16k;
};

/// Real-time audio pipeline pinned to HP Core 0 (I2S DMA -> AFE/VAD/WakeNet -> Opus 16kHz up / 24kHz down).
class AudioEngine {
public:
    using OpusUplinkCallback = std::function<void(const uint8_t* frame, size_t len)>;
    using WakeEventCallback = std::function<void(const WakeEvent& event)>;
    using VadStateCallback = std::function<void(bool speaking, float snr_db)>;

    AudioEngine() = default;
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool Init(AudioCodecHal* codec, const AudioEngineConfig& cfg);
    bool Start(OpusUplinkCallback on_uplink_opus,
               WakeEventCallback on_wake = nullptr,
               VadStateCallback on_vad = nullptr);
    void Stop();

    /// Enables or pauses upstream microphone frame transmission (controlled by AppStateMachine).
    void SetUplinkEnabled(bool enabled);
    bool IsUplinkEnabled() const { return uplink_enabled_.load(); }

    /// Feeds one captured 16kHz mono PCM frame through AFE/VAD/Pre-roll (used by Core 0 loop and host tests).
    bool ProcessCapturedPcm(const int16_t* mic_pcm, size_t samples, AfeStepResult* out = nullptr);

    /// Triggers manual/button wake while preserving recent 1.5s pre-roll PCM for Core 1 voiceprint.
    bool TriggerWakeForTest(float wake_score, float snr_db);

    /// Enqueues a 24kHz Opus downlink frame into bounded DecodeQ.
    bool EnqueueDownlinkOpus(const uint8_t* frame, size_t len);

    /// Triggers an immediate local ascending wake-confirmation chime on Core 0.
    void PlayWakeChime();

    /// Drains uplink SendQ into the supplied sender callback; returns count flushed.
    size_t FlushUplink(const OpusUplinkCallback& sender);

    void ClearQueues();

    size_t UplinkQueueSize() const { return send_queue_ ? send_queue_->Size() : 0; }
    size_t DownlinkQueueSize() const { return decode_queue_ ? decode_queue_->Size() : 0; }

    /// Normalized real-time microphone and speaker energy in [0.0, 1.0] (lock-free).
    float MicLevel() const { return mic_rms_.load(std::memory_order_relaxed); }
    float CleanMicLevel() const { return clean_mic_rms_.load(std::memory_order_relaxed); }
    float SpeakerLevel() const { return spk_rms_.load(std::memory_order_relaxed); }
    bool IsVadSpeaking() const { return vad_speaking_.load(std::memory_order_relaxed); }
    bool IsWakeChimePlaying() const {
        return wake_chime_playing_.load(std::memory_order_acquire) ||
               (skip_uplink_samples_.load(std::memory_order_relaxed) > 0);
    }

private:
    void HandleAfeStep(const AfeStepResult& step);

    AudioCodecHal* codec_ = nullptr;
    AudioEngineConfig cfg_;
    AfePipeline afe_;
    std::unique_ptr<BoundedFrameQueue> send_queue_;
    std::unique_ptr<BoundedFrameQueue> decode_queue_;
    std::unique_ptr<PcmPreRollBuffer> aec_ref_ring_;
    OpusUplinkCallback on_uplink_opus_;
    WakeEventCallback on_wake_;
    VadStateCallback on_vad_;
    std::atomic<bool> running_{false};
    std::atomic<bool> uplink_enabled_{false};
    std::atomic<bool> wake_chime_pending_{false};
    std::atomic<bool> wake_chime_playing_{false};
    std::atomic<uint32_t> skip_uplink_samples_{0};
    std::atomic<float> mic_rms_{0.0f};
    std::atomic<float> clean_mic_rms_{0.0f};
    std::atomic<float> spk_rms_{0.0f};
    std::atomic<bool> vad_speaking_{false};

#ifdef ESP_PLATFORM
    int16_t* dma_rx_pcm_ = nullptr;
    int16_t* dma_tx_pcm_ = nullptr;
    size_t tx_max_samples_ = 0;
    uint8_t* spiram_opus_scratch_ = nullptr;
    void* opus_enc_ = nullptr;
    void* opus_dec_ = nullptr;
    TaskHandle_t input_task_ = nullptr;
    TaskHandle_t output_task_ = nullptr;

    static void AudioInputTaskEntry(void* arg);
    static void AudioOutputTaskEntry(void* arg);
    void RunInputLoop();
    void RunOutputLoop();
#endif
};

} // namespace esp_pika
