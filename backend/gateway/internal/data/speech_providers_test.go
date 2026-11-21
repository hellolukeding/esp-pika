package data

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"

	"esp-pika/backend/gateway/internal/conf"

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
	asr := NewASRProvider(cfg, log.DefaultLogger)
	st, err := asr.StartStream(ctx, "30:ed:a0:f3:dc:70")
	if err != nil {
		t.Fatalf("start stream: %v", err)
	}
	_ = st.WriteOpusFrame(ctx, []byte{0xDE, 0xAD, 0xBE, 0xEF})
	text, err := st.Finish(ctx)
	if err != nil || text != "帮我打开客厅的灯" {
		t.Fatalf("unexpected asr result: text=%q err=%v", text, err)
	}

	tts := NewTTSProvider(cfg, log.DefaultLogger)
	outFrames, err := tts.Synthesize(ctx, "好的，已开启。", "")
	if err != nil || len(outFrames) != 2 || !bytes.Equal(outFrames[0], ttsFrame1) || !bytes.Equal(outFrames[1], ttsFrame2) {
		t.Fatalf("unexpected tts frames: %v err=%v", outFrames, err)
	}
}
