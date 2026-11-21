package data

import (
	"context"
	"database/sql"
	"fmt"
	"os"
	"path/filepath"
	"time"

	"esp-pika/backend/gateway/internal/conf"

	"github.com/go-kratos/kratos/v2/log"
	"github.com/google/wire"
	"github.com/nats-io/nats.go"
	"github.com/nats-io/nats.go/jetstream"
	_ "modernc.org/sqlite"
)

// ProviderSet provides data layer repositories and clients.
var ProviderSet = wire.NewSet(
	NewData,
	NewSQLiteAllowedMacRepo,
	NewSQLiteACLRepo,
	NewSQLiteSpeakerRepo,
	NewDeviceRepo,
	NewNATSEventBus,
	NewGRPCAgentAdapter,
	NewASRProvider,
	NewTTSProvider,
	NewVoiceReviewer,
)

// Data holds shared infrastructure clients (SQLite, NATS JetStream, Shadow KV).
type Data struct {
	db       *sql.DB
	nc       *nats.Conn
	js       jetstream.JetStream
	shadowKV jetstream.KeyValue
	log      *log.Helper
}

// NewData initializes SQLite schema and optional NATS JetStream connection.
func NewData(c *conf.Data, logger log.Logger) (*Data, func(), error) {
	helper := log.NewHelper(logger)

	dbPath := "./data/gateway.db"
	if c != nil && c.Database != nil && c.Database.Source != "" {
		dbPath = c.Database.Source
	}
	if dbPath != ":memory:" {
		if err := os.MkdirAll(filepath.Dir(dbPath), 0o755); err != nil {
			return nil, nil, fmt.Errorf("create db directory: %w", err)
		}
	}

	db, err := sql.Open("sqlite", dbPath)
	if err != nil {
		return nil, nil, fmt.Errorf("open sqlite %s: %w", dbPath, err)
	}
	if err := initSchema(db); err != nil {
		_ = db.Close()
		return nil, nil, fmt.Errorf("init sqlite schema: %w", err)
	}

	d := &Data{
		db:  db,
		log: helper,
	}

	if c != nil && c.Nats != nil && c.Nats.Url != "" {
		nc, err := nats.Connect(
			c.Nats.Url,
			nats.Name("esp-pika-gateway"),
			nats.MaxReconnects(-1),
			nats.ReconnectWait(2*time.Second),
		)
		if err != nil {
			helper.Warnf("nats connect %s failed (running in local degraded mode): %v", c.Nats.Url, err)
		} else {
			d.nc = nc
			js, err := jetstream.New(nc)
			if err != nil {
				helper.Warnf("nats jetstream init failed: %v", err)
			} else {
				d.js = js
				bucket := c.Nats.ShadowBucket
				if bucket == "" {
					bucket = "esp_pika_shadow"
				}
				kv, err := js.CreateOrUpdateKeyValue(context.Background(), jetstream.KeyValueConfig{
					Bucket: bucket,
				})
				if err != nil {
					helper.Warnf("nats kv bucket %s init failed: %v", bucket, err)
				} else {
					d.shadowKV = kv
				}
			}
		}
	}

	cleanup := func() {
		helper.Info("closing data resources")
		if d.nc != nil {
			d.nc.Close()
		}
		if d.db != nil {
			_ = d.db.Close()
		}
	}
	return d, cleanup, nil
}

func initSchema(db *sql.DB) error {
	stmts := []string{
		`CREATE TABLE IF NOT EXISTS allowed_macs (
			mac TEXT PRIMARY KEY,
			group_id TEXT NOT NULL,
			zone TEXT NOT NULL DEFAULT '',
			label TEXT NOT NULL DEFAULT '',
			enabled INTEGER NOT NULL DEFAULT 1,
			created_at DATETIME NOT NULL,
			updated_at DATETIME NOT NULL
		);`,
		`CREATE TABLE IF NOT EXISTS devices (
			device_id TEXT PRIMARY KEY,
			hello_json TEXT NOT NULL,
			updated_at DATETIME NOT NULL
		);`,
		`CREATE TABLE IF NOT EXISTS device_shadows (
			device_id TEXT PRIMARY KEY,
			group_id TEXT NOT NULL,
			shadow_json TEXT NOT NULL,
			updated_at DATETIME NOT NULL
		);`,
		`CREATE TABLE IF NOT EXISTS speaker_groups (
			speaker_id TEXT PRIMARY KEY,
			uid TEXT NOT NULL DEFAULT '',
			agent_id TEXT NOT NULL DEFAULT '',
			group_id TEXT NOT NULL,
			display_name TEXT NOT NULL DEFAULT '',
			role INTEGER NOT NULL DEFAULT 2,
			prompt TEXT NOT NULL DEFAULT '',
			tts_voice TEXT NOT NULL DEFAULT '',
			embedding_json TEXT NOT NULL DEFAULT '[]',
			version INTEGER NOT NULL DEFAULT 1,
			updated_at DATETIME NOT NULL
		);`,
		`CREATE TABLE IF NOT EXISTS acl_rules (
			rule_id TEXT PRIMARY KEY,
			agent_id TEXT NOT NULL,
			group_id TEXT NOT NULL DEFAULT '',
			zone_pattern TEXT NOT NULL DEFAULT '',
			device_id TEXT NOT NULL DEFAULT '',
			capability_pattern TEXT NOT NULL DEFAULT '',
			min_speaker_role INTEGER NOT NULL DEFAULT 0,
			allow INTEGER NOT NULL DEFAULT 1
		);`,
	}
	for _, s := range stmts {
		if _, err := db.Exec(s); err != nil {
			return err
		}
	}
	return nil
}
