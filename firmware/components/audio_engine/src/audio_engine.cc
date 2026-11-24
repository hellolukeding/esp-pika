#include "audio_engine.h"
#include "chime_pcm.h"

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
#include "freertos/idf_additions.h"
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
    // RFC 6716 allows up to 120ms per Opus packet (e.g. DeepSeek WS TTS sends 100ms multi-frame packets).
    tx_max_samples_ = std::max<size_t>((cfg_.downlink_sample_rate * 120) / 1000, 2880);

    // Allocate I2S DMA scratch buffers explicitly in internal DRAM per AGENTS.md §3.1.
    dma_rx_pcm_ = static_cast<int16_t*>(
        heap_caps_malloc(rx_samples * sizeof(int16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    dma_tx_pcm_ = static_cast<int16_t*>(
        heap_caps_malloc(tx_max_samples_ * sizeof(int16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
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
    dec_cfg.sample_rate = static_cast<int>(cfg_.uplink_sample_rate);
    dec_cfg.channel = 1;
    dec_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_120_MS;
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
    // Pin real-time audio tasks strictly to HP Core 0 per AGENTS.md §3.1;
    // esp_opus_enc (~40 KB) / esp_opus_dec (~20 KB) stacks live in PSRAM cleanly (no Flash writes here).
    BaseType_t ok_in = xTaskCreatePinnedToCoreWithCaps(
        &AudioEngine::AudioInputTaskEntry, "AudioInputTask", 40960, this, 6, &input_task_,
        kAudioCoreId, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    BaseType_t ok_out = xTaskCreatePinnedToCoreWithCaps(
        &AudioEngine::AudioOutputTaskEntry, "AudioOutputTask", 24576, this, 7, &output_task_,
        kAudioCoreId, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(kTag, "audio tasks created: in=%d out=%d",
             static_cast<int>(ok_in == pdPASS), static_cast<int>(ok_out == pdPASS));
    if (ok_in != pdPASS || ok_out != pdPASS) {
        ESP_LOGE(kTag, "xTaskCreatePinnedToCoreWithCaps failed (in=%d out=%d)",
                 static_cast<int>(ok_in), static_cast<int>(ok_out));
        Stop();
        return false;
    }
#endif
    return true;
}

void AudioEngine::Stop() {
    running_.store(false);
    wake_chime_playing_.store(false, std::memory_order_release);
    skip_uplink_samples_.store(0, std::memory_order_relaxed);
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
    afe_.SetWakeDetectionEnabled(!enabled);
}

void AudioEngine::HandleAfeStep(const AfeStepResult& step) {
    vad_speaking_.store(step.vad_speaking, std::memory_order_relaxed);
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
    if (!step->clean_pcm.empty()) {
        const float clean_inst =
            ComputeNormalizedPcmRms(step->clean_pcm.data(), step->clean_pcm.size());
        const float clean_prev = clean_mic_rms_.load(std::memory_order_relaxed);
        const float clean_smooth = (clean_inst > clean_prev)
                                       ? (clean_prev * 0.30f + clean_inst * 0.70f)
                                       : (clean_prev * 0.50f + clean_inst * 0.50f);
        clean_mic_rms_.store(clean_smooth, std::memory_order_relaxed);
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

void AudioEngine::PlayWakeChime() {
#ifdef ESP_PLATFORM
    if (running_.load(std::memory_order_relaxed)) {
        wake_chime_playing_.store(true, std::memory_order_release);
    }
#endif
    skip_uplink_samples_.store((cfg_.uplink_sample_rate * 80U) / 1000U, std::memory_order_relaxed);
    wake_chime_pending_.store(true, std::memory_order_release);
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
    const size_t chunk = (cfg_.uplink_sample_rate * cfg_.frame_ms) / 1000;
    std::vector<int16_t> uplink_pcm_buf;
    uint32_t sent_frames = 0;
    auto encode_one_chunk = [this, chunk, &sent_frames](const int16_t* pcm) {
        esp_audio_enc_in_frame_t in_frame = {};
        in_frame.buffer = reinterpret_cast<uint8_t*>(const_cast<int16_t*>(pcm));
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
            ++sent_frames;
        }
    };

    uint32_t loop_count = 0;
    while (running_.load()) {
        size_t read = codec_->ReadPcm(dma_rx_pcm_, rx_samples, 100);
        const bool log_diag = ((++loop_count % 50) == 0);
        if (read == 0) {
            if (log_diag) {
                ESP_LOGI(kTag, "mic diag: read=0 rms=%.4f min=0 max=0 uplink=%d sent_frames=%lu",
                         static_cast<double>(mic_rms_.load(std::memory_order_relaxed)),
                         uplink_enabled_.load() ? 1 : 0,
                         static_cast<unsigned long>(sent_frames));
            }
            vTaskDelay(pdMS_TO_TICKS(60));
            continue;
        }
        int16_t raw_min_s = dma_rx_pcm_[0];
        int16_t raw_max_s = dma_rx_pcm_[0];
        if (log_diag) {
            for (size_t i = 1; i < read; ++i) {
                if (dma_rx_pcm_[i] < raw_min_s) {
                    raw_min_s = dma_rx_pcm_[i];
                }
                if (dma_rx_pcm_[i] > raw_max_s) {
                    raw_max_s = dma_rx_pcm_[i];
                }
            }
        }
        const bool chime_playing = wake_chime_playing_.load(std::memory_order_acquire);
        const uint32_t skip_rem = skip_uplink_samples_.load(std::memory_order_relaxed);
        if (chime_playing || skip_rem > 0) {
            if (!chime_playing && skip_rem > 0) {
                const uint32_t dec = static_cast<uint32_t>(std::min<size_t>(read, skip_rem));
                skip_uplink_samples_.store(skip_rem - dec, std::memory_order_relaxed);
            }
            std::memset(dma_rx_pcm_, 0, read * sizeof(int16_t));
            AfeStepResult muted_step;
            ProcessCapturedPcm(dma_rx_pcm_, read, &muted_step);
            clean_mic_rms_.store(0.0f, std::memory_order_relaxed);
            if (log_diag) {
                ESP_LOGI(kTag, "mic diag: read=%u rms=%.4f clean=%.4f vad=0 min=%d max=%d uplink=%d sent_frames=%lu",
                         static_cast<unsigned>(read),
                         static_cast<double>(mic_rms_.load(std::memory_order_relaxed)),
                         static_cast<double>(clean_mic_rms_.load(std::memory_order_relaxed)),
                         static_cast<int>(raw_min_s),
                         static_cast<int>(raw_max_s),
                         uplink_enabled_.load() ? 1 : 0,
                         static_cast<unsigned long>(sent_frames));
            }
            uplink_pcm_buf.clear();
            continue;
        }
        const bool speaker_playing =
            (decode_queue_ != nullptr && decode_queue_->Size() > 0) ||
            (spk_rms_.load(std::memory_order_relaxed) > 0.01f);
        if (speaker_playing && !uplink_enabled_.load(std::memory_order_relaxed)) {
            afe_.SetWakeDetectionEnabled(false);
            mic_rms_.store(0.0f, std::memory_order_relaxed);
            clean_mic_rms_.store(0.0f, std::memory_order_relaxed);
            uplink_pcm_buf.clear();
            if (log_diag) {
                ESP_LOGI(kTag, "mic diag: read=%u rms=0.0000 clean=0.0000 vad=0 min=%d max=%d uplink=0 sent_frames=%lu",
                         static_cast<unsigned>(read),
                         static_cast<int>(raw_min_s),
                         static_cast<int>(raw_max_s),
                         static_cast<unsigned long>(sent_frames));
            }
            continue;
        }
        afe_.SetWakeDetectionEnabled(!uplink_enabled_.load(std::memory_order_relaxed) && !speaker_playing);
        AfeStepResult step;
        if (!ProcessCapturedPcm(dma_rx_pcm_, read, &step)) {
            continue;
        }
        if (log_diag) {
            ESP_LOGI(kTag, "mic diag: read=%u rms=%.4f clean=%.4f vad=%d min=%d max=%d uplink=%d sent_frames=%lu",
                     static_cast<unsigned>(read),
                     static_cast<double>(mic_rms_.load(std::memory_order_relaxed)),
                     static_cast<double>(clean_mic_rms_.load(std::memory_order_relaxed)),
                     step.vad_speaking ? 1 : 0,
                     static_cast<int>(raw_min_s),
                     static_cast<int>(raw_max_s),
                     uplink_enabled_.load() ? 1 : 0,
                     static_cast<unsigned long>(sent_frames));
        }
        if (!uplink_enabled_.load()) {
            uplink_pcm_buf.clear();
            continue;
        }
        if (step.vad_started && !step.pre_roll_pcm.empty()) {
            uplink_pcm_buf.insert(uplink_pcm_buf.end(), step.pre_roll_pcm.begin(),
                                  step.pre_roll_pcm.end());
        }
        if (!step.clean_pcm.empty()) {
            uplink_pcm_buf.insert(uplink_pcm_buf.end(), step.clean_pcm.begin(),
                                  step.clean_pcm.end());
        }
        bool encoded = false;
        while (uplink_pcm_buf.size() >= chunk) {
            encode_one_chunk(uplink_pcm_buf.data());
            uplink_pcm_buf.erase(uplink_pcm_buf.begin(),
                                 uplink_pcm_buf.begin() + static_cast<std::ptrdiff_t>(chunk));
            encoded = true;
        }
        if (encoded && on_uplink_opus_) {
            FlushUplink(on_uplink_opus_);
        }
    }
}

void AudioEngine::RunOutputLoop() {
    const size_t tx_bytes = tx_max_samples_ * sizeof(int16_t);
    bool logged_first_decode = false;
    int decode_err_logs = 0;
    bool playing_downlink = false;
    int16_t last_tx_sample = 0;
    int idle_ticks = 0;
    int prebuf_ticks = 0;
    while (running_.load()) {
        if (wake_chime_pending_.exchange(false, std::memory_order_acq_rel)) {
            if (decode_queue_->Size() == 0) {
                playing_downlink = false;
                last_tx_sample = 0;
                idle_ticks = 0;
                prebuf_ticks = 0;
                size_t total_samples = 0;
                const int16_t* pcm = GetSystemChimePcm16k(&total_samples);
                const size_t max_chunk = std::min<size_t>(320, tx_max_samples_);
                if (pcm != nullptr && total_samples > 0 && max_chunk > 0) {
                    size_t offset = 0;
                    while (offset < total_samples && running_.load()) {
                        if (wake_chime_pending_.exchange(false, std::memory_order_acq_rel)) {
                            offset = 0;
                        }
                        if (decode_queue_->Size() > 0) {
                            break;
                        }
                        const size_t n = std::min(max_chunk, total_samples - offset);
                        std::memcpy(dma_tx_pcm_, pcm + offset, n * sizeof(int16_t));
                        if (cfg_.afe.enable_aec && aec_ref_ring_) {
                            aec_ref_ring_->PushSamples(dma_tx_pcm_, n);
                        }
                        codec_->WritePcm(dma_tx_pcm_, n, 100);
                        spk_rms_.store(ComputeNormalizedPcmRms(dma_tx_pcm_, n),
                                       std::memory_order_relaxed);
                        offset += n;
                    }
                }
            } else {
                skip_uplink_samples_.store(0, std::memory_order_relaxed);
            }
            wake_chime_playing_.store(false, std::memory_order_release);
        }
        if (!playing_downlink) {
            const size_t q_sz = decode_queue_->Size();
            if (q_sz == 0) {
                const float prev = spk_rms_.load(std::memory_order_relaxed);
                spk_rms_.store((prev > 0.005f) ? (prev * 0.72f) : 0.0f, std::memory_order_relaxed);
                if (idle_ticks < 100) {
                    ++idle_ticks;
                }
                prebuf_ticks = 0;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (q_sz < 6 && prebuf_ticks < 12) {
                ++prebuf_ticks;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (idle_ticks >= 25 && opus_dec_ != nullptr) {
                esp_opus_dec_reset(opus_dec_);
            }
        }
        std::vector<uint8_t> pkt;
        if (!decode_queue_->Pop(&pkt)) {
            if (playing_downlink) {
                constexpr size_t kRampSamples = 64;
                constexpr size_t kTailZeros = 160;
                const size_t ramp_out_samples = std::min<size_t>(kRampSamples + kTailZeros, tx_max_samples_);
                const size_t ramp_n = std::min<size_t>(kRampSamples, ramp_out_samples);
                for (size_t i = 0; i < ramp_n; ++i) {
                    const float gain = 1.0f - (static_cast<float>(i + 1) / static_cast<float>(ramp_n));
                    dma_tx_pcm_[i] = static_cast<int16_t>(static_cast<float>(last_tx_sample) * gain);
                }
                if (ramp_out_samples > ramp_n) {
                    std::memset(dma_tx_pcm_ + ramp_n, 0,
                                (ramp_out_samples - ramp_n) * sizeof(int16_t));
                }
                codec_->WritePcm(dma_tx_pcm_, ramp_out_samples, 100);
                if (cfg_.afe.enable_aec && aec_ref_ring_) {
                    aec_ref_ring_->PushSamples(dma_tx_pcm_, ramp_out_samples);
                }
                playing_downlink = false;
                last_tx_sample = 0;
                prebuf_ticks = 0;
                idle_ticks = 0;
            }
            const float prev = spk_rms_.load(std::memory_order_relaxed);
            spk_rms_.store((prev > 0.005f) ? (prev * 0.72f) : 0.0f, std::memory_order_relaxed);
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

        esp_audio_dec_info_t dec_info = {};
        esp_audio_err_t dec_err = esp_opus_dec_decode(opus_dec_, &raw, &out_frame, &dec_info);
        if (dec_err == ESP_AUDIO_ERR_OK && out_frame.decoded_size > 0) {
            const size_t decoded_samples = out_frame.decoded_size / sizeof(int16_t);
            if (!logged_first_decode) {
                ESP_LOGI(kTag, "decoded downlink opus frame: raw=%u pcm_samples=%u sr=%lu ch=%u",
                         static_cast<unsigned>(pkt.size()), static_cast<unsigned>(decoded_samples),
                         static_cast<unsigned long>(dec_info.sample_rate),
                         static_cast<unsigned>(dec_info.channel));
                logged_first_decode = true;
            }
            if (!playing_downlink) {
                const size_t fade_n = std::min<size_t>(64, decoded_samples);
                for (size_t i = 0; i < fade_n; ++i) {
                    const float gain = static_cast<float>(i) / static_cast<float>(fade_n);
                    dma_tx_pcm_[i] =
                        static_cast<int16_t>(static_cast<float>(dma_tx_pcm_[i]) * gain);
                }
                playing_downlink = true;
                idle_ticks = 0;
                prebuf_ticks = 0;
            }
            last_tx_sample = dma_tx_pcm_[decoded_samples - 1];
            const float spk_lvl = ComputeNormalizedPcmRms(dma_tx_pcm_, decoded_samples);
            const float prev = spk_rms_.load(std::memory_order_relaxed);
            spk_rms_.store(prev * 0.3f + spk_lvl * 0.7f, std::memory_order_relaxed);
            codec_->WritePcm(dma_tx_pcm_, decoded_samples, 100);
            if (cfg_.afe.enable_aec && aec_ref_ring_) {
                aec_ref_ring_->PushSamples(dma_tx_pcm_, decoded_samples);
            }
        } else if (decode_err_logs < 5) {
            ++decode_err_logs;
            ESP_LOGW(kTag, "esp_opus_dec_decode failed err=%d raw_len=%u decoded_size=%u",
                     static_cast<int>(dec_err), static_cast<unsigned>(raw.len),
                     static_cast<unsigned>(out_frame.decoded_size));
        }
    }
}
#endif

} // namespace esp_pika
