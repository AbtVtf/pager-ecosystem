// Package config loads the exit-node YAML configuration.
package config

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"

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

type Config struct {
	NodeName  string    `yaml:"node_name"`
	LoRa      LoRa      `yaml:"lora"`
	RateLimit RateLimit `yaml:"rate_limit"`
	Stats     Stats     `yaml:"stats"`
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
	return nil
}
