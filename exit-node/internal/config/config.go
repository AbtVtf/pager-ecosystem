// Package config loads the exit-node YAML configuration.
package config

import (
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"gopkg.in/yaml.v3"
)

type LoRa struct {
	Port     string `yaml:"port"`
	BaudRate int    `yaml:"baud_rate"`
	Region   string `yaml:"region"`
}

type RateLimit struct {
	PerDevicePerMin int `yaml:"per_device_per_min"`
}

type Stats struct {
	Enabled bool   `yaml:"enabled"`
	URL     string `yaml:"url"`
}

// Advertise configures the EXIT-005 discovery beacon. The exit-node
// broadcasts an `exit-node-advertise` packet every IntervalSec seconds
// when Enabled is true (see protocol/exit-node-advertise.md).
type Advertise struct {
	Enabled           bool   `yaml:"enabled"`
	IntervalSec       int    `yaml:"interval_sec"`
	BWClass           string `yaml:"bw_class"`
	IdentityPubkeyHex string `yaml:"identity_pubkey_hex"`
}

type Config struct {
	NodeName  string    `yaml:"node_name"`
	LoRa      LoRa      `yaml:"lora"`
	RateLimit RateLimit `yaml:"rate_limit"`
	Stats     Stats     `yaml:"stats"`
	Advertise Advertise `yaml:"advertise"`
}

func defaults() Config {
	return Config{
		NodeName: "exit-node",
		LoRa: LoRa{
			Port:     "/dev/ttyUSB0",
			BaudRate: 115200,
			Region:   "US915",
		},
		RateLimit: RateLimit{PerDevicePerMin: 60},
		Stats:     Stats{Enabled: false},
		Advertise: Advertise{
			Enabled:     false,
			IntervalSec: 30,
			BWClass:     "unknown",
		},
	}
}

func Load(path string) (Config, error) {
	cfg := defaults()
	if path == "" {
		return cfg, errors.New("config: empty path")
	}
	abs, err := filepath.Abs(path)
	if err != nil {
		return cfg, fmt.Errorf("config: resolve path: %w", err)
	}
	data, err := os.ReadFile(abs)
	if err != nil {
		return cfg, fmt.Errorf("config: read %s: %w", abs, err)
	}
	if err := yaml.Unmarshal(data, &cfg); err != nil {
		return cfg, fmt.Errorf("config: parse %s: %w", abs, err)
	}
	if err := cfg.Validate(); err != nil {
		return cfg, fmt.Errorf("config: %s: %w", abs, err)
	}
	return cfg, nil
}

func (c Config) Validate() error {
	if c.LoRa.Port == "" {
		return errors.New("lora.port is required")
	}
	if c.LoRa.BaudRate <= 0 {
		return errors.New("lora.baud_rate must be > 0")
	}
	switch c.LoRa.Region {
	case "US915", "EU868", "AS923", "AU915":
	default:
		return fmt.Errorf("lora.region %q not one of US915/EU868/AS923/AU915", c.LoRa.Region)
	}
	if c.RateLimit.PerDevicePerMin < 0 {
		return errors.New("rate_limit.per_device_per_min must be >= 0")
	}
	return c.Advertise.validate()
}

func (a Advertise) validate() error {
	if !a.Enabled {
		return nil
	}
	if a.IntervalSec < 10 || a.IntervalSec > 300 {
		return fmt.Errorf("advertise.interval_sec %d out of allowed range [10,300]", a.IntervalSec)
	}
	if _, err := ParseBWClass(a.BWClass); err != nil {
		return fmt.Errorf("advertise.bw_class: %w", err)
	}
	pk, err := a.DecodePubkey()
	if err != nil {
		return fmt.Errorf("advertise.identity_pubkey_hex: %w", err)
	}
	if pk == ([32]byte{}) {
		return errors.New("advertise.identity_pubkey_hex must not be all-zero")
	}
	return nil
}

// Interval returns the configured beacon interval as a time.Duration.
// Always positive when Validate() passed.
func (a Advertise) Interval() time.Duration {
	return time.Duration(a.IntervalSec) * time.Second
}

// DecodePubkey parses the configured identity pubkey hex into the
// 32-byte Ed25519 key the advert payload expects.
func (a Advertise) DecodePubkey() ([32]byte, error) {
	var out [32]byte
	s := strings.TrimSpace(a.IdentityPubkeyHex)
	if s == "" {
		return out, errors.New("required when advertise.enabled=true")
	}
	raw, err := hex.DecodeString(s)
	if err != nil {
		return out, fmt.Errorf("not valid hex: %w", err)
	}
	if len(raw) != 32 {
		return out, fmt.Errorf("got %d bytes, want 32", len(raw))
	}
	copy(out[:], raw)
	return out, nil
}

// BWClassValue is the numeric on-wire bw_class byte for the configured
// string. Returns 0 (BWUnknown) when the config string is unknown to
// older readers; Validate rejects unknown strings upstream so callers
// don't need to recheck.
//
// The mapping mirrors protocol/exit-node-advertise.md §3.
func (a Advertise) BWClassValue() uint8 {
	v, _ := ParseBWClass(a.BWClass)
	return v
}

// ParseBWClass maps a config bw_class string to the on-wire byte value.
// Accepts case-insensitive: unknown, low, medium, high, gigabit.
func ParseBWClass(s string) (uint8, error) {
	switch strings.ToLower(strings.TrimSpace(s)) {
	case "", "unknown":
		return 0, nil
	case "low":
		return 1, nil
	case "medium":
		return 2, nil
	case "high":
		return 3, nil
	case "gigabit", "fiber":
		return 4, nil
	default:
		return 0, fmt.Errorf("unknown bw_class %q (want unknown|low|medium|high|gigabit)", s)
	}
}
