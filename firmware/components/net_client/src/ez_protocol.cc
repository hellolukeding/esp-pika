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

const char* EzWakeModeWireName(EzWakeMode mode) {
    switch (mode) {
    case EzWakeMode::kWakeWordPlusVp: return "wake_word_plus_vp";
    case EzWakeMode::kVoiceprintDirect: return "voiceprint_direct";
    case EzWakeMode::kWakeWord:
    default:
        return "wake_word";
    }
}

bool EzParseWakeMode(const std::string& wire, EzWakeMode* out) {
    if (out == nullptr) {
        return false;
    }
    if (wire == "wake_word") {
        *out = EzWakeMode::kWakeWord;
        return true;
    }
    if (wire == "wake_word_plus_vp") {
        *out = EzWakeMode::kWakeWordPlusVp;
        return true;
    }
    if (wire == "voiceprint_direct") {
        *out = EzWakeMode::kVoiceprintDirect;
        return true;
    }
    return false;
}

std::string EzWakeConfig::ToJson() const {
    char num_buf[256] = {};
    std::snprintf(num_buf, sizeof(num_buf),
                  ",\"vp_threshold\":%.3f,\"min_speech_ms\":%lu,\"cooldown_ms\":%lu,"
                  "\"vp_wake_chime\":%s,\"speech_end_silence_ms\":%lu,\"turn_commit_delay_ms\":%lu",
                  static_cast<double>(vp_threshold),
                  static_cast<unsigned long>(min_speech_ms),
                  static_cast<unsigned long>(cooldown_ms),
                  vp_wake_chime ? "true" : "false",
                  static_cast<unsigned long>(speech_end_silence_ms),
                  static_cast<unsigned long>(turn_commit_delay_ms));
    std::string out = "{";
    out += "\"mode\":\"" + std::string(EzWakeModeWireName(mode)) + "\"";
    out += ",\"wake_word\":\"" + EscapeJsonString(wake_word) + "\"";
    out += ",\"wake_pinyin\":\"" + EscapeJsonString(wake_pinyin) + "\"";
    out += num_buf;
    out.push_back('}');
    return out;
}

bool EzWakeConfig::FromJson(const std::string& json_text) {
    EzWakeConfig parsed = *this;
    std::string mode_s;
    if (ExtractJsonStringField(json_text, "mode", &mode_s)) {
        EzWakeMode m = EzWakeMode::kWakeWord;
        if (!EzParseWakeMode(mode_s, &m)) {
            return false;
        }
        parsed.mode = m;
    }
    std::string word;
    if (ExtractJsonStringField(json_text, "wake_word", &word) && !word.empty()) {
        parsed.wake_word = word;
    }
    std::string pinyin;
    if (ExtractJsonStringField(json_text, "wake_pinyin", &pinyin) && !pinyin.empty()) {
        parsed.wake_pinyin = pinyin;
    }
    std::string th_raw;
    if (ExtractJsonRawField(json_text, "vp_threshold", &th_raw) && !th_raw.empty()) {
        char* end_ptr = nullptr;
        const double th = std::strtod(th_raw.c_str(), &end_ptr);
        if (end_ptr != th_raw.c_str() && th >= 0.4 && th <= 0.99) {
            parsed.vp_threshold = std::clamp(static_cast<float>(th), 0.55f, 0.95f);
        }
    }
    int64_t ms = 0;
    if (ExtractJsonIntField(json_text, "min_speech_ms", &ms) && ms > 0) {
        parsed.min_speech_ms = static_cast<uint32_t>(std::clamp<int64_t>(ms, 400, 1500));
    }
    if (ExtractJsonIntField(json_text, "cooldown_ms", &ms) && ms > 0) {
        parsed.cooldown_ms = static_cast<uint32_t>(std::clamp<int64_t>(ms, 500, 5000));
    }
    bool chime = false;
    if (ExtractJsonBoolField(json_text, "vp_wake_chime", &chime)) {
        parsed.vp_wake_chime = chime;
    }
    if (ExtractJsonIntField(json_text, "speech_end_silence_ms", &ms) && ms > 0) {
        parsed.speech_end_silence_ms = static_cast<uint32_t>(std::clamp<int64_t>(ms, 250, 2500));
    }
    if (ExtractJsonIntField(json_text, "turn_commit_delay_ms", &ms) && ms >= 0) {
        parsed.turn_commit_delay_ms = static_cast<uint32_t>(std::clamp<int64_t>(ms, 0, 5000));
    }
    *this = std::move(parsed);
    return true;
}

#ifdef ESP_PLATFORM
bool EzWakeConfig::LoadFromNvs() {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("ez_wake", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK) {
        return false;
    }
    size_t required = 0;
    err = nvs_get_str(handle, "cfg_json", nullptr, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND || required == 0 || required > 512) {
        nvs_close(handle);
        return true;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }
    std::string raw(required, '\0');
    err = nvs_get_str(handle, "cfg_json", raw.data(), &required);
    nvs_close(handle);
    if (err != ESP_OK) {
        return false;
    }
    if (!raw.empty() && raw.back() == '\0') {
        raw.pop_back();
    }
    return FromJson(raw);
}

bool EzWakeConfig::SaveToNvs() const {
    const std::string encoded = ToJson();
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("ez_wake", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "ez_wake nvs_open write failed: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_str(handle, "cfg_json", encoded.c_str());
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "ez_wake nvs_commit failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}
#endif

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
                                 const std::string& signature_b64u,
                                 const EzHardwareProfile* hardware) {
    std::string p = "{";
    p += "\"device_id\":\"" + EscapeJsonString(device_id) + "\"";
    p += ",\"device_nonce\":\"" + EscapeJsonString(device_nonce) + "\"";
    p += ",\"capabilities\":" + CapabilitiesToJson(caps);
    p += ",\"output_preference\":\"" + std::string(EzOutputPreferenceWireName(output_preference)) + "\"";
    p += ",\"client_version\":\"" + EscapeJsonString(client_version) + "\"";
    p += ",\"signature\":\"" + EscapeJsonString(signature_b64u) + "\"";
    if (hardware != nullptr && hardware->enabled) {
        char vp_buf[32] = {};
        std::snprintf(vp_buf, sizeof(vp_buf), "%llu",
                      static_cast<unsigned long long>(hardware->vp_version));
        p += ",\"hardware\":{";
        p += "\"mac\":\"" + EscapeJsonString(hardware->mac) + "\"";
        p += ",\"chip\":\"" + EscapeJsonString(hardware->chip) + "\"";
        p += ",\"model\":\"" + EscapeJsonString(hardware->model) + "\"";
        p += ",\"zone\":\"" + EscapeJsonString(hardware->zone) + "\"";
        p += ",\"group_id\":\"" + EscapeJsonString(hardware->group_id) + "\"";
        p += ",\"vp_version\":" + std::string(vp_buf);
        p += std::string(",\"multinet_ready\":") + (hardware->multinet_ready ? "true" : "false");
        p += ",\"wake_config\":" + hardware->wake_config.ToJson();
        p += ",\"capabilities\":[";
        for (size_t i = 0; i < hardware->capabilities.size(); ++i) {
            if (i > 0) {
                p.push_back(',');
            }
            p += "\"" + EscapeJsonString(hardware->capabilities[i]) + "\"";
        }
        p += "]}";
    }
    p.push_back('}');
    return WrapEnvelope(msg_id, "hello", p);
}

std::string EzBuildDeviceCommandResultJson(const std::string& msg_id,
                                           const std::string& command_id,
                                           bool ok,
                                           const std::string& result_json,
                                           const std::string& media_url,
                                           const std::string& error_code,
                                           const std::string& error_message) {
    std::string p = "{";
    p += "\"command_id\":\"" + EscapeJsonString(command_id) + "\"";
    p += std::string(",\"ok\":") + (ok ? "true" : "false");
    if (!result_json.empty()) {
        p += ",\"result_json\":\"" + EscapeJsonString(result_json) + "\"";
    }
    if (!media_url.empty()) {
        p += ",\"media_url\":\"" + EscapeJsonString(media_url) + "\"";
    }
    if (!error_code.empty()) {
        p += ",\"error_code\":\"" + EscapeJsonString(error_code) + "\"";
    }
    if (!error_message.empty()) {
        p += ",\"error_message\":\"" + EscapeJsonString(error_message) + "\"";
    }
    p.push_back('}');
    return WrapEnvelope(msg_id, "device_command_result", p);
}

std::string EzBuildVoiceprintSyncAckJson(const std::string& msg_id,
                                         uint64_t vp_version,
                                         uint32_t count,
                                         bool ok,
                                         const std::string& error) {
    char num_buf[64] = {};
    std::snprintf(num_buf, sizeof(num_buf), "\"vp_version\":%llu,\"count\":%lu",
                  static_cast<unsigned long long>(vp_version),
                  static_cast<unsigned long>(count));
    std::string p = "{";
    p += num_buf;
    p += std::string(",\"ok\":") + (ok ? "true" : "false");
    if (!error.empty()) {
        p += ",\"error\":\"" + EscapeJsonString(error) + "\"";
    }
    p.push_back('}');
    return WrapEnvelope(msg_id, "voiceprint_sync_ack", p);
}

std::string EzBuildWakeConfigAckJson(const std::string& msg_id,
                                     const std::string& sync_id,
                                     bool ok,
                                     const EzWakeConfig* active_config,
                                     const std::string& error) {
    std::string p = "{";
    p += "\"sync_id\":\"" + EscapeJsonString(sync_id) + "\"";
    p += std::string(",\"ok\":") + (ok ? "true" : "false");
    if (active_config != nullptr) {
        p += ",\"active_config\":" + active_config->ToJson();
    }
    if (!error.empty()) {
        p += ",\"error\":\"" + EscapeJsonString(error) + "\"";
    }
    p.push_back('}');
    return WrapEnvelope(msg_id, "wake_config_ack", p);
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
                                   EzOutputPreference output_preference,
                                   const std::string& wake_source,
                                   const std::string& speaker_id,
                                   int speaker_role,
                                   float speaker_score) {
    char num_buf[24] = {};
    std::snprintf(num_buf, sizeof(num_buf), "%lu", static_cast<unsigned long>(stream_id));
    std::string p = "{";
    p += "\"client_input_id\":\"" + EscapeJsonString(client_input_id) + "\"";
    p += ",\"stream_id\":" + std::string(num_buf);
    p += ",\"format\":{\"encoding\":\"pcm_s16le\",\"sample_rate_hz\":16000,\"channels\":1,\"frame_duration_ms\":20}";
    p += ",\"output_preference\":\"" + std::string(EzOutputPreferenceWireName(output_preference)) + "\"";
    if (!speaker_id.empty()) {
        p += ",\"speaker_id\":\"" + EscapeJsonString(speaker_id) + "\"";
    }
    if (speaker_role >= 1 && speaker_role <= 3) {
        p += ",\"speaker_role\":" + std::to_string(speaker_role);
    }
    if (speaker_score > 0.0f) {
        char score_buf[24] = {};
        std::snprintf(score_buf, sizeof(score_buf), "%.4f", static_cast<double>(speaker_score));
        p += ",\"speaker_score\":" + std::string(score_buf);
    }
    if (!wake_source.empty()) {
        p += ",\"wake_source\":\"" + EscapeJsonString(wake_source) + "\"";
    }
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
    if (type == "speech_status_changed") {
        out->type = EzInboundType::kSpeechStatusChanged;
        std::string ss_raw;
        const std::string& src = ExtractJsonRawField(payload, "speech_services", &ss_raw) ? ss_raw : payload;
        std::string asr_s;
        std::string tts_s;
        if (ExtractJsonStringField(src, "asr", &asr_s)) {
            out->speech_asr_ready = (asr_s != "unavailable" && !asr_s.empty());
        }
        if (ExtractJsonStringField(src, "tts", &tts_s)) {
            out->speech_tts_ready = (tts_s != "unavailable" && !tts_s.empty());
        }
        return true;
    }
    if (type == "device_command") {
        out->type = EzInboundType::kDeviceCommand;
        if (!ExtractJsonStringField(payload, "command_id", &out->device_command.command_id) ||
            !ExtractJsonStringField(payload, "capability", &out->device_command.capability)) {
            return false;
        }
        ExtractJsonStringField(payload, "action", &out->device_command.action);
        if (!ExtractJsonStringField(payload, "params_json", &out->device_command.params_json)) {
            out->device_command.params_json = "{}";
        }
        return true;
    }
    if (type == "voiceprint_sync") {
        out->type = EzInboundType::kVoiceprintSync;
        out->raw_payload = payload;
        return true;
    }
    if (type == "wake_config_sync") {
        out->type = EzInboundType::kWakeConfigSync;
        ExtractJsonStringField(payload, "sync_id", &out->wake_config_sync.sync_id);
        int64_t cnt = 0;
        if (ExtractJsonIntField(payload, "enrolled_speaker_count", &cnt) && cnt >= 0) {
            out->wake_config_sync.enrolled_speaker_count = static_cast<uint32_t>(cnt);
        }
        std::string cfg_raw;
        const std::string& cfg_src = ExtractJsonRawField(payload, "config", &cfg_raw) ? cfg_raw : payload;
        return out->wake_config_sync.config.FromJson(cfg_src);
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
