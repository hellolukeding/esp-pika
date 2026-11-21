package service

import (
	"context"
	"strings"

	devicev1 "esp-pika/backend/gateway/api/device/v1"
	gatewayv1 "esp-pika/backend/gateway/api/gateway/v1"
	"esp-pika/backend/gateway/internal/biz"

	"github.com/go-kratos/kratos/v2/errors"
	"github.com/go-kratos/kratos/v2/log"
	"google.golang.org/protobuf/types/known/timestamppb"
)

// GatewayAdminService implements Kratos gRPC and HTTP administration endpoints.
type GatewayAdminService struct {
	gatewayv1.UnimplementedGatewayAdminServiceServer

	macAuth  *biz.MacAuthUsecase
	devices  *biz.DeviceUsecase
	speakers *biz.SpeakerUsecase
	acl      *biz.ACLUsecase
	log      *log.Helper
}

// NewGatewayAdminService constructs a GatewayAdminService.
func NewGatewayAdminService(
	macAuth *biz.MacAuthUsecase,
	devices *biz.DeviceUsecase,
	speakers *biz.SpeakerUsecase,
	acl *biz.ACLUsecase,
	logger log.Logger,
) *GatewayAdminService {
	return &GatewayAdminService{
		macAuth:  macAuth,
		devices:  devices,
		speakers: speakers,
		acl:      acl,
		log:      log.NewHelper(logger),
	}
}

func (s *GatewayAdminService) UpsertAllowedMac(ctx context.Context, req *gatewayv1.UpsertAllowedMacRequest) (*gatewayv1.AllowedMacEntry, error) {
	saved, err := s.macAuth.Upsert(ctx, &biz.AllowedMac{
		MAC:     req.GetMac(),
		GroupID: req.GetGroupId(),
		Zone:    req.GetZone(),
		Label:   req.GetLabel(),
		Enabled: req.GetEnabled(),
	})
	if err != nil {
		return nil, err
	}
	return toProtoAllowedMac(saved), nil
}

func (s *GatewayAdminService) DeleteAllowedMac(ctx context.Context, req *gatewayv1.DeleteAllowedMacRequest) (*gatewayv1.DeleteAllowedMacResponse, error) {
	deleted, err := s.macAuth.Delete(ctx, req.GetMac())
	if err != nil {
		return nil, err
	}
	return &gatewayv1.DeleteAllowedMacResponse{Deleted: deleted}, nil
}

func (s *GatewayAdminService) ListAllowedMacs(ctx context.Context, req *gatewayv1.ListAllowedMacsRequest) (*gatewayv1.ListAllowedMacsResponse, error) {
	items, err := s.macAuth.List(ctx, req.GetGroupId())
	if err != nil {
		return nil, err
	}
	out := make([]*gatewayv1.AllowedMacEntry, 0, len(items))
	for _, it := range items {
		out = append(out, toProtoAllowedMac(it))
	}
	return &gatewayv1.ListAllowedMacsResponse{Items: out}, nil
}

func (s *GatewayAdminService) ListDevices(ctx context.Context, req *gatewayv1.ListDevicesRequest) (*gatewayv1.ListDevicesResponse, error) {
	items, err := s.devices.ListDevices(ctx, req.GetGroupId(), req.GetOnlineOnly())
	if err != nil {
		return nil, err
	}
	out := make([]*gatewayv1.DeviceSummary, 0, len(items))
	for _, d := range items {
		out = append(out, &gatewayv1.DeviceSummary{
			Hello:  toProtoHello(d.Hello),
			Shadow: toProtoShadow(d.Shadow),
		})
	}
	return &gatewayv1.ListDevicesResponse{Devices: out}, nil
}

func (s *GatewayAdminService) GetDeviceShadow(ctx context.Context, req *gatewayv1.GetDeviceShadowRequest) (*devicev1.DeviceShadow, error) {
	sh, err := s.devices.GetShadow(ctx, req.GetDeviceId())
	if err != nil {
		return nil, err
	}
	return toProtoShadow(sh), nil
}

func (s *GatewayAdminService) InvokeDeviceCommand(ctx context.Context, req *gatewayv1.InvokeDeviceCommandRequest) (*devicev1.DeviceCommandResult, error) {
	agentID := strings.TrimSpace(req.GetAgentId())
	if agentID == "" {
		return nil, errors.BadRequest("INVALID_ARGUMENT", "agent_id is required")
	}
	res, err := s.devices.InvokeCommand(ctx, agentID, biz.SpeakerRoleOwner, req.GetDeviceId(), &biz.DeviceCommand{
		Capability: req.GetCapability(),
		Action:     req.GetAction(),
		ParamsJSON: req.GetParamsJson(),
		TimeoutMS:  req.GetTimeoutMs(),
	})
	if err != nil {
		return nil, err
	}
	return &devicev1.DeviceCommandResult{
		CommandId:    res.CommandID,
		DeviceId:     res.DeviceID,
		Ok:           res.OK,
		ErrorCode:    res.ErrorCode,
		ErrorMessage: res.ErrorMessage,
		ResultJson:   res.ResultJSON,
		MediaUrl:     res.MediaURL,
	}, nil
}

func (s *GatewayAdminService) UpsertSpeakerGroup(ctx context.Context, req *gatewayv1.UpsertSpeakerGroupRequest) (*gatewayv1.SpeakerGroup, error) {
	saved, err := s.speakers.Upsert(ctx, &biz.SpeakerGroup{
		SpeakerID:   req.GetSpeakerId(),
		UID:         req.GetUid(),
		AgentID:     req.GetAgentId(),
		GroupID:     req.GetGroupId(),
		DisplayName: req.GetDisplayName(),
		Role:        biz.SpeakerRole(req.GetRole()),
		Prompt:      req.GetPrompt(),
		TTSVoice:    req.GetTtsVoice(),
	})
	if err != nil {
		return nil, err
	}
	return toProtoSpeaker(saved), nil
}

func (s *GatewayAdminService) ListSpeakerGroups(ctx context.Context, req *gatewayv1.ListSpeakerGroupsRequest) (*gatewayv1.ListSpeakerGroupsResponse, error) {
	items, err := s.speakers.List(ctx, req.GetGroupId())
	if err != nil {
		return nil, err
	}
	out := make([]*gatewayv1.SpeakerGroup, 0, len(items))
	for _, it := range items {
		out = append(out, toProtoSpeaker(it))
	}
	return &gatewayv1.ListSpeakerGroupsResponse{Speakers: out}, nil
}

func (s *GatewayAdminService) DeleteSpeakerGroup(ctx context.Context, req *gatewayv1.DeleteSpeakerGroupRequest) (*gatewayv1.DeleteSpeakerGroupResponse, error) {
	deleted, err := s.speakers.Delete(ctx, req.GetSpeakerId())
	if err != nil {
		return nil, err
	}
	return &gatewayv1.DeleteSpeakerGroupResponse{Deleted: deleted}, nil
}

func (s *GatewayAdminService) UpsertACLRule(ctx context.Context, req *gatewayv1.UpsertACLRuleRequest) (*gatewayv1.ACLRule, error) {
	saved, err := s.acl.Upsert(ctx, &biz.ACLRule{
		RuleID:            req.GetRuleId(),
		AgentID:           req.GetAgentId(),
		GroupID:           req.GetGroupId(),
		ZonePattern:       req.GetZonePattern(),
		DeviceID:          req.GetDeviceId(),
		CapabilityPattern: req.GetCapabilityPattern(),
		MinSpeakerRole:    biz.SpeakerRole(req.GetMinSpeakerRole()),
		Allow:             req.GetAllow(),
	})
	if err != nil {
		return nil, err
	}
	return toProtoACL(saved), nil
}

func (s *GatewayAdminService) ListACLRules(ctx context.Context, req *gatewayv1.ListACLRulesRequest) (*gatewayv1.ListACLRulesResponse, error) {
	items, err := s.acl.List(ctx, req.GetAgentId())
	if err != nil {
		return nil, err
	}
	out := make([]*gatewayv1.ACLRule, 0, len(items))
	for _, it := range items {
		out = append(out, toProtoACL(it))
	}
	return &gatewayv1.ListACLRulesResponse{Rules: out}, nil
}

func (s *GatewayAdminService) DeleteACLRule(ctx context.Context, req *gatewayv1.DeleteACLRuleRequest) (*gatewayv1.DeleteACLRuleResponse, error) {
	deleted, err := s.acl.Delete(ctx, req.GetRuleId())
	if err != nil {
		return nil, err
	}
	return &gatewayv1.DeleteACLRuleResponse{Deleted: deleted}, nil
}

func toProtoAllowedMac(e *biz.AllowedMac) *gatewayv1.AllowedMacEntry {
	if e == nil {
		return nil
	}
	return &gatewayv1.AllowedMacEntry{
		Mac:       e.MAC,
		GroupId:   e.GroupID,
		Zone:      e.Zone,
		Label:     e.Label,
		Enabled:   e.Enabled,
		CreatedAt: timestamppb.New(e.CreatedAt),
		UpdatedAt: timestamppb.New(e.UpdatedAt),
	}
}

func toProtoHello(h *biz.DeviceHello) *devicev1.DeviceHello {
	if h == nil {
		return nil
	}
	caps := make([]*devicev1.DeviceCapability, 0, len(h.Capabilities))
	for _, c := range h.Capabilities {
		caps = append(caps, &devicev1.DeviceCapability{
			Name:            c.Name,
			Type:            parseCapType(c.Type),
			Description:     c.Description,
			UploadChannel:   c.UploadChannel,
			Fields:          c.Fields,
			InputSchemaJson: c.InputSchemaJSON,
		})
	}
	return &devicev1.DeviceHello{
		DeviceId:        h.DeviceID,
		Mac:             h.MAC,
		GroupId:         h.GroupID,
		Zone:            h.Zone,
		Chip:            parseChipType(h.Chip),
		VpVersion:       h.VPVersion,
		FirmwareVersion: h.FirmwareVersion,
		Capabilities:    caps,
	}
}

func toProtoShadow(sh *biz.DeviceShadow) *devicev1.DeviceShadow {
	if sh == nil {
		return nil
	}
	return &devicev1.DeviceShadow{
		DeviceId:       sh.DeviceID,
		GroupId:        sh.GroupID,
		Zone:           sh.Zone,
		Online:         sh.Online,
		BatteryPercent: sh.BatteryPercent,
		Rssi:           sh.RSSI,
		Volume:         sh.Volume,
		VadState:       sh.VADState,
		Metrics:        sh.Metrics,
		Attributes:     sh.Attributes,
		UpdatedAt:      timestamppb.New(sh.UpdatedAt),
	}
}

func toProtoSpeaker(sp *biz.SpeakerGroup) *gatewayv1.SpeakerGroup {
	if sp == nil {
		return nil
	}
	return &gatewayv1.SpeakerGroup{
		SpeakerId:   sp.SpeakerID,
		Uid:         sp.UID,
		AgentId:     sp.AgentID,
		GroupId:     sp.GroupID,
		DisplayName: sp.DisplayName,
		Role:        devicev1.SpeakerRole(sp.Role),
		Prompt:      sp.Prompt,
		TtsVoice:    sp.TTSVoice,
		Version:     sp.Version,
		UpdatedAt:   timestamppb.New(sp.UpdatedAt),
	}
}

func toProtoACL(r *biz.ACLRule) *gatewayv1.ACLRule {
	if r == nil {
		return nil
	}
	return &gatewayv1.ACLRule{
		RuleId:            r.RuleID,
		AgentId:           r.AgentID,
		GroupId:           r.GroupID,
		ZonePattern:       r.ZonePattern,
		DeviceId:          r.DeviceID,
		CapabilityPattern: r.CapabilityPattern,
		MinSpeakerRole:    devicev1.SpeakerRole(r.MinSpeakerRole),
		Allow:             r.Allow,
	}
}

func parseChipType(chip string) devicev1.ChipType {
	switch strings.ToLower(chip) {
	case "esp32s31":
		return devicev1.ChipType_CHIP_TYPE_ESP32_S31
	case "esp32s3":
		return devicev1.ChipType_CHIP_TYPE_ESP32_S3
	case "esp32p4":
		return devicev1.ChipType_CHIP_TYPE_ESP32_P4
	case "esp32c3":
		return devicev1.ChipType_CHIP_TYPE_ESP32_C3
	case "esp32c5":
		return devicev1.ChipType_CHIP_TYPE_ESP32_C5
	case "esp32c6":
		return devicev1.ChipType_CHIP_TYPE_ESP32_C6
	case "esp32":
		return devicev1.ChipType_CHIP_TYPE_ESP32
	default:
		return devicev1.ChipType_CHIP_TYPE_UNSPECIFIED
	}
}

func parseCapType(t string) devicev1.CapabilityType {
	switch strings.ToLower(t) {
	case "tool":
		return devicev1.CapabilityType_CAPABILITY_TYPE_TOOL
	case "shadow":
		return devicev1.CapabilityType_CAPABILITY_TYPE_SHADOW
	case "event":
		return devicev1.CapabilityType_CAPABILITY_TYPE_EVENT
	default:
		return devicev1.CapabilityType_CAPABILITY_TYPE_UNSPECIFIED
	}
}
