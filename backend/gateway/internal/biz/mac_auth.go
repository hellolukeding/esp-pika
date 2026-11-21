package biz

import (
	"context"
	"fmt"
	"net"
	"strings"
	"time"

	"github.com/go-kratos/kratos/v2/errors"
	"github.com/go-kratos/kratos/v2/log"
)

// ErrMacNotAllowed is returned when a device MAC is missing or not in the enabled whitelist.
var ErrMacNotAllowed = errors.Forbidden("MAC_NOT_ALLOWED", "device mac address is not allowed")

// ErrInvalidMac is returned when the provided MAC address cannot be parsed as a 48-bit MAC.
var ErrInvalidMac = errors.BadRequest("INVALID_MAC", "invalid 48-bit mac address")

// AllowedMac represents an authorized device MAC entry in the gateway whitelist.
type AllowedMac struct {
	MAC       string
	GroupID   string
	Zone      string
	Label     string
	Enabled   bool
	CreatedAt time.Time
	UpdatedAt time.Time
}

// AllowedMacRepo defines persistence operations for the MAC whitelist.
type AllowedMacRepo interface {
	GetByMAC(ctx context.Context, mac string) (*AllowedMac, error)
	Upsert(ctx context.Context, entry *AllowedMac) (*AllowedMac, error)
	Delete(ctx context.Context, mac string) (bool, error)
	List(ctx context.Context, groupID string) ([]*AllowedMac, error)
}

// MacAuthUsecase handles MAC normalization and strict whitelist authentication.
type MacAuthUsecase struct {
	repo AllowedMacRepo
	log  *log.Helper
}

// NewMacAuthUsecase constructs a MacAuthUsecase.
func NewMacAuthUsecase(repo AllowedMacRepo, logger log.Logger) *MacAuthUsecase {
	return &MacAuthUsecase{
		repo: repo,
		log:  log.NewHelper(logger),
	}
}

// NormalizeMAC parses a 48-bit MAC address and formats it as lowercase colon-separated hex.
func NormalizeMAC(raw string) (string, error) {
	trimmed := strings.TrimSpace(raw)
	if trimmed == "" {
		return "", ErrInvalidMac
	}
	if len(trimmed) == 12 && !strings.ContainsAny(trimmed, ":-.") {
		trimmed = fmt.Sprintf("%s:%s:%s:%s:%s:%s",
			trimmed[0:2], trimmed[2:4], trimmed[4:6],
			trimmed[6:8], trimmed[8:10], trimmed[10:12])
	}
	hw, err := net.ParseMAC(trimmed)
	if err != nil || len(hw) != 6 {
		return "", fmt.Errorf("%w: %q", ErrInvalidMac, raw)
	}
	return strings.ToLower(hw.String()), nil
}

// Authorize validates and checks whether the device MAC is enabled in the whitelist.
func (uc *MacAuthUsecase) Authorize(ctx context.Context, rawMAC string) (*AllowedMac, error) {
	mac, err := NormalizeMAC(rawMAC)
	if err != nil {
		return nil, ErrMacNotAllowed
	}
	entry, err := uc.repo.GetByMAC(ctx, mac)
	if err != nil {
		return nil, fmt.Errorf("lookup allowed mac %s: %w", mac, err)
	}
	if entry == nil || !entry.Enabled {
		uc.log.WithContext(ctx).Warnf("rejected unauthorized device mac=%s", mac)
		return nil, ErrMacNotAllowed
	}
	return entry, nil
}

// Upsert creates or updates a MAC whitelist entry.
func (uc *MacAuthUsecase) Upsert(ctx context.Context, entry *AllowedMac) (*AllowedMac, error) {
	mac, err := NormalizeMAC(entry.MAC)
	if err != nil {
		return nil, err
	}
	copyEntry := *entry
	copyEntry.MAC = mac
	if copyEntry.GroupID == "" {
		copyEntry.GroupID = "home-default"
	}
	return uc.repo.Upsert(ctx, &copyEntry)
}

// Delete removes a MAC whitelist entry.
func (uc *MacAuthUsecase) Delete(ctx context.Context, rawMAC string) (bool, error) {
	mac, err := NormalizeMAC(rawMAC)
	if err != nil {
		return false, err
	}
	return uc.repo.Delete(ctx, mac)
}

// List returns MAC whitelist entries optionally filtered by groupID.
func (uc *MacAuthUsecase) List(ctx context.Context, groupID string) ([]*AllowedMac, error) {
	return uc.repo.List(ctx, groupID)
}
