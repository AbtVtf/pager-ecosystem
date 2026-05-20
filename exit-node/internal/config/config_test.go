package config

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const goodYAML = `
node_name: edge-1
lora:
  port: /dev/ttyUSB0
  baud_rate: 115200
  region: US915
rate_limit:
  per_device_per_min: 30
stats:
  enabled: true
  url: https://stats.example/exit
`

const goodYAMLWithAdvertise = `
node_name: edge-1
lora:
  port: /dev/ttyUSB0
  baud_rate: 115200
  region: US915
advertise:
  enabled: true
  interval_sec: 45
  bw_class: high
  identity_pubkey_hex: 0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20
`

func writeTemp(t *testing.T, body string) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), "exit-node.yaml")
	if err := os.WriteFile(p, []byte(body), 0o600); err != nil {
		t.Fatal(err)
	}
	return p
}

func TestLoadGood(t *testing.T) {
	p := writeTemp(t, goodYAML)
	cfg, err := Load(p)
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	if cfg.NodeName != "edge-1" {
		t.Errorf("node_name: got %q", cfg.NodeName)
	}
	if cfg.LoRa.Port != "/dev/ttyUSB0" {
		t.Errorf("port: got %q", cfg.LoRa.Port)
	}
	if cfg.LoRa.BaudRate != 115200 {
		t.Errorf("baud_rate: got %d", cfg.LoRa.BaudRate)
	}
	if cfg.RateLimit.PerDevicePerMin != 30 {
		t.Errorf("rate limit: got %d", cfg.RateLimit.PerDevicePerMin)
	}
	if !cfg.Stats.Enabled {
		t.Errorf("stats.enabled should be true")
	}
	if cfg.Advertise.Enabled {
		t.Errorf("advertise.enabled should default to false when omitted")
	}
}

func TestLoadDefaultsFillUnset(t *testing.T) {
	p := writeTemp(t, "node_name: minimal\n")
	cfg, err := Load(p)
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	if cfg.LoRa.Port != "/dev/ttyUSB0" {
		t.Errorf("default port not applied: %q", cfg.LoRa.Port)
	}
	if cfg.LoRa.BaudRate != 115200 {
		t.Errorf("default baud not applied: %d", cfg.LoRa.BaudRate)
	}
	if cfg.LoRa.Region != "US915" {
		t.Errorf("default region not applied: %q", cfg.LoRa.Region)
	}
	if cfg.RateLimit.PerDevicePerMin != 60 {
		t.Errorf("default rate limit not applied: %d", cfg.RateLimit.PerDevicePerMin)
	}
	if cfg.Advertise.IntervalSec != 30 {
		t.Errorf("advertise.interval_sec default: got %d want 30", cfg.Advertise.IntervalSec)
	}
	if cfg.Advertise.BWClass != "unknown" {
		t.Errorf("advertise.bw_class default: got %q want unknown", cfg.Advertise.BWClass)
	}
}

func TestLoadRejectsBadRegion(t *testing.T) {
	p := writeTemp(t, "lora:\n  region: ZZ999\n")
	if _, err := Load(p); err == nil {
		t.Fatal("expected region validation error")
	}
}

func TestLoadRejectsMissingFile(t *testing.T) {
	if _, err := Load("/no/such/path.yaml"); err == nil {
		t.Fatal("expected read error")
	}
}

func TestLoadRejectsEmptyPath(t *testing.T) {
	if _, err := Load(""); err == nil {
		t.Fatal("expected empty-path error")
	}
}

func TestLoadRejectsBadYAML(t *testing.T) {
	p := writeTemp(t, "lora: { port: ]")
	if _, err := Load(p); err == nil {
		t.Fatal("expected parse error")
	}
}

func TestLoadAdvertiseGood(t *testing.T) {
	p := writeTemp(t, goodYAMLWithAdvertise)
	cfg, err := Load(p)
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	if !cfg.Advertise.Enabled {
		t.Errorf("advertise.enabled: want true")
	}
	if cfg.Advertise.IntervalSec != 45 {
		t.Errorf("interval_sec: got %d want 45", cfg.Advertise.IntervalSec)
	}
	if cfg.Advertise.BWClassValue() != 3 {
		t.Errorf("bw_class value: got %d want 3 (high)", cfg.Advertise.BWClassValue())
	}
	pk, err := cfg.Advertise.DecodePubkey()
	if err != nil {
		t.Fatalf("DecodePubkey: %v", err)
	}
	if pk[0] != 0x01 || pk[31] != 0x20 {
		t.Errorf("pubkey not decoded correctly: %x", pk)
	}
	if cfg.Advertise.Interval().Seconds() != 45 {
		t.Errorf("Interval(): got %s want 45s", cfg.Advertise.Interval())
	}
}

func TestLoadAdvertiseRejectsIntervalOutOfRange(t *testing.T) {
	for _, sec := range []int{0, 9, 301, 3600} {
		yaml := "advertise:\n  enabled: true\n  interval_sec: " +
			itoa(sec) +
			"\n  bw_class: high\n  identity_pubkey_hex: 0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20\n"
		p := writeTemp(t, yaml)
		_, err := Load(p)
		if err == nil {
			t.Errorf("interval %d: expected validation error", sec)
			continue
		}
		if !strings.Contains(err.Error(), "interval_sec") {
			t.Errorf("interval %d: error not about interval_sec: %v", sec, err)
		}
	}
}

func TestLoadAdvertiseRejectsBadBWClass(t *testing.T) {
	p := writeTemp(t, "advertise:\n  enabled: true\n  interval_sec: 30\n  bw_class: blazing\n  identity_pubkey_hex: 0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20\n")
	_, err := Load(p)
	if err == nil {
		t.Fatal("expected bw_class validation error")
	}
	if !strings.Contains(err.Error(), "bw_class") {
		t.Errorf("error not about bw_class: %v", err)
	}
}

func TestLoadAdvertiseRejectsBadPubkey(t *testing.T) {
	cases := []struct {
		name string
		body string
	}{
		{"empty", "advertise:\n  enabled: true\n  interval_sec: 30\n  bw_class: high\n"},
		{"not hex", "advertise:\n  enabled: true\n  interval_sec: 30\n  bw_class: high\n  identity_pubkey_hex: not-hex-at-all\n"},
		{"short", "advertise:\n  enabled: true\n  interval_sec: 30\n  bw_class: high\n  identity_pubkey_hex: 0102\n"},
		{"all zero", "advertise:\n  enabled: true\n  interval_sec: 30\n  bw_class: high\n  identity_pubkey_hex: 0000000000000000000000000000000000000000000000000000000000000000\n"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			p := writeTemp(t, tc.body)
			if _, err := Load(p); err == nil {
				t.Errorf("expected pubkey validation error")
			}
		})
	}
}

func TestLoadAdvertiseDisabledSkipsValidation(t *testing.T) {
	// With advertise disabled, intentionally-broken sub-fields are tolerated.
	p := writeTemp(t, "advertise:\n  enabled: false\n  interval_sec: 0\n  bw_class: garbage\n  identity_pubkey_hex: nope\n")
	if _, err := Load(p); err != nil {
		t.Fatalf("disabled advertise should not fail validation: %v", err)
	}
}

func TestParseBWClass(t *testing.T) {
	cases := []struct {
		in   string
		want uint8
	}{
		{"unknown", 0}, {"UNKNOWN", 0}, {"", 0},
		{"low", 1}, {"Low", 1},
		{"medium", 2},
		{"high", 3},
		{"gigabit", 4}, {"fiber", 4},
	}
	for _, c := range cases {
		v, err := ParseBWClass(c.in)
		if err != nil {
			t.Errorf("ParseBWClass(%q): unexpected err %v", c.in, err)
			continue
		}
		if v != c.want {
			t.Errorf("ParseBWClass(%q): got %d want %d", c.in, v, c.want)
		}
	}
	if _, err := ParseBWClass("plaid"); err == nil {
		t.Error("ParseBWClass(plaid): want error")
	}
}

// tiny local int->string to avoid importing strconv just for tests
func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	neg := false
	if n < 0 {
		neg = true
		n = -n
	}
	var b [20]byte
	i := len(b)
	for n > 0 {
		i--
		b[i] = byte('0' + n%10)
		n /= 10
	}
	if neg {
		i--
		b[i] = '-'
	}
	return string(b[i:])
}
