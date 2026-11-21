#include "audio_engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_audio_dec_default.h"
#include "esp_audio_enc_default.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#endif

namespace esp_pika {
namespace {

float ComputeNormalizedPcmRms(const int16_t* pcm, size_t samples) {
    if (pcm == nullptr || samples == 0) {
        return 0.0f;
    }
    double sum_sq = 0.0;
    for (size_t i = 0; i < samples; ++i) {
        const double v = static_cast<double>(pcm[i]) / 32768.0;
        sum_sq += v * v;
    }
    const float rms = static_cast<float>(std::sqrt(sum_sq / static_cast<double>(samples)));
    return std::clamp(rms * 3.5f, 0.0f, 1.0f);
}

#ifdef ESP_PLATFORM
constexpr const char* kTag = "audio_engine";
constexpr size_t kOpusFrameMaxBytes = 1024;
constexpr UBaseType_t kAudioCoreId = 0;

void Downsample24kTo16kLinear(const int16_t* in24k, size_t in_samples, std::vector<int16_t>* out16k) {
    if (in24k == nullptr || in_samples == 0 || out16k == nullptr) {
        return;
    }
    const size_t out_samples = (in_samples * 2) / 3;
    out16k->resize(out_samples);
    for (size_t i = 0; i < out_samples; ++i) {
        size_t src_idx = (i * 3) / 2;
        if (src_idx >= in_samples) {
            src_idx = in_samples - 1;
        }
        (*out16k)[i] = in24k[src_idx];
    }
}
#endif

} // namespace

AudioEngine::~AudioEngine() {
    Stop();
#ifdef ESP_PLATFORM
    if (dma_rx_pcm_ != nullptr) {
        heap_caps_free(dma_rx_pcm_);
        dma_rx_pcm_ = nullptr;
    }
    if (dma_tx_pcm_ != nullptr) {
        heap_caps_free(dma_tx_pcm_);
        dma_tx_pcm_ = nullptr;
    }
    if (spiram_opus_scratch_ != nullptr) {
        heap_caps_free(spiram_opus_scratch_);
        spiram_opus_scratch_ = nullptr;
    }
    if (opus_enc_ != nullptr) {
        esp_opus_enc_close(opus_enc_);
        opus_enc_ = nullptr;
    }
    if (opus_dec_ != nullptr) {
        esp_opus_dec_close(opus_dec_);
        opus_dec_ = nullptr;
    }
#endif
}

bool AudioEngine::Init(AudioCodecHal* codec, const AudioEngineConfig& cfg) {
    if (codec == nullptr || cfg.frame_ms == 0) {
        return false;
    }
    codec_ = codec;
    cfg_ = cfg;
    send_queue_ = std::make_unique<BoundedFrameQueue>(cfg_.max_send_frames);
    decode_queue_ = std::make_unique<BoundedFrameQueue>(cfg_.max_decode_frames);
    aec_ref_ring_ = std::make_unique<PcmPreRollBuffer>(cfg_.uplink_sample_rate / 2);

    if (!afe_.Init(cfg_.afe)) {
        return false;
    }

#ifdef ESP_PLATFORM
    const size_t rx_samples = (cfg_.uplink_sample_rate * cfg_.frame_ms) / 1000;
    const size_t tx_samples = (cfg_.downlink_sample_rate * cfg_.frame_ms) / 1000;

    // Allocate I2S DMA scratch buffers explicitly in internal DRAM per AGENTS.md §3.1.
    dma_rx_pcm_ = static_cast<int16_t*>(
        heap_caps_malloc(rx_samples * sizeof(int16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    dma_tx_pcm_ = static_cast<int16_t*>(
        heap_caps_malloc(tx_samples * sizeof(int16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    // Allocate larger codec scratch in PSRAM.
    spiram_opus_scratch_ = static_cast<uint8_t*>(
        heap_caps_malloc(kOpusFrameMaxBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    if (dma_rx_pcm_ == nullptr || dma_tx_pcm_ == nullptr || spiram_opus_scratch_ == nullptr) {
        ESP_LOGE(kTag, "failed to allocate audio buffers (DMA/PSRAM)");
        return false;
    }

    esp_opus_enc_config_t enc_cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc_cfg.sample_rate = static_cast<int>(cfg_.uplink_sample_rate);
    enc_cfg.channel = 1;
    enc_cfg.bits_per_sample = 16;
    enc_cfg.bitrate = 24000;
    enc_cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    enc_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    if (esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &opus_enc_) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(kTag, "esp_opus_enc_open failed");
        return false;
    }

    esp_opus_dec_cfg_t dec_cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    dec_cfg.sample_rate = static_cast<int>(cfg_.downlink_sample_rate);
    dec_cfg.channel = 1;
    dec_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    if (esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &opus_dec_) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(kTag, "esp_opus_dec_open failed");
        return false;
    }
#endif
    return true;
}

bool AudioEngine::Start(OpusUplinkCallback on_uplink_opus,
                        WakeEventCallback on_wake,
                        VadStateCallback on_vad) {
    if (codec_ == nullptr || running_.load()) {
        return false;
    }
    on_uplink_opus_ = std::move(on_uplink_opus);
    on_wake_ = std::move(on_wake);
    on_vad_ = std::move(on_vad);
    running_.store(true);

#ifdef ESP_PLATFORM
    // Pin real-time audio tasks strictly to HP Core 0 per AGENTS.md §3.1.
    BaseType_t ok_in = xTaskCreatePinnedToCore(&AudioEngine::AudioInputTaskEntry, "AudioInputTask",
                                               12288, this, 6, &input_task_, kAudioCoreId);
    BaseType_t ok_out = xTaskCreatePinnedToCore(&AudioEngine::AudioOutputTaskEntry, "AudioOutputTask",
                                                8192, this, 6, &output_task_, kAudioCoreId);
    if (ok_in != pdPASS || ok_out != pdPASS) {
        Stop();
        return false;
    }
#endif
    return true;
}

void AudioEngine::Stop() {
    running_.store(false);
#ifdef ESP_PLATFORM
    if (input_task_ != nullptr || output_task_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(80));
        input_task_ = nullptr;
        output_task_ = nullptr;
    }
#endif
}

void AudioEngine::SetUplinkEnabled(bool enabled) {
    uplink_enabled_.store(enabled);
}

void AudioEngine::HandleAfeStep(const AfeStepResult& step) {
    if ((step.vad_started || step.vad_ended) && on_vad_) {
        on_vad_(step.vad_speaking, step.snr_db);
    }
    if (step.wake_detected && on_wake_) {
        WakeEvent evt;
        evt.wake_word = step.wake_word;
        evt.wake_score = step.wake_score;
        evt.snr_db = step.snr_db;
        evt.wake_pcm_16k = step.wake_pcm;
        on_wake_(evt);
    }
}

bool AudioEngine::ProcessCapturedPcm(const int16_t* mic_pcm, size_t samples, AfeStepResult* out) {
    const float instant = ComputeNormalizedPcmRms(mic_pcm, samples);
    const float prev = mic_rms_.load(std::memory_order_relaxed);
    const float smoothed = (instant > prev) ? (prev * 0.35f + instant * 0.65f) : (prev * 0.80f + instant * 0.20f);
    mic_rms_.store(smoothed, std::memory_order_relaxed);

    std::vector<int16_t> ref;
    if (aec_ref_ring_) {
        aec_ref_ring_->SnapshotRecent(samples, &ref);
    }
    AfeStepResult local_step;
    AfeStepResult* step = (out != nullptr) ? out : &local_step;
    if (!afe_.ProcessFrame(mic_pcm, ref.empty() ? nullptr : ref.data(), samples, step)) {
        return false;
    }
    HandleAfeStep(*step);
    return true;
}

bool AudioEngine::TriggerWakeForTest(float wake_score, float snr_db) {
    AfeStepResult step;
    if (!afe_.TriggerManualWake(wake_score, snr_db, &step)) {
        return false;
    }
    HandleAfeStep(step);
    return true;
}

bool AudioEngine::EnqueueDownlinkOpus(const uint8_t* frame, size_t len) {
    if (decode_queue_ == nullptr || frame == nullptr || len == 0) {
        return false;
    }
    std::vector<uint8_t> buf(frame, frame + len);
    return decode_queue_->PushDropOldest(std::move(buf));
}

size_t AudioEngine::FlushUplink(const OpusUplinkCallback& sender) {
    if (send_queue_ == nullptr || !sender) {
        return 0;
    }
    size_t count = 0;
    std::vector<uint8_t> frame;
    while (send_queue_->Pop(&frame)) {
        sender(frame.data(), frame.size());
        ++count;
    }
    return count;
}

void AudioEngine::ClearQueues() {
    if (send_queue_) {
        send_queue_->Clear();
    }
    if (decode_queue_) {
        decode_queue_->Clear();
    }
}

#ifdef ESP_PLATFORM
void AudioEngine::AudioInputTaskEntry(void* arg) {
    static_cast<AudioEngine*>(arg)->RunInputLoop();
    vTaskDelete(nullptr);
}

void AudioEngine::AudioOutputTaskEntry(void* arg) {
    static_cast<AudioEngine*>(arg)->RunOutputLoop();
    vTaskDelete(nullptr);
}

void AudioEngine::RunInputLoop() {
    const size_t rx_samples = (cfg_.uplink_sample_rate * cfg_.frame_ms) / 1000;
    auto encode_and_queue = [this](const int16_t* pcm, size_t samples) {
        size_t offset = 0;
        const size_t chunk = (cfg_.uplink_sample_rate * cfg_.frame_ms) / 1000;
        while (offset + chunk <= samples) {
            esp_audio_enc_in_frame_t in_frame = {};
            in_frame.buffer = reinterpret_cast<uint8_t*>(const_cast<int16_t*>(pcm + offset));
            in_frame.len = static_cast<uint32_t>(chunk * sizeof(int16_t));

            esp_audio_enc_out_frame_t out_frame = {};
            out_frame.buffer = spiram_opus_scratch_;
            out_frame.len = static_cast<uint32_t>(kOpusFrameMaxBytes);
            out_frame.encoded_bytes = 0;

            if (esp_opus_enc_process(opus_enc_, &in_frame, &out_frame) == ESP_AUDIO_ERR_OK &&
                out_frame.encoded_bytes > 0) {
                std::vector<uint8_t> pkt(spiram_opus_scratch_,
                                         spiram_opus_scratch_ + out_frame.encoded_bytes);
                send_queue_->PushDropOldest(std::move(pkt));
            }
            offset += chunk;
        }
        FlushUplink(on_uplink_opus_);
    };

    while (running_.load()) {
        size_t read = codec_->ReadPcm(dma_rx_pcm_, rx_samples, 100);
        if (read == 0) {
            continue;
        }
        AfeStepResult step;
        if (!ProcessCapturedPcm(dma_rx_pcm_, read, &step)) {
            continue;
        }
        if (!uplink_enabled_.load()) {
            continue;
        }
        if (step.vad_started && !step.pre_roll_pcm.empty()) {
            encode_and_queue(step.pre_roll_pcm.data(), step.pre_roll_pcm.size());
        }
        if (!step.clean_pcm.empty()) {
            encode_and_queue(step.clean_pcm.data(), step.clean_pcm.size());
        }
    }
}

void AudioEngine::RunOutputLoop() {
    const size_t tx_bytes = ((cfg_.downlink_sample_rate * cfg_.frame_ms) / 1000) * sizeof(int16_t);
    std::vector<int16_t> ref_16k;
    while (running_.load()) {
        std::vector<uint8_t> pkt;
        if (!decode_queue_->Pop(&pkt)) {
            const float prev = spk_rms_.load(std::memory_order_relaxed);
            if (prev > 0.005f) {
                spk_rms_.store(prev * 0.72f, std::memory_order_relaxed);
            } else {
                spk_rms_.store(0.0f, std::memory_order_relaxed);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = pkt.data();
        raw.len = static_cast<uint32_t>(pkt.size());
        raw.consumed = 0;
        raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;

        esp_audio_dec_out_frame_t out_frame = {};
        out_frame.buffer = reinterpret_cast<uint8_t*>(dma_tx_pcm_);
        out_frame.len = static_cast<uint32_t>(tx_bytes);
        out_frame.decoded_size = 0;

        if (esp_opus_dec_decode(opus_dec_, &raw, &out_frame, nullptr) == ESP_AUDIO_ERR_OK &&
            out_frame.decoded_size > 0) {
            const size_t decoded_samples = out_frame.decoded_size / sizeof(int16_t);
            const float spk_lvl = ComputeNormalizedPcmRms(dma_tx_pcm_, decoded_samples);
            const float prev = spk_rms_.load(std::memory_order_relaxed);
            spk_rms_.store(prev * 0.3f + spk_lvl * 0.7f, std::memory_order_relaxed);
            codec_->WritePcm(dma_tx_pcm_, decoded_samples, 100);
            if (cfg_.afe.enable_aec && aec_ref_ring_) {
                Downsample24kTo16kLinear(dma_tx_pcm_, decoded_samples, &ref_16k);
                aec_ref_ring_->PushSamples(ref_16k.data(), ref_16k.size());
            }
        }
    }
}
#endif

} // namespace esp_pika
