#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace esp_pika {

/// Device Protocol 1.0 persistent pairing identity (stored encrypted in NVS `ez_pair`).
struct EzPairingIdentity {
    bool has_seed = false;
    uint8_t ed25519_seed[32] = {};
    uint8_t ed25519_pub[32] = {};

    bool paired = false;
    std::string installation_id;
    std::string cert_fingerprint;
    std::string device_id;
    std::string host_address;
    uint16_t host_port = 0;
    std::string host_path = "/device";
    std::string display_name = "Mosaico";

    std::string PublicKeyBase64Url() const;
    std::string PublicKeyFingerprint() const;
    void ClearPairing();

    bool Serialize(std::vector<uint8_t>* out) const;
    bool Deserialize(const uint8_t* data, size_t len);

#ifdef ESP_PLATFORM
    bool LoadFromNvs();
    bool SaveToNvs() const;
#endif
};

/// Exact 7-field capability mask matching `DeviceCapabilitiesSnapshot`.
struct EzCapabilities {
    bool input_text = true;
    bool input_pcm16_16k_mono = true;
    bool output_text = true;
    bool output_pcm16_16k_mono = true;
    bool playback_cancel = true;
    bool display_status = true;
    bool display_transcript = true;
};

enum class EzOutputPreference {
    kText = 0,
    kAudio,
    kTextAndAudio,
};

const char* EzOutputPreferenceWireName(EzOutputPreference pref);
bool EzParseOutputPreference(const std::string& wire, EzOutputPreference* out);

/// Appends length-prefixed (`uint64_t` big-endian length + bytes) chunk to transcript.
void EzAppendLengthPrefixed(std::vector<uint8_t>* buf, const uint8_t* data, size_t len);
void EzAppendLengthPrefixed(std::vector<uint8_t>* buf, const std::string& s);

/// Computes base64url(Ed25519 signature) for `pairing_direct_commit`.
std::string EzComputePairingCommitSignature(const uint8_t seed[32],
                                            const uint8_t pub[32],
                                            const std::string& pairing_request_id,
                                            const std::string& installation_id,
                                            const std::string& cert_fingerprint,
                                            const std::string& device_nonce,
                                            const std::string& host_nonce,
                                            const EzCapabilities& caps,
                                            const std::string& device_id);

/// Computes base64url(Ed25519 signature) for authenticated `hello`.
std::string EzComputeAuthSignature(const uint8_t seed[32],
                                   const uint8_t pub[32],
                                   const std::string& connection_id,
                                   const std::string& host_nonce,
                                   const std::string& device_id,
                                   const std::string& device_nonce,
                                   const EzCapabilities& caps,
                                   EzOutputPreference output_preference);

// Outbound Device Protocol 1.0 JSON envelope builders
std::string EzBuildPairingHelloJson(const std::string& msg_id,
                                    const std::string& pairing_request_id,
                                    const std::string& display_name,
                                    const std::string& device_nonce,
                                    const EzCapabilities& caps,
                                    const std::string& public_key_b64u,
                                    const std::string& client_version);

std::string EzBuildPairingDirectCommitJson(const std::string& msg_id,
                                           const std::string& pairing_request_id,
                                           const std::string& device_id,
                                           const std::string& signature_b64u);

/// Hardware profile declared in authenticated `hello.hardware` for Phase 2 native console & MCP.
struct EzHardwareProfile {
    bool enabled = false;
    std::string mac;
    std::string chip;
    std::string model;
    std::string zone;
    std::string group_id;
    uint64_t vp_version = 0;
    std::vector<std::string> capabilities;
};

/// Inbound hardware control command delivered over authenticated WSS `/device`.
struct EzDeviceCommandRequest {
    std::string command_id;
    std::string capability;
    std::string action;
    std::string params_json;
};

std::string EzBuildAuthHelloJson(const std::string& msg_id,
                                 const std::string& device_id,
                                 const std::string& device_nonce,
                                 const EzCapabilities& caps,
                                 EzOutputPreference output_preference,
                                 const std::string& client_version,
                                 const std::string& signature_b64u,
                                 const EzHardwareProfile* hardware = nullptr);

std::string EzBuildDeviceCommandResultJson(const std::string& msg_id,
                                           const std::string& command_id,
                                           bool ok,
                                           const std::string& result_json,
                                           const std::string& media_url,
                                           const std::string& error_code,
                                           const std::string& error_message);

std::string EzBuildVoiceprintSyncAckJson(const std::string& msg_id,
                                         uint64_t vp_version,
                                         uint32_t count,
                                         bool ok,
                                         const std::string& error = "");

std::string EzBuildSetOutputPreferenceJson(const std::string& msg_id, EzOutputPreference pref);

std::string EzBuildListenStartJson(const std::string& msg_id,
                                   const std::string& client_input_id,
                                   uint32_t stream_id,
                                   EzOutputPreference output_preference);

std::string EzBuildListenStopJson(const std::string& msg_id,
                                  uint32_t stream_id,
                                  uint32_t last_sequence);

std::string EzBuildListenCancelJson(const std::string& msg_id, uint32_t stream_id);

std::string EzBuildPlaybackCancelJson(const std::string& msg_id,
                                      const std::string& output_id,
                                      uint32_t stream_id);

std::string EzBuildPongJson(const std::string& msg_id,
                            const std::string& nonce,
                            int64_t sent_at_ms);

enum class EzInboundType {
    kUnknown = 0,
    kAuthChallenge,
    kPairingPending,
    kPairingDirectChallenge,
    kPairingComplete,
    kHelloAck,
    kOutputPreferenceChanged,
    kStateChanged,
    kTranscript,
    kTextOutput,
    kPlaybackStart,
    kPlaybackEnd,
    kSpeechStatusChanged,
    kDeviceCommand,
    kVoiceprintSync,
    kPing,
    kError,
};

struct EzInboundMessage {
    EzInboundType type = EzInboundType::kUnknown;
    std::string raw_type;
    std::string message_id;

    // auth_challenge
    std::string connection_id;
    std::string host_nonce;

    // pairing_pending / pairing_direct_challenge / pairing_complete
    std::string pairing_request_id;
    std::string device_id;
    std::string display_name;

    // hello_ack / output_preference_changed
    EzCapabilities capabilities;
    EzOutputPreference output_preference = EzOutputPreference::kText;

    // speech_status_changed
    bool speech_asr_ready = false;
    bool speech_tts_ready = false;

    // device_command
    EzDeviceCommandRequest device_command;

    // voiceprint_sync (raw JSON object of `payload` for VoiceprintStore)
    std::string raw_payload;

    // state_changed / transcript / text_output
    std::string state;
    std::string reason;
    std::string text;

    // playback_start / playback_end
    std::string output_id;
    uint32_t stream_id = 0;

    // ping
    std::string ping_nonce;
    int64_t ping_sent_at_ms = 0;

    // error
    std::string error_code;
    bool recoverable = false;
};

bool EzParseInboundMessage(const std::string& json_text, EzInboundMessage* out);

/// Encodes a 656-byte `UplinkPcmFrame` (`version=1, kind=1, 640B 16kHz PCM16`).
bool EzEncodeUplinkPcmFrame(uint32_t stream_id,
                            uint32_t sequence,
                            const uint8_t pcm640[640],
                            uint8_t out656[656]);

/// Decodes a `DownlinkPcmFrame` (`version=1, kind=2`) and returns pointer into `frame`.
bool EzDecodeDownlinkPcmFrame(const uint8_t* frame,
                              size_t len,
                              uint32_t* out_stream_id,
                              uint32_t* out_sequence,
                              const uint8_t** out_pcm,
                              size_t* out_pcm_len);

/// Payload parsed from HTTP `POST /ez/connect`.
struct EzConnectTrigger {
    std::string host;
    uint16_t port = 0;
    std::string path = "/device";
    std::string installation_id;
    std::string certificate_fingerprint;
};

bool EzParseConnectTriggerJson(const std::string& json_text, EzConnectTrigger* out);

/// Validates 32 hex (`installation_id`) or 64 hex (`certificate_fingerprint`) lowercase/hex strings.
bool EzIsValidHexLower(const std::string& value, size_t expected_len, std::string* normalized_out = nullptr);

} // namespace esp_pika
