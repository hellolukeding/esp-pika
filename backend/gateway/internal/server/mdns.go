package server

import (
	"context"
	"fmt"
	"sync"

	"esp-pika/backend/gateway/internal/conf"

	"github.com/go-kratos/kratos/v2/log"
	"github.com/grandcat/zeroconf"
)

// MDNSServer advertises `_esp-gateway._tcp.local` and participates in the Kratos transport lifecycle.
type MDNSServer struct {
	cfg *conf.Discovery
	log *log.Helper

	mu     sync.Mutex
	server *zeroconf.Server
}

// NewMDNSServer constructs an MDNSServer.
func NewMDNSServer(c *conf.Discovery, logger log.Logger) *MDNSServer {
	return &MDNSServer{
		cfg: c,
		log: log.NewHelper(logger),
	}
}

// Start registers the gateway mDNS service record.
func (m *MDNSServer) Start(ctx context.Context) error {
	if m.cfg == nil || !m.cfg.MdnsEnabled {
		m.log.WithContext(ctx).Info("mDNS advertisement disabled")
		return nil
	}

	serviceName := m.cfg.ServiceName
	if serviceName == "" {
		serviceName = "esp-pika-gateway"
	}
	serviceType := m.cfg.ServiceType
	if serviceType == "" {
		serviceType = "_esp-gateway._tcp"
	}
	domain := m.cfg.Domain
	if domain == "" {
		domain = "local."
	}
	port := int(m.cfg.Port)
	if port <= 0 {
		port = 8080
	}
	txt := []string{
		"proto=ws",
		fmt.Sprintf("group_id=%s", m.cfg.GroupId),
		fmt.Sprintf("tls=%t", m.cfg.Tls),
		"ws_path=/ws/device",
		"media_path=/api/v1/media/upload",
	}

	srv, err := zeroconf.Register(serviceName, serviceType, domain, port, txt, nil)
	if err != nil {
		return fmt.Errorf("register mdns %s.%s: %w", serviceName, serviceType, err)
	}

	m.mu.Lock()
	m.server = srv
	m.mu.Unlock()
	m.log.WithContext(ctx).Infof("mDNS advertising %s.%s%s on port %d", serviceName, serviceType, domain, port)
	return nil
}

// Stop shuts down the mDNS advertisement cleanly.
func (m *MDNSServer) Stop(ctx context.Context) error {
	m.mu.Lock()
	srv := m.server
	m.server = nil
	m.mu.Unlock()
	if srv != nil {
		srv.Shutdown()
		m.log.WithContext(ctx).Info("mDNS advertisement stopped")
	}
	return nil
}
