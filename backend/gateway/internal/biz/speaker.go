package biz

import (
	"context"
	"fmt"
	"strings"
	"time"

	"github.com/go-kratos/kratos/v2/errors"
	"github.com/go-kratos/kratos/v2/log"
)

// SpeakerGroup binds a registered user/speaker voiceprint to an Agent, prompt, and TTS voice.
type SpeakerGroup struct {
	SpeakerID   string
	UID         string
	AgentID     string
	GroupID     string
	DisplayName string
	Role        SpeakerRole
	Prompt      string
	TTSVoice    string
	Embedding   []float32
	Version     uint64
	UpdatedAt   time.Time
}

// VoiceprintTemplate is synced down to edge devices for on-device speaker verification.
type VoiceprintTemplate struct {
	SpeakerID   string      `json:"speaker_id"`
	DisplayName string      `json:"display_name"`
	Role        SpeakerRole `json:"role"`
	Embedding   []float32   `json:"embedding"`
	Version     uint64      `json:"version"`
}

// VoiceprintSync carries incremental or full voiceprint updates to devices in a group.
type VoiceprintSync struct {
	GroupID           string               `json:"group_id"`
	VPVersion         uint64               `json:"vp_version"`
	FullSync          bool                 `json:"full_sync"`
	Upserts           []VoiceprintTemplate `json:"upserts,omitempty"`
	DeletedSpeakerIDs []string             `json:"deleted_speaker_ids,omitempty"`
}

// SpeakerRepo persists speaker groups and monotonically versioned voiceprint templates.
type SpeakerRepo interface {
	Upsert(ctx context.Context, speaker *SpeakerGroup) (*SpeakerGroup, error)
	Delete(ctx context.Context, speakerID string) (*SpeakerGroup, error)
	Get(ctx context.Context, speakerID string) (*SpeakerGroup, error)
	List(ctx context.Context, groupID string) ([]*SpeakerGroup, error)
	GroupVersion(ctx context.Context, groupID string) (uint64, error)
}

// SpeakerUsecase manages speaker registration, metadata, and multi-terminal voiceprint sync.
type SpeakerUsecase struct {
	repo    SpeakerRepo
	devices *DeviceUsecase
	events  EventBus
	log     *log.Helper
}

// NewSpeakerUsecase constructs a SpeakerUsecase.
func NewSpeakerUsecase(repo SpeakerRepo, devices *DeviceUsecase, events EventBus, logger log.Logger) *SpeakerUsecase {
	return &SpeakerUsecase{
		repo:    repo,
		devices: devices,
		events:  events,
		log:     log.NewHelper(logger),
	}
}

// Upsert creates or updates a speaker group and broadcasts incremental sync to the group's devices.
func (uc *SpeakerUsecase) Upsert(ctx context.Context, speaker *SpeakerGroup) (*SpeakerGroup, error) {
	if strings.TrimSpace(speaker.SpeakerID) == "" {
		return nil, errors.BadRequest("INVALID_SPEAKER", "speaker_id is required")
	}
	if speaker.GroupID == "" {
		speaker.GroupID = "home-default"
	}
	if speaker.Role == SpeakerRoleUnspecified {
		speaker.Role = SpeakerRoleMember
	}
	saved, err := uc.repo.Upsert(ctx, speaker)
	if err != nil {
		return nil, fmt.Errorf("upsert speaker %s: %w", speaker.SpeakerID, err)
	}

	syncEvt := &VoiceprintSync{
		GroupID:   saved.GroupID,
		VPVersion: saved.Version,
		FullSync:  false,
		Upserts: []VoiceprintTemplate{
			{
				SpeakerID:   saved.SpeakerID,
				DisplayName: saved.DisplayName,
				Role:        saved.Role,
				Embedding:   saved.Embedding,
				Version:     saved.Version,
			},
		},
	}
	uc.broadcastSync(ctx, syncEvt)
	return saved, nil
}

// Delete removes a speaker group and broadcasts deletion to devices in the same group.
func (uc *SpeakerUsecase) Delete(ctx context.Context, speakerID string) (bool, error) {
	deleted, err := uc.repo.Delete(ctx, speakerID)
	if err != nil {
		return false, fmt.Errorf("delete speaker %s: %w", speakerID, err)
	}
	if deleted == nil {
		return false, nil
	}
	ver, _ := uc.repo.GroupVersion(ctx, deleted.GroupID)
	uc.broadcastSync(ctx, &VoiceprintSync{
		GroupID:           deleted.GroupID,
		VPVersion:         ver,
		FullSync:          false,
		DeletedSpeakerIDs: []string{speakerID},
	})
	return true, nil
}

// List returns all speaker groups optionally filtered by groupID.
func (uc *SpeakerUsecase) List(ctx context.Context, groupID string) ([]*SpeakerGroup, error) {
	return uc.repo.List(ctx, groupID)
}

// SyncDeviceIfBehind checks a newly connected device's vp_version and pushes a full sync if outdated.
func (uc *SpeakerUsecase) SyncDeviceIfBehind(ctx context.Context, deviceID, groupID string, deviceVPVersion uint64) error {
	latestVer, err := uc.repo.GroupVersion(ctx, groupID)
	if err != nil {
		return fmt.Errorf("read group vp_version: %w", err)
	}
	if latestVer <= deviceVPVersion {
		return nil
	}
	speakers, err := uc.repo.List(ctx, groupID)
	if err != nil {
		return fmt.Errorf("list group speakers: %w", err)
	}
	upserts := make([]VoiceprintTemplate, 0, len(speakers))
	for _, sp := range speakers {
		if len(sp.Embedding) == 0 {
			continue
		}
		upserts = append(upserts, VoiceprintTemplate{
			SpeakerID:   sp.SpeakerID,
			DisplayName: sp.DisplayName,
			Role:        sp.Role,
			Embedding:   sp.Embedding,
			Version:     sp.Version,
		})
	}
	return uc.devices.SendToDevice(ctx, deviceID, map[string]any{
		"type":       "voiceprint.sync",
		"group_id":   groupID,
		"vp_version": latestVer,
		"full_sync":  true,
		"upserts":    upserts,
	})
}

func (uc *SpeakerUsecase) broadcastSync(ctx context.Context, syncEvt *VoiceprintSync) {
	if uc.events != nil {
		_ = uc.events.PublishVoiceprintSync(ctx, syncEvt)
	}
	if uc.devices == nil {
		return
	}
	devs, err := uc.devices.ListDevices(ctx, syncEvt.GroupID, true)
	if err != nil {
		return
	}
	payload := map[string]any{
		"type":                "voiceprint.sync",
		"group_id":            syncEvt.GroupID,
		"vp_version":          syncEvt.VPVersion,
		"full_sync":           syncEvt.FullSync,
		"upserts":             syncEvt.Upserts,
		"deleted_speaker_ids": syncEvt.DeletedSpeakerIDs,
	}
	for _, d := range devs {
		if d.Hello == nil {
			continue
		}
		_ = uc.devices.SendToDevice(ctx, d.Hello.DeviceID, payload)
	}
}
