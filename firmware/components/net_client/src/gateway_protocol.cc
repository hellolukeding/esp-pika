#include "gateway_protocol.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace esp_pika {
namespace {

void SkipWhitespace(const std::string& s, size_t* pos) {
    while (*pos < s.size() && std::isspace(static_cast<unsigned char>(s[*pos]))) {
        ++(*pos);
    }
}

void AppendUtf8Codepoint(uint32_t cp, std::string* out) {
    if (cp <= 0x7Fu) {
        out->push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out->push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out->push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out->push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out->push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

bool ParseHex4(const std::string& s, size_t* pos, uint32_t* out) {
    if (*pos + 4 > s.size()) {
        return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        char ch = s[*pos + static_cast<size_t>(i)];
        v <<= 4;
        if (ch >= '0' && ch <= '9') {
            v |= static_cast<uint32_t>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            v |= static_cast<uint32_t>(10 + (ch - 'a'));
        } else if (ch >= 'A' && ch <= 'F') {
            v |= static_cast<uint32_t>(10 + (ch - 'A'));
        } else {
            return false;
        }
    }
    *pos += 4;
    *out = v;
    return true;
}

bool ParseJsonStringAt(const std::string& s, size_t* pos, std::string* out) {
    if (*pos >= s.size() || s[*pos] != '"') {
        return false;
    }
    ++(*pos);
    std::string res;
    while (*pos < s.size()) {
        char c = s[(*pos)++];
        if (c == '"') {
            *out = res;
            return true;
        }
        if (c == '\\') {
            if (*pos >= s.size()) {
                return false;
            }
            char esc = s[(*pos)++];
            switch (esc) {
            case '"': res.push_back('"'); break;
            case '\\': res.push_back('\\'); break;
            case '/': res.push_back('/'); break;
            case 'b': res.push_back('\b'); break;
            case 'f': res.push_back('\f'); break;
            case 'n': res.push_back('\n'); break;
            case 'r': res.push_back('\r'); break;
            case 't': res.push_back('\t'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!ParseHex4(s, pos, &cp)) {
                    return false;
                }
                if (cp >= 0xD800u && cp <= 0xDBFFu && *pos + 2 <= s.size() &&
                    s[*pos] == '\\' && s[*pos + 1] == 'u') {
                    size_t next_pos = *pos + 2;
                    uint32_t low = 0;
                    if (ParseHex4(s, &next_pos, &low) && low >= 0xDC00u && low <= 0xDFFFu) {
                        *pos = next_pos;
                        cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                    }
                }
                AppendUtf8Codepoint(cp, &res);
                break;
            }
            default: res.push_back(esc); break;
            }
        } else {
            res.push_back(c);
        }
    }
    return false;
}

bool SkipJsonValueAt(const std::string& s, size_t* pos) {
    SkipWhitespace(s, pos);
    if (*pos >= s.size()) {
        return false;
    }
    char c = s[*pos];
    if (c == '"') {
        std::string tmp;
        return ParseJsonStringAt(s, pos, &tmp);
    }
    if (c == '{' || c == '[') {
        char open_ch = c;
        char close_ch = (c == '{') ? '}' : ']';
        int depth = 0;
        while (*pos < s.size()) {
            char cur = s[*pos];
            if (cur == '"') {
                std::string tmp;
                if (!ParseJsonStringAt(s, pos, &tmp)) {
                    return false;
                }
                continue;
            }
            if (cur == open_ch) {
                ++depth;
            } else if (cur == close_ch) {
                --depth;
                ++(*pos);
                if (depth == 0) {
                    return true;
                }
                continue;
            }
            ++(*pos);
        }
        return false;
    }
    while (*pos < s.size() && s[*pos] != ',' && s[*pos] != '}' && s[*pos] != ']' &&
           !std::isspace(static_cast<unsigned char>(s[*pos]))) {
        ++(*pos);
    }
    return true;
}

bool FindTopLevelValueSpan(const std::string& json_text,
                           const std::string& target_key,
                           size_t* val_start,
                           size_t* val_end) {
    size_t pos = 0;
    SkipWhitespace(json_text, &pos);
    if (pos >= json_text.size() || json_text[pos] != '{') {
        return false;
    }
    ++pos;
    while (pos < json_text.size()) {
        SkipWhitespace(json_text, &pos);
        if (pos >= json_text.size() || json_text[pos] == '}') {
            return false;
        }
        if (json_text[pos] == ',') {
            ++pos;
            continue;
        }
        std::string key;
        if (!ParseJsonStringAt(json_text, &pos, &key)) {
            return false;
        }
        SkipWhitespace(json_text, &pos);
        if (pos >= json_text.size() || json_text[pos] != ':') {
            return false;
        }
        ++pos;
        SkipWhitespace(json_text, &pos);
        size_t start = pos;
        if (!SkipJsonValueAt(json_text, &pos)) {
            return false;
        }
        size_t end = pos;
        if (key == target_key) {
            *val_start = start;
            *val_end = end;
            return true;
        }
    }
    return false;
}

std::string FormatFloat(float value) {
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "%.4f", static_cast<double>(value));
    return std::string(buf);
}

std::string FormatInt64(int64_t value) {
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
    return std::string(buf);
}

std::string FormatUint64(uint64_t value) {
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(value));
    return std::string(buf);
}

} // namespace

std::string EscapeJsonString(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (unsigned char c : input) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[7] = {};
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

std::string BuildDeviceHelloJson(const DeviceHelloPayload& payload) {
    std::string out = "{\"type\":\"device.hello\"";
    out += ",\"device_id\":\"" + EscapeJsonString(payload.device_id) + "\"";
    out += ",\"mac\":\"" + EscapeJsonString(payload.mac) + "\"";
    out += ",\"group_id\":\"" + EscapeJsonString(payload.group_id) + "\"";
    out += ",\"zone\":\"" + EscapeJsonString(payload.zone) + "\"";
    out += ",\"chip\":\"" + EscapeJsonString(payload.chip) + "\"";
    out += ",\"vp_version\":" + FormatUint64(payload.vp_version);
    out += ",\"firmware_version\":\"" + EscapeJsonString(payload.firmware_version) + "\"";
    out += ",\"capabilities\":[";
    for (size_t i = 0; i < payload.capabilities.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        const auto& cap = payload.capabilities[i];
        out += "{\"name\":\"" + EscapeJsonString(cap.name) + "\"";
        out += ",\"type\":\"" + EscapeJsonString(cap.kind) + "\"";
        if (!cap.upload.empty()) {
            out += ",\"upload_channel\":\"" + EscapeJsonString(cap.upload) + "\"";
        }
        if (!cap.fields.empty()) {
            out += ",\"fields\":[";
            for (size_t j = 0; j < cap.fields.size(); ++j) {
                if (j > 0) {
                    out.push_back(',');
                }
                out += "\"" + EscapeJsonString(cap.fields[j]) + "\"";
            }
            out.push_back(']');
        }
        out.push_back('}');
    }
    out += "]}";
    return out;
}

std::string BuildWakeDetectedJson(const WakeDetectedPayload& payload) {
    std::string out = "{\"type\":\"wake.detected\"";
    out += ",\"group_id\":\"" + EscapeJsonString(payload.group_id) + "\"";
    out += ",\"zone\":\"" + EscapeJsonString(payload.zone) + "\"";
    out += ",\"wake_word\":\"" + EscapeJsonString(payload.wake_word) + "\"";
    out += ",\"wake_score\":" + FormatFloat(payload.wake_score);
    out += ",\"snr_db\":" + FormatFloat(payload.snr_db);
    out += ",\"detected_at_ms\":" + FormatInt64(payload.detected_at_ms);
    out.push_back('}');
    return out;
}

std::string BuildSpeechStartJson(const std::string& speaker_id,
                                 const std::string& agent_id,
                                 int speaker_role,
                                 float confidence) {
    std::string out = "{\"type\":\"speech.start\"";
    out += ",\"speaker_id\":\"" + EscapeJsonString(speaker_id) + "\"";
    out += ",\"agent_id\":\"" + EscapeJsonString(agent_id) + "\"";
    out += ",\"speaker_role\":" + FormatInt64(speaker_role);
    out += ",\"confidence\":" + FormatFloat(confidence);
    out.push_back('}');
    return out;
}

std::string BuildSpeechStopJson(const std::string& turn_id) {
    std::string out = "{\"type\":\"speech.stop\"";
    if (!turn_id.empty()) {
        out += ",\"turn_id\":\"" + EscapeJsonString(turn_id) + "\"";
    }
    out.push_back('}');
    return out;
}

std::string BuildShadowUpdateJson(const ShadowUpdatePayload& payload) {
    std::string out = "{\"type\":\"shadow.update\"";
    if (payload.battery_percent.has_value()) {
        out += ",\"battery_percent\":" + FormatInt64(*payload.battery_percent);
    }
    if (payload.rssi.has_value()) {
        out += ",\"rssi\":" + FormatInt64(*payload.rssi);
    }
    if (payload.volume.has_value()) {
        out += ",\"volume\":" + FormatInt64(*payload.volume);
    }
    if (!payload.vad_state.empty()) {
        out += ",\"vad_state\":\"" + EscapeJsonString(payload.vad_state) + "\"";
    }
    out.push_back('}');
    return out;
}

std::string BuildCommandResultJson(const CommandResultPayload& payload) {
    std::string out = "{\"type\":\"command.result\"";
    out += ",\"command_id\":\"" + EscapeJsonString(payload.command_id) + "\"";
    out += std::string(",\"ok\":") + (payload.ok ? "true" : "false");
    if (!payload.error_code.empty()) {
        out += ",\"error_code\":\"" + EscapeJsonString(payload.error_code) + "\"";
    }
    if (!payload.error_message.empty()) {
        out += ",\"error_message\":\"" + EscapeJsonString(payload.error_message) + "\"";
    }
    if (!payload.result_json.empty()) {
        out += ",\"result_json\":\"" + EscapeJsonString(payload.result_json) + "\"";
    }
    if (!payload.media_url.empty()) {
        out += ",\"media_url\":\"" + EscapeJsonString(payload.media_url) + "\"";
    }
    out.push_back('}');
    return out;
}

std::string BuildPingJson() {
    return "{\"type\":\"ping\"}";
}

bool ExtractJsonRawField(const std::string& json_text, const std::string& key, std::string* out) {
    if (out == nullptr) {
        return false;
    }
    size_t start = 0;
    size_t end = 0;
    if (!FindTopLevelValueSpan(json_text, key, &start, &end) || start >= end) {
        return false;
    }
    *out = json_text.substr(start, end - start);
    return true;
}

bool ExtractJsonBoolField(const std::string& json_text, const std::string& key, bool* out) {
    if (out == nullptr) {
        return false;
    }
    size_t start = 0;
    size_t end = 0;
    if (!FindTopLevelValueSpan(json_text, key, &start, &end) || start >= end) {
        return false;
    }
    const std::string val = json_text.substr(start, end - start);
    if (val == "true") {
        *out = true;
        return true;
    }
    if (val == "false") {
        *out = false;
        return true;
    }
    return false;
}

bool ExtractJsonStringField(const std::string& json_text, const std::string& key, std::string* out) {
    if (out == nullptr) {
        return false;
    }
    size_t start = 0;
    size_t end = 0;
    if (!FindTopLevelValueSpan(json_text, key, &start, &end)) {
        return false;
    }
    if (start >= json_text.size() || json_text[start] != '"') {
        return false;
    }
    size_t pos = start;
    return ParseJsonStringAt(json_text, &pos, out);
}

bool SplitJsonArrayElements(const std::string& array_json, std::vector<std::string>* out) {
    if (out == nullptr) {
        return false;
    }
    out->clear();
    size_t pos = 0;
    SkipWhitespace(array_json, &pos);
    if (pos >= array_json.size() || array_json[pos] != '[') {
        return false;
    }
    ++pos;
    while (pos < array_json.size()) {
        SkipWhitespace(array_json, &pos);
        if (pos >= array_json.size()) {
            return false;
        }
        if (array_json[pos] == ']') {
            return true;
        }
        if (array_json[pos] == ',') {
            ++pos;
            continue;
        }
        size_t start = pos;
        if (!SkipJsonValueAt(array_json, &pos)) {
            return false;
        }
        out->push_back(array_json.substr(start, pos - start));
    }
    return false;
}

bool ExtractJsonIntField(const std::string& json_text, const std::string& key, int64_t* out) {
    if (out == nullptr) {
        return false;
    }
    size_t start = 0;
    size_t end = 0;
    if (!FindTopLevelValueSpan(json_text, key, &start, &end)) {
        return false;
    }
    if (start >= end) {
        return false;
    }
    std::string num_str = json_text.substr(start, end - start);
    char* end_ptr = nullptr;
    long long parsed = std::strtoll(num_str.c_str(), &end_ptr, 10);
    if (end_ptr == num_str.c_str() || *end_ptr != '\0') {
        return false;
    }
    *out = static_cast<int64_t>(parsed);
    return true;
}

bool ParseGatewayMessage(const std::string& json_text, ParsedGatewayMessage* out) {
    if (out == nullptr) {
        return false;
    }
    *out = ParsedGatewayMessage{};
    std::string raw_type;
    if (!ExtractJsonStringField(json_text, "type", &raw_type)) {
        return false;
    }
    out->raw_type = raw_type;

    if (raw_type == "device.welcome") {
        out->type = GatewayMsgType::kDeviceWelcome;
        ExtractJsonStringField(json_text, "device_id", &out->device_id);
        ExtractJsonStringField(json_text, "group_id", &out->group_id);
        ExtractJsonStringField(json_text, "zone", &out->zone);
    } else if (raw_type == "wake.accept") {
        out->type = GatewayMsgType::kWakeAccept;
        ExtractJsonStringField(json_text, "group_id", &out->group_id);
        ExtractJsonStringField(json_text, "winner_device_id", &out->winner_device_id);
    } else if (raw_type == "wake.suppress") {
        out->type = GatewayMsgType::kWakeSuppress;
        ExtractJsonStringField(json_text, "group_id", &out->group_id);
        ExtractJsonStringField(json_text, "winner_device_id", &out->winner_device_id);
    } else if (raw_type == "session.routed") {
        out->type = GatewayMsgType::kSessionRouted;
        ExtractJsonStringField(json_text, "session_id", &out->session_id);
        ExtractJsonStringField(json_text, "agent_id", &out->agent_id);
    } else if (raw_type == "device.command") {
        out->type = GatewayMsgType::kDeviceCommand;
        ExtractJsonStringField(json_text, "command_id", &out->command.command_id);
        ExtractJsonStringField(json_text, "capability", &out->command.capability);
        ExtractJsonStringField(json_text, "action", &out->command.action);
        ExtractJsonStringField(json_text, "params_json", &out->command.params_json);
        int64_t timeout = 5000;
        if (ExtractJsonIntField(json_text, "timeout_ms", &timeout)) {
            out->command.timeout_ms = static_cast<int32_t>(timeout);
        }
    } else if (raw_type == "voiceprint.sync") {
        out->type = GatewayMsgType::kVoiceprintSync;
        int64_t ver = 0;
        if (ExtractJsonIntField(json_text, "vp_version", &ver) && ver >= 0) {
            out->vp_version = static_cast<uint64_t>(ver);
        }
    } else if (raw_type == "pong") {
        out->type = GatewayMsgType::kPong;
    } else if (raw_type == "error") {
        out->type = GatewayMsgType::kError;
        ExtractJsonStringField(json_text, "code", &out->error_code);
    }
    return true;
}

bool ParseMediaUploadResponse(const std::string& json_text, std::string* out_media_url) {
    return ExtractJsonStringField(json_text, "media_url", out_media_url);
}

} // namespace esp_pika
