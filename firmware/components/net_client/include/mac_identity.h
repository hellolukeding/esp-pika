#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace esp_pika {

/// Formats 6 raw MAC bytes into canonical lowercase colon-separated ASCII ("30:ed:a0:f3:dc:70").
std::string FormatMacAddress(const uint8_t mac[6]);

/// Validates and normalizes an input MAC string ("AA-BB-CC-DD-EE-FF" or "aa:bb:cc:dd:ee:ff").
bool NormalizeMacAddress(const std::string& raw, std::string* out);

/// Reads the factory-burned eFuse default MAC address on ESP32-S31.
/// Returns false if unavailable (e.g. non-ESP host builds) or all-zero.
bool ReadFactoryEfuseMac(std::string* out_mac);

} // namespace esp_pika
