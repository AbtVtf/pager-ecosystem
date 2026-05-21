package stats

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// stubSource lets tests vary the snapshot returned each tick.
type stubSource struct {
	mu sync.Mutex
	v  LoopSnapshot
}

func (s *stubSource) Snapshot() LoopSnapshot {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.v
}

func (s *stubSource) Set(v LoopSnapshot) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.v = v
}

// collectingServer records every POST body for assertions.
type collectingServer struct {
	mu       sync.Mutex
	posts    [][]byte
	statusOK bool
}

func newCollectingServer() (*httptest.Server, *collectingServer) {
	cs := &collectingServer{statusOK: true}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		cs.mu.Lock()
		cs.posts = append(cs.posts, body)
		ok := cs.statusOK
		cs.mu.Unlock()
		if ok {
			w.WriteHeader(http.StatusOK)
			return
		}
		w.WriteHeader(http.StatusInternalServerError)
	}))
	return srv, cs
}

func (c *collectingServer) Posts() [][]byte {
	c.mu.Lock()
	defer c.mu.Unlock()
	out := make([][]byte, len(c.posts))
	copy(out, c.posts)
	return out
}

func TestOptOutByDisabled(t *testing.T) {
	src := &stubSource{}
	p, err := NewPublisher(Config{Enabled: false, URL: "http://example/", Source: src})
	if err != nil {
		t.Fatal(err)
	}
	// Run should return promptly without doing anything.
	ctx, cancel := context.WithTimeout(context.Background(), 200*time.Millisecond)
	defer cancel()
	if err := p.Run(ctx); err != nil {
		t.Errorf("opt-out Run should return nil, got %v", err)
	}
}

func TestOptOutByEmptyURL(t *testing.T) {
	if _, err := NewPublisher(Config{Enabled: true, URL: "", Source: &stubSource{}}); err == nil {
		t.Fatal("expected error when Enabled and URL empty")
	}
}

func TestPublishOnceShapeAndAnonymization(t *testing.T) {
	srv, cs := newCollectingServer()
	defer srv.Close()

	src := &stubSource{}
	src.Set(LoopSnapshot{
		PacketsReceived:   42,
		PacketsSent:       40,
		MessagesAssembled: 30,
		HandlerErrors:     2,
	})

	now := time.Unix(1_700_000_000, 0)
	clock := &atomic.Int64{}
	clock.Store(now.UnixNano())
	tick := func() time.Time { return time.Unix(0, clock.Load()) }

	p, err := NewPublisher(Config{
		Enabled:    true,
		URL:        srv.URL,
		NodeName:   "edge-test-1",
		Interval:   30 * time.Second,
		Source:     src,
		HTTPClient: srv.Client(),
		Now:        tick,
	})
	if err != nil {
		t.Fatal(err)
	}

	// Advance the clock 1 hour from the publisher's start time.
	clock.Store(now.Add(time.Hour).UnixNano())

	if err := p.publishOnce(context.Background()); err != nil {
		t.Fatalf("publishOnce: %v", err)
	}
	posts := cs.Posts()
	if len(posts) != 1 {
		t.Fatalf("posts: got %d want 1", len(posts))
	}

	var got Payload
	if err := json.Unmarshal(posts[0], &got); err != nil {
		t.Fatalf("payload not json: %v\nraw=%s", err, string(posts[0]))
	}
	if got.Version != PayloadVersion {
		t.Errorf("version: got %d want %d", got.Version, PayloadVersion)
	}
	if got.NodeName != "edge-test-1" {
		t.Errorf("node_name: got %q", got.NodeName)
	}
	if got.UptimeSec != uint64(time.Hour.Seconds()) {
		t.Errorf("uptime_s: got %d want %d", got.UptimeSec, uint64(time.Hour.Seconds()))
	}
	if got.IntervalSec != 30 {
		t.Errorf("interval_s: got %d want 30", got.IntervalSec)
	}
	if got.PacketsRxTotal != 42 || got.PacketsTxTotal != 40 {
		t.Errorf("packet totals: rx=%d tx=%d", got.PacketsRxTotal, got.PacketsTxTotal)
	}
	if got.HandlerErrors != 2 {
		t.Errorf("handler_errors_total: got %d want 2", got.HandlerErrors)
	}
	// Anonymization contract: no device pubkeys, URLs, IPs etc.
	// We assert by re-marshalling and confirming only the known
	// allow-listed keys are present.
	var asMap map[string]any
	if err := json.Unmarshal(posts[0], &asMap); err != nil {
		t.Fatal(err)
	}
	allowed := map[string]bool{
		"version": true, "node_name": true, "uptime_s": true, "interval_s": true,
		"requests_per_hour": true, "packets_received_total": true,
		"packets_sent_total": true, "handler_errors_total": true,
	}
	for k := range asMap {
		if !allowed[k] {
			t.Errorf("payload leaked key %q (anonymization contract requires explicit allow-list)", k)
		}
	}
}

func TestRequestsPerHourSlidingWindow(t *testing.T) {
	src := &stubSource{}
	now := time.Unix(1_700_000_000, 0)
	clock := &atomic.Int64{}
	clock.Store(now.UnixNano())

	srv, _ := newCollectingServer()
	defer srv.Close()

	p, err := NewPublisher(Config{
		Enabled:    true,
		URL:        srv.URL,
		Interval:   10 * time.Minute,
		Source:     src,
		HTTPClient: srv.Client(),
		Now:        func() time.Time { return time.Unix(0, clock.Load()) },
	})
	if err != nil {
		t.Fatal(err)
	}

	// Sample 1 at t=0: total=0 → no history, rate=0.
	src.Set(LoopSnapshot{MessagesAssembled: 0})
	if r := p.requestsPerHour(time.Unix(0, clock.Load()), 0); r != 0 {
		t.Errorf("first sample rate: got %d want 0", r)
	}

	// Sample 2 at t=+15m: total=10 → 10 requests in 15 min ⇒ 40/hr.
	clock.Store(now.Add(15 * time.Minute).UnixNano())
	if r := p.requestsPerHour(time.Unix(0, clock.Load()), 10); r != 40 {
		t.Errorf("15-min sample rate: got %d want 40", r)
	}

	// Sample 3 at t=+1h15m: oldest sample now slips outside the
	// 1-hour window (it was at t=0). The 15-min sample is still
	// inside, so rate = (50-10)/(60min) ⇒ 40/hr.
	clock.Store(now.Add(75 * time.Minute).UnixNano())
	if r := p.requestsPerHour(time.Unix(0, clock.Load()), 50); r != 40 {
		t.Errorf("75-min sample rate: got %d want 40", r)
	}
}

func TestRunEmitsImmediateThenTicks(t *testing.T) {
	srv, cs := newCollectingServer()
	defer srv.Close()

	src := &stubSource{}
	src.Set(LoopSnapshot{MessagesAssembled: 5})

	p, err := NewPublisher(Config{
		Enabled:    true,
		URL:        srv.URL,
		Interval:   25 * time.Millisecond,
		Source:     src,
		HTTPClient: srv.Client(),
	})
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 200*time.Millisecond)
	defer cancel()
	err = p.Run(ctx)
	if err != nil && !errors.Is(err, context.DeadlineExceeded) {
		t.Errorf("Run returned %v", err)
	}
	posts := cs.Posts()
	// At least one immediate + a few ticks within 200ms / 25ms.
	if len(posts) < 2 {
		t.Errorf("expected >=2 posts under repeated tick, got %d", len(posts))
	}
}

func TestPublishOnceSurfacesDashboardErrors(t *testing.T) {
	srv, cs := newCollectingServer()
	defer srv.Close()
	cs.statusOK = false

	src := &stubSource{}
	p, err := NewPublisher(Config{
		Enabled:    true,
		URL:        srv.URL,
		Source:     src,
		HTTPClient: srv.Client(),
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := p.publishOnce(context.Background()); err == nil {
		t.Fatal("expected error for non-2xx dashboard response")
	}
}
