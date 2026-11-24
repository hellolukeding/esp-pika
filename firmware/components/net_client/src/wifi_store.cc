#include "wifi_store.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#endif

namespace esp_pika {
namespace {

constexpr uint32_t kBlobMagic = 0x57494649U; // "WIFI"
constexpr uint16_t kBlobFormatVersion = 1;
constexpr size_t kMaxSsidBytes = 32;
constexpr size_t kMaxPasswordBytes = 64;
constexpr size_t kMaxHostBytes = 128;

#ifdef ESP_PLATFORM
constexpr const char* kTag = "wifi_store";
constexpr const char* kNvsNamespace = "wifi_store";
constexpr const char* kNvsKeyBlob = "cfg_bin";
#endif

uint32_t Fnv1a32(const uint8_t* data, size_t len) {
    uint32_t h = 2166136261U;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 16777619U;
    }
    return h;
}

template <typename T>
void AppendPod(std::vector<uint8_t>* dst, const T& val) {
    const auto* raw = reinterpret_cast<const uint8_t*>(&val);
    dst->insert(dst->end(), raw, raw + sizeof(T));
}

void AppendString(std::vector<uint8_t>* dst, const std::string& s) {
    const uint16_t len = static_cast<uint16_t>(std::min<size_t>(s.size(), 255));
    AppendPod(dst, len);
    dst->insert(dst->end(), s.begin(), s.begin() + len);
}

template <typename T>
bool ReadPod(const uint8_t* data, size_t total, size_t* offset, T* out) {
    if (*offset + sizeof(T) > total) {
        return false;
    }
    std::memcpy(out, data + *offset, sizeof(T));
    *offset += sizeof(T);
    return true;
}

bool ReadString(const uint8_t* data, size_t total, size_t* offset, size_t max_len, std::string* out) {
    uint16_t len = 0;
    if (!ReadPod(data, total, offset, &len)) {
        return false;
    }
    if (len > max_len || *offset + len > total) {
        return false;
    }
    out->assign(reinterpret_cast<const char*>(data + *offset), len);
    *offset += len;
    return true;
}

std::string TrimCopy(const std::string& raw) {
    size_t start = 0;
    while (start < raw.size() && std::isspace(static_cast<unsigned char>(raw[start]))) {
        ++start;
    }
    size_t end = raw.size();
    while (end > start && std::isspace(static_cast<unsigned char>(raw[end - 1]))) {
        --end;
    }
    return raw.substr(start, end - start);
}

bool IsValidCredential(const std::string& ssid, const std::string& password) {
    return !ssid.empty() && ssid.size() <= kMaxSsidBytes && password.size() <= kMaxPasswordBytes;
}

} // namespace

void WifiStore::SortLocked() {
    std::sort(credentials_.begin(), credentials_.end(),
              [](const WifiCredential& a, const WifiCredential& b) {
                  if (a.last_used_seq != b.last_used_seq) {
                      return a.last_used_seq > b.last_used_seq;
                  }
                  return a.ssid < b.ssid;
              });
}

bool WifiStore::Upsert(const std::string& ssid, const std::string& password, bool mark_used) {
    if (!IsValidCredential(ssid, password)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& existing : credentials_) {
        if (existing.ssid == ssid) {
            existing.password = password;
            if (mark_used) {
                existing.last_used_seq = next_seq_++;
            }
            SortLocked();
            return true;
        }
    }
    if (credentials_.size() >= kMaxCredentials) {
        SortLocked();
        credentials_.pop_back();
    }
    WifiCredential cred;
    cred.ssid = ssid;
    cred.password = password;
    cred.last_used_seq = mark_used ? (next_seq_++) : 0U;
    credentials_.push_back(std::move(cred));
    SortLocked();
    return true;
}

bool WifiStore::MarkConnected(const std::string& ssid) {
    if (ssid.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& existing : credentials_) {
        if (existing.ssid == ssid) {
            existing.last_used_seq = next_seq_++;
            SortLocked();
            return true;
        }
    }
    return false;
}

bool WifiStore::Remove(const std::string& ssid) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::remove_if(credentials_.begin(), credentials_.end(),
                             [&](const WifiCredential& c) { return c.ssid == ssid; });
    if (it == credentials_.end()) {
        return false;
    }
    credentials_.erase(it, credentials_.end());
    return true;
}

bool WifiStore::Find(const std::string& ssid, WifiCredential* out) const {
    if (out == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& c : credentials_) {
        if (c.ssid == ssid) {
            *out = c;
            return true;
        }
    }
    return false;
}

std::vector<WifiCredential> WifiStore::List() const {
    std::lock_guard<std::mutex> lock(mu_);
    return credentials_;
}

bool WifiStore::Empty() const {
    std::lock_guard<std::mutex> lock(mu_);
    return credentials_.empty();
}

size_t WifiStore::Count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return credentials_.size();
}

bool WifiStore::SeedFromDefaults(const std::string& ssid, const std::string& password) {
    if (!IsValidCredential(ssid, password)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (!credentials_.empty()) {
        return false;
    }
    WifiCredential cred;
    cred.ssid = ssid;
    cred.password = password;
    cred.last_used_seq = next_seq_++;
    credentials_.push_back(std::move(cred));
    return true;
}

void WifiStore::SetGatewayOverride(const std::string& host, uint16_t port) {
    std::lock_guard<std::mutex> lock(mu_);
    std::string trimmed = TrimCopy(host);
    if (trimmed.size() > kMaxHostBytes) {
        trimmed.resize(kMaxHostBytes);
    }
    if (trimmed.empty()) {
        gateway_override_ = WifiGatewayOverride{};
        return;
    }
    gateway_override_.enabled = true;
    gateway_override_.host = std::move(trimmed);
    gateway_override_.port = (port == 0) ? 8080 : port;
}

void WifiStore::ClearGatewayOverride() {
    std::lock_guard<std::mutex> lock(mu_);
    gateway_override_ = WifiGatewayOverride{};
}

WifiGatewayOverride WifiStore::GatewayOverride() const {
    std::lock_guard<std::mutex> lock(mu_);
    return gateway_override_;
}

bool WifiStore::Serialize(std::vector<uint8_t>* out) const {
    if (out == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    out->clear();
    AppendPod(out, kBlobMagic);
    AppendPod(out, kBlobFormatVersion);
    AppendPod(out, next_seq_);
    const uint8_t gw_enabled = gateway_override_.enabled ? 1U : 0U;
    AppendPod(out, gw_enabled);
    AppendString(out, gateway_override_.host);
    AppendPod(out, gateway_override_.port);

    const uint16_t count = static_cast<uint16_t>(credentials_.size());
    AppendPod(out, count);
    for (const auto& cred : credentials_) {
        AppendString(out, cred.ssid);
        AppendString(out, cred.password);
        AppendPod(out, cred.last_used_seq);
    }

    const uint32_t checksum = Fnv1a32(out->data(), out->size());
    AppendPod(out, checksum);
    return true;
}

bool WifiStore::Deserialize(const uint8_t* data, size_t len) {
    if (data == nullptr || len < sizeof(kBlobMagic) + sizeof(kBlobFormatVersion) + sizeof(uint32_t)) {
        return false;
    }
    const size_t payload_len = len - sizeof(uint32_t);
    uint32_t expected_crc = 0;
    std::memcpy(&expected_crc, data + payload_len, sizeof(uint32_t));
    if (Fnv1a32(data, payload_len) != expected_crc) {
        return false;
    }

    size_t offset = 0;
    uint32_t magic = 0;
    uint16_t fmt_ver = 0;
    uint32_t parsed_next_seq = 1;
    uint8_t gw_enabled = 0;
    WifiGatewayOverride parsed_gw;
    uint16_t count = 0;
    if (!ReadPod(data, payload_len, &offset, &magic) || magic != kBlobMagic ||
        !ReadPod(data, payload_len, &offset, &fmt_ver) || fmt_ver != kBlobFormatVersion ||
        !ReadPod(data, payload_len, &offset, &parsed_next_seq) ||
        !ReadPod(data, payload_len, &offset, &gw_enabled) ||
        !ReadString(data, payload_len, &offset, kMaxHostBytes, &parsed_gw.host) ||
        !ReadPod(data, payload_len, &offset, &parsed_gw.port) ||
        !ReadPod(data, payload_len, &offset, &count) || count > kMaxCredentials) {
        return false;
    }
    parsed_gw.enabled = (gw_enabled != 0) && !parsed_gw.host.empty();
    if (parsed_gw.port == 0) {
        parsed_gw.port = 8080;
    }

    std::vector<WifiCredential> parsed;
    parsed.reserve(count);
    uint32_t max_seq = 0;
    for (uint16_t i = 0; i < count; ++i) {
        WifiCredential cred;
        if (!ReadString(data, payload_len, &offset, kMaxSsidBytes, &cred.ssid) ||
            !ReadString(data, payload_len, &offset, kMaxPasswordBytes, &cred.password) ||
            !ReadPod(data, payload_len, &offset, &cred.last_used_seq) ||
            !IsValidCredential(cred.ssid, cred.password)) {
            return false;
        }
        max_seq = std::max(max_seq, cred.last_used_seq);
        parsed.push_back(std::move(cred));
    }
    if (offset != payload_len) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mu_);
    credentials_ = std::move(parsed);
    gateway_override_ = std::move(parsed_gw);
    next_seq_ = std::max<uint32_t>(std::max<uint32_t>(parsed_next_seq, max_seq + 1U), 1U);
    SortLocked();
    return true;
}

bool WifiStore::LoadFromNvs() {
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(kTag, "no stored Wi-Fi config yet");
        return true;
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_open read failed: %s", esp_err_to_name(err));
        return false;
    }
    size_t required = 0;
    err = nvs_get_blob(handle, kNvsKeyBlob, nullptr, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND || required == 0) {
        nvs_close(handle);
        return true;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }
    std::vector<uint8_t> blob(required);
    err = nvs_get_blob(handle, kNvsKeyBlob, blob.data(), &required);
    nvs_close(handle);
    if (err != ESP_OK) {
        return false;
    }
    if (!Deserialize(blob.data(), required)) {
        ESP_LOGW(kTag, "Wi-Fi NVS blob checksum/format mismatch");
        return false;
    }
    ESP_LOGI(kTag, "loaded %zu Wi-Fi credentials from NVS", Count());
    return true;
#else
    return true;
#endif
}

bool WifiStore::SaveToNvs() const {
    std::vector<uint8_t> blob;
    if (!Serialize(&blob)) {
        return false;
    }
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open write failed: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_blob(handle, kNvsKeyBlob, blob.data(), blob.size());
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs commit failed: %s", esp_err_to_name(err));
        return false;
    }
#endif
    return true;
}

} // namespace esp_pika
