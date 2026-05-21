// Package server wires the push-relay HTTP listener and handlers.
package server

import (
	"context"
	"crypto/tls"
	"log/slog"
	"net/http"
	"time"

	"github.com/pageros/pageros/push-relay/internal/admin"
	"github.com/pageros/pageros/push-relay/internal/manifest"
	"github.com/pageros/pageros/push-relay/internal/metrics"
	"github.com/pageros/pageros/push-relay/internal/ratelimit"
	"github.com/pageros/pageros/push-relay/internal/storage"
)

type Options struct {
	Addr     string
	TLSCert  string
	TLSKey   string
	Storage  storage.Store
	Manifest manifest.Lookup
	Logger   *slog.Logger
	BuildTag string
	// MaxPushBytes optionally overrides DefaultMaxPushBytes for POST /push.
	MaxPushBytes int64
	// Admin owns send-volume metrics and the ban list (PUSH-008). When nil,
	// the server constructs a fresh in-memory store so the push handler can
	// still record volumes; admin routes only mount if AdminToken is set.
	Admin      *admin.Store
	AdminToken string
	// Limiter enforces the per-(app, device) push quotas from SPEC §6.6.4
	// (PUSH-006). When nil, the server constructs an in-memory limiter with
	// the default 60/hour + 1000/day caps.
	Limiter ratelimit.Limiter
	// GroupLimiter enforces the same quotas as Limiter but for group events
	// (SPEC §6.6.6 keeps the bucket separate from user notifications,
	// PUSH-007). When nil, the server allocates a fresh in-memory limiter.
	GroupLimiter ratelimit.Limiter
	// Metrics is the Prometheus surface (PUSH-009). When nil, the server
	// constructs a fresh MetricSet so /metrics still serves something — but
	// callers that want to inject custom metrics (or share a registry with
	// the rest of the process) should pass an instance.
	Metrics *MetricSet
}

type Server struct {
	opts   Options
	http   *http.Server
	useTLS bool
}

func New(opts Options) *Server {
	if opts.Logger == nil {
		opts.Logger = slog.Default()
	}

	if opts.Admin == nil {
		opts.Admin = admin.New()
	}
	if opts.Limiter == nil {
		opts.Limiter = ratelimit.NewMemory()
	}
	if opts.GroupLimiter == nil {
		opts.GroupLimiter = ratelimit.NewMemory()
	}
	if opts.Metrics == nil {
		opts.Metrics = NewMetricSet(opts.BuildTag)
	}

	mux := http.NewServeMux()
	mux.Handle("/healthz", newHealthzHandler(opts.Storage, opts.BuildTag))
	mux.Handle("/metrics", metrics.NewHandler(opts.Metrics.Registry))
	mux.Handle("GET /pull/{device_pubkey}", instrument(newPullHandler(opts.Storage, opts.Logger, 0), opts.Metrics.PullRequests))
	mux.Handle("POST /push/{device_pubkey}", instrument(newPushHandler(opts.Storage, opts.Manifest, opts.Admin, opts.Limiter, opts.Logger, 0, opts.MaxPushBytes), opts.Metrics.PushRequests))
	mux.Handle("DELETE /pull/{device_pubkey}/{notification_id}", instrument(newAckHandler(opts.Storage, opts.Logger, 0), opts.Metrics.AckRequests))
	mux.Handle("POST /group_push", instrument(newGroupPushHandler(opts.Storage, opts.Manifest, opts.Admin, opts.GroupLimiter, opts.Logger, 0, 0, 0, 0, opts.Metrics.GroupRecipients), opts.Metrics.GroupRequests))
	admin.Mount(mux, admin.MountConfig{Store: opts.Admin, Token: opts.AdminToken, Logger: opts.Logger})

	s := &Server{
		opts:   opts,
		useTLS: opts.TLSCert != "" && opts.TLSKey != "",
	}

	s.http = &http.Server{
		Addr:              opts.Addr,
		Handler:           mux,
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       15 * time.Second,
		WriteTimeout:      15 * time.Second,
		IdleTimeout:       60 * time.Second,
	}
	if s.useTLS {
		s.http.TLSConfig = &tls.Config{MinVersion: tls.VersionTLS12}
	}
	return s
}

// ListenAndServe blocks until the listener errors or Shutdown is called. It
// serves TLS when both cert and key paths are configured, plain HTTP otherwise
// (intended for local dev only — production deployments MUST set TLS).
func (s *Server) ListenAndServe() error {
	if s.useTLS {
		return s.http.ListenAndServeTLS(s.opts.TLSCert, s.opts.TLSKey)
	}
	s.opts.Logger.Warn("starting without TLS — dev mode only; set PUSH_RELAY_TLS_CERT and PUSH_RELAY_TLS_KEY for production")
	return s.http.ListenAndServe()
}

func (s *Server) Shutdown(ctx context.Context) error {
	return s.http.Shutdown(ctx)
}

// Handler exposes the underlying mux for tests.
func (s *Server) Handler() http.Handler {
	return s.http.Handler
}

// Metrics returns the metric set wired into the server, useful for the
// stats refresher in main.go and for tests that need to assert counter
// state directly.
func (s *Server) Metrics() *MetricSet {
	return s.opts.Metrics
}
