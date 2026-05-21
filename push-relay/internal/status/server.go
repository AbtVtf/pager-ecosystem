package status

import (
	"context"
	"encoding/json"
	"errors"
	"html/template"
	"log/slog"
	"net/http"
	"sync"
	"time"
)

// Server is the HTTP listener that backs status.pageros.org. It caches the
// last successful Snapshot so a flap on the Prometheus side doesn't take
// the public page down for the cache lifetime.
type Server struct {
	builder *Builder
	tmpl    *template.Template
	logger  *slog.Logger
	cacheTTL time.Duration

	mu      sync.RWMutex
	last    Snapshot
	lastErr error
	lastAt  time.Time
}

// NewServer returns a Server using the supplied builder. cacheTTL is how
// long a successful Build's result is reused before re-querying Prometheus;
// the recommended value is 15s, matching the relay's Prometheus scrape
// interval (no point fetching faster than upstream data refreshes).
func NewServer(builder *Builder, cacheTTL time.Duration, logger *slog.Logger) *Server {
	if cacheTTL <= 0 {
		cacheTTL = 15 * time.Second
	}
	if logger == nil {
		logger = slog.Default()
	}
	return &Server{
		builder:  builder,
		tmpl:     template.Must(template.New("status").Funcs(templateFuncs()).Parse(htmlTemplate)),
		logger:   logger,
		cacheTTL: cacheTTL,
	}
}

// Handler returns the http.Handler. Routes:
//
//	GET /            — HTML status page
//	GET /api/status.json — machine-readable snapshot (for uptime aggregators)
//	GET /healthz     — status server's own liveness (NOT the relay's)
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.Handle("GET /", s.htmlHandler())
	mux.Handle("GET /api/status.json", s.jsonHandler())
	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"status":"ok"}`))
	})
	return mux
}

// snapshot returns a Snapshot, using the cached value when it's still fresh.
// On a Build failure, the previous-success snapshot is returned (stale-on-
// error) so the page stays useful during Prometheus blips. If we have
// never succeeded, the error is propagated.
func (s *Server) snapshot(ctx context.Context) (Snapshot, error) {
	s.mu.RLock()
	cached := s.last
	hasCached := !s.lastAt.IsZero() && s.lastErr == nil
	fresh := hasCached && time.Since(s.lastAt) < s.cacheTTL
	s.mu.RUnlock()
	if fresh {
		return cached, nil
	}

	snap, err := s.builder.Build(ctx)
	s.mu.Lock()
	defer s.mu.Unlock()
	if err != nil {
		s.lastErr = err
		// Serve stale-on-error if we ever saw a successful build.
		if hasCached {
			return cached, nil
		}
		return Snapshot{}, err
	}
	s.last = snap
	s.lastErr = nil
	s.lastAt = time.Now()
	return snap, nil
}

func (s *Server) htmlHandler() http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// The status page lives at "/" but we also catch /anything as a
		// fallthrough so simple typos still resolve to the page rather
		// than a 404 (this is a small public-facing surface — no point
		// being strict).
		snap, err := s.snapshot(r.Context())
		if err != nil {
			s.logger.Error("status: snapshot failed", "err", err)
			http.Error(w, "status temporarily unavailable", http.StatusServiceUnavailable)
			return
		}
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Header().Set("Cache-Control", "max-age=15")
		if err := s.tmpl.Execute(w, snap); err != nil {
			s.logger.Error("status: template execute failed", "err", err)
		}
	})
}

func (s *Server) jsonHandler() http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		snap, err := s.snapshot(r.Context())
		if err != nil {
			if errors.Is(err, context.Canceled) {
				return
			}
			http.Error(w, `{"error":"unavailable"}`, http.StatusServiceUnavailable)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "max-age=15")
		_ = json.NewEncoder(w).Encode(snap)
	})
}
