package link

import (
	"fmt"
	"net"
	"strings"
)

// DefaultLoopback identifies the dev server's default bind host. When the
// user runs `pagerctl dev` with no `--host` flag, the server listens on
// 127.0.0.1, which a real device on a different IP cannot reach. The link
// command rewrites that to a routable LAN address before sending it to
// the device.
const DefaultLoopback = "127.0.0.1"

// LANResolver returns a host-LAN IPv4 reachable from devices on the same
// subnet. Abstracted so tests can inject a deterministic address.
type LANResolver interface {
	LAN() (string, error)
}

// SystemLAN reads interfaces from the host kernel and returns the first
// non-loopback, non-link-local IPv4 address it finds. Wi-Fi and Ethernet
// addresses are equally acceptable — the dev server is typically reachable
// from a USB-tethered device on either path.
type SystemLAN struct{}

func (SystemLAN) LAN() (string, error) {
	ifaces, err := net.Interfaces()
	if err != nil {
		return "", fmt.Errorf("list interfaces: %w", err)
	}
	for _, ifc := range ifaces {
		if ifc.Flags&net.FlagUp == 0 || ifc.Flags&net.FlagLoopback != 0 {
			continue
		}
		addrs, err := ifc.Addrs()
		if err != nil {
			continue
		}
		for _, a := range addrs {
			ip := addrToIP(a)
			if ip == nil {
				continue
			}
			v4 := ip.To4()
			if v4 == nil || v4.IsLoopback() || v4.IsLinkLocalUnicast() || v4.IsUnspecified() {
				continue
			}
			return v4.String(), nil
		}
	}
	return "", fmt.Errorf("no routable IPv4 address found on any interface")
}

func addrToIP(a net.Addr) net.IP {
	switch v := a.(type) {
	case *net.IPNet:
		return v.IP
	case *net.IPAddr:
		return v.IP
	}
	return nil
}

// ResolveDevServerURL canonicalises the URL the device will fetch Frames
// from. If host is empty or a loopback address (127.0.0.1, localhost,
// ::1), it is replaced with a routable LAN IP from resolver; otherwise
// the input is returned unchanged after light normalisation.
//
// Returns the canonical URL string, e.g. "http://192.168.1.42:8000/".
func ResolveDevServerURL(host string, port int, resolver LANResolver) (string, error) {
	if port < 1 || port > 65535 {
		return "", fmt.Errorf("invalid port %d (must be 1-65535)", port)
	}
	h := strings.TrimSpace(host)
	if h == "" || isLoopbackHost(h) {
		lan, err := resolver.LAN()
		if err != nil {
			return "", fmt.Errorf("dev server is on %q which a real device cannot reach, and %w", host, err)
		}
		h = lan
	}
	return fmt.Sprintf("http://%s/", net.JoinHostPort(h, fmt.Sprintf("%d", port))), nil
}

func isLoopbackHost(h string) bool {
	switch strings.ToLower(h) {
	case "localhost", "127.0.0.1", "::1":
		return true
	}
	if ip := net.ParseIP(h); ip != nil {
		return ip.IsLoopback()
	}
	return false
}
