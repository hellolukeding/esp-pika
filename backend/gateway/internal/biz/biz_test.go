package biz

import (
	"context"
	"errors"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/go-kratos/kratos/v2/log"
)

type memMacRepo struct {
	mu    sync.RWMutex
	items map[string]*AllowedMac
}

func newMemMacRepo() *memMacRepo {
	return &memMacRepo{items: make(map[string]*AllowedMac)}
}

func (m *memMacRepo) GetByMAC(_ context.Context, mac string) (*AllowedMac, error) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	v := m.items[mac]
	if v == nil {
		return nil, nil
	}
	cp := *v
	return &cp, nil
}

func (m *memMacRepo) Upsert(_ context.Context, entry *AllowedMac) (*AllowedMac, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	cp := *entry
	m.items[entry.MAC] = &cp
	return &cp, nil
}

func (m *memMacRepo) Delete(_ context.Context, mac string) (bool, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	_, ok := m.items[mac]
	delete(m.items, mac)
	return ok, nil
}

func (m *memMacRepo) List(_ context.Context, groupID string) ([]*AllowedMac, error) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	var out []*AllowedMac
	for _, v := range m.items {
		if groupID == "" || v.GroupID == groupID {
			cp := *v
			out = append(out, &cp)
		}
	}
	return out, nil
}

type memACLRepo struct {
	rules []*ACLRule
}

func (m *memACLRepo) Upsert(_ context.Context, rule *ACLRule) (*ACLRule, error) {
	for i, r := range m.rules {
		if r.RuleID == rule.RuleID {
			cp := *rule
			m.rules[i] = &cp
			return &cp, nil
		}
	}
	cp := *rule
	m.rules = append(m.rules, &cp)
	return &cp, nil
}

func (m *memACLRepo) Delete(_ context.Context, ruleID string) (bool, error) {
	for i, r := range m.rules {
		if r.RuleID == ruleID {
			m.rules = append(m.rules[:i], m.rules[i+1:]...)
			return true, nil
		}
	}
	return false, nil
}

func (m *memACLRepo) List(_ context.Context, agentID string) ([]*ACLRule, error) {
	var out []*ACLRule
	for _, r := range m.rules {
		if agentID == "" || r.AgentID == agentID || r.AgentID == "*" {
			out = append(out, r)
		}
	}
	return out, nil
}

type memDeviceRepo struct {
	mu      sync.RWMutex
	hellos  map[string]*DeviceHello
	shadows map[string]*DeviceShadow
}

func newMemDeviceRepo() *memDeviceRepo {
	return &memDeviceRepo{
		hellos:  make(map[string]*DeviceHello),
		shadows: make(map[string]*DeviceShadow),
	}
}

func (m *memDeviceRepo) SaveHello(_ context.Context, h *DeviceHello) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	cp := *h
	m.hellos[h.DeviceID] = &cp
	return nil
}

func (m *memDeviceRepo) GetHello(_ context.Context, id string) (*DeviceHello, error) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	if h := m.hellos[id]; h != nil {
		cp := *h
		return &cp, nil
	}
	return nil, nil
}

func (m *memDeviceRepo) ListHellos(_ context.Context, groupID string) ([]*DeviceHello, error) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	var out []*DeviceHello
	for _, h := range m.hellos {
		if groupID == "" || h.GroupID == groupID {
			cp := *h
			out = append(out, &cp)
		}
	}
	return out, nil
}

func (m *memDeviceRepo) SaveShadow(_ context.Context, s *DeviceShadow) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	cp := *s
	m.shadows[s.DeviceID] = &cp
	return nil
}

func (m *memDeviceRepo) GetShadow(_ context.Context, id string) (*DeviceShadow, error) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	if s := m.shadows[id]; s != nil {
		cp := *s
		return &cp, nil
	}
	return nil, nil
}

func (m *memDeviceRepo) ListShadows(_ context.Context, groupID string) ([]*DeviceShadow, error) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	var out []*DeviceShadow
	for _, s := range m.shadows {
		if groupID == "" || s.GroupID == groupID {
			cp := *s
			out = append(out, &cp)
		}
	}
	return out, nil
}

type fakeSink struct {
	mu     sync.Mutex
	sent   []map[string]any
	closed string
}

func (f *fakeSink) SendJSON(_ context.Context, v any) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if m, ok := v.(map[string]any); ok {
		f.sent = append(f.sent, m)
	}
	return nil
}

func (f *fakeSink) SendAudio(_ context.Context, _ []byte) error { return nil }

func (f *fakeSink) InvokeCommand(_ context.Context, cmd *DeviceCommand) (*DeviceCommandResult, error) {
	return &DeviceCommandResult{CommandID: cmd.CommandID, DeviceID: cmd.DeviceID, OK: true}, nil
}

func (f *fakeSink) Close(reason string) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.closed = reason
	return nil
}

func TestMacAuthNormalizeAndAuthorize(t *testing.T) {
	ctx := context.Background()
	repo := newMemMacRepo()
	uc := NewMacAuthUsecase(repo, log.DefaultLogger)

	if _, err := uc.Upsert(ctx, &AllowedMac{
		MAC:     "30-ED-A0-F3-DC-70",
		GroupID: "home-default",
		Zone:    "study",
		Enabled: true,
	}); err != nil {
		t.Fatalf("upsert failed: %v", err)
	}

	entry, err := uc.Authorize(ctx, "30eda0f3dc70")
	if err != nil {
		t.Fatalf("expected authorize success, got %v", err)
	}
	if entry.MAC != "30:ed:a0:f3:dc:70" {
		t.Fatalf("unexpected normalized mac: %s", entry.MAC)
	}

	if _, err := uc.Authorize(ctx, "aa:bb:cc:dd:ee:ff"); !errors.Is(err, ErrMacNotAllowed) {
		t.Fatalf("expected ErrMacNotAllowed, got %v", err)
	}

	if _, err := uc.Upsert(ctx, &AllowedMac{
		MAC:     "30:ed:a0:f3:dc:70",
		Enabled: false,
	}); err != nil {
		t.Fatalf("disable mac failed: %v", err)
	}
	if _, err := uc.Authorize(ctx, "30:ed:a0:f3:dc:70"); !errors.Is(err, ErrMacNotAllowed) {
		t.Fatalf("expected disabled mac to fail with ErrMacNotAllowed, got %v", err)
	}
}

func TestDeviceUsecaseSingleMacMutex(t *testing.T) {
	ctx := context.Background()
	devRepo := newMemDeviceRepo()
	aclUC := NewACLUsecase(&memACLRepo{}, log.DefaultLogger)
	uc := NewDeviceUsecase(devRepo, aclUC, nil, log.DefaultLogger)

	sink1 := &fakeSink{}
	conn1, err := uc.RegisterConnection(ctx, &DeviceHello{
		MAC:     "30:ed:a0:f3:dc:70",
		GroupID: "home-default",
		Zone:    "study",
	}, sink1)
	if err != nil {
		t.Fatalf("register conn1: %v", err)
	}

	sink2 := &fakeSink{}
	conn2, err := uc.RegisterConnection(ctx, &DeviceHello{
		MAC:     "30:ed:a0:f3:dc:70",
		GroupID: "home-default",
		Zone:    "study",
	}, sink2)
	if err != nil {
		t.Fatalf("register conn2: %v", err)
	}

	if sink1.closed == "" {
		t.Fatalf("expected conn1 sink to be closed when replaced by conn2")
	}
	if conn2 == conn1 {
		t.Fatalf("expected distinct connection ids")
	}

	// Old connection disconnect must NOT mark device offline.
	uc.Disconnect(ctx, "30:ed:a0:f3:dc:70", conn1)
	sh, err := uc.GetShadow(ctx, "30:ed:a0:f3:dc:70")
	if err != nil || !sh.Online {
		t.Fatalf("expected device to stay online after stale conn1 disconnect")
	}

	// Current connection disconnect marks device offline.
	uc.Disconnect(ctx, "30:ed:a0:f3:dc:70", conn2)
	sh, err = uc.GetShadow(ctx, "30:ed:a0:f3:dc:70")
	if err != nil || sh.Online {
		t.Fatalf("expected device offline after conn2 disconnect")
	}
}

func TestWakeArbiterWinnerSelection(t *testing.T) {
	ctx := context.Background()
	devRepo := newMemDeviceRepo()
	aclUC := NewACLUsecase(&memACLRepo{}, log.DefaultLogger)
	devUC := NewDeviceUsecase(devRepo, aclUC, nil, log.DefaultLogger)

	sinkStudy := &fakeSink{}
	sinkLiving := &fakeSink{}
	_, _ = devUC.RegisterConnection(ctx, &DeviceHello{MAC: "30:ed:a0:f3:dc:70", GroupID: "home-default", Zone: "study"}, sinkStudy)
	_, _ = devUC.RegisterConnection(ctx, &DeviceHello{MAC: "30:ed:a0:f3:dc:71", GroupID: "home-default", Zone: "living_room"}, sinkLiving)

	arb := NewWakeArbiter(30*time.Millisecond, devUC, nil, log.DefaultLogger)
	defer arb.Close()

	arb.Submit(ctx, WakeCandidate{
		DeviceID:     "30:ed:a0:f3:dc:70",
		GroupID:      "home-default",
		Zone:         "study",
		WakeScore:    0.85,
		SNRDB:        8.0,
		DetectedAtMS: 1000,
	})
	arb.Submit(ctx, WakeCandidate{
		DeviceID:     "30:ed:a0:f3:dc:71",
		GroupID:      "home-default",
		Zone:         "living_room",
		WakeScore:    0.92,
		SNRDB:        18.0,
		DetectedAtMS: 1005,
	})

	time.Sleep(80 * time.Millisecond)

	sinkLiving.mu.Lock()
	defer sinkLiving.mu.Unlock()
	sinkStudy.mu.Lock()
	defer sinkStudy.mu.Unlock()

	if len(sinkLiving.sent) != 1 || sinkLiving.sent[0]["type"] != "wake.accept" {
		t.Fatalf("expected living_room to receive wake.accept, got %+v", sinkLiving.sent)
	}
	if len(sinkStudy.sent) != 1 || sinkStudy.sent[0]["type"] != "wake.suppress" {
		t.Fatalf("expected study to receive wake.suppress, got %+v", sinkStudy.sent)
	}
}

func TestACLUsecaseAndSessionRouter(t *testing.T) {
	ctx := context.Background()
	aclRepo := &memACLRepo{}
	aclUC := NewACLUsecase(aclRepo, log.DefaultLogger)

	_, _ = aclUC.Upsert(ctx, &ACLRule{
		RuleID:            "allow-butler-all",
		AgentID:           "butler",
		GroupID:           "home-default",
		ZonePattern:       "*",
		CapabilityPattern: "device.*",
		MinSpeakerRole:    SpeakerRoleMember,
		Allow:             true,
	})
	_, _ = aclUC.Upsert(ctx, &ACLRule{
		RuleID:            "deny-butler-lock",
		AgentID:           "butler",
		GroupID:           "home-default",
		ZonePattern:       "entrance",
		CapabilityPattern: "device.actuator_set",
		Allow:             false,
	})

	entranceDev := &DeviceHello{DeviceID: "30:ed:a0:f3:dc:72", GroupID: "home-default", Zone: "entrance"}
	if err := aclUC.CheckAccess(ctx, "butler", SpeakerRoleOwner, entranceDev, "device.camera_capture"); err != nil {
		t.Fatalf("expected camera_capture allowed for owner, got %v", err)
	}
	if err := aclUC.CheckAccess(ctx, "butler", SpeakerRoleGuest, entranceDev, "device.camera_capture"); !errors.Is(err, ErrACLDenied) {
		t.Fatalf("expected guest denied, got %v", err)
	}
	if err := aclUC.CheckAccess(ctx, "butler", SpeakerRoleOwner, entranceDev, "device.actuator_set"); !errors.Is(err, ErrACLDenied) {
		t.Fatalf("expected explicit deny on entrance actuator_set, got %v", err)
	}

	router := NewSessionRouter(nil)
	r1 := router.ResolveTurn(ctx, "alice", "butler", "30:ed:a0:f3:dc:71")
	r2 := router.ResolveTurn(ctx, "alice", "butler", "30:ed:a0:f3:dc:70")
	if r1.SessionID != r2.SessionID {
		t.Fatalf("expected session handoff to preserve session_id: %s vs %s", r1.SessionID, r2.SessionID)
	}
	if got := router.ResolveCallbackDevice("alice", "30:ed:a0:f3:dc:71"); got != "30:ed:a0:f3:dc:70" {
		t.Fatalf("expected last active device to follow to study, got %s", got)
	}
}

func TestSpeechFilterChunkerAndArbitrator(t *testing.T) {
	raw := "请看[报告](https://example.com)结果：\n```go\nfmt.Println(1)\n```\n已经完成。"
	spoken := SpokenFilter(raw)
	if strings.Contains(spoken, "```") || strings.Contains(spoken, "https://") {
		t.Fatalf("expected code block and URL stripped from spoken output, got %q", spoken)
	}
	if !strings.Contains(spoken, "报告") || !strings.Contains(spoken, "详情已发送到屏幕") {
		t.Fatalf("expected link label and screen hint preserved, got %q", spoken)
	}

	var chunker SentenceChunker
	if sents := chunker.Push("今天天气"); len(sents) != 0 {
		t.Fatalf("expected no cut before punctuation, got %v", sents)
	}
	sents := chunker.Push("不错。准备出门")
	if len(sents) != 1 || sents[0] != "今天天气不错。" {
		t.Fatalf("expected first sentence flush, got %v", sents)
	}
	if rem := chunker.Flush(); rem != "准备出门" {
		t.Fatalf("expected remainder flush, got %q", rem)
	}

	// Verify decimals, short prefixes, and long comma clauses behave smoothly.
	if decSents := chunker.Push("今天气温12."); len(decSents) != 0 {
		t.Fatalf("expected pending decimal not to split early, got %v", decSents)
	}
	if decSents := chunker.Push("5度，体感舒适。"); len(decSents) != 1 || decSents[0] != "今天气温12.5度，体感舒适。" {
		t.Fatalf("expected decimal kept together, got %v", decSents)
	}
	if prefixSents := chunker.Push("好的。今天宁波晴转多云。"); len(prefixSents) != 1 || prefixSents[0] != "好的。今天宁波晴转多云。" {
		t.Fatalf("expected short prefix merged with next sentence, got %v", prefixSents)
	}
	longClause := "今天宁波天气非常不错，空气质量良好适合户外散步，下午温度大约在二十二度左右，" +
		"晚上可能会有微风吹过但整体依然舒适，建议出门的时候带一件轻薄外套以备降温"
	longSents := chunker.Push(longClause)
	if len(longSents) != 1 || !strings.HasSuffix(longSents[0], "，") {
		t.Fatalf("expected long unpunctuated clause split on comma boundary, got %v", longSents)
	}
	if !strings.Contains(longSents[0]+chunker.Flush(), "建议出门") {
		t.Fatalf("expected UTF-8 intact remainder after comma split")
	}

	arb := NewSpeakerArbitrator()
	dev := "30:ed:a0:f3:dc:70"
	if !arb.Acquire(dev, "turn_conv", AudioPriorityConversation) {
		t.Fatalf("expected conversation acquire to succeed")
	}
	if arb.Acquire(dev, "turn_bg", AudioPriorityNotification) {
		t.Fatalf("expected lower-priority notification rejected during conversation")
	}
	if !arb.Acquire(dev, "turn_alert", AudioPriorityAlert) {
		t.Fatalf("expected P0 alert to preempt P1 conversation")
	}
	arb.Release(dev, "turn_alert")
	if !arb.Acquire(dev, "turn_bg", AudioPriorityNotification) {
		t.Fatalf("expected notification allowed after alert released")
	}
}

func TestDeviceUsecaseReconcilesStaleOnlineShadow(t *testing.T) {
	ctx := context.Background()
	repo := newMemDeviceRepo()
	acl := NewACLUsecase(&memACLRepo{}, log.DefaultLogger)
	uc := NewDeviceUsecase(repo, acl, nil, log.DefaultLogger)

	mac := "30:ed:a0:f4:5b:f0"
	_ = repo.SaveHello(ctx, &DeviceHello{
		DeviceID: mac,
		MAC:      mac,
		GroupID:  "home-default",
		Zone:     "study",
	})
	_ = repo.SaveShadow(ctx, &DeviceShadow{
		DeviceID: mac,
		GroupID:  "home-default",
		Zone:     "study",
		Online:   true, // stale persisted state after ungraceful shutdown
	})

	sh, err := uc.GetShadow(ctx, mac)
	if err != nil {
		t.Fatalf("GetShadow: %v", err)
	}
	if sh.Online {
		t.Fatalf("expected GetShadow to reconcile Online=false when disconnected")
	}

	all, err := uc.ListDevices(ctx, "", false)
	if err != nil || len(all) != 1 || all[0].Shadow.Online {
		t.Fatalf("expected ListDevices(onlineOnly=false) to reconcile Online=false, got %+v err=%v", all, err)
	}

	online, err := uc.ListDevices(ctx, "", true)
	if err != nil || len(online) != 0 {
		t.Fatalf("expected ListDevices(onlineOnly=true) to exclude disconnected ghost, got %+v err=%v", online, err)
	}
}
