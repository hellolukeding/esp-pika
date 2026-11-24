package deepseek

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"sync"
	"time"

	agentv1 "esp-pika/backend/gateway/api/agent/v1"
	devicev1 "esp-pika/backend/gateway/api/device/v1"
)

const (
	defaultBaseURL        = "https://api.deepseek.com"
	defaultModel          = "deepseek-flash"
	defaultMaxToolRounds  = 6
	defaultMaxHistoryMsgs = 20
	defaultSystemPrompt   = "You are Pika, a concise, helpful smart-home and voice AI assistant running on ESP32-S31 terminals. " +
		"Reply in natural spoken language matching the user's language (avoid markdown tables or raw JSON unless asked). " +
		"When the user asks about connected devices or wants to control hardware (display, speaker volume, sensors, actuators, camera), call the available device MCP tools using the target device_id or zone from the turn context."
)

// Config configures the direct DeepSeek Chat Completions AgentSessionService server.
type Config struct {
	APIKey        string
	BaseURL       string
	Model         string
	GatewayMCPURL string
	AgentID       string
	SystemPrompt  string
	HTTPClient    *http.Client
}

type chatMessage struct {
	Role       string     `json:"role"`
	Content    string     `json:"content,omitempty"`
	ToolCalls  []toolCall `json:"tool_calls,omitempty"`
	ToolCallID string     `json:"tool_call_id,omitempty"`
}

type toolCall struct {
	ID       string           `json:"id"`
	Type     string           `json:"type"`
	Function toolCallFunction `json:"function"`
}

type toolCallFunction struct {
	Name      string `json:"name"`
	Arguments string `json:"arguments"`
}

type chatToolDef struct {
	Type     string          `json:"type"`
	Function chatToolFuncDef `json:"function"`
}

type chatToolFuncDef struct {
	Name        string         `json:"name"`
	Description string         `json:"description,omitempty"`
	Parameters  map[string]any `json:"parameters,omitempty"`
}

type chatCompletionRequest struct {
	Model    string        `json:"model"`
	Messages []chatMessage `json:"messages"`
	Tools    []chatToolDef `json:"tools,omitempty"`
	Stream   bool          `json:"stream"`
}

// Server implements `AgentSessionService` directly against DeepSeek `/chat/completions`
// and executes hardware/fleet tool calls over Gateway `/mcp`.
type Server struct {
	agentv1.UnimplementedAgentSessionServiceServer

	cfg        Config
	httpClient *http.Client

	mu       sync.Mutex
	sessions map[string][]chatMessage
	runs     map[string]context.CancelFunc
}

// NewServer constructs a direct DeepSeek AgentSessionService server.
func NewServer(cfg Config) (*Server, error) {
	cfg.APIKey = strings.TrimSpace(cfg.APIKey)
	if strings.TrimSpace(cfg.BaseURL) == "" {
		cfg.BaseURL = defaultBaseURL
	}
	cfg.BaseURL = strings.TrimRight(cfg.BaseURL, "/")
	if strings.TrimSpace(cfg.Model) == "" {
		cfg.Model = defaultModel
	}
	if strings.TrimSpace(cfg.AgentID) == "" {
		cfg.AgentID = "ez-assistant"
	}
	if strings.TrimSpace(cfg.SystemPrompt) == "" {
		cfg.SystemPrompt = defaultSystemPrompt
	}
	hc := cfg.HTTPClient
	if hc == nil {
		hc = &http.Client{Timeout: 90 * time.Second}
	}
	return &Server{
		cfg:        cfg,
		httpClient: hc,
		sessions:   make(map[string][]chatMessage),
		runs:       make(map[string]context.CancelFunc),
	}, nil
}

// Health verifies configuration readiness and reports serving status for the DeepSeek agent.
func (s *Server) Health(_ context.Context, _ *agentv1.HealthRequest) (*agentv1.HealthResponse, error) {
	return &agentv1.HealthResponse{
		Status:  "SERVING",
		AgentId: s.cfg.AgentID,
	}, nil
}

// Converse handles bidirectional gateway <-> DeepSeek streaming turns, device push events, and cancellations.
func (s *Server) Converse(stream agentv1.AgentSessionService_ConverseServer) error {
	for {
		ev, err := stream.Recv()
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return err
		}

		switch p := ev.Payload.(type) {
		case *agentv1.GatewayToAgentEvent_SendTurn:
			if err := s.handleTurn(stream.Context(), stream, p.SendTurn); err != nil {
				return err
			}
		case *agentv1.GatewayToAgentEvent_SendEvent:
			if err := s.handleDeviceEvent(stream.Context(), stream, p.SendEvent); err != nil {
				return err
			}
		case *agentv1.GatewayToAgentEvent_Cancel:
			s.handleCancel(p.Cancel)
		}
	}
}

func (s *Server) handleTurn(ctx context.Context, stream agentv1.AgentSessionService_ConverseServer, turn *agentv1.SendTurn) error {
	if turn == nil {
		return nil
	}
	agentID := turn.GetAgentId()
	if agentID == "" {
		agentID = s.cfg.AgentID
	}
	return s.executePrompt(ctx, stream, turn.GetSessionId(), turn.GetTurnId(), agentID, turn.GetSpeakerRole(), turn.GetSystemPrompt(), turn.GetApiKey(), turn.GetModel(), formatTurnPrompt(turn))
}

func (s *Server) handleDeviceEvent(ctx context.Context, stream agentv1.AgentSessionService_ConverseServer, devEvt *agentv1.SendEvent) error {
	if devEvt == nil {
		return nil
	}
	agentID := devEvt.GetAgentId()
	if agentID == "" {
		agentID = s.cfg.AgentID
	}
	prompt := fmt.Sprintf("[Proactive Device Event]\nevent_type: %s\nsource_device_id: %s\nzone: %s\ngroup_id: %s\npayload_json: %s\nDecide whether to inspect via MCP tools or alert the user briefly in spoken language.",
		devEvt.GetEventType(), devEvt.GetSourceDeviceId(), devEvt.GetZone(), devEvt.GetGroupId(), devEvt.GetPayloadJson())
	return s.executePrompt(ctx, stream, devEvt.GetSessionId(), "", agentID, devicev1.SpeakerRole_SPEAKER_ROLE_OWNER, "", "", "", prompt)
}

func (s *Server) handleCancel(cancel *agentv1.CancelTurn) {
	if cancel == nil {
		return
	}
	key := cancel.GetSessionId() + ":" + cancel.GetTurnId()
	s.mu.Lock()
	fn := s.runs[key]
	s.mu.Unlock()
	if fn != nil {
		fn()
	}
}

func (s *Server) executePrompt(
	ctx context.Context,
	stream agentv1.AgentSessionService_ConverseServer,
	sessionID string,
	turnID string,
	agentID string,
	speakerRole devicev1.SpeakerRole,
	systemPrompt string,
	apiKey string,
	model string,
	userPrompt string,
) error {
	runCtx, cancel := context.WithCancel(ctx)
	runKey := sessionID + ":" + turnID
	s.mu.Lock()
	s.runs[runKey] = cancel
	s.mu.Unlock()
	defer func() {
		cancel()
		s.mu.Lock()
		delete(s.runs, runKey)
		s.mu.Unlock()
	}()

	_ = sendStatus(stream, sessionID, turnID, "thinking", "正在思考")

	tools, nameMap := s.fetchMCPTools(runCtx, agentID)
	history := s.snapshotSession(sessionID, systemPrompt)
	history = append(history, chatMessage{Role: "user", Content: userPrompt})

	for round := 0; round < defaultMaxToolRounds; round++ {
		assistantMsg, err := s.streamCompletionRound(runCtx, stream, sessionID, turnID, apiKey, model, history, tools)
		if err != nil {
			if errors.Is(err, context.Canceled) {
				return stream.Send(makeDoneEvent(sessionID, turnID, "cancelled", err.Error()))
			}
			return stream.Send(makeDoneEvent(sessionID, turnID, "failed", err.Error()))
		}
		history = append(history, assistantMsg)

		if len(assistantMsg.ToolCalls) == 0 {
			s.saveSession(sessionID, history)
			return stream.Send(makeDoneEvent(sessionID, turnID, "completed", ""))
		}

		for _, tc := range assistantMsg.ToolCalls {
			mcpName := tc.Function.Name
			if orig, ok := nameMap[mcpName]; ok {
				mcpName = orig
			}
			_ = sendStatus(stream, sessionID, turnID, "calling_tool", mcpName)
			toolOut := s.callMCPTool(runCtx, agentID, speakerRole, mcpName, tc.Function.Arguments)
			history = append(history, chatMessage{
				Role:       "tool",
				ToolCallID: tc.ID,
				Content:    toolOut,
			})
		}
	}

	s.saveSession(sessionID, history)
	return stream.Send(makeDoneEvent(sessionID, turnID, "completed", ""))
}

func (s *Server) snapshotSession(sessionID, systemPrompt string) []chatMessage {
	sys := strings.TrimSpace(systemPrompt)
	if sys == "" {
		sys = s.cfg.SystemPrompt
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	existing := s.sessions[sessionID]
	out := make([]chatMessage, 0, len(existing)+1)
	out = append(out, chatMessage{Role: "system", Content: sys})
	out = append(out, existing...)
	return out
}

func (s *Server) saveSession(sessionID string, full []chatMessage) {
	if len(full) == 0 {
		return
	}
	nonSystem := make([]chatMessage, 0, len(full))
	for _, m := range full {
		if m.Role == "system" {
			continue
		}
		nonSystem = append(nonSystem, m)
	}
	if len(nonSystem) > defaultMaxHistoryMsgs {
		start := len(nonSystem) - defaultMaxHistoryMsgs
		// Keep tool/assistant pairs aligned by advancing past orphaned tool messages.
		for start < len(nonSystem) && nonSystem[start].Role == "tool" {
			start++
		}
		nonSystem = nonSystem[start:]
	}
	s.mu.Lock()
	s.sessions[sessionID] = nonSystem
	s.mu.Unlock()
}

func (s *Server) streamCompletionRound(
	ctx context.Context,
	stream agentv1.AgentSessionService_ConverseServer,
	sessionID string,
	turnID string,
	apiKey string,
	model string,
	messages []chatMessage,
	tools []chatToolDef,
) (chatMessage, error) {
	effectiveKey := strings.TrimSpace(apiKey)
	if effectiveKey == "" {
		effectiveKey = s.cfg.APIKey
	}
	if effectiveKey == "" {
		return chatMessage{}, fmt.Errorf("deepseek api_key is not configured (set it in Web Console or DEEPSEEK_API_KEY)")
	}
	effectiveModel := strings.TrimSpace(model)
	if effectiveModel == "" {
		effectiveModel = s.cfg.Model
	}
	reqBody := chatCompletionRequest{
		Model:    effectiveModel,
		Messages: messages,
		Tools:    tools,
		Stream:   true,
	}
	raw, err := json.Marshal(reqBody)
	if err != nil {
		return chatMessage{}, fmt.Errorf("marshal deepseek request: %w", err)
	}

	endpoint := s.cfg.BaseURL + "/chat/completions"
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, endpoint, bytes.NewReader(raw))
	if err != nil {
		return chatMessage{}, fmt.Errorf("build deepseek request: %w", err)
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Authorization", "Bearer "+effectiveKey)

	resp, err := s.httpClient.Do(req)
	if err != nil {
		return chatMessage{}, fmt.Errorf("deepseek request: %w", err)
	}
	defer func() { _ = resp.Body.Close() }()

	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		errBody, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return chatMessage{}, fmt.Errorf("deepseek status %d: %s", resp.StatusCode, strings.TrimSpace(string(errBody)))
	}

	return readSSECompletion(resp.Body, stream, sessionID, turnID)
}

type sseChunk struct {
	Choices []struct {
		Delta struct {
			Content          string `json:"content"`
			ReasoningContent string `json:"reasoning_content"`
			ToolCalls        []struct {
				Index    int    `json:"index"`
				ID       string `json:"id"`
				Type     string `json:"type"`
				Function struct {
					Name      string `json:"name"`
					Arguments string `json:"arguments"`
				} `json:"function"`
			} `json:"tool_calls"`
		} `json:"delta"`
		FinishReason string `json:"finish_reason"`
	} `json:"choices"`
	Error *struct {
		Message string `json:"message"`
		Code    any    `json:"code"`
	} `json:"error"`
}

func readSSECompletion(
	body io.Reader,
	stream agentv1.AgentSessionService_ConverseServer,
	sessionID string,
	turnID string,
) (chatMessage, error) {
	scanner := bufio.NewScanner(body)
	scanner.Buffer(make([]byte, 0, 64*1024), 1<<20)

	var textBuf strings.Builder
	callsByIdx := make(map[int]*toolCall)
	maxIdx := -1

	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, ":") {
			continue
		}
		if !strings.HasPrefix(line, "data:") {
			continue
		}
		payload := strings.TrimSpace(strings.TrimPrefix(line, "data:"))
		if payload == "[DONE]" {
			break
		}

		var chunk sseChunk
		if err := json.Unmarshal([]byte(payload), &chunk); err != nil {
			continue
		}
		if chunk.Error != nil && chunk.Error.Message != "" {
			return chatMessage{}, fmt.Errorf("deepseek stream error: %s", chunk.Error.Message)
		}
		if len(chunk.Choices) == 0 {
			continue
		}
		delta := chunk.Choices[0].Delta
		if delta.Content != "" {
			textBuf.WriteString(delta.Content)
			if err := stream.Send(&agentv1.AgentToGatewayEvent{
				Payload: &agentv1.AgentToGatewayEvent_TextDelta{
					TextDelta: &agentv1.TextDelta{
						SessionId: sessionID,
						TurnId:    turnID,
						Chunk:     delta.Content,
					},
				},
			}); err != nil {
				return chatMessage{}, err
			}
		}
		for _, tc := range delta.ToolCalls {
			item, ok := callsByIdx[tc.Index]
			if !ok {
				item = &toolCall{Type: "function"}
				callsByIdx[tc.Index] = item
				if tc.Index > maxIdx {
					maxIdx = tc.Index
				}
			}
			if tc.ID != "" {
				item.ID += tc.ID
			}
			if tc.Type != "" {
				item.Type = tc.Type
			}
			if tc.Function.Name != "" {
				item.Function.Name += tc.Function.Name
			}
			if tc.Function.Arguments != "" {
				item.Function.Arguments += tc.Function.Arguments
			}
		}
	}
	if err := scanner.Err(); err != nil {
		return chatMessage{}, fmt.Errorf("read deepseek sse: %w", err)
	}

	var calls []toolCall
	for i := 0; i <= maxIdx; i++ {
		if c, ok := callsByIdx[i]; ok && c.Function.Name != "" {
			if c.ID == "" {
				c.ID = fmt.Sprintf("call_%d", i)
			}
			calls = append(calls, *c)
		}
	}

	return chatMessage{
		Role:      "assistant",
		Content:   textBuf.String(),
		ToolCalls: calls,
	}, nil
}

func sanitizeToolName(mcpName string) string {
	return strings.ReplaceAll(mcpName, ".", "__")
}

func (s *Server) fetchMCPTools(ctx context.Context, agentID string) ([]chatToolDef, map[string]string) {
	if strings.TrimSpace(s.cfg.GatewayMCPURL) == "" {
		return nil, nil
	}
	rpcReq := map[string]any{
		"jsonrpc": "2.0",
		"id":      1,
		"method":  "tools/list",
	}
	raw, _ := json.Marshal(rpcReq)
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, s.cfg.GatewayMCPURL, bytes.NewReader(raw))
	if err != nil {
		return nil, nil
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-Agent-ID", agentID)

	resp, err := s.httpClient.Do(req)
	if err != nil {
		return nil, nil
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, nil
	}

	var rpcResp struct {
		Result struct {
			Tools []struct {
				Name        string         `json:"name"`
				Description string         `json:"description"`
				InputSchema map[string]any `json:"inputSchema"`
			} `json:"tools"`
		} `json:"result"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&rpcResp); err != nil {
		return nil, nil
	}

	defs := make([]chatToolDef, 0, len(rpcResp.Result.Tools))
	nameMap := make(map[string]string, len(rpcResp.Result.Tools))
	for _, t := range rpcResp.Result.Tools {
		fnName := sanitizeToolName(t.Name)
		nameMap[fnName] = t.Name
		defs = append(defs, chatToolDef{
			Type: "function",
			Function: chatToolFuncDef{
				Name:        fnName,
				Description: t.Description,
				Parameters:  t.InputSchema,
			},
		})
	}
	return defs, nameMap
}

func (s *Server) callMCPTool(
	ctx context.Context,
	agentID string,
	role devicev1.SpeakerRole,
	mcpToolName string,
	rawArgs string,
) string {
	if strings.TrimSpace(s.cfg.GatewayMCPURL) == "" {
		return `{"error":"gateway MCP url not configured"}`
	}
	args := map[string]any{}
	if strings.TrimSpace(rawArgs) != "" {
		_ = json.Unmarshal([]byte(rawArgs), &args)
	}
	rpcReq := map[string]any{
		"jsonrpc": "2.0",
		"id":      2,
		"method":  "tools/call",
		"params": map[string]any{
			"name":      mcpToolName,
			"arguments": args,
		},
	}
	raw, _ := json.Marshal(rpcReq)
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, s.cfg.GatewayMCPURL, bytes.NewReader(raw))
	if err != nil {
		return fmt.Sprintf(`{"error":%q}`, err.Error())
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-Agent-ID", agentID)
	req.Header.Set("X-Speaker-Role", speakerRoleHeader(role))

	resp, err := s.httpClient.Do(req)
	if err != nil {
		return fmt.Sprintf(`{"error":%q}`, err.Error())
	}
	defer func() { _ = resp.Body.Close() }()

	var rpcResp struct {
		Result struct {
			Content []struct {
				Type string `json:"type"`
				Text string `json:"text"`
			} `json:"content"`
			IsError bool `json:"isError"`
		} `json:"result"`
		Error *struct {
			Message string `json:"message"`
		} `json:"error"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&rpcResp); err != nil {
		return fmt.Sprintf(`{"error":%q}`, err.Error())
	}
	if rpcResp.Error != nil {
		return fmt.Sprintf(`{"error":%q}`, rpcResp.Error.Message)
	}
	if len(rpcResp.Result.Content) > 0 {
		return rpcResp.Result.Content[0].Text
	}
	return `{"ok":true}`
}

func speakerRoleHeader(role devicev1.SpeakerRole) string {
	switch role {
	case devicev1.SpeakerRole_SPEAKER_ROLE_OWNER:
		return "owner"
	case devicev1.SpeakerRole_SPEAKER_ROLE_MEMBER:
		return "member"
	case devicev1.SpeakerRole_SPEAKER_ROLE_GUEST:
		return "guest"
	default:
		return "owner"
	}
}

func sendStatus(stream agentv1.AgentSessionService_ConverseServer, sessionID, turnID, state, message string) error {
	return stream.Send(&agentv1.AgentToGatewayEvent{
		Payload: &agentv1.AgentToGatewayEvent_Status{
			Status: &agentv1.StatusUpdate{
				SessionId: sessionID,
				TurnId:    turnID,
				State:     state,
				Message:   message,
			},
		},
	})
}

func makeDoneEvent(sessionID, turnID, reason, errMsg string) *agentv1.AgentToGatewayEvent {
	return &agentv1.AgentToGatewayEvent{
		Payload: &agentv1.AgentToGatewayEvent_Done{
			Done: &agentv1.TurnDone{
				SessionId:    sessionID,
				TurnId:       turnID,
				FinishReason: reason,
				ErrorMessage: errMsg,
			},
		},
	}
}

func formatTurnPrompt(turn *agentv1.SendTurn) string {
	var b strings.Builder
	b.WriteString("[ESP-Pika Device Context]\n")
	if turn.GetSpeakerId() != "" {
		fmt.Fprintf(&b, "speaker_id: %s\n", turn.GetSpeakerId())
	}
	if turn.GetSpeakerRole() != devicev1.SpeakerRole_SPEAKER_ROLE_UNSPECIFIED {
		fmt.Fprintf(&b, "speaker_role: %s\n", turn.GetSpeakerRole().String())
	}
	if turn.GetSourceDeviceId() != "" {
		fmt.Fprintf(&b, "source_device_id: %s\n", turn.GetSourceDeviceId())
	}
	if turn.GetZone() != "" {
		fmt.Fprintf(&b, "zone: %s\n", turn.GetZone())
	}
	if turn.GetGroupId() != "" {
		fmt.Fprintf(&b, "group_id: %s\n", turn.GetGroupId())
	}
	if turn.GetFleetSummaryJson() != "" {
		fmt.Fprintf(&b, "fleet_summary: %s\n", turn.GetFleetSummaryJson())
	}
	b.WriteString("[User Utterance]\n")
	b.WriteString(turn.GetText())
	return b.String()
}
