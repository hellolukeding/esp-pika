package bridge

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"esp-pika/agent/ez-bridge/ezruntime"
	agentv1 "esp-pika/backend/gateway/api/agent/v1"

	"github.com/coder/websocket"
	"google.golang.org/grpc"
)

func reservePort(t *testing.T) int {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("reserve port: %v", err)
	}
	port := l.Addr().(*net.TCPAddr).Port
	_ = l.Close()
	return port
}

func TestFullFleetGatewayBridgeAndMCPRoundTrip(t *testing.T) {
	gwBin, err := filepath.Abs("../../../backend/gateway/bin/gateway")
	if err != nil {
		t.Fatalf("resolve gateway binary: %v", err)
	}
	if _, err := os.Stat(gwBin); err != nil {
		t.Skipf("gateway binary %s not built yet: %v", gwBin, err)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 12*time.Second)
	defer cancel()

	gwHTTPPort := reservePort(t)
	gwGRPCPort := reservePort(t)
	gatewayURL := fmt.Sprintf("http://127.0.0.1:%d", gwHTTPPort)

	// 1. Mock ez-assistant-runtime that calls real Gateway /mcp during turn execution.
	mock := &mockEZRuntime{}
	mux := http.NewServeMux()
	mux.HandleFunc("/health", func(w http.ResponseWriter, _ *http.Request) { w.WriteHeader(http.StatusOK) })
	mux.HandleFunc("/runtime/ensure-ready", func(w http.ResponseWriter, _ *http.Request) { w.WriteHeader(http.StatusNoContent) })
	mux.HandleFunc("/events", func(w http.ResponseWriter, r *http.Request) {
		flusher := w.(http.Flusher)
		w.Header().Set("Content-Type", "text/event-stream")
		_, _ = fmt.Fprint(w, ": connected\n\n")
		flusher.Flush()
		ch := make(chan string, 16)
		mock.mu.Lock()
		mock.sseClients = append(mock.sseClients, ch)
		mock.mu.Unlock()
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
					Type string `json:"type"`
				} `json:"payload"`
			} `json:"command"`
		}
		_ = json.NewDecoder(r.Body).Decode(&req)
		var resPayload any
		switch req.Command.Payload.Type {
		case "create_session":
			resPayload = map[string]any{"session": map[string]any{"session_id": "ez_e2e_1"}}
		case "set_session_approval_mode":
			resPayload = map[string]any{"session": map[string]any{"session_id": "ez_e2e_1"}}
		case "submit_input":
			resPayload = map[string]any{"input_id": "inp_e2e", "run": map[string]any{"run_id": "run_e2e"}}
			go func() {
				mock.broadcastSSE(`{"sequence":1,"emitted_at_ms":1,"event":{"type":"run_started","session_id":"ez_e2e_1","run_id":"run_e2e"}}`)
				mock.broadcastSSE(`{"sequence":2,"emitted_at_ms":2,"event":{"type":"tool_started","session_id":"ez_e2e_1","run_id":"run_e2e","step":1,"call_id":"call_1","tool_name":"device.camera_capture"}}`)

				mcpReq, _ := http.NewRequestWithContext(ctx, http.MethodPost, gatewayURL+"/mcp",
					strings.NewReader(`{"jsonrpc":"2.0","id":99,"method":"tools/call","params":{"name":"device.camera_capture","arguments":{"target":"entrance"}}}`))
				mcpReq.Header.Set("Content-Type", "application/json")
				mcpReq.Header.Set("X-Agent-ID", "ez-assistant")
				mcpReq.Header.Set("X-Speaker-Role", "owner")
				resp, err := http.DefaultClient.Do(mcpReq)
				reply := "门口拍照失败。"
				if err == nil {
					var rpcOut map[string]any
					_ = json.NewDecoder(resp.Body).Decode(&rpcOut)
					_ = resp.Body.Close()
					raw, _ := json.Marshal(rpcOut)
					if strings.Contains(string(raw), ".jpg") {
						reply = "门口拍好了，是快递员。"
					}
				}

				deltaJSON, _ := json.Marshal(map[string]any{
					"sequence":      3,
					"emitted_at_ms": 3,
					"event": map[string]any{
						"type":       "text_delta",
						"session_id": "ez_e2e_1",
						"run_id":     "run_e2e",
						"step":       1,
						"part_id":    "p1",
						"delta":      reply,
					},
				})
				mock.broadcastSSE(string(deltaJSON))
				mock.broadcastSSE(`{"sequence":4,"emitted_at_ms":4,"event":{"type":"run_finished","session_id":"ez_e2e_1","run_id":"run_e2e","status":"completed"}}`)
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
	ezTS := httptest.NewServer(mux)
	defer ezTS.Close()

	// 2. Start real ez-bridge gRPC server.
	ezClient, err := ezruntime.NewClient(ezruntime.Config{BaseURL: ezTS.URL, AccessToken: "tok"})
	if err != nil {
		t.Fatalf("new ezruntime client: %v", err)
	}
	ezClient.StartEventStream(ctx)
	defer ezClient.StopEventStream()

	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	grpcSrv := grpc.NewServer()
	agentv1.RegisterAgentSessionServiceServer(grpcSrv, NewServer(ezClient, Options{
		AgentID:      "ez-assistant",
		MCPServerKey: "esp-pika",
		AutoApprove:  true,
	}))
	go func() { _ = grpcSrv.Serve(lis) }()
	defer grpcSrv.Stop()

	// 3. Write temp gateway config and launch real compiled Gateway process.
	tmpDir := t.TempDir()
	cfgPath := filepath.Join(tmpDir, "config.yaml")
	cfgYAML := fmt.Sprintf(`server:
  http:
    network: tcp
    addr: 127.0.0.1:%d
    timeout: 15s
  grpc:
    network: tcp
    addr: 127.0.0.1:%d
    timeout: 15s
  media_dir: %s
data:
  database:
    driver: sqlite3
    source: %s
discovery:
  mdns_enabled: false
arbiter:
  window: 0.05s
voice:
  agent_grpc_addr: %s
  asr_provider: mock
  tts_provider: mock
  default_tts_voice: zh_female_calm
`, gwHTTPPort, gwGRPCPort, filepath.Join(tmpDir, "media"), filepath.Join(tmpDir, "gw.db"), lis.Addr().String())
	if err := os.WriteFile(cfgPath, []byte(cfgYAML), 0o600); err != nil {
		t.Fatalf("write temp config: %v", err)
	}

	gwCmd := exec.CommandContext(ctx, gwBin, "-conf", cfgPath)
	if err := gwCmd.Start(); err != nil {
		t.Fatalf("start gateway: %v", err)
	}
	defer func() {
		if gwCmd.Process != nil {
			_ = gwCmd.Process.Kill()
			_ = gwCmd.Wait()
		}
	}()

	// Wait for /healthz.
	ready := false
	for i := 0; i < 40; i++ {
		resp, err := http.Get(gatewayURL + "/healthz")
		if err == nil {
			_ = resp.Body.Close()
			if resp.StatusCode == http.StatusOK {
				ready = true
				break
			}
		}
		time.Sleep(50 * time.Millisecond)
	}
	if !ready {
		t.Fatalf("gateway failed to become ready at %s", gatewayURL)
	}

	// 4. Register allowed MACs & ACL rule via Kratos REST API.
	putJSON := func(path string, body any) {
		b, _ := json.Marshal(body)
		req, _ := http.NewRequestWithContext(ctx, http.MethodPut, gatewayURL+path, bytes.NewReader(b))
		req.Header.Set("Content-Type", "application/json")
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatalf("PUT %s failed: %v", path, err)
		}
		_ = resp.Body.Close()
		if resp.StatusCode >= 300 {
			t.Fatalf("PUT %s HTTP %d", path, resp.StatusCode)
		}
	}
	studyMAC := "30:ed:a0:f3:dc:70"
	entranceMAC := "30:ed:a0:f3:dc:72"
	putJSON("/api/v1/macs/"+studyMAC, map[string]any{"group_id": "home-default", "zone": "study", "enabled": true})
	putJSON("/api/v1/macs/"+entranceMAC, map[string]any{"group_id": "home-default", "zone": "entrance", "enabled": true})
	putJSON("/api/v1/acls/allow-ez", map[string]any{
		"agent_id": "ez-assistant", "group_id": "home-default",
		"zone_pattern": "*", "capability_pattern": "device.*", "min_speaker_role": 1, "allow": true,
	})

	// 5. Connect simulated Study + Entrance ESP32-S31 WebSocket devices.
	wsBase := fmt.Sprintf("ws://127.0.0.1:%d/ws/device", gwHTTPPort)
	connectDev := func(mac, zone string) *websocket.Conn {
		c, _, err := websocket.Dial(ctx, wsBase, &websocket.DialOptions{HTTPHeader: http.Header{"X-Device-MAC": []string{mac}}})
		if err != nil {
			t.Fatalf("dial %s: %v", zone, err)
		}
		hello, _ := json.Marshal(map[string]any{"type": "device.hello", "mac": mac, "group_id": "home-default", "zone": zone, "chip": "esp32s31"})
		_ = c.Write(ctx, websocket.MessageText, hello)
		_, _, _ = c.Read(ctx) // device.welcome
		return c
	}
	studyConn := connectDev(studyMAC, "study")
	defer studyConn.CloseNow()
	entranceConn := connectDev(entranceMAC, "entrance")
	defer entranceConn.CloseNow()

	go func() {
		for {
			_, msg, err := entranceConn.Read(ctx)
			if err != nil {
				return
			}
			var env map[string]any
			if json.Unmarshal(msg, &env) == nil && env["type"] == "device.command" {
				reqUp, _ := http.NewRequestWithContext(ctx, http.MethodPost, gatewayURL+"/api/v1/media/upload", bytes.NewReader([]byte{0xff, 0xd8, 0xff, 0xd9}))
				reqUp.Header.Set("X-Device-MAC", entranceMAC)
				upResp, _ := http.DefaultClient.Do(reqUp)
				var upJSON map[string]any
				_ = json.NewDecoder(upResp.Body).Decode(&upJSON)
				_ = upResp.Body.Close()

				ack, _ := json.Marshal(map[string]any{
					"type":       "command.result",
					"command_id": env["command_id"],
					"ok":         true,
					"media_url":  upJSON["media_url"],
				})
				_ = entranceConn.Write(ctx, websocket.MessageText, ack)
			}
		}
	}()

	turnMsg, _ := json.Marshal(map[string]any{
		"type":         "text.turn",
		"turn_id":      "turn_cross_room",
		"text":         "帮我看看门口是谁",
		"speaker_id":   "spk_luke",
		"speaker_role": 1,
		"agent_id":     "ez-assistant",
	})
	if err := studyConn.Write(ctx, websocket.MessageText, turnMsg); err != nil {
		t.Fatalf("study write turn: %v", err)
	}

	var gotText string
	var gotAudio bool
	var gotDone bool
	for !gotDone {
		mt, data, err := studyConn.Read(ctx)
		if err != nil {
			t.Fatalf("study read: %v", err)
		}
		if mt == websocket.MessageBinary {
			if strings.Contains(string(data), "门口拍好了") {
				gotAudio = true
			}
			continue
		}
		var env map[string]any
		_ = json.Unmarshal(data, &env)
		switch env["type"] {
		case "agent.text_delta":
			gotText += env["chunk"].(string)
		case "agent.done":
			gotDone = true
		}
	}

	if !gotAudio || gotText != "门口拍好了，是快递员。" {
		t.Fatalf("unexpected cross-room result: gotAudio=%v gotText=%q", gotAudio, gotText)
	}
}
