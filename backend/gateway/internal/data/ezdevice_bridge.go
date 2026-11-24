package data

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"math/big"
	"net"
	"net/http"
	"net/url"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
	"unicode/utf8"

	"esp-pika/backend/gateway/internal/biz"
	"esp-pika/backend/gateway/internal/conf"

	"github.com/coder/websocket"
	"github.com/go-kratos/kratos/v2/log"
	"github.com/google/uuid"
	"github.com/grandcat/zeroconf"
	"github.com/miekg/dns"
)

const (
	ezMDNSServiceType       = "_ez-assistant._tcp"
	ezMDNSDomain            = "local."
	ezClientVersion         = "esp-pika-gateway/0.3.0"
	ezMaxControlMessageSize = 64 * 1024
	ezStepReadTimeout       = 30 * time.Second
	ezIdleReadTimeout       = 40 * time.Second
	ezDefaultTurnTimeout    = 180 * time.Second
	ezDefaultPairingWindow  = 5 * time.Minute
	ezRelayHeaderBytes      = 16
	ezRelayKindUplink       = byte(3)
	ezRelayKindDownlink     = byte(4)
	ezRelayMaxUplinkChunk   = 32 * 1024
	ezRelayMaxPCMBytes      = 60 * 16000 * 2
	ezRelayMaxTTSPCMBytes   = 16 * 1024 * 1024
)

var ezExcludedBrowseIfacePrefixes = []string{
	"utun", "tun", "tap", "bridge", "br-", "tailscale",
	"awdl", "llw", "docker", "veth", "virbr", "vboxnet", "vmnet", "lo",
}

var ezReconnectBackoffs = []time.Duration{
	1 * time.Second,
	2 * time.Second,
	4 * time.Second,
	8 * time.Second,
	15 * time.Second,
}

type persistedEZIdentity struct {
	State                  string `json:"state"` // "pending" | "active"
	InstallationID         string `json:"installation_id"`
	CertificateFingerprint string `json:"certificate_fingerprint"`
	HostAddress            string `json:"host_address"`
	HostPath               string `json:"host_path"`
	DeviceID               string `json:"device_id"`
	DisplayName            string `json:"display_name"`
	PrivateSeedB64U        string `json:"private_seed_b64u"`
	PublicKeyB64U          string `json:"public_key_b64u"`
	UpdatedAtMs            int64  `json:"updated_at_ms"`
}

type ezWireEnvelope struct {
	ProtocolMajor int             `json:"protocol_major"`
	ProtocolMinor int             `json:"protocol_minor"`
	MessageID     string          `json:"message_id"`
	Type          string          `json:"type"`
	Payload       json.RawMessage `json:"payload"`
}

type ezAuthChallengePayload struct {
	ConnectionID string `json:"connection_id"`
	Nonce        string `json:"nonce"`
	ServerTimeMs int64  `json:"server_time_ms"`
}

type ezPairingHelloPayload struct {
	PairingRequestID string         `json:"pairing_request_id"`
	DisplayName      string         `json:"display_name"`
	DeviceNonce      string         `json:"device_nonce"`
	Capabilities     ezCapabilities `json:"capabilities"`
	PairingMode      string         `json:"pairing_mode,omitempty"`
	PakeShare        string         `json:"pake_share,omitempty"`
	PublicKey        string         `json:"public_key,omitempty"`
	ClientVersion    string         `json:"client_version,omitempty"`
}

type ezPairingDirectChallengePayload struct {
	PairingRequestID string `json:"pairing_request_id"`
	HostNonce        string `json:"host_nonce"`
	DeviceID         string `json:"device_id"`
}

type ezPairingDirectCommitPayload struct {
	PairingRequestID string `json:"pairing_request_id"`
	DeviceID         string `json:"device_id"`
	Signature        string `json:"signature"`
}

type ezPairingPendingPayload struct {
	PairingRequestID string `json:"pairing_request_id"`
	ExpiresAtMs      int64  `json:"expires_at_ms"`
}

type ezPairingPakePayload struct {
	PairingRequestID string `json:"pairing_request_id"`
	HostNonce        string `json:"host_nonce"`
	PakeShare        string `json:"pake_share"`
	ConfirmationMAC  string `json:"confirmation_mac"`
}

type ezPairingConfirmPayload struct {
	PairingRequestID string `json:"pairing_request_id"`
	ConfirmationMAC  string `json:"confirmation_mac"`
}

type ezPairingBindPayload struct {
	PairingRequestID string `json:"pairing_request_id"`
	PublicKey        string `json:"public_key"`
	Signature        string `json:"signature"`
	BindingMAC       string `json:"binding_mac"`
}

type ezPairingBindAckPayload struct {
	PairingRequestID string `json:"pairing_request_id"`
	DeviceID         string `json:"device_id"`
	HostProof        string `json:"host_proof"`
}

type ezPairingCommitPayload struct {
	PairingRequestID string `json:"pairing_request_id"`
	DeviceID         string `json:"device_id"`
	Signature        string `json:"signature"`
	BindingMAC       string `json:"binding_mac"`
}

type ezPairingCompletePayload struct {
	PairingRequestID string `json:"pairing_request_id"`
	DeviceID         string `json:"device_id"`
	DisplayName      string `json:"display_name"`
}

type ezDeviceMCPServerDecl struct {
	ID          string            `json:"id,omitempty"`
	DisplayName string            `json:"display_name"`
	Description string            `json:"description,omitempty"`
	Scheme      string            `json:"scheme,omitempty"`
	Port        int               `json:"port"`
	Path        string            `json:"path"`
	Headers     map[string]string `json:"headers,omitempty"`
}

type ezDeviceConsoleDecl struct {
	Scheme    string `json:"scheme,omitempty"`
	Port      int    `json:"port"`
	APIPrefix string `json:"api_prefix,omitempty"`
	UIPath    string `json:"ui_path,omitempty"`
}

type ezDeviceHelloPayload struct {
	DeviceID         string                  `json:"device_id"`
	DeviceNonce      string                  `json:"device_nonce"`
	Capabilities     ezCapabilities          `json:"capabilities"`
	OutputPreference string                  `json:"output_preference"`
	ClientVersion    string                  `json:"client_version"`
	Signature        string                  `json:"signature"`
	MCPServers       []ezDeviceMCPServerDecl `json:"mcp_servers,omitempty"`
	Console          *ezDeviceConsoleDecl    `json:"console,omitempty"`
	SpeechRelay      bool                    `json:"speech_relay,omitempty"`
}

type ezHelloAckPayload struct {
	DeviceID         string                      `json:"device_id"`
	ConnectionID     string                      `json:"connection_id"`
	Capabilities     ezCapabilities              `json:"capabilities"`
	OutputPreference string                      `json:"output_preference"`
	SpeechServices   *biz.EZSpeechServicesStatus `json:"speech_services,omitempty"`
}

type ezSpeechStatusChangedPayload struct {
	SpeechServices biz.EZSpeechServicesStatus `json:"speech_services"`
}

type ezSpeechPCMFormat struct {
	Encoding        string `json:"encoding"`
	SampleRateHz    uint32 `json:"sample_rate_hz"`
	Channels        uint8  `json:"channels"`
	FrameDurationMs uint16 `json:"frame_duration_ms"`
}

type ezSpeechAsrStartPayload struct {
	RequestID string            `json:"request_id"`
	StreamID  uint32            `json:"stream_id"`
	Format    ezSpeechPCMFormat `json:"format"`
}

type ezSpeechAsrStopPayload struct {
	RequestID    string `json:"request_id"`
	StreamID     uint32 `json:"stream_id"`
	LastSequence uint32 `json:"last_sequence"`
}

type ezSpeechAsrCancelPayload struct {
	RequestID string `json:"request_id"`
}

type ezSpeechAsrResultPayload struct {
	RequestID string `json:"request_id"`
	OK        bool   `json:"ok"`
	Text      string `json:"text,omitempty"`
	Error     string `json:"error,omitempty"`
}

type ezSpeechTtsRequestPayload struct {
	RequestID string `json:"request_id"`
	StreamID  uint32 `json:"stream_id"`
	Text      string `json:"text"`
}

type ezSpeechTtsStartPayload struct {
	RequestID  string             `json:"request_id"`
	StreamID   uint32             `json:"stream_id"`
	OK         bool               `json:"ok"`
	Format     *ezSpeechPCMFormat `json:"format,omitempty"`
	TotalBytes uint64             `json:"total_bytes,omitempty"`
	Error      string             `json:"error,omitempty"`
}

type ezSpeechTtsEndPayload struct {
	RequestID string `json:"request_id"`
	StreamID  uint32 `json:"stream_id"`
	OK        bool   `json:"ok"`
	Error     string `json:"error,omitempty"`
}

type ezTextInputPayload struct {
	ClientInputID    string `json:"client_input_id"`
	Text             string `json:"text"`
	OutputPreference string `json:"output_preference"`
}

type ezInputAcceptedPayload struct {
	ClientInputID string `json:"client_input_id"`
	InputID       string `json:"input_id"`
	RunID         string `json:"run_id"`
	QueueState    string `json:"queue_state"`
}

type ezTextOutputPayload struct {
	OutputID string `json:"output_id"`
	RunID    string `json:"run_id"`
	Text     string `json:"text"`
}

type ezStateChangedPayload struct {
	RunID         string `json:"run_id"`
	ClientInputID string `json:"client_input_id"`
	State         string `json:"state"`
	Reason        string `json:"reason"`
}

type ezErrorPayload struct {
	Code                 string `json:"code"`
	CorrelationMessageID string `json:"correlation_message_id"`
	Recoverable          bool   `json:"recoverable"`
}

type ezSessionConn struct {
	epoch   uint64
	conn    *websocket.Conn
	closeFn func()
	writeMu sync.Mutex
}

func (c *ezSessionConn) sendJSON(ctx context.Context, msgType string, payload any) (string, error) {
	payloadRaw, err := json.Marshal(payload)
	if err != nil {
		return "", fmt.Errorf("marshal %s payload: %w", msgType, err)
	}
	return c.sendRawPayload(ctx, msgType, payloadRaw)
}

func (c *ezSessionConn) sendRawPayload(ctx context.Context, msgType string, rawPayload json.RawMessage) (string, error) {
	msgID := uuid.NewString()
	env := ezWireEnvelope{
		ProtocolMajor: int(ezProtocolMajor),
		ProtocolMinor: int(ezProtocolMinor),
		MessageID:     msgID,
		Type:          msgType,
		Payload:       rawPayload,
	}
	encoded, err := json.Marshal(env)
	if err != nil {
		return "", fmt.Errorf("marshal %s envelope: %w", msgType, err)
	}
	if len(encoded) > ezMaxControlMessageSize {
		return "", fmt.Errorf("control message %s exceeds 64KiB (%d bytes)", msgType, len(encoded))
	}

	c.writeMu.Lock()
	defer c.writeMu.Unlock()

	writeCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	if err := c.conn.Write(writeCtx, websocket.MessageText, encoded); err != nil {
		return "", err
	}
	return msgID, nil
}

func (c *ezSessionConn) sendBinary(ctx context.Context, frame []byte) error {
	c.writeMu.Lock()
	defer c.writeMu.Unlock()

	writeCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	return c.conn.Write(writeCtx, websocket.MessageBinary, frame)
}

func (c *ezSessionConn) close() {
	if c.closeFn != nil {
		c.closeFn()
	}
}

type ezTurnOutcome struct {
	text string
	err  error
}

type ezTurnWaiter struct {
	clientInputID string
	inputMsgID    string
	runID         string
	accepted      bool
	createdAt     time.Time
	cb            biz.AgentStreamCallbacks
	doneCh        chan ezTurnOutcome
	once          sync.Once
}

func (w *ezTurnWaiter) resolve(err error) {
	w.once.Do(func() {
		w.doneCh <- ezTurnOutcome{err: err}
	})
}

func (w *ezTurnWaiter) resolveText(text string) {
	w.once.Do(func() {
		w.doneCh <- ezTurnOutcome{text: text}
	})
}

type ezSpeechASROutcome struct {
	text string
	err  error
}

type ezSpeechASRWaiter struct {
	requestID string
	msgID     string
	doneCh    chan ezSpeechASROutcome
	once      sync.Once
}

func (w *ezSpeechASRWaiter) resolve(text string, err error) {
	w.once.Do(func() {
		w.doneCh <- ezSpeechASROutcome{text: text, err: err}
	})
}

type ezSpeechTTSOutcome struct {
	pcm []byte
	err error
}

type ezSpeechTTSWaiter struct {
	requestID   string
	msgID       string
	streamID    uint32
	expectedSeq uint32
	started     bool
	pcm         bytes.Buffer
	doneCh      chan ezSpeechTTSOutcome
	once        sync.Once
}

func (w *ezSpeechTTSWaiter) resolve(pcm []byte, err error) {
	w.once.Do(func() {
		w.doneCh <- ezSpeechTTSOutcome{pcm: pcm, err: err}
	})
}

// EZDeviceBridgeImpl implements biz.EZDeviceBridge using Device Protocol 1.0 over TLS-pinned WSS.
type EZDeviceBridgeImpl struct {
	settings   biz.SettingsRepo
	log        *log.Helper
	mcpPort    int
	discoverFn func(ctx context.Context, timeout time.Duration) ([]biz.EZHostCandidate, error)

	mu           sync.Mutex
	started      bool
	rootCtx      context.Context
	rootCancel   context.CancelFunc
	pairCancel   context.CancelFunc
	wakeCh       chan struct{}
	mode         string
	status       biz.EZBridgeStatus
	identity     *persistedEZIdentity
	conn         *ezSessionConn
	connEpoch    atomic.Uint64
	nextStreamID atomic.Uint32
	speechStatus *biz.EZSpeechServicesStatus
	turns        map[string]*ezTurnWaiter
	asrWaiters   map[string]*ezSpeechASRWaiter
	ttsWaiters   map[uint32]*ezSpeechTTSWaiter
	ttsByReq     map[string]*ezSpeechTTSWaiter

	wg sync.WaitGroup
}

// NewEZDeviceBridge constructs a Device Protocol 1.0 EZ Assistant Desktop bridge.
func NewEZDeviceBridge(serverCfg *conf.Server, settings biz.SettingsRepo, logger log.Logger) biz.EZDeviceBridge {
	b := &EZDeviceBridgeImpl{
		settings:   settings,
		log:        log.NewHelper(logger),
		mcpPort:    parseEZServerHTTPPort(serverCfg),
		discoverFn: mdnsBrowseEZHosts,
		wakeCh:     make(chan struct{}, 1),
		mode:       biz.EZModeAuto,
		turns:      make(map[string]*ezTurnWaiter),
		asrWaiters: make(map[string]*ezSpeechASRWaiter),
		ttsWaiters: make(map[uint32]*ezSpeechTTSWaiter),
		ttsByReq:   make(map[string]*ezSpeechTTSWaiter),
		status: biz.EZBridgeStatus{
			Mode:          biz.EZModeAuto,
			ActiveBackend: biz.EZModeGRPC,
			State:         "unpaired",
			UpdatedAtMs:   time.Now().UnixMilli(),
		},
	}
	b.loadPersistedState(context.Background())
	return b
}

func parseEZServerHTTPPort(serverCfg *conf.Server) int {
	addr := ""
	if serverCfg != nil && serverCfg.GetHttp() != nil {
		addr = strings.TrimSpace(serverCfg.GetHttp().GetAddr())
	}
	if addr != "" {
		if _, portStr, err := net.SplitHostPort(addr); err == nil {
			if p, pErr := strconv.Atoi(portStr); pErr == nil && p > 0 && p <= 65535 {
				return p
			}
		}
	}
	return 8080
}

func isEZPairingState(state string) bool {
	return state == "pairing" || state == "awaiting_code" || state == "awaiting_confirm"
}

func ezPublicKeyFingerprint(pub []byte) string {
	sum := sha256.Sum256(pub)
	return fmt.Sprintf("%02X%02X-%02X%02X", sum[0], sum[1], sum[2], sum[3])
}

func normalizeRequestedPairingMode(raw string) (string, error) {
	mode := strings.ToLower(strings.TrimSpace(raw))
	switch mode {
	case "", "auto":
		return "auto", nil
	case "direct_confirm", "spake2":
		return mode, nil
	default:
		return "", fmt.Errorf("unsupported pairing_mode %q (expected auto, direct_confirm, or spake2)", raw)
	}
}

func parsePairingModesTXT(raw string) []string {
	if strings.TrimSpace(raw) == "" {
		return nil
	}
	parts := strings.Split(raw, ",")
	out := make([]string, 0, len(parts))
	for _, p := range parts {
		trimmed := strings.TrimSpace(p)
		if trimmed != "" {
			out = append(out, trimmed)
		}
	}
	if len(out) == 0 {
		return nil
	}
	return out
}

func (b *EZDeviceBridgeImpl) loadPersistedState(ctx context.Context) {
	if b.settings == nil {
		return
	}
	loadCtx, cancel := context.WithTimeout(ctx, 3*time.Second)
	defer cancel()

	modeRaw, err := b.settings.Get(loadCtx, biz.SettingKeyEZAgentMode)
	if err == nil {
		if norm, ok := normalizeEZMode(modeRaw); ok {
			b.mode = norm
		}
	}

	idRaw, err := b.settings.Get(loadCtx, biz.SettingKeyEZDeviceIdentity)
	if err == nil && strings.TrimSpace(idRaw) != "" {
		var id persistedEZIdentity
		if uErr := json.Unmarshal([]byte(idRaw), &id); uErr == nil && validatePersistedIdentity(&id) {
			b.identity = &id
			b.status.IdentityState = id.State
			b.status.DeviceID = id.DeviceID
			b.status.DisplayName = id.DisplayName
			b.status.InstallationID = id.InstallationID
			b.status.CertificateFingerprint = id.CertificateFingerprint
			b.status.HostAddress = id.HostAddress
			b.status.HostPath = id.HostPath
			if b.mode == biz.EZModeGRPC {
				b.status.State = "offline"
			} else {
				b.status.State = "connecting"
			}
		} else {
			b.log.WithContext(ctx).Warn("ignoring malformed persisted ez_device_identity")
		}
	}
	b.status.Mode = b.mode
	b.status.ActiveBackend = b.computeActiveBackendLocked()
	b.status.UpdatedAtMs = time.Now().UnixMilli()
}

func (b *EZDeviceBridgeImpl) Start(ctx context.Context) error {
	b.mu.Lock()
	if b.started {
		b.mu.Unlock()
		return nil
	}
	b.started = true
	b.rootCtx, b.rootCancel = context.WithCancel(context.Background())
	b.wg.Add(1)
	go b.supervisorLoop(b.rootCtx)
	b.mu.Unlock()
	b.notifySupervisor()
	b.log.WithContext(ctx).Infof("EZDeviceBridge started (mode=%s identity_state=%s)", b.mode, b.status.IdentityState)
	return nil
}

func (b *EZDeviceBridgeImpl) Stop(ctx context.Context) error {
	b.mu.Lock()
	if !b.started {
		b.mu.Unlock()
		return nil
	}
	b.started = false
	if b.pairCancel != nil {
		b.pairCancel()
		b.pairCancel = nil
	}
	if b.rootCancel != nil {
		b.rootCancel()
	}
	if b.conn != nil {
		b.conn.close()
		b.conn = nil
	}
	b.speechStatus = nil
	b.status.SpeechServices = nil
	b.failAllTurnsLocked(errors.New("ez-assistant bridge stopping"))
	b.mu.Unlock()

	done := make(chan struct{})
	go func() {
		b.wg.Wait()
		close(done)
	}()

	select {
	case <-done:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

func (b *EZDeviceBridgeImpl) Status(ctx context.Context) biz.EZBridgeStatus {
	b.mu.Lock()
	defer b.mu.Unlock()
	st := b.status
	st.ActiveBackend = b.computeActiveBackendLocked()
	if b.speechStatus != nil {
		cp := *b.speechStatus
		st.SpeechServices = &cp
	} else {
		st.SpeechServices = nil
	}
	return st
}

func (b *EZDeviceBridgeImpl) EffectiveMode(ctx context.Context) string {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.mode == "" {
		return biz.EZModeAuto
	}
	return b.mode
}

func (b *EZDeviceBridgeImpl) IsOnline() bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.conn != nil && b.status.State == "online" && b.mode != biz.EZModeGRPC
}

func (b *EZDeviceBridgeImpl) CanUseASR() bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.conn != nil &&
		b.status.State == "online" &&
		b.mode != biz.EZModeGRPC &&
		b.speechStatus != nil &&
		strings.EqualFold(b.speechStatus.ASR, "ready")
}

func (b *EZDeviceBridgeImpl) CanUseTTS() bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.conn != nil &&
		b.status.State == "online" &&
		b.mode != biz.EZModeGRPC &&
		b.speechStatus != nil &&
		strings.EqualFold(b.speechStatus.TTS, "ready")
}

func (b *EZDeviceBridgeImpl) computeActiveBackendLocked() string {
	switch b.mode {
	case biz.EZModeDevice:
		return biz.EZModeDevice
	case biz.EZModeGRPC:
		return biz.EZModeGRPC
	default:
		if b.conn != nil && b.status.State == "online" {
			return biz.EZModeDevice
		}
		return biz.EZModeGRPC
	}
}

func (b *EZDeviceBridgeImpl) SetMode(ctx context.Context, mode string) (biz.EZBridgeStatus, error) {
	norm, ok := normalizeEZMode(mode)
	if !ok {
		return biz.EZBridgeStatus{}, fmt.Errorf("invalid ez_agent_mode %q (want auto, ez_device, or grpc)", mode)
	}
	if b.settings != nil {
		if err := b.settings.Set(ctx, biz.SettingKeyEZAgentMode, norm); err != nil {
			return biz.EZBridgeStatus{}, fmt.Errorf("persist ez_agent_mode: %w", err)
		}
	}

	b.mu.Lock()
	b.mode = norm
	b.status.Mode = norm
	if norm == biz.EZModeGRPC && b.conn != nil {
		b.conn.close()
		b.conn = nil
	}
	if b.identity == nil && !isEZPairingState(b.status.State) {
		b.status.State = "unpaired"
	} else if norm == biz.EZModeGRPC && b.identity != nil {
		b.status.State = "offline"
	} else if b.identity != nil && b.status.State == "offline" {
		b.status.State = "connecting"
	}
	b.status.ActiveBackend = b.computeActiveBackendLocked()
	b.status.UpdatedAtMs = time.Now().UnixMilli()
	st := b.status
	b.mu.Unlock()

	b.notifySupervisor()
	return st, nil
}

func (b *EZDeviceBridgeImpl) DiscoverHosts(ctx context.Context, timeout time.Duration) ([]biz.EZHostCandidate, error) {
	if timeout <= 0 {
		timeout = 2500 * time.Millisecond
	}
	hosts, err := b.discoverFn(ctx, timeout)
	if err != nil {
		return nil, err
	}
	if b.settings != nil {
		if manualRaw, gErr := b.settings.Get(ctx, biz.SettingKeyEZManualHost); gErr == nil && strings.TrimSpace(manualRaw) != "" {
			var manual biz.EZHostCandidate
			if json.Unmarshal([]byte(manualRaw), &manual) == nil && manual.CertificateFingerprint != "" && len(manual.Addresses) > 0 && manual.Port > 0 {
				dup := false
				for _, h := range hosts {
					if h.InstallationID == manual.InstallationID && h.Port == manual.Port {
						dup = true
						break
					}
				}
				if !dup {
					hosts = append(hosts, manual)
				}
			}
		}
	}
	sort.Slice(hosts, func(i, j int) bool {
		if hosts[i].InstanceName != hosts[j].InstanceName {
			return hosts[i].InstanceName < hosts[j].InstanceName
		}
		return hosts[i].Port < hosts[j].Port
	})
	return hosts, nil
}

func (b *EZDeviceBridgeImpl) StartPairing(ctx context.Context, req biz.EZPairRequest) (biz.EZBridgeStatus, error) {
	addr := strings.TrimSpace(req.Address)
	host, portStr, splitErr := net.SplitHostPort(addr)
	if splitErr != nil || strings.TrimSpace(host) == "" {
		return biz.EZBridgeStatus{}, errors.New("address must be host:port (e.g. 192.168.101.12:54321)")
	}
	port, portErr := strconv.Atoi(portStr)
	if portErr != nil || port <= 0 || port > 65535 {
		return biz.EZBridgeStatus{}, fmt.Errorf("invalid port in address %q", addr)
	}

	installID := strings.ToLower(strings.TrimSpace(req.InstallationID))
	if !isValidLowerHex(installID, 32) {
		return biz.EZBridgeStatus{}, errors.New("installation_id must be 32 lowercase hex characters")
	}

	certFP := normalizeFingerprintHex(req.CertificateFingerprint)
	if !isValidLowerHex(certFP, 64) {
		return biz.EZBridgeStatus{}, errors.New("certificate_fingerprint must be 64 hex characters (SHA-256 of leaf cert DER)")
	}

	path := normalizeWSPath(req.Path)
	displayName := strings.TrimSpace(req.DisplayName)
	if displayName == "" {
		displayName = "ESP-Pika Gateway"
	}
	if len(displayName) > 128 {
		displayName = displayName[:128]
	}

	requestedMode, err := normalizeRequestedPairingMode(req.PairingMode)
	if err != nil {
		return biz.EZBridgeStatus{}, err
	}

	var (
		code        string
		pubKey      ed25519.PublicKey
		privKey     ed25519.PrivateKey
		statusMode  string
		fingerprint string
	)
	if requestedMode == "spake2" {
		code, err = generate6DigitPairingCode()
		if err != nil {
			return biz.EZBridgeStatus{}, err
		}
		statusMode = "spake2"
	} else {
		pubKey, privKey, err = ed25519.GenerateKey(rand.Reader)
		if err != nil {
			return biz.EZBridgeStatus{}, fmt.Errorf("generate Ed25519 identity: %w", err)
		}
		statusMode = "direct_confirm"
		fingerprint = ezPublicKeyFingerprint(pubKey)
	}

	if b.settings != nil {
		manual, _ := json.Marshal(biz.EZHostCandidate{
			InstanceName:           "Manual EZ Assistant",
			HostName:               host,
			Addresses:              []string{host},
			Port:                   port,
			Path:                   path,
			ProtocolMajor:          int(ezProtocolMajor),
			ProtocolMinor:          int(ezProtocolMinor),
			InstallationID:         installID,
			CertificateFingerprint: certFP,
			PairingAvailable:       true,
			PairingModes:           parsePairingModesTXT(statusMode),
		})
		_ = b.settings.Set(ctx, biz.SettingKeyEZManualHost, string(manual))
	}

	b.mu.Lock()
	if b.pairCancel != nil {
		b.pairCancel()
		b.pairCancel = nil
	}
	if b.conn != nil {
		b.conn.close()
		b.conn = nil
	}
	baseCtx := b.rootCtx
	if baseCtx == nil {
		baseCtx = context.Background()
	}
	pairCtx, pairCancel := context.WithCancel(baseCtx)
	b.pairCancel = pairCancel
	b.status.State = "pairing"
	b.status.DisplayName = displayName
	b.status.InstallationID = installID
	b.status.CertificateFingerprint = certFP
	b.status.HostAddress = addr
	b.status.HostPath = path
	b.status.PairingMode = statusMode
	b.status.PairingCode = code
	b.status.KeyFingerprint = fingerprint
	b.status.PairingExpiresAtMs = time.Now().Add(ezDefaultPairingWindow).UnixMilli()
	b.status.LastError = ""
	b.status.ActiveBackend = b.computeActiveBackendLocked()
	b.status.UpdatedAtMs = time.Now().UnixMilli()
	st := b.status

	b.wg.Add(1)
	b.mu.Unlock()

	go b.runPairingWorker(pairCtx, addr, path, installID, certFP, displayName, requestedMode, code, pubKey, privKey)
	return st, nil
}

func (b *EZDeviceBridgeImpl) ResetPairing(ctx context.Context) (biz.EZBridgeStatus, error) {
	b.mu.Lock()
	if b.pairCancel != nil {
		b.pairCancel()
		b.pairCancel = nil
	}
	if b.conn != nil {
		b.conn.close()
		b.conn = nil
	}
	b.failAllTurnsLocked(errors.New("ez-assistant pairing reset"))
	b.identity = nil
	b.status = biz.EZBridgeStatus{
		Mode:          b.mode,
		ActiveBackend: b.computeActiveBackendLocked(),
		State:         "unpaired",
		IdentityState: "",
		UpdatedAtMs:   time.Now().UnixMilli(),
	}
	st := b.status
	b.mu.Unlock()

	if b.settings != nil {
		if err := b.settings.Set(ctx, biz.SettingKeyEZDeviceIdentity, ""); err != nil {
			return st, fmt.Errorf("clear ez_device_identity: %w", err)
		}
	}
	b.notifySupervisor()
	return st, nil
}

func (b *EZDeviceBridgeImpl) RunTurn(ctx context.Context, req *biz.AgentTurnRequest, cb biz.AgentStreamCallbacks) error {
	if req == nil {
		return errors.New("nil agent turn request")
	}
	text := strings.TrimSpace(req.Text)
	if text == "" {
		return errors.New("empty user speech text")
	}
	if len(text) > 49152 {
		text = text[:49152]
	}

	b.mu.Lock()
	if b.mode == biz.EZModeGRPC || b.conn == nil || b.status.State != "online" {
		b.mu.Unlock()
		return errors.New("ez-assistant device bridge is not online")
	}
	sessConn := b.conn

	clientInputID := strings.TrimSpace(req.TurnID)
	if clientInputID == "" {
		clientInputID = "pika-" + uuid.NewString()
	}
	if len(clientInputID) > 256 {
		clientInputID = clientInputID[:256]
	}
	waiter := &ezTurnWaiter{
		clientInputID: clientInputID,
		createdAt:     time.Now(),
		cb:            cb,
		doneCh:        make(chan ezTurnOutcome, 1),
	}
	b.turns[clientInputID] = waiter
	b.mu.Unlock()

	defer func() {
		b.mu.Lock()
		if existing, ok := b.turns[clientInputID]; ok && existing == waiter {
			delete(b.turns, clientInputID)
		}
		b.mu.Unlock()
	}()

	if cb.OnStatus != nil {
		cb.OnStatus("sending", "已发送至 EZ Assistant", false)
	}
	b.log.WithContext(ctx).Infof("EZ Assistant submitting text_input client_input_id=%s text=%q", clientInputID, text)
	msgID, err := sessConn.sendJSON(ctx, "text_input", ezTextInputPayload{
		ClientInputID:    clientInputID,
		Text:             text,
		OutputPreference: "text",
	})
	if err != nil {
		return fmt.Errorf("send text_input to ez-assistant: %w", err)
	}

	b.mu.Lock()
	if existing, ok := b.turns[clientInputID]; ok && existing == waiter {
		existing.inputMsgID = msgID
	}
	b.mu.Unlock()

	timer := time.NewTimer(ezDefaultTurnTimeout)
	defer timer.Stop()

	select {
	case outcome := <-waiter.doneCh:
		timer.Stop()
		if outcome.err != nil {
			return outcome.err
		}
		if cb.OnTextDelta != nil {
			cb.OnTextDelta(outcome.text, "")
		}
		if cb.OnDone != nil {
			cb.OnDone("completed", "")
		}
		return nil
	case <-ctx.Done():
		return ctx.Err()
	case <-timer.C:
		return errors.New("ez-assistant response timed out")
	}
}

func (b *EZDeviceBridgeImpl) SendEvent(_ context.Context, _ *biz.AgentDeviceEvent, _ biz.AgentStreamCallbacks) error {
	// Pure-text Device Protocol 1.0 sessions only accept explicit user inputs.
	return nil
}

func (b *EZDeviceBridgeImpl) CancelTurn(_ context.Context, _ string, turnID string, reason string) error {
	b.mu.Lock()
	defer b.mu.Unlock()

	err := context.Canceled
	if strings.TrimSpace(reason) != "" {
		err = fmt.Errorf("turn cancelled: %s", reason)
	}
	if strings.TrimSpace(turnID) != "" {
		if w, ok := b.turns[turnID]; ok {
			delete(b.turns, turnID)
			w.resolve(err)
		}
		return nil
	}
	b.failAllTurnsLocked(err)
	return nil
}

func (b *EZDeviceBridgeImpl) failAllTurnsLocked(err error) {
	for id, w := range b.turns {
		delete(b.turns, id)
		w.resolve(err)
	}
	for id, w := range b.asrWaiters {
		delete(b.asrWaiters, id)
		w.resolve("", err)
	}
	for reqID, w := range b.ttsByReq {
		delete(b.ttsByReq, reqID)
		delete(b.ttsWaiters, w.streamID)
		w.resolve(nil, err)
	}
	for streamID, w := range b.ttsWaiters {
		delete(b.ttsWaiters, streamID)
		w.resolve(nil, err)
	}
}

func (b *EZDeviceBridgeImpl) allocRelayStreamID() uint32 {
	for {
		id := b.nextStreamID.Add(1)
		if id != 0 {
			return id
		}
	}
}

// RecognizeSpeech relays 16 kHz mono PCM16 audio to EZ Assistant Desktop ASR via WSS binary kind=3 frames.
func (b *EZDeviceBridgeImpl) RecognizeSpeech(ctx context.Context, pcm16k []byte) (string, error) {
	if len(pcm16k) == 0 {
		return "", nil
	}
	if len(pcm16k)%2 != 0 || len(pcm16k) > ezRelayMaxPCMBytes {
		return "", fmt.Errorf("invalid relay ASR PCM length %d", len(pcm16k))
	}

	b.mu.Lock()
	if b.conn == nil || b.status.State != "online" || b.mode == biz.EZModeGRPC ||
		b.speechStatus == nil || !strings.EqualFold(b.speechStatus.ASR, "ready") {
		b.mu.Unlock()
		return "", errors.New("ez-assistant speech relay ASR unavailable")
	}
	sessConn := b.conn
	requestID := uuid.NewString()
	streamID := b.allocRelayStreamID()
	waiter := &ezSpeechASRWaiter{
		requestID: requestID,
		doneCh:    make(chan ezSpeechASROutcome, 1),
	}
	b.asrWaiters[requestID] = waiter
	b.mu.Unlock()

	cleanupWaiter := func() {
		b.mu.Lock()
		if cur, ok := b.asrWaiters[requestID]; ok && cur == waiter {
			delete(b.asrWaiters, requestID)
		}
		b.mu.Unlock()
	}

	startMsgID, err := sessConn.sendJSON(ctx, "speech_asr_start", ezSpeechAsrStartPayload{
		RequestID: requestID,
		StreamID:  streamID,
		Format: ezSpeechPCMFormat{
			Encoding:        "pcm_s16le",
			SampleRateHz:    16000,
			Channels:        1,
			FrameDurationMs: 0,
		},
	})
	if err != nil {
		cleanupWaiter()
		return "", fmt.Errorf("send speech_asr_start: %w", err)
	}

	b.mu.Lock()
	waiter.msgID = startMsgID
	b.mu.Unlock()

	var seq uint32
	for offset := 0; offset < len(pcm16k); offset += ezRelayMaxUplinkChunk {
		end := min(offset+ezRelayMaxUplinkChunk, len(pcm16k))
		chunk := pcm16k[offset:end]
		frame := make([]byte, ezRelayHeaderBytes+len(chunk))
		frame[0] = 1
		frame[1] = ezRelayKindUplink
		binary.BigEndian.PutUint32(frame[4:8], streamID)
		binary.BigEndian.PutUint32(frame[8:12], seq)
		binary.BigEndian.PutUint16(frame[12:14], uint16(len(chunk)))
		copy(frame[ezRelayHeaderBytes:], chunk)
		if err := sessConn.sendBinary(ctx, frame); err != nil {
			cleanupWaiter()
			return "", fmt.Errorf("send speech relay uplink frame: %w", err)
		}
		seq++
	}

	stopMsgID, err := sessConn.sendJSON(ctx, "speech_asr_stop", ezSpeechAsrStopPayload{
		RequestID:    requestID,
		StreamID:     streamID,
		LastSequence: seq - 1,
	})
	if err != nil {
		cleanupWaiter()
		return "", fmt.Errorf("send speech_asr_stop: %w", err)
	}

	b.mu.Lock()
	waiter.msgID = stopMsgID
	b.mu.Unlock()

	select {
	case <-ctx.Done():
		cleanupWaiter()
		cancelCtx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
		_, _ = sessConn.sendJSON(cancelCtx, "speech_asr_cancel", ezSpeechAsrCancelPayload{RequestID: requestID})
		cancel()
		return "", ctx.Err()
	case out := <-waiter.doneCh:
		cleanupWaiter()
		return out.text, out.err
	}
}

// SynthesizeSpeech requests TTS synthesis from EZ Assistant Desktop via WSS and collects kind=4 PCM16 frames.
func (b *EZDeviceBridgeImpl) SynthesizeSpeech(ctx context.Context, text string) ([]byte, error) {
	trimmed := strings.TrimSpace(text)
	if trimmed == "" {
		return nil, errors.New("empty TTS text")
	}
	if utf8.RuneCountInString(trimmed) > relayMaxTTSSegmentRunes {
		return nil, fmt.Errorf("TTS segment exceeds %d runes", relayMaxTTSSegmentRunes)
	}

	b.mu.Lock()
	if b.conn == nil || b.status.State != "online" || b.mode == biz.EZModeGRPC ||
		b.speechStatus == nil || !strings.EqualFold(b.speechStatus.TTS, "ready") {
		b.mu.Unlock()
		return nil, errors.New("ez-assistant speech relay TTS unavailable")
	}
	sessConn := b.conn
	requestID := uuid.NewString()
	streamID := b.allocRelayStreamID()
	waiter := &ezSpeechTTSWaiter{
		requestID: requestID,
		streamID:  streamID,
		doneCh:    make(chan ezSpeechTTSOutcome, 1),
	}
	b.ttsWaiters[streamID] = waiter
	b.ttsByReq[requestID] = waiter
	b.mu.Unlock()

	cleanupWaiter := func() {
		b.mu.Lock()
		if cur, ok := b.ttsByReq[requestID]; ok && cur == waiter {
			delete(b.ttsByReq, requestID)
		}
		if cur, ok := b.ttsWaiters[streamID]; ok && cur == waiter {
			delete(b.ttsWaiters, streamID)
		}
		b.mu.Unlock()
	}

	msgID, err := sessConn.sendJSON(ctx, "speech_tts_request", ezSpeechTtsRequestPayload{
		RequestID: requestID,
		StreamID:  streamID,
		Text:      trimmed,
	})
	if err != nil {
		cleanupWaiter()
		return nil, fmt.Errorf("send speech_tts_request: %w", err)
	}

	b.mu.Lock()
	waiter.msgID = msgID
	b.mu.Unlock()

	select {
	case <-ctx.Done():
		cleanupWaiter()
		return nil, ctx.Err()
	case out := <-waiter.doneCh:
		cleanupWaiter()
		return out.pcm, out.err
	}
}

func (b *EZDeviceBridgeImpl) notifySupervisor() {
	select {
	case b.wakeCh <- struct{}{}:
	default:
	}
}

func (b *EZDeviceBridgeImpl) runPairingWorker(
	pairCtx context.Context,
	addr, path, installID, certFP, displayName, requestedMode, code string,
	pubKey ed25519.PublicKey,
	privKey ed25519.PrivateKey,
) {
	defer b.wg.Done()

	var err error
	switch requestedMode {
	case "spake2":
		err = b.executeSpake2PairingFlow(pairCtx, addr, path, installID, certFP, displayName, code)
	case "direct_confirm":
		err = b.executeDirectPairingFlow(pairCtx, addr, path, installID, certFP, displayName, pubKey, privKey)
	default:
		err = b.executeDirectPairingFlow(pairCtx, addr, path, installID, certFP, displayName, pubKey, privKey)
		if errors.Is(err, errDirectPairingUnsupported) && pairCtx.Err() == nil {
			b.log.Infof("ez-assistant host rejected direct_confirm (install_id=%s), falling back to SPAKE2: %v", installID, err)
			fallbackCode, genErr := generate6DigitPairingCode()
			if genErr != nil {
				err = genErr
			} else {
				b.mu.Lock()
				if isEZPairingState(b.status.State) {
					b.status.State = "pairing"
					b.status.PairingMode = "spake2"
					b.status.PairingCode = fallbackCode
					b.status.KeyFingerprint = ""
					b.status.PairingExpiresAtMs = time.Now().Add(ezDefaultPairingWindow).UnixMilli()
					b.status.LastError = "桌面端版本较旧，已自动切换为 6 位配对码模式"
					b.status.UpdatedAtMs = time.Now().UnixMilli()
				}
				b.mu.Unlock()
				err = b.executeSpake2PairingFlow(pairCtx, addr, path, installID, certFP, displayName, fallbackCode)
			}
		}
	}

	if err != nil && !errors.Is(err, context.Canceled) {
		b.log.Warnf("ez-assistant pairing failed (install_id=%s): %v", installID, err)
		b.mu.Lock()
		if isEZPairingState(b.status.State) {
			if b.identity == nil {
				b.status.State = "unpaired"
			} else {
				b.status.State = "connecting"
			}
			b.status.PairingMode = ""
			b.status.PairingCode = ""
			b.status.KeyFingerprint = ""
			b.status.PairingExpiresAtMs = 0
			b.status.LastError = err.Error()
			b.status.ActiveBackend = b.computeActiveBackendLocked()
			b.status.UpdatedAtMs = time.Now().UnixMilli()
		}
		b.mu.Unlock()
		b.notifySupervisor()
	}
}

var (
	errPairingNotOpen           = errors.New("pairing_not_open")
	errDirectPairingUnsupported = errors.New("direct_pairing_unsupported")
)

func isDirectUnsupportedErrorCode(code string) bool {
	return code == "invalid_message" || code == "invalid_payload" || code == "unsupported_pairing_mode"
}

func waitDeadlineForPending(pending ezPairingPendingPayload) time.Duration {
	waitDeadline := ezDefaultPairingWindow
	if pending.ExpiresAtMs > 0 {
		until := time.Until(time.UnixMilli(pending.ExpiresAtMs)) + 5*time.Second
		if until > 10*time.Second && until <= 10*time.Minute {
			waitDeadline = until
		}
	}
	return waitDeadline
}

func (b *EZDeviceBridgeImpl) updatePairingAwaitingState(waitingState string, expiresAtMs int64, lastError string) {
	b.mu.Lock()
	b.status.State = waitingState
	b.status.LastError = lastError
	if expiresAtMs > 0 {
		b.status.PairingExpiresAtMs = expiresAtMs
	}
	b.status.UpdatedAtMs = time.Now().UnixMilli()
	b.mu.Unlock()
}

func (b *EZDeviceBridgeImpl) retryPairingUntilOpen(
	ctx context.Context,
	addr, path, installID, certFP, waitingState, waitingHint string,
	dialAttempt func(curAddr, curPath string) error,
) (string, string, error) {
	const maxPairingNotOpenAttempts = 45
	refreshedPortOnce := false
	for attempt := 0; attempt < maxPairingNotOpenAttempts; attempt++ {
		if ctx.Err() != nil {
			return addr, path, ctx.Err()
		}
		err := dialAttempt(addr, path)
		if err == nil {
			return addr, path, nil
		}
		if !errors.Is(err, errPairingNotOpen) {
			if !refreshedPortOnce && !errors.Is(err, errDirectPairingUnsupported) {
				if resolvedAddr, resolvedPath, ok := b.resolveHostByInstallID(ctx, installID, certFP); ok && resolvedAddr != addr {
					refreshedPortOnce = true
					b.log.Infof("ez-assistant port updated via mDNS during pairing: %s -> %s", addr, resolvedAddr)
					addr = resolvedAddr
					path = resolvedPath
					b.mu.Lock()
					b.status.HostAddress = resolvedAddr
					b.status.HostPath = resolvedPath
					b.status.UpdatedAtMs = time.Now().UnixMilli()
					b.mu.Unlock()
					continue
				}
			}
			return addr, path, err
		}

		b.updatePairingAwaitingState(waitingState, 0, waitingHint)

		if attempt == maxPairingNotOpenAttempts-1 {
			return addr, path, errors.New("host pairing_not_open timed out (please click Add Device in EZ Assistant Desktop)")
		}
		timer := time.NewTimer(2 * time.Second)
		select {
		case <-ctx.Done():
			timer.Stop()
			return addr, path, ctx.Err()
		case <-timer.C:
		}
	}
	return addr, path, errors.New("host pairing_not_open timed out")
}

func (b *EZDeviceBridgeImpl) readPairingComplete(ctx context.Context, sessConn *ezSessionConn, pairingRequestID, deviceID string) (ezPairingCompletePayload, error) {
	completeEnv, err := readNextControlFrame(ctx, sessConn, ezStepReadTimeout)
	if err != nil {
		b.notifySupervisor()
		return ezPairingCompletePayload{}, fmt.Errorf("read pairing_complete (pending identity retained): %w", err)
	}
	if completeEnv.Type == "error" {
		var ep ezErrorPayload
		_ = json.Unmarshal(completeEnv.Payload, &ep)
		return ezPairingCompletePayload{}, fmt.Errorf("pairing commit rejected: %s", ep.Code)
	}
	if completeEnv.Type != "pairing_complete" {
		return ezPairingCompletePayload{}, fmt.Errorf("expected pairing_complete, got %q", completeEnv.Type)
	}
	var complete ezPairingCompletePayload
	if err := json.Unmarshal(completeEnv.Payload, &complete); err != nil {
		return ezPairingCompletePayload{}, fmt.Errorf("decode pairing_complete: %w", err)
	}
	if complete.PairingRequestID != pairingRequestID || complete.DeviceID != deviceID {
		return ezPairingCompletePayload{}, errors.New("pairing_complete ID mismatch")
	}
	return complete, nil
}

func (b *EZDeviceBridgeImpl) activatePairedIdentity(ctx context.Context, pendingID persistedEZIdentity, finalDisplayName string) error {
	activeID := pendingID
	activeID.State = "active"
	activeID.DisplayName = finalDisplayName
	activeID.UpdatedAtMs = time.Now().UnixMilli()
	if err := b.saveIdentity(ctx, &activeID); err != nil {
		return fmt.Errorf("persist active identity: %w", err)
	}

	b.mu.Lock()
	b.identity = &activeID
	b.status.State = "connecting"
	b.status.IdentityState = "active"
	b.status.DisplayName = activeID.DisplayName
	b.status.PairingMode = ""
	b.status.PairingCode = ""
	b.status.KeyFingerprint = ""
	b.status.PairingExpiresAtMs = 0
	b.status.LastError = ""
	b.status.ActiveBackend = b.computeActiveBackendLocked()
	b.status.UpdatedAtMs = time.Now().UnixMilli()
	b.mu.Unlock()

	b.log.Infof("EZ Assistant pairing complete! device_id=%s install_id=%s", activeID.DeviceID, activeID.InstallationID)
	b.notifySupervisor()
	return nil
}

func (b *EZDeviceBridgeImpl) openDirectPairingPendingSession(
	ctx context.Context,
	addr, path, certFP, displayName string,
	pubKey ed25519.PublicKey,
) (*ezSessionConn, string, string, ezCapabilities, ezPairingPendingPayload, error) {
	dialCtx, cancelDial := context.WithTimeout(ctx, 15*time.Second)
	sessConn, err := dialPinnedWSS(dialCtx, addr, path, certFP, 0)
	cancelDial()
	if err != nil {
		return nil, "", "", ezCapabilities{}, ezPairingPendingPayload{}, fmt.Errorf("connect pinned WSS %s%s: %w", addr, path, err)
	}
	closeOnErr := func() { sessConn.close() }

	challengeEnv, err := readNextControlFrame(ctx, sessConn, ezStepReadTimeout)
	if err != nil {
		closeOnErr()
		return nil, "", "", ezCapabilities{}, ezPairingPendingPayload{}, fmt.Errorf("read auth_challenge: %w", err)
	}
	if challengeEnv.Type != "auth_challenge" {
		closeOnErr()
		return nil, "", "", ezCapabilities{}, ezPairingPendingPayload{}, fmt.Errorf("expected auth_challenge, got %q", challengeEnv.Type)
	}

	pairingRequestID := "pair-" + uuid.NewString()
	deviceNonce, err := randomNonceB64U(32)
	if err != nil {
		closeOnErr()
		return nil, "", "", ezCapabilities{}, ezPairingPendingPayload{}, err
	}
	caps := defaultGatewayEZCapabilities()

	if _, err := sessConn.sendJSON(ctx, "pairing_hello", ezPairingHelloPayload{
		PairingRequestID: pairingRequestID,
		DisplayName:      displayName,
		DeviceNonce:      deviceNonce,
		Capabilities:     caps,
		PairingMode:      "direct_confirm",
		PublicKey:        b64uEncode(pubKey),
		ClientVersion:    ezClientVersion,
	}); err != nil {
		closeOnErr()
		return nil, "", "", ezCapabilities{}, ezPairingPendingPayload{}, fmt.Errorf("send direct pairing_hello: %w", err)
	}

	pendingEnv, err := readNextControlFrame(ctx, sessConn, ezStepReadTimeout)
	if err != nil {
		closeOnErr()
		if websocket.CloseStatus(err) != -1 || strings.Contains(err.Error(), "EOF") {
			return nil, "", "", ezCapabilities{}, ezPairingPendingPayload{}, fmt.Errorf("%w: %v", errDirectPairingUnsupported, err)
		}
		return nil, "", "", ezCapabilities{}, ezPairingPendingPayload{}, fmt.Errorf("read pairing_pending: %w", err)
	}
	if pendingEnv.Type == "error" {
		closeOnErr()
		var ep ezErrorPayload
		_ = json.Unmarshal(pendingEnv.Payload, &ep)
		if ep.Code == "pairing_not_open" {
			return nil, "", "", ezCapabilities{}, ezPairingPendingPayload{}, errPairingNotOpen
		}
		if isDirectUnsupportedErrorCode(ep.Code) {
			return nil, "", "", ezCapabilities{}, ezPairingPendingPayload{}, fmt.Errorf("%w: %s", errDirectPairingUnsupported, ep.Code)
		}
		return nil, "", "", ezCapabilities{}, ezPairingPendingPayload{}, fmt.Errorf("host rejected pairing: %s", ep.Code)
	}
	if pendingEnv.Type != "pairing_pending" {
		closeOnErr()
		return nil, "", "", ezCapabilities{}, ezPairingPendingPayload{}, fmt.Errorf("expected pairing_pending, got %q", pendingEnv.Type)
	}
	var pending ezPairingPendingPayload
	if err := json.Unmarshal(pendingEnv.Payload, &pending); err != nil {
		closeOnErr()
		return nil, "", "", ezCapabilities{}, ezPairingPendingPayload{}, fmt.Errorf("decode pairing_pending: %w", err)
	}
	if pending.PairingRequestID != pairingRequestID {
		closeOnErr()
		return nil, "", "", ezCapabilities{}, ezPairingPendingPayload{}, errors.New("pairing_pending pairing_request_id mismatch")
	}
	return sessConn, pairingRequestID, deviceNonce, caps, pending, nil
}

func (b *EZDeviceBridgeImpl) executeDirectPairingFlow(
	ctx context.Context,
	addr, path, installID, certFP, displayName string,
	pubKey ed25519.PublicKey,
	privKey ed25519.PrivateKey,
) error {
	var (
		sessConn         *ezSessionConn
		pairingRequestID string
		deviceNonce      string
		caps             ezCapabilities
		pending          ezPairingPendingPayload
	)
	finalAddr, finalPath, err := b.retryPairingUntilOpen(
		ctx, addr, path, installID, certFP,
		"awaiting_confirm", "等待桌面端点击「添加设备」开放配对窗口...",
		func(curAddr, curPath string) error {
			var openErr error
			sessConn, pairingRequestID, deviceNonce, caps, pending, openErr =
				b.openDirectPairingPendingSession(ctx, curAddr, curPath, certFP, displayName, pubKey)
			return openErr
		},
	)
	if err != nil {
		return err
	}
	addr, path = finalAddr, finalPath
	defer sessConn.close()

	waitDeadline := waitDeadlineForPending(pending)
	b.updatePairingAwaitingState("awaiting_confirm", pending.ExpiresAtMs, "")

	chEnv, err := readNextControlFrame(ctx, sessConn, waitDeadline)
	if err != nil {
		return fmt.Errorf("wait pairing_direct_challenge: %w", err)
	}
	if chEnv.Type == "error" {
		var ep ezErrorPayload
		_ = json.Unmarshal(chEnv.Payload, &ep)
		return fmt.Errorf("pairing failed on host: %s", ep.Code)
	}
	if chEnv.Type != "pairing_direct_challenge" {
		return fmt.Errorf("expected pairing_direct_challenge, got %q", chEnv.Type)
	}

	var ch ezPairingDirectChallengePayload
	if err := json.Unmarshal(chEnv.Payload, &ch); err != nil {
		return fmt.Errorf("decode pairing_direct_challenge: %w", err)
	}
	if ch.PairingRequestID != pairingRequestID || ch.HostNonce == "" || strings.TrimSpace(ch.DeviceID) == "" {
		return errors.New("invalid pairing_direct_challenge fields")
	}

	ad := buildPairingAssociatedData(pairingRequestID, installID, certFP, deviceNonce, ch.HostNonce, caps)
	bt := buildBindTranscript(ad, pubKey)
	ct := buildCommitTranscript(bt, ch.DeviceID)
	seed := privKey.Seed()

	pendingID := persistedEZIdentity{
		State:                  "pending",
		InstallationID:         installID,
		CertificateFingerprint: certFP,
		HostAddress:            addr,
		HostPath:               path,
		DeviceID:               ch.DeviceID,
		DisplayName:            displayName,
		PrivateSeedB64U:        b64uEncode(seed),
		PublicKeyB64U:          b64uEncode(pubKey),
		UpdatedAtMs:            time.Now().UnixMilli(),
	}
	if err := b.saveIdentity(ctx, &pendingID); err != nil {
		return fmt.Errorf("persist pending identity: %w", err)
	}
	b.mu.Lock()
	b.identity = &pendingID
	b.status.IdentityState = "pending"
	b.status.DeviceID = pendingID.DeviceID
	b.status.UpdatedAtMs = time.Now().UnixMilli()
	b.mu.Unlock()

	sig, err := signEd25519Transcript(seed, ct)
	if err != nil {
		return err
	}
	if _, err := sessConn.sendJSON(ctx, "pairing_direct_commit", ezPairingDirectCommitPayload{
		PairingRequestID: pairingRequestID,
		DeviceID:         ch.DeviceID,
		Signature:        b64uEncode(sig),
	}); err != nil {
		return fmt.Errorf("send pairing_direct_commit: %w", err)
	}

	complete, err := b.readPairingComplete(ctx, sessConn, pairingRequestID, ch.DeviceID)
	if err != nil {
		return err
	}
	return b.activatePairedIdentity(ctx, pendingID, complete.DisplayName)
}

func (b *EZDeviceBridgeImpl) openPairingPendingSession(
	ctx context.Context,
	addr, path, installID, certFP, displayName, code string,
) (*ezSessionConn, string, string, *spake2PartyA, ezCapabilities, ezPairingPendingPayload, error) {
	dialCtx, cancelDial := context.WithTimeout(ctx, 15*time.Second)
	sessConn, err := dialPinnedWSS(dialCtx, addr, path, certFP, 0)
	cancelDial()
	if err != nil {
		return nil, "", "", nil, ezCapabilities{}, ezPairingPendingPayload{}, fmt.Errorf("connect pinned WSS %s%s: %w", addr, path, err)
	}
	closeOnErr := func() { sessConn.close() }

	challengeEnv, err := readNextControlFrame(ctx, sessConn, ezStepReadTimeout)
	if err != nil {
		closeOnErr()
		return nil, "", "", nil, ezCapabilities{}, ezPairingPendingPayload{}, fmt.Errorf("read auth_challenge: %w", err)
	}
	if challengeEnv.Type != "auth_challenge" {
		closeOnErr()
		return nil, "", "", nil, ezCapabilities{}, ezPairingPendingPayload{}, fmt.Errorf("expected auth_challenge, got %q", challengeEnv.Type)
	}

	pairingRequestID := "pair-" + uuid.NewString()
	deviceNonce, err := randomNonceB64U(32)
	if err != nil {
		closeOnErr()
		return nil, "", "", nil, ezCapabilities{}, ezPairingPendingPayload{}, err
	}
	partyA, err := newSPAKE2PartyA(code, pairingRequestID, installID)
	if err != nil {
		closeOnErr()
		return nil, "", "", nil, ezCapabilities{}, ezPairingPendingPayload{}, err
	}
	caps := defaultGatewayEZCapabilities()

	if _, err := sessConn.sendJSON(ctx, "pairing_hello", ezPairingHelloPayload{
		PairingRequestID: pairingRequestID,
		DisplayName:      displayName,
		DeviceNonce:      deviceNonce,
		Capabilities:     caps,
		PakeShare:        partyA.DeviceShareB64U(),
	}); err != nil {
		closeOnErr()
		return nil, "", "", nil, ezCapabilities{}, ezPairingPendingPayload{}, fmt.Errorf("send pairing_hello: %w", err)
	}

	pendingEnv, err := readNextControlFrame(ctx, sessConn, ezStepReadTimeout)
	if err != nil {
		closeOnErr()
		return nil, "", "", nil, ezCapabilities{}, ezPairingPendingPayload{}, fmt.Errorf("read pairing_pending: %w", err)
	}
	if pendingEnv.Type == "error" {
		closeOnErr()
		var ep ezErrorPayload
		_ = json.Unmarshal(pendingEnv.Payload, &ep)
		if ep.Code == "pairing_not_open" {
			return nil, "", "", nil, ezCapabilities{}, ezPairingPendingPayload{}, errPairingNotOpen
		}
		return nil, "", "", nil, ezCapabilities{}, ezPairingPendingPayload{}, fmt.Errorf("host rejected pairing: %s", ep.Code)
	}
	if pendingEnv.Type != "pairing_pending" {
		closeOnErr()
		return nil, "", "", nil, ezCapabilities{}, ezPairingPendingPayload{}, fmt.Errorf("expected pairing_pending, got %q", pendingEnv.Type)
	}
	var pending ezPairingPendingPayload
	if err := json.Unmarshal(pendingEnv.Payload, &pending); err != nil {
		closeOnErr()
		return nil, "", "", nil, ezCapabilities{}, ezPairingPendingPayload{}, fmt.Errorf("decode pairing_pending: %w", err)
	}
	if pending.PairingRequestID != pairingRequestID {
		closeOnErr()
		return nil, "", "", nil, ezCapabilities{}, ezPairingPendingPayload{}, errors.New("pairing_pending pairing_request_id mismatch")
	}
	return sessConn, pairingRequestID, deviceNonce, partyA, caps, pending, nil
}

func (b *EZDeviceBridgeImpl) executeSpake2PairingFlow(
	ctx context.Context,
	addr, path, installID, certFP, displayName, code string,
) error {
	var (
		sessConn         *ezSessionConn
		pairingRequestID string
		deviceNonce      string
		partyA           *spake2PartyA
		caps             ezCapabilities
		pending          ezPairingPendingPayload
	)
	finalAddr, finalPath, err := b.retryPairingUntilOpen(
		ctx, addr, path, installID, certFP,
		"awaiting_code", "等待桌面端点击「添加设备」开放配对窗口...",
		func(curAddr, curPath string) error {
			var openErr error
			sessConn, pairingRequestID, deviceNonce, partyA, caps, pending, openErr =
				b.openPairingPendingSession(ctx, curAddr, curPath, installID, certFP, displayName, code)
			return openErr
		},
	)
	if err != nil {
		return err
	}
	addr, path = finalAddr, finalPath
	defer sessConn.close()

	waitDeadline := waitDeadlineForPending(pending)
	b.updatePairingAwaitingState("awaiting_code", pending.ExpiresAtMs, "")

	pakeEnv, err := readNextControlFrame(ctx, sessConn, waitDeadline)
	if err != nil {
		return fmt.Errorf("wait pairing_pake: %w", err)
	}
	if pakeEnv.Type == "error" {
		var ep ezErrorPayload
		_ = json.Unmarshal(pakeEnv.Payload, &ep)
		return fmt.Errorf("pairing failed on host: %s", ep.Code)
	}
	if pakeEnv.Type != "pairing_pake" {
		return fmt.Errorf("expected pairing_pake, got %q", pakeEnv.Type)
	}

	var pake ezPairingPakePayload
	if err := json.Unmarshal(pakeEnv.Payload, &pake); err != nil {
		return fmt.Errorf("decode pairing_pake: %w", err)
	}
	if pake.PairingRequestID != pairingRequestID || pake.HostNonce == "" {
		return errors.New("invalid pairing_pake request_id or host_nonce")
	}
	hostShare, err := b64uDecodeExact(pake.PakeShare, 65)
	if err != nil {
		return fmt.Errorf("decode pairing_pake pake_share: %w", err)
	}
	hostConfWire, err := b64uDecodeExact(pake.ConfirmationMAC, 32)
	if err != nil {
		return fmt.Errorf("decode pairing_pake confirmation_mac: %w", err)
	}

	ad := buildPairingAssociatedData(pairingRequestID, installID, certFP, deviceNonce, pake.HostNonce, caps)
	sessKey, devConf, hostConf, err := partyA.Finish(hostShare, ad)
	if err != nil {
		return fmt.Errorf("SPAKE2 finish: %w", err)
	}
	if !constantTimeEqual(hostConfWire, hostConf[:]) {
		return errors.New("SPAKE2 host confirmation MAC mismatch (check 6-digit code)")
	}

	if _, err := sessConn.sendJSON(ctx, "pairing_confirm", ezPairingConfirmPayload{
		PairingRequestID: pairingRequestID,
		ConfirmationMAC:  b64uEncode(devConf[:]),
	}); err != nil {
		return fmt.Errorf("send pairing_confirm: %w", err)
	}

	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return fmt.Errorf("generate Ed25519 identity: %w", err)
	}
	seed := priv.Seed()
	bt := buildBindTranscript(ad, pub)
	bindSig, err := signEd25519Transcript(seed, bt)
	if err != nil {
		return err
	}
	bindMAC := computeDeviceBindMAC(sessKey[:], bt)

	if _, err := sessConn.sendJSON(ctx, "pairing_bind", ezPairingBindPayload{
		PairingRequestID: pairingRequestID,
		PublicKey:        b64uEncode(pub),
		Signature:        b64uEncode(bindSig),
		BindingMAC:       b64uEncode(bindMAC[:]),
	}); err != nil {
		return fmt.Errorf("send pairing_bind: %w", err)
	}

	bindAckEnv, err := readNextControlFrame(ctx, sessConn, ezStepReadTimeout)
	if err != nil {
		return fmt.Errorf("read pairing_bind_ack: %w", err)
	}
	if bindAckEnv.Type == "error" {
		var ep ezErrorPayload
		_ = json.Unmarshal(bindAckEnv.Payload, &ep)
		return fmt.Errorf("pairing_bind rejected: %s", ep.Code)
	}
	if bindAckEnv.Type != "pairing_bind_ack" {
		return fmt.Errorf("expected pairing_bind_ack, got %q", bindAckEnv.Type)
	}

	var bindAck ezPairingBindAckPayload
	if err := json.Unmarshal(bindAckEnv.Payload, &bindAck); err != nil {
		return fmt.Errorf("decode pairing_bind_ack: %w", err)
	}
	if bindAck.PairingRequestID != pairingRequestID || strings.TrimSpace(bindAck.DeviceID) == "" {
		return errors.New("invalid pairing_bind_ack fields")
	}
	hostProofWire, err := b64uDecodeExact(bindAck.HostProof, 32)
	if err != nil {
		return fmt.Errorf("decode host_proof: %w", err)
	}
	ct := buildCommitTranscript(bt, bindAck.DeviceID)
	expectedAckMAC := computeHostBindAckMAC(sessKey[:], ct)
	if !constantTimeEqual(hostProofWire, expectedAckMAC[:]) {
		return errors.New("pairing_bind_ack host_proof MAC mismatch")
	}

	// Atomically persist pending identity BEFORE sending pairing_commit per spec §4.3.
	pendingID := persistedEZIdentity{
		State:                  "pending",
		InstallationID:         installID,
		CertificateFingerprint: certFP,
		HostAddress:            addr,
		HostPath:               path,
		DeviceID:               bindAck.DeviceID,
		DisplayName:            displayName,
		PrivateSeedB64U:        b64uEncode(seed),
		PublicKeyB64U:          b64uEncode(pub),
		UpdatedAtMs:            time.Now().UnixMilli(),
	}
	if err := b.saveIdentity(ctx, &pendingID); err != nil {
		return fmt.Errorf("persist pending identity: %w", err)
	}
	b.mu.Lock()
	b.identity = &pendingID
	b.status.IdentityState = "pending"
	b.status.DeviceID = pendingID.DeviceID
	b.status.UpdatedAtMs = time.Now().UnixMilli()
	b.mu.Unlock()

	commitSig, err := signEd25519Transcript(seed, ct)
	if err != nil {
		return err
	}
	commitMAC := computeDeviceCommitMAC(sessKey[:], ct)

	if _, err := sessConn.sendJSON(ctx, "pairing_commit", ezPairingCommitPayload{
		PairingRequestID: pairingRequestID,
		DeviceID:         bindAck.DeviceID,
		Signature:        b64uEncode(commitSig),
		BindingMAC:       b64uEncode(commitMAC[:]),
	}); err != nil {
		return fmt.Errorf("send pairing_commit: %w", err)
	}

	complete, err := b.readPairingComplete(ctx, sessConn, pairingRequestID, bindAck.DeviceID)
	if err != nil {
		return err
	}
	return b.activatePairedIdentity(ctx, pendingID, complete.DisplayName)
}

func (b *EZDeviceBridgeImpl) supervisorLoop(ctx context.Context) {
	defer b.wg.Done()
	backoffIdx := 0

	for {
		if ctx.Err() != nil {
			return
		}

		b.mu.Lock()
		var idCopy *persistedEZIdentity
		if b.identity != nil {
			c := *b.identity
			idCopy = &c
		}
		mode := b.mode
		state := b.status.State
		b.mu.Unlock()

		// Idle while unpaired, pairing in progress, gRPC-only mode, or halted on revocation/auth error.
		if idCopy == nil || mode == biz.EZModeGRPC || isEZPairingState(state) || state == "revoked" || state == "auth_error" {
			select {
			case <-ctx.Done():
				return
			case <-b.wakeCh:
				backoffIdx = 0
				continue
			}
		}

		// Re-discover dynamic port via mDNS if previous attempt failed (backoffIdx > 0).
		if backoffIdx > 0 {
			if resolvedAddr, resolvedPath, ok := b.resolveHostByInstallID(ctx, idCopy.InstallationID, idCopy.CertificateFingerprint); ok {
				if resolvedAddr != idCopy.HostAddress || resolvedPath != idCopy.HostPath {
					idCopy.HostAddress = resolvedAddr
					idCopy.HostPath = resolvedPath
					idCopy.UpdatedAtMs = time.Now().UnixMilli()
					_ = b.saveIdentity(ctx, idCopy)
					b.mu.Lock()
					if b.identity != nil && b.identity.InstallationID == idCopy.InstallationID {
						b.identity.HostAddress = resolvedAddr
						b.identity.HostPath = resolvedPath
						b.status.HostAddress = resolvedAddr
						b.status.HostPath = resolvedPath
					}
					b.mu.Unlock()
				}
			}
		}

		connectedOnce, fatalStop, err := b.connectAndServe(ctx, idCopy)
		if ctx.Err() != nil {
			return
		}
		if connectedOnce {
			backoffIdx = 0
		}
		if fatalStop {
			b.log.Warnf("EZ Assistant connection halted cleanly: %v", err)
			continue
		}
		if err != nil {
			b.log.Warnf("EZ Assistant connection ended (%s): %v", idCopy.HostAddress, err)
		}

		b.mu.Lock()
		if b.status.State == "online" || b.status.State == "connecting" {
			b.status.State = "connecting"
			if err != nil {
				b.status.LastError = err.Error()
			}
			b.status.ActiveBackend = b.computeActiveBackendLocked()
			b.status.UpdatedAtMs = time.Now().UnixMilli()
		}
		b.mu.Unlock()

		delay := ezReconnectBackoffs[min(backoffIdx, len(ezReconnectBackoffs)-1)]
		if backoffIdx < len(ezReconnectBackoffs)-1 {
			backoffIdx++
		}
		// Add small jitter (+0..20%)
		jitter := time.Duration(time.Now().UnixNano() % int64(delay/5+1))
		timer := time.NewTimer(delay + jitter)
		select {
		case <-ctx.Done():
			timer.Stop()
			return
		case <-b.wakeCh:
			timer.Stop()
			backoffIdx = 0
		case <-timer.C:
		}
	}
}

func (b *EZDeviceBridgeImpl) resolveHostByInstallID(ctx context.Context, installID, certFP string) (string, string, bool) {
	discCtx, cancel := context.WithTimeout(ctx, 1800*time.Millisecond)
	defer cancel()
	hosts, err := b.discoverFn(discCtx, 1500*time.Millisecond)
	if err != nil {
		return "", "", false
	}
	for _, h := range hosts {
		if strings.EqualFold(h.InstallationID, installID) &&
			strings.EqualFold(h.CertificateFingerprint, certFP) &&
			len(h.Addresses) > 0 && h.Port > 0 {
			return net.JoinHostPort(h.Addresses[0], strconv.Itoa(h.Port)), normalizeWSPath(h.Path), true
		}
	}
	return "", "", false
}

func (b *EZDeviceBridgeImpl) connectAndServe(ctx context.Context, id *persistedEZIdentity) (connected bool, fatalStop bool, err error) {
	seed, err := b64uDecodeExact(id.PrivateSeedB64U, ed25519.SeedSize)
	if err != nil {
		return false, true, fmt.Errorf("invalid stored Ed25519 seed: %w", err)
	}

	b.mu.Lock()
	if b.mode != biz.EZModeGRPC && !isEZPairingState(b.status.State) {
		b.status.State = "connecting"
		b.status.ActiveBackend = b.computeActiveBackendLocked()
		b.status.UpdatedAtMs = time.Now().UnixMilli()
	}
	b.mu.Unlock()

	epoch := b.connEpoch.Add(1)
	dialCtx, cancelDial := context.WithTimeout(ctx, 12*time.Second)
	sessConn, err := dialPinnedWSS(dialCtx, id.HostAddress, id.HostPath, id.CertificateFingerprint, epoch)
	cancelDial()
	if err != nil {
		return false, false, err
	}
	defer func() {
		sessConn.close()
		b.mu.Lock()
		if b.conn == sessConn {
			b.conn = nil
			b.failAllTurnsLocked(errors.New("ez-assistant connection closed"))
			if b.status.State == "online" {
				b.status.State = "connecting"
				b.status.ActiveBackend = b.computeActiveBackendLocked()
				b.status.UpdatedAtMs = time.Now().UnixMilli()
			}
		}
		b.mu.Unlock()
	}()

	challengeEnv, err := readNextControlFrame(ctx, sessConn, ezStepReadTimeout)
	if err != nil {
		return false, false, fmt.Errorf("read auth_challenge: %w", err)
	}
	if challengeEnv.Type != "auth_challenge" {
		return false, false, fmt.Errorf("expected auth_challenge, got %q", challengeEnv.Type)
	}
	var challenge ezAuthChallengePayload
	if err := json.Unmarshal(challengeEnv.Payload, &challenge); err != nil {
		return false, false, fmt.Errorf("decode auth_challenge: %w", err)
	}
	if challenge.ConnectionID == "" || challenge.Nonce == "" {
		return false, false, errors.New("empty auth_challenge connection_id or nonce")
	}

	deviceNonce, err := randomNonceB64U(32)
	if err != nil {
		return false, false, err
	}
	caps := defaultGatewayEZCapabilities()
	authTranscript := buildAuthTranscript(
		ezProtocolMajor,
		ezProtocolMinor,
		challenge.ConnectionID,
		challenge.Nonce,
		id.DeviceID,
		deviceNonce,
		caps,
		"text",
	)
	sig, err := signEd25519Transcript(seed, authTranscript)
	if err != nil {
		return false, true, err
	}

	var (
		mcpServers []ezDeviceMCPServerDecl
		console    *ezDeviceConsoleDecl
	)
	if b.mcpPort > 0 {
		mcpServers = []ezDeviceMCPServerDecl{{
			ID:          "esp-pika-gateway",
			DisplayName: "ESP-Pika 设备网关",
			Description: "通过 ESP-Pika Voice & Device Gateway 安全控制已配对局域网硬件终端",
			Scheme:      "http",
			Port:        b.mcpPort,
			Path:        "/mcp",
			Headers: map[string]string{
				"X-Agent-ID":     "ez-assistant",
				"X-Speaker-Role": "owner",
			},
		}}
		console = &ezDeviceConsoleDecl{
			Scheme:    "http",
			Port:      b.mcpPort,
			APIPrefix: "/api/v1",
			UIPath:    "/ui/",
		}
	}

	if _, err := sessConn.sendJSON(ctx, "hello", ezDeviceHelloPayload{
		DeviceID:         id.DeviceID,
		DeviceNonce:      deviceNonce,
		Capabilities:     caps,
		OutputPreference: "text",
		ClientVersion:    ezClientVersion,
		Signature:        b64uEncode(sig),
		MCPServers:       mcpServers,
		Console:          console,
		SpeechRelay:      true,
	}); err != nil {
		return false, false, fmt.Errorf("send hello: %w", err)
	}

	ackEnv, err := readNextControlFrame(ctx, sessConn, ezStepReadTimeout)
	if err != nil {
		return false, false, fmt.Errorf("read hello_ack: %w", err)
	}
	if ackEnv.Type == "error" {
		var ep ezErrorPayload
		_ = json.Unmarshal(ackEnv.Payload, &ep)
		return false, b.handleTerminalAuthError(ctx, id, ep), fmt.Errorf("hello rejected: %s", ep.Code)
	}
	if ackEnv.Type != "hello_ack" {
		return false, false, fmt.Errorf("expected hello_ack, got %q", ackEnv.Type)
	}
	var ack ezHelloAckPayload
	if err := json.Unmarshal(ackEnv.Payload, &ack); err != nil {
		return false, false, fmt.Errorf("decode hello_ack: %w", err)
	}
	if ack.DeviceID != id.DeviceID || ack.ConnectionID != challenge.ConnectionID {
		return false, false, errors.New("hello_ack device_id or connection_id mismatch")
	}

	if id.State == "pending" {
		id.State = "active"
		id.UpdatedAtMs = time.Now().UnixMilli()
		_ = b.saveIdentity(ctx, id)
	}

	b.mu.Lock()
	b.identity = id
	b.conn = sessConn
	if ack.SpeechServices != nil {
		cp := *ack.SpeechServices
		b.speechStatus = &cp
		b.status.SpeechServices = &cp
	} else {
		b.speechStatus = nil
		b.status.SpeechServices = nil
	}
	b.status.State = "online"
	b.status.IdentityState = id.State
	b.status.LastError = ""
	b.status.ActiveBackend = b.computeActiveBackendLocked()
	b.status.UpdatedAtMs = time.Now().UnixMilli()
	b.mu.Unlock()

	defer func() {
		b.mu.Lock()
		if b.conn == sessConn {
			b.conn = nil
			b.speechStatus = nil
			b.status.SpeechServices = nil
			b.failAllTurnsLocked(errors.New("ez-assistant session disconnected"))
		}
		b.mu.Unlock()
	}()

	b.log.Infof("EZ Assistant WSS online! device_id=%s addr=%s", id.DeviceID, id.HostAddress)

	for {
		env, bin, rErr := readNextSessionFrame(ctx, sessConn, ezIdleReadTimeout)
		if rErr != nil {
			return true, false, rErr
		}
		if bin != nil {
			b.handleBinaryRelayFrame(bin)
			continue
		}
		if env != nil {
			if stopFatal, handleErr := b.dispatchSessionFrame(ctx, id, *env); handleErr != nil {
				return true, stopFatal, handleErr
			}
		}
	}
}

func (b *EZDeviceBridgeImpl) handleTerminalAuthError(ctx context.Context, id *persistedEZIdentity, ep ezErrorPayload) bool {
	if ep.Code == "device_revoked" && !ep.Recoverable {
		_, _ = b.ResetPairing(ctx)
		b.mu.Lock()
		b.status.State = "revoked"
		b.status.LastError = "device_revoked"
		b.status.UpdatedAtMs = time.Now().UnixMilli()
		b.mu.Unlock()
		return true
	}
	if ep.Code == "authentication_failed" {
		if id.State == "pending" {
			_, _ = b.ResetPairing(ctx)
			return true
		}
		b.mu.Lock()
		b.status.State = "auth_error"
		b.status.LastError = "authentication_failed"
		b.status.UpdatedAtMs = time.Now().UnixMilli()
		b.mu.Unlock()
		return true
	}
	return false
}

func (b *EZDeviceBridgeImpl) dispatchSessionFrame(ctx context.Context, id *persistedEZIdentity, env ezWireEnvelope) (bool, error) {
	switch env.Type {
	case "input_accepted":
		var p ezInputAcceptedPayload
		if err := json.Unmarshal(env.Payload, &p); err != nil {
			return false, nil
		}
		b.log.Infof("EZ Assistant input_accepted client_input_id=%s input_id=%s run_id=%s queue_state=%s", p.ClientInputID, p.InputID, p.RunID, p.QueueState)
		b.mu.Lock()
		if w, ok := b.turns[p.ClientInputID]; ok {
			w.accepted = true
			if p.RunID != "" {
				w.runID = p.RunID
			}
			cb := w.cb
			b.mu.Unlock()
			if cb.OnStatus != nil {
				statusMsg := "EZ Assistant 思考中"
				if p.QueueState == "queued" {
					statusMsg = "已在 EZ Assistant 队列中"
				}
				cb.OnStatus("thinking", statusMsg, false)
			}
			return false, nil
		}
		b.mu.Unlock()

	case "text_output":
		var p ezTextOutputPayload
		if err := json.Unmarshal(env.Payload, &p); err != nil {
			return false, nil
		}
		b.log.Infof("EZ Assistant received text_output run_id=%s chars=%d", p.RunID, len(p.Text))
		waiter := b.popWaiterByRunID(p.RunID)
		if waiter != nil {
			waiter.resolveText(p.Text)
		}

	case "state_changed":
		var p ezStateChangedPayload
		if err := json.Unmarshal(env.Payload, &p); err != nil {
			return false, nil
		}
		b.log.Infof("EZ Assistant state_changed state=%s reason=%s run_id=%s client_input_id=%s", p.State, p.Reason, p.RunID, p.ClientInputID)
		if p.State == "unavailable" {
			waiter := b.popWaiterByIDs(p.RunID, p.ClientInputID)
			if waiter != nil {
				reason := p.Reason
				if reason == "" {
					reason = "unavailable"
				}
				if reason == "assistant_text_unavailable" {
					waiter.resolve(fmt.Errorf("ez-assistant output unavailable: assistant_text_unavailable (请确认 EZ Assistant「主控会话」已配置模型，或打开主控会话查看具体错误)"))
				} else {
					waiter.resolve(fmt.Errorf("ez-assistant output unavailable: %s", reason))
				}
			}
		}

	case "speech_status_changed":
		var p ezSpeechStatusChangedPayload
		if err := json.Unmarshal(env.Payload, &p); err != nil {
			return false, nil
		}
		b.mu.Lock()
		cp := p.SpeechServices
		b.speechStatus = &cp
		b.status.SpeechServices = &cp
		b.status.UpdatedAtMs = time.Now().UnixMilli()
		b.mu.Unlock()
		b.log.Infof("EZ Assistant speech_status_changed asr=%s tts=%s", cp.ASR, cp.TTS)

	case "speech_asr_result":
		var p ezSpeechAsrResultPayload
		if err := json.Unmarshal(env.Payload, &p); err != nil {
			return false, nil
		}
		b.mu.Lock()
		w := b.asrWaiters[p.RequestID]
		if w != nil {
			delete(b.asrWaiters, p.RequestID)
		}
		b.mu.Unlock()
		if w != nil {
			if p.OK {
				w.resolve(p.Text, nil)
			} else {
				errCode := p.Error
				if errCode == "" {
					errCode = "asr_failed"
				}
				w.resolve("", fmt.Errorf("ez-assistant speech ASR error: %s", errCode))
			}
		}

	case "speech_tts_start":
		var p ezSpeechTtsStartPayload
		if err := json.Unmarshal(env.Payload, &p); err != nil {
			return false, nil
		}
		b.mu.Lock()
		w := b.ttsByReq[p.RequestID]
		if w != nil {
			if !p.OK {
				delete(b.ttsByReq, p.RequestID)
				delete(b.ttsWaiters, w.streamID)
			} else {
				w.started = true
			}
		}
		b.mu.Unlock()
		if w != nil && !p.OK {
			errCode := p.Error
			if errCode == "" {
				errCode = "tts_failed"
			}
			w.resolve(nil, fmt.Errorf("ez-assistant speech TTS error: %s", errCode))
		}

	case "speech_tts_end":
		var p ezSpeechTtsEndPayload
		if err := json.Unmarshal(env.Payload, &p); err != nil {
			return false, nil
		}
		b.mu.Lock()
		w := b.ttsByReq[p.RequestID]
		var pcm []byte
		if w != nil {
			delete(b.ttsByReq, p.RequestID)
			delete(b.ttsWaiters, w.streamID)
			if p.OK {
				pcm = append([]byte(nil), w.pcm.Bytes()...)
			}
		}
		b.mu.Unlock()
		if w != nil {
			if p.OK {
				w.resolve(pcm, nil)
			} else {
				errCode := p.Error
				if errCode == "" {
					errCode = "tts_failed"
				}
				w.resolve(nil, fmt.Errorf("ez-assistant speech TTS error: %s", errCode))
			}
		}

	case "error":
		var ep ezErrorPayload
		if err := json.Unmarshal(env.Payload, &ep); err != nil {
			return false, nil
		}
		if ep.Code == "device_revoked" || (ep.Code == "authentication_failed" && !ep.Recoverable) {
			fatal := b.handleTerminalAuthError(ctx, id, ep)
			return fatal, fmt.Errorf("ez-assistant terminal error: %s", ep.Code)
		}
		if ep.CorrelationMessageID != "" {
			if w := b.popWaiterByMsgID(ep.CorrelationMessageID); w != nil {
				w.resolve(fmt.Errorf("ez-assistant error: %s", ep.Code))
			}
			b.failRelayWaitersByMsgID(ep.CorrelationMessageID, fmt.Errorf("ez-assistant error: %s", ep.Code))
		}
		b.log.Warnf("EZ Assistant protocol error: code=%s recoverable=%v", ep.Code, ep.Recoverable)
	}
	return false, nil
}

func (b *EZDeviceBridgeImpl) failRelayWaitersByMsgID(msgID string, err error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	for reqID, w := range b.asrWaiters {
		if w.msgID == msgID {
			delete(b.asrWaiters, reqID)
			w.resolve("", err)
		}
	}
	for reqID, w := range b.ttsByReq {
		if w.msgID == msgID {
			delete(b.ttsByReq, reqID)
			delete(b.ttsWaiters, w.streamID)
			w.resolve(nil, err)
		}
	}
}

func (b *EZDeviceBridgeImpl) handleBinaryRelayFrame(bin []byte) {
	if len(bin) <= ezRelayHeaderBytes || bin[0] != 1 || bin[1] != ezRelayKindDownlink {
		return
	}
	flags := binary.BigEndian.Uint16(bin[2:4])
	streamID := binary.BigEndian.Uint32(bin[4:8])
	seq := binary.BigEndian.Uint32(bin[8:12])
	payloadLen := int(binary.BigEndian.Uint16(bin[12:14]))
	reserved := binary.BigEndian.Uint16(bin[14:16])
	if flags != 0 || reserved != 0 || streamID == 0 || payloadLen == 0 || payloadLen%2 != 0 || len(bin) != ezRelayHeaderBytes+payloadLen {
		return
	}

	b.mu.Lock()
	w := b.ttsWaiters[streamID]
	if w == nil {
		b.mu.Unlock()
		return
	}
	if !w.started || seq != w.expectedSeq {
		delete(b.ttsWaiters, streamID)
		delete(b.ttsByReq, w.requestID)
		b.mu.Unlock()
		w.resolve(nil, fmt.Errorf("unexpected relay TTS binary frame seq=%d", seq))
		return
	}
	if w.pcm.Len()+payloadLen > ezRelayMaxTTSPCMBytes {
		delete(b.ttsWaiters, streamID)
		delete(b.ttsByReq, w.requestID)
		b.mu.Unlock()
		w.resolve(nil, errors.New("relay TTS PCM exceeded size limit"))
		return
	}
	_, _ = w.pcm.Write(bin[ezRelayHeaderBytes:])
	w.expectedSeq++
	b.mu.Unlock()
}

func (b *EZDeviceBridgeImpl) popOldestAcceptedUnboundLocked() *ezTurnWaiter {
	// If input_accepted returned queue_state=queued (run_id was null), match oldest accepted waiter without runID.
	var oldestKey string
	var oldest *ezTurnWaiter
	for id, w := range b.turns {
		if w.accepted && w.runID == "" {
			if oldest == nil || w.createdAt.Before(oldest.createdAt) {
				oldest = w
				oldestKey = id
			}
		}
	}
	if oldest != nil {
		delete(b.turns, oldestKey)
		return oldest
	}
	return nil
}

func (b *EZDeviceBridgeImpl) popWaiterByRunID(runID string) *ezTurnWaiter {
	b.mu.Lock()
	defer b.mu.Unlock()

	if runID != "" {
		for id, w := range b.turns {
			if w.runID == runID {
				delete(b.turns, id)
				return w
			}
		}
	}
	return b.popOldestAcceptedUnboundLocked()
}

func (b *EZDeviceBridgeImpl) popWaiterByIDs(runID, clientInputID string) *ezTurnWaiter {
	b.mu.Lock()
	defer b.mu.Unlock()

	if clientInputID != "" {
		if w, ok := b.turns[clientInputID]; ok {
			delete(b.turns, clientInputID)
			return w
		}
	}
	if runID != "" {
		for id, w := range b.turns {
			if w.runID == runID {
				delete(b.turns, id)
				return w
			}
		}
	}
	return b.popOldestAcceptedUnboundLocked()
}

func (b *EZDeviceBridgeImpl) popWaiterByMsgID(msgID string) *ezTurnWaiter {
	b.mu.Lock()
	defer b.mu.Unlock()
	for id, w := range b.turns {
		if w.inputMsgID == msgID {
			delete(b.turns, id)
			return w
		}
	}
	return nil
}

func (b *EZDeviceBridgeImpl) saveIdentity(ctx context.Context, id *persistedEZIdentity) error {
	if b.settings == nil || id == nil {
		return nil
	}
	raw, err := json.Marshal(id)
	if err != nil {
		return err
	}
	saveCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_ = ctx
	return b.settings.Set(saveCtx, biz.SettingKeyEZDeviceIdentity, string(raw))
}

func dialPinnedWSS(ctx context.Context, addr, path, expectedFingerprintHex string, epoch uint64) (*ezSessionConn, error) {
	expectedFP := normalizeFingerprintHex(expectedFingerprintHex)
	if !isValidLowerHex(expectedFP, 64) {
		return nil, errors.New("invalid pinned certificate fingerprint")
	}

	transport := &http.Transport{
		Proxy:       nil,
		DialContext: dialEZHostTCP,
		TLSClientConfig: &tls.Config{
			MinVersion:         tls.VersionTLS12,
			InsecureSkipVerify: true, // Verified strictly by VerifyConnection leaf-cert SHA-256 pinning below.
			VerifyConnection: func(cs tls.ConnectionState) error {
				if len(cs.PeerCertificates) == 0 {
					return errors.New("server presented no TLS certificate")
				}
				sum := sha256.Sum256(cs.PeerCertificates[0].Raw)
				gotFP := hex.EncodeToString(sum[:])
				if !constantTimeEqual([]byte(gotFP), []byte(expectedFP)) {
					return fmt.Errorf("TLS leaf certificate fingerprint mismatch (got=%s want=%s)", gotFP, expectedFP)
				}
				return nil
			},
		},
	}
	httpClient := &http.Client{Transport: transport}

	wssURL := (&url.URL{
		Scheme: "wss",
		Host:   addr,
		Path:   normalizeWSPath(path),
	}).String()

	conn, _, err := websocket.Dial(ctx, wssURL, &websocket.DialOptions{
		HTTPClient: httpClient,
	})
	if err != nil {
		transport.CloseIdleConnections()
		return nil, err
	}
	conn.SetReadLimit(ezMaxControlMessageSize)
	return &ezSessionConn{
		epoch: epoch,
		conn:  conn,
		closeFn: func() {
			_ = conn.Close(websocket.StatusNormalClosure, "closing")
			transport.CloseIdleConnections()
		},
	}, nil
}

// readNextControlFrame reads non-ping control envelopes before timeout while immediately answering `ping` with `pong`.
func readNextControlFrame(ctx context.Context, sc *ezSessionConn, timeout time.Duration) (ezWireEnvelope, error) {
	for {
		env, _, err := readNextSessionFrame(ctx, sc, timeout)
		if err != nil {
			return ezWireEnvelope{}, err
		}
		if env != nil {
			return *env, nil
		}
	}
}

func readNextSessionFrame(ctx context.Context, sc *ezSessionConn, timeout time.Duration) (*ezWireEnvelope, []byte, error) {
	deadline := time.Now().Add(timeout)
	for {
		rem := time.Until(deadline)
		if rem <= 0 {
			return nil, nil, context.DeadlineExceeded
		}
		readCtx, cancel := context.WithDeadline(ctx, deadline)
		msgType, raw, err := sc.conn.Read(readCtx)
		cancel()
		if err != nil {
			return nil, nil, err
		}
		if msgType == websocket.MessageBinary {
			return nil, raw, nil
		}
		if msgType != websocket.MessageText {
			continue
		}
		var env ezWireEnvelope
		if err := json.Unmarshal(raw, &env); err != nil {
			return nil, nil, fmt.Errorf("invalid control JSON: %w", err)
		}
		if env.ProtocolMajor != int(ezProtocolMajor) || env.ProtocolMinor != int(ezProtocolMinor) || env.MessageID == "" || env.Type == "" {
			return nil, nil, fmt.Errorf("invalid envelope header major=%d minor=%d type=%s", env.ProtocolMajor, env.ProtocolMinor, env.Type)
		}
		if env.Type == "ping" {
			if _, pErr := sc.sendRawPayload(ctx, "pong", env.Payload); pErr != nil {
				return nil, nil, fmt.Errorf("send pong: %w", pErr)
			}
			// Keepalive traffic extends an idle session read window cleanly.
			deadline = time.Now().Add(timeout)
			continue
		}
		return &env, nil, nil
	}
}

type ezLANSubnet struct {
	iface   net.Interface
	localIP net.IP
	net     *net.IPNet
}

type ezDialAttempt struct {
	localIP net.IP
	dstHost string
}

func dialEZHostTCP(ctx context.Context, network, address string) (net.Conn, error) {
	host, portStr, err := net.SplitHostPort(address)
	if err != nil {
		return (&net.Dialer{Timeout: 6 * time.Second}).DialContext(ctx, network, address)
	}

	targets := []string{host}
	cleanHost := strings.TrimRight(strings.TrimSpace(host), ".")
	if net.ParseIP(cleanHost) == nil && strings.HasSuffix(strings.ToLower(cleanHost), ".local") {
		if resolved := resolveMDNSHostIPv4(ctx, cleanHost, 1500*time.Millisecond); len(resolved) > 0 {
			targets = resolved
		}
	}

	subnets := selectEZBrowseLANSubnets()
	var attempts []ezDialAttempt
	for _, dst := range targets {
		if ip4 := net.ParseIP(dst).To4(); ip4 != nil && len(subnets) > 0 {
			matchedAny := false
			for _, sub := range subnets {
				if sub.net != nil && sub.net.Contains(ip4) {
					attempts = append(attempts, ezDialAttempt{localIP: sub.localIP, dstHost: dst})
					matchedAny = true
				}
			}
			if !matchedAny {
				attempts = append(attempts, ezDialAttempt{dstHost: dst})
			}
			continue
		}
		attempts = append(attempts, ezDialAttempt{dstHost: dst})
	}

	return dialStaggeredAttempts(ctx, network, portStr, attempts, 200*time.Millisecond, 6*time.Second, nil)
}

func dialStaggeredAttempts(
	ctx context.Context,
	network string,
	portStr string,
	attempts []ezDialAttempt,
	stagger time.Duration,
	perAttemptTimeout time.Duration,
	dialFn func(ctx context.Context, localIP net.IP, dstAddr string) (net.Conn, error),
) (net.Conn, error) {
	if len(attempts) == 0 {
		return nil, errors.New("no dial attempts")
	}
	if dialFn == nil {
		dialFn = func(dCtx context.Context, localIP net.IP, dstAddr string) (net.Conn, error) {
			d := &net.Dialer{Timeout: perAttemptTimeout}
			netName := network
			if localIP != nil {
				d.LocalAddr = &net.TCPAddr{IP: localIP}
				netName = "tcp4"
			}
			return d.DialContext(dCtx, netName, dstAddr)
		}
	}
	if len(attempts) == 1 {
		a := attempts[0]
		dCtx, cancel := context.WithTimeout(ctx, perAttemptTimeout)
		defer cancel()
		return dialFn(dCtx, a.localIP, net.JoinHostPort(a.dstHost, portStr))
	}

	attemptCtx, cancelAll := context.WithCancel(ctx)
	defer cancelAll()

	type dialRes struct {
		conn net.Conn
		err  error
	}
	resCh := make(chan dialRes, len(attempts))
	var claimed atomic.Bool

	for i, att := range attempts {
		idx := i
		a := att
		go func() {
			if idx > 0 && stagger > 0 {
				timer := time.NewTimer(time.Duration(idx) * stagger)
				select {
				case <-attemptCtx.Done():
					timer.Stop()
					resCh <- dialRes{err: attemptCtx.Err()}
					return
				case <-timer.C:
				}
			}
			dCtx, cancel := context.WithTimeout(attemptCtx, perAttemptTimeout)
			c, err := dialFn(dCtx, a.localIP, net.JoinHostPort(a.dstHost, portStr))
			cancel()
			if err != nil {
				resCh <- dialRes{err: err}
				return
			}
			if !claimed.CompareAndSwap(false, true) {
				_ = c.Close()
				resCh <- dialRes{err: context.Canceled}
				return
			}
			cancelAll()
			resCh <- dialRes{conn: c}
		}()
	}

	var firstErr error
	for range attempts {
		r := <-resCh
		if r.conn != nil {
			return r.conn, nil
		}
		if firstErr == nil && r.err != nil {
			firstErr = r.err
		}
	}
	if firstErr == nil {
		firstErr = errors.New("all dial attempts failed")
	}
	return nil, firstErr
}

type ezMDNSParsedCollector struct {
	subnets  []ezLANSubnet
	ptrs     map[string]struct{}
	srvs     map[string]ezMDNSSRVRecord
	txts     map[string]map[string]string
	aRecs    map[string][]net.IP
	srcBySrv map[string][]net.IP
	srcByA   map[string][]net.IP
}

type ezMDNSSRVRecord struct {
	target string
	port   int
}

func newEZMDNSCollector(subnets []ezLANSubnet) *ezMDNSParsedCollector {
	return &ezMDNSParsedCollector{
		subnets:  subnets,
		ptrs:     make(map[string]struct{}),
		srvs:     make(map[string]ezMDNSSRVRecord),
		txts:     make(map[string]map[string]string),
		aRecs:    make(map[string][]net.IP),
		srcBySrv: make(map[string][]net.IP),
		srcByA:   make(map[string][]net.IP),
	}
}

func (c *ezMDNSParsedCollector) ingest(msg *dns.Msg, srcIP net.IP) []string {
	if msg == nil {
		return nil
	}
	src4 := srcIP.To4()
	allRRs := make([]dns.RR, 0, len(msg.Answer)+len(msg.Extra))
	allRRs = append(allRRs, msg.Answer...)
	allRRs = append(allRRs, msg.Extra...)

	var missingHosts []string
	for _, rr := range allRRs {
		switch v := rr.(type) {
		case *dns.PTR:
			name := normalizeDNSName(v.Hdr.Name)
			if name == normalizeDNSName(ezMDNSServiceType+"."+ezMDNSDomain) {
				ptr := normalizeDNSName(v.Ptr)
				if ptr != "" {
					c.ptrs[ptr] = struct{}{}
				}
			}
		case *dns.SRV:
			name := normalizeDNSName(v.Hdr.Name)
			tgt := normalizeDNSName(v.Target)
			if name != "" && tgt != "" && v.Port > 0 {
				c.srvs[name] = ezMDNSSRVRecord{target: tgt, port: int(v.Port)}
				if isUsableLANIPv4(src4) {
					c.srcBySrv[name] = append(c.srcBySrv[name], src4)
				}
				if len(c.aRecs[tgt]) == 0 {
					missingHosts = append(missingHosts, tgt)
				}
			}
		case *dns.TXT:
			name := normalizeDNSName(v.Hdr.Name)
			if name == "" {
				continue
			}
			m := c.txts[name]
			if m == nil {
				m = make(map[string]string, len(v.Txt))
				c.txts[name] = m
			}
			for _, item := range v.Txt {
				k, val, ok := strings.Cut(item, "=")
				if ok {
					m[strings.TrimSpace(k)] = strings.TrimSpace(val)
				}
			}
		case *dns.A:
			name := normalizeDNSName(v.Hdr.Name)
			ip4 := v.A.To4()
			if name != "" && isUsableLANIPv4(ip4) {
				c.aRecs[name] = append(c.aRecs[name], ip4)
				if isUsableLANIPv4(src4) {
					c.srcByA[name] = append(c.srcByA[name], src4)
				}
			}
		}
	}
	return missingHosts
}

func (c *ezMDNSParsedCollector) candidates() []biz.EZHostCandidate {
	names := make(map[string]struct{})
	for n := range c.ptrs {
		names[n] = struct{}{}
	}
	for n := range c.srvs {
		if strings.HasSuffix(n, "."+normalizeDNSName(ezMDNSServiceType+"."+ezMDNSDomain)) {
			names[n] = struct{}{}
		}
	}

	seen := make(map[string]biz.EZHostCandidate)
	suffix := "." + normalizeDNSName(ezMDNSServiceType+"."+ezMDNSDomain)
	for fullName := range names {
		srv, ok := c.srvs[fullName]
		if !ok || srv.port <= 0 {
			continue
		}
		txt := c.txts[fullName]
		if len(txt) == 0 {
			continue
		}
		major, _ := strconv.Atoi(txt["protocol_major"])
		minor, _ := strconv.Atoi(txt["protocol_minor"])
		if major != 1 {
			continue
		}
		installID := strings.ToLower(txt["installation_id"])
		certFP := normalizeFingerprintHex(txt["certificate_fingerprint"])
		if !isValidLowerHex(installID, 32) || !isValidLowerHex(certFP, 64) {
			continue
		}

		srcIPs := append([]net.IP{}, c.srcBySrv[fullName]...)
		srcIPs = append(srcIPs, c.srcByA[srv.target]...)
		addrs := rankEZCandidateIPv4s(srcIPs, c.aRecs[srv.target], c.subnets)
		if len(addrs) == 0 {
			addrs = []string{strings.TrimRight(srv.target, ".")}
		}

		instLabel := strings.TrimSuffix(fullName, suffix)
		instLabel = unescapeDNSLabel(instLabel)
		cand := biz.EZHostCandidate{
			InstanceName:           instLabel,
			HostName:               strings.TrimRight(srv.target, "."),
			Addresses:              addrs,
			Port:                   srv.port,
			Path:                   normalizeWSPath(txt["path"]),
			ProtocolMajor:          major,
			ProtocolMinor:          minor,
			InstallationID:         installID,
			CertificateFingerprint: certFP,
			PairingAvailable:       txt["pairing_available"] == "true",
			PairingModes:           parsePairingModesTXT(txt["pairing_modes"]),
		}
		seen[installID+":"+strconv.Itoa(srv.port)] = cand
	}

	out := make([]biz.EZHostCandidate, 0, len(seen))
	for _, cand := range seen {
		out = append(out, cand)
	}
	return out
}

func mdnsBrowseEZHosts(ctx context.Context, timeout time.Duration) ([]biz.EZHostCandidate, error) {
	if timeout <= 0 {
		timeout = 2 * time.Second
	}
	subnets := selectEZBrowseLANSubnets()
	collector := queryMDNSRecords(ctx, timeout, subnets, []dns.Question{
		{
			Name:   ezMDNSServiceType + "." + ezMDNSDomain,
			Qtype:  dns.TypePTR,
			Qclass: dns.ClassINET | (1 << 15),
		},
	}, false, "")

	cands := collector.candidates()
	if len(cands) > 0 {
		return cands, nil
	}

	// Secondary fallback via zeroconf.
	ifaces := selectEZBrowseIfaces()
	var opts []zeroconf.ClientOption
	if len(ifaces) > 0 {
		opts = append(opts, zeroconf.SelectIfaces(ifaces))
	}
	resolver, err := zeroconf.NewResolver(opts...)
	if err != nil {
		return nil, fmt.Errorf("create mDNS resolver: %w", err)
	}

	browseCtx, cancel := context.WithTimeout(ctx, minDuration(timeout, 1500*time.Millisecond))
	defer cancel()

	entries := make(chan *zeroconf.ServiceEntry, 32)
	var mu sync.Mutex
	seen := make(map[string]biz.EZHostCandidate)
	done := make(chan struct{})

	go func() {
		defer close(done)
		for entry := range entries {
			if cand, ok := parseEZMDNSEntry(entry, subnets); ok {
				key := cand.InstallationID + ":" + strconv.Itoa(cand.Port)
				mu.Lock()
				seen[key] = cand
				mu.Unlock()
			}
		}
	}()

	if err := resolver.Browse(browseCtx, ezMDNSServiceType, ezMDNSDomain, entries); err != nil {
		return nil, fmt.Errorf("browse %s: %w", ezMDNSServiceType, err)
	}
	<-browseCtx.Done()
	<-done

	mu.Lock()
	out := make([]biz.EZHostCandidate, 0, len(seen))
	for _, c := range seen {
		out = append(out, c)
	}
	mu.Unlock()
	return out, nil
}

func resolveMDNSHostIPv4(ctx context.Context, host string, timeout time.Duration) []string {
	clean := normalizeDNSName(host)
	if clean == "" {
		return nil
	}
	subnets := selectEZBrowseLANSubnets()
	collector := queryMDNSRecords(ctx, timeout, subnets, []dns.Question{
		{Name: clean, Qtype: dns.TypeA, Qclass: dns.ClassINET | (1 << 15)},
		{Name: ezMDNSServiceType + "." + ezMDNSDomain, Qtype: dns.TypePTR, Qclass: dns.ClassINET | (1 << 15)},
	}, true, clean)

	srcIPs := append([]net.IP{}, collector.srcByA[clean]...)
	aIPs := append([]net.IP{}, collector.aRecs[clean]...)
	for srvName, srv := range collector.srvs {
		if srv.target == clean {
			srcIPs = append(srcIPs, collector.srcBySrv[srvName]...)
		}
	}
	return rankEZCandidateIPv4s(srcIPs, aIPs, subnets)
}

func queryMDNSRecords(
	ctx context.Context,
	timeout time.Duration,
	subnets []ezLANSubnet,
	questions []dns.Question,
	stopOnTargetA bool,
	targetHostFQDN string,
) *ezMDNSParsedCollector {
	collector := newEZMDNSCollector(subnets)
	qCtx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()

	mcastAddr := &net.UDPAddr{IP: net.IPv4(224, 0, 0, 251), Port: 5353}
	type udpPkt struct {
		raw   []byte
		srcIP net.IP
	}
	pktCh := make(chan udpPkt, 64)
	var sockMu sync.Mutex
	var sockets []*net.UDPConn
	var wg sync.WaitGroup

	listenConn := func(c *net.UDPConn) {
		sockMu.Lock()
		sockets = append(sockets, c)
		sockMu.Unlock()
		wg.Add(1)
		go func() {
			defer wg.Done()
			buf := make([]byte, 8192)
			for {
				_ = c.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
				n, raddr, err := c.ReadFromUDP(buf)
				if err != nil {
					if qCtx.Err() != nil {
						return
					}
					if ne, ok := err.(net.Error); ok && ne.Timeout() {
						continue
					}
					return
				}
				if n > 0 && raddr != nil {
					cp := make([]byte, n)
					copy(cp, buf[:n])
					select {
					case pktCh <- udpPkt{raw: cp, srcIP: raddr.IP}:
					case <-qCtx.Done():
						return
					}
				}
			}
		}()
	}

	seenIface := make(map[int]struct{})
	var querySockets []*net.UDPConn
	for _, sub := range subnets {
		if _, exists := seenIface[sub.iface.Index]; !exists {
			seenIface[sub.iface.Index] = struct{}{}
			ifaceCopy := sub.iface
			if mc, err := net.ListenMulticastUDP("udp4", &ifaceCopy, mcastAddr); err == nil {
				listenConn(mc)
			}
		}
		if uc, err := net.ListenUDP("udp4", &net.UDPAddr{IP: sub.localIP, Port: 0}); err == nil {
			querySockets = append(querySockets, uc)
			listenConn(uc)
		}
	}
	if len(querySockets) == 0 {
		if uc, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4zero, Port: 0}); err == nil {
			querySockets = append(querySockets, uc)
			listenConn(uc)
		}
	}

	sendQuestions := func(qs []dns.Question, unicastBit bool) {
		if len(qs) == 0 || len(querySockets) == 0 {
			return
		}
		m := new(dns.Msg)
		m.Id = 0
		m.RecursionDesired = false
		m.Question = make([]dns.Question, len(qs))
		for i, q := range qs {
			if unicastBit {
				q.Qclass = dns.ClassINET | (1 << 15)
			} else {
				q.Qclass = dns.ClassINET
			}
			m.Question[i] = q
		}
		packed, err := m.Pack()
		if err != nil {
			return
		}
		for _, qsck := range querySockets {
			_, _ = qsck.WriteToUDP(packed, mcastAddr)
		}
	}

	sendQuestions(questions, true)
	sendQuestions(questions, false)
	reQueryTimer := time.NewTimer(300 * time.Millisecond)
	defer reQueryTimer.Stop()

	queriedA := make(map[string]struct{})
loop:
	for {
		select {
		case <-qCtx.Done():
			break loop
		case <-reQueryTimer.C:
			sendQuestions(questions, true)
		case pkt := <-pktCh:
			var msg dns.Msg
			if err := msg.Unpack(pkt.raw); err != nil {
				continue
			}
			missingHosts := collector.ingest(&msg, pkt.srcIP)
			var extraQs []dns.Question
			for _, h := range missingHosts {
				if _, already := queriedA[h]; !already {
					queriedA[h] = struct{}{}
					extraQs = append(extraQs, dns.Question{Name: h, Qtype: dns.TypeA})
				}
			}
			if len(extraQs) > 0 {
				sendQuestions(extraQs, true)
			}
			if stopOnTargetA && targetHostFQDN != "" && len(collector.aRecs[targetHostFQDN]) > 0 {
				cancel()
				break loop
			}
		}
	}

	cancel()
	sockMu.Lock()
	for _, s := range sockets {
		_ = s.Close()
	}
	sockMu.Unlock()
	wg.Wait()
	return collector
}

func parseEZMDNSEntry(entry *zeroconf.ServiceEntry, subnets []ezLANSubnet) (biz.EZHostCandidate, bool) {
	if entry == nil || entry.Port <= 0 {
		return biz.EZHostCandidate{}, false
	}
	txt := make(map[string]string, len(entry.Text))
	for _, item := range entry.Text {
		k, v, ok := strings.Cut(item, "=")
		if ok {
			txt[strings.TrimSpace(k)] = strings.TrimSpace(v)
		}
	}

	major, _ := strconv.Atoi(txt["protocol_major"])
	minor, _ := strconv.Atoi(txt["protocol_minor"])
	if major != 1 {
		return biz.EZHostCandidate{}, false
	}
	installID := strings.ToLower(txt["installation_id"])
	certFP := normalizeFingerprintHex(txt["certificate_fingerprint"])
	if !isValidLowerHex(installID, 32) || !isValidLowerHex(certFP, 64) {
		return biz.EZHostCandidate{}, false
	}

	addrs := rankEZCandidateIPv4s(nil, entry.AddrIPv4, subnets)
	if len(addrs) == 0 {
		for _, ip := range entry.AddrIPv6 {
			if ip != nil && !ip.IsLoopback() && !ip.IsLinkLocalUnicast() && !ip.IsMulticast() && !ip.IsUnspecified() {
				addrs = append(addrs, ip.String())
			}
		}
	}
	if len(addrs) == 0 && entry.HostName != "" {
		addrs = append(addrs, strings.TrimRight(entry.HostName, "."))
	}
	if len(addrs) == 0 {
		return biz.EZHostCandidate{}, false
	}

	return biz.EZHostCandidate{
		InstanceName:           unescapeDNSLabel(entry.Instance),
		HostName:               strings.TrimRight(entry.HostName, "."),
		Addresses:              addrs,
		Port:                   entry.Port,
		Path:                   normalizeWSPath(txt["path"]),
		ProtocolMajor:          major,
		ProtocolMinor:          minor,
		InstallationID:         installID,
		CertificateFingerprint: certFP,
		PairingAvailable:       txt["pairing_available"] == "true",
		PairingModes:           parsePairingModesTXT(txt["pairing_modes"]),
	}, true
}

func selectEZBrowseLANSubnets() []ezLANSubnet {
	ifaces := selectEZBrowseIfaces()
	var out []ezLANSubnet
	for _, iface := range ifaces {
		addrs, err := iface.Addrs()
		if err != nil {
			continue
		}
		for _, a := range addrs {
			ipNet, ok := a.(*net.IPNet)
			if !ok {
				continue
			}
			v4 := ipNet.IP.To4()
			if !isUsableLANIPv4(v4) {
				continue
			}
			subnetCopy := &net.IPNet{
				IP:   v4.Mask(ipNet.Mask),
				Mask: ipNet.Mask,
			}
			out = append(out, ezLANSubnet{
				iface:   iface,
				localIP: v4,
				net:     subnetCopy,
			})
		}
	}
	return out
}

func isUsableLANIPv4(ip net.IP) bool {
	if ip == nil {
		return false
	}
	v4 := ip.To4()
	if v4 == nil {
		return false
	}
	if v4.IsLoopback() || v4.IsLinkLocalUnicast() || v4.IsLinkLocalMulticast() || v4.IsMulticast() || v4.IsUnspecified() || !v4.IsGlobalUnicast() {
		return false
	}
	// Reject RFC6598 CGNAT / Tailscale (100.64.0.0/10) and Clash TUN (198.18.0.0/15).
	if (v4[0] == 100 && v4[1] >= 64 && v4[1] <= 127) || (v4[0] == 198 && (v4[1] == 18 || v4[1] == 19)) {
		return false
	}
	return true
}

func rankEZCandidateIPv4s(srcIPs, aRecordIPs []net.IP, subnets []ezLANSubnet) []string {
	type ipEntry struct {
		str  string
		tier int
		seq  int
	}
	entries := make(map[string]ipEntry)
	srcSet := make(map[string]struct{})
	for _, sip := range srcIPs {
		if v4 := sip.To4(); isUsableLANIPv4(v4) {
			srcSet[v4.String()] = struct{}{}
		}
	}
	inLocalSubnet := func(ip net.IP) bool {
		for _, s := range subnets {
			if s.net != nil && s.net.Contains(ip) {
				return true
			}
		}
		return false
	}

	seq := 0
	consider := func(ip net.IP) {
		v4 := ip.To4()
		if !isUsableLANIPv4(v4) {
			return
		}
		s := v4.String()
		_, isSrc := srcSet[s]
		inSub := inLocalSubnet(v4)
		tier := 3
		switch {
		case isSrc && inSub:
			tier = 0
		case inSub:
			tier = 1
		case isSrc:
			tier = 2
		}
		if cur, ok := entries[s]; !ok || tier < cur.tier {
			if ok {
				entries[s] = ipEntry{str: s, tier: tier, seq: cur.seq}
			} else {
				entries[s] = ipEntry{str: s, tier: tier, seq: seq}
				seq++
			}
		}
	}

	for _, ip := range srcIPs {
		consider(ip)
	}
	for _, ip := range aRecordIPs {
		consider(ip)
	}

	list := make([]ipEntry, 0, len(entries))
	for _, e := range entries {
		list = append(list, e)
	}
	sort.Slice(list, func(i, j int) bool {
		if list[i].tier != list[j].tier {
			return list[i].tier < list[j].tier
		}
		return list[i].seq < list[j].seq
	})

	out := make([]string, len(list))
	for i, e := range list {
		out[i] = e.str
	}
	return out
}

func normalizeDNSName(name string) string {
	n := strings.TrimSpace(strings.ToLower(name))
	if n == "" {
		return ""
	}
	return strings.TrimRight(n, ".") + "."
}

func unescapeDNSLabel(s string) string {
	var b strings.Builder
	b.Grow(len(s))
	for i := 0; i < len(s); i++ {
		if s[i] == '\\' && i+1 < len(s) {
			if i+3 < len(s) && s[i+1] >= '0' && s[i+1] <= '9' && s[i+2] >= '0' && s[i+2] <= '9' && s[i+3] >= '0' && s[i+3] <= '9' {
				if val, err := strconv.Atoi(s[i+1 : i+4]); err == nil && val <= 255 {
					b.WriteByte(byte(val))
					i += 3
					continue
				}
			}
			i++
			b.WriteByte(s[i])
			continue
		}
		b.WriteByte(s[i])
	}
	return b.String()
}

func minDuration(a, b time.Duration) time.Duration {
	if a < b {
		return a
	}
	return b
}

func selectEZBrowseIfaces() []net.Interface {
	raw, err := net.Interfaces()
	if err != nil {
		return nil
	}
	var out []net.Interface
	for _, iface := range raw {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagMulticast == 0 {
			continue
		}
		if iface.Flags&net.FlagLoopback != 0 || iface.Flags&net.FlagPointToPoint != 0 {
			continue
		}
		lower := strings.ToLower(iface.Name)
		skip := false
		for _, prefix := range ezExcludedBrowseIfacePrefixes {
			if strings.HasPrefix(lower, prefix) {
				skip = true
				break
			}
		}
		if !skip {
			out = append(out, iface)
		}
	}
	return out
}

func normalizeEZMode(mode string) (string, bool) {
	switch strings.ToLower(strings.TrimSpace(mode)) {
	case "", biz.EZModeAuto:
		return biz.EZModeAuto, true
	case biz.EZModeDevice:
		return biz.EZModeDevice, true
	case biz.EZModeGRPC:
		return biz.EZModeGRPC, true
	default:
		return "", false
	}
}

func normalizeFingerprintHex(fp string) string {
	s := strings.ToLower(strings.TrimSpace(fp))
	s = strings.TrimPrefix(s, "sha256:")
	s = strings.ReplaceAll(s, ":", "")
	s = strings.ReplaceAll(s, " ", "")
	return s
}

func normalizeWSPath(path string) string {
	p := strings.TrimSpace(path)
	if p == "" {
		return "/device"
	}
	if !strings.HasPrefix(p, "/") {
		p = "/" + p
	}
	return p
}

func validatePersistedIdentity(id *persistedEZIdentity) bool {
	if id == nil {
		return false
	}
	if id.State != "pending" && id.State != "active" {
		return false
	}
	if !isValidLowerHex(id.InstallationID, 32) || !isValidLowerHex(id.CertificateFingerprint, 64) {
		return false
	}
	if strings.TrimSpace(id.DeviceID) == "" || strings.TrimSpace(id.HostAddress) == "" {
		return false
	}
	if _, err := b64uDecodeExact(id.PrivateSeedB64U, ed25519.SeedSize); err != nil {
		return false
	}
	if _, err := b64uDecodeExact(id.PublicKeyB64U, ed25519.PublicKeySize); err != nil {
		return false
	}
	return true
}

func generate6DigitPairingCode() (string, error) {
	n, err := rand.Int(rand.Reader, big.NewInt(1000000))
	if err != nil {
		return "", fmt.Errorf("generate pairing code: %w", err)
	}
	return fmt.Sprintf("%06d", n.Int64()), nil
}

func randomNonceB64U(nBytes int) (string, error) {
	buf := make([]byte, nBytes)
	if _, err := rand.Read(buf); err != nil {
		return "", fmt.Errorf("generate nonce: %w", err)
	}
	return b64uEncode(buf), nil
}
