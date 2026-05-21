// Command push-relay runs the PagerOS project-operated push notification relay.
//
// PUSH-001 deliverable: HTTP service skeleton with TLS, /healthz, and a
// pluggable storage backend. Subsequent PUSH-00x tasks layer on the actual
// /push, /pull, and /ack handlers, signature verification, rate limiting, etc.
package main

import (
	"context"
	"errors"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"net/url"

	"github.com/pageros/pageros/push-relay/internal/config"
	"github.com/pageros/pageros/push-relay/internal/manifest"
	"github.com/pageros/pageros/push-relay/internal/server"
	"github.com/pageros/pageros/push-relay/internal/storage"
)

func main() {
	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}))
	slog.SetDefault(logger)

	cfg, err := config.FromEnv()
	if err != nil {
		logger.Error("invalid configuration", "err", err)
		os.Exit(2)
	}

	startCtx, startCancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer startCancel()

	store, err := storage.OpenRedis(startCtx, cfg.RedisURL)
	if err != nil {
		logger.Error("storage open failed", "err", err)
		os.Exit(1)
	}
	defer store.Close()

	var mlookup manifest.Lookup
	if cfg.MarketplaceURL != "" {
		mlookup = manifest.NewHTTPClient(cfg.MarketplaceURL, nil)
	} else {
		logger.Warn("PUSH_RELAY_MARKETPLACE_URL unset — /push will return 503 for all requests until configured")
	}

	if cfg.AdminToken == "" {
		logger.Warn("PUSH_RELAY_ADMIN_TOKEN unset — admin dashboard disabled")
	}

	srv := server.New(server.Options{
		Addr:       cfg.Addr,
		TLSCert:    cfg.TLSCertFile,
		TLSKey:     cfg.TLSKeyFile,
		Storage:    store,
		Manifest:   mlookup,
		Logger:     logger,
		BuildTag:   cfg.BuildTag,
		AdminToken: cfg.AdminToken,
	})

	rootCtx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	// Background queue-depth + storage Ping sampler that feeds /metrics. We
	// own the goroutine lifecycle here so cancellation on SIGTERM stops the
	// scan cleanly before Shutdown returns.
	refresher := server.NewStatsRefresher(store, srv.Metrics(), 0, 0, logger)
	refresherDone := make(chan struct{})
	go func() {
		refresher.Run(rootCtx)
		close(refresherDone)
	}()

	errs := make(chan error, 1)
	go func() {
		logger.Info("push-relay starting",
			"addr", cfg.Addr,
			"tls", cfg.TLSCertFile != "",
			"storage", "redis",
			"redis", redactRedisURL(cfg.RedisURL),
			"marketplace", cfg.MarketplaceURL,
		)
		errs <- srv.ListenAndServe()
	}()

	select {
	case err := <-errs:
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			logger.Error("server exited", "err", err)
			os.Exit(1)
		}
	case <-rootCtx.Done():
		logger.Info("shutdown signal received")
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		if err := srv.Shutdown(shutdownCtx); err != nil {
			logger.Error("graceful shutdown failed", "err", err)
			os.Exit(1)
		}
		// Wait for the metrics refresher to drain after the listener stops so
		// the last sample doesn't race a Redis close.
		<-refresherDone
		logger.Info("push-relay stopped cleanly")
	}
}

func redactRedisURL(u string) string {
	if u == "" {
		return ""
	}
	parsed, err := storage.ParseRedisURL(u)
	if err != nil {
		return "<unparseable>"
	}
	if parsed.User != nil {
		if _, hasPass := parsed.User.Password(); hasPass {
			parsed.User = url.UserPassword(parsed.User.Username(), "***")
		}
	}
	return parsed.String()
}
