package bridge

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"esp-pika/agent/ez-bridge/ezruntime"
	agentv1 "esp-pika/backend/gateway/api/agent/v1"
	devicev1 "esp-pika/backend/gateway/api/device/v1"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
)

type mockEZRuntime struct {
	mu           sync.Mutex
	approvalMode string
	cancelledRun string
	lastMessage  string
	lastMCPKey   string
	sseClients   []chan string
}

func (m *mockEZRuntime) broadcastSSE(eventJSON string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, ch := range m.sseClients {
		ch <- eventJSON
	}
}

func (m *mockEZRuntime) handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer test-token" ||
			r.Header.Get("x-ez-client-version") != ezruntime.SoftwareVersion {
			http.Error(w, "unauthorized or incompatible", http.StatusUnauthorized)
			return
		}
		w.WriteHeader(http.StatusOK)
	})
	mux.HandleFunc("/runtime/ensure-ready", func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	})
	mux.HandleFunc("/events", func(w http.ResponseWriter, r *http.Request) {
		flusher, ok := w.(http.Flusher)
		if !ok {
			http.Error(w, "no flusher", http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "text/event-stream")
		_, _ = fmt.Fprint(w, ": connected\n\n")
		flusher.Flush()

		ch := make(chan string, 16)
		m.mu.Lock()
		m.sseClients = append(m.sseClients, ch)
		m.mu.Unlock()

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
				Scope   string `json:"scope"`
				Payload struct {
					Type    string         `json:"type"`
					Payload map[string]any `json:"payload"`
				} `json:"payload"`
			} `json:"command"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}

		var resultPayload any
		switch req.Command.Payload.Type {
		case "create_session":
			resultPayload = map[string]any{"session": map[string]any{"session_id": "ez_sess_1"}}
		case "set_session_approval_mode":
			m.mu.Lock()
			m.approvalMode, _ = req.Command.Payload.Payload["approval_mode"].(string)
			m.mu.Unlock()
			resultPayload = map[string]any{"session": map[string]any{"session_id": "ez_sess_1"}}
		case "submit_input":
			m.mu.Lock()
			m.lastMessage, _ = req.Command.Payload.Payload["message"].(string)
			m.lastMCPKey, _ = req.Command.Payload.Payload["mcp_server_key"].(string)
			m.mu.Unlock()
			resultPayload = map[string]any{
				"input_id": "inp_1",
				"run":      map[string]any{"run_id": "run_1"},
			}
			go func() {
				time.Sleep(15 * time.Millisecond)
				m.broadcastSSE(`{"sequence":1,"emitted_at_ms":1,"event":{"type":"run_started","session_id":"ez_sess_1","run_id":"run_1"}}`)
				m.broadcastSSE(`{"sequence":2,"emitted_at_ms":2,"event":{"type":"tool_started","session_id":"ez_sess_1","run_id":"run_1","step":1,"call_id":"c1","tool_name":"call_mcp_tool"}}`)
				m.broadcastSSE(`{"sequence":3,"emitted_at_ms":3,"event":{"type":"text_delta","session_id":"ez_sess_1","run_id":"run_1","step":1,"part_id":"p1","delta":"门口是快递员。"}}`)
				m.broadcastSSE(`{"sequence":4,"emitted_at_ms":4,"event":{"type":"run_finished","session_id":"ez_sess_1","run_id":"run_1","status":"completed"}}`)
			}()
		case "cancel_run":
			m.mu.Lock()
			m.cancelledRun, _ = req.Command.Payload.Payload["run_id"].(string)
			m.mu.Unlock()
			resultPayload = map[string]any{"run": map[string]any{"run_id": "run_1"}}
		}

		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]any{
			"request_id": req.RequestID,
			"result": map[string]any{
				"scope": "runtime",
				"payload": map[string]any{
					"type":    req.Command.Payload.Type,
					"payload": resultPayload,
				},
			},
		})
	})
	return mux
}

func TestConverseBridgeEndToEnd(t *testing.T) {
	mock := &mockEZRuntime{}
	ts := httptest.NewServer(mock.handler())
	defer ts.Close()

	client, err := ezruntime.NewClient(ezruntime.Config{
		BaseURL:     ts.URL,
		AccessToken: "test-token",
	})
	if err != nil {
		t.Fatalf("new ezruntime client: %v", err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	client.StartEventStream(ctx)
	defer client.StopEventStream()

	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	grpcSrv := grpc.NewServer()
	bridgeSrv := NewServer(client, Options{
		AgentID:      "ez-assistant",
		MCPServerKey: "esp-pika",
		AutoApprove:  true,
	})
	agentv1.RegisterAgentSessionServiceServer(grpcSrv, bridgeSrv)
	go func() { _ = grpcSrv.Serve(lis) }()
	defer grpcSrv.Stop()

	conn, err := grpc.NewClient(lis.Addr().String(), grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatalf("grpc dial: %v", err)
	}
	defer func() { _ = conn.Close() }()

	cli := agentv1.NewAgentSessionServiceClient(conn)
	hResp, err := cli.Health(ctx, &agentv1.HealthRequest{})
	if err != nil || hResp.GetStatus() != "SERVING" {
		t.Fatalf("health failed: resp=%v err=%v", hResp, err)
	}

	stream, err := cli.Converse(ctx)
	if err != nil {
		t.Fatalf("converse open: %v", err)
	}
	if err := stream.Send(&agentv1.GatewayToAgentEvent{
		Payload: &agentv1.GatewayToAgentEvent_SendTurn{
			SendTurn: &agentv1.SendTurn{
				SessionId:      "sess:spk_1:ez-assistant",
				TurnId:         "turn_1",
				AgentId:        "ez-assistant",
				Text:           "帮我看看门口是谁",
				SpeakerId:      "spk_1",
				SpeakerRole:    devicev1.SpeakerRole_SPEAKER_ROLE_OWNER,
				SourceDeviceId: "30:ed:a0:f3:dc:70",
				Zone:           "study",
				GroupId:        "home-default",
			},
		},
	}); err != nil {
		t.Fatalf("send turn: %v", err)
	}
	_ = stream.CloseSend()

	var chunks []string
	var gotDone bool
	for {
		resp, err := stream.Recv()
		if err != nil {
			break
		}
		if td := resp.GetTextDelta(); td != nil {
			chunks = append(chunks, td.GetChunk())
		}
		if done := resp.GetDone(); done != nil {
			gotDone = true
			if done.GetFinishReason() != "completed" {
				t.Fatalf("unexpected finish reason: %s", done.GetFinishReason())
			}
		}
	}

	if !gotDone || strings.Join(chunks, "") != "门口是快递员。" {
		t.Fatalf("unexpected bridge output chunks=%v gotDone=%v", chunks, gotDone)
	}
	if mock.approvalMode != "auto" || mock.lastMCPKey != "esp-pika" {
		t.Fatalf("expected auto approval and esp-pika mcp key, got mode=%s key=%s", mock.approvalMode, mock.lastMCPKey)
	}
	if !strings.Contains(mock.lastMessage, "spk_1") || !strings.Contains(mock.lastMessage, "帮我看看门口是谁") {
		t.Fatalf("missing formatted context in submitted message: %s", mock.lastMessage)
	}
}
