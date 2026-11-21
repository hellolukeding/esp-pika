package bridge

import (
	"context"
	"fmt"
	"io"
	"strings"
	"sync"

	"esp-pika/agent/ez-bridge/ezruntime"
	agentv1 "esp-pika/backend/gateway/api/agent/v1"
	devicev1 "esp-pika/backend/gateway/api/device/v1"
)

// Options configures the AgentSessionService bridge behavior.
type Options struct {
	AgentID      string
	MCPServerKey string
	AutoApprove  bool
}

// Server bridges esp-pika `AgentSessionService` gRPC calls to `ez-assistant-runtime`.
type Server struct {
	agentv1.UnimplementedAgentSessionServiceServer

	client *ezruntime.Client
	opts   Options

	mu       sync.Mutex
	sessions map[string]string // esp_session_id -> ez session_id
	runs     map[string]string // esp_session_id + ":" + turn_id -> ez run_id
}

// NewServer constructs a bridge Server backed by `client`.
func NewServer(client *ezruntime.Client, opts Options) *Server {
	if opts.AgentID == "" {
		opts.AgentID = "ez-assistant"
	}
	return &Server{
		client:   client,
		opts:     opts,
		sessions: make(map[string]string),
		runs:     make(map[string]string),
	}
}

// Health verifies that the backing ez-assistant-runtime responds to /health.
func (s *Server) Health(ctx context.Context, _ *agentv1.HealthRequest) (*agentv1.HealthResponse, error) {
	if err := s.client.Health(ctx); err != nil {
		return nil, fmt.Errorf("ez-assistant health: %w", err)
	}
	return &agentv1.HealthResponse{
		Status:  "SERVING",
		AgentId: s.opts.AgentID,
	}, nil
}

// Converse handles bidirectional gateway <-> Agent streaming turns, device push events, and cancellations.
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
			_ = s.handleCancel(stream.Context(), p.Cancel)
		}
	}
}

func (s *Server) ensureSession(ctx context.Context, espSessionID, title string) (string, error) {
	s.mu.Lock()
	existing, ok := s.sessions[espSessionID]
	s.mu.Unlock()
	if ok && existing != "" {
		return existing, nil
	}

	_ = s.client.EnsureReady(ctx)
	if title == "" {
		title = "esp-pika " + espSessionID
	}
	ezID, err := s.client.CreateSession(ctx, title)
	if err != nil {
		return "", err
	}
	if s.opts.AutoApprove {
		_ = s.client.SetSessionApprovalMode(ctx, ezID, "auto")
	}

	s.mu.Lock()
	if prev, found := s.sessions[espSessionID]; found && prev != "" {
		s.mu.Unlock()
		return prev, nil
	}
	s.sessions[espSessionID] = ezID
	s.mu.Unlock()
	return ezID, nil
}

func (s *Server) handleTurn(ctx context.Context, stream agentv1.AgentSessionService_ConverseServer, turn *agentv1.SendTurn) error {
	if turn == nil {
		return nil
	}
	ezSessionID, err := s.ensureSession(ctx, turn.GetSessionId(), "Voice "+turn.GetZone())
	if err != nil {
		return stream.Send(makeDoneEvent(turn.GetSessionId(), turn.GetTurnId(), "failed", err.Error()))
	}

	prompt := formatTurnPrompt(turn)
	return s.executeInputAndStream(ctx, stream, turn.GetSessionId(), turn.GetTurnId(), ezSessionID, prompt)
}

func (s *Server) handleDeviceEvent(ctx context.Context, stream agentv1.AgentSessionService_ConverseServer, devEvt *agentv1.SendEvent) error {
	if devEvt == nil {
		return nil
	}
	ezSessionID, err := s.ensureSession(ctx, devEvt.GetSessionId(), "Events "+devEvt.GetZone())
	if err != nil {
		return stream.Send(makeDoneEvent(devEvt.GetSessionId(), "", "failed", err.Error()))
	}

	prompt := fmt.Sprintf("[Proactive Device Event]\nevent_type: %s\nsource_device_id: %s\nzone: %s\ngroup_id: %s\npayload_json: %s\nDecide whether to inspect via MCP tools or alert the user briefly in spoken language.",
		devEvt.GetEventType(), devEvt.GetSourceDeviceId(), devEvt.GetZone(), devEvt.GetGroupId(), devEvt.GetPayloadJson())
	return s.executeInputAndStream(ctx, stream, devEvt.GetSessionId(), "", ezSessionID, prompt)
}

func (s *Server) executeInputAndStream(
	ctx context.Context,
	stream agentv1.AgentSessionService_ConverseServer,
	espSessionID string,
	turnID string,
	ezSessionID string,
	message string,
) error {
	ch, unsub := s.client.SubscribeSession(ezSessionID)
	defer unsub()

	_, activeRunID, err := s.client.SubmitInput(ctx, ezSessionID, message, s.opts.MCPServerKey, turnID)
	if err != nil {
		return stream.Send(makeDoneEvent(espSessionID, turnID, "failed", err.Error()))
	}
	if activeRunID != "" {
		s.setRunID(espSessionID, turnID, activeRunID)
	}
	defer s.clearRunID(espSessionID, turnID)

	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case evt, ok := <-ch:
			if !ok {
				return stream.Send(makeDoneEvent(espSessionID, turnID, "cancelled", "event stream closed"))
			}
			if activeRunID == "" && evt.RunID != "" {
				activeRunID = evt.RunID
				s.setRunID(espSessionID, turnID, activeRunID)
			}
			if activeRunID != "" && evt.RunID != "" && evt.RunID != activeRunID {
				continue
			}

			switch evt.Type {
			case "run_started":
				_ = stream.Send(&agentv1.AgentToGatewayEvent{
					Payload: &agentv1.AgentToGatewayEvent_Status{
						Status: &agentv1.StatusUpdate{
							SessionId: espSessionID,
							TurnId:    turnID,
							State:     "thinking",
							Message:   "正在思考",
						},
					},
				})
			case "tool_proposed", "tool_started":
				msg := evt.ToolName
				if msg == "" {
					msg = "executing tool"
				}
				_ = stream.Send(&agentv1.AgentToGatewayEvent{
					Payload: &agentv1.AgentToGatewayEvent_Status{
						Status: &agentv1.StatusUpdate{
							SessionId: espSessionID,
							TurnId:    turnID,
							State:     "calling_tool",
							Message:   msg,
						},
					},
				})
			case "text_delta":
				if evt.Delta != "" {
					_ = stream.Send(&agentv1.AgentToGatewayEvent{
						Payload: &agentv1.AgentToGatewayEvent_TextDelta{
							TextDelta: &agentv1.TextDelta{
								SessionId: espSessionID,
								TurnId:    turnID,
								Chunk:     evt.Delta,
							},
						},
					})
				}
			case "approval_requested":
				if s.opts.AutoApprove && evt.Approval != nil && evt.Approval.ApprovalID != "" {
					_ = s.client.DecideApproval(ctx, ezSessionID, evt.Approval.ApprovalID, "allow_once")
				}
			case "run_finished":
				errMsg := ""
				if evt.Error != nil {
					errMsg = evt.Error.Message
				}
				reason := evt.Status
				if reason == "" {
					reason = "completed"
				}
				return stream.Send(makeDoneEvent(espSessionID, turnID, reason, errMsg))
			}
		}
	}
}

func (s *Server) handleCancel(ctx context.Context, cancel *agentv1.CancelTurn) error {
	if cancel == nil {
		return nil
	}
	s.mu.Lock()
	ezSessionID := s.sessions[cancel.GetSessionId()]
	runID := s.runs[cancel.GetSessionId()+":"+cancel.GetTurnId()]
	s.mu.Unlock()
	if ezSessionID == "" || runID == "" {
		return nil
	}
	return s.client.CancelRun(ctx, ezSessionID, runID)
}

func (s *Server) setRunID(espSessionID, turnID, runID string) {
	s.mu.Lock()
	s.runs[espSessionID+":"+turnID] = runID
	s.mu.Unlock()
}

func (s *Server) clearRunID(espSessionID, turnID string) {
	s.mu.Lock()
	delete(s.runs, espSessionID+":"+turnID)
	s.mu.Unlock()
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
