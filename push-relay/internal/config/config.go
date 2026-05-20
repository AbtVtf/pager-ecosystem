// Package config loads push-relay configuration from environment variables.
package config

import (
	"fmt"
	"os"
	"strings"
)

type Config struct {
	Addr           string
	TLSCertFile    string
	TLSKeyFile     string
	RedisURL       string
	BuildTag       string
	MarketplaceURL string
}

func FromEnv() (Config, error) {
	cfg := Config{
		Addr:           envOr("PUSH_RELAY_ADDR", ":8443"),
		TLSCertFile:    os.Getenv("PUSH_RELAY_TLS_CERT"),
		TLSKeyFile:     os.Getenv("PUSH_RELAY_TLS_KEY"),
		RedisURL:       envOr("PUSH_RELAY_REDIS_URL", "redis://localhost:6379/0"),
		BuildTag:       envOr("PUSH_RELAY_BUILD_TAG", "dev"),
		MarketplaceURL: os.Getenv("PUSH_RELAY_MARKETPLACE_URL"),
	}

	if (cfg.TLSCertFile == "") != (cfg.TLSKeyFile == "") {
		return Config{}, fmt.Errorf("PUSH_RELAY_TLS_CERT and PUSH_RELAY_TLS_KEY must be set together")
	}
	if !strings.HasPrefix(cfg.RedisURL, "redis://") && !strings.HasPrefix(cfg.RedisURL, "rediss://") {
		return Config{}, fmt.Errorf("PUSH_RELAY_REDIS_URL must start with redis:// or rediss://")
	}
	if cfg.MarketplaceURL != "" &&
		!strings.HasPrefix(cfg.MarketplaceURL, "http://") &&
		!strings.HasPrefix(cfg.MarketplaceURL, "https://") {
		return Config{}, fmt.Errorf("PUSH_RELAY_MARKETPLACE_URL must start with http:// or https://")
	}
	return cfg, nil
}

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}
