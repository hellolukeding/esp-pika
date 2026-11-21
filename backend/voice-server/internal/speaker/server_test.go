package speaker

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"math"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/coder/websocket"
)

func makeSinePCM(freqHz float64, samples int) []byte {
	out := make([]byte, samples*2)
	for i := 0; i < samples; i++ {
		t := float64(i) / 16000.0
		v := 0.5*math.Sin(2*math.Pi*freqHz*t) + 0.3*math.Sin(2*math.Pi*freqHz*3.0*t)
		s := int16(v * 20000.0)
		binary.LittleEndian.PutUint16(out[2*i:2*i+2], uint16(s))
	}
	return out
}

func TestVoiceServerEnrollIdentifyAndStream(t *testing.T) {
	store := NewQdrantStore("", "speaker_embeddings")
	srv := NewServer(&SpectralEmbedder{}, store, 0.75)
	ts := httptest.NewServer(srv.Handler())
	defer ts.Close()

	pcmOwner := makeSinePCM(150.0, 8000)
	pcmOther := makeSinePCM(420.0, 8000)

	enrollBody, _ := json.Marshal(map[string]any{
		"speaker_id":   "spk_luke",
		"uid":          "u_1",
		"agent_id":     "ez-assistant",
		"group_id":     "home-default",
		"role":         1,
		"audio_base64": base64.StdEncoding.EncodeToString(pcmOwner),
	})
	resEnroll, err := http.Post(ts.URL+"/api/v1/speaker/enroll", "application/json", bytes.NewReader(enrollBody))
	if err != nil || resEnroll.StatusCode != http.StatusOK {
		t.Fatalf("enroll failed: err=%v resp=%v", err, resEnroll)
	}
	_ = resEnroll.Body.Close()

	// HTTP identify matching Owner.
	resID, err := http.Post(ts.URL+"/api/v1/speaker/identify?uid=u_1&agent_id=ez-assistant", "application/octet-stream", bytes.NewReader(pcmOwner))
	if err != nil {
		t.Fatalf("identify failed: %v", err)
	}
	var hit SearchHit
	_ = json.NewDecoder(resID.Body).Decode(&hit)
	_ = resID.Body.Close()
	if hit.SpeakerID != "spk_luke" || hit.Role != 1 || hit.Confidence < 0.99 {
		t.Fatalf("unexpected identify hit: %+v", hit)
	}

	// Non-matching speaker stays unidentified.
	resMiss, _ := http.Post(ts.URL+"/api/v1/speaker/identify?uid=u_1&agent_id=ez-assistant", "application/octet-stream", bytes.NewReader(pcmOther))
	var miss SearchHit
	_ = json.NewDecoder(resMiss.Body).Decode(&miss)
	_ = resMiss.Body.Close()
	if miss.SpeakerID != "" {
		t.Fatalf("expected non-matching speaker rejected, got %+v", miss)
	}

	// Streaming WS /api/v1/speaker/stream protocol per architecture.md §5.3.
	wsURL := "ws" + strings.TrimPrefix(ts.URL, "http") + "/api/v1/speaker/stream?uid=u_1&agent_id=ez-assistant"
	ctx := context.Background()
	conn, _, err := websocket.Dial(ctx, wsURL, nil)
	if err != nil {
		t.Fatalf("ws dial failed: %v", err)
	}
	defer conn.CloseNow()
	_ = conn.Write(ctx, websocket.MessageBinary, pcmOwner)
	_ = conn.Write(ctx, websocket.MessageText, []byte(`{"action":"finish"}`))
	_, msg, err := conn.Read(ctx)
	if err != nil {
		t.Fatalf("ws read failed: %v", err)
	}
	var wsHit SearchHit
	_ = json.Unmarshal(msg, &wsHit)
	if wsHit.SpeakerID != "spk_luke" {
		t.Fatalf("unexpected ws stream hit: %+v", wsHit)
	}

	// Verify WAV-wrapped PCM matches raw PCM embedding cleanly.
	wavHeader := make([]byte, 44)
	copy(wavHeader[0:4], "RIFF")
	binary.LittleEndian.PutUint32(wavHeader[4:8], uint32(36+len(pcmOwner)))
	copy(wavHeader[8:12], "WAVEfmt ")
	binary.LittleEndian.PutUint32(wavHeader[16:20], 16)
	binary.LittleEndian.PutUint16(wavHeader[20:22], 1)
	binary.LittleEndian.PutUint16(wavHeader[22:24], 1)
	binary.LittleEndian.PutUint32(wavHeader[24:28], 16000)
	binary.LittleEndian.PutUint32(wavHeader[28:32], 32000)
	binary.LittleEndian.PutUint16(wavHeader[32:34], 2)
	binary.LittleEndian.PutUint16(wavHeader[34:36], 16)
	copy(wavHeader[36:40], "data")
	binary.LittleEndian.PutUint32(wavHeader[40:44], uint32(len(pcmOwner)))
	wavPayload := append(wavHeader, pcmOwner...)

	resWAV, err := http.Post(ts.URL+"/api/v1/speaker/identify?uid=u_1&agent_id=ez-assistant", "audio/wav", bytes.NewReader(wavPayload))
	if err != nil {
		t.Fatalf("wav identify failed: %v", err)
	}
	var wavHit SearchHit
	_ = json.NewDecoder(resWAV.Body).Decode(&wavHit)
	_ = resWAV.Body.Close()
	if wavHit.SpeakerID != "spk_luke" || wavHit.Confidence < 0.99 {
		t.Fatalf("expected WAV payload to match spk_luke identically, got %+v", wavHit)
	}
}
