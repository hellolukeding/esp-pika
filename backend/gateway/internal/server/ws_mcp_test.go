package server

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"esp-pika/backend/gateway/internal/biz"
	"esp-pika/backend/gateway/internal/conf"
	"esp-pika/backend/gateway/internal/data"

	"github.com/coder/websocket"
	"github.com/go-kratos/kratos/v2/log"
	"google.golang.org/protobuf/types/known/durationpb"
)

type fakeAgentAdapter struct {
	lastText      string
	lastSpeakerID string
	lastZone      string
	cancelled     bool
}

func (f *fakeAgentAdapter) RunTurn(ctx context.Context, req *biz.AgentTurnRequest, cb biz.AgentStreamCallbacks) error {
	f.lastText = req.Text
	f.lastSpeakerID = req.SpeakerID
	f.lastZone = req.Zone
	if cb.OnStatus != nil {
		cb.OnStatus("thinking", "正在思考", false)
	}
	if cb.OnTextDelta != nil {
		cb.OnTextDelta("已收到，正在查看门口。", "")
	}
	if cb.OnDone != nil {
		cb.OnDone("completed", "")
	}
	return nil
}

func (f *fakeAgentAdapter) SendEvent(ctx context.Context, evt *biz.AgentDeviceEvent, cb biz.AgentStreamCallbacks) error {
	return nil
}

func (f *fakeAgentAdapter) CancelTurn(ctx context.Context, sessionID, turnID, reason string) error {
	f.cancelled = true
	return nil
}

func setupTestServer(t *testing.T) (*httptest.Server, *biz.MacAuthUsecase, *biz.ACLUsecase, *fakeAgentAdapter, func()) {
	t.Helper()
	d, cleanupData, err := data.NewData(&conf.Data{
		Database: &conf.Data_Database{Driver: "sqlite3", Source: ":memory:"},
	}, log.DefaultLogger)
	if err != nil {
		t.Fatalf("new data: %v", err)
	}

	macRepo := data.NewSQLiteAllowedMacRepo(d)
	aclRepo := data.NewSQLiteACLRepo(d)
	spRepo := data.NewSQLiteSpeakerRepo(d)
	devRepo := data.NewDeviceRepo(d)
	bus := data.NewNATSEventBus(d, log.DefaultLogger)

	macAuth := biz.NewMacAuthUsecase(macRepo, log.DefaultLogger)
	acl := biz.NewACLUsecase(aclRepo, log.DefaultLogger)
	devices := biz.NewDeviceUsecase(devRepo, acl, bus, log.DefaultLogger)
	speakers := biz.NewSpeakerUsecase(spRepo, devices, bus, log.DefaultLogger)
	router := biz.NewSessionRouter(spRepo)

	agent := &fakeAgentAdapter{}
	speech := biz.NewSpeechPipeline(
		data.NewASRProvider(nil, log.DefaultLogger),
		data.NewTTSProvider(nil, log.DefaultLogger),
		data.NewVoiceReviewer(nil, log.DefaultLogger),
		speakers,
	)
	wsHandler, cleanupWS := NewDeviceWSHandler(
		&conf.Server{MediaDir: t.TempDir()},
		&conf.Arbiter{Window: durationpb.New(40 * time.Millisecond)},
		macAuth, devices, speakers, router, bus, agent, speech, log.DefaultLogger,
	)
	voiceDbg := NewVoiceDebugHandler(nil, speakers, speech, log.DefaultLogger)
	mcp := NewMCPServer(devices, speakers, log.DefaultLogger)

	mux := http.NewServeMux()
	mux.HandleFunc("/ws/device", wsHandler.ServeWS)
	mux.HandleFunc("/api/v1/media/upload", wsHandler.HandleMediaUpload)
	mux.HandleFunc("/api/v1/voice/enroll", voiceDbg.HandleEnroll)
	mux.HandleFunc("/api/v1/voice/identify", voiceDbg.HandleIdentify)
	mux.HandleFunc("/api/v1/voice/transcribe", voiceDbg.HandleTranscribe)
	mux.Handle("/media/", http.StripPrefix("/media/", http.FileServer(http.Dir(wsHandler.MediaDir()))))
	mux.Handle("/ui/", NewWebUIHandler())
	srv := mcp
	mux.Handle("/mcp", srv)
	ts := httptest.NewServer(mux)

	return ts, macAuth, acl, agent, func() {
		ts.Close()
		cleanupWS()
		cleanupData()
	}
}

func TestGatewayWSAuthMediaAndMCP(t *testing.T) {
	ts, macAuth, acl, agent, cleanup := setupTestServer(t)
	defer cleanup()

	ctx := context.Background()
	wsURL := "ws" + strings.TrimPrefix(ts.URL, "http") + "/ws/device"

	// 1. Unauthorized MAC must be rejected with 403 on both WS and HTTP media upload.
	_, resp, err := websocket.Dial(ctx, wsURL, &websocket.DialOptions{
		HTTPHeader: http.Header{"X-Device-MAC": []string{"ff:ff:ff:ff:ff:ff"}},
	})
	if err == nil || (resp != nil && resp.StatusCode != http.StatusForbidden) {
		t.Fatalf("expected 403 for unauthorized MAC ws dial, got err=%v resp=%v", err, resp)
	}

	reqMediaBad, _ := http.NewRequest(http.MethodPost, ts.URL+"/api/v1/media/upload", bytes.NewReader([]byte("jpeg")))
	reqMediaBad.Header.Set("X-Device-MAC", "ff:ff:ff:ff:ff:ff")
	respMediaBad, err := http.DefaultClient.Do(reqMediaBad)
	if err != nil {
		t.Fatalf("media upload request error: %v", err)
	}
	_ = respMediaBad.Body.Close()
	if respMediaBad.StatusCode != http.StatusForbidden {
		t.Fatalf("expected 403 for unauthorized media upload, got %d", respMediaBad.StatusCode)
	}

	// 2. Register allowed MAC and ACL rule.
	allowedMAC := "30:ed:a0:f3:dc:70"
	if _, err := macAuth.Upsert(ctx, &biz.AllowedMac{
		MAC:     allowedMAC,
		GroupID: "home-default",
		Zone:    "entrance",
		Label:   "Front Door S31",
		Enabled: true,
	}); err != nil {
		t.Fatalf("upsert allowed mac: %v", err)
	}
	if _, err := acl.Upsert(ctx, &biz.ACLRule{
		RuleID:            "butler-entrance",
		AgentID:           "butler",
		GroupID:           "home-default",
		ZonePattern:       "entrance",
		CapabilityPattern: "device.*",
		MinSpeakerRole:    biz.SpeakerRoleOwner,
		Allow:             true,
	}); err != nil {
		t.Fatalf("upsert acl: %v", err)
	}

	// 3. Connect authorized device over WS and perform device.hello.
	conn, _, err := websocket.Dial(ctx, wsURL, &websocket.DialOptions{
		HTTPHeader: http.Header{"X-Device-MAC": []string{allowedMAC}},
	})
	if err != nil {
		t.Fatalf("authorized ws dial failed: %v", err)
	}
	defer conn.CloseNow()

	helloJSON, _ := json.Marshal(map[string]any{
		"type":     "device.hello",
		"mac":      allowedMAC,
		"group_id": "home-default",
		"zone":     "entrance",
		"chip":     "esp32s31",
		"capabilities": []map[string]any{
			{"name": "device.camera_capture", "type": "tool", "upload_channel": "http_post"},
		},
	})
	if err := conn.Write(ctx, websocket.MessageText, helloJSON); err != nil {
		t.Fatalf("write hello: %v", err)
	}
	_, welcomeBytes, err := conn.Read(ctx)
	if err != nil {
		t.Fatalf("read welcome: %v", err)
	}
	if !strings.Contains(string(welcomeBytes), `"device.welcome"`) {
		t.Fatalf("expected device.welcome, got %s", string(welcomeBytes))
	}

	// 4. Authorized media upload succeeds and uploaded JPEG is downloadable via /media/.
	jpegStub := []byte{0xff, 0xd8, 0xff, 0xd9}
	reqMediaOK, _ := http.NewRequest(http.MethodPost, ts.URL+"/api/v1/media/upload", bytes.NewReader(jpegStub))
	reqMediaOK.Header.Set("X-Device-MAC", allowedMAC)
	respMediaOK, err := http.DefaultClient.Do(reqMediaOK)
	if err != nil {
		t.Fatalf("authorized media upload failed: %v", err)
	}
	var uploadRes map[string]any
	_ = json.NewDecoder(respMediaOK.Body).Decode(&uploadRes)
	_ = respMediaOK.Body.Close()
	if respMediaOK.StatusCode != http.StatusOK {
		t.Fatalf("expected 200 for authorized media upload, got %d", respMediaOK.StatusCode)
	}
	mediaURL, _ := uploadRes["media_url"].(string)
	if mediaURL == "" {
		t.Fatalf("expected non-empty media_url in upload response, got %+v", uploadRes)
	}
	getMedia, err := http.Get(ts.URL + mediaURL)
	if err != nil {
		t.Fatalf("GET %s failed: %v", mediaURL, err)
	}
	_ = getMedia.Body.Close()
	if getMedia.StatusCode != http.StatusOK {
		t.Fatalf("expected 200 GET %s, got %d", mediaURL, getMedia.StatusCode)
	}

	// 5. Device responder goroutine for incoming MCP device.command & agent turn output collection.
	var (
		gotDelta    string
		gotTTSAudio bool
		gotDone     bool
	)
	doneCh := make(chan struct{})
	go func() {
		defer close(doneCh)
		for {
			msgType, msg, err := conn.Read(ctx)
			if err != nil {
				return
			}
			if msgType == websocket.MessageBinary {
				if strings.HasPrefix(string(msg), "OPUS24K:") {
					gotTTSAudio = true
				}
				continue
			}
			var env map[string]any
			if err := json.Unmarshal(msg, &env); err != nil {
				continue
			}
			switch env["type"] {
			case "device.command":
				ack, _ := json.Marshal(map[string]any{
					"type":       "command.result",
					"command_id": env["command_id"],
					"ok":         true,
					"media_url":  "/media/30-ed-a0-f3-dc-70/test.jpg",
				})
				_ = conn.Write(ctx, websocket.MessageText, ack)
			case "agent.text_delta":
				if chunk, ok := env["chunk"].(string); ok {
					gotDelta += chunk
				}
			case "agent.done":
				gotDone = true
				return
			}
		}
	}()

	// 6. Verify Streamable HTTP MCP initialize + notifications/initialized (202 Accepted) for rmcp 3.2.
	initBody := `{"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"ez-assistant","version":"0.33.1"}}}`
	resInit, err := http.Post(ts.URL+"/mcp", "application/json", strings.NewReader(initBody))
	if err != nil {
		t.Fatalf("mcp initialize failed: %v", err)
	}
	var initResp map[string]any
	_ = json.NewDecoder(resInit.Body).Decode(&initResp)
	_ = resInit.Body.Close()
	if initResp["error"] != nil {
		t.Fatalf("unexpected mcp initialize error: %+v", initResp["error"])
	}
	resNotify, err := http.Post(ts.URL+"/mcp", "application/json", strings.NewReader(`{"jsonrpc":"2.0","method":"notifications/initialized"}`))
	if err != nil {
		t.Fatalf("mcp notifications/initialized failed: %v", err)
	}
	_ = resNotify.Body.Close()
	if resNotify.StatusCode != http.StatusAccepted {
		t.Fatalf("expected 202 Accepted for notifications/initialized, got %d", resNotify.StatusCode)
	}

	// 6a. MCP resources/read fleet://topology & tools/call with ACL check.
	mcpTopoBody := `{"jsonrpc":"2.0","id":1,"method":"resources/read","params":{"uri":"fleet://topology"}}`
	resTopo, err := http.Post(ts.URL+"/mcp", "application/json", strings.NewReader(mcpTopoBody))
	if err != nil {
		t.Fatalf("mcp resources/read failed: %v", err)
	}
	var topoResp map[string]any
	_ = json.NewDecoder(resTopo.Body).Decode(&topoResp)
	_ = resTopo.Body.Close()
	if topoResp["error"] != nil {
		t.Fatalf("unexpected mcp error: %+v", topoResp["error"])
	}

	// 6a. Guest role must be denied by ACL.
	mcpDeniedReq, _ := http.NewRequest(http.MethodPost, ts.URL+"/mcp",
		strings.NewReader(`{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"device.camera_capture","arguments":{"target":"entrance"}}}`))
	mcpDeniedReq.Header.Set("Content-Type", "application/json")
	mcpDeniedReq.Header.Set("X-Agent-ID", "butler")
	mcpDeniedReq.Header.Set("X-Speaker-Role", "guest")
	resDenied, err := http.DefaultClient.Do(mcpDeniedReq)
	if err != nil {
		t.Fatalf("mcp denied call failed: %v", err)
	}
	var deniedResp map[string]any
	_ = json.NewDecoder(resDenied.Body).Decode(&deniedResp)
	_ = resDenied.Body.Close()
	if deniedResp["error"] == nil {
		t.Fatalf("expected ACL error for guest speaker role, got %+v", deniedResp)
	}

	// 6b. Owner role succeeds end-to-end through WS command and returns media_url.
	mcpOKReq, _ := http.NewRequest(http.MethodPost, ts.URL+"/mcp",
		strings.NewReader(`{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"device.camera_capture","arguments":{"target":"entrance"}}}`))
	mcpOKReq.Header.Set("Content-Type", "application/json")
	mcpOKReq.Header.Set("X-Agent-ID", "butler")
	mcpOKReq.Header.Set("X-Speaker-Role", "owner")
	resOK, err := http.DefaultClient.Do(mcpOKReq)
	if err != nil {
		t.Fatalf("mcp ok call failed: %v", err)
	}
	var okResp map[string]any
	_ = json.NewDecoder(resOK.Body).Decode(&okResp)
	_ = resOK.Body.Close()
	if okResp["error"] != nil {
		t.Fatalf("unexpected mcp error for owner: %+v", okResp["error"])
	}
	rawResult, _ := json.Marshal(okResp["result"])
	if !strings.Contains(string(rawResult), "/media/30-ed-a0-f3-dc-70/test.jpg") {
		t.Fatalf("expected media_url in MCP response, got %s", string(rawResult))
	}

	// 7. Send speech.start + binary Opus frame + speech.stop -> verify ASR -> Agent -> TTS audio downlink.
	if err := conn.Write(ctx, websocket.MessageText, []byte(`{"type":"speech.start","speaker_id":"spk_luke","speaker_role":1,"confidence":0.92}`)); err != nil {
		t.Fatalf("write speech.start: %v", err)
	}
	if err := conn.Write(ctx, websocket.MessageBinary, []byte{0xAA, 0xBB, 0xCC, 0xDD}); err != nil {
		t.Fatalf("write binary opus frame: %v", err)
	}
	if err := conn.Write(ctx, websocket.MessageText, []byte(`{"type":"speech.stop","turn_id":"t_ws_1"}`)); err != nil {
		t.Fatalf("write speech.stop: %v", err)
	}
	select {
	case <-doneCh:
	case <-time.After(2 * time.Second):
		t.Fatalf("timed out waiting for agent.done")
	}
	if !gotDone || !gotTTSAudio || gotDelta != "已收到，正在查看门口。" {
		t.Fatalf("unexpected WS agent/TTS stream output: gotDone=%v gotTTSAudio=%v gotDelta=%q", gotDone, gotTTSAudio, gotDelta)
	}
	if agent.lastSpeakerID != "spk_luke" || agent.lastZone != "entrance" || !strings.Contains(agent.lastText, "frames=1") {
		t.Fatalf("expected ASR + speaker context forwarded to AgentAdapter, got speaker=%q zone=%q text=%q", agent.lastSpeakerID, agent.lastZone, agent.lastText)
	}

	// 8. Verify embedded Web Console assets & browser ASR debug endpoint serve cleanly.
	uiResp, err := http.Get(ts.URL + "/ui/")
	if err != nil || uiResp.StatusCode != http.StatusOK {
		t.Fatalf("expected 200 GET /ui/, got err=%v resp=%v", err, uiResp)
	}
	_ = uiResp.Body.Close()
	jsResp, err := http.Get(ts.URL + "/ui/app.js")
	if err != nil || jsResp.StatusCode != http.StatusOK {
		t.Fatalf("expected 200 GET /ui/app.js, got err=%v resp=%v", err, jsResp)
	}
	_ = jsResp.Body.Close()

	transcribeReq, _ := json.Marshal(map[string]any{
		"device_id":    "web-sim",
		"audio_base64": base64.StdEncoding.EncodeToString([]byte("RIFFmockwavdata")),
	})
	trResp, err := http.Post(ts.URL+"/api/v1/voice/transcribe", "application/json", bytes.NewReader(transcribeReq))
	if err != nil || trResp.StatusCode != http.StatusOK {
		t.Fatalf("expected 200 POST /api/v1/voice/transcribe, got err=%v resp=%v", err, trResp)
	}
	var trJSON map[string]any
	_ = json.NewDecoder(trResp.Body).Decode(&trJSON)
	_ = trResp.Body.Close()
	if txt, _ := trJSON["text"].(string); !strings.Contains(txt, "web-sim") {
		t.Fatalf("expected mock ASR text from /api/v1/voice/transcribe, got %+v", trJSON)
	}
}
