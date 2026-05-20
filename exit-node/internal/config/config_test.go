package config

import (
	"os"
	"path/filepath"
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
