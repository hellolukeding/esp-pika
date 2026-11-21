#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "board.h"

namespace esp_pika {

enum class GatewayMsgType {
    kUnknown = 0,
    kDeviceWelcome,
    kWakeAccept,
    kWakeSuppress,
    kSessionRouted,
    kDeviceCommand,
    kVoiceprintSync,
    kPong,
    kError,
};

struct DeviceHelloPayload {
    std::string device_id;
    std::string mac;
    std::string group_id;
    std::string zone;
    std::string chip;
    uint64_t vp_version = 0;
    std::string firmware_version;
    std::vector<CapabilityDescriptor> capabilities;
};

struct WakeDetectedPayload {
    std::string group_id;
    std::string zone;
    std::string wake_word;
    float wake_score = 0.0f;
    float snr_db = 0.0f;
    int64_t detected_at_ms = 0;
};

struct ShadowUpdatePayload {
    std::optional<int32_t> battery_percent;
    std::optional<int32_t> rssi;
    std::optional<int32_t> volume;
    std::string vad_state;
};

struct CommandResultPayload {
    std::string command_id;
    bool ok = false;
    std::string error_code;
    std::string error_message;
    std::string result_json;
    std::string media_url;
};

struct GatewayCommand {
    std::string command_id;
    std::string capability;
    std::string action;
    std::string params_json;
    int32_t timeout_ms = 5000;
};

struct ParsedGatewayMessage {
    GatewayMsgType type = GatewayMsgType::kUnknown;
    std::string raw_type;
    std::string device_id;
    std::string group_id;
    std::string zone;
    std::string winner_device_id;
    std::string session_id;
    std::string agent_id;
    std::string error_code;
    uint64_t vp_version = 0;
    GatewayCommand command;
};

std::string EscapeJsonString(const std::string& input);

std::string BuildDeviceHelloJson(const DeviceHelloPayload& payload);
std::string BuildWakeDetectedJson(const WakeDetectedPayload& payload);
std::string BuildSpeechStartJson(const std::string& speaker_id, const std::string& agent_id);
std::string BuildShadowUpdateJson(const ShadowUpdatePayload& payload);
std::string BuildCommandResultJson(const CommandResultPayload& payload);
std::string BuildPingJson();

bool ParseGatewayMessage(const std::string& json_text, ParsedGatewayMessage* out);
bool ParseMediaUploadResponse(const std::string& json_text, std::string* out_media_url);

/// Extracts a raw JSON value slice (object `{...}` or array `[...]`) for a top-level key.
bool ExtractJsonRawField(const std::string& json_text, const std::string& key, std::string* out);

/// Extracts a boolean field from a flat JSON object.
bool ExtractJsonBoolField(const std::string& json_text, const std::string& key, bool* out);

/// Extracts a string field from a flat JSON object (used for command params_json).
bool ExtractJsonStringField(const std::string& json_text, const std::string& key, std::string* out);

/// Splits a top-level JSON array `[elem1, elem2, ...]` into raw trimmed element slices.
bool SplitJsonArrayElements(const std::string& array_json, std::vector<std::string>* out);

/// Extracts an integer field from a flat JSON object.
bool ExtractJsonIntField(const std::string& json_text, const std::string& key, int64_t* out);

} // namespace esp_pika
