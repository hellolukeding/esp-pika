package data

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"net/url"
	"os"
	"strings"
	"sync"
	"time"

	"esp-pika/backend/gateway/internal/biz"
	"esp-pika/backend/gateway/internal/conf"

	"github.com/coder/websocket"
	"github.com/go-kratos/kratos/v2/log"
)

const (
	defaultOpenAIASRURL     = "https://api.openai.com/v1/audio/transcriptions"
	defaultOpenAITTSURL     = "https://api.openai.com/v1/audio/speech"
	defaultDeepSeekASRURL   = "https://api.deepseek.com/v1/tools/asr"
	defaultDeepSeekTTSWSURL = "wss://api.deepseek.com/v1/tools/tts"
)

func resolveSpeechAPIKey(c *conf.Voice) string {
	return resolveSpeechAPIKeyWithFallback(c, "")
}

func resolveSpeechAPIKeyWithFallback(c *conf.Voice, fallbackEnv string) string {
	if c != nil {
		envName := strings.TrimSpace(c.SpeechApiKeyEnv)
		if envName != "" {
			return strings.TrimSpace(os.Getenv(envName))
		}
	}
	if fallbackEnv != "" {
		return strings.TrimSpace(os.Getenv(fallbackEnv))
	}
	return ""
}

func normalizeDeepSeekVoice(voice, defaultVoice string) string {
	return biz.NormalizeDeepSeekVoice(voice, defaultVoice)
}

func resolveDynamicDeepSeekKey(ctx context.Context, repo biz.SettingsRepo, staticFallback string) string {
	if repo != nil {
		if stored, err := repo.Get(ctx, biz.SettingKeyDeepSeekAPIKey); err == nil {
			if trimmed := strings.TrimSpace(stored); trimmed != "" {
				return trimmed
			}
		}
	}
	if strings.TrimSpace(staticFallback) != "" {
		return strings.TrimSpace(staticFallback)
	}
	return strings.TrimSpace(os.Getenv("DEEPSEEK_API_KEY"))
}

// NewASRProvider selects EZ Assistant Desktop speech relay first when ready, falling back to local configured/mock ASR.
func NewASRProvider(c *conf.Voice, settingsRepo biz.SettingsRepo, ezBridge biz.EZDeviceBridge, logger log.Logger) biz.ASRProvider {
	helper := log.NewHelper(logger)
	local := buildLocalASRProvider(c, settingsRepo, helper)
	if ezBridge == nil {
		return local
	}
	return &routingASRProvider{
		local:    local,
		ezBridge: ezBridge,
		log:      helper,
	}
}

func buildLocalASRProvider(c *conf.Voice, settingsRepo biz.SettingsRepo, helper *log.Helper) biz.ASRProvider {
	asrHTTPClient := &http.Client{
		Timeout: 30 * time.Second,
		Transport: &http.Transport{
			ForceAttemptHTTP2:   true,
			MaxIdleConns:        16,
			MaxIdleConnsPerHost: 4,
			IdleConnTimeout:     90 * time.Second,
		},
	}
	if c != nil {
		switch strings.ToLower(strings.TrimSpace(c.AsrProvider)) {
		case "deepseek":
			endpoint := strings.TrimSpace(c.AsrEndpoint)
			if endpoint == "" {
				endpoint = defaultDeepSeekASRURL
			}
			helper.Infof("initialized DeepSeek ASRProvider endpoint=%s", endpoint)
			return &openaiASRProvider{
				endpoint:     endpoint,
				model:        "",
				apiKey:       resolveSpeechAPIKeyWithFallback(c, "DEEPSEEK_API_KEY"),
				settingsRepo: settingsRepo,
				isDeepSeek:   true,
				client:       asrHTTPClient,
			}
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
				client:   asrHTTPClient,
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

// NewTTSProvider selects EZ Assistant Desktop speech relay first when ready, falling back to local configured/mock TTS.
func NewTTSProvider(c *conf.Voice, settingsRepo biz.SettingsRepo, ezBridge biz.EZDeviceBridge, logger log.Logger) biz.TTSProvider {
	helper := log.NewHelper(logger)
	local := buildLocalTTSProvider(c, settingsRepo, helper)
	if ezBridge == nil {
		return local
	}
	return &routingTTSProvider{
		local:    local,
		ezBridge: ezBridge,
		log:      helper,
	}
}

func buildLocalTTSProvider(c *conf.Voice, settingsRepo biz.SettingsRepo, helper *log.Helper) biz.TTSProvider {
	if c != nil {
		switch strings.ToLower(strings.TrimSpace(c.TtsProvider)) {
		case "deepseek":
			endpoint := strings.TrimSpace(c.TtsEndpoint)
			if endpoint == "" {
				endpoint = defaultDeepSeekTTSWSURL
			}
			defaultVoice := normalizeDeepSeekVoice(c.DefaultTtsVoice, "stella")
			helper.Infof("initialized DeepSeek WS TTSProvider endpoint=%s voice=%s", endpoint, defaultVoice)
			return &deepseekTTSProvider{
				endpoint:     endpoint,
				defaultVoice: defaultVoice,
				apiKey:       resolveSpeechAPIKeyWithFallback(c, "DEEPSEEK_API_KEY"),
				settingsRepo: settingsRepo,
			}
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

type routingASRProvider struct {
	local    biz.ASRProvider
	ezBridge biz.EZDeviceBridge
	log      *log.Helper
}

func (p *routingASRProvider) StartStream(_ context.Context, deviceID string) (biz.ASRStream, error) {
	return &routingASRStream{
		deviceID: deviceID,
		local:    p.local,
		ezBridge: p.ezBridge,
		log:      p.log,
	}, nil
}

type routingASRStream struct {
	deviceID string
	local    biz.ASRProvider
	ezBridge biz.EZDeviceBridge
	log      *log.Helper

	mu     sync.Mutex
	frames [][]byte
}

func (s *routingASRStream) WriteOpusFrame(_ context.Context, frame []byte) error {
	cp := make([]byte, len(frame))
	copy(cp, frame)
	s.mu.Lock()
	s.frames = append(s.frames, cp)
	s.mu.Unlock()
	return nil
}

func (s *routingASRStream) Finish(ctx context.Context) (string, error) {
	s.mu.Lock()
	frames := s.frames
	s.frames = nil
	s.mu.Unlock()
	if len(frames) == 0 {
		return "", nil
	}

	if s.ezBridge != nil && s.ezBridge.CanUseASR() {
		var (
			pcm []byte
			err error
		)
		if len(frames) == 1 && bytes.HasPrefix(frames[0], []byte("RIFF")) {
			pcm, err = WAVToPCM16kMono(frames[0])
		} else {
			ogg := BuildOggOpus(frames, 16000, 60)
			pcm, err = OggOpusToPCM16kMono(ctx, ogg)
		}
		if err == nil && len(pcm) > 0 {
			text, rErr := s.ezBridge.RecognizeSpeech(ctx, pcm)
			if rErr == nil {
				return text, nil
			}
			if ctx.Err() != nil {
				return "", ctx.Err()
			}
			s.log.Warnf("EZ Assistant speech relay ASR failed, falling back to local provider: %v", rErr)
		} else if err != nil {
			if ctx.Err() != nil {
				return "", ctx.Err()
			}
			s.log.Warnf("failed to prepare PCM16k for EZ speech relay ASR, falling back to local provider: %v", err)
		}
	}

	localStream, err := s.local.StartStream(ctx, s.deviceID)
	if err != nil {
		return "", err
	}
	defer func() { _ = localStream.Close() }()
	for _, fr := range frames {
		if err := localStream.WriteOpusFrame(ctx, fr); err != nil {
			return "", err
		}
	}
	return localStream.Finish(ctx)
}

func (s *routingASRStream) Close() error {
	s.mu.Lock()
	s.frames = nil
	s.mu.Unlock()
	return nil
}

type routingTTSProvider struct {
	local    biz.TTSProvider
	ezBridge biz.EZDeviceBridge
	log      *log.Helper
}

func (p *routingTTSProvider) Synthesize(ctx context.Context, text, voice string) ([][]byte, error) {
	trimmed := strings.TrimSpace(text)
	if trimmed != "" && p.ezBridge != nil && p.ezBridge.CanUseTTS() {
		segments := SplitTextForRelayTTS(trimmed, relayMaxTTSSegmentRunes)
		if len(segments) > 0 {
			var combinedPCM bytes.Buffer
			relayOK := true
			for _, seg := range segments {
				pcm, err := p.ezBridge.SynthesizeSpeech(ctx, seg)
				if err != nil {
					if ctx.Err() != nil {
						return nil, ctx.Err()
					}
					p.log.Warnf("EZ Assistant speech relay TTS failed, falling back to local provider: %v", err)
					relayOK = false
					break
				}
				_, _ = combinedPCM.Write(pcm)
			}
			if relayOK && combinedPCM.Len() > 0 {
				ogg, err := PCM16kMonoToOggOpus(ctx, combinedPCM.Bytes())
				if err == nil {
					packets := SplitOpusPackets(ExtractOggOpusPackets(ogg))
					if len(packets) > 0 {
						return packets, nil
					}
				} else {
					if ctx.Err() != nil {
						return nil, ctx.Err()
					}
					p.log.Warnf("failed to encode EZ relay TTS PCM16k to OggOpus, falling back to local provider: %v", err)
				}
			}
		}
	}
	return p.local.Synthesize(ctx, text, voice)
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
	endpoint     string
	model        string
	apiKey       string
	settingsRepo biz.SettingsRepo
	isDeepSeek   bool
	client       *http.Client
}

func (p *openaiASRProvider) resolveAPIKey(ctx context.Context) string {
	if p.isDeepSeek {
		return resolveDynamicDeepSeekKey(ctx, p.settingsRepo, p.apiKey)
	}
	return p.apiKey
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
	} else if s.provider.isDeepSeek {
		_ = os.WriteFile("/tmp/esp_pika_last_asr.ogg", audioBytes, 0o600)
	}

	var body bytes.Buffer
	mw := multipart.NewWriter(&body)
	if s.provider.model != "" {
		_ = mw.WriteField("model", s.provider.model)
	}
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
	if apiKey := s.provider.resolveAPIKey(ctx); apiKey != "" {
		req.Header.Set("Authorization", "Bearer "+apiKey)
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
	return SplitOpusPackets(ExtractOggOpusPackets(raw)), nil
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

type deepseekTTSProvider struct {
	endpoint     string
	defaultVoice string
	apiKey       string
	settingsRepo biz.SettingsRepo
}

func (p *deepseekTTSProvider) Synthesize(ctx context.Context, text, voice string) ([][]byte, error) {
	if strings.TrimSpace(text) == "" {
		return nil, nil
	}
	u, err := url.Parse(p.endpoint)
	if err != nil {
		return nil, fmt.Errorf("invalid deepseek tts endpoint %q: %w", p.endpoint, err)
	}
	switch strings.ToLower(u.Scheme) {
	case "https":
		u.Scheme = "wss"
	case "http":
		u.Scheme = "ws"
	}
	effectiveVoice := strings.TrimSpace(voice)
	if effectiveVoice == "" && p.settingsRepo != nil {
		if savedVoice, err := p.settingsRepo.Get(ctx, biz.SettingKeyDeepSeekVoice); err == nil {
			effectiveVoice = strings.TrimSpace(savedVoice)
		}
	}
	q := u.Query()
	q.Set("voice_id", normalizeDeepSeekVoice(effectiveVoice, p.defaultVoice))
	q.Set("format", "opus")
	q.Set("filter_markdown", "true")
	u.RawQuery = q.Encode()

	var opts *websocket.DialOptions
	if apiKey := resolveDynamicDeepSeekKey(ctx, p.settingsRepo, p.apiKey); apiKey != "" {
		opts = &websocket.DialOptions{
			HTTPHeader: http.Header{
				"Authorization": []string{"Bearer " + apiKey},
			},
		}
	}
	wsURL := u.String()
	frames, err := p.synthesizeOnce(ctx, wsURL, opts, text)
	if err != nil && ctx.Err() == nil {
		timer := time.NewTimer(250 * time.Millisecond)
		select {
		case <-ctx.Done():
			timer.Stop()
			return nil, ctx.Err()
		case <-timer.C:
		}
		frames, err = p.synthesizeOnce(ctx, wsURL, opts, text)
	}
	return frames, err
}

func (p *deepseekTTSProvider) synthesizeOnce(ctx context.Context, wsURL string, opts *websocket.DialOptions, text string) ([][]byte, error) {
	dialCtx, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()

	conn, _, err := websocket.Dial(dialCtx, wsURL, opts)
	if err != nil {
		return nil, fmt.Errorf("dial deepseek tts ws: %w", err)
	}
	defer func() { _ = conn.CloseNow() }()
	conn.SetReadLimit(4 << 20)

	textReq, _ := json.Marshal(map[string]string{
		"event": "text",
		"text":  text,
	})
	if err := conn.Write(dialCtx, websocket.MessageText, textReq); err != nil {
		return nil, fmt.Errorf("send deepseek tts text: %w", err)
	}
	if err := conn.Write(dialCtx, websocket.MessageText, []byte(`{"event":"finish_text"}`)); err != nil {
		return nil, fmt.Errorf("send deepseek tts finish_text: %w", err)
	}

	var frames [][]byte
	for {
		mt, msg, err := conn.Read(dialCtx)
		if err != nil {
			if len(frames) > 0 && ctx.Err() == nil {
				break
			}
			return nil, fmt.Errorf("read deepseek tts ws: %w", err)
		}
		switch mt {
		case websocket.MessageBinary:
			if len(msg) > 0 {
				if bytes.HasPrefix(msg, []byte("OggS")) {
					frames = append(frames, ExtractOggOpusPackets(msg)...)
				} else {
					cp := make([]byte, len(msg))
					copy(cp, msg)
					frames = append(frames, cp)
				}
			}
		case websocket.MessageText:
			var evt struct {
				Event   string `json:"event"`
				Code    string `json:"code"`
				Message string `json:"message"`
			}
			if err := json.Unmarshal(msg, &evt); err != nil {
				continue
			}
			if evt.Event == "finish" {
				if evt.Code != "" && evt.Code != "success" {
					return nil, fmt.Errorf("deepseek tts finish error (%s): %s", evt.Code, evt.Message)
				}
				return SplitOpusPackets(frames), nil
			}
		}
	}
	return SplitOpusPackets(frames), nil
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
