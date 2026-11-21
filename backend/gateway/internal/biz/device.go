package biz

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"sync"
	"time"

	"github.com/go-kratos/kratos/v2/errors"
	"github.com/go-kratos/kratos/v2/log"
)

var (
	// ErrDeviceOffline indicates the target device has no active control-plane connection.
	ErrDeviceOffline = errors.ServiceUnavailable("DEVICE_OFFLINE", "device is offline")
	// ErrDeviceNotFound indicates the target device is unknown.
	ErrDeviceNotFound = errors.NotFound("DEVICE_NOT_FOUND", "device not found")
)

// DeviceCapability describes a tool, shadow, or push-event capability registered by a device.
type DeviceCapability struct {
	Name            string   `json:"name"`
	Type            string   `json:"type"`
	Description     string   `json:"description,omitempty"`
	UploadChannel   string   `json:"upload_channel,omitempty"`
	Fields          []string `json:"fields,omitempty"`
	InputSchemaJSON string   `json:"input_schema_json,omitempty"`
}

// DeviceHello holds static device metadata and capabilities reported at connection time.
type DeviceHello struct {
	DeviceID        string             `json:"device_id"`
	MAC             string             `json:"mac"`
	GroupID         string             `json:"group_id"`
	Zone            string             `json:"zone"`
	Chip            string             `json:"chip"`
	VPVersion       uint64             `json:"vp_version"`
	FirmwareVersion string             `json:"firmware_version"`
	Capabilities    []DeviceCapability `json:"capabilities"`
}

// DeviceShadow represents the latest cached state of a device.
type DeviceShadow struct {
	DeviceID       string             `json:"device_id"`
	GroupID        string             `json:"group_id"`
	Zone           string             `json:"zone"`
	Online         bool               `json:"online"`
	BatteryPercent int32              `json:"battery_percent"`
	RSSI           int32              `json:"rssi"`
	Volume         int32              `json:"volume"`
	VADState       string             `json:"vad_state"`
	Metrics        map[string]float64 `json:"metrics,omitempty"`
	Attributes     map[string]string  `json:"attributes,omitempty"`
	UpdatedAt      time.Time          `json:"updated_at"`
}

// DeviceSummary pairs registration info with live shadow state.
type DeviceSummary struct {
	Hello  *DeviceHello  `json:"hello"`
	Shadow *DeviceShadow `json:"shadow"`
}

// DeviceCommand represents a command dispatched to a connected device.
type DeviceCommand struct {
	CommandID  string `json:"command_id"`
	DeviceID   string `json:"device_id"`
	Capability string `json:"capability"`
	Action     string `json:"action"`
	ParamsJSON string `json:"params_json"`
	TimeoutMS  uint32 `json:"timeout_ms"`
}

// DeviceCommandResult holds the device execution result.
type DeviceCommandResult struct {
	CommandID    string `json:"command_id"`
	DeviceID     string `json:"device_id"`
	OK           bool   `json:"ok"`
	ErrorCode    string `json:"error_code,omitempty"`
	ErrorMessage string `json:"error_message,omitempty"`
	ResultJSON   string `json:"result_json,omitempty"`
	MediaURL     string `json:"media_url,omitempty"`
}

// DeviceSessionSink abstracts the active control-plane transport to a connected device.
type DeviceSessionSink interface {
	SendJSON(ctx context.Context, v any) error
	SendAudio(ctx context.Context, opusFrame []byte) error
	InvokeCommand(ctx context.Context, cmd *DeviceCommand) (*DeviceCommandResult, error)
	Close(reason string) error
}

// DeviceRepo persists device registration metadata and shadows.
type DeviceRepo interface {
	SaveHello(ctx context.Context, hello *DeviceHello) error
	GetHello(ctx context.Context, deviceID string) (*DeviceHello, error)
	ListHellos(ctx context.Context, groupID string) ([]*DeviceHello, error)
	SaveShadow(ctx context.Context, shadow *DeviceShadow) error
	GetShadow(ctx context.Context, deviceID string) (*DeviceShadow, error)
	ListShadows(ctx context.Context, groupID string) ([]*DeviceShadow, error)
}

type activeDeviceConn struct {
	connID uint64
	hello  *DeviceHello
	sink   DeviceSessionSink
}

// DeviceUsecase manages device lifecycle, single-MAC mutual exclusion, shadows, and command dispatch.
type DeviceUsecase struct {
	repo   DeviceRepo
	acl    *ACLUsecase
	events EventBus
	log    *log.Helper

	mu     sync.RWMutex
	conns  map[string]*activeDeviceConn
	nextID uint64
}

// NewDeviceUsecase constructs a DeviceUsecase.
func NewDeviceUsecase(repo DeviceRepo, acl *ACLUsecase, events EventBus, logger log.Logger) *DeviceUsecase {
	return &DeviceUsecase{
		repo:   repo,
		acl:    acl,
		events: events,
		log:    log.NewHelper(logger),
		conns:  make(map[string]*activeDeviceConn),
	}
}

// RegisterConnection binds an authenticated MAC session, evicting any existing session for the same MAC.
func (uc *DeviceUsecase) RegisterConnection(ctx context.Context, hello *DeviceHello, sink DeviceSessionSink) (uint64, error) {
	mac, err := NormalizeMAC(hello.MAC)
	if err != nil {
		return 0, err
	}
	hello.MAC = mac
	hello.DeviceID = mac

	uc.mu.Lock()
	uc.nextID++
	connID := uc.nextID
	prev := uc.conns[mac]
	uc.conns[mac] = &activeDeviceConn{
		connID: connID,
		hello:  hello,
		sink:   sink,
	}
	uc.mu.Unlock()

	if prev != nil && prev.sink != nil {
		uc.log.WithContext(ctx).Infof("evicting previous connection for mac=%s prev_conn=%d", mac, prev.connID)
		_ = prev.sink.Close("replaced by new connection")
	}

	if err := uc.repo.SaveHello(ctx, hello); err != nil {
		return 0, fmt.Errorf("save device hello: %w", err)
	}

	shadow, err := uc.repo.GetShadow(ctx, mac)
	if err != nil || shadow == nil {
		shadow = &DeviceShadow{
			DeviceID:   mac,
			Metrics:    make(map[string]float64),
			Attributes: make(map[string]string),
		}
	}
	shadow.GroupID = hello.GroupID
	shadow.Zone = hello.Zone
	shadow.Online = true
	shadow.UpdatedAt = time.Now().UTC()
	if err := uc.repo.SaveShadow(ctx, shadow); err != nil {
		return 0, fmt.Errorf("save device shadow: %w", err)
	}

	if uc.events != nil {
		_ = uc.events.PublishPresence(ctx, mac, hello.GroupID, hello.Zone, true)
	}
	return connID, nil
}

// Disconnect marks the device offline only if connID still owns the active slot.
func (uc *DeviceUsecase) Disconnect(ctx context.Context, rawMAC string, connID uint64) {
	mac, err := NormalizeMAC(rawMAC)
	if err != nil {
		return
	}

	var hello *DeviceHello
	uc.mu.Lock()
	cur := uc.conns[mac]
	if cur != nil && cur.connID == connID {
		hello = cur.hello
		delete(uc.conns, mac)
	}
	uc.mu.Unlock()

	if hello == nil {
		return
	}

	shadow, err := uc.repo.GetShadow(ctx, mac)
	if err == nil && shadow != nil {
		shadow.Online = false
		shadow.UpdatedAt = time.Now().UTC()
		_ = uc.repo.SaveShadow(ctx, shadow)
	}
	if uc.events != nil {
		_ = uc.events.PublishPresence(ctx, mac, hello.GroupID, hello.Zone, false)
	}
}

// SendToDevice sends a JSON control message to the active device connection.
func (uc *DeviceUsecase) SendToDevice(ctx context.Context, deviceID string, payload any) error {
	mac, err := NormalizeMAC(deviceID)
	if err != nil {
		return err
	}
	uc.mu.RLock()
	cur := uc.conns[mac]
	uc.mu.RUnlock()
	if cur == nil || cur.sink == nil {
		return ErrDeviceOffline
	}
	return cur.sink.SendJSON(ctx, payload)
}

// SendAudioToDevice sends a downstream Opus frame to an online device.
func (uc *DeviceUsecase) SendAudioToDevice(ctx context.Context, deviceID string, opusFrame []byte) error {
	mac, err := NormalizeMAC(deviceID)
	if err != nil {
		return err
	}
	uc.mu.RLock()
	cur := uc.conns[mac]
	uc.mu.RUnlock()
	if cur == nil || cur.sink == nil {
		return ErrDeviceOffline
	}
	return cur.sink.SendAudio(ctx, opusFrame)
}

// UpdateShadow merges partial shadow metrics/attributes for a device.
func (uc *DeviceUsecase) UpdateShadow(ctx context.Context, rawMAC string, battery, rssi, volume *int32, vadState string, metrics map[string]float64, attrs map[string]string) (*DeviceShadow, error) {
	mac, err := NormalizeMAC(rawMAC)
	if err != nil {
		return nil, err
	}
	shadow, err := uc.repo.GetShadow(ctx, mac)
	if err != nil {
		return nil, fmt.Errorf("get shadow: %w", err)
	}
	if shadow == nil {
		shadow = &DeviceShadow{
			DeviceID:   mac,
			Metrics:    make(map[string]float64),
			Attributes: make(map[string]string),
		}
	}
	if shadow.Metrics == nil {
		shadow.Metrics = make(map[string]float64)
	}
	if shadow.Attributes == nil {
		shadow.Attributes = make(map[string]string)
	}
	if battery != nil {
		shadow.BatteryPercent = *battery
	}
	if rssi != nil {
		shadow.RSSI = *rssi
	}
	if volume != nil {
		shadow.Volume = *volume
	}
	if vadState != "" {
		shadow.VADState = vadState
	}
	for k, v := range metrics {
		shadow.Metrics[k] = v
	}
	for k, v := range attrs {
		shadow.Attributes[k] = v
	}
	shadow.UpdatedAt = time.Now().UTC()
	if err := uc.repo.SaveShadow(ctx, shadow); err != nil {
		return nil, fmt.Errorf("save shadow: %w", err)
	}
	return shadow, nil
}

// ResolveTarget resolves either a MAC device_id or a zone/room name to an online device.
func (uc *DeviceUsecase) ResolveTarget(ctx context.Context, target string) (*DeviceHello, DeviceSessionSink, error) {
	trimmed := strings.TrimSpace(target)
	if trimmed == "" {
		return nil, nil, ErrDeviceNotFound
	}
	if mac, err := NormalizeMAC(trimmed); err == nil {
		uc.mu.RLock()
		cur := uc.conns[mac]
		uc.mu.RUnlock()
		if cur != nil {
			return cur.hello, cur.sink, nil
		}
		hello, err := uc.repo.GetHello(ctx, mac)
		if err != nil || hello == nil {
			return nil, nil, ErrDeviceNotFound
		}
		return hello, nil, ErrDeviceOffline
	}

	uc.mu.RLock()
	defer uc.mu.RUnlock()
	for _, conn := range uc.conns {
		if strings.EqualFold(conn.hello.Zone, trimmed) {
			return conn.hello, conn.sink, nil
		}
	}
	return nil, nil, ErrDeviceNotFound
}

// InvokeCommand enforces ACL and dispatches a command to the resolved target device.
func (uc *DeviceUsecase) InvokeCommand(ctx context.Context, agentID string, role SpeakerRole, target string, cmd *DeviceCommand) (*DeviceCommandResult, error) {
	hello, sink, err := uc.ResolveTarget(ctx, target)
	if err != nil {
		return nil, err
	}
	if err := uc.acl.CheckAccess(ctx, agentID, role, hello, cmd.Capability); err != nil {
		return nil, err
	}
	if sink == nil {
		return nil, ErrDeviceOffline
	}
	cmd.DeviceID = hello.DeviceID
	return sink.InvokeCommand(ctx, cmd)
}

// GetShadow returns the cached shadow of a device.
func (uc *DeviceUsecase) GetShadow(ctx context.Context, rawMAC string) (*DeviceShadow, error) {
	mac, err := NormalizeMAC(rawMAC)
	if err != nil {
		return nil, err
	}
	shadow, err := uc.repo.GetShadow(ctx, mac)
	if err != nil {
		return nil, fmt.Errorf("get shadow %s: %w", mac, err)
	}
	if shadow == nil {
		return nil, ErrDeviceNotFound
	}
	return shadow, nil
}

// ListDevices returns all known devices and their shadow states.
func (uc *DeviceUsecase) ListDevices(ctx context.Context, groupID string, onlineOnly bool) ([]*DeviceSummary, error) {
	hellos, err := uc.repo.ListHellos(ctx, groupID)
	if err != nil {
		return nil, fmt.Errorf("list hellos: %w", err)
	}
	shadows, err := uc.repo.ListShadows(ctx, groupID)
	if err != nil {
		return nil, fmt.Errorf("list shadows: %w", err)
	}
	shadowMap := make(map[string]*DeviceShadow, len(shadows))
	for _, s := range shadows {
		shadowMap[s.DeviceID] = s
	}

	res := make([]*DeviceSummary, 0, len(hellos))
	for _, h := range hellos {
		s := shadowMap[h.DeviceID]
		if onlineOnly && (s == nil || !s.Online) {
			continue
		}
		res = append(res, &DeviceSummary{
			Hello:  h,
			Shadow: s,
		})
	}
	return res, nil
}

// FleetTopologyJSON builds a JSON snapshot of online/registered devices for MCP `fleet://topology`.
func (uc *DeviceUsecase) FleetTopologyJSON(ctx context.Context, groupID string) (string, error) {
	devices, err := uc.ListDevices(ctx, groupID, false)
	if err != nil {
		return "", err
	}
	b, err := json.MarshalIndent(devices, "", "  ")
	if err != nil {
		return "", fmt.Errorf("marshal fleet topology: %w", err)
	}
	return string(b), nil
}
