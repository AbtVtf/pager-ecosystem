package flash

import (
	"fmt"
	"strings"

	"go.bug.st/serial/enumerator"
)

// USB identifiers known to indicate an ESP32-class device.
//
// VID is normalized to lower-case hex without the 0x prefix; PID is "*" when
// any PID under that VID is acceptable. Lists are documented in
// https://docs.espressif.com/projects/esptool/en/latest/esp32s3/troubleshooting/connecting-devices.html
// and the data sheets for the relevant USB-UART bridges.
var espIDs = []struct {
	VID, PID string
	Name     string
}{
	// Espressif native USB-Serial-JTAG on ESP32-S3 (and S2/C3/C6/H2).
	{"303a", "*", "Espressif USB-Serial-JTAG"},
	// Silicon Labs CP210x.
	{"10c4", "ea60", "Silicon Labs CP210x"},
	// WCH CH340/CH341.
	{"1a86", "7523", "WCH CH340"},
	{"1a86", "55d4", "WCH CH343"},
	// FTDI FT232.
	{"0403", "6001", "FTDI FT232"},
	{"0403", "6014", "FTDI FT232H"},
	{"0403", "6015", "FTDI FT-X"},
}

// Port describes a serial port candidate.
type Port struct {
	Name    string // OS-level path or identifier (e.g. /dev/ttyACM0, COM3).
	VID     string // 4-char lower-case hex, no prefix.
	PID     string // 4-char lower-case hex, no prefix.
	Product string
	Serial  string
	IsUSB   bool
	Match   string // Human-readable match descriptor for ESP32 USB IDs.
}

// Lister enumerates serial ports on the host.
type Lister interface {
	List() ([]Port, error)
}

// SystemLister enumerates serial ports via go.bug.st/serial.
type SystemLister struct{}

func (SystemLister) List() ([]Port, error) {
	raw, err := enumerator.GetDetailedPortsList()
	if err != nil {
		return nil, fmt.Errorf("list serial ports: %w", err)
	}
	out := make([]Port, 0, len(raw))
	for _, p := range raw {
		out = append(out, Port{
			Name:    p.Name,
			VID:     strings.ToLower(p.VID),
			PID:     strings.ToLower(p.PID),
			Product: p.Product,
			Serial:  p.SerialNumber,
			IsUSB:   p.IsUSB,
		})
	}
	return out, nil
}

// FilterESP returns ports whose USB VID/PID match a known ESP32 entry. Each
// returned port's Match field is set to the matched device family name.
func FilterESP(ports []Port) []Port {
	matched := make([]Port, 0, len(ports))
	for _, p := range ports {
		name, ok := matchESP(p)
		if !ok {
			continue
		}
		p.Match = name
		matched = append(matched, p)
	}
	return matched
}

func matchESP(p Port) (string, bool) {
	if !p.IsUSB {
		return "", false
	}
	vid := strings.ToLower(p.VID)
	pid := strings.ToLower(p.PID)
	for _, id := range espIDs {
		if vid != id.VID {
			continue
		}
		if id.PID == "*" || pid == id.PID {
			return id.Name, true
		}
	}
	return "", false
}

// SelectPort picks a single port for flashing. If override is non-empty it is
// returned as-is (no enumeration). Otherwise the lister is queried and the
// result is filtered to ESP32 USB IDs.
//
// Errors:
//   - if no override and zero matching ports are found
//   - if no override and multiple matching ports are found (require user to pick)
func SelectPort(l Lister, override string) (Port, error) {
	if override != "" {
		return Port{Name: override, Match: "user-supplied"}, nil
	}
	all, err := l.List()
	if err != nil {
		return Port{}, err
	}
	candidates := FilterESP(all)
	switch len(candidates) {
	case 0:
		return Port{}, fmt.Errorf("no ESP32 device detected (checked %d serial port(s)); pass --port to specify one", len(all))
	case 1:
		return candidates[0], nil
	default:
		names := make([]string, 0, len(candidates))
		for _, c := range candidates {
			names = append(names, c.Name)
		}
		return Port{}, fmt.Errorf("multiple ESP32 devices detected (%s); pass --port to disambiguate", strings.Join(names, ", "))
	}
}
