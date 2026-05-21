package server

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/pageros/pageros/push-relay/internal/storage"
)

// statsStubStore lets a test drive Stats() return values + Ping outcomes
// without touching Redis. Implements the storage.Store interface.
type statsStubStore struct {
	pingErr  error
	stats    storage.Stats
	statsErr error
	calls    atomic.Int64
}

func (s *statsStubStore) Ping(context.Context) error { return s.pingErr }
func (s *statsStubStore) Close() error               { return nil }
func (s *statsStubStore) Enqueue(context.Context, string, storage.Notification) (storage.Notification, error) {
	return storage.Notification{}, nil
}
func (s *statsStubStore) List(context.Context, string) ([]storage.Notification, error) {
	return nil, nil
}
func (s *statsStubStore) Delete(context.Context, string, string) (bool, error) { return false, nil }
func (s *statsStubStore) Stats(context.Context) (storage.Stats, error) {
	s.calls.Add(1)
	return s.stats, s.statsErr
}

func TestMetricsEndpointExposesBuildInfo(t *testing.T) {
	srv := New(Options{Storage: &fakeStore{}, BuildTag: "v1.2.3"})
	req := httptest.NewRequest(http.MethodGet, "/metrics", nil)
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rec.Code)
	}
	body := rec.Body.String()
	if !strings.Contains(body, `push_relay_build_info{tag="v1.2.3"} 1`) {
		t.Errorf("missing build_info series in:\n%s", body)
	}
	if !strings.Contains(body, "push_relay_push_requests_total") {
		t.Errorf("missing push_requests_total help line in:\n%s", body)
	}
}

func TestInstrumentRecordsResultFromStatus(t *testing.T) {
	srv := New(Options{Storage: &fakeStore{}, BuildTag: "test"})

	// Hit /pull with no auth — should yield a 401, recorded as
	// result="unauthorized".
	req := httptest.NewRequest(http.MethodGet, "/pull/abcdef", nil)
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", rec.Code)
	}

	// Scrape metrics and look for the labelled counter.
	scrape := httptest.NewRecorder()
	srv.Handler().ServeHTTP(scrape, httptest.NewRequest(http.MethodGet, "/metrics", nil))
	body := scrape.Body.String()
	if !strings.Contains(body, `push_relay_pull_requests_total{result="unauthorized"} 1`) {
		t.Errorf("missing pull unauthorized counter, got:\n%s", body)
	}
}

func TestStatsRefresherUpdatesGauges(t *testing.T) {
	store := &statsStubStore{stats: storage.Stats{Devices: 3, Entries: 9, MaxDepth: 5}}
	set := NewMetricSet("test")

	r := NewStatsRefresher(store, set, 0, 0, nil)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	done := make(chan struct{})
	go func() {
		r.Run(ctx)
		close(done)
	}()

	// Wait for the first sample, which fires immediately on Run().
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		if store.calls.Load() > 0 {
			break
		}
		time.Sleep(2 * time.Millisecond)
	}
	if store.calls.Load() == 0 {
		t.Fatal("Stats() never called")
	}

	cancel()
	<-done

	// Scrape and inspect.
	rec := httptest.NewRecorder()
	(set.Registry).WriteExposition(rec.Body)
	body := rec.Body.String()
	for _, want := range []string{
		"push_relay_storage_up 1",
		"push_relay_queue_devices 3",
		"push_relay_queue_entries 9",
		"push_relay_queue_depth_max 5",
	} {
		if !strings.Contains(body, want) {
			t.Errorf("metrics body missing %q:\n%s", want, body)
		}
	}
}

func TestStatsRefresherMarksStorageDownOnPingFail(t *testing.T) {
	store := &statsStubStore{pingErr: context.DeadlineExceeded}
	set := NewMetricSet("test")
	// Pre-seed the queue gauges so we can prove they're NOT zeroed when the
	// ping fails — see the comment in StatsRefresher.sampleOnce.
	set.QueueDevices.Set(7)

	r := NewStatsRefresher(store, set, 0, 0, nil)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan struct{})
	go func() { r.Run(ctx); close(done) }()

	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) && store.calls.Load() == 0 {
		time.Sleep(2 * time.Millisecond)
	}
	// We don't call Stats on ping fail, so calls stays at 0. Sample the
	// metrics directly and check StorageUp went to 0.
	cancel()
	<-done

	rec := httptest.NewRecorder()
	set.Registry.WriteExposition(rec.Body)
	body := rec.Body.String()
	if !strings.Contains(body, "push_relay_storage_up 0") {
		t.Errorf("expected storage_up 0 after ping failure, got:\n%s", body)
	}
	if !strings.Contains(body, "push_relay_queue_devices 7") {
		t.Errorf("expected queue_devices to stay at last value 7 on ping fail, got:\n%s", body)
	}
}
