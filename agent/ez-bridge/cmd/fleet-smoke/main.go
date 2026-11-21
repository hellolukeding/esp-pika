package main

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"math"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"

	"github.com/coder/websocket"
)

func main() {
	gwURL := flag.String("gateway", "http://127.0.0.1:8080", "Gateway HTTP base URL")
	vsURL := flag.String("voice-server", "http://127.0.0.1:8081", "Voice Server HTTP base URL")
	qdrantURL := flag.String("qdrant", "http://127.0.0.1:6333", "Qdrant HTTP base URL")
	natsMonURL := flag.String("nats-mon", "http://127.0.0.1:8222", "NATS monitoring HTTP base URL")
	mockEZAddr := flag.String("mock-ez-listen", "", "If non-empty, serve a dev ez-assistant HTTP/SSE stub on this addr (e.g. 127.0.0.1:7240)")
	flag.Parse()

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	if *mockEZAddr != "" {
		srv := startMockEZRuntime(*mockEZAddr, *gwURL)
		defer func() { _ = srv.Close() }()
		fmt.Printf("[smoke] started local ez-assistant stub on http://%s\n", *mockEZAddr)
		// Wait briefly for downstream processes (ez-bridge + gateway) to come up before testing.
		waitReady(ctx, *gwURL+"/healthz", 15*time.Second)
	}

	mustHealthy(ctx, "NATS", *natsMonURL+"/healthz")
	mustHealthy(ctx, "Qdrant", *qdrantURL+"/readyz")
	mustHealthy(ctx, "voice-server", *vsURL+"/healthz")
	mustHealthy(ctx, "gateway", *gwURL+"/healthz")
	mustHealthy(ctx, "gateway-webui", *gwURL+"/ui/")
	mustHealthy(ctx, "gateway-webui-js", *gwURL+"/ui/app.js")

	// 1. Verify unauthorized MAC gets 403 on both WS and Media upload.
	wsBase := "ws" + strings.TrimPrefix(*gwURL, "http") + "/ws/device"
	_, resp, err := websocket.Dial(ctx, wsBase, &websocket.DialOptions{
		HTTPHeader: http.Header{"X-Device-MAC": []string{"ff:ff:ff:ff:ff:ff"}},
	})
	if err == nil || (resp != nil && resp.StatusCode != http.StatusForbidden) {
		fatalf("expected 403 for unauthorized MAC on WS, got err=%v resp=%v", err, resp)
	}
	reqBad, _ := http.NewRequestWithContext(ctx, http.MethodPost, *gwURL+"/api/v1/media/upload", bytes.NewReader([]byte("x")))
	reqBad.Header.Set("X-Device-MAC", "ff:ff:ff:ff:ff:ff")
	respBad, err := http.DefaultClient.Do(reqBad)
	if err != nil || respBad.StatusCode != http.StatusForbidden {
		fatalf("expected 403 for unauthorized MAC media upload, got err=%v resp=%v", err, respBad)
	}
	_ = respBad.Body.Close()
	fmt.Println("[PASS] 1. Strict MAC whitelist rejects unauthorized device (403)")

	// 2. Register study & entrance devices, ACL, and speaker voiceprint in Qdrant + Gateway.
	studyMAC := "30:ed:a0:f3:dc:70"
	entranceMAC := "30:ed:a0:f3:dc:72"
	putJSON(ctx, *gwURL+"/api/v1/macs/"+studyMAC, map[string]any{
		"group_id": "home-default", "zone": "study", "label": "Study S31", "enabled": true,
	})
	putJSON(ctx, *gwURL+"/api/v1/macs/"+entranceMAC, map[string]any{
		"group_id": "home-default", "zone": "entrance", "label": "Entrance S31", "enabled": true,
	})
	putJSON(ctx, *gwURL+"/api/v1/acls/allow-ez-all", map[string]any{
		"agent_id": "ez-assistant", "group_id": "home-default",
		"zone_pattern": "*", "capability_pattern": "device.*", "min_speaker_role": 1, "allow": true,
	})
	putJSON(ctx, *gwURL+"/api/v1/speakers/spk_luke", map[string]any{
		"uid": "u_luke", "agent_id": "ez-assistant", "group_id": "home-default",
		"display_name": "Luke", "role": 1, "tts_voice": "zh_male_warm",
	})
	pcmOwner := makeSinePCM(150.0, 8000)
	postJSON(ctx, *vsURL+"/api/v1/speaker/enroll", map[string]any{
		"speaker_id": "spk_luke", "uid": "u_luke", "agent_id": "ez-assistant",
		"group_id": "home-default", "role": 1,
		"audio_base64": base64.StdEncoding.EncodeToString(pcmOwner),
	})
	// Verify Gateway same-origin Web Console voice proxy endpoints (/api/v1/voice/enroll & identify).
	postJSON(ctx, *gwURL+"/api/v1/voice/enroll", map[string]any{
		"speaker_id": "spk_web_test", "display_name": "WebTest", "uid": "u_luke",
		"agent_id": "ez-assistant", "group_id": "home-default", "role": 1,
		"tts_voice": "zh_male_warm", "audio_base64": base64.StdEncoding.EncodeToString(pcmOwner),
	})
	idBody, _ := json.Marshal(map[string]any{
		"uid": "u_luke", "agent_id": "ez-assistant",
		"audio_base64": base64.StdEncoding.EncodeToString(pcmOwner),
	})
	idResp, err := http.Post(*gwURL+"/api/v1/voice/identify", "application/json", bytes.NewReader(idBody))
	if err != nil || idResp.StatusCode != http.StatusOK {
		fatalf("gateway /api/v1/voice/identify failed: err=%v resp=%v", err, idResp)
	}
	var idHit struct {
		SpeakerID  string  `json:"speaker_id"`
		Confidence float32 `json:"confidence"`
	}
	_ = json.NewDecoder(idResp.Body).Decode(&idHit)
	_ = idResp.Body.Close()
	if idHit.SpeakerID == "" || idHit.Confidence < 0.99 {
		fatalf("expected gateway /api/v1/voice/identify high match, got %+v", idHit)
	}
	fmt.Println("[PASS] 2. Registered fleet MACs, ACLs, and Qdrant speaker voiceprint (incl. /api/v1/voice/* proxy)")

	// 3. Connect both simulated ESP32-S31 WebSocket devices and perform device.hello.
	studyConn := dialAndHello(ctx, wsBase, studyMAC, "study", []map[string]any{
		{"name": "device.display_show", "type": "tool"},
	})
	defer studyConn.CloseNow()
	entranceConn := dialAndHello(ctx, wsBase, entranceMAC, "entrance", []map[string]any{
		{"name": "device.camera_capture", "type": "tool", "upload_channel": "http_post"},
	})
	defer entranceConn.CloseNow()

	// 4. Multi-device Wake Arbitration: study has higher SNR & score than entrance.
	_ = writeJSON(ctx, studyConn, map[string]any{
		"type": "wake.detected", "group_id": "home-default", "zone": "study",
		"wake_word": "hi_esp", "wake_score": 0.96, "snr_db": 22.5, "detected_at_ms": time.Now().UnixMilli(),
	})
	_ = writeJSON(ctx, entranceConn, map[string]any{
		"type": "wake.detected", "group_id": "home-default", "zone": "entrance",
		"wake_word": "hi_esp", "wake_score": 0.80, "snr_db": 11.0, "detected_at_ms": time.Now().UnixMilli(),
	})

	studyWake := readUntilType(ctx, studyConn, "wake.accept", "wake.suppress")
	entranceWake := readUntilType(ctx, entranceConn, "wake.accept", "wake.suppress")
	if studyWake["type"] != "wake.accept" || entranceWake["type"] != "wake.suppress" {
		fatalf("unexpected wake arbitration outcome: study=%v entrance=%v", studyWake, entranceWake)
	}
	fmt.Println("[PASS] 3. Multi-device wake arbitration selected study (accept) and suppressed entrance")

	// 5. Entrance command handler: responds to MCP device.camera_capture with JPEG upload.
	go func() {
		for {
			_, msg, err := entranceConn.Read(ctx)
			if err != nil {
				return
			}
			var env map[string]any
			if json.Unmarshal(msg, &env) == nil && env["type"] == "device.command" {
				jpegPayload := []byte{0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0xff, 0xd9}
				reqUp, _ := http.NewRequestWithContext(ctx, http.MethodPost, *gwURL+"/api/v1/media/upload", bytes.NewReader(jpegPayload))
				reqUp.Header.Set("X-Device-MAC", entranceMAC)
				upResp, err := http.DefaultClient.Do(reqUp)
				if err != nil {
					return
				}
				var upJSON map[string]any
				_ = json.NewDecoder(upResp.Body).Decode(&upJSON)
				_ = upResp.Body.Close()

				_ = writeJSON(ctx, entranceConn, map[string]any{
					"type":       "command.result",
					"command_id": env["command_id"],
					"ok":         true,
					"media_url":  upJSON["media_url"],
				})
			}
		}
	}()

	// 6. Cross-room voice turn from study -> ez-bridge -> Agent -> MCP entrance capture -> /media GET -> TTS downlink.
	_ = writeJSON(ctx, studyConn, map[string]any{
		"type":         "text.turn",
		"turn_id":      "turn_live_1",
		"text":         "帮我看看门口是谁",
		"speaker_id":   "spk_luke",
		"speaker_role": 1,
		"agent_id":     "ez-assistant",
	})
	gotText, gotTTS := collectTurnOutput(ctx, studyConn)
	if !gotTTS || !strings.Contains(gotText, "快递员") {
		fatalf("cross-room turn mismatch: gotTTS=%v gotText=%q", gotTTS, gotText)
	}
	fmt.Printf("[PASS] 4. Cross-room turn -> MCP camera_capture -> /media GET -> TTS audio: %q\n", gotText)

	// 7. Binary 16kHz Opus speech.start -> binary frames -> speech.stop -> ASR -> Agent -> TTS downlink.
	_ = writeJSON(ctx, studyConn, map[string]any{
		"type": "speech.start", "speaker_id": "spk_luke", "speaker_role": 1, "confidence": 0.91, "agent_id": "ez-assistant",
	})
	_ = readUntilType(ctx, studyConn, "session.routed")
	_ = studyConn.Write(ctx, websocket.MessageBinary, []byte{0x01, 0x02, 0x03, 0x04, 0x05, 0x06})
	_ = writeJSON(ctx, studyConn, map[string]any{"type": "speech.stop", "turn_id": "turn_live_2"})
	gotText2, gotTTS2 := collectTurnOutput(ctx, studyConn)
	if !gotTTS2 || gotText2 == "" {
		fatalf("binary speech turn mismatch: gotTTS2=%v gotText2=%q", gotTTS2, gotText2)
	}
	fmt.Printf("[PASS] 5. Binary Opus speech.start/stop -> ASR -> Agent -> TTS downlink: %q\n", gotText2)
	fmt.Println("ALL LIVE STACK SMOKE SCENARIOS PASSED")
}

func startMockEZRuntime(listenAddr, gwURL string) *http.Server {
	var (
		mu         sync.Mutex
		sseClients []chan string
	)
	broadcast := func(s string) {
		mu.Lock()
		defer mu.Unlock()
		for _, ch := range sseClients {
			ch <- s
		}
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/health", func(w http.ResponseWriter, _ *http.Request) { w.WriteHeader(http.StatusOK) })
	mux.HandleFunc("/runtime/ensure-ready", func(w http.ResponseWriter, _ *http.Request) { w.WriteHeader(http.StatusNoContent) })
	mux.HandleFunc("/events", func(w http.ResponseWriter, r *http.Request) {
		flusher := w.(http.Flusher)
		w.Header().Set("Content-Type", "text/event-stream")
		_, _ = fmt.Fprint(w, ": connected\n\n")
		flusher.Flush()
		ch := make(chan string, 32)
		mu.Lock()
		sseClients = append(sseClients, ch)
		mu.Unlock()
		for {
			select {
			case <-r.Context().Done():
				return
			case msg := <-ch:
				_, _ = fmt.Fprintf(w, "event: runtime_event\ndata: %s\n\n", msg)
				flusher.Flush()
			}
		}
	})
	mux.HandleFunc("/commands", func(w http.ResponseWriter, r *http.Request) {
		var req struct {
			RequestID string `json:"request_id"`
			Command   struct {
				Payload struct {
					Type    string         `json:"type"`
					Payload map[string]any `json:"payload"`
				} `json:"payload"`
			} `json:"command"`
		}
		_ = json.NewDecoder(r.Body).Decode(&req)
		var resPayload any
		switch req.Command.Payload.Type {
		case "create_session":
			resPayload = map[string]any{"session": map[string]any{"session_id": "ez_live_1"}}
		case "set_session_approval_mode":
			resPayload = map[string]any{"session": map[string]any{"session_id": "ez_live_1"}}
		case "submit_input":
			msgText, _ := req.Command.Payload.Payload["message"].(string)
			runID := fmt.Sprintf("run_%d", time.Now().UnixNano())
			resPayload = map[string]any{"input_id": "inp_live", "run": map[string]any{"run_id": runID}}
			go func() {
				broadcast(fmt.Sprintf(`{"sequence":1,"emitted_at_ms":1,"event":{"type":"run_started","session_id":"ez_live_1","run_id":%q}}`, runID))
				reply := "已收到语音指令，一切正常。"
				if strings.Contains(msgText, "门口") {
					broadcast(fmt.Sprintf(`{"sequence":2,"emitted_at_ms":2,"event":{"type":"tool_started","session_id":"ez_live_1","run_id":%q,"step":1,"call_id":"c1","tool_name":"device.camera_capture"}}`, runID))
					reply = runMCPCameraCaptureFlow(gwURL)
				}
				delta, _ := json.Marshal(map[string]any{
					"sequence": 3, "emitted_at_ms": 3,
					"event": map[string]any{"type": "text_delta", "session_id": "ez_live_1", "run_id": runID, "step": 1, "part_id": "p1", "delta": reply},
				})
				broadcast(string(delta))
				broadcast(fmt.Sprintf(`{"sequence":4,"emitted_at_ms":4,"event":{"type":"run_finished","session_id":"ez_live_1","run_id":%q,"status":"completed"}}`, runID))
			}()
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]any{
			"request_id": req.RequestID,
			"result": map[string]any{
				"scope":   "runtime",
				"payload": map[string]any{"type": req.Command.Payload.Type, "payload": resPayload},
			},
		})
	})

	srv := &http.Server{Addr: listenAddr, Handler: mux}
	go func() { _ = srv.ListenAndServe() }()
	return srv
}

func runMCPCameraCaptureFlow(gwURL string) string {
	// Perform real rmcp-style Streamable HTTP handshake: initialize -> 202 notification -> tools/call -> GET /media/
	postMCP := func(body string) (*http.Response, []byte) {
		req, _ := http.NewRequest(http.MethodPost, gwURL+"/mcp", strings.NewReader(body))
		req.Header.Set("Content-Type", "application/json")
		req.Header.Set("X-Agent-ID", "ez-assistant")
		req.Header.Set("X-Speaker-Role", "owner")
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			return nil, nil
		}
		b, _ := io.ReadAll(resp.Body)
		_ = resp.Body.Close()
		return resp, b
	}

	_, _ = postMCP(`{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}}`)
	nResp, _ := postMCP(`{"jsonrpc":"2.0","method":"notifications/initialized"}`)
	if nResp == nil || nResp.StatusCode != http.StatusAccepted {
		return "MCP 握手异常。"
	}
	_, callRaw := postMCP(`{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"device.camera_capture","arguments":{"target":"entrance"}}}`)
	var rpcResp struct {
		Result struct {
			Content []struct {
				Text string `json:"text"`
			} `json:"content"`
		} `json:"result"`
	}
	if err := json.Unmarshal(callRaw, &rpcResp); err != nil || len(rpcResp.Result.Content) == 0 {
		return "门口拍照工具调用失败。"
	}
	var cmdOut struct {
		MediaURL string `json:"media_url"`
	}
	_ = json.Unmarshal([]byte(rpcResp.Result.Content[0].Text), &cmdOut)
	if cmdOut.MediaURL == "" {
		return "门口未返回图片地址。"
	}
	imgResp, err := http.Get(gwURL + cmdOut.MediaURL)
	if err != nil || imgResp.StatusCode != http.StatusOK {
		return "门口照片下载失败。"
	}
	_ = imgResp.Body.Close()
	return "门口已经拍照确认，来访的是快递员。"
}

func dialAndHello(ctx context.Context, wsBase, mac, zone string, caps []map[string]any) *websocket.Conn {
	c, _, err := websocket.Dial(ctx, wsBase, &websocket.DialOptions{
		HTTPHeader: http.Header{"X-Device-MAC": []string{mac}},
	})
	if err != nil {
		fatalf("dial %s: %v", zone, err)
	}
	_ = writeJSON(ctx, c, map[string]any{
		"type": "device.hello", "mac": mac, "group_id": "home-default", "zone": zone,
		"chip": "esp32s31", "capabilities": caps,
	})
	_ = readUntilType(ctx, c, "device.welcome")
	return c
}

func readUntilType(ctx context.Context, c *websocket.Conn, want ...string) map[string]any {
	wantSet := make(map[string]bool, len(want))
	for _, w := range want {
		wantSet[w] = true
	}
	for {
		mt, data, err := c.Read(ctx)
		if err != nil {
			fatalf("ws read waiting for %v: %v", want, err)
		}
		if mt != websocket.MessageText {
			continue
		}
		var env map[string]any
		if json.Unmarshal(data, &env) == nil {
			if t, _ := env["type"].(string); wantSet[t] {
				return env
			}
		}
	}
}

func collectTurnOutput(ctx context.Context, c *websocket.Conn) (string, bool) {
	var text strings.Builder
	var gotTTS bool
	for {
		mt, data, err := c.Read(ctx)
		if err != nil {
			fatalf("ws read turn output: %v", err)
		}
		if mt == websocket.MessageBinary {
			if strings.HasPrefix(string(data), "OPUS24K:") {
				gotTTS = true
			}
			continue
		}
		var env map[string]any
		if json.Unmarshal(data, &env) != nil {
			continue
		}
		switch env["type"] {
		case "agent.text_delta":
			if s, _ := env["chunk"].(string); s != "" {
				text.WriteString(s)
			}
		case "agent.done":
			return text.String(), gotTTS
		}
	}
}

func waitReady(ctx context.Context, url string, timeout time.Duration) {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		req, _ := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
		resp, err := http.DefaultClient.Do(req)
		if err == nil {
			_ = resp.Body.Close()
			if resp.StatusCode == http.StatusOK {
				return
			}
		}
		time.Sleep(100 * time.Millisecond)
	}
	fatalf("timed out waiting for %s", url)
}

func mustHealthy(ctx context.Context, name, url string) {
	req, _ := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	resp, err := http.DefaultClient.Do(req)
	if err != nil || resp.StatusCode >= 300 {
		fatalf("%s unhealthy (%s): err=%v resp=%v", name, url, err, resp)
	}
	_ = resp.Body.Close()
}

func putJSON(ctx context.Context, url string, body any) {
	b, _ := json.Marshal(body)
	req, _ := http.NewRequestWithContext(ctx, http.MethodPut, url, bytes.NewReader(b))
	req.Header.Set("Content-Type", "application/json")
	resp, err := http.DefaultClient.Do(req)
	if err != nil || resp.StatusCode >= 300 {
		fatalf("PUT %s failed: err=%v resp=%v", url, err, resp)
	}
	_ = resp.Body.Close()
}

func postJSON(ctx context.Context, url string, body any) {
	b, _ := json.Marshal(body)
	req, _ := http.NewRequestWithContext(ctx, http.MethodPost, url, bytes.NewReader(b))
	req.Header.Set("Content-Type", "application/json")
	resp, err := http.DefaultClient.Do(req)
	if err != nil || resp.StatusCode >= 300 {
		fatalf("POST %s failed: err=%v resp=%v", url, err, resp)
	}
	_ = resp.Body.Close()
}

func writeJSON(ctx context.Context, c *websocket.Conn, v any) error {
	b, err := json.Marshal(v)
	if err != nil {
		return err
	}
	return c.Write(ctx, websocket.MessageText, b)
}

func makeSinePCM(freqHz float64, samples int) []byte {
	out := make([]byte, samples*2)
	for i := 0; i < samples; i++ {
		t := float64(i) / 16000.0
		v := 0.5*math.Sin(2*math.Pi*freqHz*t) + 0.3*math.Sin(2*math.Pi*freqHz*3.0*t)
		binary.LittleEndian.PutUint16(out[2*i:2*i+2], uint16(int16(v*20000.0)))
	}
	return out
}

func fatalf(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "FAIL: "+format+"\n", args...)
	os.Exit(1)
}
