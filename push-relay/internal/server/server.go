// Package server wires the push-relay HTTP listener and handlers.
package server

import (
	"context"
	"crypto/tls"
	"log/slog"
	"net/http"
	"time"

	"github.com/pageros/pageros/push-relay/internal/manifest"
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

	mux := http.NewServeMux()
	mux.Handle("/healthz", newHealthzHandler(opts.Storage, opts.BuildTag))
	mux.Handle("GET /pull/{device_pubkey}", newPullHandler(opts.Storage, opts.Logger, 0))
	mux.Handle("POST /push/{device_pubkey}", newPushHandler(opts.Storage, opts.Manifest, opts.Logger, 0, opts.MaxPushBytes))
	mux.Handle("DELETE /pull/{device_pubkey}/{notification_id}", newAckHandler(opts.Storage, opts.Logger, 0))

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
