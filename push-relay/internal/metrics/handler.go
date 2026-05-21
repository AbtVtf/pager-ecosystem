package metrics

import (
	"net/http"
)

// NewHandler returns an http.Handler that emits Prometheus text exposition for
// r. The exposition format is the version-0 text format; Prometheus servers
// negotiate that by default, so we don't bother with the experimental OpenMetrics
// content negotiation.
func NewHandler(r *Registry) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, req *http.Request) {
		if req.Method != http.MethodGet && req.Method != http.MethodHead {
			w.Header().Set("Allow", "GET, HEAD")
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		w.Header().Set("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
		if req.Method == http.MethodHead {
			w.WriteHeader(http.StatusOK)
			return
		}
		if err := r.WriteExposition(w); err != nil {
			// At this point status has already gone out (200) — best we can
			// do is log via the response writer's underlying transport, which
			// Go handles by closing the conn. No-op here.
			return
		}
	})
}
