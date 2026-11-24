#pragma once

#include <cstddef>
#include <cstdint>

namespace esp_pika {

constexpr uint32_t kSystemChimeSampleRate = 16000;

/// Returns 16kHz mono signed 16-bit PCM samples for the boot/wake confirmation chime.
const int16_t* GetSystemChimePcm16k(size_t* sample_count);

} // namespace esp_pika
