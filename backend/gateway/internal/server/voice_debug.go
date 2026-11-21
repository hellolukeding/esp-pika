package server

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"

	"esp-pika/backend/gateway/internal/biz"
	"esp-pika/backend/gateway/internal/conf"

	"github.com/go-kratos/kratos/v2/log"
)

// VoiceDebugHandler exposes same-origin Web Console endpoints bridging browser
// microphone recordings (16kHz WAV/PCM) to voice-server Qdrant and gateway ASR/TTS.
type VoiceDebugHandler struct {
	voiceServerURL string
	speakers       *biz.SpeakerUsecase
	speech         *biz.SpeechPipeline
	client         *http.Client
	log            *log.Helper
}

// NewVoiceDebugHandler constructs a VoiceDebugHandler.
func NewVoiceDebugHandler(
	cVoice *conf.Voice,
	speakers *biz.SpeakerUsecase,
	speech *biz.SpeechPipeline,
	logger log.Logger,
) *VoiceDebugHandler {
	vsURL := ""
	if cVoice != nil {
		vsURL = strings.TrimRight(cVoice.VoiceServerUrl, "/")
	}
	return &VoiceDebugHandler{
		voiceServerURL: vsURL,
		speakers:       speakers,
		speech:         speech,
		client:         &http.Client{Timeout: 15 * time.Second},
		log:            log.NewHelper(logger),
	}
}

// HandleEnroll handles `POST /api/v1/voice/enroll` from the Web Console:
// enrolls the 192-D embedding into voice-server (Qdrant) and syncs SpeakerGroup metadata.
func (h *VoiceDebugHandler) HandleEnroll(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		SpeakerID   string          `json:"speaker_id"`
		DisplayName string          `json:"display_name"`
		UID         string          `json:"uid"`
		AgentID     string          `json:"agent_id"`
		GroupID     string          `json:"group_id"`
		Role        biz.SpeakerRole `json:"role"`
		TTSVoice    string          `json:"tts_voice"`
		Prompt      string          `json:"prompt"`
		AudioBase64 string          `json:"audio_base64"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 8<<20)).Decode(&req); err != nil || strings.TrimSpace(req.SpeakerID) == "" {
		http.Error(w, "invalid enroll payload", http.StatusBadRequest)
		return
	}
	if req.Role == 0 {
		req.Role = biz.SpeakerRoleMember
	}
	if req.GroupID == "" {
		req.GroupID = "home-default"
	}

	var vsResult map[string]any
	if h.voiceServerURL != "" && req.AudioBase64 != "" {
		fwdBody, _ := json.Marshal(map[string]any{
			"speaker_id":   req.SpeakerID,
			"uid":          req.UID,
			"agent_id":     req.AgentID,
			"group_id":     req.GroupID,
			"role":         int32(req.Role),
			"audio_base64": req.AudioBase64,
		})
		resp, err := h.client.Post(h.voiceServerURL+"/api/v1/speaker/enroll", "application/json", bytes.NewReader(fwdBody))
		if err != nil {
			http.Error(w, fmt.Sprintf("voice-server unreachable (%s): %v", h.voiceServerURL, err), http.StatusBadGateway)
			return
		}
		rawResp, _ := io.ReadAll(io.LimitReader(resp.Body, 64*1024))
		_ = resp.Body.Close()
		if resp.StatusCode >= 300 {
			http.Error(w, string(rawResp), resp.StatusCode)
			return
		}
		_ = json.Unmarshal(rawResp, &vsResult)
	}

	if h.speakers != nil {
		_, err := h.speakers.Upsert(r.Context(), &biz.SpeakerGroup{
			SpeakerID:   req.SpeakerID,
			UID:         req.UID,
			AgentID:     req.AgentID,
			GroupID:     req.GroupID,
			DisplayName: req.DisplayName,
			Role:        req.Role,
			Prompt:      req.Prompt,
			TTSVoice:    req.TTSVoice,
		})
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
	}

	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{
		"ok":           true,
		"speaker_id":   req.SpeakerID,
		"voice_server": vsResult,
	})
}

// HandleIdentify handles `POST /api/v1/voice/identify` from the Web Console:
// decodes base64 WAV/PCM and identifies the speaker against voice-server (Qdrant).
func (h *VoiceDebugHandler) HandleIdentify(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		UID         string `json:"uid"`
		AgentID     string `json:"agent_id"`
		AudioBase64 string `json:"audio_base64"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 8<<20)).Decode(&req); err != nil {
		http.Error(w, "invalid identify payload", http.StatusBadRequest)
		return
	}
	audioBytes, err := base64.StdEncoding.DecodeString(req.AudioBase64)
	if err != nil || len(audioBytes) == 0 {
		http.Error(w, "invalid audio_base64", http.StatusBadRequest)
		return
	}
	if h.voiceServerURL == "" {
		http.Error(w, "voice_server_url is not configured", http.StatusServiceUnavailable)
		return
	}

	q := url.Values{}
	if req.UID != "" {
		q.Set("uid", req.UID)
	}
	if req.AgentID != "" {
		q.Set("agent_id", req.AgentID)
	}
	target := h.voiceServerURL + "/api/v1/speaker/identify"
	if encoded := q.Encode(); encoded != "" {
		target += "?" + encoded
	}
	resp, err := h.client.Post(target, "application/octet-stream", bytes.NewReader(audioBytes))
	if err != nil {
		http.Error(w, fmt.Sprintf("voice-server unreachable: %v", err), http.StatusBadGateway)
		return
	}
	defer func() { _ = resp.Body.Close() }()
	rawResp, _ := io.ReadAll(io.LimitReader(resp.Body, 64*1024))
	if resp.StatusCode >= 300 {
		http.Error(w, string(rawResp), resp.StatusCode)
		return
	}

	w.Header().Set("Content-Type", "application/json")
	_, _ = w.Write(rawResp)
}

// HandleTranscribe handles `POST /api/v1/voice/transcribe` for testing browser recordings via gateway ASRProvider.
func (h *VoiceDebugHandler) HandleTranscribe(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		DeviceID    string `json:"device_id"`
		AudioBase64 string `json:"audio_base64"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 8<<20)).Decode(&req); err != nil {
		http.Error(w, "invalid transcribe payload", http.StatusBadRequest)
		return
	}
	audioBytes, err := base64.StdEncoding.DecodeString(req.AudioBase64)
	if err != nil || len(audioBytes) == 0 {
		http.Error(w, "invalid audio_base64", http.StatusBadRequest)
		return
	}
	if h.speech == nil || h.speech.ASR == nil {
		http.Error(w, "ASRProvider unavailable", http.StatusServiceUnavailable)
		return
	}
	devID := req.DeviceID
	if devID == "" {
		devID = "web-sim"
	}
	st, err := h.speech.ASR.StartStream(r.Context(), devID)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	defer func() { _ = st.Close() }()
	if err := st.WriteOpusFrame(r.Context(), audioBytes); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	text, err := st.Finish(r.Context())
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadGateway)
		return
	}

	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{
		"ok":   true,
		"text": text,
	})
}
