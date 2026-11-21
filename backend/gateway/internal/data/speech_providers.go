package data

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"

	"esp-pika/backend/gateway/internal/biz"
	"esp-pika/backend/gateway/internal/conf"

	"github.com/go-kratos/kratos/v2/log"
)

const (
	defaultOpenAIASRURL = "https://api.openai.com/v1/audio/transcriptions"
	defaultOpenAITTSURL = "https://api.openai.com/v1/audio/speech"
)

func resolveSpeechAPIKey(c *conf.Voice) string {
	if c == nil {
		return ""
	}
	envName := strings.TrimSpace(c.SpeechApiKeyEnv)
	if envName == "" {
		return ""
	}
	return strings.TrimSpace(os.Getenv(envName))
}

// NewASRProvider selects OpenAI-compatible or custom HTTP ASR when configured, otherwise falls back to mock bench ASR.
func NewASRProvider(c *conf.Voice, logger log.Logger) biz.ASRProvider {
	helper := log.NewHelper(logger)
	if c != nil {
		switch strings.ToLower(strings.TrimSpace(c.AsrProvider)) {
		case "openai":
			endpoint := c.AsrEndpoint
			if endpoint == "" {
				endpoint = defaultOpenAIASRURL
			}
			model := c.AsrModel
			if model == "" {
				model = "whisper-1"
			}
			helper.Infof("initialized OpenAI-compatible ASRProvider endpoint=%s model=%s", endpoint, model)
			return &openaiASRProvider{
				endpoint: endpoint,
				model:    model,
				apiKey:   resolveSpeechAPIKey(c),
				client:   &http.Client{Timeout: 30 * time.Second},
			}
		case "http":
			if c.AsrEndpoint != "" {
				helper.Infof("initialized HTTP ASRProvider endpoint=%s", c.AsrEndpoint)
				return &httpASRProvider{
					endpoint: c.AsrEndpoint,
					client:   &http.Client{Timeout: 15 * time.Second},
				}
			}
		}
	}
	helper.Info("using local mock ASRProvider")
	return &mockASRProvider{}
}

// NewTTSProvider selects OpenAI-compatible or custom HTTP TTS when configured, otherwise falls back to mock Opus frame synthesis.
func NewTTSProvider(c *conf.Voice, logger log.Logger) biz.TTSProvider {
	helper := log.NewHelper(logger)
	if c != nil {
		switch strings.ToLower(strings.TrimSpace(c.TtsProvider)) {
		case "openai":
			endpoint := c.TtsEndpoint
			if endpoint == "" {
				endpoint = defaultOpenAITTSURL
			}
			model := c.TtsModel
			if model == "" {
				model = "tts-1"
			}
			defaultVoice := c.DefaultTtsVoice
			if defaultVoice == "" {
				defaultVoice = "alloy"
			}
			helper.Infof("initialized OpenAI-compatible TTSProvider endpoint=%s model=%s", endpoint, model)
			return &openaiTTSProvider{
				endpoint:     endpoint,
				model:        model,
				defaultVoice: defaultVoice,
				apiKey:       resolveSpeechAPIKey(c),
				client:       &http.Client{Timeout: 30 * time.Second},
			}
		case "http":
			if c.TtsEndpoint != "" {
				helper.Infof("initialized HTTP TTSProvider endpoint=%s", c.TtsEndpoint)
				return &httpTTSProvider{
					endpoint:     c.TtsEndpoint,
					defaultVoice: c.DefaultTtsVoice,
					client:       &http.Client{Timeout: 15 * time.Second},
				}
			}
		}
	}
	helper.Info("using local mock TTSProvider")
	return &mockTTSProvider{}
}

// NewVoiceReviewer builds the gray-zone cloud voiceprint verifier talking to voice-server when enabled.
func NewVoiceReviewer(c *conf.Voice, logger log.Logger) biz.VoiceReviewer {
	helper := log.NewHelper(logger)
	url := ""
	if c != nil {
		url = c.VoiceServerUrl
	}
	return &httpVoiceReviewer{
		endpoint: url,
		client:   &http.Client{Timeout: 5 * time.Second},
		log:      helper,
	}
}

type mockASRProvider struct{}

func (m *mockASRProvider) StartStream(_ context.Context, deviceID string) (biz.ASRStream, error) {
	return &mockASRStream{deviceID: deviceID}, nil
}

type mockASRStream struct {
	deviceID string
	mu       sync.Mutex
	bytes    int
	frames   int
}

func (s *mockASRStream) WriteOpusFrame(_ context.Context, frame []byte) error {
	s.mu.Lock()
	s.frames++
	s.bytes += len(frame)
	s.mu.Unlock()
	return nil
}

func (s *mockASRStream) Finish(_ context.Context) (string, error) {
	s.mu.Lock()
	frames := s.frames
	bytesLen := s.bytes
	s.mu.Unlock()
	if frames == 0 {
		return "", nil
	}
	return fmt.Sprintf("[语音输入 device=%s frames=%d bytes=%d]", s.deviceID, frames, bytesLen), nil
}

func (s *mockASRStream) Close() error { return nil }

type openaiASRProvider struct {
	endpoint string
	model    string
	apiKey   string
	client   *http.Client
}

func (p *openaiASRProvider) StartStream(_ context.Context, _ string) (biz.ASRStream, error) {
	return &openaiASRStream{provider: p}, nil
}

type openaiASRStream struct {
	provider *openaiASRProvider
	mu       sync.Mutex
	frames   [][]byte
}

func (s *openaiASRStream) WriteOpusFrame(_ context.Context, frame []byte) error {
	cp := make([]byte, len(frame))
	copy(cp, frame)
	s.mu.Lock()
	s.frames = append(s.frames, cp)
	s.mu.Unlock()
	return nil
}

func (s *openaiASRStream) Finish(ctx context.Context) (string, error) {
	s.mu.Lock()
	frames := s.frames
	s.frames = nil
	s.mu.Unlock()
	if len(frames) == 0 {
		return "", nil
	}

	fileName := "utterance.ogg"
	audioBytes := BuildOggOpus(frames, 16000, 60)
	if len(frames) == 1 && bytes.HasPrefix(frames[0], []byte("RIFF")) {
		fileName = "utterance.wav"
		audioBytes = frames[0]
	}

	var body bytes.Buffer
	mw := multipart.NewWriter(&body)
	_ = mw.WriteField("model", s.provider.model)
	fw, err := mw.CreateFormFile("file", fileName)
	if err != nil {
		return "", err
	}
	if _, err := fw.Write(audioBytes); err != nil {
		return "", err
	}
	if err := mw.Close(); err != nil {
		return "", err
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, s.provider.endpoint, &body)
	if err != nil {
		return "", err
	}
	req.Header.Set("Content-Type", mw.FormDataContentType())
	if s.provider.apiKey != "" {
		req.Header.Set("Authorization", "Bearer "+s.provider.apiKey)
	}

	resp, err := s.provider.client.Do(req)
	if err != nil {
		return "", err
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		errBody, _ := io.ReadAll(io.LimitReader(resp.Body, 1024))
		return "", fmt.Errorf("openai asr status %d: %s", resp.StatusCode, string(errBody))
	}
	var out struct {
		Text string `json:"text"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		return "", err
	}
	return out.Text, nil
}

func (s *openaiASRStream) Close() error { return nil }

type httpASRProvider struct {
	endpoint string
	client   *http.Client
}

func (p *httpASRProvider) StartStream(_ context.Context, deviceID string) (biz.ASRStream, error) {
	return &httpASRStream{
		endpoint: p.endpoint,
		deviceID: deviceID,
		client:   p.client,
	}, nil
}

type httpASRStream struct {
	endpoint string
	deviceID string
	client   *http.Client
	mu       sync.Mutex
	chunks   [][]byte
}

func (s *httpASRStream) WriteOpusFrame(_ context.Context, frame []byte) error {
	cp := make([]byte, len(frame))
	copy(cp, frame)
	s.mu.Lock()
	s.chunks = append(s.chunks, cp)
	s.mu.Unlock()
	return nil
}

func (s *httpASRStream) Finish(ctx context.Context) (string, error) {
	s.mu.Lock()
	chunks := s.chunks
	s.chunks = nil
	s.mu.Unlock()

	var buf bytes.Buffer
	for _, ch := range chunks {
		buf.Write(ch)
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, s.endpoint, &buf)
	if err != nil {
		return "", err
	}
	req.Header.Set("Content-Type", "application/octet-stream")
	req.Header.Set("X-Device-MAC", s.deviceID)
	resp, err := s.client.Do(req)
	if err != nil {
		return "", err
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return "", fmt.Errorf("asr http status %d", resp.StatusCode)
	}
	var out struct {
		Text string `json:"text"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		return "", err
	}
	return out.Text, nil
}

func (s *httpASRStream) Close() error { return nil }

type mockTTSProvider struct{}

func (m *mockTTSProvider) Synthesize(_ context.Context, text, _ string) ([][]byte, error) {
	if text == "" {
		return nil, nil
	}
	return [][]byte{[]byte("OPUS24K:" + text)}, nil
}

type openaiTTSProvider struct {
	endpoint     string
	model        string
	defaultVoice string
	apiKey       string
	client       *http.Client
}

func (p *openaiTTSProvider) Synthesize(ctx context.Context, text, voice string) ([][]byte, error) {
	if text == "" {
		return nil, nil
	}
	if voice == "" {
		voice = p.defaultVoice
	}
	body, _ := json.Marshal(map[string]any{
		"model":           p.model,
		"input":           text,
		"voice":           voice,
		"response_format": "opus",
	})
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, p.endpoint, bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/json")
	if p.apiKey != "" {
		req.Header.Set("Authorization", "Bearer "+p.apiKey)
	}
	resp, err := p.client.Do(req)
	if err != nil {
		return nil, err
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		errBody, _ := io.ReadAll(io.LimitReader(resp.Body, 1024))
		return nil, fmt.Errorf("openai tts status %d: %s", resp.StatusCode, string(errBody))
	}
	raw, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return nil, err
	}
	return ExtractOggOpusPackets(raw), nil
}

type httpTTSProvider struct {
	endpoint     string
	defaultVoice string
	client       *http.Client
}

func (p *httpTTSProvider) Synthesize(ctx context.Context, text, voice string) ([][]byte, error) {
	if text == "" {
		return nil, nil
	}
	if voice == "" {
		voice = p.defaultVoice
	}
	body, _ := json.Marshal(map[string]any{
		"text":        text,
		"voice":       voice,
		"codec":       "opus",
		"sample_rate": 24000,
	})
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, p.endpoint, bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := p.client.Do(req)
	if err != nil {
		return nil, err
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, fmt.Errorf("tts http status %d", resp.StatusCode)
	}
	raw, err := io.ReadAll(io.LimitReader(resp.Body, 4<<20))
	if err != nil {
		return nil, err
	}
	if len(raw) == 0 {
		return nil, nil
	}
	return [][]byte{raw}, nil
}

type httpVoiceReviewer struct {
	endpoint string
	client   *http.Client
	log      *log.Helper
}

func (r *httpVoiceReviewer) Review(ctx context.Context, deviceID string, opusFrames [][]byte) (string, biz.SpeakerRole, float32, error) {
	if r.endpoint == "" || len(opusFrames) == 0 {
		return "", biz.SpeakerRoleGuest, 0, nil
	}
	var buf bytes.Buffer
	for _, fr := range opusFrames {
		buf.Write(fr)
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, r.endpoint+"/api/v1/speaker/identify", &buf)
	if err != nil {
		return "", biz.SpeakerRoleGuest, 0, err
	}
	req.Header.Set("X-Device-MAC", deviceID)
	resp, err := r.client.Do(req)
	if err != nil {
		r.log.WithContext(ctx).Debugf("voice-server unreachable (%v); keeping edge decision", err)
		return "", biz.SpeakerRoleGuest, 0, err
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return "", biz.SpeakerRoleGuest, 0, fmt.Errorf("voice-server status %d", resp.StatusCode)
	}
	var out struct {
		SpeakerID  string          `json:"speaker_id"`
		Role       biz.SpeakerRole `json:"role"`
		Confidence float32         `json:"confidence"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		return "", biz.SpeakerRoleGuest, 0, err
	}
	return out.SpeakerID, out.Role, out.Confidence, nil
}
