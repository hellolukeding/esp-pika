package data

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"sync"
	"time"

	"esp-pika/backend/gateway/internal/biz"
)

func natsShadowKey(deviceID string) string {
	return strings.ReplaceAll(deviceID, ":", "_")
}

type sqliteAllowedMacRepo struct {
	data *Data
}

// NewSQLiteAllowedMacRepo constructs an AllowedMacRepo backed by SQLite.
func NewSQLiteAllowedMacRepo(data *Data) biz.AllowedMacRepo {
	return &sqliteAllowedMacRepo{data: data}
}

func (r *sqliteAllowedMacRepo) GetByMAC(ctx context.Context, mac string) (*biz.AllowedMac, error) {
	row := r.data.db.QueryRowContext(ctx,
		`SELECT mac, group_id, zone, label, enabled, created_at, updated_at FROM allowed_macs WHERE mac = ?`, mac)
	var e biz.AllowedMac
	var enabled int
	err := row.Scan(&e.MAC, &e.GroupID, &e.Zone, &e.Label, &enabled, &e.CreatedAt, &e.UpdatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("query allowed_mac %s: %w", mac, err)
	}
	e.Enabled = enabled == 1
	return &e, nil
}

func (r *sqliteAllowedMacRepo) Upsert(ctx context.Context, entry *biz.AllowedMac) (*biz.AllowedMac, error) {
	now := time.Now().UTC()
	enabled := 0
	if entry.Enabled {
		enabled = 1
	}
	_, err := r.data.db.ExecContext(ctx, `
		INSERT INTO allowed_macs (mac, group_id, zone, label, enabled, created_at, updated_at)
		VALUES (?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(mac) DO UPDATE SET
			group_id = excluded.group_id,
			zone = excluded.zone,
			label = excluded.label,
			enabled = excluded.enabled,
			updated_at = excluded.updated_at
	`, entry.MAC, entry.GroupID, entry.Zone, entry.Label, enabled, now, now)
	if err != nil {
		return nil, fmt.Errorf("upsert allowed_mac %s: %w", entry.MAC, err)
	}
	return r.GetByMAC(ctx, entry.MAC)
}

func (r *sqliteAllowedMacRepo) Delete(ctx context.Context, mac string) (bool, error) {
	res, err := r.data.db.ExecContext(ctx, `DELETE FROM allowed_macs WHERE mac = ?`, mac)
	if err != nil {
		return false, fmt.Errorf("delete allowed_mac %s: %w", mac, err)
	}
	n, _ := res.RowsAffected()
	return n > 0, nil
}

func (r *sqliteAllowedMacRepo) List(ctx context.Context, groupID string) ([]*biz.AllowedMac, error) {
	query := `SELECT mac, group_id, zone, label, enabled, created_at, updated_at FROM allowed_macs`
	var args []any
	if groupID != "" {
		query += ` WHERE group_id = ?`
		args = append(args, groupID)
	}
	query += ` ORDER BY mac ASC`

	rows, err := r.data.db.QueryContext(ctx, query, args...)
	if err != nil {
		return nil, fmt.Errorf("list allowed_macs: %w", err)
	}
	defer rows.Close()

	var items []*biz.AllowedMac
	for rows.Next() {
		var e biz.AllowedMac
		var enabled int
		if err := rows.Scan(&e.MAC, &e.GroupID, &e.Zone, &e.Label, &enabled, &e.CreatedAt, &e.UpdatedAt); err != nil {
			return nil, fmt.Errorf("scan allowed_mac: %w", err)
		}
		e.Enabled = enabled == 1
		items = append(items, &e)
	}
	return items, rows.Err()
}

type sqliteACLRepo struct {
	data *Data
}

// NewSQLiteACLRepo constructs an ACLRepo backed by SQLite.
func NewSQLiteACLRepo(data *Data) biz.ACLRepo {
	return &sqliteACLRepo{data: data}
}

func (r *sqliteACLRepo) Upsert(ctx context.Context, rule *biz.ACLRule) (*biz.ACLRule, error) {
	allow := 0
	if rule.Allow {
		allow = 1
	}
	_, err := r.data.db.ExecContext(ctx, `
		INSERT INTO acl_rules (rule_id, agent_id, group_id, zone_pattern, device_id, capability_pattern, min_speaker_role, allow)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(rule_id) DO UPDATE SET
			agent_id = excluded.agent_id,
			group_id = excluded.group_id,
			zone_pattern = excluded.zone_pattern,
			device_id = excluded.device_id,
			capability_pattern = excluded.capability_pattern,
			min_speaker_role = excluded.min_speaker_role,
			allow = excluded.allow
	`, rule.RuleID, rule.AgentID, rule.GroupID, rule.ZonePattern, rule.DeviceID, rule.CapabilityPattern, int(rule.MinSpeakerRole), allow)
	if err != nil {
		return nil, fmt.Errorf("upsert acl_rule %s: %w", rule.RuleID, err)
	}
	copyRule := *rule
	return &copyRule, nil
}

func (r *sqliteACLRepo) Delete(ctx context.Context, ruleID string) (bool, error) {
	res, err := r.data.db.ExecContext(ctx, `DELETE FROM acl_rules WHERE rule_id = ?`, ruleID)
	if err != nil {
		return false, fmt.Errorf("delete acl_rule %s: %w", ruleID, err)
	}
	n, _ := res.RowsAffected()
	return n > 0, nil
}

func (r *sqliteACLRepo) List(ctx context.Context, agentID string) ([]*biz.ACLRule, error) {
	query := `SELECT rule_id, agent_id, group_id, zone_pattern, device_id, capability_pattern, min_speaker_role, allow FROM acl_rules`
	var args []any
	if agentID != "" {
		query += ` WHERE agent_id = ? OR agent_id = '*'`
		args = append(args, agentID)
	}
	rows, err := r.data.db.QueryContext(ctx, query, args...)
	if err != nil {
		return nil, fmt.Errorf("list acl_rules: %w", err)
	}
	defer rows.Close()

	var rules []*biz.ACLRule
	for rows.Next() {
		var rule biz.ACLRule
		var roleInt int
		var allow int
		if err := rows.Scan(&rule.RuleID, &rule.AgentID, &rule.GroupID, &rule.ZonePattern, &rule.DeviceID, &rule.CapabilityPattern, &roleInt, &allow); err != nil {
			return nil, fmt.Errorf("scan acl_rule: %w", err)
		}
		rule.MinSpeakerRole = biz.SpeakerRole(roleInt)
		rule.Allow = allow == 1
		rules = append(rules, &rule)
	}
	return rules, rows.Err()
}

type sqliteSpeakerRepo struct {
	data *Data
	mu   sync.Mutex
}

// NewSQLiteSpeakerRepo constructs a SpeakerRepo backed by SQLite.
func NewSQLiteSpeakerRepo(data *Data) biz.SpeakerRepo {
	return &sqliteSpeakerRepo{data: data}
}

func (r *sqliteSpeakerRepo) Upsert(ctx context.Context, speaker *biz.SpeakerGroup) (*biz.SpeakerGroup, error) {
	r.mu.Lock()
	defer r.mu.Unlock()

	maxVer, err := r.GroupVersion(ctx, speaker.GroupID)
	if err != nil {
		return nil, err
	}
	nextVer := maxVer + 1
	now := time.Now().UTC()
	embBytes, err := json.Marshal(speaker.Embedding)
	if err != nil {
		return nil, fmt.Errorf("marshal embedding: %w", err)
	}

	_, err = r.data.db.ExecContext(ctx, `
		INSERT INTO speaker_groups (speaker_id, uid, agent_id, group_id, display_name, role, prompt, tts_voice, embedding_json, version, updated_at)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(speaker_id) DO UPDATE SET
			uid = excluded.uid,
			agent_id = excluded.agent_id,
			group_id = excluded.group_id,
			display_name = excluded.display_name,
			role = excluded.role,
			prompt = excluded.prompt,
			tts_voice = excluded.tts_voice,
			embedding_json = excluded.embedding_json,
			version = excluded.version,
			updated_at = excluded.updated_at
	`, speaker.SpeakerID, speaker.UID, speaker.AgentID, speaker.GroupID, speaker.DisplayName,
		int(speaker.Role), speaker.Prompt, speaker.TTSVoice, string(embBytes), nextVer, now)
	if err != nil {
		return nil, fmt.Errorf("upsert speaker %s: %w", speaker.SpeakerID, err)
	}
	return r.Get(ctx, speaker.SpeakerID)
}

func (r *sqliteSpeakerRepo) Delete(ctx context.Context, speakerID string) (*biz.SpeakerGroup, error) {
	existing, err := r.Get(ctx, speakerID)
	if err != nil || existing == nil {
		return nil, err
	}
	_, err = r.data.db.ExecContext(ctx, `DELETE FROM speaker_groups WHERE speaker_id = ?`, speakerID)
	if err != nil {
		return nil, fmt.Errorf("delete speaker %s: %w", speakerID, err)
	}
	return existing, nil
}

func (r *sqliteSpeakerRepo) Get(ctx context.Context, speakerID string) (*biz.SpeakerGroup, error) {
	row := r.data.db.QueryRowContext(ctx, `
		SELECT speaker_id, uid, agent_id, group_id, display_name, role, prompt, tts_voice, embedding_json, version, updated_at
		FROM speaker_groups WHERE speaker_id = ?`, speakerID)
	return scanSpeakerRow(row)
}

func (r *sqliteSpeakerRepo) List(ctx context.Context, groupID string) ([]*biz.SpeakerGroup, error) {
	query := `SELECT speaker_id, uid, agent_id, group_id, display_name, role, prompt, tts_voice, embedding_json, version, updated_at FROM speaker_groups`
	var args []any
	if groupID != "" {
		query += ` WHERE group_id = ?`
		args = append(args, groupID)
	}
	query += ` ORDER BY speaker_id ASC`
	rows, err := r.data.db.QueryContext(ctx, query, args...)
	if err != nil {
		return nil, fmt.Errorf("list speakers: %w", err)
	}
	defer rows.Close()

	var items []*biz.SpeakerGroup
	for rows.Next() {
		sp, err := scanSpeakerRow(rows)
		if err != nil {
			return nil, err
		}
		items = append(items, sp)
	}
	return items, rows.Err()
}

func (r *sqliteSpeakerRepo) GroupVersion(ctx context.Context, groupID string) (uint64, error) {
	var ver sql.NullInt64
	err := r.data.db.QueryRowContext(ctx, `SELECT MAX(version) FROM speaker_groups WHERE group_id = ?`, groupID).Scan(&ver)
	if err != nil {
		return 0, fmt.Errorf("query max vp version: %w", err)
	}
	if !ver.Valid || ver.Int64 < 0 {
		return 0, nil
	}
	return uint64(ver.Int64), nil
}

type rowScanner interface {
	Scan(dest ...any) error
}

func scanSpeakerRow(s rowScanner) (*biz.SpeakerGroup, error) {
	var sp biz.SpeakerGroup
	var roleInt int
	var embJSON string
	err := s.Scan(&sp.SpeakerID, &sp.UID, &sp.AgentID, &sp.GroupID, &sp.DisplayName,
		&roleInt, &sp.Prompt, &sp.TTSVoice, &embJSON, &sp.Version, &sp.UpdatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("scan speaker: %w", err)
	}
	sp.Role = biz.SpeakerRole(roleInt)
	if embJSON != "" {
		_ = json.Unmarshal([]byte(embJSON), &sp.Embedding)
	}
	return &sp, nil
}

type deviceRepo struct {
	data *Data
}

// NewDeviceRepo constructs a DeviceRepo backed by SQLite + optional NATS JetStream KV.
func NewDeviceRepo(data *Data) biz.DeviceRepo {
	return &deviceRepo{data: data}
}

func (r *deviceRepo) SaveHello(ctx context.Context, hello *biz.DeviceHello) error {
	b, err := json.Marshal(hello)
	if err != nil {
		return fmt.Errorf("marshal device hello: %w", err)
	}
	_, err = r.data.db.ExecContext(ctx, `
		INSERT INTO devices (device_id, hello_json, updated_at)
		VALUES (?, ?, ?)
		ON CONFLICT(device_id) DO UPDATE SET
			hello_json = excluded.hello_json,
			updated_at = excluded.updated_at
	`, hello.DeviceID, string(b), time.Now().UTC())
	if err != nil {
		return fmt.Errorf("save device hello: %w", err)
	}
	return nil
}

func (r *deviceRepo) GetHello(ctx context.Context, deviceID string) (*biz.DeviceHello, error) {
	var raw string
	err := r.data.db.QueryRowContext(ctx, `SELECT hello_json FROM devices WHERE device_id = ?`, deviceID).Scan(&raw)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("get device hello: %w", err)
	}
	var h biz.DeviceHello
	if err := json.Unmarshal([]byte(raw), &h); err != nil {
		return nil, fmt.Errorf("unmarshal device hello: %w", err)
	}
	return &h, nil
}

func (r *deviceRepo) ListHellos(ctx context.Context, groupID string) ([]*biz.DeviceHello, error) {
	rows, err := r.data.db.QueryContext(ctx, `SELECT hello_json FROM devices ORDER BY device_id ASC`)
	if err != nil {
		return nil, fmt.Errorf("list device hellos: %w", err)
	}
	defer rows.Close()

	var items []*biz.DeviceHello
	for rows.Next() {
		var raw string
		if err := rows.Scan(&raw); err != nil {
			return nil, fmt.Errorf("scan device hello: %w", err)
		}
		var h biz.DeviceHello
		if err := json.Unmarshal([]byte(raw), &h); err != nil {
			continue
		}
		if groupID != "" && h.GroupID != groupID {
			continue
		}
		items = append(items, &h)
	}
	return items, rows.Err()
}

func (r *deviceRepo) SaveShadow(ctx context.Context, shadow *biz.DeviceShadow) error {
	b, err := json.Marshal(shadow)
	if err != nil {
		return fmt.Errorf("marshal shadow: %w", err)
	}
	if r.data.shadowKV != nil {
		if _, err := r.data.shadowKV.Put(ctx, natsShadowKey(shadow.DeviceID), b); err != nil {
			r.data.log.WithContext(ctx).Warnf("nats kv put shadow %s failed: %v", shadow.DeviceID, err)
		}
	}
	_, err = r.data.db.ExecContext(ctx, `
		INSERT INTO device_shadows (device_id, group_id, shadow_json, updated_at)
		VALUES (?, ?, ?, ?)
		ON CONFLICT(device_id) DO UPDATE SET
			group_id = excluded.group_id,
			shadow_json = excluded.shadow_json,
			updated_at = excluded.updated_at
	`, shadow.DeviceID, shadow.GroupID, string(b), time.Now().UTC())
	if err != nil {
		return fmt.Errorf("save shadow sqlite: %w", err)
	}
	return nil
}

func (r *deviceRepo) GetShadow(ctx context.Context, deviceID string) (*biz.DeviceShadow, error) {
	if r.data.shadowKV != nil {
		if entry, err := r.data.shadowKV.Get(ctx, natsShadowKey(deviceID)); err == nil {
			var s biz.DeviceShadow
			if err := json.Unmarshal(entry.Value(), &s); err == nil {
				return &s, nil
			}
		}
	}
	var raw string
	err := r.data.db.QueryRowContext(ctx, `SELECT shadow_json FROM device_shadows WHERE device_id = ?`, deviceID).Scan(&raw)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("get shadow sqlite: %w", err)
	}
	var s biz.DeviceShadow
	if err := json.Unmarshal([]byte(raw), &s); err != nil {
		return nil, fmt.Errorf("unmarshal shadow: %w", err)
	}
	return &s, nil
}

func (r *deviceRepo) ListShadows(ctx context.Context, groupID string) ([]*biz.DeviceShadow, error) {
	query := `SELECT shadow_json FROM device_shadows`
	var args []any
	if groupID != "" {
		query += ` WHERE group_id = ?`
		args = append(args, groupID)
	}
	query += ` ORDER BY device_id ASC`
	rows, err := r.data.db.QueryContext(ctx, query, args...)
	if err != nil {
		return nil, fmt.Errorf("list shadows: %w", err)
	}
	defer rows.Close()

	var items []*biz.DeviceShadow
	for rows.Next() {
		var raw string
		if err := rows.Scan(&raw); err != nil {
			return nil, fmt.Errorf("scan shadow: %w", err)
		}
		var s biz.DeviceShadow
		if err := json.Unmarshal([]byte(raw), &s); err != nil {
			continue
		}
		items = append(items, &s)
	}
	return items, rows.Err()
}
