#include "ez_protocol.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#include "ed25519_lite.h"
#include "gateway_protocol.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "nvs.h"
#endif

namespace esp_pika {
namespace {

constexpr uint32_t kBlobMagic = 0x455A5031u; // "EZP1"
constexpr uint16_t kBlobVersion = 1;
#ifdef ESP_PLATFORM
constexpr const char* kNvsNamespace = "ez_pair";
constexpr const char* kNvsKeyBlob = "id_v1";
constexpr const char* kTag = "ez_protocol";
#endif

uint32_t Fnv1a32(const uint8_t* data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

template <typename T>
void AppendPod(std::vector<uint8_t>* out, const T& val) {
    const auto* p = reinterpret_cast<const uint8_t*>(&val);
    out->insert(out->end(), p, p + sizeof(T));
}

template <typename T>
bool ReadPod(const uint8_t* data, size_t total, size_t* off, T* out) {
    if (*off + sizeof(T) > total) {
        return false;
    }
    std::memcpy(out, data + *off, sizeof(T));
    *off += sizeof(T);
    return true;
}

void AppendStr(std::vector<uint8_t>* out, const std::string& s) {
    const uint16_t n = static_cast<uint16_t>(s.size());
    AppendPod(out, n);
    out->insert(out->end(), s.begin(), s.end());
}

bool ReadStr(const uint8_t* data, size_t total, size_t* off, std::string* out) {
    uint16_t n = 0;
    if (!ReadPod(data, total, off, &n) || *off + n > total) {
        return false;
    }
    out->assign(reinterpret_cast<const char*>(data + *off), n);
    *off += n;
    return true;
}

void AppendCapabilitiesArray(const EzCapabilities& caps, uint8_t out[7]) {
    out[0] = caps.input_text ? 1u : 0u;
    out[1] = caps.input_pcm16_16k_mono ? 1u : 0u;
    out[2] = caps.output_text ? 1u : 0u;
    out[3] = caps.output_pcm16_16k_mono ? 1u : 0u;
    out[4] = caps.playback_cancel ? 1u : 0u;
    out[5] = caps.display_status ? 1u : 0u;
    out[6] = caps.display_transcript ? 1u : 0u;
}

std::string CapabilitiesToJson(const EzCapabilities& c) {
    std::string out = "{";
    out += std::string("\"input_text\":") + (c.input_text ? "true" : "false");
    out += std::string(",\"input_pcm16_16k_mono\":") + (c.input_pcm16_16k_mono ? "true" : "false");
    out += std::string(",\"output_text\":") + (c.output_text ? "true" : "false");
    out += std::string(",\"output_pcm16_16k_mono\":") + (c.output_pcm16_16k_mono ? "true" : "false");
    out += std::string(",\"playback_cancel\":") + (c.playback_cancel ? "true" : "false");
    out += std::string(",\"display_status\":") + (c.display_status ? "true" : "false");
    out += std::string(",\"display_transcript\":") + (c.display_transcript ? "true" : "false");
    out.push_back('}');
    return out;
}

std::string WrapEnvelope(const std::string& msg_id,
                         const std::string& type,
                         const std::string& payload_json) {
    std::string out = "{\"protocol_major\":1,\"protocol_minor\":0";
    out += ",\"message_id\":\"" + EscapeJsonString(msg_id) + "\"";
    out += ",\"type\":\"" + EscapeJsonString(type) + "\"";
    out += ",\"payload\":" + payload_json;
    out.push_back('}');
    return out;
}

void ParseCapabilitiesJson(const std::string& json_text, EzCapabilities* out) {
    if (out == nullptr) {
        return;
    }
    bool v = false;
    if (ExtractJsonBoolField(json_text, "input_text", &v)) out->input_text = v;
    if (ExtractJsonBoolField(json_text, "input_pcm16_16k_mono", &v)) out->input_pcm16_16k_mono = v;
    if (ExtractJsonBoolField(json_text, "output_text", &v)) out->output_text = v;
    if (ExtractJsonBoolField(json_text, "output_pcm16_16k_mono", &v)) out->output_pcm16_16k_mono = v;
    if (ExtractJsonBoolField(json_text, "playback_cancel", &v)) out->playback_cancel = v;
    if (ExtractJsonBoolField(json_text, "display_status", &v)) out->display_status = v;
    if (ExtractJsonBoolField(json_text, "display_transcript", &v)) out->display_transcript = v;
}

void StoreBe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xffu);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xffu);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xffu);
    p[3] = static_cast<uint8_t>(v & 0xffu);
}

uint32_t LoadBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint16_t LoadBe16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
}

} // namespace

std::string EzPairingIdentity::PublicKeyBase64Url() const {
    if (!has_seed) {
        return "";
    }
    char buf[64] = {};
    ez_base64url_encode(ed25519_pub, 32, buf, sizeof(buf));
    return std::string(buf);
}

std::string EzPairingIdentity::PublicKeyFingerprint() const {
    if (!has_seed) {
        return "";
    }
    char buf[10] = {};
    ez_public_key_fingerprint(ed25519_pub, buf);
    return std::string(buf);
}

void EzPairingIdentity::ClearPairing() {
    paired = false;
    installation_id.clear();
    cert_fingerprint.clear();
    device_id.clear();
    host_address.clear();
    host_port = 0;
    host_path = "/device";
}

bool EzPairingIdentity::Serialize(std::vector<uint8_t>* out) const {
    if (out == nullptr) {
        return false;
    }
    out->clear();
    AppendPod(out, kBlobMagic);
    AppendPod(out, kBlobVersion);
    const uint8_t flags = static_cast<uint8_t>((has_seed ? 1u : 0u) | (paired ? 2u : 0u));
    AppendPod(out, flags);
    out->insert(out->end(), ed25519_seed, ed25519_seed + 32);
    out->insert(out->end(), ed25519_pub, ed25519_pub + 32);
    AppendStr(out, installation_id);
    AppendStr(out, cert_fingerprint);
    AppendStr(out, device_id);
    AppendStr(out, host_address);
    AppendPod(out, host_port);
    AppendStr(out, host_path);
    AppendStr(out, display_name);
    const uint32_t crc = Fnv1a32(out->data(), out->size());
    AppendPod(out, crc);
    return true;
}

bool EzPairingIdentity::Deserialize(const uint8_t* data, size_t len) {
    if (data == nullptr || len < sizeof(kBlobMagic) + sizeof(kBlobVersion) + 1 + 64 + sizeof(uint32_t)) {
        return false;
    }
    const size_t payload_len = len - sizeof(uint32_t);
    uint32_t expected_crc = 0;
    std::memcpy(&expected_crc, data + payload_len, sizeof(expected_crc));
    if (Fnv1a32(data, payload_len) != expected_crc) {
        return false;
    }

    size_t off = 0;
    uint32_t magic = 0;
    uint16_t ver = 0;
    uint8_t flags = 0;
    if (!ReadPod(data, payload_len, &off, &magic) || magic != kBlobMagic ||
        !ReadPod(data, payload_len, &off, &ver) || ver != kBlobVersion ||
        !ReadPod(data, payload_len, &off, &flags) || off + 64 > payload_len) {
        return false;
    }

    EzPairingIdentity tmp;
    tmp.has_seed = (flags & 1u) != 0;
    tmp.paired = (flags & 2u) != 0;
    std::memcpy(tmp.ed25519_seed, data + off, 32);
    off += 32;
    std::memcpy(tmp.ed25519_pub, data + off, 32);
    off += 32;

    if (!ReadStr(data, payload_len, &off, &tmp.installation_id) ||
        !ReadStr(data, payload_len, &off, &tmp.cert_fingerprint) ||
        !ReadStr(data, payload_len, &off, &tmp.device_id) ||
        !ReadStr(data, payload_len, &off, &tmp.host_address) ||
        !ReadPod(data, payload_len, &off, &tmp.host_port) ||
        !ReadStr(data, payload_len, &off, &tmp.host_path) ||
        !ReadStr(data, payload_len, &off, &tmp.display_name) || off != payload_len) {
        return false;
    }

    if (tmp.host_path.empty()) {
        tmp.host_path = "/device";
    }
    *this = std::move(tmp);
    return true;
}

#ifdef ESP_PLATFORM
bool EzPairingIdentity::LoadFromNvs() {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
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
        ESP_LOGW(kTag, "ez_pair NVS blob invalid");
        return false;
    }
    return true;
}

bool EzPairingIdentity::SaveToNvs() const {
    std::vector<uint8_t> blob;
    if (!Serialize(&blob)) {
        return false;
    }
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
        ESP_LOGE(kTag, "nvs_commit failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}
#endif

const char* EzOutputPreferenceWireName(EzOutputPreference pref) {
    switch (pref) {
    case EzOutputPreference::kAudio: return "audio";
    case EzOutputPreference::kTextAndAudio: return "text_and_audio";
    case EzOutputPreference::kText:
    default:
        return "text";
    }
}

bool EzParseOutputPreference(const std::string& wire, EzOutputPreference* out) {
    if (out == nullptr) {
        return false;
    }
    if (wire == "text") {
        *out = EzOutputPreference::kText;
        return true;
    }
    if (wire == "audio") {
        *out = EzOutputPreference::kAudio;
        return true;
    }
    if (wire == "text_and_audio") {
        *out = EzOutputPreference::kTextAndAudio;
        return true;
    }
    return false;
}

void EzAppendLengthPrefixed(std::vector<uint8_t>* buf, const uint8_t* data, size_t len) {
    if (buf == nullptr) {
        return;
    }
    const uint64_t n = static_cast<uint64_t>(len);
    for (int shift = 56; shift >= 0; shift -= 8) {
        buf->push_back(static_cast<uint8_t>((n >> shift) & 0xffu));
    }
    if (data != nullptr && len > 0) {
        buf->insert(buf->end(), data, data + len);
    }
}

void EzAppendLengthPrefixed(std::vector<uint8_t>* buf, const std::string& s) {
    EzAppendLengthPrefixed(buf, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

std::string EzComputePairingCommitSignature(const uint8_t seed[32],
                                            const uint8_t pub[32],
                                            const std::string& pairing_request_id,
                                            const std::string& installation_id,
                                            const std::string& cert_fingerprint,
                                            const std::string& device_nonce,
                                            const std::string& host_nonce,
                                            const EzCapabilities& caps,
                                            const std::string& device_id) {
    uint8_t cap_bytes[7] = {};
    AppendCapabilitiesArray(caps, cap_bytes);

    std::vector<uint8_t> ad;
    ad.reserve(256);
    EzAppendLengthPrefixed(&ad, "ez-assistant-pairing-v1");
    EzAppendLengthPrefixed(&ad, pairing_request_id);
    EzAppendLengthPrefixed(&ad, installation_id);
    EzAppendLengthPrefixed(&ad, cert_fingerprint);
    EzAppendLengthPrefixed(&ad, device_nonce);
    EzAppendLengthPrefixed(&ad, host_nonce);
    EzAppendLengthPrefixed(&ad, cap_bytes, sizeof(cap_bytes));

    std::vector<uint8_t> bind_t;
    bind_t.reserve(64 + ad.size() + 32);
    EzAppendLengthPrefixed(&bind_t, "ez-assistant-bind-v1");
    EzAppendLengthPrefixed(&bind_t, ad.data(), ad.size());
    EzAppendLengthPrefixed(&bind_t, pub, 32);

    std::vector<uint8_t> commit_t;
    commit_t.reserve(64 + bind_t.size() + device_id.size());
    EzAppendLengthPrefixed(&commit_t, "ez-assistant-commit-v1");
    EzAppendLengthPrefixed(&commit_t, bind_t.data(), bind_t.size());
    EzAppendLengthPrefixed(&commit_t, device_id);

    uint8_t sig[64] = {};
    ez_ed25519_sign(seed, pub, commit_t.data(), commit_t.size(), sig);
    char b64[128] = {};
    ez_base64url_encode(sig, 64, b64, sizeof(b64));
    return std::string(b64);
}

std::string EzComputeAuthSignature(const uint8_t seed[32],
                                   const uint8_t pub[32],
                                   const std::string& connection_id,
                                   const std::string& host_nonce,
                                   const std::string& device_id,
                                   const std::string& device_nonce,
                                   const EzCapabilities& caps,
                                   EzOutputPreference output_preference) {
    const uint8_t ver_major[2] = {0x00u, 0x01u};
    const uint8_t ver_minor[2] = {0x00u, 0x00u};
    uint8_t cap_bytes[7] = {};
    AppendCapabilitiesArray(caps, cap_bytes);

    std::vector<uint8_t> auth_t;
    auth_t.reserve(256);
    EzAppendLengthPrefixed(&auth_t, "ez-assistant-auth-v1");
    EzAppendLengthPrefixed(&auth_t, ver_major, sizeof(ver_major));
    EzAppendLengthPrefixed(&auth_t, ver_minor, sizeof(ver_minor));
    EzAppendLengthPrefixed(&auth_t, connection_id);
    EzAppendLengthPrefixed(&auth_t, host_nonce);
    EzAppendLengthPrefixed(&auth_t, device_id);
    EzAppendLengthPrefixed(&auth_t, device_nonce);
    EzAppendLengthPrefixed(&auth_t, cap_bytes, sizeof(cap_bytes));
    EzAppendLengthPrefixed(&auth_t, EzOutputPreferenceWireName(output_preference));

    uint8_t sig[64] = {};
    ez_ed25519_sign(seed, pub, auth_t.data(), auth_t.size(), sig);
    char b64[128] = {};
    ez_base64url_encode(sig, 64, b64, sizeof(b64));
    return std::string(b64);
}

std::string EzBuildPairingHelloJson(const std::string& msg_id,
                                    const std::string& pairing_request_id,
                                    const std::string& display_name,
                                    const std::string& device_nonce,
                                    const EzCapabilities& caps,
                                    const std::string& public_key_b64u,
                                    const std::string& client_version) {
    std::string p = "{";
    p += "\"pairing_request_id\":\"" + EscapeJsonString(pairing_request_id) + "\"";
    p += ",\"display_name\":\"" + EscapeJsonString(display_name) + "\"";
    p += ",\"device_nonce\":\"" + EscapeJsonString(device_nonce) + "\"";
    p += ",\"capabilities\":" + CapabilitiesToJson(caps);
    p += ",\"pairing_mode\":\"direct_confirm\"";
    p += ",\"public_key\":\"" + EscapeJsonString(public_key_b64u) + "\"";
    if (!client_version.empty()) {
        p += ",\"client_version\":\"" + EscapeJsonString(client_version) + "\"";
    }
    p.push_back('}');
    return WrapEnvelope(msg_id, "pairing_hello", p);
}

std::string EzBuildPairingDirectCommitJson(const std::string& msg_id,
                                           const std::string& pairing_request_id,
                                           const std::string& device_id,
                                           const std::string& signature_b64u) {
    std::string p = "{";
    p += "\"pairing_request_id\":\"" + EscapeJsonString(pairing_request_id) + "\"";
    p += ",\"device_id\":\"" + EscapeJsonString(device_id) + "\"";
    p += ",\"signature\":\"" + EscapeJsonString(signature_b64u) + "\"";
    p.push_back('}');
    return WrapEnvelope(msg_id, "pairing_direct_commit", p);
}

std::string EzBuildAuthHelloJson(const std::string& msg_id,
                                 const std::string& device_id,
                                 const std::string& device_nonce,
                                 const EzCapabilities& caps,
                                 EzOutputPreference output_preference,
                                 const std::string& client_version,
                                 const std::string& signature_b64u) {
    std::string p = "{";
    p += "\"device_id\":\"" + EscapeJsonString(device_id) + "\"";
    p += ",\"device_nonce\":\"" + EscapeJsonString(device_nonce) + "\"";
    p += ",\"capabilities\":" + CapabilitiesToJson(caps);
    p += ",\"output_preference\":\"" + std::string(EzOutputPreferenceWireName(output_preference)) + "\"";
    p += ",\"client_version\":\"" + EscapeJsonString(client_version) + "\"";
    p += ",\"signature\":\"" + EscapeJsonString(signature_b64u) + "\"";
    p.push_back('}');
    return WrapEnvelope(msg_id, "hello", p);
}

std::string EzBuildSetOutputPreferenceJson(const std::string& msg_id, EzOutputPreference pref) {
    std::string p = "{\"output_preference\":\"";
    p += EzOutputPreferenceWireName(pref);
    p += "\"}";
    return WrapEnvelope(msg_id, "set_output_preference", p);
}

std::string EzBuildListenStartJson(const std::string& msg_id,
                                   const std::string& client_input_id,
                                   uint32_t stream_id,
                                   EzOutputPreference output_preference) {
    char num_buf[24] = {};
    std::snprintf(num_buf, sizeof(num_buf), "%lu", static_cast<unsigned long>(stream_id));
    std::string p = "{";
    p += "\"client_input_id\":\"" + EscapeJsonString(client_input_id) + "\"";
    p += ",\"stream_id\":" + std::string(num_buf);
    p += ",\"format\":{\"encoding\":\"pcm_s16le\",\"sample_rate_hz\":16000,\"channels\":1,\"frame_duration_ms\":20}";
    p += ",\"output_preference\":\"" + std::string(EzOutputPreferenceWireName(output_preference)) + "\"";
    p.push_back('}');
    return WrapEnvelope(msg_id, "listen_start", p);
}

std::string EzBuildListenStopJson(const std::string& msg_id,
                                  uint32_t stream_id,
                                  uint32_t last_sequence) {
    char buf[96] = {};
    std::snprintf(buf, sizeof(buf), "{\"stream_id\":%lu,\"last_sequence\":%lu}",
                  static_cast<unsigned long>(stream_id),
                  static_cast<unsigned long>(last_sequence));
    return WrapEnvelope(msg_id, "listen_stop", buf);
}

std::string EzBuildListenCancelJson(const std::string& msg_id, uint32_t stream_id) {
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "{\"stream_id\":%lu}", static_cast<unsigned long>(stream_id));
    return WrapEnvelope(msg_id, "listen_cancel", buf);
}

std::string EzBuildPlaybackCancelJson(const std::string& msg_id,
                                      const std::string& output_id,
                                      uint32_t stream_id) {
    char num_buf[24] = {};
    std::snprintf(num_buf, sizeof(num_buf), "%lu", static_cast<unsigned long>(stream_id));
    std::string p = "{\"output_id\":\"" + EscapeJsonString(output_id) + "\",\"stream_id\":";
    p += num_buf;
    p.push_back('}');
    return WrapEnvelope(msg_id, "playback_cancel", p);
}

std::string EzBuildPongJson(const std::string& msg_id,
                            const std::string& nonce,
                            int64_t sent_at_ms) {
    char num_buf[32] = {};
    std::snprintf(num_buf, sizeof(num_buf), "%lld", static_cast<long long>(sent_at_ms));
    std::string p = "{\"nonce\":\"" + EscapeJsonString(nonce) + "\",\"sent_at_ms\":";
    p += num_buf;
    p.push_back('}');
    return WrapEnvelope(msg_id, "pong", p);
}

bool EzParseInboundMessage(const std::string& json_text, EzInboundMessage* out) {
    if (out == nullptr) {
        return false;
    }
    *out = EzInboundMessage{};
    std::string type;
    if (!ExtractJsonStringField(json_text, "type", &type) || type.empty()) {
        return false;
    }
    out->raw_type = type;
    ExtractJsonStringField(json_text, "message_id", &out->message_id);

    std::string payload;
    if (!ExtractJsonRawField(json_text, "payload", &payload)) {
        return false;
    }

    if (type == "auth_challenge") {
        out->type = EzInboundType::kAuthChallenge;
        return ExtractJsonStringField(payload, "connection_id", &out->connection_id) &&
               ExtractJsonStringField(payload, "nonce", &out->host_nonce);
    }
    if (type == "pairing_pending") {
        out->type = EzInboundType::kPairingPending;
        ExtractJsonStringField(payload, "pairing_request_id", &out->pairing_request_id);
        return true;
    }
    if (type == "pairing_direct_challenge") {
        out->type = EzInboundType::kPairingDirectChallenge;
        return ExtractJsonStringField(payload, "pairing_request_id", &out->pairing_request_id) &&
               ExtractJsonStringField(payload, "host_nonce", &out->host_nonce) &&
               ExtractJsonStringField(payload, "device_id", &out->device_id);
    }
    if (type == "pairing_complete") {
        out->type = EzInboundType::kPairingComplete;
        ExtractJsonStringField(payload, "pairing_request_id", &out->pairing_request_id);
        ExtractJsonStringField(payload, "display_name", &out->display_name);
        return ExtractJsonStringField(payload, "device_id", &out->device_id);
    }
    if (type == "hello_ack") {
        out->type = EzInboundType::kHelloAck;
        ExtractJsonStringField(payload, "device_id", &out->device_id);
        ExtractJsonStringField(payload, "connection_id", &out->connection_id);
        std::string caps_raw;
        if (ExtractJsonRawField(payload, "capabilities", &caps_raw)) {
            ParseCapabilitiesJson(caps_raw, &out->capabilities);
        }
        std::string pref;
        if (ExtractJsonStringField(payload, "output_preference", &pref)) {
            EzParseOutputPreference(pref, &out->output_preference);
        }
        return true;
    }
    if (type == "output_preference_changed") {
        out->type = EzInboundType::kOutputPreferenceChanged;
        std::string pref;
        if (ExtractJsonStringField(payload, "output_preference", &pref)) {
            EzParseOutputPreference(pref, &out->output_preference);
        }
        return true;
    }
    if (type == "state_changed") {
        out->type = EzInboundType::kStateChanged;
        ExtractJsonStringField(payload, "state", &out->state);
        ExtractJsonStringField(payload, "reason", &out->reason);
        return true;
    }
    if (type == "transcript") {
        out->type = EzInboundType::kTranscript;
        return ExtractJsonStringField(payload, "text", &out->text);
    }
    if (type == "text_output") {
        out->type = EzInboundType::kTextOutput;
        ExtractJsonStringField(payload, "output_id", &out->output_id);
        return ExtractJsonStringField(payload, "text", &out->text);
    }
    if (type == "playback_start") {
        out->type = EzInboundType::kPlaybackStart;
        int64_t sid = 0;
        ExtractJsonStringField(payload, "output_id", &out->output_id);
        ExtractJsonStringField(payload, "text", &out->text);
        if (!ExtractJsonIntField(payload, "stream_id", &sid) || sid <= 0) {
            return false;
        }
        out->stream_id = static_cast<uint32_t>(sid);
        return true;
    }
    if (type == "playback_end") {
        out->type = EzInboundType::kPlaybackEnd;
        int64_t sid = 0;
        ExtractJsonStringField(payload, "output_id", &out->output_id);
        ExtractJsonStringField(payload, "reason", &out->reason);
        if (ExtractJsonIntField(payload, "stream_id", &sid) && sid > 0) {
            out->stream_id = static_cast<uint32_t>(sid);
        }
        return true;
    }
    if (type == "ping") {
        out->type = EzInboundType::kPing;
        ExtractJsonStringField(payload, "nonce", &out->ping_nonce);
        ExtractJsonIntField(payload, "sent_at_ms", &out->ping_sent_at_ms);
        return true;
    }
    if (type == "error") {
        out->type = EzInboundType::kError;
        ExtractJsonStringField(payload, "code", &out->error_code);
        ExtractJsonBoolField(payload, "recoverable", &out->recoverable);
        return true;
    }

    out->type = EzInboundType::kUnknown;
    return true;
}

bool EzEncodeUplinkPcmFrame(uint32_t stream_id,
                            uint32_t sequence,
                            const uint8_t pcm640[640],
                            uint8_t out656[656]) {
    if (stream_id == 0 || pcm640 == nullptr || out656 == nullptr) {
        return false;
    }
    out656[0] = 1u; // protocol version
    out656[1] = 1u; // kind = voice_uplink
    out656[2] = 0u;
    out656[3] = 0u;
    StoreBe32(out656 + 4, stream_id);
    StoreBe32(out656 + 8, sequence);
    out656[12] = static_cast<uint8_t>((640u >> 8) & 0xffu);
    out656[13] = static_cast<uint8_t>(640u & 0xffu);
    out656[14] = 0u;
    out656[15] = 0u;
    std::memcpy(out656 + 16, pcm640, 640);
    return true;
}

bool EzDecodeDownlinkPcmFrame(const uint8_t* frame,
                              size_t len,
                              uint32_t* out_stream_id,
                              uint32_t* out_sequence,
                              const uint8_t** out_pcm,
                              size_t* out_pcm_len) {
    if (frame == nullptr || len < 16 || out_stream_id == nullptr || out_sequence == nullptr ||
        out_pcm == nullptr || out_pcm_len == nullptr) {
        return false;
    }
    if (frame[0] != 1u || frame[1] != 2u || frame[2] != 0u || frame[3] != 0u ||
        frame[14] != 0u || frame[15] != 0u) {
        return false;
    }
    const uint16_t payload_len = LoadBe16(frame + 12);
    if (payload_len == 0 || payload_len > 640 || (payload_len % 2) != 0 ||
        len != static_cast<size_t>(16u + payload_len)) {
        return false;
    }
    const uint32_t sid = LoadBe32(frame + 4);
    if (sid == 0) {
        return false;
    }
    *out_stream_id = sid;
    *out_sequence = LoadBe32(frame + 8);
    *out_pcm = frame + 16;
    *out_pcm_len = payload_len;
    return true;
}

bool EzIsValidHexLower(const std::string& value, size_t expected_len, std::string* normalized_out) {
    if (value.size() != expected_len) {
        return false;
    }
    std::string norm;
    norm.reserve(expected_len);
    for (unsigned char c : value) {
        if (!std::isxdigit(c)) {
            return false;
        }
        norm.push_back(static_cast<char>(std::tolower(c)));
    }
    if (normalized_out != nullptr) {
        *normalized_out = std::move(norm);
    }
    return true;
}

bool EzParseConnectTriggerJson(const std::string& json_text, EzConnectTrigger* out) {
    if (out == nullptr) {
        return false;
    }
    EzConnectTrigger parsed;
    ExtractJsonStringField(json_text, "host", &parsed.host);
    int64_t port = 0;
    if (!ExtractJsonIntField(json_text, "port", &port) || port <= 0 || port > 65535) {
        return false;
    }
    parsed.port = static_cast<uint16_t>(port);

    if (!ExtractJsonStringField(json_text, "path", &parsed.path) || parsed.path.empty()) {
        parsed.path = "/device";
    } else if (parsed.path[0] != '/') {
        parsed.path.insert(parsed.path.begin(), '/');
    }

    std::string raw_install;
    std::string raw_fp;
    if (!ExtractJsonStringField(json_text, "installation_id", &raw_install) ||
        !EzIsValidHexLower(raw_install, 32, &parsed.installation_id)) {
        return false;
    }
    if (!ExtractJsonStringField(json_text, "certificate_fingerprint", &raw_fp) ||
        !EzIsValidHexLower(raw_fp, 64, &parsed.certificate_fingerprint)) {
        return false;
    }

    *out = std::move(parsed);
    return true;
}

} // namespace esp_pika
