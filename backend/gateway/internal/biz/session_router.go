package biz

import (
	"context"
	"fmt"
	"sync"
)

// SessionRoute describes the resolved (session_id, agent_id, source_device_id) tuple for a turn.
type SessionRoute struct {
	SessionID      string
	AgentID        string
	SourceDeviceID string
}

// SessionRouter decouples physical Device from logical Session and Agent,
// supporting 1:N and M:N routing plus cross-room session handoff.
type SessionRouter struct {
	speakers SpeakerRepo

	mu               sync.RWMutex
	lastActiveDevice map[string]string // speaker_id -> device_id
}

// NewSessionRouter constructs a SessionRouter.
func NewSessionRouter(speakers SpeakerRepo) *SessionRouter {
	return &SessionRouter{
		speakers:         speakers,
		lastActiveDevice: make(map[string]string),
	}
}

// RecordActivity updates the most recently active device for a speaker (used for session handoff).
func (r *SessionRouter) RecordActivity(speakerID, deviceID string) {
	if speakerID == "" || deviceID == "" {
		return
	}
	r.mu.Lock()
	r.lastActiveDevice[speakerID] = deviceID
	r.mu.Unlock()
}

// ResolveTurn resolves the logical session and target agent for an incoming turn.
// Sessions bind to (speaker_id, agent_id) when speaker_id is known, allowing seamless room-to-room handoff.
func (r *SessionRouter) ResolveTurn(ctx context.Context, speakerID, requestedAgentID, sourceDeviceID string) SessionRoute {
	agentID := requestedAgentID
	if agentID == "" && speakerID != "" && r.speakers != nil {
		if sp, err := r.speakers.Get(ctx, speakerID); err == nil && sp != nil && sp.AgentID != "" {
			agentID = sp.AgentID
		}
	}
	if agentID == "" {
		agentID = "default-agent"
	}

	if speakerID != "" {
		r.RecordActivity(speakerID, sourceDeviceID)
		return SessionRoute{
			SessionID:      fmt.Sprintf("sess:%s:%s", speakerID, agentID),
			AgentID:        agentID,
			SourceDeviceID: sourceDeviceID,
		}
	}

	return SessionRoute{
		SessionID:      fmt.Sprintf("sess:dev:%s:%s", sourceDeviceID, agentID),
		AgentID:        agentID,
		SourceDeviceID: sourceDeviceID,
	}
}

// ResolveCallbackDevice picks the speaker's last active device for async task notifications,
// falling back to fallbackDeviceID if no prior activity is recorded.
func (r *SessionRouter) ResolveCallbackDevice(speakerID, fallbackDeviceID string) string {
	if speakerID == "" {
		return fallbackDeviceID
	}
	r.mu.RLock()
	dev, ok := r.lastActiveDevice[speakerID]
	r.mu.RUnlock()
	if ok && dev != "" {
		return dev
	}
	return fallbackDeviceID
}
