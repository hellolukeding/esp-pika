package biz

import (
	"context"
	"sync"
	"time"

	"github.com/go-kratos/kratos/v2/log"
)

// EventBus defines asynchronous event publication across the gateway and NATS JetStream.
type EventBus interface {
	PublishPresence(ctx context.Context, deviceID, groupID, zone string, online bool) error
	PublishWakeDecision(ctx context.Context, decision *WakeDecision) error
	PublishVoiceprintSync(ctx context.Context, syncEvt *VoiceprintSync) error
	PublishSensorPush(ctx context.Context, deviceID, groupID, zone, eventType, payloadJSON string) error
}

// WakeCandidate represents a wake-word detection report from one device in a group.
type WakeCandidate struct {
	DeviceID     string
	GroupID      string
	Zone         string
	WakeWord     string
	WakeScore    float32
	SNRDB        float32
	DetectedAtMS int64
}

// WakeDecision is the arbitration outcome emitted for each candidate in the wake window.
type WakeDecision struct {
	GroupID        string `json:"group_id"`
	WinnerDeviceID string `json:"winner_device_id"`
	TargetDeviceID string `json:"target_device_id"`
	Accepted       bool   `json:"accepted"`
	Reason         string `json:"reason"`
	DecidedAtMS    int64  `json:"decided_at_ms"`
}

type wakeWindow struct {
	candidates map[string]WakeCandidate
	timer      *time.Timer
}

// WakeArbiter collects concurrent wake detections per group_id within a short window
// and selects the best device based on SNR and wake_score.
type WakeArbiter struct {
	window  time.Duration
	devices *DeviceUsecase
	events  EventBus
	log     *log.Helper

	mu      sync.Mutex
	windows map[string]*wakeWindow
	closed  bool
}

// NewWakeArbiter constructs a WakeArbiter with the given arbitration window duration.
func NewWakeArbiter(window time.Duration, devices *DeviceUsecase, events EventBus, logger log.Logger) *WakeArbiter {
	if window <= 0 {
		window = 120 * time.Millisecond
	}
	return &WakeArbiter{
		window:  window,
		devices: devices,
		events:  events,
		log:     log.NewHelper(logger),
		windows: make(map[string]*wakeWindow),
	}
}

// Submit registers a wake candidate from a device. The first report in an idle group opens the timer window.
func (a *WakeArbiter) Submit(ctx context.Context, cand WakeCandidate) {
	if cand.GroupID == "" {
		cand.GroupID = "home-default"
	}

	a.mu.Lock()
	defer a.mu.Unlock()
	if a.closed {
		return
	}

	w, ok := a.windows[cand.GroupID]
	if !ok {
		w = &wakeWindow{
			candidates: make(map[string]WakeCandidate),
		}
		groupID := cand.GroupID
		w.timer = time.AfterFunc(a.window, func() {
			a.resolveGroup(groupID)
		})
		a.windows[cand.GroupID] = w
	}
	w.candidates[cand.DeviceID] = cand
}

func (a *WakeArbiter) resolveGroup(groupID string) {
	a.mu.Lock()
	if a.closed {
		a.mu.Unlock()
		return
	}
	w, ok := a.windows[groupID]
	if !ok {
		a.mu.Unlock()
		return
	}
	delete(a.windows, groupID)
	a.mu.Unlock()

	if len(w.candidates) == 0 {
		return
	}

	var winner WakeCandidate
	bestScore := float64(-1e9)
	first := true
	for _, c := range w.candidates {
		score := float64(c.SNRDB) + float64(c.WakeScore)*10.0
		if first || score > bestScore || (score == bestScore && c.DetectedAtMS < winner.DetectedAtMS) {
			winner = c
			bestScore = score
			first = false
		}
	}

	nowMS := time.Now().UnixMilli()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()

	for _, c := range w.candidates {
		accepted := c.DeviceID == winner.DeviceID
		msgType := "wake.suppress"
		reason := "suppressed_by_higher_snr_peer"
		if accepted {
			msgType = "wake.accept"
			reason = "highest_composite_score"
		}
		decision := &WakeDecision{
			GroupID:        groupID,
			WinnerDeviceID: winner.DeviceID,
			TargetDeviceID: c.DeviceID,
			Accepted:       accepted,
			Reason:         reason,
			DecidedAtMS:    nowMS,
		}
		if a.devices != nil {
			if err := a.devices.SendToDevice(ctx, c.DeviceID, map[string]any{
				"type":             msgType,
				"group_id":         groupID,
				"winner_device_id": winner.DeviceID,
				"reason":           reason,
			}); err != nil {
				a.log.Warnf("send %s to %s failed: %v", msgType, c.DeviceID, err)
			}
		}
		if a.events != nil {
			_ = a.events.PublishWakeDecision(ctx, decision)
		}
	}
}

// Close stops all pending arbitration timers cleanly during shutdown.
func (a *WakeArbiter) Close() {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.closed = true
	for groupID, w := range a.windows {
		if w.timer != nil {
			w.timer.Stop()
		}
		delete(a.windows, groupID)
	}
}
