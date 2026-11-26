#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "afe_pipeline.h"
#include "board.h"
#include "ring_frame_queue.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace esp_pika {

/// Uplink / downlink audio framing format on the active transport connection.
enum class AudioTransportCodec {
    kOpus60ms = 0, // Gateway mode: 16kHz 60ms Opus uplink / Opus downlink
    kPcm16_20ms,   // EZ Assistant direct mode: 16kHz mono 20ms (640B) PCM16 LE
};

/// Active wake detection strategy controlled by EZ Assistant native wake config.
enum class AudioWakeMode {
    kWakeWord = 0,      // MultiNet7 wake word
    kWakeWordPlusVp,    // MultiNet7 wake word + Core 1 voiceprint gate
    kVoiceprintDirect,  // MultiNet7 paused; VAD + PSRAM speculative utterance + Core 1 voiceprint
};

struct AudioEngineConfig {
    uint32_t uplink_sample_rate = 16000;
    uint32_t downlink_sample_rate = 24000;
    uint16_t frame_ms = 60;
    AudioTransportCodec transport_codec = AudioTransportCodec::kOpus60ms;
    size_t max_send_frames = 180;   // 180 * 20ms = 3.6s PCM16 headroom for speculative first-sentence flush
    size_t max_decode_frames = 512; // Absorb burst TTS packets (~10.2s buffer)
    AfePipelineConfig afe;
};

struct WakeEvent {
    std::string wake_word;
    float wake_score = 0.0f;
    float snr_db = 0.0f;
    std::vector<int16_t> wake_pcm_16k;
};

/// Candidate speech window emitted in `kVoiceprintDirect` mode for Core 1 voiceprint verification.
struct VoiceprintCandidateEvent {
    uint32_t generation = 0;
    uint32_t voiced_ms = 0;
    float snr_db = 0.0f;
    std::vector<int16_t> pcm_16k;
};

/// Real-time audio pipeline pinned to HP Core 0 (I2S DMA -> AFE/VAD/WakeNet -> Opus 16kHz up / 24kHz down).
class AudioEngine {
public:
    using OpusUplinkCallback = std::function<void(const uint8_t* frame, size_t len)>;
    using WakeEventCallback = std::function<void(const WakeEvent& event)>;
    using VadStateCallback = std::function<void(bool speaking, float snr_db)>;
    using VoiceprintCandidateCallback = std::function<void(const VoiceprintCandidateEvent& event)>;

    AudioEngine() = default;
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool Init(AudioCodecHal* codec, const AudioEngineConfig& cfg);
    bool Start(OpusUplinkCallback on_uplink_opus,
               WakeEventCallback on_wake = nullptr,
               VadStateCallback on_vad = nullptr,
               VoiceprintCandidateCallback on_vp_candidate = nullptr);
    void Stop();

    /// Enables or pauses upstream microphone frame transmission (controlled by AppStateMachine).
    void SetUplinkEnabled(bool enabled);
    bool IsUplinkEnabled() const { return uplink_enabled_.load(); }

    /// Configures active wake mode & voiceprint-direct gating parameters at runtime.
    void SetWakeMode(AudioWakeMode mode, uint32_t min_speech_ms = 700, uint32_t cooldown_ms = 1500);
    AudioWakeMode WakeMode() const {
        return static_cast<AudioWakeMode>(wake_mode_.load(std::memory_order_acquire));
    }

    /// Hot-updates the active MultiNet7 wake word and pinyin at runtime without reboot.
    bool UpdateWakePinyin(const std::string& wake_word,
                          const std::string& wake_pinyin,
                          std::string* error_out = nullptr);
    bool IsMultiNetReady() const { return afe_.IsMultiNetReady(); }

    /// Approves a pending `kVoiceprintDirect` candidate and flushes buffered first-sentence PCM.
    bool AcceptSpeculativeWake(uint32_t generation);
    /// Rejects a pending `kVoiceprintDirect` candidate, discards buffer, and starts cooldown.
    void RejectSpeculativeWake(uint32_t generation);
    /// Drains any approved speculative PCM waiting to be prepended to the uplink stream.
    bool TakePendingSpeculativePcm(std::vector<int16_t>* out);

    /// Captures up to `max_ms` of AFE-cleaned 16kHz speech from Mosaico's own mic for voiceprint enrollment.
    bool CaptureSpeechSample(uint32_t max_ms,
                             std::vector<int16_t>* out_pcm,
                             uint32_t* out_voiced_ms,
                             float* out_snr_db);

    /// Switches uplink/downlink transport framing dynamically between Opus 60ms and PCM16 20ms.
    void SetTransportCodec(AudioTransportCodec codec);
    AudioTransportCodec TransportCodec() const {
        return static_cast<AudioTransportCodec>(transport_codec_.load(std::memory_order_acquire));
    }

    /// Feeds one captured 16kHz mono PCM frame through AFE/VAD/Pre-roll (used by Core 0 loop and host tests).
    bool ProcessCapturedPcm(const int16_t* mic_pcm, size_t samples, AfeStepResult* out = nullptr);

    /// Triggers manual/button wake while preserving recent 1.5s pre-roll PCM for Core 1 voiceprint.
    bool TriggerWakeForTest(float wake_score, float snr_db);

    /// Enqueues a 24kHz Opus downlink frame into bounded DecodeQ.
    bool EnqueueDownlinkOpus(const uint8_t* frame, size_t len);

    /// Enqueues a 16kHz mono little-endian PCM16 downlink chunk into bounded DecodeQ.
    bool EnqueueDownlinkPcm16(const uint8_t* pcm_bytes, size_t len);

    /// Clears only buffered downlink frames (e.g. barge-in playback_cancel) without touching uplink.
    void ClearDownlinkQueue();

    /// Triggers an immediate local ascending wake-confirmation chime on Core 0.
    void PlayWakeChime();

    /// Drains uplink SendQ into the supplied sender callback; returns count flushed.
    size_t FlushUplink(const OpusUplinkCallback& sender);
    /// Drains uplink SendQ while `sender` returns true; stops immediately on false.
    size_t FlushUplinkChecked(const std::function<bool(const uint8_t* frame, size_t len)>& sender,
                              bool* out_aborted = nullptr);

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
    enum class SpeculativeState {
        kIdle = 0,
        kCollecting,
        kEvaluating,
        kCooldown,
    };

    void HandleAfeStep(const AfeStepResult& step);
    void UpdateSpeculativeVoiceprint(const AfeStepResult& step, uint32_t frame_ms);
    void UpdateCaptureSample(const AfeStepResult& step, uint32_t frame_ms);

    AudioCodecHal* codec_ = nullptr;
    AudioEngineConfig cfg_;
    AfePipeline afe_;
    std::unique_ptr<BoundedFrameQueue> send_queue_;
    std::unique_ptr<BoundedFrameQueue> decode_queue_;
    std::unique_ptr<PcmPreRollBuffer> aec_ref_ring_;
    OpusUplinkCallback on_uplink_opus_;
    WakeEventCallback on_wake_;
    VadStateCallback on_vad_;
    VoiceprintCandidateCallback on_vp_candidate_;
    std::atomic<bool> running_{false};
    std::atomic<bool> uplink_enabled_{false};
    std::atomic<int> wake_mode_{static_cast<int>(AudioWakeMode::kWakeWord)};
    std::atomic<uint32_t> vp_min_speech_ms_{700};
    std::atomic<uint32_t> vp_cooldown_ms_{1500};
    std::atomic<int> transport_codec_{static_cast<int>(AudioTransportCodec::kOpus60ms)};
    std::atomic<bool> wake_chime_pending_{false};
    std::atomic<bool> wake_chime_playing_{false};
    std::atomic<uint32_t> skip_uplink_samples_{0};
    std::atomic<float> mic_rms_{0.0f};
    std::atomic<float> clean_mic_rms_{0.0f};
    std::atomic<float> spk_rms_{0.0f};
    std::atomic<bool> vad_speaking_{false};

    // Speculative PSRAM utterance buffer for zero-word-loss voiceprint-direct wake.
    mutable std::mutex spec_mu_;
    SpeculativeState spec_state_ = SpeculativeState::kIdle;
    uint32_t spec_gen_ = 0;
    uint32_t spec_voiced_ms_ = 0;
    uint32_t spec_cooldown_rem_ms_ = 0;
    std::vector<int16_t> spec_pcm_;
    std::vector<int16_t> pending_flush_pcm_;

    // On-device voiceprint capture state (used by `device.voiceprint_capture`).
    mutable std::mutex capture_mu_;
    std::condition_variable capture_cv_;
    std::atomic<bool> capture_active_{false};
    bool capture_done_ = false;
    uint32_t capture_max_ms_ = 3000;
    uint32_t capture_elapsed_ms_ = 0;
    uint32_t capture_voiced_ms_ = 0;
    uint32_t capture_silence_ms_ = 0;
    float capture_peak_snr_db_ = 0.0f;
    std::vector<int16_t> capture_pcm_;

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
