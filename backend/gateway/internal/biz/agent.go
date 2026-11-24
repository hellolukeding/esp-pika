package biz

import (
	"context"
	"time"
)

// AgentTurnRequest carries a single user turn and full device/fleet context into the Agent brain layer.
type AgentTurnRequest struct {
	SessionID        string
	TurnID           string
	AgentID          string
	Text             string
	SpeakerID        string
	SpeakerRole      SpeakerRole
	Confidence       float32
	SourceDeviceID   string
	GroupID          string
	Zone             string
	Shadow           *DeviceShadow
	FleetSummaryJSON string
	SystemPrompt     string
	APIKey           string
	Model            string
}

// AgentDeviceEvent carries a proactive device/sensor push event into the Agent brain layer.
type AgentDeviceEvent struct {
	SessionID      string
	AgentID        string
	SourceDeviceID string
	GroupID        string
	Zone           string
	EventType      string
	PayloadJSON    string
	OccurredAt     time.Time
}

// AgentStreamCallbacks receives incremental Agent stream responses for device delivery.
type AgentStreamCallbacks struct {
	OnStatus    func(state, message string, speakHint bool)
	OnTextDelta func(chunk, ttsVoice string)
	OnUIUpdate  func(targetDeviceID, emotion, text, cardJSON string)
	OnDone      func(finishReason, errorMessage string)
}

// AgentAdapter bridges the gateway to an external Agent brain (such as ez-assistant via gRPC).
type AgentAdapter interface {
	RunTurn(ctx context.Context, req *AgentTurnRequest, cb AgentStreamCallbacks) error
	SendEvent(ctx context.Context, evt *AgentDeviceEvent, cb AgentStreamCallbacks) error
	CancelTurn(ctx context.Context, sessionID, turnID, reason string) error
}
