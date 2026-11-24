package server

import (
	"context"
	"fmt"
	"net"
	"os"
	"slices"
	"strings"
	"sync"
	"time"

	"esp-pika/backend/gateway/internal/conf"

	"github.com/go-kratos/kratos/v2/log"
	"github.com/grandcat/zeroconf"
)

const mdnsWatchInterval = 10 * time.Second

var excludedMDNSInterfacePrefixes = []string{
	"utun",
	"tun",
	"tap",
	"bridge",
	"br-",
	"tailscale",
	"awdl",
	"llw",
	"docker",
	"veth",
	"virbr",
	"vboxnet",
	"vmnet",
	"anpi",
	"ap",
	"lo",
}

// MDNSServer advertises `_esp-gateway._tcp.local` and participates in the Kratos transport lifecycle.
type MDNSServer struct {
	cfg *conf.Discovery
	log *log.Helper

	mu      sync.Mutex
	server  *zeroconf.Server
	lastIPs []string
	stopCh  chan struct{}
	wg      sync.WaitGroup
}

// NewMDNSServer constructs an MDNSServer.
func NewMDNSServer(c *conf.Discovery, logger log.Logger) *MDNSServer {
	return &MDNSServer{
		cfg: c,
		log: log.NewHelper(logger),
	}
}

// Start registers the gateway mDNS service record and monitors active LAN interfaces for changes.
func (m *MDNSServer) Start(ctx context.Context) error {
	if m.cfg == nil || !m.cfg.MdnsEnabled {
		m.log.WithContext(ctx).Info("mDNS advertisement disabled")
		return nil
	}

	serviceName := strings.TrimSpace(m.cfg.ServiceName)
	if serviceName == "" {
		serviceName = "esp-pika-gateway"
	}
	serviceType := strings.TrimSpace(m.cfg.ServiceType)
	if serviceType == "" {
		serviceType = "_esp-gateway._tcp"
	}
	domain := strings.TrimRight(strings.TrimSpace(m.cfg.Domain), ".")
	if domain == "" {
		domain = "local"
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

	ifaces, ips := selectLANInterfaces()
	host := resolveMDNSHost(domain)
	srv, err := registerMDNSService(serviceName, serviceType, domain, port, host, ips, txt, ifaces)
	if err != nil {
		return err
	}

	stopCh := make(chan struct{})
	m.mu.Lock()
	if m.stopCh != nil {
		close(m.stopCh)
	}
	if m.server != nil {
		m.server.Shutdown()
	}
	m.server = srv
	m.lastIPs = slices.Clone(ips)
	m.stopCh = stopCh
	m.wg.Add(1)
	go m.watchInterfaces(stopCh, serviceName, serviceType, domain, port, txt)
	m.mu.Unlock()

	m.log.WithContext(ctx).Infof("mDNS advertising %s.%s.%s. on port %d host=%s.%s. ips=%v",
		serviceName, serviceType, domain, port, host, domain, ips)
	return nil
}

// Stop shuts down the mDNS watcher and advertisement cleanly.
func (m *MDNSServer) Stop(ctx context.Context) error {
	m.mu.Lock()
	stopCh := m.stopCh
	m.stopCh = nil
	if stopCh != nil {
		close(stopCh)
	}
	m.mu.Unlock()

	m.wg.Wait()

	m.mu.Lock()
	srv := m.server
	m.server = nil
	m.lastIPs = nil
	m.mu.Unlock()

	if srv != nil {
		srv.Shutdown()
		m.log.WithContext(ctx).Info("mDNS advertisement stopped")
	}
	return nil
}

func (m *MDNSServer) watchInterfaces(stopCh <-chan struct{}, serviceName, serviceType, domain string, port int, txt []string) {
	defer m.wg.Done()
	ticker := time.NewTicker(mdnsWatchInterval)
	defer ticker.Stop()

	for {
		select {
		case <-stopCh:
			return
		case <-ticker.C:
			ifaces, ips := selectLANInterfaces()
			m.mu.Lock()
			changed := !slices.Equal(m.lastIPs, ips)
			oldSrv := m.server
			m.mu.Unlock()
			if !changed {
				continue
			}

			host := resolveMDNSHost(domain)
			srv, err := registerMDNSService(serviceName, serviceType, domain, port, host, ips, txt, ifaces)
			if err != nil {
				m.log.Warnf("mDNS refresh failed for ips=%v: %v", ips, err)
				continue
			}

			m.mu.Lock()
			if m.stopCh == nil {
				m.mu.Unlock()
				srv.Shutdown()
				return
			}
			oldSrv = m.server
			m.server = srv
			m.lastIPs = slices.Clone(ips)
			m.mu.Unlock()

			if oldSrv != nil {
				oldSrv.Shutdown()
			}
			m.log.Infof("mDNS advertisement updated host=%s.%s. ips=%v", host, domain, ips)
		}
	}
}

func resolveMDNSHost(domain string) string {
	hostname, err := os.Hostname()
	if err != nil {
		return "esp-pika-gateway"
	}
	return sanitizeMDNSHost(hostname, domain)
}

func sanitizeMDNSHost(hostname, domain string) string {
	cleanDomain := strings.ToLower(strings.TrimRight(strings.TrimSpace(domain), "."))
	host := strings.TrimRight(strings.TrimSpace(hostname), ".")
	if cleanDomain != "" {
		suffix := "." + cleanDomain
		for {
			lower := strings.ToLower(host)
			if strings.HasSuffix(lower, suffix) {
				host = strings.TrimRight(host[:len(host)-len(suffix)], ".")
				continue
			}
			if lower == cleanDomain {
				host = ""
			}
			break
		}
	}
	if host == "" {
		return "esp-pika-gateway"
	}
	return host
}

func isExcludedMDNSInterface(name string) bool {
	lower := strings.ToLower(strings.TrimSpace(name))
	if lower == "" {
		return true
	}
	for _, prefix := range excludedMDNSInterfacePrefixes {
		if strings.HasPrefix(lower, prefix) {
			return true
		}
	}
	return false
}

func isAdvertisableLANIPv4(ip net.IP) bool {
	if ip == nil {
		return false
	}
	v4 := ip.To4()
	if v4 == nil {
		return false
	}
	if v4.IsLoopback() || v4.IsLinkLocalUnicast() || v4.IsLinkLocalMulticast() || v4.IsMulticast() || v4.IsUnspecified() || !v4.IsGlobalUnicast() {
		return false
	}
	// Reject RFC6598 CGNAT / Tailscale (100.64.0.0/10)
	if v4[0] == 100 && v4[1] >= 64 && v4[1] <= 127 {
		return false
	}
	// Reject RFC2544 benchmark / Clash TUN fake-IP (198.18.0.0/15)
	if v4[0] == 198 && (v4[1] == 18 || v4[1] == 19) {
		return false
	}
	return true
}

func selectLANInterfaces() ([]net.Interface, []string) {
	rawIfaces, err := net.Interfaces()
	if err != nil {
		return nil, nil
	}

	var ifaces []net.Interface
	ipSet := make(map[string]struct{})
	var ips []string

	for _, iface := range rawIfaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagMulticast == 0 {
			continue
		}
		if iface.Flags&net.FlagLoopback != 0 || iface.Flags&net.FlagPointToPoint != 0 {
			continue
		}
		if isExcludedMDNSInterface(iface.Name) {
			continue
		}
		addrs, err := iface.Addrs()
		if err != nil {
			continue
		}

		matched := false
		for _, addr := range addrs {
			ipNet, ok := addr.(*net.IPNet)
			if !ok {
				continue
			}
			v4 := ipNet.IP.To4()
			if !isAdvertisableLANIPv4(v4) {
				continue
			}
			ipStr := v4.String()
			if _, exists := ipSet[ipStr]; !exists {
				ipSet[ipStr] = struct{}{}
				ips = append(ips, ipStr)
			}
			matched = true
		}
		if matched {
			ifaces = append(ifaces, iface)
		}
	}

	slices.Sort(ips)
	return ifaces, ips
}

func registerMDNSService(serviceName, serviceType, domain string, port int, host string, ips, txt []string, ifaces []net.Interface) (*zeroconf.Server, error) {
	if len(ips) > 0 {
		srv, err := zeroconf.RegisterProxy(serviceName, serviceType, domain, port, host, ips, txt, ifaces)
		if err != nil {
			return nil, fmt.Errorf("register mdns proxy %s.%s: %w", serviceName, serviceType, err)
		}
		return srv, nil
	}
	srv, err := zeroconf.Register(serviceName, serviceType, domain, port, txt, nil)
	if err != nil {
		return nil, fmt.Errorf("register mdns %s.%s: %w", serviceName, serviceType, err)
	}
	return srv, nil
}
