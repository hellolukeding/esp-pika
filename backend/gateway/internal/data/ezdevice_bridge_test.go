package data

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"math/big"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"esp-pika/backend/gateway/internal/biz"
	"esp-pika/backend/gateway/internal/conf"

	"github.com/coder/websocket"
	"github.com/go-kratos/kratos/v2/log"
	"github.com/grandcat/zeroconf"
	"github.com/miekg/dns"
)

type mockEZHostServer struct {
	t                 *testing.T
	installID         string
	certFP            string
	pairingCodeCh     chan string
	assignedDevID     string
	pubKey            ed25519.PublicKey
	pingReceived      chan struct{}
	lastTextInput     chan string
	server            *httptest.Server
	mu                sync.Mutex
	directModeAction  string
	lastHello         *ezDeviceHelloPayload
	hostReplayMode    string
	queuedUnavailable bool
	speechServices    *biz.EZSpeechServicesStatus
	activeConn        *websocket.Conn
	activeCtx         context.Context
	relayASRAudio     []byte
}

func newMockEZHostServer(t *testing.T, installID, assignedDevID string) *mockEZHostServer {
	t.Helper()
	m := &mockEZHostServer{
		t:             t,
		installID:     installID,
		assignedDevID: assignedDevID,
		pairingCodeCh: make(chan string, 1),
		pingReceived:  make(chan struct{}, 4),
		lastTextInput: make(chan string, 4),
	}
	m.server = httptest.NewTLSServer(http.HandlerFunc(m.serveWS))
	sum := sha256.Sum256(m.server.TLS.Certificates[0].Certificate[0])
	m.certFP = hex.EncodeToString(sum[:])
	return m
}

func (m *mockEZHostServer) addr() string {
	return strings.TrimPrefix(m.server.URL, "https://")
}

func (m *mockEZHostServer) close() {
	m.server.Close()
}

func (m *mockEZHostServer) sendFrame(ctx context.Context, conn *websocket.Conn, id, msgType string, payload any) error {
	rawPayload, err := json.Marshal(payload)
	if err != nil {
		return err
	}
	rawEnv, err := json.Marshal(ezWireEnvelope{
		ProtocolMajor: 1,
		ProtocolMinor: 0,
		MessageID:     id,
		Type:          msgType,
		Payload:       rawPayload,
	})
	if err != nil {
		return err
	}
	return conn.Write(ctx, websocket.MessageText, rawEnv)
}

func (m *mockEZHostServer) readFrame(ctx context.Context, conn *websocket.Conn) (ezWireEnvelope, error) {
	_, raw, err := conn.Read(ctx)
	if err != nil {
		return ezWireEnvelope{}, err
	}
	var env ezWireEnvelope
	if err := json.Unmarshal(raw, &env); err != nil {
		return ezWireEnvelope{}, err
	}
	return env, nil
}

func (m *mockEZHostServer) serveWS(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/device" {
		http.NotFound(w, r)
		return
	}
	conn, err := websocket.Accept(w, r, nil)
	if err != nil {
		return
	}
	defer conn.Close(websocket.StatusNormalClosure, "done")

	ctx := r.Context()
	connID := "conn-mock-1"
	challengeNonce := "host-challenge-nonce-1"
	if err := m.sendFrame(ctx, conn, "h-1", "auth_challenge", ezAuthChallengePayload{
		ConnectionID: connID,
		Nonce:        challengeNonce,
		ServerTimeMs: time.Now().UnixMilli(),
	}); err != nil {
		return
	}

	first, err := m.readFrame(ctx, conn)
	if err != nil {
		return
	}

	switch first.Type {
	case "pairing_hello":
		m.handlePairing(ctx, conn, first)
	case "hello":
		m.handleAuthenticatedSession(ctx, conn, connID, challengeNonce, first)
	}
}

func (m *mockEZHostServer) handlePairing(ctx context.Context, conn *websocket.Conn, first ezWireEnvelope) {
	var hello ezPairingHelloPayload
	if err := json.Unmarshal(first.Payload, &hello); err != nil {
		return
	}
	if hello.PairingMode == "direct_confirm" {
		m.mu.Lock()
		action := m.directModeAction
		m.mu.Unlock()
		if action == "legacy_reject" {
			_ = m.sendFrame(ctx, conn, "h-err-legacy", "error", ezErrorPayload{Code: "invalid_message", Recoverable: false})
			return
		}
		if err := m.sendFrame(ctx, conn, "h-2", "pairing_pending", ezPairingPendingPayload{
			PairingRequestID: hello.PairingRequestID,
			ExpiresAtMs:      time.Now().Add(time.Minute).UnixMilli(),
		}); err != nil {
			return
		}
		if action == "reject" {
			_ = m.sendFrame(ctx, conn, "h-err-reject", "error", ezErrorPayload{Code: "pairing_rejected", Recoverable: false})
			return
		}
		pubRaw, err := b64uDecodeExact(hello.PublicKey, ed25519.PublicKeySize)
		if err != nil {
			_ = m.sendFrame(ctx, conn, "h-err-pk", "error", ezErrorPayload{Code: "invalid_message"})
			return
		}
		hostNonce := "host-direct-nonce-1"
		if err := m.sendFrame(ctx, conn, "h-3-direct", "pairing_direct_challenge", ezPairingDirectChallengePayload{
			PairingRequestID: hello.PairingRequestID,
			HostNonce:        hostNonce,
			DeviceID:         m.assignedDevID,
		}); err != nil {
			return
		}
		commitEnv, err := m.readFrame(ctx, conn)
		if err != nil || commitEnv.Type != "pairing_direct_commit" {
			return
		}
		var commitMsg ezPairingDirectCommitPayload
		if err := json.Unmarshal(commitEnv.Payload, &commitMsg); err != nil {
			return
		}
		sig, err := b64uDecodeExact(commitMsg.Signature, ed25519.SignatureSize)
		if err != nil {
			return
		}
		ad := buildPairingAssociatedData(hello.PairingRequestID, m.installID, m.certFP, hello.DeviceNonce, hostNonce, hello.Capabilities)
		ct := buildCommitTranscript(buildBindTranscript(ad, pubRaw), m.assignedDevID)
		if !ed25519.Verify(pubRaw, ct, sig) {
			_ = m.sendFrame(ctx, conn, "h-err-sig", "error", ezErrorPayload{Code: "pairing_failed"})
			return
		}
		m.mu.Lock()
		m.pubKey = pubRaw
		m.mu.Unlock()
		_ = m.sendFrame(ctx, conn, "h-5", "pairing_complete", ezPairingCompletePayload{
			PairingRequestID: hello.PairingRequestID,
			DeviceID:         m.assignedDevID,
			DisplayName:      hello.DisplayName,
		})
		return
	}
	if err := m.sendFrame(ctx, conn, "h-2", "pairing_pending", ezPairingPendingPayload{
		PairingRequestID: hello.PairingRequestID,
		ExpiresAtMs:      time.Now().Add(time.Minute).UnixMilli(),
	}); err != nil {
		return
	}

	// Send an application ping while user is entering 6-digit code; expect exact pong!
	pingPayload := map[string]any{"nonce": "hb-pair-1", "sent_at_ms": time.Now().UnixMilli()}
	if err := m.sendFrame(ctx, conn, "h-ping-pair", "ping", pingPayload); err != nil {
		return
	}
	pongEnv, err := m.readFrame(ctx, conn)
	if err != nil || pongEnv.Type != "pong" {
		return
	}

	var code string
	select {
	case code = <-m.pairingCodeCh:
	case <-ctx.Done():
		return
	}

	curve := elliptic.P256()
	w, err := derivePasswordScalar(code, hello.PairingRequestID, m.installID)
	if err != nil {
		return
	}
	nMinusOne := new(big.Int).Sub(curve.Params().N, big.NewInt(1))
	yr, err := rand.Int(rand.Reader, nMinusOne)
	if err != nil {
		return
	}
	y := new(big.Int).Add(yr, big.NewInt(1))
	yScalar := i2osp32(y)
	wScalar := i2osp32(w)

	yGx, yGy := curve.ScalarBaseMult(yScalar)
	wNx, wNy := curve.ScalarMult(spake2PointNx, spake2PointNy, wScalar)
	Yx, Yy := curve.Add(yGx, yGy, wNx, wNy)
	hostShare := elliptic.Marshal(curve, Yx, Yy)

	devShare, err := b64uDecodeExact(hello.PakeShare, 65)
	if err != nil {
		return
	}
	Xx, Xy := elliptic.Unmarshal(curve, devShare)
	wMx, wMy := curve.ScalarMult(spake2PointMx, spake2PointMy, wScalar)
	negWMy := new(big.Int).Mod(new(big.Int).Sub(curve.Params().P, wMy), curve.Params().P)
	zx, zy := curve.Add(Xx, Xy, wMx, negWMy)
	kx, ky := curve.ScalarMult(zx, zy, yScalar)
	k65 := elliptic.Marshal(curve, kx, ky)

	tt := encodeRFC(
		[]byte(hello.PairingRequestID),
		[]byte(m.installID),
		devShare,
		hostShare,
		k65,
		wScalar,
	)
	sched := sha256.Sum256(tt)
	sessKey := sched[:16]
	authKey := sched[16:32]

	hostNonce := "host-pake-nonce-1"
	ad := buildPairingAssociatedData(hello.PairingRequestID, m.installID, m.certFP, hello.DeviceNonce, hostNonce, hello.Capabilities)
	confKeys := hkdfSHA25632(authKey, append([]byte("ConfirmationKeys"), ad...))
	devConfWant := hmacSHA256(confKeys[:16], tt)
	hostConf := hmacSHA256(confKeys[16:32], tt)

	if err := m.sendFrame(ctx, conn, "h-3", "pairing_pake", ezPairingPakePayload{
		PairingRequestID: hello.PairingRequestID,
		HostNonce:        hostNonce,
		PakeShare:        b64uEncode(hostShare),
		ConfirmationMAC:  b64uEncode(hostConf[:]),
	}); err != nil {
		return
	}

	confEnv, err := m.readFrame(ctx, conn)
	if err != nil || confEnv.Type != "pairing_confirm" {
		return
	}
	var devConfMsg ezPairingConfirmPayload
	if err := json.Unmarshal(confEnv.Payload, &devConfMsg); err != nil {
		return
	}
	gotDevConf, err := b64uDecodeExact(devConfMsg.ConfirmationMAC, 32)
	if err != nil || !constantTimeEqual(gotDevConf, devConfWant[:]) {
		return
	}

	bindEnv, err := m.readFrame(ctx, conn)
	if err != nil || bindEnv.Type != "pairing_bind" {
		return
	}
	var bindMsg ezPairingBindPayload
	if err := json.Unmarshal(bindEnv.Payload, &bindMsg); err != nil {
		return
	}
	pubRaw, err := b64uDecodeExact(bindMsg.PublicKey, ed25519.PublicKeySize)
	if err != nil {
		return
	}
	bindSig, err := b64uDecodeExact(bindMsg.Signature, ed25519.SignatureSize)
	if err != nil {
		return
	}
	bindMAC, err := b64uDecodeExact(bindMsg.BindingMAC, 32)
	if err != nil {
		return
	}
	bt := buildBindTranscript(ad, pubRaw)
	if !ed25519.Verify(pubRaw, bt, bindSig) {
		return
	}
	wantBindMAC := computeDeviceBindMAC(sessKey, bt)
	if !constantTimeEqual(bindMAC, wantBindMAC[:]) {
		return
	}

	ct := buildCommitTranscript(bt, m.assignedDevID)
	ackMAC := computeHostBindAckMAC(sessKey, ct)
	if err := m.sendFrame(ctx, conn, "h-4", "pairing_bind_ack", ezPairingBindAckPayload{
		PairingRequestID: hello.PairingRequestID,
		DeviceID:         m.assignedDevID,
		HostProof:        b64uEncode(ackMAC[:]),
	}); err != nil {
		return
	}

	commitEnv, err := m.readFrame(ctx, conn)
	if err != nil || commitEnv.Type != "pairing_commit" {
		return
	}
	var commitMsg ezPairingCommitPayload
	if err := json.Unmarshal(commitEnv.Payload, &commitMsg); err != nil {
		return
	}
	commitSig, err := b64uDecodeExact(commitMsg.Signature, ed25519.SignatureSize)
	if err != nil {
		return
	}
	commitMAC, err := b64uDecodeExact(commitMsg.BindingMAC, 32)
	if err != nil {
		return
	}
	wantCommitMAC := computeDeviceCommitMAC(sessKey, ct)
	if !ed25519.Verify(pubRaw, ct, commitSig) || !constantTimeEqual(commitMAC, wantCommitMAC[:]) {
		return
	}

	m.mu.Lock()
	m.pubKey = pubRaw
	m.mu.Unlock()

	_ = m.sendFrame(ctx, conn, "h-5", "pairing_complete", ezPairingCompletePayload{
		PairingRequestID: hello.PairingRequestID,
		DeviceID:         m.assignedDevID,
		DisplayName:      hello.DisplayName,
	})
}

func (m *mockEZHostServer) handleAuthenticatedSession(ctx context.Context, conn *websocket.Conn, connID, challengeNonce string, first ezWireEnvelope) {
	var hello ezDeviceHelloPayload
	if err := json.Unmarshal(first.Payload, &hello); err != nil {
		return
	}
	m.mu.Lock()
	helloCopy := hello
	m.lastHello = &helloCopy
	pub := m.pubKey
	m.mu.Unlock()
	if len(pub) != ed25519.PublicKeySize || hello.DeviceID != m.assignedDevID {
		_ = m.sendFrame(ctx, conn, "h-err", "error", ezErrorPayload{Code: "authentication_failed"})
		return
	}
	authT := buildAuthTranscript(1, 0, connID, challengeNonce, hello.DeviceID, hello.DeviceNonce, hello.Capabilities, hello.OutputPreference)
	sig, err := b64uDecodeExact(hello.Signature, ed25519.SignatureSize)
	if err != nil || !ed25519.Verify(pub, authT, sig) {
		_ = m.sendFrame(ctx, conn, "h-err", "error", ezErrorPayload{Code: "authentication_failed"})
		return
	}

	m.mu.Lock()
	speechServices := m.speechServices
	if m.speechServices != nil && !hello.SpeechRelay {
		speechServices = nil
	}
	m.activeConn = conn
	m.activeCtx = ctx
	m.mu.Unlock()

	if err := m.sendFrame(ctx, conn, "h-ack", "hello_ack", ezHelloAckPayload{
		DeviceID:         hello.DeviceID,
		ConnectionID:     connID,
		Capabilities:     hello.Capabilities,
		OutputPreference: "text",
		SpeechServices:   speechServices,
	}); err != nil {
		return
	}

	// Verify application-level ping/pong works on active session.
	if err := m.sendFrame(ctx, conn, "h-ping-sess", "ping", map[string]any{"nonce": "sess-1", "sent_at_ms": 100}); err != nil {
		return
	}
	pongEnv, err := m.readFrame(ctx, conn)
	if err != nil || pongEnv.Type != "pong" {
		return
	}
	select {
	case m.pingReceived <- struct{}{}:
	default:
	}

	var activeASRReq ezSpeechAsrStartPayload
	var activeASRPcm bytes.Buffer
	for {
		msgType, raw, err := conn.Read(ctx)
		if err != nil {
			return
		}
		if msgType == websocket.MessageBinary {
			if len(raw) > ezRelayHeaderBytes && raw[0] == 1 && raw[1] == ezRelayKindUplink {
				payloadLen := int(binary.BigEndian.Uint16(raw[12:14]))
				activeASRPcm.Write(raw[ezRelayHeaderBytes : ezRelayHeaderBytes+payloadLen])
			}
			continue
		}
		var env ezWireEnvelope
		if err := json.Unmarshal(raw, &env); err != nil {
			return
		}
		if env.Type == "speech_asr_start" {
			_ = json.Unmarshal(env.Payload, &activeASRReq)
			activeASRPcm.Reset()
			continue
		}
		if env.Type == "speech_asr_stop" {
			var stopReq ezSpeechAsrStopPayload
			_ = json.Unmarshal(env.Payload, &stopReq)
			m.mu.Lock()
			m.relayASRAudio = append([]byte(nil), activeASRPcm.Bytes()...)
			m.mu.Unlock()
			_ = m.sendFrame(ctx, conn, "h-asr-res", "speech_asr_result", ezSpeechAsrResultPayload{
				RequestID: stopReq.RequestID,
				OK:        true,
				Text:      "Mock Host 识别结果",
			})
			continue
		}
		if env.Type == "speech_tts_request" {
			var ttsReq ezSpeechTtsRequestPayload
			_ = json.Unmarshal(env.Payload, &ttsReq)
			pcmOut := []byte{1, 0, 2, 0, 3, 0, 4, 0}
			_ = m.sendFrame(ctx, conn, "h-tts-start", "speech_tts_start", ezSpeechTtsStartPayload{
				RequestID:  ttsReq.RequestID,
				StreamID:   ttsReq.StreamID,
				OK:         true,
				TotalBytes: uint64(len(pcmOut)),
			})
			binFrame := make([]byte, ezRelayHeaderBytes+len(pcmOut))
			binFrame[0] = 1
			binFrame[1] = ezRelayKindDownlink
			binary.BigEndian.PutUint32(binFrame[4:8], ttsReq.StreamID)
			binary.BigEndian.PutUint32(binFrame[8:12], 0)
			binary.BigEndian.PutUint16(binFrame[12:14], uint16(len(pcmOut)))
			copy(binFrame[ezRelayHeaderBytes:], pcmOut)
			_ = conn.Write(ctx, websocket.MessageBinary, binFrame)
			_ = m.sendFrame(ctx, conn, "h-tts-end", "speech_tts_end", ezSpeechTtsEndPayload{
				RequestID: ttsReq.RequestID,
				StreamID:  ttsReq.StreamID,
				OK:        true,
			})
			continue
		}
		if env.Type == "text_input" {
			var in ezTextInputPayload
			if err := json.Unmarshal(env.Payload, &in); err != nil {
				return
			}
			select {
			case m.lastTextInput <- in.Text:
			default:
			}
			m.mu.Lock()
			sendQueuedUnavailable := m.queuedUnavailable
			m.mu.Unlock()
			if sendQueuedUnavailable {
				if err := m.sendFrame(ctx, conn, "h-ia-q", "input_accepted", map[string]any{
					"client_input_id": in.ClientInputID,
					"input_id":        "in_1",
					"run_id":          nil,
					"queue_state":     "queued",
				}); err != nil {
					return
				}
				if err := m.sendFrame(ctx, conn, "h-st-unavail", "state_changed", map[string]any{
					"run_id":          "r_1",
					"client_input_id": nil,
					"state":           "unavailable",
					"reason":          "assistant_text_unavailable",
				}); err != nil {
					return
				}
				continue
			}
			runID := "run-mock-99"
			if err := m.sendFrame(ctx, conn, "h-ia", "input_accepted", ezInputAcceptedPayload{
				ClientInputID: in.ClientInputID,
				InputID:       "input-mock-99",
				RunID:         runID,
				QueueState:    "accepted",
			}); err != nil {
				return
			}
			if err := m.sendFrame(ctx, conn, "h-out", "text_output", ezTextOutputPayload{
				OutputID: "out-mock-99",
				RunID:    runID,
				Text:     "你好，我是桌面端 EZ Assistant，已收到：" + in.Text,
			}); err != nil {
				return
			}
		}
	}
}

func TestEZDeviceBridgePairingSessionAndRouting(t *testing.T) {
	d, cleanup, err := NewData(&conf.Data{
		Database: &conf.Data_Database{Driver: "sqlite3", Source: ":memory:"},
	}, log.DefaultLogger)
	if err != nil {
		t.Fatalf("NewData: %v", err)
	}
	defer cleanup()

	installID := "0123456789abcdef0123456789abcdef"
	mockHost := newMockEZHostServer(t, installID, "dev-gateway-01")
	defer mockHost.close()

	settingsRepo := NewSQLiteSettingsRepo(d)
	bridge := NewEZDeviceBridge(&conf.Server{Http: &conf.Server_HTTP{Addr: "0.0.0.0:8088"}}, settingsRepo, log.DefaultLogger)
	impl := bridge.(*EZDeviceBridgeImpl)
	impl.discoverFn = func(_ context.Context, _ time.Duration) ([]biz.EZHostCandidate, error) {
		return nil, nil
	}

	// Wrong certificate fingerprint must be rejected during TLS handshake BEFORE upgrade!
	ctx := context.Background()
	if err := bridge.Start(ctx); err != nil {
		t.Fatalf("bridge.Start: %v", err)
	}
	defer func() {
		stopCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = bridge.Stop(stopCtx)
	}()

	wrongFP := strings.Repeat("b", 64)
	if _, err := dialPinnedWSS(ctx, mockHost.addr(), "/device", wrongFP, 1); err == nil {
		t.Fatal("expected TLS pinning failure on wrong certificate fingerprint")
	}

	st, err := bridge.StartPairing(ctx, biz.EZPairRequest{
		Address:                mockHost.addr(),
		Path:                   "/device",
		InstallationID:         installID,
		CertificateFingerprint: mockHost.certFP,
		DisplayName:            "Test Pika Gateway",
		PairingMode:            "spake2",
	})
	if err != nil {
		t.Fatalf("StartPairing: %v", err)
	}
	if len(st.PairingCode) != 6 {
		t.Fatalf("expected 6-digit pairing code, got %q", st.PairingCode)
	}

	// Wait until bridge receives pairing_pending ("awaiting_code") then feed code into mock Host.
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if bridge.Status(ctx).State == "awaiting_code" {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	mockHost.pairingCodeCh <- st.PairingCode

	// Wait for pairing completion and authenticated reconnect -> online.
	deadline = time.Now().Add(8 * time.Second)
	for time.Now().Before(deadline) {
		cur := bridge.Status(ctx)
		if cur.State == "online" && cur.IdentityState == "active" && cur.DeviceID == "dev-gateway-01" {
			break
		}
		time.Sleep(25 * time.Millisecond)
	}
	cur := bridge.Status(ctx)
	if cur.State != "online" || cur.IdentityState != "active" || cur.PairingCode != "" {
		t.Fatalf("unexpected post-pairing bridge status: %+v", cur)
	}

	select {
	case <-mockHost.pingReceived:
	case <-time.After(3 * time.Second):
		t.Fatal("expected active session ping/pong handshake")
	}

	// Execute a full voice-transcript turn via RoutingAgentAdapter (auto mode -> ez_device).
	grpcStub := &GRPCAgentAdapter{log: log.NewHelper(log.DefaultLogger), active: make(map[string]context.CancelFunc)}
	router := NewAgentAdapter(grpcStub, bridge, log.DefaultLogger)

	var statuses []string
	var deltas []string
	var doneReason string
	err = router.RunTurn(ctx, &biz.AgentTurnRequest{
		SessionID: "sess-1",
		TurnID:    "turn-1",
		Text:      "打开书房的灯",
	}, biz.AgentStreamCallbacks{
		OnStatus: func(state, _ string, _ bool) {
			statuses = append(statuses, state)
		},
		OnTextDelta: func(chunk, _ string) {
			deltas = append(deltas, chunk)
		},
		OnDone: func(finishReason, _ string) {
			doneReason = finishReason
		},
	})
	if err != nil {
		t.Fatalf("RunTurn failed: %v", err)
	}
	if len(deltas) != 1 || !strings.Contains(deltas[0], "打开书房的灯") || doneReason != "completed" {
		t.Fatalf("unexpected turn outputs: statuses=%v deltas=%v done=%q", statuses, deltas, doneReason)
	}

	// Switching to grpc mode closes ez_device session cleanly and errors clearly in ez_device-only mode after reset.
	st, err = bridge.SetMode(ctx, biz.EZModeGRPC)
	if err != nil || st.ActiveBackend != biz.EZModeGRPC {
		t.Fatalf("SetMode(grpc) failed: st=%+v err=%v", st, err)
	}
	if bridge.IsOnline() {
		t.Fatal("expected bridge offline when mode=grpc")
	}

	// Re-instantiate from same SQLite DB to verify persisted active identity survives restarts!
	bridge2 := NewEZDeviceBridge(&conf.Server{Http: &conf.Server_HTTP{Addr: "0.0.0.0:8088"}}, settingsRepo, log.DefaultLogger)
	st2 := bridge2.Status(ctx)
	if st2.IdentityState != "active" || st2.DeviceID != "dev-gateway-01" || st2.Mode != biz.EZModeGRPC {
		t.Fatalf("expected persisted active identity on reload, got %+v", st2)
	}

	// Reset pairing clears identity.
	stReset, err := bridge.ResetPairing(ctx)
	if err != nil || stReset.State != "unpaired" || stReset.IdentityState != "" {
		t.Fatalf("ResetPairing unexpected status: st=%+v err=%v", stReset, err)
	}
}

func TestEZMultiNICDialerAndDNSUnescape(t *testing.T) {
	if got := unescapeDNSLabel(`1f72dc9391f178d9f51f522c11ed73af\ \(4\)`); got != "1f72dc9391f178d9f51f522c11ed73af (4)" {
		t.Fatalf("unescapeDNSLabel got %q", got)
	}
	if got := unescapeDNSLabel(`host\032name`); got != "host name" {
		t.Fatalf("unescapeDNSLabel octal/dec byte got %q", got)
	}

	_, lanNet, _ := net.ParseCIDR("192.168.101.0/24")
	subnets := []ezLANSubnet{
		{localIP: net.ParseIP("192.168.101.13").To4(), net: lanNet},
	}
	ranked := rankEZCandidateIPv4s(
		[]net.IP{net.ParseIP("192.168.101.10")},
		[]net.IP{
			net.ParseIP("192.168.139.3"),
			net.ParseIP("198.18.0.1"),
			net.ParseIP("100.100.142.175"),
			net.ParseIP("192.168.101.10"),
		},
		subnets,
	)
	if len(ranked) != 2 || ranked[0] != "192.168.101.10" || ranked[1] != "192.168.139.3" {
		t.Fatalf("unexpected ranked IPv4s: %v", ranked)
	}

	// Verify staggered dialer ignores a blackholed first source IP and succeeds via the second source IP.
	attempts := []ezDialAttempt{
		{localIP: net.ParseIP("192.168.101.7"), dstHost: "192.168.101.10"},
		{localIP: net.ParseIP("192.168.101.13"), dstHost: "192.168.101.10"},
	}
	start := time.Now()
	c, err := dialStaggeredAttempts(
		context.Background(),
		"tcp4",
		"55360",
		attempts,
		60*time.Millisecond,
		2*time.Second,
		func(dCtx context.Context, localIP net.IP, _ string) (net.Conn, error) {
			if localIP.Equal(net.ParseIP("192.168.101.7")) {
				<-dCtx.Done()
				return nil, errors.New("simulated blackhole")
			}
			c1, c2 := net.Pipe()
			_ = c2.Close()
			return c1, nil
		},
	)
	if err != nil {
		t.Fatalf("dialStaggeredAttempts failed: %v", err)
	}
	_ = c.Close()
	if elapsed := time.Since(start); elapsed > 500*time.Millisecond {
		t.Fatalf("expected fast staggered failover <500ms, took %v", elapsed)
	}
}

func TestSubmitTurn_QueuedThenUnavailableResolvesImmediately(t *testing.T) {
	d, cleanup, err := NewData(&conf.Data{
		Database: &conf.Data_Database{Driver: "sqlite3", Source: ":memory:"},
	}, log.DefaultLogger)
	if err != nil {
		t.Fatalf("NewData: %v", err)
	}
	defer cleanup()

	installID := "fedcba9876543210fedcba9876543210"
	mockHost := newMockEZHostServer(t, installID, "dev-gateway-unavail")
	mockHost.queuedUnavailable = true
	defer mockHost.close()

	settingsRepo := NewSQLiteSettingsRepo(d)
	bridge := NewEZDeviceBridge(&conf.Server{Http: &conf.Server_HTTP{Addr: "0.0.0.0:8088"}}, settingsRepo, log.DefaultLogger)
	impl := bridge.(*EZDeviceBridgeImpl)
	impl.discoverFn = func(_ context.Context, _ time.Duration) ([]biz.EZHostCandidate, error) {
		return nil, nil
	}

	ctx := context.Background()
	if err := bridge.Start(ctx); err != nil {
		t.Fatalf("bridge.Start: %v", err)
	}
	defer func() {
		stopCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = bridge.Stop(stopCtx)
	}()

	st, err := bridge.StartPairing(ctx, biz.EZPairRequest{
		Address:                mockHost.addr(),
		Path:                   "/device",
		InstallationID:         installID,
		CertificateFingerprint: mockHost.certFP,
		DisplayName:            "Test Pika Unavail",
		PairingMode:            "spake2",
	})
	if err != nil {
		t.Fatalf("StartPairing: %v", err)
	}

	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if bridge.Status(ctx).State == "awaiting_code" {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	mockHost.pairingCodeCh <- st.PairingCode

	deadline = time.Now().Add(8 * time.Second)
	for time.Now().Before(deadline) {
		if bridge.IsOnline() {
			break
		}
		time.Sleep(25 * time.Millisecond)
	}
	if !bridge.IsOnline() {
		t.Fatalf("bridge did not come online: %+v", bridge.Status(ctx))
	}

	start := time.Now()
	err = bridge.RunTurn(ctx, &biz.AgentTurnRequest{
		SessionID: "sess-q",
		TurnID:    "turn-q",
		Text:      "test missing model",
	}, biz.AgentStreamCallbacks{})
	elapsed := time.Since(start)
	if err == nil || !strings.Contains(err.Error(), "assistant_text_unavailable") {
		t.Fatalf("expected error containing assistant_text_unavailable, got err=%v", err)
	}
	if elapsed >= time.Second {
		t.Fatalf("expected immediate error resolution (<1s), took %v", elapsed)
	}
}

func TestSubmitTurn_SlowCallbackDoesNotBlockSessionReader(t *testing.T) {
	d, cleanup, err := NewData(&conf.Data{
		Database: &conf.Data_Database{Driver: "sqlite3", Source: ":memory:"},
	}, log.DefaultLogger)
	if err != nil {
		t.Fatalf("NewData: %v", err)
	}
	defer cleanup()

	installID := "00112233445566778899aabbccddeeff"
	mockHost := newMockEZHostServer(t, installID, "dev-gateway-slowcb")
	defer mockHost.close()

	settingsRepo := NewSQLiteSettingsRepo(d)
	bridge := NewEZDeviceBridge(&conf.Server{Http: &conf.Server_HTTP{Addr: "0.0.0.0:8088"}}, settingsRepo, log.DefaultLogger)
	impl := bridge.(*EZDeviceBridgeImpl)
	impl.discoverFn = func(_ context.Context, _ time.Duration) ([]biz.EZHostCandidate, error) {
		return nil, nil
	}

	ctx := context.Background()
	if err := bridge.Start(ctx); err != nil {
		t.Fatalf("bridge.Start: %v", err)
	}
	defer func() {
		stopCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = bridge.Stop(stopCtx)
	}()

	st, err := bridge.StartPairing(ctx, biz.EZPairRequest{
		Address:                mockHost.addr(),
		Path:                   "/device",
		InstallationID:         installID,
		CertificateFingerprint: mockHost.certFP,
		DisplayName:            "Test Pika SlowCB",
		PairingMode:            "spake2",
	})
	if err != nil {
		t.Fatalf("StartPairing: %v", err)
	}
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if bridge.Status(ctx).State == "awaiting_code" {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	mockHost.pairingCodeCh <- st.PairingCode

	deadline = time.Now().Add(8 * time.Second)
	for time.Now().Before(deadline) {
		if bridge.IsOnline() {
			break
		}
		time.Sleep(25 * time.Millisecond)
	}
	if !bridge.IsOnline() {
		t.Fatalf("bridge did not come online: %+v", bridge.Status(ctx))
	}

	enteredSlow := make(chan struct{}, 1)
	turn1ErrCh := make(chan error, 1)
	go func() {
		turn1ErrCh <- bridge.RunTurn(ctx, &biz.AgentTurnRequest{
			SessionID: "sess-s1",
			TurnID:    "turn-s1",
			Text:      "第一句话",
		}, biz.AgentStreamCallbacks{
			OnTextDelta: func(_, _ string) {
				select {
				case enteredSlow <- struct{}{}:
				default:
				}
				time.Sleep(250 * time.Millisecond)
			},
		})
	}()

	select {
	case <-enteredSlow:
	case <-time.After(3 * time.Second):
		t.Fatal("timed out waiting for turn 1 slow callback")
	}

	// While turn 1's OnTextDelta is blocked, turn 2 must still be read and resolved quickly.
	start2 := time.Now()
	var turn2Text string
	err2 := bridge.RunTurn(ctx, &biz.AgentTurnRequest{
		SessionID: "sess-s2",
		TurnID:    "turn-s2",
		Text:      "第二句话",
	}, biz.AgentStreamCallbacks{
		OnTextDelta: func(chunk, _ string) {
			turn2Text += chunk
		},
	})
	elapsed2 := time.Since(start2)
	if err2 != nil || turn2Text == "" {
		t.Fatalf("expected turn 2 to succeed while turn 1 callback blocked, err=%v text=%q", err2, turn2Text)
	}
	if elapsed2 >= 180*time.Millisecond {
		t.Fatalf("expected turn 2 to complete without waiting for turn 1 callback (<180ms), took %v", elapsed2)
	}
	if !bridge.IsOnline() {
		t.Fatal("expected bridge session to stay online")
	}
	if err1 := <-turn1ErrCh; err1 != nil {
		t.Fatalf("turn 1 failed: %v", err1)
	}
}

func TestEZDeviceBridgeDirectConfirmAndMCPServers(t *testing.T) {
	d, cleanup, err := NewData(&conf.Data{
		Database: &conf.Data_Database{Driver: "sqlite3", Source: ":memory:"},
	}, log.DefaultLogger)
	if err != nil {
		t.Fatalf("NewData: %v", err)
	}
	defer cleanup()

	installID := "abcdef0123456789abcdef0123456789"
	mockHost := newMockEZHostServer(t, installID, "dev-direct-01")
	defer mockHost.close()

	settingsRepo := NewSQLiteSettingsRepo(d)
	bridge := NewEZDeviceBridge(&conf.Server{Http: &conf.Server_HTTP{Addr: "0.0.0.0:8088"}}, settingsRepo, log.DefaultLogger)
	impl := bridge.(*EZDeviceBridgeImpl)
	impl.discoverFn = func(_ context.Context, _ time.Duration) ([]biz.EZHostCandidate, error) {
		return nil, nil
	}

	ctx := context.Background()
	if err := bridge.Start(ctx); err != nil {
		t.Fatalf("bridge.Start: %v", err)
	}
	defer func() {
		stopCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = bridge.Stop(stopCtx)
	}()

	st, err := bridge.StartPairing(ctx, biz.EZPairRequest{
		Address:                mockHost.addr(),
		Path:                   "/device",
		InstallationID:         installID,
		CertificateFingerprint: mockHost.certFP,
		DisplayName:            "Direct Gateway",
		PairingMode:            "direct_confirm",
	})
	if err != nil {
		t.Fatalf("StartPairing: %v", err)
	}
	if st.PairingMode != "direct_confirm" || st.PairingCode != "" || len(st.KeyFingerprint) != 9 {
		t.Fatalf("expected direct_confirm status with fingerprint and empty code, got %+v", st)
	}

	deadline := time.Now().Add(8 * time.Second)
	for time.Now().Before(deadline) {
		if bridge.IsOnline() {
			break
		}
		time.Sleep(25 * time.Millisecond)
	}
	if !bridge.IsOnline() {
		t.Fatalf("direct_confirm bridge did not come online: %+v", bridge.Status(ctx))
	}

	cur := bridge.Status(ctx)
	if cur.DeviceID != "dev-direct-01" {
		t.Fatalf("expected DeviceID dev-direct-01, got %+v", cur)
	}
	mockHost.mu.Lock()
	lastHello := mockHost.lastHello
	mockHost.mu.Unlock()
	if lastHello == nil {
		t.Fatal("expected DeviceHello to be captured by mock Host")
	}
	if lastHello.ClientVersion != ezClientVersion {
		t.Fatalf("expected ClientVersion %q, got %q", ezClientVersion, lastHello.ClientVersion)
	}
	if len(lastHello.MCPServers) != 1 {
		t.Fatalf("expected 1 declared MCP server in DeviceHello, got %+v", lastHello.MCPServers)
	}
	decl := lastHello.MCPServers[0]
	if decl.ID != "esp-pika-gateway" || decl.Scheme != "http" || decl.Port != 8088 || decl.Path != "/mcp" {
		t.Fatalf("unexpected DeviceHello MCP declaration: %+v", decl)
	}
	if decl.Headers["X-Agent-ID"] != "ez-assistant" || decl.Headers["X-Speaker-Role"] != "owner" {
		t.Fatalf("unexpected DeviceHello MCP headers: %+v", decl.Headers)
	}
}

func TestEZDeviceBridgeDirectConfirmRejected(t *testing.T) {
	d, cleanup, err := NewData(&conf.Data{
		Database: &conf.Data_Database{Driver: "sqlite3", Source: ":memory:"},
	}, log.DefaultLogger)
	if err != nil {
		t.Fatalf("NewData: %v", err)
	}
	defer cleanup()

	installID := "11112222333344445555666677778888"
	mockHost := newMockEZHostServer(t, installID, "dev-direct-reject")
	mockHost.directModeAction = "reject"
	defer mockHost.close()

	settingsRepo := NewSQLiteSettingsRepo(d)
	bridge := NewEZDeviceBridge(&conf.Server{Http: &conf.Server_HTTP{Addr: "0.0.0.0:8088"}}, settingsRepo, log.DefaultLogger)
	ctx := context.Background()
	if err := bridge.Start(ctx); err != nil {
		t.Fatalf("bridge.Start: %v", err)
	}
	defer func() {
		stopCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = bridge.Stop(stopCtx)
	}()

	if _, err := bridge.StartPairing(ctx, biz.EZPairRequest{
		Address:                mockHost.addr(),
		Path:                   "/device",
		InstallationID:         installID,
		CertificateFingerprint: mockHost.certFP,
		DisplayName:            "Rejected Gateway",
		PairingMode:            "direct_confirm",
	}); err != nil {
		t.Fatalf("StartPairing: %v", err)
	}

	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		cur := bridge.Status(ctx)
		if cur.State == "unpaired" && strings.Contains(cur.LastError, "pairing_rejected") {
			return
		}
		time.Sleep(25 * time.Millisecond)
	}
	t.Fatalf("expected unpaired with pairing_rejected error, got %+v", bridge.Status(ctx))
}

func TestEZDeviceBridgeAutoFallbackToSpake2(t *testing.T) {
	d, cleanup, err := NewData(&conf.Data{
		Database: &conf.Data_Database{Driver: "sqlite3", Source: ":memory:"},
	}, log.DefaultLogger)
	if err != nil {
		t.Fatalf("NewData: %v", err)
	}
	defer cleanup()

	installID := "9999aaaabbbbccccddddeeeeffff0000"
	mockHost := newMockEZHostServer(t, installID, "dev-fallback-01")
	mockHost.directModeAction = "legacy_reject"
	defer mockHost.close()

	settingsRepo := NewSQLiteSettingsRepo(d)
	bridge := NewEZDeviceBridge(&conf.Server{Http: &conf.Server_HTTP{Addr: "0.0.0.0:8088"}}, settingsRepo, log.DefaultLogger)
	ctx := context.Background()
	if err := bridge.Start(ctx); err != nil {
		t.Fatalf("bridge.Start: %v", err)
	}
	defer func() {
		stopCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = bridge.Stop(stopCtx)
	}()

	if _, err := bridge.StartPairing(ctx, biz.EZPairRequest{
		Address:                mockHost.addr(),
		Path:                   "/device",
		InstallationID:         installID,
		CertificateFingerprint: mockHost.certFP,
		DisplayName:            "Fallback Gateway",
	}); err != nil {
		t.Fatalf("StartPairing: %v", err)
	}

	var fallbackCode string
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		cur := bridge.Status(ctx)
		if cur.PairingMode == "spake2" && len(cur.PairingCode) == 6 && cur.State == "awaiting_code" {
			fallbackCode = cur.PairingCode
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if len(fallbackCode) != 6 {
		t.Fatalf("expected automatic fallback to SPAKE2 awaiting_code, got %+v", bridge.Status(ctx))
	}
	mockHost.pairingCodeCh <- fallbackCode

	deadline = time.Now().Add(8 * time.Second)
	for time.Now().Before(deadline) {
		if bridge.IsOnline() {
			return
		}
		time.Sleep(25 * time.Millisecond)
	}
	t.Fatalf("expected bridge online after SPAKE2 fallback, got %+v", bridge.Status(ctx))
}

func TestEZMDNSPairingModesParsing(t *testing.T) {
	installID := "0123456789abcdef0123456789abcdef"
	certFP := strings.Repeat("a", 64)
	entry := &zeroconf.ServiceEntry{
		ServiceRecord: zeroconf.ServiceRecord{Instance: "ez-desktop"},
		HostName:      "ez-desktop.local.",
		Port:          54321,
		AddrIPv4:      []net.IP{net.ParseIP("192.168.101.20")},
		Text: []string{
			"protocol_major=1",
			"protocol_minor=0",
			"installation_id=" + installID,
			"certificate_fingerprint=" + certFP,
			"path=/device",
			"pairing_available=true",
			"pairing_modes=direct_confirm,spake2",
		},
	}
	cand, ok := parseEZMDNSEntry(entry, nil)
	if !ok {
		t.Fatal("expected parseEZMDNSEntry to succeed")
	}
	if len(cand.PairingModes) != 2 || cand.PairingModes[0] != "direct_confirm" || cand.PairingModes[1] != "spake2" {
		t.Fatalf("unexpected PairingModes from parseEZMDNSEntry: %+v", cand.PairingModes)
	}

	collector := newEZMDNSCollector(nil)
	fqdn := "ez-desktop." + ezMDNSServiceType + "." + ezMDNSDomain
	collector.ingest(&dns.Msg{
		Answer: []dns.RR{
			&dns.PTR{
				Hdr: dns.RR_Header{Name: ezMDNSServiceType + "." + ezMDNSDomain, Rrtype: dns.TypePTR},
				Ptr: fqdn,
			},
			&dns.SRV{
				Hdr:    dns.RR_Header{Name: fqdn, Rrtype: dns.TypeSRV},
				Port:   54321,
				Target: "ez-desktop.local.",
			},
			&dns.TXT{
				Hdr: dns.RR_Header{Name: fqdn, Rrtype: dns.TypeTXT},
				Txt: []string{
					"protocol_major=1",
					"protocol_minor=0",
					"installation_id=" + installID,
					"certificate_fingerprint=" + certFP,
					"path=/device",
					"pairing_available=true",
					"pairing_modes=direct_confirm,spake2",
				},
			},
			&dns.A{
				Hdr: dns.RR_Header{Name: "ez-desktop.local.", Rrtype: dns.TypeA},
				A:   net.ParseIP("192.168.101.20"),
			},
		},
	}, net.ParseIP("192.168.101.20"))
	cands := collector.candidates()
	if len(cands) != 1 || len(cands[0].PairingModes) != 2 || cands[0].PairingModes[0] != "direct_confirm" {
		t.Fatalf("unexpected collector candidates PairingModes: %+v", cands)
	}
}

func TestEZDeviceBridgeConsoleAndSpeechRelay(t *testing.T) {
	d, cleanup, err := NewData(&conf.Data{
		Database: &conf.Data_Database{Driver: "sqlite3", Source: ":memory:"},
	}, log.DefaultLogger)
	if err != nil {
		t.Fatalf("NewData: %v", err)
	}
	defer cleanup()

	installID := "abcdef0123456789abcdef0123456789"
	mockHost := newMockEZHostServer(t, installID, "dev-relay-01")
	mockHost.speechServices = &biz.EZSpeechServicesStatus{ASR: "ready", TTS: "unavailable"}
	defer mockHost.close()

	settingsRepo := NewSQLiteSettingsRepo(d)
	bridge := NewEZDeviceBridge(&conf.Server{Http: &conf.Server_HTTP{Addr: "0.0.0.0:8088"}}, settingsRepo, log.DefaultLogger)
	impl := bridge.(*EZDeviceBridgeImpl)
	impl.discoverFn = func(_ context.Context, _ time.Duration) ([]biz.EZHostCandidate, error) {
		return nil, nil
	}

	ctx := context.Background()
	if err := bridge.Start(ctx); err != nil {
		t.Fatalf("bridge.Start: %v", err)
	}
	defer func() {
		stopCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = bridge.Stop(stopCtx)
	}()

	if _, err := bridge.StartPairing(ctx, biz.EZPairRequest{
		Address:                mockHost.addr(),
		Path:                   "/device",
		InstallationID:         installID,
		CertificateFingerprint: mockHost.certFP,
		DisplayName:            "Relay Gateway",
		PairingMode:            "direct_confirm",
	}); err != nil {
		t.Fatalf("StartPairing: %v", err)
	}

	deadline := time.Now().Add(8 * time.Second)
	for time.Now().Before(deadline) && !bridge.IsOnline() {
		time.Sleep(20 * time.Millisecond)
	}
	if !bridge.IsOnline() {
		t.Fatalf("bridge did not come online: %+v", bridge.Status(ctx))
	}

	mockHost.mu.Lock()
	hello := mockHost.lastHello
	mockConn := mockHost.activeConn
	mockCtx := mockHost.activeCtx
	mockHost.mu.Unlock()
	if hello == nil || !hello.SpeechRelay || hello.Console == nil {
		t.Fatalf("expected Console and SpeechRelay=true in DeviceHello, got %+v", hello)
	}
	if hello.Console.Port != 8088 || hello.Console.APIPrefix != "/api/v1" || hello.Console.UIPath != "/ui/" {
		t.Fatalf("unexpected Console declaration: %+v", hello.Console)
	}
	if !bridge.CanUseASR() || bridge.CanUseTTS() {
		t.Fatalf("expected initial CanUseASR=true CanUseTTS=false, got asr=%v tts=%v", bridge.CanUseASR(), bridge.CanUseTTS())
	}

	// Host pushes speech_status_changed enabling TTS live
	_ = mockHost.sendFrame(mockCtx, mockConn, "h-speech-upd", "speech_status_changed", ezSpeechStatusChangedPayload{
		SpeechServices: biz.EZSpeechServicesStatus{ASR: "ready", TTS: "ready"},
	})
	ttsDeadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(ttsDeadline) && !bridge.CanUseTTS() {
		time.Sleep(15 * time.Millisecond)
	}
	if !bridge.CanUseTTS() {
		t.Fatalf("expected CanUseTTS()=true after speech_status_changed")
	}

	pcmInput := bytes.Repeat([]byte{0x10, 0x20}, 320)
	text, err := bridge.RecognizeSpeech(ctx, pcmInput)
	if err != nil || text != "Mock Host 识别结果" {
		t.Fatalf("RecognizeSpeech failed: text=%q err=%v", text, err)
	}
	mockHost.mu.Lock()
	gotAudio := mockHost.relayASRAudio
	mockHost.mu.Unlock()
	if !bytes.Equal(gotAudio, pcmInput) {
		t.Fatalf("uploaded PCM mismatch: got %d bytes want %d", len(gotAudio), len(pcmInput))
	}

	ttsPCM, err := bridge.SynthesizeSpeech(ctx, "播报测试")
	if err != nil || !bytes.Equal(ttsPCM, []byte{1, 0, 2, 0, 3, 0, 4, 0}) {
		t.Fatalf("SynthesizeSpeech failed: pcm=%v err=%v", ttsPCM, err)
	}

	// Disconnect tears down speech readiness
	_ = mockConn.Close(websocket.StatusNormalClosure, "drop")
	dropDeadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(dropDeadline) && (bridge.CanUseASR() || bridge.CanUseTTS()) {
		time.Sleep(15 * time.Millisecond)
	}
	if bridge.CanUseASR() || bridge.CanUseTTS() {
		t.Fatalf("expected speech readiness cleared on disconnect")
	}
}
