#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace esp_pika {

/// Bounded thread-safe frame queue implementing drop-oldest backpressure for audio streams.
/// Prevents PSRAM OOM and unbounded latency buildup during weak-network bursts.
class BoundedFrameQueue {
public:
    explicit BoundedFrameQueue(size_t max_frames) : max_frames_(max_frames > 0 ? max_frames : 1) {}

    /// Enqueues a frame; if full, drops the oldest frame and returns false to signal drop.
    bool PushDropOldest(std::vector<uint8_t> frame) {
        std::lock_guard<std::mutex> lock(mu_);
        bool dropped = false;
        if (queue_.size() >= max_frames_) {
            queue_.pop_front();
            ++dropped_count_;
            dropped = true;
        }
        queue_.push_back(std::move(frame));
        return !dropped;
    }

    bool Pop(std::vector<uint8_t>* out) {
        if (out == nullptr) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (queue_.empty()) {
            return false;
        }
        *out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mu_);
        queue_.clear();
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return queue_.size();
    }

    uint64_t DroppedCount() const {
        std::lock_guard<std::mutex> lock(mu_);
        return dropped_count_;
    }

private:
    size_t max_frames_;
    mutable std::mutex mu_;
    std::deque<std::vector<uint8_t>> queue_;
    uint64_t dropped_count_ = 0;
};

} // namespace esp_pika
