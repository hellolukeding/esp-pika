package server

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"time"

	"esp-pika/backend/gateway/internal/biz"

	"github.com/go-kratos/kratos/v2/log"
)

// EZDeviceServer manages the EZDeviceBridge Kratos transport lifecycle and exposes Web Console REST APIs.
type EZDeviceServer struct {
	bridge  biz.EZDeviceBridge
	devices *biz.DeviceUsecase
	speech  *biz.SpeechPipeline
	log     *log.Helper
}

// NewEZDeviceServer constructs an EZDeviceServer.
func NewEZDeviceServer(bridge biz.EZDeviceBridge, devices *biz.DeviceUsecase, speech *biz.SpeechPipeline, logger log.Logger) *EZDeviceServer {
	return &EZDeviceServer{
		bridge:  bridge,
		devices: devices,
		speech:  speech,
		log:     log.NewHelper(logger),
	}
}

// Start implements Kratos transport.Server by starting the EZDeviceBridge supervisor.
func (s *EZDeviceServer) Start(ctx context.Context) error {
	if s.bridge == nil {
		return nil
	}
	return s.bridge.Start(ctx)
}

// Stop implements Kratos transport.Server by gracefully stopping EZDeviceBridge.
func (s *EZDeviceServer) Stop(ctx context.Context) error {
	if s.bridge == nil {
		return nil
	}
	return s.bridge.Stop(ctx)
}

// HandleStatus serves `GET /api/v1/ezdevice/status`.
func (s *EZDeviceServer) HandleStatus(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	st := biz.EZBridgeStatus{
		Mode:          biz.EZModeAuto,
		ActiveBackend: biz.EZModeGRPC,
		State:         "unpaired",
	}
	if s.bridge != nil {
		st = s.bridge.Status(r.Context())
	}
	scheme := "http"
	if r.TLS != nil {
		scheme = "https"
	}
	host := r.Host
	if host == "" {
		host = "127.0.0.1:8088"
	}
	writeJSONResponse(w, http.StatusOK, map[string]any{
		"status":       st,
		"mcp_endpoint": scheme + "://" + host + "/mcp",
	})
}

// HandleDiscover serves `POST /api/v1/ezdevice/discover`.
func (s *EZDeviceServer) HandleDiscover(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if s.bridge == nil {
		writeJSONResponse(w, http.StatusOK, map[string]any{"hosts": []biz.EZHostCandidate{}})
		return
	}
	hosts, err := s.bridge.DiscoverHosts(r.Context(), 2500*time.Millisecond)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadGateway)
		return
	}
	if hosts == nil {
		hosts = []biz.EZHostCandidate{}
	}
	writeJSONResponse(w, http.StatusOK, map[string]any{"hosts": hosts})
}

// HandleMode serves `PUT /api/v1/ezdevice/mode`.
func (s *EZDeviceServer) HandleMode(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPut && r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		Mode string `json:"mode"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&req); err != nil {
		http.Error(w, "invalid mode JSON", http.StatusBadRequest)
		return
	}
	st, err := s.bridge.SetMode(r.Context(), req.Mode)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	writeJSONResponse(w, http.StatusOK, map[string]any{"status": st})
}

// HandlePair serves `POST /api/v1/ezdevice/pair`.
func (s *EZDeviceServer) HandlePair(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var req biz.EZPairRequest
	if err := json.NewDecoder(io.LimitReader(r.Body, 16384)).Decode(&req); err != nil {
		http.Error(w, "invalid pairing JSON", http.StatusBadRequest)
		return
	}
	req.Address = strings.TrimSpace(req.Address)
	req.InstallationID = strings.TrimSpace(req.InstallationID)
	req.CertificateFingerprint = strings.TrimSpace(req.CertificateFingerprint)
	st, err := s.bridge.StartPairing(r.Context(), req)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	writeJSONResponse(w, http.StatusOK, map[string]any{"status": st})
}

// HandleReset serves `POST /api/v1/ezdevice/reset`.
func (s *EZDeviceServer) HandleReset(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	st, err := s.bridge.ResetPairing(r.Context())
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSONResponse(w, http.StatusOK, map[string]any{"status": st})
}

// HandleTestTurn serves `POST /api/v1/ezdevice/test`, sending a text turn directly to the paired EZ Assistant
// Controller Session and optionally speaking its reply on an online ESP-Mosaico device.
func (s *EZDeviceServer) HandleTestTurn(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if s.bridge == nil || !s.bridge.IsOnline() {
		http.Error(w, "ez-assistant device bridge is not online", http.StatusConflict)
		return
	}
	var req struct {
		Text     string `json:"text"`
		DeviceID string `json:"device_id"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 16384)).Decode(&req); err != nil && err != io.EOF {
		http.Error(w, "invalid test JSON", http.StatusBadRequest)
		return
	}
	text := strings.TrimSpace(req.Text)
	if text == "" {
		text = "你好，我是小元语音终端，请用一句话简短回复确认连接成功。"
	}

	var replyBuf strings.Builder
	turnID := "test-" + time.Now().Format("150405.000")
	err := s.bridge.RunTurn(r.Context(), &biz.AgentTurnRequest{
		SessionID: "ez-test",
		TurnID:    turnID,
		Text:      text,
	}, biz.AgentStreamCallbacks{
		OnTextDelta: func(chunk, _ string) {
			replyBuf.WriteString(chunk)
		},
	})
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadGateway)
		return
	}

	replyText := strings.TrimSpace(replyBuf.String())
	targetDev := strings.TrimSpace(req.DeviceID)
	if targetDev == "" && s.devices != nil {
		if list, lErr := s.devices.ListDevices(r.Context(), "", true); lErr == nil && len(list) > 0 && list[0].Hello != nil {
			targetDev = list[0].Hello.DeviceID
		}
	}

	ttsFramesSent := 0
	var ttsErrMsg string
	if replyText != "" && targetDev != "" && s.devices != nil && s.speech != nil && s.speech.TTS != nil {
		spoken := biz.SpokenFilter(replyText)
		if spoken != "" {
			frames, tErr := s.speech.TTS.Synthesize(r.Context(), spoken, "")
			if tErr != nil {
				ttsErrMsg = tErr.Error()
			} else if len(frames) > 0 {
				ttsFramesSent = len(frames)
				pacer := &opusBurstPacer{}
				if sendErr := pacer.sendAll(r.Context(), frames, func(fr []byte) error {
					return s.devices.SendAudioToDevice(r.Context(), targetDev, fr)
				}); sendErr != nil {
					ttsErrMsg = sendErr.Error()
				}
			}
		}
	}

	writeJSONResponse(w, http.StatusOK, map[string]any{
		"ok":              true,
		"sent_text":       text,
		"reply_text":      replyText,
		"target_device":   targetDev,
		"tts_frames_sent": ttsFramesSent,
		"tts_error":       ttsErrMsg,
	})
}

func writeJSONResponse(w http.ResponseWriter, statusCode int, body any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(statusCode)
	_ = json.NewEncoder(w).Encode(body)
}
