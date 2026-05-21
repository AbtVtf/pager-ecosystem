package status

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

// fakePromHandler routes by `query` parameter and returns canned responses.
// Tests register `{ "<query>": vectorValue }` and any unknown query gets an
// empty vector (ErrNoSamples). This is enough to drive Builder.Build through
// every branch.
type fakePromHandler struct {
	mu         sync.Mutex
	responses  map[string]float64
	statusFor  map[string]int  // optional non-200 status codes
	missingFor map[string]bool // queries that should return empty vector
}

func newFakeProm() *fakePromHandler {
	return &fakePromHandler{
		responses:  map[string]float64{},
		statusFor:  map[string]int{},
		missingFor: map[string]bool{},
	}
}

func (h *fakePromHandler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query().Get("query")
	h.mu.Lock()
	defer h.mu.Unlock()

	if code, ok := h.statusFor[q]; ok {
		w.WriteHeader(code)
		return
	}
	w.Header().Set("Content-Type", "application/json")

	if h.missingFor[q] {
		_, _ = w.Write([]byte(`{"status":"success","data":{"resultType":"vector","result":[]}}`))
		return
	}
	val, ok := h.responses[q]
	if !ok {
		// Unknown query → empty vector. Tests that want a value must set it.
		_, _ = w.Write([]byte(`{"status":"success","data":{"resultType":"vector","result":[]}}`))
		return
	}
	_, _ = w.Write([]byte(`{"status":"success","data":{"resultType":"vector","result":[{"value":[1,"` + jsonFloat(val) + `"]}]}}`))
}

// jsonFloat formats v as a plain decimal Prometheus would emit, trimming
// trailing zeros so the canned JSON stays compact.
func jsonFloat(v float64) string {
	s := fmt.Sprintf("%.9f", v)
	s = strings.TrimRight(s, "0")
	return strings.TrimRight(s, ".")
}

func TestBuildAllOperational(t *testing.T) {
	prom := newFakeProm()
	prom.responses[`max(up{job="push-relay"})`] = 1
	prom.responses[`min(push_relay_storage_up)`] = 1
	prom.responses[`sum(rate(push_relay_push_requests_total{result="lookup_unavailable"}[5m]))`] = 0
	prom.responses[`avg_over_time((up{job="push-relay"} * push_relay_storage_up)[28d:1m])`] = 0.9995
	prom.responses[`sum(push_relay_queue_entries)`] = 42
	prom.responses[`sum(push_relay_queue_devices)`] = 7
	prom.responses[`time() - max(push_relay_started_at_seconds)`] = 9000

	srv := httptest.NewServer(prom)
	defer srv.Close()
	b := NewBuilder(NewPromClient(srv.URL, nil), 0.995)

	snap, err := b.Build(context.Background())
	if err != nil {
		t.Fatalf("Build: %v", err)
	}
	if snap.Overall != StatusOperational {
		t.Fatalf("overall = %q, want operational", snap.Overall)
	}
	if snap.Availability28d != 0.9995 {
		t.Fatalf("availability = %v, want 0.9995", snap.Availability28d)
	}
	// Error budget remaining: 1 - (1-0.9995)/(1-0.995) = 1 - 0.1 = 0.9
	if snap.ErrorBudgetRemaining < 0.89 || snap.ErrorBudgetRemaining > 0.91 {
		t.Fatalf("error budget = %v, want ~0.9", snap.ErrorBudgetRemaining)
	}
	if snap.QueueEntries != 42 || snap.QueueDevices != 7 {
		t.Fatalf("queue gauges: entries=%d devices=%d", snap.QueueEntries, snap.QueueDevices)
	}
	if snap.UptimeSeconds != 9000 {
		t.Fatalf("uptime = %d, want 9000", snap.UptimeSeconds)
	}
	if len(snap.Components) != 3 {
		t.Fatalf("expected 3 components, got %d", len(snap.Components))
	}
	for _, c := range snap.Components {
		if c.Status != StatusOperational {
			t.Fatalf("component %q = %q, want operational", c.Name, c.Status)
		}
	}
}

func TestBuildRelayDownDominatesRollup(t *testing.T) {
	prom := newFakeProm()
	prom.responses[`max(up{job="push-relay"})`] = 0
	prom.responses[`min(push_relay_storage_up)`] = 1
	prom.responses[`sum(rate(push_relay_push_requests_total{result="lookup_unavailable"}[5m]))`] = 0
	srv := httptest.NewServer(prom)
	defer srv.Close()
	b := NewBuilder(NewPromClient(srv.URL, nil), 0.995)

	snap, err := b.Build(context.Background())
	if err != nil {
		t.Fatalf("Build: %v", err)
	}
	if snap.Overall != StatusDown {
		t.Fatalf("overall = %q, want down", snap.Overall)
	}
	// First component is the relay; it should be down.
	if snap.Components[0].Status != StatusDown {
		t.Fatalf("relay component = %q, want down", snap.Components[0].Status)
	}
	// Storage should still report operational — we don't propagate.
	if snap.Components[1].Status != StatusOperational {
		t.Fatalf("storage component = %q, want operational", snap.Components[1].Status)
	}
}

func TestBuildMarketplaceErrorsDegraded(t *testing.T) {
	prom := newFakeProm()
	prom.responses[`max(up{job="push-relay"})`] = 1
	prom.responses[`min(push_relay_storage_up)`] = 1
	prom.responses[`sum(rate(push_relay_push_requests_total{result="lookup_unavailable"}[5m]))`] = 0.5
	srv := httptest.NewServer(prom)
	defer srv.Close()
	b := NewBuilder(NewPromClient(srv.URL, nil), 0.995)

	snap, err := b.Build(context.Background())
	if err != nil {
		t.Fatalf("Build: %v", err)
	}
	if snap.Overall != StatusDegraded {
		t.Fatalf("overall = %q, want degraded", snap.Overall)
	}
}

func TestBuildPromUnavailableMarksComponentsUnknown(t *testing.T) {
	// Server unreachable: every Query fails. Builder should not return an
	// error — it should mark every component "unknown" instead, so the
	// public page still renders.
	c := NewPromClient("http://127.0.0.1:1", nil) // unroutable
	b := NewBuilder(c, 0.995)

	snap, err := b.Build(context.Background())
	if err != nil {
		t.Fatalf("Build returned error: %v", err)
	}
	if snap.Overall != StatusUnknown {
		t.Fatalf("overall = %q, want unknown", snap.Overall)
	}
	for _, c := range snap.Components {
		if c.Status != StatusUnknown {
			t.Fatalf("%s = %q, want unknown", c.Name, c.Status)
		}
	}
}

func TestBuildNilClient(t *testing.T) {
	b := NewBuilder(nil, 0.995)
	if _, err := b.Build(context.Background()); err == nil {
		t.Fatalf("expected error when prom is nil")
	}
}

func TestSnapshotJSONIsStable(t *testing.T) {
	// Downstream uptime aggregators consume status.json — guard against
	// accidental field renames.
	now := time.Date(2026, 5, 21, 12, 0, 0, 0, time.UTC)
	snap := Snapshot{
		Generated:            now,
		Overall:              StatusOperational,
		Availability28d:      0.9995,
		AvailabilityTarget:   0.995,
		ErrorBudgetRemaining: 0.9,
		QueueEntries:         10,
		QueueDevices:         2,
		UptimeSeconds:        3600,
		Components:           []Component{{Name: "Push Relay", Status: StatusOperational}},
	}
	b, err := json.Marshal(snap)
	if err != nil {
		t.Fatalf("Marshal: %v", err)
	}
	got := string(b)
	wantContains := []string{
		`"overall":"operational"`,
		`"availability_28d":0.9995`,
		`"availability_target":0.995`,
		`"error_budget_remaining":0.9`,
		`"queue_entries":10`,
		`"queue_devices":2`,
		`"uptime_seconds":3600`,
		`"components":[`,
	}
	for _, w := range wantContains {
		if !strings.Contains(got, w) {
			t.Fatalf("JSON missing %q in %s", w, got)
		}
	}
}

func TestRollupPrecedence(t *testing.T) {
	cases := []struct {
		in   []Component
		want string
	}{
		{[]Component{{Status: StatusOperational}, {Status: StatusOperational}}, StatusOperational},
		{[]Component{{Status: StatusOperational}, {Status: StatusUnknown}}, StatusUnknown},
		{[]Component{{Status: StatusUnknown}, {Status: StatusDegraded}}, StatusDegraded},
		{[]Component{{Status: StatusDegraded}, {Status: StatusDown}}, StatusDown},
		{nil, StatusUnknown},
	}
	for _, tc := range cases {
		if got := rollupOverall(tc.in); got != tc.want {
			t.Fatalf("rollup(%v) = %q, want %q", tc.in, got, tc.want)
		}
	}
}
