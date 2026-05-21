// Command statusd serves the public push-relay status page at
// status.pageros.org (PUSH-010).
//
// statusd queries the relay's Prometheus instance (PUSH-009) and renders an
// HTML page plus a JSON snapshot. It deliberately runs in its own process
// so that a push-relay crash does not take the status page down with it —
// the entire point of a status page is that it works when the service it
// reports on does not.
package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"syscall"
	"time"

	"github.com/pageros/pageros/push-relay/internal/status"
)

func main() {
	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}))
	slog.SetDefault(logger)

	cfg, err := loadConfig()
	if err != nil {
		logger.Error("invalid configuration", "err", err)
		os.Exit(2)
	}

	prom := status.NewPromClient(cfg.PrometheusURL, nil)
	builder := status.NewBuilder(prom, cfg.AvailabilityTarget)
	srv := status.NewServer(builder, cfg.CacheTTL, logger)

	httpServer := &http.Server{
		Addr:              cfg.Addr,
		Handler:           srv.Handler(),
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       10 * time.Second,
		WriteTimeout:      15 * time.Second,
		IdleTimeout:       60 * time.Second,
	}

	rootCtx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	errs := make(chan error, 1)
	go func() {
		logger.Info("statusd starting",
			"addr", cfg.Addr,
			"prometheus", cfg.PrometheusURL,
			"availability_target", cfg.AvailabilityTarget,
			"cache_ttl", cfg.CacheTTL.String(),
		)
		errs <- httpServer.ListenAndServe()
	}()

	select {
	case err := <-errs:
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			logger.Error("statusd exited", "err", err)
			os.Exit(1)
		}
	case <-rootCtx.Done():
		logger.Info("shutdown signal received")
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		if err := httpServer.Shutdown(shutdownCtx); err != nil {
			logger.Error("graceful shutdown failed", "err", err)
			os.Exit(1)
		}
		logger.Info("statusd stopped cleanly")
	}
}

type config struct {
	Addr               string
	PrometheusURL      string
	AvailabilityTarget float64
	CacheTTL           time.Duration
}

func loadConfig() (config, error) {
	cfg := config{
		Addr:               envOr("STATUSD_ADDR", ":8080"),
		PrometheusURL:      envOr("STATUSD_PROMETHEUS_URL", ""),
		AvailabilityTarget: 0.995, // SLO §1.1
		CacheTTL:           15 * time.Second,
	}
	if cfg.PrometheusURL == "" {
		return config{}, errors.New("STATUSD_PROMETHEUS_URL must be set (e.g. http://prometheus:9090)")
	}
	if v := os.Getenv("STATUSD_AVAILABILITY_TARGET"); v != "" {
		t, err := strconv.ParseFloat(v, 64)
		if err != nil || t <= 0 || t >= 1 {
			return config{}, fmt.Errorf("STATUSD_AVAILABILITY_TARGET must be 0<t<1: %q", v)
		}
		cfg.AvailabilityTarget = t
	}
	if v := os.Getenv("STATUSD_CACHE_TTL"); v != "" {
		d, err := time.ParseDuration(v)
		if err != nil || d <= 0 {
			return config{}, fmt.Errorf("STATUSD_CACHE_TTL must be a positive duration: %q", v)
		}
		cfg.CacheTTL = d
	}
	return cfg, nil
}

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}
