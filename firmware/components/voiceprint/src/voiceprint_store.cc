#include "voiceprint_store.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "gateway_protocol.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#endif

namespace esp_pika {
namespace {

constexpr uint32_t kBlobMagic = 0x50565045U; // "EPVP"
constexpr uint16_t kBlobFormatVersion = 1;
constexpr size_t kMaxTemplates = 64;
constexpr size_t kMaxEmbeddingDim = 512;

#ifdef ESP_PLATFORM
constexpr const char* kTag = "vp_store";
constexpr const char* kNvsNamespace = "voiceprint";
constexpr const char* kNvsKeyBlob = "templates_bin";
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

bool ReadString(const uint8_t* data, size_t total, size_t* offset, std::string* out) {
    uint16_t len = 0;
    if (!ReadPod(data, total, offset, &len)) {
        return false;
    }
    if (*offset + len > total) {
        return false;
    }
    out->assign(reinterpret_cast<const char*>(data + *offset), len);
    *offset += len;
    return true;
}

} // namespace

const char* SpeakerRoleName(SpeakerRole role) {
    switch (role) {
    case SpeakerRole::kOwner: return "owner";
    case SpeakerRole::kMember: return "member";
    case SpeakerRole::kGuest: return "guest";
    case SpeakerRole::kUnspecified:
    default:
        return "unspecified";
    }
}

bool VoiceprintStore::UpsertUnlocked(VoiceprintTemplate tpl) {
    if (tpl.speaker_id.empty() || tpl.embedding.empty() || tpl.embedding.size() > kMaxEmbeddingDim) {
        return false;
    }
    for (auto& existing : templates_) {
        if (existing.speaker_id == tpl.speaker_id) {
            existing = std::move(tpl);
            if (existing.version > vp_version_) {
                vp_version_ = existing.version;
            }
            return true;
        }
    }
    if (templates_.size() >= kMaxTemplates) {
        return false;
    }
    if (tpl.version > vp_version_) {
        vp_version_ = tpl.version;
    }
    templates_.push_back(std::move(tpl));
    return true;
}

bool VoiceprintStore::Upsert(VoiceprintTemplate tpl) {
    std::lock_guard<std::mutex> lock(mu_);
    return UpsertUnlocked(std::move(tpl));
}

bool VoiceprintStore::Delete(const std::string& speaker_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::remove_if(templates_.begin(), templates_.end(),
                             [&](const VoiceprintTemplate& t) { return t.speaker_id == speaker_id; });
    if (it == templates_.end()) {
        return false;
    }
    templates_.erase(it, templates_.end());
    return true;
}

bool VoiceprintStore::ApplySync(const VoiceprintSyncPayload& sync) {
    std::lock_guard<std::mutex> lock(mu_);
    if (sync.vp_version > 0 && sync.vp_version < vp_version_) {
        return false;
    }
    if (sync.full_sync) {
        templates_.clear();
    } else {
        for (const auto& del_id : sync.deleted_speaker_ids) {
            templates_.erase(
                std::remove_if(templates_.begin(), templates_.end(),
                               [&](const VoiceprintTemplate& t) { return t.speaker_id == del_id; }),
                templates_.end());
        }
    }
    for (const auto& tpl : sync.upserts) {
        UpsertUnlocked(tpl);
    }
    if (sync.vp_version > vp_version_) {
        vp_version_ = sync.vp_version;
    }
    return true;
}

std::vector<VoiceprintTemplate> VoiceprintStore::Snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return templates_;
}

size_t VoiceprintStore::Count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return templates_.size();
}

uint64_t VoiceprintStore::Version() const {
    std::lock_guard<std::mutex> lock(mu_);
    return vp_version_;
}

bool VoiceprintStore::Serialize(std::vector<uint8_t>* out) const {
    if (out == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    out->clear();
    AppendPod(out, kBlobMagic);
    AppendPod(out, kBlobFormatVersion);
    AppendPod(out, vp_version_);
    const uint16_t count = static_cast<uint16_t>(templates_.size());
    AppendPod(out, count);

    for (const auto& tpl : templates_) {
        AppendString(out, tpl.speaker_id);
        AppendString(out, tpl.display_name);
        const int32_t role_i32 = static_cast<int32_t>(tpl.role);
        AppendPod(out, role_i32);
        AppendPod(out, tpl.version);
        const uint16_t dim = static_cast<uint16_t>(tpl.embedding.size());
        AppendPod(out, dim);
        for (float v : tpl.embedding) {
            AppendPod(out, v);
        }
    }

    const uint32_t checksum = Fnv1a32(out->data(), out->size());
    AppendPod(out, checksum);
    return true;
}

bool VoiceprintStore::Deserialize(const uint8_t* data, size_t len) {
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
    uint64_t new_vp_ver = 0;
    uint16_t count = 0;
    if (!ReadPod(data, payload_len, &offset, &magic) || magic != kBlobMagic ||
        !ReadPod(data, payload_len, &offset, &fmt_ver) || fmt_ver != kBlobFormatVersion ||
        !ReadPod(data, payload_len, &offset, &new_vp_ver) ||
        !ReadPod(data, payload_len, &offset, &count) || count > kMaxTemplates) {
        return false;
    }

    std::vector<VoiceprintTemplate> parsed;
    parsed.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        VoiceprintTemplate tpl;
        int32_t role_i32 = 0;
        uint16_t dim = 0;
        if (!ReadString(data, payload_len, &offset, &tpl.speaker_id) ||
            !ReadString(data, payload_len, &offset, &tpl.display_name) ||
            !ReadPod(data, payload_len, &offset, &role_i32) ||
            !ReadPod(data, payload_len, &offset, &tpl.version) ||
            !ReadPod(data, payload_len, &offset, &dim) || dim == 0 || dim > kMaxEmbeddingDim) {
            return false;
        }
        tpl.role = static_cast<SpeakerRole>(role_i32);
        tpl.embedding.resize(dim);
        for (uint16_t d = 0; d < dim; ++d) {
            if (!ReadPod(data, payload_len, &offset, &tpl.embedding[d])) {
                return false;
            }
        }
        parsed.push_back(std::move(tpl));
    }
    if (offset != payload_len) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mu_);
    templates_ = std::move(parsed);
    vp_version_ = new_vp_ver;
    return true;
}

bool VoiceprintStore::LoadFromNvs() {
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(kTag, "no stored voiceprints yet");
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
        ESP_LOGW(kTag, "voiceprint NVS blob checksum/format mismatch");
        return false;
    }
    ESP_LOGI(kTag, "loaded %zu voiceprints (vp_version=%llu)", Count(),
             static_cast<unsigned long long>(Version()));
    return true;
#else
    return true;
#endif
}

bool VoiceprintStore::SaveToNvs() const {
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

bool ParseVoiceprintSyncJson(const std::string& json_text, VoiceprintSyncPayload* out) {
    if (out == nullptr) {
        return false;
    }
    *out = VoiceprintSyncPayload{};
    ExtractJsonStringField(json_text, "group_id", &out->group_id);
    int64_t vp_ver = 0;
    if (ExtractJsonIntField(json_text, "vp_version", &vp_ver) && vp_ver >= 0) {
        out->vp_version = static_cast<uint64_t>(vp_ver);
    }
    ExtractJsonBoolField(json_text, "full_sync", &out->full_sync);

    std::string upserts_raw;
    if (ExtractJsonRawField(json_text, "upserts", &upserts_raw)) {
        std::vector<std::string> items;
        if (SplitJsonArrayElements(upserts_raw, &items)) {
            for (const auto& item : items) {
                VoiceprintTemplate tpl;
                if (!ExtractJsonStringField(item, "speaker_id", &tpl.speaker_id) || tpl.speaker_id.empty()) {
                    continue;
                }
                ExtractJsonStringField(item, "display_name", &tpl.display_name);
                int64_t role_val = 2;
                if (ExtractJsonIntField(item, "role", &role_val)) {
                    tpl.role = static_cast<SpeakerRole>(role_val);
                }
                int64_t tpl_ver = 0;
                if (ExtractJsonIntField(item, "version", &tpl_ver) && tpl_ver >= 0) {
                    tpl.version = static_cast<uint64_t>(tpl_ver);
                }
                std::string emb_raw;
                std::vector<std::string> emb_elems;
                if (ExtractJsonRawField(item, "embedding", &emb_raw) &&
                    SplitJsonArrayElements(emb_raw, &emb_elems)) {
                    tpl.embedding.reserve(emb_elems.size());
                    for (const auto& num_s : emb_elems) {
                        tpl.embedding.push_back(std::strtof(num_s.c_str(), nullptr));
                    }
                }
                if (!tpl.embedding.empty()) {
                    out->upserts.push_back(std::move(tpl));
                }
            }
        }
    }

    std::string deleted_raw;
    if (ExtractJsonRawField(json_text, "deleted_speaker_ids", &deleted_raw)) {
        std::vector<std::string> items;
        if (SplitJsonArrayElements(deleted_raw, &items)) {
            for (const auto& elem : items) {
                if (elem.size() >= 2 && elem.front() == '"' && elem.back() == '"') {
                    out->deleted_speaker_ids.push_back(elem.substr(1, elem.size() - 2));
                }
            }
        }
    }
    return true;
}

} // namespace esp_pika
