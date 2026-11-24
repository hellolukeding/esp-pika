package data

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
	"unicode/utf8"

	"esp-pika/backend/gateway/internal/biz"
	"esp-pika/backend/gateway/internal/conf"

	"github.com/coder/websocket"
	"github.com/go-kratos/kratos/v2/log"
)

func TestOggOpusRoundTrip(t *testing.T) {
	p1 := []byte{0x01, 0x02, 0x03, 0x04}
	p2 := bytes.Repeat([]byte{0xAB}, 300) // spans >255 Ogg lacing segments
	ogg := BuildOggOpus([][]byte{p1, p2}, 16000, 60)

	if !bytes.HasPrefix(ogg, []byte("OggS")) {
		t.Fatalf("expected OggS magic header")
	}
	pkts := ExtractOggOpusPackets(ogg)
	if len(pkts) != 2 {
		t.Fatalf("expected 2 extracted audio packets, got %d", len(pkts))
	}
	if !bytes.Equal(pkts[0], p1) || !bytes.Equal(pkts[1], p2) {
		t.Fatalf("round-trip packet mismatch")
	}
}

func TestOpenAIASRAndTTSProviders(t *testing.T) {
	t.Setenv("TEST_SPEECH_KEY", "sk-test-123")

	asrTS := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer sk-test-123" {
			http.Error(w, "missing bearer token", http.StatusUnauthorized)
			return
		}
		if err := r.ParseMultipartForm(4 << 20); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		if r.FormValue("model") != "whisper-1" {
			http.Error(w, "unexpected model", http.StatusBadRequest)
			return
		}
		f, _, err := r.FormFile("file")
		if err != nil {
			http.Error(w, "missing file", http.StatusBadRequest)
			return
		}
		defer func() { _ = f.Close() }()
		raw, _ := io.ReadAll(f)
		if len(ExtractOggOpusPackets(raw)) != 1 {
			http.Error(w, "invalid ogg opus upload", http.StatusBadRequest)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]string{"text": "帮我打开客厅的灯"})
	}))
	defer asrTS.Close()

	ttsFrame1 := []byte{0x11, 0x22, 0x33}
	ttsFrame2 := []byte{0x44, 0x55, 0x66}
	ttsTS := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer sk-test-123" {
			http.Error(w, "missing bearer token", http.StatusUnauthorized)
			return
		}
		var req map[string]string
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		if req["model"] != "tts-1" || req["voice"] != "nova" || req["response_format"] != "opus" {
			http.Error(w, "unexpected tts payload", http.StatusBadRequest)
			return
		}
		w.Header().Set("Content-Type", "audio/ogg")
		_, _ = w.Write(BuildOggOpus([][]byte{ttsFrame1, ttsFrame2}, 24000, 60))
	}))
	defer ttsTS.Close()

	cfg := &conf.Voice{
		AsrProvider:     "openai",
		AsrEndpoint:     asrTS.URL,
		AsrModel:        "whisper-1",
		TtsProvider:     "openai",
		TtsEndpoint:     ttsTS.URL,
		TtsModel:        "tts-1",
		DefaultTtsVoice: "nova",
		SpeechApiKeyEnv: "TEST_SPEECH_KEY",
	}

	ctx := context.Background()
	asr := NewASRProvider(cfg, nil, nil, log.DefaultLogger)
	st, err := asr.StartStream(ctx, "30:ed:a0:f3:dc:70")
	if err != nil {
		t.Fatalf("start stream: %v", err)
	}
	_ = st.WriteOpusFrame(ctx, []byte{0xDE, 0xAD, 0xBE, 0xEF})
	text, err := st.Finish(ctx)
	if err != nil || text != "帮我打开客厅的灯" {
		t.Fatalf("unexpected asr result: text=%q err=%v", text, err)
	}

	tts := NewTTSProvider(cfg, nil, nil, log.DefaultLogger)
	outFrames, err := tts.Synthesize(ctx, "好的，已开启。", "")
	if err != nil || len(outFrames) != 2 || !bytes.Equal(outFrames[0], ttsFrame1) || !bytes.Equal(outFrames[1], ttsFrame2) {
		t.Fatalf("unexpected tts frames: %v err=%v", outFrames, err)
	}
}

func TestDeepSeekASRAndTTSProviders(t *testing.T) {
	t.Setenv("DEEPSEEK_API_KEY", "sk-ds-test")

	asrTS := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer sk-ds-test" {
			http.Error(w, "missing bearer token", http.StatusUnauthorized)
			return
		}
		if err := r.ParseMultipartForm(4 << 20); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		if r.FormValue("model") != "" {
			http.Error(w, "unexpected model field for deepseek asr", http.StatusBadRequest)
			return
		}
		f, _, err := r.FormFile("file")
		if err != nil {
			http.Error(w, "missing file", http.StatusBadRequest)
			return
		}
		defer func() { _ = f.Close() }()
		raw, _ := io.ReadAll(f)
		if len(ExtractOggOpusPackets(raw)) != 1 {
			http.Error(w, "invalid ogg opus upload", http.StatusBadRequest)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]any{
			"request_id":  "req_1",
			"text":        "今天天气怎么样",
			"duration_ms": 1200,
		})
	}))
	defer asrTS.Close()

	dsFrame1 := []byte{0x21, 0x22, 0x23, 0x24}
	dsFrame2 := []byte{0x31, 0x32, 0x33}
	ttsTS := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer sk-ds-test" {
			http.Error(w, "missing bearer token", http.StatusUnauthorized)
			return
		}
		q := r.URL.Query()
		if q.Get("format") != "opus" || q.Get("filter_markdown") != "true" || q.Get("voice_id") != "tide" {
			http.Error(w, "unexpected deepseek tts query: "+r.URL.RawQuery, http.StatusBadRequest)
			return
		}
		conn, err := websocket.Accept(w, r, nil)
		if err != nil {
			return
		}
		defer func() { _ = conn.CloseNow() }()

		_, msg1, err := conn.Read(r.Context())
		if err != nil {
			t.Errorf("read ws text frame: %v", err)
			return
		}
		var evt1 map[string]string
		if json.Unmarshal(msg1, &evt1) != nil || evt1["event"] != "text" || evt1["text"] != "当前温度24度" {
			t.Errorf("unexpected text event: %s", string(msg1))
			return
		}
		_, msg2, err := conn.Read(r.Context())
		if err != nil {
			t.Errorf("read ws finish_text frame: %v", err)
			return
		}
		var evt2 map[string]string
		if json.Unmarshal(msg2, &evt2) != nil || evt2["event"] != "finish_text" {
			t.Errorf("unexpected finish_text event: %s", string(msg2))
			return
		}

		_ = conn.Write(r.Context(), websocket.MessageBinary, dsFrame1)
		_ = conn.Write(r.Context(), websocket.MessageBinary, dsFrame2)
		_ = conn.Write(r.Context(), websocket.MessageText, []byte(`{"event":"finish","code":"success"}`))
	}))
	defer ttsTS.Close()

	// Leave SpeechApiKeyEnv unset to verify default DEEPSEEK_API_KEY fallback, and verify voice normalization.
	cfg := &conf.Voice{
		AsrProvider: "deepseek",
		AsrEndpoint: asrTS.URL,
		TtsProvider: "deepseek",
		TtsEndpoint: ttsTS.URL,
	}

	ctx := context.Background()
	asr := NewASRProvider(cfg, nil, nil, log.DefaultLogger)
	st, err := asr.StartStream(ctx, "30:ed:a0:f4:5b:f0")
	if err != nil {
		t.Fatalf("start stream: %v", err)
	}
	_ = st.WriteOpusFrame(ctx, []byte{0xCA, 0xFE, 0xBA, 0xBE})
	text, err := st.Finish(ctx)
	if err != nil || text != "今天天气怎么样" {
		t.Fatalf("unexpected deepseek asr result: text=%q err=%v", text, err)
	}

	if got := normalizeDeepSeekVoice("zh_female_calm", "stella"); got != "stella" {
		t.Fatalf("expected zh_female_calm normalized to stella, got %q", got)
	}
	if got := normalizeDeepSeekVoice("zh_male_warm", "stella"); got != "tide" {
		t.Fatalf("expected zh_male_warm normalized to tide, got %q", got)
	}

	tts := NewTTSProvider(cfg, nil, nil, log.DefaultLogger)
	outFrames, err := tts.Synthesize(ctx, "当前温度24度", "zh_male_warm")
	if err != nil || len(outFrames) != 2 || !bytes.Equal(outFrames[0], dsFrame1) || !bytes.Equal(outFrames[1], dsFrame2) {
		t.Fatalf("unexpected deepseek tts frames: %v err=%v", outFrames, err)
	}
}

func TestDeepSeekDynamicSettingsOverrideForASRAndTTS(t *testing.T) {
	t.Setenv("DEEPSEEK_API_KEY", "")

	d, cleanup, err := NewData(&conf.Data{
		Database: &conf.Data_Database{Driver: "sqlite3", Source: ":memory:"},
	}, log.DefaultLogger)
	if err != nil {
		t.Fatalf("NewData: %v", err)
	}
	defer cleanup()

	repo := NewSQLiteSettingsRepo(d)
	ctx := context.Background()

	asrTS := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer sk-console-speech-8888" {
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"text":"动态密钥识别成功"}`))
	}))
	defer asrTS.Close()

	ttsFrame := []byte{0xAA, 0xBB, 0xCC}
	ttsTS := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer sk-console-speech-8888" {
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		if r.URL.Query().Get("voice_id") != "tide" {
			http.Error(w, "unexpected voice_id", http.StatusBadRequest)
			return
		}
		conn, err := websocket.Accept(w, r, nil)
		if err != nil {
			return
		}
		defer func() { _ = conn.CloseNow() }()
		for i := 0; i < 2; i++ {
			if _, _, err := conn.Read(r.Context()); err != nil {
				return
			}
		}
		_ = conn.Write(r.Context(), websocket.MessageBinary, ttsFrame)
		_ = conn.Write(r.Context(), websocket.MessageText, []byte(`{"event":"finish","code":"success"}`))
	}))
	defer ttsTS.Close()

	cfg := &conf.Voice{
		AsrProvider: "deepseek",
		AsrEndpoint: asrTS.URL,
		TtsProvider: "deepseek",
		TtsEndpoint: ttsTS.URL,
	}
	asr := NewASRProvider(cfg, repo, nil, log.DefaultLogger)
	tts := NewTTSProvider(cfg, repo, nil, log.DefaultLogger)

	// Save key and voice after providers were constructed to verify immediate effect without restart.
	if err := repo.Set(ctx, biz.SettingKeyDeepSeekAPIKey, "sk-console-speech-8888"); err != nil {
		t.Fatalf("save api key: %v", err)
	}
	if err := repo.Set(ctx, biz.SettingKeyDeepSeekVoice, "tide"); err != nil {
		t.Fatalf("save voice: %v", err)
	}

	st, err := asr.StartStream(ctx, "30:ed:a0:f4:5b:f0")
	if err != nil {
		t.Fatalf("StartStream: %v", err)
	}
	_ = st.WriteOpusFrame(ctx, []byte{0x01, 0x02})
	gotText, err := st.Finish(ctx)
	if err != nil || gotText != "动态密钥识别成功" {
		t.Fatalf("expected dynamic ASR success, got text=%q err=%v", gotText, err)
	}

	frames, err := tts.Synthesize(ctx, "测试动态语音", "")
	if err != nil || len(frames) != 1 || !bytes.Equal(frames[0], ttsFrame) {
		t.Fatalf("expected dynamic TTS success, got frames=%v err=%v", frames, err)
	}
}

func TestSplitOpusPacketToSingleFrames(t *testing.T) {
	// Realistic 5-frame VBR DeepSeek 100ms packet: TOC=0x6b (config=13, stereo=0, code=3)
	// frame counts byte = 0x85 (vbr=1, pad=0, M=5), with 2-byte length >= 252 for frame 1.
	f0 := bytes.Repeat([]byte{0x11}, 40)
	f1 := bytes.Repeat([]byte{0x22}, 260) // 252 + 4*2 = 260 -> encoded as [252, 2]
	f2 := bytes.Repeat([]byte{0x33}, 50)
	f3 := bytes.Repeat([]byte{0x44}, 60)
	f4 := bytes.Repeat([]byte{0x55}, 45)

	var pkt bytes.Buffer
	pkt.WriteByte(0x6b)
	pkt.WriteByte(0x85)
	pkt.WriteByte(byte(len(f0)))
	pkt.WriteByte(252)
	pkt.WriteByte(2)
	pkt.WriteByte(byte(len(f2)))
	pkt.WriteByte(byte(len(f3)))
	pkt.Write(f0)
	pkt.Write(f1)
	pkt.Write(f2)
	pkt.Write(f3)
	pkt.Write(f4)

	got := SplitOpusPacketToSingleFrames(pkt.Bytes())
	want := [][]byte{f0, f1, f2, f3, f4}
	if len(got) != len(want) {
		t.Fatalf("expected %d split frames, got %d", len(want), len(got))
	}
	for i, w := range want {
		if got[i][0] != 0x68 {
			t.Fatalf("frame %d expected TOC=0x68, got 0x%02x", i, got[i][0])
		}
		if !bytes.Equal(got[i][1:], w) {
			t.Fatalf("frame %d payload mismatch", i)
		}
	}

	// Code=3 with padding flag (fc = 0x43 -> vbr=0, pad=1, M=3, padLen=4, three 10-byte frames)
	padded := []byte{0x6b, 0x43, 4}
	padded = append(padded, bytes.Repeat([]byte{0xa1}, 10)...)
	padded = append(padded, bytes.Repeat([]byte{0xb2}, 10)...)
	padded = append(padded, bytes.Repeat([]byte{0xc3}, 10)...)
	padded = append(padded, 0, 0, 0, 0)
	gotPad := SplitOpusPacketToSingleFrames(padded)
	if len(gotPad) != 3 || !bytes.Equal(gotPad[1][1:], bytes.Repeat([]byte{0xb2}, 10)) {
		t.Fatalf("padded code=3 split failed: %v", gotPad)
	}

	// Non-Opus mock string falls back safely.
	mockPkt := []byte("OPUS24K:hello")
	gotMock := SplitOpusPacketToSingleFrames(mockPkt)
	if len(gotMock) != 1 || !bytes.Equal(gotMock[0], mockPkt) {
		t.Fatalf("expected mock fallback, got %v", gotMock)
	}
}

type stubSpeechEZBridge struct {
	asrReady bool
	ttsReady bool
	asrText  string
	asrErr   error
	ttsPCM   []byte
	ttsErr   error
	asrCalls int
	ttsCalls int
	lastPCM  []byte
	lastText []string
}

func (s *stubSpeechEZBridge) Start(context.Context) error               { return nil }
func (s *stubSpeechEZBridge) Stop(context.Context) error                { return nil }
func (s *stubSpeechEZBridge) Status(context.Context) biz.EZBridgeStatus { return biz.EZBridgeStatus{} }
func (s *stubSpeechEZBridge) SetMode(context.Context, string) (biz.EZBridgeStatus, error) {
	return biz.EZBridgeStatus{}, nil
}
func (s *stubSpeechEZBridge) DiscoverHosts(context.Context, time.Duration) ([]biz.EZHostCandidate, error) {
	return nil, nil
}
func (s *stubSpeechEZBridge) StartPairing(context.Context, biz.EZPairRequest) (biz.EZBridgeStatus, error) {
	return biz.EZBridgeStatus{}, nil
}
func (s *stubSpeechEZBridge) ResetPairing(context.Context) (biz.EZBridgeStatus, error) {
	return biz.EZBridgeStatus{}, nil
}
func (s *stubSpeechEZBridge) IsOnline() bool                       { return s.asrReady || s.ttsReady }
func (s *stubSpeechEZBridge) EffectiveMode(context.Context) string { return biz.EZModeAuto }
func (s *stubSpeechEZBridge) RunTurn(context.Context, *biz.AgentTurnRequest, biz.AgentStreamCallbacks) error {
	return nil
}
func (s *stubSpeechEZBridge) SendEvent(context.Context, *biz.AgentDeviceEvent, biz.AgentStreamCallbacks) error {
	return nil
}
func (s *stubSpeechEZBridge) CancelTurn(context.Context, string, string, string) error { return nil }
func (s *stubSpeechEZBridge) CanUseASR() bool                                          { return s.asrReady }
func (s *stubSpeechEZBridge) CanUseTTS() bool                                          { return s.ttsReady }
func (s *stubSpeechEZBridge) RecognizeSpeech(_ context.Context, pcm16k []byte) (string, error) {
	s.asrCalls++
	s.lastPCM = append([]byte(nil), pcm16k...)
	return s.asrText, s.asrErr
}
func (s *stubSpeechEZBridge) SynthesizeSpeech(_ context.Context, text string) ([]byte, error) {
	s.ttsCalls++
	s.lastText = append(s.lastText, text)
	return s.ttsPCM, s.ttsErr
}

func buildTestPCM16WAV(sampleRate uint32, channels uint16, samples []int16) []byte {
	dataBytes := uint32(len(samples) * 2)
	var buf bytes.Buffer
	buf.WriteString("RIFF")
	_ = binary.Write(&buf, binary.LittleEndian, 36+dataBytes)
	buf.WriteString("WAVE")
	buf.WriteString("fmt ")
	_ = binary.Write(&buf, binary.LittleEndian, uint32(16))
	_ = binary.Write(&buf, binary.LittleEndian, uint16(1))
	_ = binary.Write(&buf, binary.LittleEndian, channels)
	_ = binary.Write(&buf, binary.LittleEndian, sampleRate)
	_ = binary.Write(&buf, binary.LittleEndian, sampleRate*uint32(channels)*2)
	_ = binary.Write(&buf, binary.LittleEndian, channels*2)
	_ = binary.Write(&buf, binary.LittleEndian, uint16(16))
	buf.WriteString("data")
	_ = binary.Write(&buf, binary.LittleEndian, dataBytes)
	for _, sample := range samples {
		_ = binary.Write(&buf, binary.LittleEndian, sample)
	}
	return buf.Bytes()
}

func TestWAVToPCM16kMonoAndSplitTextForRelayTTS(t *testing.T) {
	// Stereo 32kHz -> Mono 16kHz downmix & downsample check.
	samples := make([]int16, 0, 128)
	for i := 0; i < 64; i++ {
		samples = append(samples, 1000, -1000)
	}
	wav := buildTestPCM16WAV(32000, 2, samples)
	pcm, err := WAVToPCM16kMono(wav)
	if err != nil {
		t.Fatalf("WAVToPCM16kMono: %v", err)
	}
	if len(pcm) != 64 { // 64/2=32 mono 16kHz samples * 2 bytes = 64 bytes
		t.Fatalf("expected 64 bytes PCM16k mono, got %d", len(pcm))
	}
	if gotSample := int16(binary.LittleEndian.Uint16(pcm[0:2])); gotSample != 0 {
		t.Fatalf("expected centered downmixed sample 0, got %d", gotSample)
	}

	longText := "今天早上北京天气晴朗温暖，适合去公园散步，顺便买一杯热拿铁；下午有轻度多云但不会下雨，晚上记得关窗休息！接下来还有一段很长很长的连续字符测试用来确保超过一百二十个字符时会自动安全拆分为多段输出并且不会丢失任何原始内容，同时再追加一些额外描述保证总长度显著超越一百二十个 Unicode 字符上限。"
	parts := SplitTextForRelayTTS(longText, 120)
	if len(parts) < 2 {
		t.Fatalf("expected multiple segments for long text, got %v", parts)
	}
	for i, part := range parts {
		if utf8.RuneCountInString(part) > 120 || part == "" {
			t.Fatalf("segment %d violates length bound: runes=%d part=%q", i, utf8.RuneCountInString(part), part)
		}
	}
	noDelim := make([]rune, 250)
	for i := range noDelim {
		noDelim[i] = '测'
	}
	hardParts := SplitTextForRelayTTS(string(noDelim), 120)
	if len(hardParts) != 3 {
		t.Fatalf("expected 3 hard-split chunks for 250 runes, got %d", len(hardParts))
	}
}

func TestRoutingSpeechProvidersPreferEZRelayAndFallback(t *testing.T) {
	ctx := context.Background()
	wav := buildTestPCM16WAV(16000, 1, make([]int16, 1600)) // 0.1s 16kHz mono
	stub := &stubSpeechEZBridge{
		asrReady: true,
		ttsReady: true,
		asrText:  "EZ中继识别成功",
		ttsPCM:   make([]byte, 3200), // 0.1s 16kHz mono PCM16
	}

	asr := NewASRProvider(nil, nil, stub, log.DefaultLogger)
	tts := NewTTSProvider(nil, nil, stub, log.DefaultLogger)

	// 1. Prefer EZ relay when ready
	st, err := asr.StartStream(ctx, "dev-1")
	if err != nil {
		t.Fatalf("StartStream: %v", err)
	}
	_ = st.WriteOpusFrame(ctx, wav)
	gotText, err := st.Finish(ctx)
	if err != nil || gotText != "EZ中继识别成功" || stub.asrCalls != 1 {
		t.Fatalf("expected EZ ASR relay result, got text=%q err=%v calls=%d", gotText, err, stub.asrCalls)
	}

	// Verify ffmpeg OggOpus encoding works end-to-end for TTS relay.
	opusPackets, err := tts.Synthesize(ctx, "你好，这是中继测试。", "")
	if err != nil || len(opusPackets) == 0 || stub.ttsCalls != 1 {
		t.Fatalf("expected EZ TTS relay Opus packets, got len=%d err=%v calls=%d", len(opusPackets), err, stub.ttsCalls)
	}
	// Feed encoded Opus frames back through ASR relay (exercises OggOpusToPCM16kMono + ffmpeg).
	stOpus, _ := asr.StartStream(ctx, "dev-1")
	for _, pkt := range opusPackets {
		_ = stOpus.WriteOpusFrame(ctx, pkt)
	}
	if _, err := stOpus.Finish(ctx); err != nil {
		t.Fatalf("expected Opus -> PCM16k ASR relay round-trip ok, got %v", err)
	}

	// 2. Fallback seamlessly to local mock provider when relay errors
	stub.asrErr = errors.New("relay broken")
	stub.ttsErr = errors.New("relay broken")
	stFallback, _ := asr.StartStream(ctx, "dev-1")
	_ = stFallback.WriteOpusFrame(ctx, []byte{0x01, 0x02, 0x03})
	fallbackText, err := stFallback.Finish(ctx)
	if err != nil || fallbackText == "" {
		t.Fatalf("expected local mock ASR fallback on relay failure, got text=%q err=%v", fallbackText, err)
	}

	fallbackPackets, err := tts.Synthesize(ctx, "本地回退测试", "")
	if err != nil || len(fallbackPackets) == 0 {
		t.Fatalf("expected local mock TTS fallback on relay failure, got len=%d err=%v", len(fallbackPackets), err)
	}
}
