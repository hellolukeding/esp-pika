#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace esp_pika {

/// Stored Wi-Fi STA credential ordered by most-recent successful use.
struct WifiCredential {
    std::string ssid;
    std::string password;
    uint32_t last_used_seq = 0;
};

/// Optional fallback gateway endpoint override configured via Captive Portal / NVS.
struct WifiGatewayOverride {
    bool enabled = false;
    std::string host;
    uint16_t port = 8080;
};

/// Multi-network Wi-Fi credential store backed by NVS (`wifi_store` namespace).
class WifiStore {
public:
    static constexpr size_t kMaxCredentials = 8;

    bool Upsert(const std::string& ssid, const std::string& password, bool mark_used = true);
    bool MarkConnected(const std::string& ssid);
    bool Remove(const std::string& ssid);
    bool Find(const std::string& ssid, WifiCredential* out) const;
    std::vector<WifiCredential> List() const;
    bool Empty() const;
    size_t Count() const;

    /// Seeds an initial credential only when the store is empty and ssid is non-empty.
    bool SeedFromDefaults(const std::string& ssid, const std::string& password);

    void SetGatewayOverride(const std::string& host, uint16_t port);
    void ClearGatewayOverride();
    WifiGatewayOverride GatewayOverride() const;

    bool Serialize(std::vector<uint8_t>* out) const;
    bool Deserialize(const uint8_t* data, size_t len);

    bool LoadFromNvs();
    bool SaveToNvs() const;

private:
    void SortLocked();

    mutable std::mutex mu_;
    std::vector<WifiCredential> credentials_;
    uint32_t next_seq_ = 1;
    WifiGatewayOverride gateway_override_;
};

} // namespace esp_pika
