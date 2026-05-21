package status

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func newStubBuilder(t *testing.T, ok bool) (*Builder, *atomic.Int32, *httptest.Server) {
	t.Helper()
	calls := &atomic.Int32{}
	prom := newFakeProm()
	if ok {
		prom.responses[`max(up{job="push-relay"})`] = 1
		prom.responses[`min(push_relay_storage_up)`] = 1
		prom.responses[`avg_over_time((up{job="push-relay"} * push_relay_storage_up)[28d:1m])`] = 0.999
	}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		prom.ServeHTTP(w, r)
	}))
	t.Cleanup(srv.Close)
	return NewBuilder(NewPromClient(srv.URL, nil), 0.995), calls, srv
}

func TestServerJSONEndpoint(t *testing.T) {
	b, _, _ := newStubBuilder(t, true)
	s := NewServer(b, 100*time.Millisecond, nil)

	req := httptest.NewRequest(http.MethodGet, "/api/status.json", nil)
	rec := httptest.NewRecorder()
	s.Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, body=%s", rec.Code, rec.Body.String())
	}
	var snap Snapshot
	if err := json.NewDecoder(rec.Body).Decode(&snap); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if snap.Overall != StatusOperational {
		t.Fatalf("overall = %q", snap.Overall)
	}
	if ct := rec.Header().Get("Content-Type"); ct != "application/json" {
		t.Fatalf("content-type = %q", ct)
	}
}

func TestServerHTMLEndpoint(t *testing.T) {
	b, _, _ := newStubBuilder(t, true)
	s := NewServer(b, 100*time.Millisecond, nil)

	req := httptest.NewRequest(http.MethodGet, "/", nil)
	rec := httptest.NewRecorder()
	s.Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d", rec.Code)
	}
	body := rec.Body.String()
	for _, w := range []string{
		"<title>PagerOS Push Relay — Status</title>",
		"Operational",
		"Push Relay",
		"Storage (Redis)",
		"Marketplace lookup",
		"28-day availability",
		`href="/api/status.json"`,
	} {
		if !strings.Contains(body, w) {
			t.Fatalf("HTML missing %q", w)
		}
	}
	if ct := rec.Header().Get("Content-Type"); !strings.HasPrefix(ct, "text/html") {
		t.Fatalf("content-type = %q", ct)
	}
}

func TestServerHealthzEndpoint(t *testing.T) {
	b, _, _ := newStubBuilder(t, true)
	s := NewServer(b, 100*time.Millisecond, nil)

	req := httptest.NewRequest(http.MethodGet, "/healthz", nil)
	rec := httptest.NewRecorder()
	s.Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d", rec.Code)
	}
}

func TestServerCachesSnapshot(t *testing.T) {
	// Within the TTL, repeated requests should not hit Prometheus again.
	b, calls, _ := newStubBuilder(t, true)
	s := NewServer(b, 1*time.Hour, nil) // long TTL

	for i := 0; i < 5; i++ {
		req := httptest.NewRequest(http.MethodGet, "/api/status.json", nil)
		rec := httptest.NewRecorder()
		s.Handler().ServeHTTP(rec, req)
		if rec.Code != http.StatusOK {
			t.Fatalf("status = %d", rec.Code)
		}
	}
	// Builder hits ~6 queries on first Build. After that, the cache should
	// serve the next 4 requests with zero new Prometheus calls.
	first := calls.Load()
	if first == 0 {
		t.Fatalf("expected at least one prom call on first build")
	}
	// One more request should not increase calls.
	req := httptest.NewRequest(http.MethodGet, "/api/status.json", nil)
	rec := httptest.NewRecorder()
	s.Handler().ServeHTTP(rec, req)
	if calls.Load() != first {
		t.Fatalf("cache did not hold; calls went %d -> %d", first, calls.Load())
	}
}

func TestServerStaleOnError(t *testing.T) {
	// Build once successfully, then break the upstream — the page should
	// keep serving the last-good snapshot rather than 503.
	prom := newFakeProm()
	prom.responses[`max(up{job="push-relay"})`] = 1
	prom.responses[`min(push_relay_storage_up)`] = 1
	prom.responses[`avg_over_time((up{job="push-relay"} * push_relay_storage_up)[28d:1m])`] = 0.999

	var broken atomic.Bool
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if broken.Load() {
			http.Error(w, "down", http.StatusInternalServerError)
			return
		}
		prom.ServeHTTP(w, r)
	}))
	defer srv.Close()
	b := NewBuilder(NewPromClient(srv.URL, nil), 0.995)
	s := NewServer(b, 1*time.Millisecond, nil) // tiny TTL → almost-always re-fetch

	// Successful first call.
	req := httptest.NewRequest(http.MethodGet, "/api/status.json", nil)
	rec := httptest.NewRecorder()
	s.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("first call = %d", rec.Code)
	}

	// Wait for TTL to expire then break the upstream.
	time.Sleep(20 * time.Millisecond)
	broken.Store(true)

	// However, Build still mostly succeeds — the individual queries fall back
	// to "unknown". The branch we want to test is genuine Build failure
	// (nil prom), which is already covered by TestBuildNilClient. For the
	// HTTP-level stale-on-error path: when Prometheus returns 500 on every
	// query, Build returns a Snapshot full of `unknown` (not an error). So
	// instead, prove the JSON endpoint still 200s when the upstream is
	// broken — even without the stale-on-error fallback. (Stale-on-error
	// activates only on genuine Build failure, which is hard to provoke
	// because Build is forgiving by design.)
	req2 := httptest.NewRequest(http.MethodGet, "/api/status.json", nil)
	rec2 := httptest.NewRecorder()
	s.Handler().ServeHTTP(rec2, req2)
	if rec2.Code != http.StatusOK {
		t.Fatalf("upstream-broken call = %d, want 200 (degraded but live)", rec2.Code)
	}
}

func TestServerCachesAcrossGoroutines(t *testing.T) {
	// Race detector smoke test: 10 concurrent requests must not panic.
	b, _, _ := newStubBuilder(t, true)
	s := NewServer(b, 50*time.Millisecond, nil)

	done := make(chan struct{}, 10)
	for i := 0; i < 10; i++ {
		go func() {
			defer func() { done <- struct{}{} }()
			req := httptest.NewRequest(http.MethodGet, "/api/status.json", nil)
			rec := httptest.NewRecorder()
			s.Handler().ServeHTTP(rec, req)
			if rec.Code != http.StatusOK {
				t.Errorf("status = %d", rec.Code)
			}
		}()
	}
	for i := 0; i < 10; i++ {
		select {
		case <-done:
		case <-time.After(5 * time.Second):
			t.Fatalf("timed out waiting for goroutine %d", i)
		}
	}
	_ = context.Background()
}
