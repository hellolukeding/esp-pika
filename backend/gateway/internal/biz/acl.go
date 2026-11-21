package biz

import (
	"context"
	"fmt"
	"strings"
	"sync"

	"github.com/go-kratos/kratos/v2/errors"
	"github.com/go-kratos/kratos/v2/log"
)

// ErrACLDenied is returned when an Agent lacks permission to invoke a capability on a device.
var ErrACLDenied = errors.Forbidden("ACL_DENIED", "agent is not permitted to invoke device capability")

// SpeakerRole represents the verified identity tier of the speaker.
type SpeakerRole int32

const (
	SpeakerRoleUnspecified SpeakerRole = 0
	SpeakerRoleOwner       SpeakerRole = 1
	SpeakerRoleMember      SpeakerRole = 2
	SpeakerRoleGuest       SpeakerRole = 3
)

func roleRank(role SpeakerRole) int {
	switch role {
	case SpeakerRoleOwner:
		return 3
	case SpeakerRoleMember:
		return 2
	case SpeakerRoleGuest:
		return 1
	default:
		return 0
	}
}

// ACLRule defines an Agent-to-Device capability access rule.
type ACLRule struct {
	RuleID            string
	AgentID           string
	GroupID           string
	ZonePattern       string
	DeviceID          string
	CapabilityPattern string
	MinSpeakerRole    SpeakerRole
	Allow             bool
}

// ACLRepo persists ACL rules.
type ACLRepo interface {
	Upsert(ctx context.Context, rule *ACLRule) (*ACLRule, error)
	Delete(ctx context.Context, ruleID string) (bool, error)
	List(ctx context.Context, agentID string) ([]*ACLRule, error)
}

// ACLUsecase enforces multi-terminal Agent -> Device access control.
type ACLUsecase struct {
	repo ACLRepo
	log  *log.Helper
	mu   sync.RWMutex
}

// NewACLUsecase constructs an ACLUsecase.
func NewACLUsecase(repo ACLRepo, logger log.Logger) *ACLUsecase {
	return &ACLUsecase{
		repo: repo,
		log:  log.NewHelper(logger),
	}
}

// Upsert stores or updates an ACL rule.
func (uc *ACLUsecase) Upsert(ctx context.Context, rule *ACLRule) (*ACLRule, error) {
	if strings.TrimSpace(rule.RuleID) == "" || strings.TrimSpace(rule.AgentID) == "" {
		return nil, errors.BadRequest("INVALID_ACL_RULE", "rule_id and agent_id are required")
	}
	return uc.repo.Upsert(ctx, rule)
}

// Delete removes an ACL rule by ID.
func (uc *ACLUsecase) Delete(ctx context.Context, ruleID string) (bool, error) {
	return uc.repo.Delete(ctx, ruleID)
}

// List returns ACL rules optionally filtered by agentID.
func (uc *ACLUsecase) List(ctx context.Context, agentID string) ([]*ACLRule, error) {
	return uc.repo.List(ctx, agentID)
}

// CheckAccess verifies whether agentID acting for speakerRole can invoke capability on targetDevice.
// Explicit deny rules take precedence; if any matching allow rule satisfies min_speaker_role, access is granted.
// When no rules are configured for an agent, access defaults to deny-by-default unless an explicit wildcard allow rule exists.
func (uc *ACLUsecase) CheckAccess(ctx context.Context, agentID string, speakerRole SpeakerRole, device *DeviceHello, capability string) error {
	rules, err := uc.repo.List(ctx, agentID)
	if err != nil {
		return fmt.Errorf("load acl rules for agent %s: %w", agentID, err)
	}

	allowed := false
	for _, r := range rules {
		if !matchRule(r, device, capability) {
			continue
		}
		if !r.Allow {
			uc.log.WithContext(ctx).Warnf("acl explicit deny rule=%s agent=%s device=%s cap=%s", r.RuleID, agentID, device.DeviceID, capability)
			return ErrACLDenied
		}
		if roleRank(speakerRole) >= roleRank(r.MinSpeakerRole) {
			allowed = true
		}
	}
	if !allowed {
		uc.log.WithContext(ctx).Warnf("acl denied agent=%s role=%d device=%s cap=%s", agentID, speakerRole, device.DeviceID, capability)
		return ErrACLDenied
	}
	return nil
}

func matchRule(r *ACLRule, device *DeviceHello, capability string) bool {
	if r.GroupID != "" && r.GroupID != "*" && r.GroupID != device.GroupID {
		return false
	}
	if r.DeviceID != "" && r.DeviceID != "*" && !strings.EqualFold(r.DeviceID, device.DeviceID) {
		return false
	}
	if !matchGlob(r.ZonePattern, device.Zone) {
		return false
	}
	if !matchGlob(r.CapabilityPattern, capability) {
		return false
	}
	return true
}

func matchGlob(pattern, value string) bool {
	if pattern == "" || pattern == "*" {
		return true
	}
	if strings.HasSuffix(pattern, "*") {
		prefix := strings.TrimSuffix(pattern, "*")
		return strings.HasPrefix(value, prefix)
	}
	return pattern == value
}
