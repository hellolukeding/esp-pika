package server

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"esp-pika/backend/gateway/internal/biz"
	"esp-pika/backend/gateway/internal/conf"

	"github.com/coder/websocket"
	"github.com/go-kratos/kratos/v2/log"
)

const maxMediaUploadBytes = 5 << 20 // 5 MiB

// DeviceWSHandler manages ESP32 WebSocket control/audio connections and media uploads.
type DeviceWSHandler struct {
	macAuth  *biz.MacAuthUsecase
	devices  *biz.DeviceUsecase
	speakers *biz.SpeakerUsecase
	arbiter  *biz.WakeArbiter
	router   *biz.SessionRouter
	events   biz.EventBus
	agent    biz.AgentAdapter
	speech   *biz.SpeechPipeline
	mediaDir string
	log      *log.Helper

	wg sync.WaitGroup
}

// NewDeviceWSHandler constructs a DeviceWSHandler and its WakeArbiter.
func NewDeviceWSHandler(
	cServer *conf.Server,
	cArbiter *conf.Arbiter,
	macAuth *biz.MacAuthUsecase,
	devices *biz.DeviceUsecase,
	speakers *biz.SpeakerUsecase,
	router *biz.SessionRouter,
	events biz.EventBus,
	agent biz.AgentAdapter,
	speech *biz.SpeechPipeline,
	logger log.Logger,
) (*DeviceWSHandler, func()) {
	window := 120 * time.Millisecond
	if cArbiter != nil && cArbiter.Window != nil {
		window = cArbiter.Window.AsDuration()
	}
	mediaDir := "./data/media"
	if cServer != nil && cServer.MediaDir != "" {
		mediaDir = cServer.MediaDir
	}
	arbiter := biz.NewWakeArbiter(window, devices, events, logger)
	h := &DeviceWSHandler{
		macAuth:  macAuth,
		devices:  devices,
		speakers: speakers,
		arbiter:  arbiter,
		router:   router,
		events:   events,
		agent:    agent,
		speech:   speech,
		mediaDir: mediaDir,
		log:      log.NewHelper(logger),
	}
	return h, func() {
		arbiter.Close()
		h.wg.Wait()
	}
}

// MediaDir returns the local directory where uploaded camera/sensor media blobs are stored.
func (h *DeviceWSHandler) MediaDir() string {
	return h.mediaDir
}

type wsSpeechState struct {
	mu          sync.Mutex
	asr         biz.ASRStream
	speakerID   string
	speakerRole biz.SpeakerRole
	confidence  float32
	agentID     string
	grayFrames  [][]byte
	turnCancel  context.CancelFunc
}

func (s *wsSpeechState) resetASR() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.asr != nil {
		_ = s.asr.Close()
		s.asr = nil
	}
	s.grayFrames = nil
}

func (s *wsSpeechState) cancelActiveTurn() {
	s.mu.Lock()
	c := s.turnCancel
	s.turnCancel = nil
	s.mu.Unlock()
	if c != nil {
		c()
	}
}

// ServeWS handles `/ws/device` WebSocket upgrades with strict MAC whitelist verification.
func (h *DeviceWSHandler) ServeWS(w http.ResponseWriter, r *http.Request) {
	rawMAC := r.Header.Get("X-Device-MAC")
	if rawMAC == "" {
		rawMAC = r.URL.Query().Get("mac")
	}
	allowed, err := h.macAuth.Authorize(r.Context(), rawMAC)
	if err != nil {
		http.Error(w, "forbidden: unauthorized device mac", http.StatusForbidden)
		return
	}

	// Clear HTTP server per-request deadlines before upgrading to a long-lived WebSocket.
	rc := http.NewResponseController(w)
	_ = rc.SetReadDeadline(time.Time{})
	_ = rc.SetWriteDeadline(time.Time{})

	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{
		InsecureSkipVerify: true,
	})
	if err != nil {
		h.log.WithContext(r.Context()).Warnf("websocket accept failed for mac=%s: %v", allowed.MAC, err)
		return
	}

	sink := newWSSink(conn)
	// Detach from Kratos's HTTP request timeout (e.g. 30s) so persistent device sessions stay open.
	ctx, cancel := context.WithCancel(context.WithoutCancel(r.Context()))
	pingDone := make(chan struct{})
	go func() {
		defer close(pingDone)
		ticker := time.NewTicker(20 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				pctx, pcancel := context.WithTimeout(ctx, 8*time.Second)
				err := conn.Ping(pctx)
				pcancel()
				if err != nil {
					cancel()
					return
				}
			}
		}
	}()
	defer func() {
		cancel()
		<-pingDone
	}()

	spState := &wsSpeechState{}
	var connID uint64
	defer func() {
		spState.cancelActiveTurn()
		spState.resetASR()
		sink.failPending(fmt.Errorf("connection closed"))
		if connID != 0 {
			h.devices.Disconnect(context.Background(), allowed.MAC, connID)
		}
		_ = conn.CloseNow()
	}()

	for {
		msgType, data, err := conn.Read(ctx)
		if err != nil {
			h.log.WithContext(ctx).Infof("device mac=%s disconnected: %v", allowed.MAC, err)
			return
		}
		switch msgType {
		case websocket.MessageText:
			newID, ok := h.handleTextFrame(ctx, allowed, sink, spState, connID, data)
			if !ok {
				return
			}
			connID = newID
		case websocket.MessageBinary:
			spState.mu.Lock()
			asr := spState.asr
			if len(spState.grayFrames) < 25 {
				cp := make([]byte, len(data))
				copy(cp, data)
				spState.grayFrames = append(spState.grayFrames, cp)
			}
			spState.mu.Unlock()
			if asr != nil {
				_ = asr.WriteOpusFrame(ctx, data)
			}
		}
	}
}

type wsEnvelope struct {
	Type string `json:"type"`
}

func (h *DeviceWSHandler) handleTextFrame(ctx context.Context, allowed *biz.AllowedMac, sink *wsSink, spState *wsSpeechState, connID uint64, raw []byte) (uint64, bool) {
	var env wsEnvelope
	if err := json.Unmarshal(raw, &env); err != nil {
		_ = sink.SendJSON(ctx, map[string]any{"type": "error", "code": "INVALID_JSON"})
		return connID, true
	}

	switch env.Type {
	case "device.hello":
		var msg struct {
			DeviceID        string                 `json:"device_id"`
			MAC             string                 `json:"mac"`
			GroupID         string                 `json:"group_id"`
			Zone            string                 `json:"zone"`
			Chip            string                 `json:"chip"`
			VPVersion       uint64                 `json:"vp_version"`
			FirmwareVersion string                 `json:"firmware_version"`
			Capabilities    []biz.DeviceCapability `json:"capabilities"`
		}
		if err := json.Unmarshal(raw, &msg); err != nil {
			return connID, true
		}
		// Never trust unverified MAC claims in payload over the authenticated handshake MAC.
		if msg.MAC != "" {
			if norm, err := biz.NormalizeMAC(msg.MAC); err != nil || norm != allowed.MAC {
				_ = sink.Close("mac mismatch")
				return connID, false
			}
		}
		groupID := msg.GroupID
		if groupID == "" {
			groupID = allowed.GroupID
		}
		zone := msg.Zone
		if zone == "" {
			zone = allowed.Zone
		}
		hello := &biz.DeviceHello{
			DeviceID:        allowed.MAC,
			MAC:             allowed.MAC,
			GroupID:         groupID,
			Zone:            zone,
			Chip:            msg.Chip,
			VPVersion:       msg.VPVersion,
			FirmwareVersion: msg.FirmwareVersion,
			Capabilities:    msg.Capabilities,
		}
		newID, err := h.devices.RegisterConnection(ctx, hello, sink)
		if err != nil {
			h.log.WithContext(ctx).Errorf("register connection failed mac=%s: %v", allowed.MAC, err)
			return connID, false
		}
		_ = sink.SendJSON(ctx, map[string]any{
			"type":      "device.welcome",
			"device_id": allowed.MAC,
			"group_id":  groupID,
			"zone":      zone,
		})
		if h.speakers != nil {
			_ = h.speakers.SyncDeviceIfBehind(ctx, allowed.MAC, groupID, msg.VPVersion)
		}
		return newID, true

	case "wake.detected":
		var msg struct {
			GroupID      string  `json:"group_id"`
			Zone         string  `json:"zone"`
			WakeWord     string  `json:"wake_word"`
			WakeScore    float32 `json:"wake_score"`
			SNRDB        float32 `json:"snr_db"`
			DetectedAtMS int64   `json:"detected_at_ms"`
		}
		if err := json.Unmarshal(raw, &msg); err != nil {
			return connID, true
		}
		groupID := msg.GroupID
		if groupID == "" {
			groupID = allowed.GroupID
		}
		zone := msg.Zone
		if zone == "" {
			zone = allowed.Zone
		}
		h.arbiter.Submit(ctx, biz.WakeCandidate{
			DeviceID:     allowed.MAC,
			GroupID:      groupID,
			Zone:         zone,
			WakeWord:     msg.WakeWord,
			WakeScore:    msg.WakeScore,
			SNRDB:        msg.SNRDB,
			DetectedAtMS: msg.DetectedAtMS,
		})

	case "speech.start":
		var msg struct {
			SpeakerID   string          `json:"speaker_id"`
			SpeakerRole biz.SpeakerRole `json:"speaker_role"`
			Confidence  float32         `json:"confidence"`
			AgentID     string          `json:"agent_id"`
		}
		if err := json.Unmarshal(raw, &msg); err == nil && h.router != nil {
			spState.resetASR()
			var asrStream biz.ASRStream
			if h.speech != nil && h.speech.ASR != nil {
				asrStream, _ = h.speech.ASR.StartStream(ctx, allowed.MAC)
			}
			spState.mu.Lock()
			spState.asr = asrStream
			spState.speakerID = msg.SpeakerID
			spState.speakerRole = msg.SpeakerRole
			spState.confidence = msg.Confidence
			spState.agentID = msg.AgentID
			spState.grayFrames = nil
			spState.mu.Unlock()

			route := h.router.ResolveTurn(ctx, msg.SpeakerID, msg.AgentID, allowed.MAC)
			_ = sink.SendJSON(ctx, map[string]any{
				"type":       "session.routed",
				"session_id": route.SessionID,
				"agent_id":   route.AgentID,
			})
		}

	case "speech.stop":
		var msg struct {
			TurnID string `json:"turn_id"`
		}
		_ = json.Unmarshal(raw, &msg)

		spState.mu.Lock()
		asr := spState.asr
		spState.asr = nil
		gray := spState.grayFrames
		spState.grayFrames = nil
		speakerID := spState.speakerID
		speakerRole := spState.speakerRole
		confidence := spState.confidence
		agentID := spState.agentID
		spState.mu.Unlock()

		var transcript string
		if asr != nil {
			transcript, _ = asr.Finish(ctx)
			_ = asr.Close()
		}
		// Perform cloud gray-zone speaker verification when confidence is in [0.65, 0.85) or missing.
		if h.speech != nil && h.speech.Reviewer != nil && len(gray) > 0 &&
			(speakerID == "" || (confidence >= 0.65 && confidence < 0.85)) {
			if revSpk, revRole, revConf, err := h.speech.Reviewer.Review(ctx, allowed.MAC, gray); err == nil && revSpk != "" {
				speakerID, speakerRole, confidence = revSpk, revRole, revConf
			}
		}
		if strings.TrimSpace(transcript) != "" && h.agent != nil {
			turnID := msg.TurnID
			if turnID == "" {
				turnID = "turn_" + randomHex(6)
			}
			route := h.router.ResolveTurn(ctx, speakerID, agentID, allowed.MAC)
			h.dispatchAgentTurn(ctx, sink, spState, allowed, route, turnID, transcript, speakerID, speakerRole, confidence)
		}

	case "shadow.update":
		var msg struct {
			BatteryPercent *int32             `json:"battery_percent"`
			RSSI           *int32             `json:"rssi"`
			Volume         *int32             `json:"volume"`
			VADState       string             `json:"vad_state"`
			Metrics        map[string]float64 `json:"metrics"`
			Attributes     map[string]string  `json:"attributes"`
		}
		if err := json.Unmarshal(raw, &msg); err == nil {
			_, _ = h.devices.UpdateShadow(ctx, allowed.MAC, msg.BatteryPercent, msg.RSSI, msg.Volume, msg.VADState, msg.Metrics, msg.Attributes)
		}

	case "sensor.push":
		var msg struct {
			EventType   string          `json:"event_type"`
			Payload     json.RawMessage `json:"payload"`
			NotifyAgent bool            `json:"notify_agent"`
			AgentID     string          `json:"agent_id"`
		}
		if err := json.Unmarshal(raw, &msg); err == nil {
			if h.events != nil {
				_ = h.events.PublishSensorPush(ctx, allowed.MAC, allowed.GroupID, allowed.Zone, msg.EventType, string(msg.Payload))
			}
			if msg.NotifyAgent && h.agent != nil {
				route := h.router.ResolveTurn(ctx, "", msg.AgentID, allowed.MAC)
				h.dispatchAgentEvent(ctx, sink, allowed, route, msg.EventType, string(msg.Payload))
			}
		}

	case "text.turn", "speech.turn":
		var msg struct {
			TurnID      string          `json:"turn_id"`
			Text        string          `json:"text"`
			SpeakerID   string          `json:"speaker_id"`
			SpeakerRole biz.SpeakerRole `json:"speaker_role"`
			Confidence  float32         `json:"confidence"`
			AgentID     string          `json:"agent_id"`
		}
		if err := json.Unmarshal(raw, &msg); err == nil && strings.TrimSpace(msg.Text) != "" && h.agent != nil {
			turnID := msg.TurnID
			if turnID == "" {
				turnID = "turn_" + randomHex(6)
			}
			route := h.router.ResolveTurn(ctx, msg.SpeakerID, msg.AgentID, allowed.MAC)
			h.dispatchAgentTurn(ctx, sink, spState, allowed, route, turnID, msg.Text, msg.SpeakerID, msg.SpeakerRole, msg.Confidence)
		}

	case "turn.cancel", "speech.barge_in":
		var msg struct {
			SessionID string `json:"session_id"`
			TurnID    string `json:"turn_id"`
			Reason    string `json:"reason"`
		}
		if err := json.Unmarshal(raw, &msg); err == nil {
			spState.cancelActiveTurn()
			spState.resetASR()
			_ = sink.SendJSON(ctx, map[string]any{"type": "audio.stop"})
			if h.agent != nil {
				_ = h.agent.CancelTurn(ctx, msg.SessionID, msg.TurnID, msg.Reason)
			}
		}

	case "command.result":
		var msg struct {
			CommandID    string `json:"command_id"`
			OK           bool   `json:"ok"`
			ErrorCode    string `json:"error_code"`
			ErrorMessage string `json:"error_message"`
			ResultJSON   string `json:"result_json"`
			MediaURL     string `json:"media_url"`
		}
		if err := json.Unmarshal(raw, &msg); err == nil {
			sink.resolvePending(&biz.DeviceCommandResult{
				CommandID:    msg.CommandID,
				DeviceID:     allowed.MAC,
				OK:           msg.OK,
				ErrorCode:    msg.ErrorCode,
				ErrorMessage: msg.ErrorMessage,
				ResultJSON:   msg.ResultJSON,
				MediaURL:     msg.MediaURL,
			})
		}

	case "ping":
		_ = sink.SendJSON(ctx, map[string]any{"type": "pong", "ts": time.Now().UnixMilli()})
	}

	return connID, true
}

func (h *DeviceWSHandler) makeAgentCallbacks(ctx context.Context, sink *wsSink, route biz.SessionRoute, turnID, speakerID string) biz.AgentStreamCallbacks {
	var chunker biz.SentenceChunker
	resolvedVoice := ""
	if h.speech != nil {
		resolvedVoice = h.speech.ResolveSpeakerVoice(ctx, speakerID)
	}
	synthesizeAndSend := func(spokenRaw, overrideVoice string) {
		if h.speech == nil || h.speech.TTS == nil {
			return
		}
		spoken := biz.SpokenFilter(spokenRaw)
		if spoken == "" {
			return
		}
		if h.speech.Arbitrator != nil && !h.speech.Arbitrator.Acquire(route.SourceDeviceID, turnID, biz.AudioPriorityConversation) {
			return
		}
		v := overrideVoice
		if v == "" {
			v = resolvedVoice
		}
		frames, err := h.speech.TTS.Synthesize(ctx, spoken, v)
		if err != nil {
			return
		}
		for _, fr := range frames {
			_ = sink.SendAudio(ctx, fr)
		}
	}

	return biz.AgentStreamCallbacks{
		OnStatus: func(state, message string, speakHint bool) {
			_ = sink.SendJSON(ctx, map[string]any{
				"type":       "agent.status",
				"session_id": route.SessionID,
				"turn_id":    turnID,
				"state":      state,
				"message":    message,
				"speak_hint": speakHint,
			})
		},
		OnTextDelta: func(chunk, ttsVoice string) {
			_ = sink.SendJSON(ctx, map[string]any{
				"type":       "agent.text_delta",
				"session_id": route.SessionID,
				"turn_id":    turnID,
				"chunk":      chunk,
				"tts_voice":  ttsVoice,
			})
			for _, sentence := range chunker.Push(chunk) {
				synthesizeAndSend(sentence, ttsVoice)
			}
		},
		OnUIUpdate: func(targetDeviceID, emotion, text, cardJSON string) {
			payload := map[string]any{
				"type":      "agent.ui_update",
				"emotion":   emotion,
				"text":      text,
				"card_json": cardJSON,
			}
			if targetDeviceID != "" && targetDeviceID != route.SourceDeviceID && h.devices != nil {
				_ = h.devices.SendToDevice(ctx, targetDeviceID, payload)
				return
			}
			_ = sink.SendJSON(ctx, payload)
		},
		OnDone: func(finishReason, errorMessage string) {
			if rem := chunker.Flush(); rem != "" {
				synthesizeAndSend(rem, "")
			}
			if h.speech != nil && h.speech.Arbitrator != nil {
				h.speech.Arbitrator.Release(route.SourceDeviceID, turnID)
			}
			_ = sink.SendJSON(ctx, map[string]any{
				"type":          "agent.done",
				"session_id":    route.SessionID,
				"turn_id":       turnID,
				"finish_reason": finishReason,
				"error_message": errorMessage,
			})
		},
	}
}

func (h *DeviceWSHandler) dispatchAgentTurn(
	ctx context.Context,
	sink *wsSink,
	spState *wsSpeechState,
	allowed *biz.AllowedMac,
	route biz.SessionRoute,
	turnID, text, speakerID string,
	role biz.SpeakerRole,
	confidence float32,
) {
	sh, _ := h.devices.GetShadow(ctx, allowed.MAC)
	fleetJSON, _ := h.devices.FleetTopologyJSON(ctx, allowed.GroupID)
	req := &biz.AgentTurnRequest{
		SessionID:        route.SessionID,
		TurnID:           turnID,
		AgentID:          route.AgentID,
		Text:             text,
		SpeakerID:        speakerID,
		SpeakerRole:      role,
		Confidence:       confidence,
		SourceDeviceID:   allowed.MAC,
		GroupID:          allowed.GroupID,
		Zone:             allowed.Zone,
		Shadow:           sh,
		FleetSummaryJSON: fleetJSON,
	}
	turnCtx, turnCancel := context.WithCancel(ctx)
	if spState != nil {
		spState.cancelActiveTurn()
		spState.mu.Lock()
		spState.turnCancel = turnCancel
		spState.mu.Unlock()
	}
	cb := h.makeAgentCallbacks(turnCtx, sink, route, turnID, speakerID)
	h.wg.Add(1)
	go func() {
		defer h.wg.Done()
		defer turnCancel()
		if err := h.agent.RunTurn(turnCtx, req, cb); err != nil {
			h.log.WithContext(turnCtx).Warnf("agent turn failed session=%s turn=%s: %v", route.SessionID, turnID, err)
			_ = sink.SendJSON(ctx, map[string]any{
				"type":          "agent.done",
				"session_id":    route.SessionID,
				"turn_id":       turnID,
				"finish_reason": "failed",
				"error_message": err.Error(),
			})
		}
	}()
}

func (h *DeviceWSHandler) dispatchAgentEvent(
	ctx context.Context,
	sink *wsSink,
	allowed *biz.AllowedMac,
	route biz.SessionRoute,
	eventType, payloadJSON string,
) {
	evt := &biz.AgentDeviceEvent{
		SessionID:      route.SessionID,
		AgentID:        route.AgentID,
		SourceDeviceID: allowed.MAC,
		GroupID:        allowed.GroupID,
		Zone:           allowed.Zone,
		EventType:      eventType,
		PayloadJSON:    payloadJSON,
		OccurredAt:     time.Now(),
	}
	cb := h.makeAgentCallbacks(ctx, sink, route, "", "")
	h.wg.Add(1)
	go func() {
		defer h.wg.Done()
		if err := h.agent.SendEvent(ctx, evt, cb); err != nil {
			h.log.WithContext(ctx).Warnf("agent event failed session=%s event=%s: %v", route.SessionID, eventType, err)
		}
	}()
}

// HandleMediaUpload handles `POST /api/v1/media/upload` for camera JPEG frames and large sensor payloads.
func (h *DeviceWSHandler) HandleMediaUpload(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	rawMAC := r.Header.Get("X-Device-MAC")
	allowed, err := h.macAuth.Authorize(r.Context(), rawMAC)
	if err != nil {
		http.Error(w, "forbidden: unauthorized device mac", http.StatusForbidden)
		return
	}

	r.Body = http.MaxBytesReader(w, r.Body, maxMediaUploadBytes)
	payload, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, "failed to read media payload", http.StatusBadRequest)
		return
	}

	safeMAC := strings.ReplaceAll(allowed.MAC, ":", "-")
	dir := filepath.Join(h.mediaDir, safeMAC)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		http.Error(w, "failed to prepare media storage", http.StatusInternalServerError)
		return
	}
	filename := fmt.Sprintf("%d_%s.jpg", time.Now().UnixMilli(), randomHex(4))
	fullPath := filepath.Join(dir, filename)
	if err := os.WriteFile(fullPath, payload, 0o644); err != nil {
		http.Error(w, "failed to store media", http.StatusInternalServerError)
		return
	}

	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{
		"ok":        true,
		"mac":       allowed.MAC,
		"size":      len(payload),
		"path":      fullPath,
		"media_url": fmt.Sprintf("/media/%s/%s", safeMAC, filename),
	})
}

type wsSink struct {
	conn    *websocket.Conn
	writeMu sync.Mutex

	pendingMu sync.Mutex
	pending   map[string]chan *biz.DeviceCommandResult
}

func newWSSink(conn *websocket.Conn) *wsSink {
	return &wsSink{
		conn:    conn,
		pending: make(map[string]chan *biz.DeviceCommandResult),
	}
}

func (s *wsSink) SendJSON(ctx context.Context, v any) error {
	b, err := json.Marshal(v)
	if err != nil {
		return fmt.Errorf("marshal ws json: %w", err)
	}
	s.writeMu.Lock()
	defer s.writeMu.Unlock()
	return s.conn.Write(ctx, websocket.MessageText, b)
}

func (s *wsSink) SendAudio(ctx context.Context, opusFrame []byte) error {
	s.writeMu.Lock()
	defer s.writeMu.Unlock()
	return s.conn.Write(ctx, websocket.MessageBinary, opusFrame)
}

func (s *wsSink) InvokeCommand(ctx context.Context, cmd *biz.DeviceCommand) (*biz.DeviceCommandResult, error) {
	if cmd.CommandID == "" {
		cmd.CommandID = "cmd_" + randomHex(8)
	}
	timeout := time.Duration(cmd.TimeoutMS) * time.Millisecond
	if timeout <= 0 {
		timeout = 5 * time.Second
	}

	ch := make(chan *biz.DeviceCommandResult, 1)
	s.pendingMu.Lock()
	s.pending[cmd.CommandID] = ch
	s.pendingMu.Unlock()
	defer func() {
		s.pendingMu.Lock()
		delete(s.pending, cmd.CommandID)
		s.pendingMu.Unlock()
	}()

	err := s.SendJSON(ctx, map[string]any{
		"type":        "device.command",
		"command_id":  cmd.CommandID,
		"capability":  cmd.Capability,
		"action":      cmd.Action,
		"params_json": cmd.ParamsJSON,
		"timeout_ms":  cmd.TimeoutMS,
	})
	if err != nil {
		return nil, err
	}

	timer := time.NewTimer(timeout)
	defer timer.Stop()

	select {
	case res := <-ch:
		if res == nil {
			return nil, fmt.Errorf("device command aborted")
		}
		return res, nil
	case <-timer.C:
		return nil, fmt.Errorf("device command %s timed out after %s", cmd.CommandID, timeout)
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}

func (s *wsSink) resolvePending(res *biz.DeviceCommandResult) {
	s.pendingMu.Lock()
	ch, ok := s.pending[res.CommandID]
	s.pendingMu.Unlock()
	if ok {
		select {
		case ch <- res:
		default:
		}
	}
}

func (s *wsSink) failPending(_ error) {
	s.pendingMu.Lock()
	defer s.pendingMu.Unlock()
	for id, ch := range s.pending {
		close(ch)
		delete(s.pending, id)
	}
}

func (s *wsSink) Close(reason string) error {
	return s.conn.Close(websocket.StatusNormalClosure, reason)
}

func randomHex(nBytes int) string {
	b := make([]byte, nBytes)
	_, _ = rand.Read(b)
	return hex.EncodeToString(b)
}
