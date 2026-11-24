package biz

import (
	"context"
	"time"
)

const (
	// SettingKeyEZAgentMode stores the gateway Agent routing mode ("auto", "ez_device", or "grpc").
	SettingKeyEZAgentMode = "ez_agent_mode"
	// SettingKeyEZDeviceIdentity stores the persisted Device Protocol 1.0 identity record as JSON.
	SettingKeyEZDeviceIdentity = "ez_device_identity"
	// SettingKeyEZManualHost stores an optional manual host target override for EZ Assistant discovery.
	SettingKeyEZManualHost = "ez_manual_host"

	// EZModeAuto routes turns to the paired EZ Assistant Desktop when online and falls back to gRPC.
	EZModeAuto = "auto"
	// EZModeDevice routes turns exclusively to the paired EZ Assistant Desktop via Device Protocol 1.0.
	EZModeDevice = "ez_device"
	// EZModeGRPC routes turns exclusively to the gRPC AgentAdapter (agent/ez-bridge).
	EZModeGRPC = "grpc"
)

// EZHostCandidate describes a discovered or manually entered EZ Assistant Desktop Runtime Host.
type EZHostCandidate struct {
	InstanceName           string   `json:"instance_name"`
	HostName               string   `json:"host_name"`
	Addresses              []string `json:"addresses"`
	Port                   int      `json:"port"`
	Path                   string   `json:"path"`
	ProtocolMajor          int      `json:"protocol_major"`
	ProtocolMinor          int      `json:"protocol_minor"`
	InstallationID         string   `json:"installation_id"`
	CertificateFingerprint string   `json:"certificate_fingerprint"`
	PairingAvailable       bool     `json:"pairing_available"`
	PairingModes           []string `json:"pairing_modes,omitempty"`
}

// EZPairRequest carries host pinning info to start a Device Protocol 1.0 pairing session.
type EZPairRequest struct {
	Address                string `json:"address"`
	Path                   string `json:"path"`
	InstallationID         string `json:"installation_id"`
	CertificateFingerprint string `json:"certificate_fingerprint"`
	DisplayName            string `json:"display_name"`
	PairingMode            string `json:"pairing_mode,omitempty"`
}

// EZSpeechServicesStatus describes live ASR/TTS readiness reported by EZ Assistant Desktop.
type EZSpeechServicesStatus struct {
	ASR string `json:"asr"`
	TTS string `json:"tts"`
}

// EZBridgeStatus summarizes the live connection, identity, routing mode, and optional pairing code/fingerprint.
type EZBridgeStatus struct {
	Mode                   string                  `json:"mode"`
	ActiveBackend          string                  `json:"active_backend"`
	State                  string                  `json:"state"`
	IdentityState          string                  `json:"identity_state"`
	DeviceID               string                  `json:"device_id,omitempty"`
	DisplayName            string                  `json:"display_name,omitempty"`
	InstallationID         string                  `json:"installation_id,omitempty"`
	CertificateFingerprint string                  `json:"certificate_fingerprint,omitempty"`
	HostAddress            string                  `json:"host_address,omitempty"`
	HostPath               string                  `json:"host_path,omitempty"`
	PairingMode            string                  `json:"pairing_mode,omitempty"`
	PairingCode            string                  `json:"pairing_code,omitempty"`
	KeyFingerprint         string                  `json:"key_fingerprint,omitempty"`
	PairingExpiresAtMs     int64                   `json:"pairing_expires_at_ms,omitempty"`
	LastError              string                  `json:"last_error,omitempty"`
	UpdatedAtMs            int64                   `json:"updated_at_ms"`
	SpeechServices         *EZSpeechServicesStatus `json:"speech_services,omitempty"`
}

// EZDeviceBridge bridges the gateway to an EZ Assistant Desktop Runtime Host using Device Protocol 1.0.
type EZDeviceBridge interface {
	AgentAdapter
	Start(ctx context.Context) error
	Stop(ctx context.Context) error
	Status(ctx context.Context) EZBridgeStatus
	SetMode(ctx context.Context, mode string) (EZBridgeStatus, error)
	DiscoverHosts(ctx context.Context, timeout time.Duration) ([]EZHostCandidate, error)
	StartPairing(ctx context.Context, req EZPairRequest) (EZBridgeStatus, error)
	ResetPairing(ctx context.Context) (EZBridgeStatus, error)
	IsOnline() bool
	EffectiveMode(ctx context.Context) string
	CanUseASR() bool
	CanUseTTS() bool
	RecognizeSpeech(ctx context.Context, pcm16k []byte) (string, error)
	SynthesizeSpeech(ctx context.Context, text string) ([]byte, error)
}
