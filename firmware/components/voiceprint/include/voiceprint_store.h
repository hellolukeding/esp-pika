#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace esp_pika {

/// Mirrors gateway biz.SpeakerRole values (0=unspecified, 1=owner, 2=member, 3=guest).
enum class SpeakerRole : int32_t {
    kUnspecified = 0,
    kOwner = 1,
    kMember = 2,
    kGuest = 3,
};

const char* SpeakerRoleName(SpeakerRole role);

struct VoiceprintTemplate {
    std::string speaker_id;
    std::string display_name;
    SpeakerRole role = SpeakerRole::kMember;
    std::vector<float> embedding;
    uint64_t version = 0;
};

struct VoiceprintSyncPayload {
    std::string group_id;
    uint64_t vp_version = 0;
    bool full_sync = false;
    std::vector<VoiceprintTemplate> upserts;
    std::vector<std::string> deleted_speaker_ids;
};

/// Thread-safe on-device voiceprint template store backed by encrypted NVS on ESP32-S31.
class VoiceprintStore {
public:
    VoiceprintStore() = default;

    /// Loads serialized voiceprint templates from encrypted NVS ("voiceprint" namespace).
    bool LoadFromNvs();

    /// Persists all templates atomically into encrypted NVS.
    /// Must only be called from a task whose stack resides in internal DRAM (AGENTS.md §3.1).
    bool SaveToNvs() const;

    bool Upsert(VoiceprintTemplate tpl);
    bool Delete(const std::string& speaker_id);
    bool ApplySync(const VoiceprintSyncPayload& sync);

    std::vector<VoiceprintTemplate> Snapshot() const;
    size_t Count() const;
    uint64_t Version() const;

    /// Deterministic binary serialization for NVS storage & host unit tests.
    bool Serialize(std::vector<uint8_t>* out) const;
    bool Deserialize(const uint8_t* data, size_t len);

private:
    bool UpsertUnlocked(VoiceprintTemplate tpl);

    mutable std::mutex mu_;
    std::vector<VoiceprintTemplate> templates_;
    uint64_t vp_version_ = 0;
};

/// Parses a gateway `voiceprint.sync` JSON control frame into VoiceprintSyncPayload.
bool ParseVoiceprintSyncJson(const std::string& json_text, VoiceprintSyncPayload* out);

} // namespace esp_pika
