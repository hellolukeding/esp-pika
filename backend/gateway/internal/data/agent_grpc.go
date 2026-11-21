package data

import (
	"context"
	"fmt"
	"io"
	"sync"

	agentv1 "esp-pika/backend/gateway/api/agent/v1"
	devicev1 "esp-pika/backend/gateway/api/device/v1"
	"esp-pika/backend/gateway/internal/biz"
	"esp-pika/backend/gateway/internal/conf"

	"github.com/go-kratos/kratos/v2/log"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/protobuf/types/known/timestamppb"
)

// GRPCAgentAdapter implements biz.AgentAdapter over proto/agent/v1 bidirectional streaming gRPC.
type GRPCAgentAdapter struct {
	addr   string
	conn   *grpc.ClientConn
	client agentv1.AgentSessionServiceClient
	log    *log.Helper

	mu     sync.Mutex
	active map[string]context.CancelFunc // key: session_id + ":" + turn_id
}

// NewGRPCAgentAdapter constructs a gRPC AgentAdapter connected lazily to conf.Voice.AgentGrpcAddr.
func NewGRPCAgentAdapter(c *conf.Voice, logger log.Logger) (biz.AgentAdapter, func(), error) {
	helper := log.NewHelper(logger)
	addr := ""
	if c != nil {
		addr = c.AgentGrpcAddr
	}
	if addr == "" {
		helper.Warn("voice.agent_grpc_addr is empty; AgentAdapter disabled")
		return &GRPCAgentAdapter{log: helper, active: make(map[string]context.CancelFunc)}, func() {}, nil
	}

	conn, err := grpc.NewClient(addr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return nil, nil, fmt.Errorf("create agent grpc client %s: %w", addr, err)
	}
	adapter := &GRPCAgentAdapter{
		addr:   addr,
		conn:   conn,
		client: agentv1.NewAgentSessionServiceClient(conn),
		log:    helper,
		active: make(map[string]context.CancelFunc),
	}
	cleanup := func() {
		if err := conn.Close(); err != nil {
			helper.Warnf("close agent grpc client: %v", err)
		}
	}
	return adapter, cleanup, nil
}

func (a *GRPCAgentAdapter) RunTurn(ctx context.Context, req *biz.AgentTurnRequest, cb biz.AgentStreamCallbacks) error {
	if a.client == nil {
		return fmt.Errorf("agent adapter not configured")
	}
	var shadowPB *devicev1.DeviceShadow
	if req.Shadow != nil {
		shadowPB = &devicev1.DeviceShadow{
			DeviceId:       req.Shadow.DeviceID,
			Online:         req.Shadow.Online,
			BatteryPercent: req.Shadow.BatteryPercent,
			Rssi:           req.Shadow.RSSI,
			Volume:         req.Shadow.Volume,
			VadState:       req.Shadow.VADState,
			Metrics:        req.Shadow.Metrics,
			Attributes:     req.Shadow.Attributes,
			UpdatedAt:      timestamppb.New(req.Shadow.UpdatedAt),
		}
	}
	ev := &agentv1.GatewayToAgentEvent{
		Payload: &agentv1.GatewayToAgentEvent_SendTurn{
			SendTurn: &agentv1.SendTurn{
				SessionId:        req.SessionID,
				TurnId:           req.TurnID,
				AgentId:          req.AgentID,
				Text:             req.Text,
				SpeakerId:        req.SpeakerID,
				SpeakerRole:      devicev1.SpeakerRole(req.SpeakerRole),
				Confidence:       req.Confidence,
				SourceDeviceId:   req.SourceDeviceID,
				GroupId:          req.GroupID,
				Zone:             req.Zone,
				ShadowSnapshot:   shadowPB,
				FleetSummaryJson: req.FleetSummaryJSON,
			},
		},
	}
	return a.streamSingleExchange(ctx, req.SessionID, req.TurnID, ev, cb)
}

func (a *GRPCAgentAdapter) SendEvent(ctx context.Context, evt *biz.AgentDeviceEvent, cb biz.AgentStreamCallbacks) error {
	if a.client == nil {
		return fmt.Errorf("agent adapter not configured")
	}
	ts := evt.OccurredAt
	if ts.IsZero() {
		ts = timestamppb.Now().AsTime()
	}
	ev := &agentv1.GatewayToAgentEvent{
		Payload: &agentv1.GatewayToAgentEvent_SendEvent{
			SendEvent: &agentv1.SendEvent{
				SessionId:      evt.SessionID,
				AgentId:        evt.AgentID,
				SourceDeviceId: evt.SourceDeviceID,
				GroupId:        evt.GroupID,
				Zone:           evt.Zone,
				EventType:      evt.EventType,
				PayloadJson:    evt.PayloadJSON,
				OccurredAt:     timestamppb.New(ts),
			},
		},
	}
	return a.streamSingleExchange(ctx, evt.SessionID, "", ev, cb)
}

func (a *GRPCAgentAdapter) CancelTurn(ctx context.Context, sessionID, turnID, reason string) error {
	a.mu.Lock()
	if cancel, ok := a.active[sessionID+":"+turnID]; ok {
		cancel()
		delete(a.active, sessionID+":"+turnID)
	}
	a.mu.Unlock()

	if a.client == nil {
		return nil
	}
	stream, err := a.client.Converse(ctx)
	if err != nil {
		return fmt.Errorf("open converse for cancel: %w", err)
	}
	defer func() { _ = stream.CloseSend() }()
	return stream.Send(&agentv1.GatewayToAgentEvent{
		Payload: &agentv1.GatewayToAgentEvent_Cancel{
			Cancel: &agentv1.CancelTurn{
				SessionId: sessionID,
				TurnId:    turnID,
				Reason:    reason,
			},
		},
	})
}

func (a *GRPCAgentAdapter) streamSingleExchange(
	ctx context.Context,
	sessionID string,
	turnID string,
	ev *agentv1.GatewayToAgentEvent,
	cb biz.AgentStreamCallbacks,
) error {
	runCtx, cancel := context.WithCancel(ctx)
	key := sessionID + ":" + turnID
	a.mu.Lock()
	a.active[key] = cancel
	a.mu.Unlock()
	defer func() {
		cancel()
		a.mu.Lock()
		delete(a.active, key)
		a.mu.Unlock()
	}()

	stream, err := a.client.Converse(runCtx)
	if err != nil {
		return fmt.Errorf("agent converse %s: %w", a.addr, err)
	}
	if err := stream.Send(ev); err != nil {
		return fmt.Errorf("agent send event: %w", err)
	}
	if err := stream.CloseSend(); err != nil {
		return fmt.Errorf("agent close send: %w", err)
	}

	for {
		resp, err := stream.Recv()
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return fmt.Errorf("agent recv: %w", err)
		}
		switch p := resp.Payload.(type) {
		case *agentv1.AgentToGatewayEvent_Status:
			if cb.OnStatus != nil {
				cb.OnStatus(p.Status.GetState(), p.Status.GetMessage(), p.Status.GetSpeakHint())
			}
		case *agentv1.AgentToGatewayEvent_TextDelta:
			if cb.OnTextDelta != nil {
				cb.OnTextDelta(p.TextDelta.GetChunk(), p.TextDelta.GetTtsVoice())
			}
		case *agentv1.AgentToGatewayEvent_UiUpdate:
			if cb.OnUIUpdate != nil {
				cb.OnUIUpdate(p.UiUpdate.GetTargetDeviceId(), p.UiUpdate.GetEmotion(), p.UiUpdate.GetText(), p.UiUpdate.GetCardJson())
			}
		case *agentv1.AgentToGatewayEvent_Done:
			if cb.OnDone != nil {
				cb.OnDone(p.Done.GetFinishReason(), p.Done.GetErrorMessage())
			}
			return nil
		}
	}
}
