#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace esp_pika {

/// Bounded thread-safe frame queue implementing drop-oldest backpressure for audio streams.
/// Explicitly allocates ring storage and frame payloads in external PSRAM on ESP32 targets
/// (per AGENTS.md §3.1) so 20ms PCM16/Opus queues never deplete DMA-capable internal DRAM.
class BoundedFrameQueue {
public:
    /// Move-only byte buffer backed by PSRAM (`MALLOC_CAP_SPIRAM`) on ESP-IDF and `malloc` on host.
    class PsramFrame {
    public:
        PsramFrame() = default;
        ~PsramFrame() { Reset(); }

        PsramFrame(const PsramFrame&) = delete;
        PsramFrame& operator=(const PsramFrame&) = delete;

        PsramFrame(PsramFrame&& other) noexcept : data_(other.data_), size_(other.size_) {
            other.data_ = nullptr;
            other.size_ = 0;
        }

        PsramFrame& operator=(PsramFrame&& other) noexcept {
            if (this != &other) {
                Reset();
                data_ = other.data_;
                size_ = other.size_;
                other.data_ = nullptr;
                other.size_ = 0;
            }
            return *this;
        }

        bool Assign(const uint8_t* src, size_t len) {
            Reset();
            if (len == 0) {
                return true;
            }
            if (src == nullptr) {
                return false;
            }
            uint8_t* buf = AllocateBytes(len);
            if (buf == nullptr) {
                return false;
            }
            std::memcpy(buf, src, len);
            data_ = buf;
            size_ = len;
            return true;
        }

        void Reset() {
            if (data_ != nullptr) {
                FreeBytes(data_);
                data_ = nullptr;
                size_ = 0;
            }
        }

        const uint8_t* data() const { return data_; }
        size_t size() const { return size_; }
        bool empty() const { return size_ == 0 || data_ == nullptr; }

    private:
        static uint8_t* AllocateBytes(size_t len) {
#ifdef ESP_PLATFORM
            void* p = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (p == nullptr) {
                p = heap_caps_malloc(len, MALLOC_CAP_8BIT);
            }
            return static_cast<uint8_t*>(p);
#else
            return static_cast<uint8_t*>(std::malloc(len));
#endif
        }

        static void FreeBytes(uint8_t* ptr) {
#ifdef ESP_PLATFORM
            heap_caps_free(ptr);
#else
            std::free(ptr);
#endif
        }

        uint8_t* data_ = nullptr;
        size_t size_ = 0;
    };

    explicit BoundedFrameQueue(size_t max_frames)
        : max_frames_(max_frames > 0 ? max_frames : 1), slots_(max_frames_) {}

    /// Enqueues raw frame bytes directly into PSRAM without creating temporary DRAM vectors.
    /// If full, drops the oldest frame and returns false to signal backpressure drop.
    bool PushBytesDropOldest(const uint8_t* data, size_t len) {
        PsramFrame frame;
        if (!frame.Assign(data, len)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mu_);
        bool dropped = false;
        if (count_ >= max_frames_) {
            slots_[head_].Reset();
            head_ = (head_ + 1) % max_frames_;
            --count_;
            ++dropped_count_;
            dropped = true;
        }
        const size_t tail = (head_ + count_) % max_frames_;
        slots_[tail] = std::move(frame);
        ++count_;
        return !dropped;
    }

    /// Enqueues a vector frame; if full, drops the oldest frame and returns false to signal drop.
    bool PushDropOldest(const std::vector<uint8_t>& frame) {
        return PushBytesDropOldest(frame.data(), frame.size());
    }

    /// Pops the oldest PSRAM-backed frame without copying into internal DRAM.
    bool PopFrame(PsramFrame* out) {
        if (out == nullptr) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (count_ == 0) {
            return false;
        }
        *out = std::move(slots_[head_]);
        head_ = (head_ + 1) % max_frames_;
        --count_;
        return true;
    }

    bool Pop(std::vector<uint8_t>* out) {
        if (out == nullptr) {
            return false;
        }
        PsramFrame frame;
        if (!PopFrame(&frame)) {
            return false;
        }
        if (frame.empty()) {
            out->clear();
        } else {
            out->assign(frame.data(), frame.data() + frame.size());
        }
        return true;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mu_);
        for (size_t i = 0; i < count_; ++i) {
            slots_[(head_ + i) % max_frames_].Reset();
        }
        head_ = 0;
        count_ = 0;
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return count_;
    }

    uint64_t DroppedCount() const {
        std::lock_guard<std::mutex> lock(mu_);
        return dropped_count_;
    }

private:
    size_t max_frames_;
    mutable std::mutex mu_;
    std::vector<PsramFrame> slots_;
    size_t head_ = 0;
    size_t count_ = 0;
    uint64_t dropped_count_ = 0;
};

} // namespace esp_pika
