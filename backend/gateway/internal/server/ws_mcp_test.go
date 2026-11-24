package server

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
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
	lastText         string
	lastSpeakerID    string
	lastZone         string
	lastSystemPrompt string
	lastAPIKey       string
	lastModel        string
	cancelled        bool
}

func (f *fakeAgentAdapter) RunTurn(ctx context.Context, req *biz.AgentTurnRequest, cb biz.AgentStreamCallbacks) error {
	f.lastText = req.Text
	f.lastSpeakerID = req.SpeakerID
	f.lastZone = req.Zone
	f.lastSystemPrompt = req.SystemPrompt
	f.lastAPIKey = req.APIKey
	f.lastModel = req.Model
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
	settingsRepo := data.NewSQLiteSettingsRepo(d)
	speech := biz.NewSpeechPipeline(
		data.NewASRProvider(nil, settingsRepo, nil, log.DefaultLogger),
		data.NewTTSProvider(nil, settingsRepo, nil, log.DefaultLogger),
		data.NewVoiceReviewer(nil, log.DefaultLogger),
		speakers,
	)
	settings := biz.NewSettingsUsecase(settingsRepo, speakers)
	wsHandler, cleanupWS := NewDeviceWSHandler(
		&conf.Server{MediaDir: t.TempDir()},
		&conf.Arbiter{Window: durationpb.New(40 * time.Millisecond)},
		macAuth, devices, speakers, router, bus, agent, speech, settings, log.DefaultLogger,
	)
	voiceDbg := NewVoiceDebugHandler(nil, speakers, speech, settings, log.DefaultLogger)
	mcp := NewMCPServer(devices, speakers, log.DefaultLogger)

	mux := http.NewServeMux()
	mux.HandleFunc("/ws/device", wsHandler.ServeWS)
	mux.HandleFunc("/api/v1/media/upload", wsHandler.HandleMediaUpload)
	mux.HandleFunc("/api/v1/voice/enroll", voiceDbg.HandleEnroll)
	mux.HandleFunc("/api/v1/voice/identify", voiceDbg.HandleIdentify)
	mux.HandleFunc("/api/v1/voice/transcribe", voiceDbg.HandleTranscribe)
	mux.HandleFunc("/api/v1/settings/prompt", voiceDbg.HandleSystemPrompt)
	mux.HandleFunc("/api/v1/settings/deepseek", voiceDbg.HandleDeepSeekSettings)
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

func setupVoiceTestServer(t *testing.T, voiceServerURL string) (*httptest.Server, *biz.MacAuthUsecase, *biz.ACLUsecase, *fakeAgentAdapter, func()) {
	t.Helper()
	dbCfg := &conf.Data{
		Database: &conf.Data_Database{
			Source: t.TempDir() + "/gateway-test.db",
		},
	}
	dataLayer, cleanupData, err := data.NewData(dbCfg, log.DefaultLogger)
	if err != nil {
		t.Fatalf("NewData failed: %v", err)
	}
	macRepo := data.NewSQLiteAllowedMacRepo(dataLayer)
	macAuth := biz.NewMacAuthUsecase(macRepo, log.DefaultLogger)
	devRepo := data.NewDeviceRepo(dataLayer)
	aclRepo := data.NewSQLiteACLRepo(dataLayer)
	acl := biz.NewACLUsecase(aclRepo, log.DefaultLogger)
	bus := data.NewNATSEventBus(dataLayer, log.DefaultLogger)
	devices := biz.NewDeviceUsecase(devRepo, acl, bus, log.DefaultLogger)
	spRepo := data.NewSQLiteSpeakerRepo(dataLayer)
	speakers := biz.NewSpeakerUsecase(spRepo, devices, bus, log.DefaultLogger)
	router := biz.NewSessionRouter(spRepo)
	agent := &fakeAgentAdapter{}
	voiceCfg := &conf.Voice{VoiceServerUrl: voiceServerURL}
	settingsRepo := data.NewSQLiteSettingsRepo(dataLayer)
	speech := biz.NewSpeechPipeline(
		data.NewASRProvider(nil, settingsRepo, nil, log.DefaultLogger),
		data.NewTTSProvider(nil, settingsRepo, nil, log.DefaultLogger),
		data.NewVoiceReviewer(voiceCfg, log.DefaultLogger),
		speakers,
	)
	settings := biz.NewSettingsUsecase(settingsRepo, speakers)
	wsHandler, cleanupWS := NewDeviceWSHandler(
		&conf.Server{MediaDir: t.TempDir()},
		&conf.Arbiter{Window: durationpb.New(40 * time.Millisecond)},
		macAuth, devices, speakers, router, bus, agent, speech, settings, log.DefaultLogger,
	)
	voiceDbg := NewVoiceDebugHandler(voiceCfg, speakers, speech, settings, log.DefaultLogger)
	mcp := NewMCPServer(devices, speakers, log.DefaultLogger)

	mux := http.NewServeMux()
	mux.HandleFunc("/ws/device", wsHandler.ServeWS)
	mux.HandleFunc("/api/v1/media/upload", wsHandler.HandleMediaUpload)
	mux.HandleFunc("/api/v1/voice/enroll", voiceDbg.HandleEnroll)
	mux.HandleFunc("/api/v1/voice/identify", voiceDbg.HandleIdentify)
	mux.HandleFunc("/api/v1/voice/transcribe", voiceDbg.HandleTranscribe)
	mux.HandleFunc("/api/v1/settings/prompt", voiceDbg.HandleSystemPrompt)
	mux.HandleFunc("/api/v1/settings/deepseek", voiceDbg.HandleDeepSeekSettings)
	mux.Handle("/mcp", mcp)
	ts := httptest.NewServer(mux)

	return ts, macAuth, acl, agent, func() {
		ts.Close()
		cleanupWS()
		cleanupData()
	}
}

func dialAuthorizedDevice(t *testing.T, ctx context.Context, tsURL string, macAuth *biz.MacAuthUsecase, mac, zone string, vpVersion uint64, caps []map[string]any) *websocket.Conn {
	t.Helper()
	if _, err := macAuth.Upsert(ctx, &biz.AllowedMac{
		MAC:     mac,
		GroupID: "home-default",
		Zone:    zone,
		Label:   zone,
		Enabled: true,
	}); err != nil {
		t.Fatalf("upsert allowed mac %s: %v", mac, err)
	}
	wsURL := "ws" + strings.TrimPrefix(tsURL, "http") + "/ws/device"
	conn, _, err := websocket.Dial(ctx, wsURL, &websocket.DialOptions{
		HTTPHeader: http.Header{"X-Device-MAC": []string{mac}},
	})
	if err != nil {
		t.Fatalf("ws dial %s failed: %v", mac, err)
	}
	helloJSON, _ := json.Marshal(map[string]any{
		"type":         "device.hello",
		"mac":          mac,
		"group_id":     "home-default",
		"zone":         zone,
		"chip":         "esp32s31",
		"vp_version":   vpVersion,
		"capabilities": caps,
	})
	if err := conn.Write(ctx, websocket.MessageText, helloJSON); err != nil {
		t.Fatalf("write device.hello %s: %v", mac, err)
	}
	_, welcomeBytes, err := conn.Read(ctx)
	if err != nil || !strings.Contains(string(welcomeBytes), `"device.welcome"`) {
		t.Fatalf("expected device.welcome for %s, got err=%v msg=%s", mac, err, string(welcomeBytes))
	}
	return conn
}

func readWSJSONType(t *testing.T, ctx context.Context, conn *websocket.Conn, timeout time.Duration, wantTypes ...string) map[string]any {
	t.Helper()
	rctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	for {
		mt, data, err := conn.Read(rctx)
		if err != nil {
			t.Fatalf("read ws frame waiting for %v: %v", wantTypes, err)
		}
		if mt != websocket.MessageText {
			continue
		}
		var env map[string]any
		if err := json.Unmarshal(data, &env); err != nil {
			continue
		}
		gotType, _ := env["type"].(string)
		if len(wantTypes) == 0 {
			return env
		}
		for _, wt := range wantTypes {
			if gotType == wt {
				return env
			}
		}
	}
}

func TestGatewayMultiTerminalVoiceprintSync192D(t *testing.T) {
	vec192 := make([]float32, 192)
	for i := range vec192 {
		vec192[i] = float32(i+1) * 0.01
	}
	vsTS := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/v1/speaker/enroll" {
			http.NotFound(w, r)
			return
		}
		var req struct {
			SpeakerID string `json:"speaker_id"`
		}
		_ = json.NewDecoder(r.Body).Decode(&req)
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]any{
			"ok":         true,
			"speaker_id": req.SpeakerID,
			"dim":        192,
			"embedding":  vec192,
		})
	}))
	defer vsTS.Close()

	ts, macAuth, _, _, cleanup := setupVoiceTestServer(t, vsTS.URL)
	defer cleanup()

	ctx := context.Background()
	conn1 := dialAuthorizedDevice(t, ctx, ts.URL, macAuth, "30:ed:a0:f3:dc:71", "living_room", 0, nil)
	defer conn1.CloseNow()
	conn2 := dialAuthorizedDevice(t, ctx, ts.URL, macAuth, "30:ed:a0:f3:dc:72", "study", 0, nil)
	defer conn2.CloseNow()

	enrollReq, _ := json.Marshal(map[string]any{
		"speaker_id":   "spk_luke",
		"uid":          "u_luke",
		"agent_id":     "ez-assistant",
		"group_id":     "home-default",
		"display_name": "Luke",
		"role":         1,
		"audio_base64": base64.StdEncoding.EncodeToString([]byte{0x01, 0x02, 0x03, 0x04}),
	})
	resp, err := http.Post(ts.URL+"/api/v1/voice/enroll", "application/json", bytes.NewReader(enrollReq))
	if err != nil || resp.StatusCode != http.StatusOK {
		t.Fatalf("enroll failed: err=%v resp=%v", err, resp)
	}
	_ = resp.Body.Close()

	assertSync := func(label string, msg map[string]any) {
		t.Helper()
		upserts, _ := msg["upserts"].([]any)
		if len(upserts) != 1 {
			t.Fatalf("%s expected 1 upsert in voiceprint.sync, got %+v", label, msg)
		}
		item, _ := upserts[0].(map[string]any)
		if item["speaker_id"] != "spk_luke" {
			t.Fatalf("%s expected spk_luke, got %+v", label, item)
		}
		emb, _ := item["embedding"].([]any)
		if len(emb) != 192 {
			t.Fatalf("%s expected 192-D embedding, got %d", label, len(emb))
		}
	}

	assertSync("term1", readWSJSONType(t, ctx, conn1, 2*time.Second, "voiceprint.sync"))
	assertSync("term2", readWSJSONType(t, ctx, conn2, 2*time.Second, "voiceprint.sync"))

	// Newly connecting 3rd terminal behind vp_version receives full_sync=true on device.hello.
	conn3 := dialAuthorizedDevice(t, ctx, ts.URL, macAuth, "30:ed:a0:f3:dc:73", "bedroom", 0, nil)
	defer conn3.CloseNow()
	sync3 := readWSJSONType(t, ctx, conn3, 2*time.Second, "voiceprint.sync")
	if full, _ := sync3["full_sync"].(bool); !full {
		t.Fatalf("expected full_sync=true for newly joined conn3, got %+v", sync3)
	}
	assertSync("term3", sync3)
}

func TestGatewayGrayZoneVoiceprintReview(t *testing.T) {
	reviewHits := make(chan string, 8)
	vsTS := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/v1/speaker/identify" {
			http.NotFound(w, r)
			return
		}
		reviewHits <- r.Header.Get("X-Device-MAC")
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]any{
			"speaker_id": "spk_cloud_verified",
			"role":       1,
			"confidence": 0.91,
		})
	}))
	defer vsTS.Close()

	ts, macAuth, _, agent, cleanup := setupVoiceTestServer(t, vsTS.URL)
	defer cleanup()

	ctx := context.Background()
	mac := "30:ed:a0:f3:dc:74"
	conn := dialAuthorizedDevice(t, ctx, ts.URL, macAuth, mac, "study", 0, nil)
	defer conn.CloseNow()

	// Gray-zone edge confidence (0.74 in [0.65, 0.85)) triggers cloud review on speech.stop.
	if err := conn.Write(ctx, websocket.MessageText, []byte(`{"type":"speech.start","speaker_id":"spk_ambiguous","speaker_role":3,"confidence":0.74}`)); err != nil {
		t.Fatalf("write speech.start gray: %v", err)
	}
	if err := conn.Write(ctx, websocket.MessageBinary, []byte{0x11, 0x22, 0x33, 0x44}); err != nil {
		t.Fatalf("write gray audio: %v", err)
	}
	if err := conn.Write(ctx, websocket.MessageText, []byte(`{"type":"speech.stop","turn_id":"t_gray_1"}`)); err != nil {
		t.Fatalf("write speech.stop gray: %v", err)
	}
	_ = readWSJSONType(t, ctx, conn, 2*time.Second, "agent.done")

	select {
	case reviewedMAC := <-reviewHits:
		if reviewedMAC != mac {
			t.Fatalf("expected X-Device-MAC %s in gray-zone review, got %s", mac, reviewedMAC)
		}
	case <-time.After(time.Second):
		t.Fatalf("expected gray-zone cloud review request to voice-server")
	}
	if agent.lastSpeakerID != "spk_cloud_verified" {
		t.Fatalf("expected agent speaker upgraded to spk_cloud_verified, got %q", agent.lastSpeakerID)
	}

	// Confident local match (>= 0.85) stays local without cloud gray-zone review.
	if err := conn.Write(ctx, websocket.MessageText, []byte(`{"type":"speech.start","speaker_id":"spk_local_owner","speaker_role":1,"confidence":0.93}`)); err != nil {
		t.Fatalf("write speech.start local: %v", err)
	}
	if err := conn.Write(ctx, websocket.MessageBinary, []byte{0x55, 0x66}); err != nil {
		t.Fatalf("write local audio: %v", err)
	}
	if err := conn.Write(ctx, websocket.MessageText, []byte(`{"type":"speech.stop","turn_id":"t_local_2"}`)); err != nil {
		t.Fatalf("write speech.stop local: %v", err)
	}
	_ = readWSJSONType(t, ctx, conn, 2*time.Second, "agent.done")

	select {
	case unexpectedMAC := <-reviewHits:
		t.Fatalf("did not expect gray-zone cloud review for confident local speaker, got hit from %s", unexpectedMAC)
	default:
	}
	if agent.lastSpeakerID != "spk_local_owner" {
		t.Fatalf("expected local speaker spk_local_owner preserved, got %q", agent.lastSpeakerID)
	}
}

func TestGatewayMultiTerminalWakeArbitration(t *testing.T) {
	ts, macAuth, _, _, cleanup := setupVoiceTestServer(t, "")
	defer cleanup()

	ctx := context.Background()
	macLiving := "30:ed:a0:f3:dc:81"
	macStudy := "30:ed:a0:f3:dc:82"
	connLiving := dialAuthorizedDevice(t, ctx, ts.URL, macAuth, macLiving, "living_room", 0, nil)
	defer connLiving.CloseNow()
	connStudy := dialAuthorizedDevice(t, ctx, ts.URL, macAuth, macStudy, "study", 0, nil)
	defer connStudy.CloseNow()

	wakeLiving := `{"type":"wake.detected","group_id":"home-default","zone":"living_room","wake_word":"pika","wake_score":0.92,"snr_db":22.5,"detected_at_ms":1000}`
	wakeStudy := `{"type":"wake.detected","group_id":"home-default","zone":"study","wake_word":"pika","wake_score":0.89,"snr_db":11.0,"detected_at_ms":1010}`
	if err := connLiving.Write(ctx, websocket.MessageText, []byte(wakeLiving)); err != nil {
		t.Fatalf("write wake living: %v", err)
	}
	if err := connStudy.Write(ctx, websocket.MessageText, []byte(wakeStudy)); err != nil {
		t.Fatalf("write wake study: %v", err)
	}

	livingDecision := readWSJSONType(t, ctx, connLiving, 2*time.Second, "wake.accept", "wake.suppress")
	studyDecision := readWSJSONType(t, ctx, connStudy, 2*time.Second, "wake.accept", "wake.suppress")
	if livingDecision["type"] != "wake.accept" || livingDecision["winner_device_id"] != macLiving {
		t.Fatalf("expected living_room wake.accept winner=%s, got %+v", macLiving, livingDecision)
	}
	if studyDecision["type"] != "wake.suppress" || studyDecision["winner_device_id"] != macLiving {
		t.Fatalf("expected study wake.suppress winner=%s, got %+v", macLiving, studyDecision)
	}
}

func TestGatewayMCPACLRoleEnforcement(t *testing.T) {
	ts, macAuth, acl, _, cleanup := setupVoiceTestServer(t, "")
	defer cleanup()

	ctx := context.Background()
	mac := "30:ed:a0:f3:dc:91"
	caps := []map[string]any{
		{"name": "device.sensor_read", "type": "tool"},
		{"name": "device.actuator_set", "type": "tool"},
	}
	conn := dialAuthorizedDevice(t, ctx, ts.URL, macAuth, mac, "study", 0, caps)
	defer conn.CloseNow()

	go func() {
		for {
			mt, data, err := conn.Read(ctx)
			if err != nil {
				return
			}
			if mt != websocket.MessageText {
				continue
			}
			var env map[string]any
			if json.Unmarshal(data, &env) == nil && env["type"] == "device.command" {
				ack, _ := json.Marshal(map[string]any{
					"type":        "command.result",
					"command_id":  env["command_id"],
					"ok":          true,
					"result_json": `{"status":"ok"}`,
				})
				_ = conn.Write(ctx, websocket.MessageText, ack)
			}
		}
	}()

	if _, err := acl.Upsert(ctx, &biz.ACLRule{
		RuleID:            "butler-study-sensor",
		AgentID:           "butler",
		GroupID:           "home-default",
		ZonePattern:       "study",
		CapabilityPattern: "device.sensor_read",
		MinSpeakerRole:    biz.SpeakerRoleMember,
		Allow:             true,
	}); err != nil {
		t.Fatalf("upsert sensor acl: %v", err)
	}
	if _, err := acl.Upsert(ctx, &biz.ACLRule{
		RuleID:            "butler-study-actuator",
		AgentID:           "butler",
		GroupID:           "home-default",
		ZonePattern:       "study",
		CapabilityPattern: "device.actuator_set",
		MinSpeakerRole:    biz.SpeakerRoleOwner,
		Allow:             true,
	}); err != nil {
		t.Fatalf("upsert actuator acl: %v", err)
	}

	callTool := func(id int, roleHeader, toolName string, args map[string]any) map[string]any {
		body, _ := json.Marshal(map[string]any{
			"jsonrpc": "2.0",
			"id":      id,
			"method":  "tools/call",
			"params": map[string]any{
				"name":      toolName,
				"arguments": args,
			},
		})
		req, _ := http.NewRequest(http.MethodPost, ts.URL+"/mcp", bytes.NewReader(body))
		req.Header.Set("Content-Type", "application/json")
		req.Header.Set("X-Agent-ID", "butler")
		if roleHeader != "" {
			req.Header.Set("X-Speaker-Role", roleHeader)
		}
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatalf("mcp request %s (%q) failed: %v", toolName, roleHeader, err)
		}
		var out map[string]any
		_ = json.NewDecoder(resp.Body).Decode(&out)
		_ = resp.Body.Close()
		return out
	}

	sensorArgs := map[string]any{"target": "study", "sensor_type": "temperature"}
	actuatorArgs := map[string]any{"target": "study", "actuator": "relay_1", "action": "on"}

	if res := callTool(1, "", "device.sensor_read", sensorArgs); res["error"] == nil {
		t.Fatalf("expected missing X-Speaker-Role to default to guest and be denied, got %+v", res)
	}
	if res := callTool(2, "guest", "device.sensor_read", sensorArgs); res["error"] == nil {
		t.Fatalf("expected guest to be denied device.sensor_read, got %+v", res)
	}
	if res := callTool(3, "member", "device.sensor_read", sensorArgs); res["error"] != nil {
		t.Fatalf("expected member allowed for device.sensor_read, got %+v", res["error"])
	}
	if res := callTool(4, "member", "device.actuator_set", actuatorArgs); res["error"] == nil {
		t.Fatalf("expected member denied for owner-only device.actuator_set, got %+v", res)
	}
	if res := callTool(5, "owner", "device.actuator_set", actuatorArgs); res["error"] != nil {
		t.Fatalf("expected owner allowed for device.actuator_set, got %+v", res["error"])
	}
}

func TestGatewaySystemPromptAPIAndTurnInjection(t *testing.T) {
	ts, macAuth, _, agent, cleanup := setupVoiceTestServer(t, "")
	defer cleanup()

	ctx := context.Background()
	mac := "30:ed:a0:f3:dc:95"
	conn := dialAuthorizedDevice(t, ctx, ts.URL, macAuth, mac, "study", 0, nil)
	defer conn.CloseNow()

	// 1. Initially returns default prompt when no override is saved.
	respGet, err := http.Get(ts.URL + "/api/v1/settings/prompt")
	if err != nil || respGet.StatusCode != http.StatusOK {
		t.Fatalf("GET /api/v1/settings/prompt failed: err=%v resp=%v", err, respGet)
	}
	var getOut map[string]any
	_ = json.NewDecoder(respGet.Body).Decode(&getOut)
	_ = respGet.Body.Close()
	if getOut["system_prompt"] != "" || getOut["effective_prompt"] != biz.DefaultSystemPrompt {
		t.Fatalf("expected default prompt initially, got %+v", getOut)
	}

	// 2. PUT custom global system prompt and verify subsequent turn receives it.
	putReq, _ := http.NewRequest(http.MethodPut, ts.URL+"/api/v1/settings/prompt",
		strings.NewReader(`{"system_prompt":"你现在是一只毒舌但暖心的桌宠，只说中文，每次不超过20字。"}`))
	putReq.Header.Set("Content-Type", "application/json")
	respPut, err := http.DefaultClient.Do(putReq)
	if err != nil || respPut.StatusCode != http.StatusOK {
		t.Fatalf("PUT /api/v1/settings/prompt failed: err=%v resp=%v", err, respPut)
	}
	_ = respPut.Body.Close()

	if err := conn.Write(ctx, websocket.MessageText, []byte(`{"type":"speech.turn","turn_id":"t_p1","text":"早安"}`)); err != nil {
		t.Fatalf("write speech.turn 1: %v", err)
	}
	_ = readWSJSONType(t, ctx, conn, 2*time.Second, "agent.done")
	if !strings.Contains(agent.lastSystemPrompt, "毒舌但暖心的桌宠") {
		t.Fatalf("expected custom global system prompt on turn, got %q", agent.lastSystemPrompt)
	}

	// 3. Enroll speaker persona prompt and verify it appends [Speaker Persona].
	enrollBody, _ := json.Marshal(map[string]any{
		"speaker_id":   "spk_boss",
		"display_name": "Boss",
		"group_id":     "home-default",
		"role":         1,
		"prompt":       "请称呼我为老板",
	})
	respSpk, err := http.Post(ts.URL+"/api/v1/voice/enroll", "application/json", bytes.NewReader(enrollBody))
	if err != nil || respSpk.StatusCode != http.StatusOK {
		t.Fatalf("enroll speaker persona failed: err=%v resp=%v", err, respSpk)
	}
	_ = respSpk.Body.Close()

	if err := conn.Write(ctx, websocket.MessageText, []byte(`{"type":"speech.turn","turn_id":"t_p2","speaker_id":"spk_boss","speaker_role":1,"text":"查一下日程"}`)); err != nil {
		t.Fatalf("write speech.turn 2: %v", err)
	}
	_ = readWSJSONType(t, ctx, conn, 2*time.Second, "agent.done")
	if !strings.Contains(agent.lastSystemPrompt, "毒舌但暖心的桌宠") || !strings.Contains(agent.lastSystemPrompt, "请称呼我为老板") {
		t.Fatalf("expected global prompt + speaker persona merged, got %q", agent.lastSystemPrompt)
	}

	// 4. Empty string resets override back to biz.DefaultSystemPrompt.
	resetReq, _ := http.NewRequest(http.MethodPut, ts.URL+"/api/v1/settings/prompt",
		strings.NewReader(`{"system_prompt":""}`))
	resetReq.Header.Set("Content-Type", "application/json")
	respReset, err := http.DefaultClient.Do(resetReq)
	if err != nil || respReset.StatusCode != http.StatusOK {
		t.Fatalf("reset prompt failed: err=%v resp=%v", err, respReset)
	}
	_ = respReset.Body.Close()

	if err := conn.Write(ctx, websocket.MessageText, []byte(`{"type":"speech.turn","turn_id":"t_p3","text":"你好"}`)); err != nil {
		t.Fatalf("write speech.turn 3: %v", err)
	}
	_ = readWSJSONType(t, ctx, conn, 2*time.Second, "agent.done")
	if agent.lastSystemPrompt != biz.DefaultSystemPrompt {
		t.Fatalf("expected reset back to DefaultSystemPrompt, got %q", agent.lastSystemPrompt)
	}
}

func TestGatewayDeepSeekSettingsAPIAndTurnInjection(t *testing.T) {
	t.Setenv("DEEPSEEK_API_KEY", "")

	ts, macAuth, _, agent, cleanup := setupVoiceTestServer(t, "")
	defer cleanup()

	ctx := context.Background()
	conn := dialAuthorizedDevice(t, ctx, ts.URL, macAuth, "30:ed:a0:f3:dc:96", "study", 0, nil)
	defer conn.CloseNow()

	// 1. Unconfigured initially.
	respGet, err := http.Get(ts.URL + "/api/v1/settings/deepseek")
	if err != nil || respGet.StatusCode != http.StatusOK {
		t.Fatalf("GET /api/v1/settings/deepseek failed: err=%v resp=%v", err, respGet)
	}
	var view map[string]any
	_ = json.NewDecoder(respGet.Body).Decode(&view)
	_ = respGet.Body.Close()
	if view["configured"] != false || view["source"] != "none" || view["model"] != biz.DefaultDeepSeekModel || view["tts_voice"] != biz.DefaultDeepSeekTTSVoice {
		t.Fatalf("unexpected initial deepseek settings view: %+v", view)
	}

	// 2. Save key, model, and voice via Web Console API and verify response masks raw key.
	putReq, _ := http.NewRequest(http.MethodPut, ts.URL+"/api/v1/settings/deepseek",
		strings.NewReader(`{"api_key":"sk-live-secret-abcd","model":"deepseek-chat","tts_voice":"tide"}`))
	putReq.Header.Set("Content-Type", "application/json")
	respPut, err := http.DefaultClient.Do(putReq)
	if err != nil || respPut.StatusCode != http.StatusOK {
		t.Fatalf("PUT /api/v1/settings/deepseek failed: err=%v resp=%v", err, respPut)
	}
	var putOut map[string]any
	_ = json.NewDecoder(respPut.Body).Decode(&putOut)
	_ = respPut.Body.Close()
	if putOut["configured"] != true || putOut["source"] != "console" || putOut["masked_key"] != "sk-****abcd" || putOut["model"] != "deepseek-chat" || putOut["tts_voice"] != "tide" {
		t.Fatalf("unexpected PUT deepseek response: %+v", putOut)
	}
	if rawJSON, _ := json.Marshal(putOut); strings.Contains(string(rawJSON), "sk-live-secret-abcd") {
		t.Fatalf("raw api_key leaked in response: %s", string(rawJSON))
	}

	// 3. Verify GET still masks key and never returns plaintext secret.
	respGet2, err := http.Get(ts.URL + "/api/v1/settings/deepseek")
	if err != nil || respGet2.StatusCode != http.StatusOK {
		t.Fatalf("GET after PUT failed: err=%v resp=%v", err, respGet2)
	}
	var getOut2 map[string]any
	_ = json.NewDecoder(respGet2.Body).Decode(&getOut2)
	_ = respGet2.Body.Close()
	if rawJSON, _ := json.Marshal(getOut2); strings.Contains(string(rawJSON), "sk-live-secret-abcd") {
		t.Fatalf("raw api_key leaked in GET response: %s", string(rawJSON))
	}
	if getOut2["masked_key"] != "sk-****abcd" {
		t.Fatalf("expected masked_key sk-****abcd, got %+v", getOut2)
	}

	// 4. Verify subsequent speech.turn injects dynamic APIKey and Model.
	if err := conn.Write(ctx, websocket.MessageText, []byte(`{"type":"speech.turn","turn_id":"t_ds1","text":"你好"}`)); err != nil {
		t.Fatalf("write speech.turn: %v", err)
	}
	_ = readWSJSONType(t, ctx, conn, 2*time.Second, "agent.done")
	if agent.lastAPIKey != "sk-live-secret-abcd" || agent.lastModel != "deepseek-chat" {
		t.Fatalf("expected turn APIKey/Model injected, got key=%q model=%q", agent.lastAPIKey, agent.lastModel)
	}

	// 5. Clear stored key and verify source resets to none.
	clrReq, _ := http.NewRequest(http.MethodPut, ts.URL+"/api/v1/settings/deepseek",
		strings.NewReader(`{"clear_key":true}`))
	clrReq.Header.Set("Content-Type", "application/json")
	respClr, err := http.DefaultClient.Do(clrReq)
	if err != nil || respClr.StatusCode != http.StatusOK {
		t.Fatalf("clear key failed: err=%v resp=%v", err, respClr)
	}
	var clrOut map[string]any
	_ = json.NewDecoder(respClr.Body).Decode(&clrOut)
	_ = respClr.Body.Close()
	if clrOut["configured"] != false || clrOut["source"] != "none" || clrOut["masked_key"] != "" {
		t.Fatalf("expected cleared key state, got %+v", clrOut)
	}
}

type fakeMultiSentenceAgent struct{}

func (f *fakeMultiSentenceAgent) RunTurn(_ context.Context, _ *biz.AgentTurnRequest, cb biz.AgentStreamCallbacks) error {
	if cb.OnTextDelta != nil {
		cb.OnTextDelta("这是第一句测试语音内容。这是第二句测试语音内容。", "")
	}
	if cb.OnDone != nil {
		cb.OnDone("completed", "")
	}
	return nil
}

func (f *fakeMultiSentenceAgent) SendEvent(context.Context, *biz.AgentDeviceEvent, biz.AgentStreamCallbacks) error {
	return nil
}

func (f *fakeMultiSentenceAgent) CancelTurn(context.Context, string, string, string) error {
	return nil
}

type fakePipelinedTTS struct {
	mu          sync.Mutex
	synthStarts []time.Time
}

func (f *fakePipelinedTTS) Synthesize(_ context.Context, _ string, _ string) ([][]byte, error) {
	f.mu.Lock()
	idx := len(f.synthStarts)
	f.synthStarts = append(f.synthStarts, time.Now())
	f.mu.Unlock()

	time.Sleep(60 * time.Millisecond)
	frames := make([][]byte, 140) // 140 * 20ms = 2800ms
	for i := range frames {
		frames[i] = []byte{0x08, byte(idx), byte(i)}
	}
	return frames, nil
}

func TestGatewayTTSTurnPipelinePrefetchAndRealTimePacer(t *testing.T) {
	d, cleanupData, err := data.NewData(&conf.Data{
		Database: &conf.Data_Database{Driver: "sqlite3", Source: ":memory:"},
	}, log.DefaultLogger)
	if err != nil {
		t.Fatalf("new data: %v", err)
	}
	defer cleanupData()

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
	tts := &fakePipelinedTTS{}
	speech := biz.NewSpeechPipeline(nil, tts, nil, speakers)
	settingsRepo := data.NewSQLiteSettingsRepo(d)
	settings := biz.NewSettingsUsecase(settingsRepo, speakers)
	agent := &fakeMultiSentenceAgent{}

	wsHandler, cleanupWS := NewDeviceWSHandler(
		&conf.Server{MediaDir: t.TempDir()},
		&conf.Arbiter{Window: durationpb.New(40 * time.Millisecond)},
		macAuth, devices, speakers, router, bus, agent, speech, settings, log.DefaultLogger,
	)
	defer cleanupWS()

	mux := http.NewServeMux()
	mux.HandleFunc("/ws/device", wsHandler.ServeWS)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	ctx := context.Background()
	conn := dialAuthorizedDevice(t, ctx, ts.URL, macAuth, "30:ed:a0:f3:dc:97", "study", 0, nil)
	defer conn.CloseNow()

	if err := conn.Write(ctx, websocket.MessageText, []byte(`{"type":"speech.turn","turn_id":"t_pipe","text":"长回复"}`)); err != nil {
		t.Fatalf("write speech.turn: %v", err)
	}

	turnStart := time.Now()
	readCtx, cancel := context.WithTimeout(ctx, 8*time.Second)
	defer cancel()

	binCount := 0
	var s1DoneAt time.Time
	doneReceived := false
	for !doneReceived {
		mt, raw, rErr := conn.Read(readCtx)
		if rErr != nil {
			t.Fatalf("ws read failed at binCount=%d: %v", binCount, rErr)
		}
		if mt == websocket.MessageBinary {
			binCount++
			if binCount == 140 {
				s1DoneAt = time.Now()
			}
			continue
		}
		var msg map[string]any
		if json.Unmarshal(raw, &msg) == nil && msg["type"] == "agent.done" {
			doneReceived = true
		}
	}

	totalElapsed := time.Since(turnStart)
	if binCount != 280 {
		t.Fatalf("expected all 280 Opus frames before agent.done, got %d", binCount)
	}
	if s1DoneAt.Sub(turnStart) < 300*time.Millisecond {
		t.Fatalf("expected sentence 1 pacing past 120-frame burst (>=300ms), got %v", s1DoneAt.Sub(turnStart))
	}
	if totalElapsed < 2900*time.Millisecond {
		t.Fatalf("expected all 280 frames paced at 1.0x (>=2900ms), got %v", totalElapsed)
	}
	tts.mu.Lock()
	starts := append([]time.Time(nil), tts.synthStarts...)
	tts.mu.Unlock()
	if len(starts) != 2 {
		t.Fatalf("expected 2 synthesized sentences, got %d", len(starts))
	}
	if !starts[1].Before(s1DoneAt) {
		t.Fatalf("expected sentence 2 synthesis (%v) to start before sentence 1 finished pacing (%v)", starts[1], s1DoneAt)
	}
}

func TestOpusBurstPacerRealTimeRate(t *testing.T) {
	if got := (&opusBurstPacer{}).effectiveLead(); got != 2400*time.Millisecond {
		t.Fatalf("expected default effectiveLead 2400ms, got %v", got)
	}
	make20msFrames := func(n int) [][]byte {
		out := make([][]byte, n)
		for i := range out {
			out[i] = []byte{0x68, 0x01, byte(i)}
		}
		return out
	}

	pacer := &opusBurstPacer{lead: 1000 * time.Millisecond}
	ctx := context.Background()
	start := time.Now()
	sent := 0
	var frame50Elapsed time.Duration

	sendFn := func([]byte) error {
		sent++
		if sent == 50 {
			frame50Elapsed = time.Since(start)
		}
		return nil
	}

	if err := pacer.sendAll(ctx, make20msFrames(45), sendFn); err != nil {
		t.Fatalf("batch 1 failed: %v", err)
	}
	if err := pacer.sendAll(ctx, make20msFrames(30), sendFn); err != nil {
		t.Fatalf("batch 2 failed: %v", err)
	}
	totalElapsed := time.Since(start)

	if sent != 75 {
		t.Fatalf("expected 75 sent frames, got %d", sent)
	}
	if frame50Elapsed >= 150*time.Millisecond {
		t.Fatalf("expected first 50 frames (1000ms lead) to burst in <150ms, took %v", frame50Elapsed)
	}
	if totalElapsed < 400*time.Millisecond || totalElapsed >= 900*time.Millisecond {
		t.Fatalf("expected total elapsed in [400ms, 900ms), took %v", totalElapsed)
	}
}

func TestMCPFleetListDefaultEZAssistantACLAndIsError(t *testing.T) {
	ts, macAuth, _, _, cleanup := setupVoiceTestServer(t, "")
	defer cleanup()

	ctx := context.Background()
	conn := dialAuthorizedDevice(t, ctx, ts.URL, macAuth, "30:ed:a0:f3:dc:99", "living", 0, nil)
	defer conn.CloseNow()

	// 1. tools/list includes device.fleet_list.
	listReq, _ := http.NewRequest(http.MethodPost, ts.URL+"/mcp", strings.NewReader(`{"jsonrpc":"2.0","id":1,"method":"tools/list"}`))
	listReq.Header.Set("Content-Type", "application/json")
	listRes, err := http.DefaultClient.Do(listReq)
	if err != nil {
		t.Fatalf("tools/list failed: %v", err)
	}
	var listPayload map[string]any
	_ = json.NewDecoder(listRes.Body).Decode(&listPayload)
	_ = listRes.Body.Close()
	toolsList, _ := listPayload["result"].(map[string]any)["tools"].([]any)
	foundFleet := false
	for _, item := range toolsList {
		if m, ok := item.(map[string]any); ok && m["name"] == "device.fleet_list" {
			foundFleet = true
			break
		}
	}
	if !foundFleet {
		t.Fatalf("expected device.fleet_list in tools/list, got %+v", toolsList)
	}

	callMCP := func(id int, agentID, role, tool string, args map[string]any) map[string]any {
		body, _ := json.Marshal(map[string]any{
			"jsonrpc": "2.0",
			"id":      id,
			"method":  "tools/call",
			"params": map[string]any{
				"name":      tool,
				"arguments": args,
			},
		})
		req, _ := http.NewRequest(http.MethodPost, ts.URL+"/mcp", bytes.NewReader(body))
		req.Header.Set("Content-Type", "application/json")
		req.Header.Set("X-Agent-ID", agentID)
		req.Header.Set("X-Speaker-Role", role)
		res, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatalf("MCP call failed: %v", err)
		}
		var out map[string]any
		_ = json.NewDecoder(res.Body).Decode(&out)
		_ = res.Body.Close()
		return out
	}

	// 2. Default seeded ACL allows ez-assistant + owner without extra Upsert, and denies guest with -32003.
	denied := callMCP(2, "ez-assistant", "guest", "device.fleet_list", map[string]any{})
	errObj, ok := denied["error"].(map[string]any)
	if !ok || int(errObj["code"].(float64)) != -32003 {
		t.Fatalf("expected JSON-RPC -32003 ACL denial for guest, got %+v", denied)
	}

	allowed := callMCP(3, "ez-assistant", "owner", "device.fleet_list", map[string]any{})
	if allowed["error"] != nil {
		t.Fatalf("expected default ez-assistant owner ACL to permit device.fleet_list, got %+v", allowed["error"])
	}
	resultObj := allowed["result"].(map[string]any)
	if isErr, _ := resultObj["isError"].(bool); isErr {
		t.Fatalf("expected valid fleet_list result, got isError: %+v", resultObj)
	}

	// 3. Operational failure (unknown/offline target) for permitted agent returns MCP isError=true result rather than -32003.
	opFail := callMCP(4, "ez-assistant", "owner", "device.camera_capture", map[string]any{"target": "non-existent-room"})
	if opFail["error"] != nil {
		t.Fatalf("expected operational error wrapped as MCP isError result instead of JSON-RPC error, got %+v", opFail)
	}
	failRes := opFail["result"].(map[string]any)
	if isErr, _ := failRes["isError"].(bool); !isErr {
		t.Fatalf("expected isError=true for offline/unknown target, got %+v", failRes)
	}
}
