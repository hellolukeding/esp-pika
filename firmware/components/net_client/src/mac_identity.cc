#include "mac_identity.h"

#include <cctype>
#include <cstdio>

#ifdef ESP_PLATFORM
#include "esp_mac.h"
#endif

namespace esp_pika {

std::string FormatMacAddress(const uint8_t mac[6]) {
    if (mac == nullptr) {
        return {};
    }
    char buf[18] = {};
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(buf);
}

bool NormalizeMacAddress(const std::string& raw, std::string* out) {
    if (out == nullptr) {
        return false;
    }
    std::string hex;
    hex.reserve(12);
    for (char ch : raw) {
        if (ch == ':' || ch == '-' || std::isspace(static_cast<unsigned char>(ch))) {
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
        hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (hex.size() != 12) {
        return false;
    }
    std::string formatted;
    formatted.reserve(17);
    for (size_t i = 0; i < 12; i += 2) {
        if (i > 0) {
            formatted.push_back(':');
        }
        formatted.push_back(hex[i]);
        formatted.push_back(hex[i + 1]);
    }
    *out = formatted;
    return true;
}

bool ReadFactoryEfuseMac(std::string* out_mac) {
    if (out_mac == nullptr) {
        return false;
    }
#ifdef ESP_PLATFORM
    // Read the 6-byte factory eFuse MAC directly; esp_efuse_mac_get_default() writes an
    // 8-byte EUI-64 on IEEE 802.15.4-capable targets (including ESP32-S31).
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY) != ESP_OK) {
        return false;
    }
    bool non_zero = false;
    for (uint8_t b : mac) {
        if (b != 0) {
            non_zero = true;
            break;
        }
    }
    if (!non_zero) {
        return false;
    }
    *out_mac = FormatMacAddress(mac);
    return true;
#else
    return false;
#endif
}

} // namespace esp_pika
