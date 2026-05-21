package status

import (
	"context"
	"errors"
	"sort"
	"time"
)

// Snapshot is the fully-resolved status payload the page + JSON endpoint
// render from. Fields use small primitive types so the JSON encoding is
// stable for downstream uptime aggregators.
type Snapshot struct {
	// Generated is the time the snapshot was built. Renderers display this
	// as "Last updated".
	Generated time.Time `json:"generated"`

	// Overall is the rolled-up status string: "operational",
	// "degraded", or "down". Computed from Components.
	Overall string `json:"overall"`

	// Components is the per-subsystem status list, ordered by
	// surface-facing importance (relay first).
	Components []Component `json:"components"`

	// Availability28d is the 28-day rolling availability as a fraction
	// (0..1). When Prometheus has not yet built up 28 days of data, this
	// will be the longest window it can compute.
	Availability28d float64 `json:"availability_28d"`

	// AvailabilityTarget is the SLO target (0..1) — duplicated from the
	// SLO document so the page is self-describing. Pin to SPEC SLO §1.1.
	AvailabilityTarget float64 `json:"availability_target"`

	// ErrorBudgetRemaining is the fraction (0..1) of the 28-day budget
	// still available. Negative values indicate the budget is exhausted.
	ErrorBudgetRemaining float64 `json:"error_budget_remaining"`

	// QueueEntries / QueueDevices come straight from the gauges added in
	// PUSH-009 so the public page reflects current backlog.
	QueueEntries int64 `json:"queue_entries"`
	QueueDevices int64 `json:"queue_devices"`

	// UptimeSeconds is `time() - push_relay_started_at_seconds` at the
	// time of snapshot. Useful for the "current process uptime" line.
	UptimeSeconds int64 `json:"uptime_seconds"`
}

// Component is one row on the status page (relay HTTP, storage, marketplace).
type Component struct {
	Name    string `json:"name"`
	Status  string `json:"status"`            // operational | degraded | down | unknown
	Detail  string `json:"detail,omitempty"`  // one-line human note
	MetricQ string `json:"metric_q,omitempty"` // PromQL expression used (for transparency)
}

const (
	StatusOperational = "operational"
	StatusDegraded    = "degraded"
	StatusDown        = "down"
	StatusUnknown     = "unknown"
)

// Builder composes a Snapshot from a PromClient. It owns the queries so the
// HTTP handler stays a thin wrapper. Splitting the queries off the handler
// also lets tests pump fake Prometheus responses through directly.
type Builder struct {
	prom               *PromClient
	availabilityTarget float64
	now                func() time.Time
}

// NewBuilder takes a Prometheus client and the SLO target (0..1, e.g. 0.995
// for the §1.1 99.5% commitment).
func NewBuilder(prom *PromClient, target float64) *Builder {
	return &Builder{
		prom:               prom,
		availabilityTarget: target,
		now:                time.Now,
	}
}

// Build evaluates all the status queries and returns a Snapshot. Failures
// on any individual query degrade just that component to "unknown" and
// continue — we never want a single bad query to take the public page down.
func (b *Builder) Build(ctx context.Context) (Snapshot, error) {
	if b.prom == nil {
		return Snapshot{}, errors.New("status: no prometheus client configured")
	}

	snap := Snapshot{
		Generated:          b.now().UTC(),
		AvailabilityTarget: b.availabilityTarget,
		Components:         make([]Component, 0, 3),
	}

	// Component 1: relay HTTP up/down.
	const upQ = `max(up{job="push-relay"})`
	relayUp, err := b.prom.Query(ctx, upQ)
	switch {
	case err == nil && relayUp >= 1:
		snap.Components = append(snap.Components, Component{
			Name: "Push Relay", Status: StatusOperational,
			Detail: "Accepting /push, /pull, and ack requests.", MetricQ: upQ,
		})
	case err == nil:
		snap.Components = append(snap.Components, Component{
			Name: "Push Relay", Status: StatusDown,
			Detail: "Prometheus reports no successful scrapes.", MetricQ: upQ,
		})
	default:
		snap.Components = append(snap.Components, Component{
			Name: "Push Relay", Status: StatusUnknown,
			Detail: "Could not query Prometheus.", MetricQ: upQ,
		})
	}

	// Component 2: storage backend.
	const storageQ = `min(push_relay_storage_up)`
	storage, err := b.prom.Query(ctx, storageQ)
	switch {
	case err == nil && storage >= 1:
		snap.Components = append(snap.Components, Component{
			Name: "Storage (Redis)", Status: StatusOperational,
			Detail: "Last Ping succeeded.", MetricQ: storageQ,
		})
	case err == nil:
		snap.Components = append(snap.Components, Component{
			Name: "Storage (Redis)", Status: StatusDown,
			Detail: "Last Ping failed; /push and /pull will 5xx until recovered.", MetricQ: storageQ,
		})
	default:
		snap.Components = append(snap.Components, Component{
			Name: "Storage (Redis)", Status: StatusUnknown,
			Detail: "Could not read storage gauge.", MetricQ: storageQ,
		})
	}

	// Component 3: marketplace dependency. Inferred from /push error rate
	// — direct upstream-up queries would require a separate scrape target.
	// 0 means no lookup_unavailable in the last 5 min (operational).
	const mktQ = `sum(rate(push_relay_push_requests_total{result="lookup_unavailable"}[5m]))`
	mktErrors, err := b.prom.Query(ctx, mktQ)
	switch {
	case errors.Is(err, ErrNoSamples), err == nil && mktErrors == 0:
		snap.Components = append(snap.Components, Component{
			Name: "Marketplace lookup", Status: StatusOperational,
			Detail: "No lookup errors in the last 5 minutes.", MetricQ: mktQ,
		})
	case err == nil:
		snap.Components = append(snap.Components, Component{
			Name: "Marketplace lookup", Status: StatusDegraded,
			Detail: "Manifest lookups are failing; /push returns 503.", MetricQ: mktQ,
		})
	default:
		snap.Components = append(snap.Components, Component{
			Name: "Marketplace lookup", Status: StatusUnknown,
			Detail: "Could not read marketplace error counter.", MetricQ: mktQ,
		})
	}

	// 28-day rolling availability. Uses avg_over_time on `up` AND
	// `push_relay_storage_up`. The product treats both as required —
	// if either is 0, the minute is "bad". `avg_over_time` over the full
	// 28d returns a 0..1 number directly. ErrNoSamples is treated as
	// "no data yet" rather than 0% available.
	const availQ = `avg_over_time((up{job="push-relay"} * push_relay_storage_up)[28d:1m])`
	avail, err := b.prom.Query(ctx, availQ)
	if err == nil {
		snap.Availability28d = avail
		// Error budget: 1 - (1 - actual)/(1 - target) gives the
		// fraction of the budget remaining. Clamp lower bound for
		// display sanity.
		if b.availabilityTarget < 1 {
			budgetUsed := (1 - avail) / (1 - b.availabilityTarget)
			snap.ErrorBudgetRemaining = 1 - budgetUsed
		}
	}

	// Queue gauges (PUSH-009).
	const queueEntriesQ = `sum(push_relay_queue_entries)`
	const queueDevicesQ = `sum(push_relay_queue_devices)`
	if v, qerr := b.prom.Query(ctx, queueEntriesQ); qerr == nil {
		snap.QueueEntries = int64(v)
	}
	if v, qerr := b.prom.Query(ctx, queueDevicesQ); qerr == nil {
		snap.QueueDevices = int64(v)
	}

	// Uptime gauge.
	const uptimeQ = `time() - max(push_relay_started_at_seconds)`
	if v, uerr := b.prom.Query(ctx, uptimeQ); uerr == nil {
		snap.UptimeSeconds = int64(v)
	}

	snap.Overall = rollupOverall(snap.Components)
	return snap, nil
}

// rollupOverall returns the worst (most-severe) status across components.
// Status precedence: down > degraded > unknown > operational. "unknown"
// outranks operational because we'd rather understate availability when we
// genuinely can't see it.
func rollupOverall(comps []Component) string {
	const (
		opRank   = 0
		unkRank  = 1
		degRank  = 2
		downRank = 3
	)
	rank := func(s string) int {
		switch s {
		case StatusOperational:
			return opRank
		case StatusUnknown:
			return unkRank
		case StatusDegraded:
			return degRank
		case StatusDown:
			return downRank
		default:
			return unkRank
		}
	}
	if len(comps) == 0 {
		return StatusUnknown
	}
	sorted := append([]Component(nil), comps...)
	sort.SliceStable(sorted, func(i, j int) bool {
		return rank(sorted[i].Status) > rank(sorted[j].Status)
	})
	return sorted[0].Status
}
