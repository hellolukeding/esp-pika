package server

import (
	"context"
	"net"
	"testing"

	"esp-pika/backend/gateway/internal/conf"

	"github.com/go-kratos/kratos/v2/log"
)

func TestSanitizeMDNSHost(t *testing.T) {
	t.Parallel()

	cases := []struct {
		hostname string
		domain   string
		want     string
	}{
		{hostname: "lukedingdeMacBook-Pro.local", domain: "local", want: "lukedingdeMacBook-Pro"},
		{hostname: "myhost.local.", domain: "local.", want: "myhost"},
		{hostname: "myhost.LOCAL.", domain: "local", want: "myhost"},
		{hostname: "myhost.local.local.", domain: "local.", want: "myhost"},
		{hostname: "local.", domain: "local", want: "esp-pika-gateway"},
		{hostname: "", domain: "local", want: "esp-pika-gateway"},
		{hostname: "studio-mac", domain: "local", want: "studio-mac"},
	}

	for _, tc := range cases {
		if got := sanitizeMDNSHost(tc.hostname, tc.domain); got != tc.want {
			t.Errorf("sanitizeMDNSHost(%q, %q) = %q, want %q", tc.hostname, tc.domain, got, tc.want)
		}
	}
}

func TestIsExcludedMDNSInterface(t *testing.T) {
	t.Parallel()

	excluded := []string{"utun1500", "utun0", "bridge100", "br-5f56855270a6", "tailscale0", "awdl0", "llw0", "lo0", "tun0", "tap1", "docker0", "veth123", "anpi0", "ap1", ""}
	for _, name := range excluded {
		if !isExcludedMDNSInterface(name) {
			t.Errorf("isExcludedMDNSInterface(%q) = false, want true", name)
		}
	}

	allowed := []string{"en0", "en1", "eth0", "wlan0"}
	for _, name := range allowed {
		if isExcludedMDNSInterface(name) {
			t.Errorf("isExcludedMDNSInterface(%q) = true, want false", name)
		}
	}
}

func TestIsAdvertisableLANIPv4(t *testing.T) {
	t.Parallel()

	cases := []struct {
		ip   string
		want bool
	}{
		{ip: "192.168.101.10", want: true},
		{ip: "10.0.0.5", want: true},
		{ip: "172.16.8.2", want: true},
		{ip: "127.0.0.1", want: false},
		{ip: "169.254.1.1", want: false},
		{ip: "100.99.179.49", want: false},
		{ip: "100.100.142.175", want: false},
		{ip: "198.18.0.1", want: false},
		{ip: "198.19.255.1", want: false},
		{ip: "224.0.0.251", want: false},
		{ip: "0.0.0.0", want: false},
		{ip: "::1", want: false},
		{ip: "fe80::1", want: false},
	}

	for _, tc := range cases {
		parsed := net.ParseIP(tc.ip)
		if got := isAdvertisableLANIPv4(parsed); got != tc.want {
			t.Errorf("isAdvertisableLANIPv4(%q) = %v, want %v", tc.ip, got, tc.want)
		}
	}
}

func TestMDNSServerDisabledLifecycleIdempotent(t *testing.T) {
	t.Parallel()

	srv := NewMDNSServer(&conf.Discovery{MdnsEnabled: false}, log.DefaultLogger)
	ctx := context.Background()
	if err := srv.Start(ctx); err != nil {
		t.Fatalf("Start() error = %v", err)
	}
	if err := srv.Stop(ctx); err != nil {
		t.Fatalf("first Stop() error = %v", err)
	}
	if err := srv.Stop(ctx); err != nil {
		t.Fatalf("second Stop() error = %v", err)
	}
}
