package deepseek

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	agentv1 "esp-pika/backend/gateway/api/agent/v1"
	devicev1 "esp-pika/backend/gateway/api/device/v1"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/test/bufconn"
)

func startBufconnServer(t *testing.T, srv *Server) agentv1.AgentSessionServiceClient {
	t.Helper()
	lis := bufconn.Listen(1 << 20)
	gs := grpc.NewServer()
	agentv1.RegisterAgentSessionServiceServer(gs, srv)
	go func() {
		_ = gs.Serve(lis)
	}()
	t.Cleanup(func() {
		gs.GracefulStop()
		_ = lis.Close()
	})

	conn, err := grpc.NewClient(
		"passthrough:///bufconn",
		grpc.WithContextDialer(func(context.Context, string) (net.Conn, error) {
			return lis.Dial()
		}),
		grpc.WithTransportCredentials(insecure.NewCredentials()),
	)
	if err != nil {
		t.Fatalf("grpc dial: %v", err)
	}
	t.Cleanup(func() { _ = conn.Close() })
	return agentv1.NewAgentSessionServiceClient(conn)
}

func TestDeepSeekStreamingWithMCPToolCalling(t *testing.T) {
	var mcpCalled atomic.Bool
	var seenRole string
	var seenAgent string

	mcpSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var req struct {
			ID     any            `json:"id"`
			Method string         `json:"method"`
			Params map[string]any `json:"params"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			t.Fatalf("decode mcp req: %v", err)
		}
		w.Header().Set("Content-Type", "application/json")
		switch req.Method {
		case "tools/list":
			_ = json.NewEncoder(w).Encode(map[string]any{
				"jsonrpc": "2.0",
				"id":      req.ID,
				"result": map[string]any{
					"tools": []map[string]any{{
						"name":        "device.display_show",
						"description": "Show text on device AMOLED",
						"inputSchema": map[string]any{
							"type": "object",
							"properties": map[string]any{
								"target": map[string]any{"type": "string"},
								"text":   map[string]any{"type": "string"},
							},
						},
					}},
				},
			})
		case "tools/call":
			seenAgent = r.Header.Get("X-Agent-ID")
			seenRole = r.Header.Get("X-Speaker-Role")
			if name, _ := req.Params["name"].(string); name != "device.display_show" {
				t.Fatalf("expected original MCP tool name device.display_show, got %q", name)
			}
			mcpCalled.Store(true)
			_ = json.NewEncoder(w).Encode(map[string]any{
				"jsonrpc": "2.0",
				"id":      req.ID,
				"result": map[string]any{
					"content": []map[string]any{{"type": "text", "text": `{"ok":true}`}},
					"isError": false,
				},
			})
		}
	}))
	defer mcpSrv.Close()

	var dsRound atomic.Int32
	dsSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/chat/completions" {
			t.Fatalf("unexpected path %s", r.URL.Path)
		}
		if got := r.Header.Get("Authorization"); got != "Bearer sk-test-key" {
			t.Fatalf("unexpected auth header %q", got)
		}
		var req chatCompletionRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			t.Fatalf("decode deepseek req: %v", err)
		}
		if req.Model != "deepseek-chat" || !req.Stream {
			t.Fatalf("unexpected model=%q stream=%v", req.Model, req.Stream)
		}
		if len(req.Tools) != 1 || req.Tools[0].Function.Name != "device__display_show" {
			t.Fatalf("expected sanitized tool device__display_show, got %+v", req.Tools)
		}

		w.Header().Set("Content-Type", "text/event-stream")
		round := dsRound.Add(1)
		if round == 1 {
			_, _ = fmt.Fprintln(w, `data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_abc","type":"function","function":{"name":"device__display_show","arguments":"{\"target\":\"study\","}}]}}]}`)
			_, _ = fmt.Fprintln(w, `data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"text\":\"你好\"}"}}]},"finish_reason":"tool_calls"}]}`)
			_, _ = fmt.Fprintln(w, `data: [DONE]`)
			return
		}
		// Second round must include the tool result message.
		hasToolMsg := false
		for _, m := range req.Messages {
			if m.Role == "tool" && m.ToolCallID == "call_abc" && strings.Contains(m.Content, `"ok":true`) {
				hasToolMsg = true
			}
		}
		if !hasToolMsg {
			t.Fatalf("expected tool result in second round messages: %+v", req.Messages)
		}
		_, _ = fmt.Fprintln(w, `data: {"choices":[{"delta":{"content":"好的，"}}]} `)
		_, _ = fmt.Fprintln(w, `data: {"choices":[{"delta":{"content":"已显示你好。"},"finish_reason":"stop"}]}`)
		_, _ = fmt.Fprintln(w, `data: [DONE]`)
	}))
	defer dsSrv.Close()

	srv, err := NewServer(Config{
		APIKey:        "sk-test-key",
		BaseURL:       dsSrv.URL,
		Model:         "deepseek-chat",
		GatewayMCPURL: mcpSrv.URL,
		AgentID:       "ez-assistant",
	})
	if err != nil {
		t.Fatalf("NewServer: %v", err)
	}
	client := startBufconnServer(t, srv)

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	hResp, err := client.Health(ctx, &agentv1.HealthRequest{})
	if err != nil || hResp.GetStatus() != "SERVING" {
		t.Fatalf("Health failed: resp=%v err=%v", hResp, err)
	}

	stream, err := client.Converse(ctx)
	if err != nil {
		t.Fatalf("Converse: %v", err)
	}
	if err := stream.Send(&agentv1.GatewayToAgentEvent{
		Payload: &agentv1.GatewayToAgentEvent_SendTurn{
			SendTurn: &agentv1.SendTurn{
				SessionId:      "sess-1",
				TurnId:         "turn-1",
				AgentId:        "ez-assistant",
				Text:           "在书房屏幕上显示你好",
				SpeakerRole:    devicev1.SpeakerRole_SPEAKER_ROLE_OWNER,
				SourceDeviceId: "30:ed:a0:f4:5b:f0",
				Zone:           "study",
			},
		},
	}); err != nil {
		t.Fatalf("SendTurn: %v", err)
	}
	_ = stream.CloseSend()

	var text strings.Builder
	var statuses []string
	var doneReason string
	for {
		ev, err := stream.Recv()
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatalf("Recv: %v", err)
		}
		switch p := ev.Payload.(type) {
		case *agentv1.AgentToGatewayEvent_Status:
			statuses = append(statuses, p.Status.GetState())
		case *agentv1.AgentToGatewayEvent_TextDelta:
			text.WriteString(p.TextDelta.GetChunk())
		case *agentv1.AgentToGatewayEvent_Done:
			doneReason = p.Done.GetFinishReason()
		}
	}

	if !mcpCalled.Load() || seenAgent != "ez-assistant" || seenRole != "owner" {
		t.Fatalf("unexpected MCP call state called=%v agent=%q role=%q", mcpCalled.Load(), seenAgent, seenRole)
	}
	if text.String() != "好的，已显示你好。" {
		t.Fatalf("unexpected text %q", text.String())
	}
	if doneReason != "completed" {
		t.Fatalf("unexpected done reason %q", doneReason)
	}
	if len(statuses) < 2 || statuses[0] != "thinking" || statuses[1] != "calling_tool" {
		t.Fatalf("unexpected statuses %v", statuses)
	}
}

func TestDeepSeekCancelTurn(t *testing.T) {
	started := make(chan struct{})
	dsSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		_, _ = fmt.Fprintln(w, `data: {"choices":[{"delta":{"content":"稍等"}}]}`)
		if f, ok := w.(http.Flusher); ok {
			f.Flush()
		}
		close(started)
		<-r.Context().Done()
	}))
	defer dsSrv.Close()

	srv, err := NewServer(Config{
		APIKey:  "sk-test-key",
		BaseURL: dsSrv.URL,
	})
	if err != nil {
		t.Fatalf("NewServer: %v", err)
	}
	client := startBufconnServer(t, srv)

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	turnStream, err := client.Converse(ctx)
	if err != nil {
		t.Fatalf("Converse: %v", err)
	}
	if err := turnStream.Send(&agentv1.GatewayToAgentEvent{
		Payload: &agentv1.GatewayToAgentEvent_SendTurn{
			SendTurn: &agentv1.SendTurn{
				SessionId: "sess-cancel",
				TurnId:    "turn-cancel",
				Text:      "讲个长故事",
			},
		},
	}); err != nil {
		t.Fatalf("SendTurn: %v", err)
	}
	_ = turnStream.CloseSend()

	select {
	case <-started:
	case <-time.After(3 * time.Second):
		t.Fatal("timed out waiting for DeepSeek mock HTTP request")
	}

	cancelStream, err := client.Converse(ctx)
	if err != nil {
		t.Fatalf("Converse cancel: %v", err)
	}
	if err := cancelStream.Send(&agentv1.GatewayToAgentEvent{
		Payload: &agentv1.GatewayToAgentEvent_Cancel{
			Cancel: &agentv1.CancelTurn{
				SessionId: "sess-cancel",
				TurnId:    "turn-cancel",
				Reason:    "barge_in",
			},
		},
	}); err != nil {
		t.Fatalf("Send cancel: %v", err)
	}
	_ = cancelStream.CloseSend()

	var doneReason string
	for {
		ev, err := turnStream.Recv()
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatalf("Recv: %v", err)
		}
		if d := ev.GetDone(); d != nil {
			doneReason = d.GetFinishReason()
		}
	}
	if doneReason != "cancelled" {
		t.Fatalf("expected cancelled turn, got %q", doneReason)
	}
}
