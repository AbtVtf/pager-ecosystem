package link

import (
	"errors"
	"strings"
	"testing"
)

type fakeLAN struct {
	ip  string
	err error
}

func (f fakeLAN) LAN() (string, error) { return f.ip, f.err }

func TestResolveDevServerURLRewritesLoopback(t *testing.T) {
	cases := []struct {
		name string
		host string
	}{
		{"127.0.0.1", "127.0.0.1"},
		{"localhost", "localhost"},
		{"LOCALHOST mixed case", "LocalHost"},
		{"empty", ""},
		{"ipv6 loopback", "::1"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			url, err := ResolveDevServerURL(c.host, 8000, fakeLAN{ip: "192.168.1.42"})
			if err != nil {
				t.Fatalf("resolve: %v", err)
			}
			want := "http://192.168.1.42:8000/"
			if url != want {
				t.Errorf("got %q, want %q", url, want)
			}
		})
	}
}

func TestResolveDevServerURLPassesThroughRoutableHost(t *testing.T) {
	url, err := ResolveDevServerURL("10.0.0.5", 9001, fakeLAN{err: errors.New("should not be called")})
	if err != nil {
		t.Fatalf("resolve: %v", err)
	}
	if url != "http://10.0.0.5:9001/" {
		t.Errorf("got %q", url)
	}
}

func TestResolveDevServerURLPropagatesLANError(t *testing.T) {
	_, err := ResolveDevServerURL("127.0.0.1", 8000, fakeLAN{err: errors.New("no nic")})
	if err == nil || !strings.Contains(err.Error(), "no nic") {
		t.Fatalf("expected lan error to surface, got %v", err)
	}
}

func TestResolveDevServerURLRejectsBadPort(t *testing.T) {
	for _, p := range []int{0, -1, 65536, 1 << 20} {
		_, err := ResolveDevServerURL("10.0.0.5", p, fakeLAN{ip: "ignored"})
		if err == nil {
			t.Errorf("port %d: expected error", p)
		}
	}
}

func TestSystemLANReturnsRoutableV4WhenAvailable(t *testing.T) {
	ip, err := SystemLAN{}.LAN()
	if err != nil {
		// Hosts without any non-loopback IPv4 (e.g. some CI sandboxes)
		// should produce the documented error, not a panic.
		if !strings.Contains(err.Error(), "no routable IPv4") {
			t.Fatalf("unexpected error: %v", err)
		}
		t.Skipf("no routable IPv4 on this host: %v", err)
	}
	if strings.HasPrefix(ip, "127.") || strings.HasPrefix(ip, "169.254.") {
		t.Errorf("returned non-routable address %q", ip)
	}
}
