// Package stats publishes anonymized uptime + requests-per-hour
// counters to a project-operated dashboard (EXIT-007, SPEC.md §11.2).
//
// The publisher is opt-in: a zero-value Config (or one with Enabled=false
// or an empty URL) results in NewPublisher returning a no-op Run, so
// nothing leaves the node unless an operator deliberately enabled it.
//
// Anonymization: the POST body intentionally carries no device pubkeys,
// no request URLs, no client IPs, and no per-request data. Only
// aggregate uptime + per-hour rate-of-change of monotonic loop
// counters. node_name is optional and operator-supplied; omit it to
// publish a strictly anonymous heartbeat.
package stats

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net/http"
	"time"
)

// PayloadVersion is the on-the-wire schema version. Bump when the
// JSON shape changes in a way receivers must distinguish.
const PayloadVersion = 1

// DefaultInterval matches the LORA-005 beacon cadence so dashboard
// operators see roughly the same time-grain across the two streams.
const DefaultInterval = 5 * time.Minute

// DefaultHTTPTimeout caps a single POST so a slow dashboard cannot
// stall the publisher loop.
const DefaultHTTPTimeout = 10 * time.Second

// LoopSnapshot is the subset of loop counters the publisher consumes.
// Mirrors lora.Stats's fields without importing the lora package, so
// the stats publisher stays unit-testable with a stub.
type LoopSnapshot struct {
	PacketsReceived   uint64
	PacketsDropped    uint64
	PacketsSent       uint64
	MessagesAssembled uint64
	HandlerErrors     uint64
}

// Source is anything that can produce a LoopSnapshot — typically the
// lora.Loop, but tests pass a stub.
type Source interface {
	Snapshot() LoopSnapshot
}

// SourceFunc adapts a function to Source.
type SourceFunc func() LoopSnapshot

// Snapshot implements Source.
func (f SourceFunc) Snapshot() LoopSnapshot { return f() }

// Config parameterises a Publisher.
type Config struct {
	// Enabled gates the entire publisher; false ⇒ Run returns
	// immediately (the opt-in property from SPEC.md §11.2).
	Enabled bool

	// URL is the dashboard endpoint. Required when Enabled.
	URL string

	// NodeName is an optional operator label included verbatim in the
	// payload. Leave empty for a strictly anonymous heartbeat.
	NodeName string

	// Interval between POSTs. Defaults to DefaultInterval.
	Interval time.Duration

	// Source supplies counters at publish time. Required when Enabled.
	Source Source

	// HTTPClient overrides the default client. Tests use httptest's
	// client; production leaves nil for a default-timeout client.
	HTTPClient *http.Client

	// Logger receives non-fatal diagnostics (post failures). nil
	// disables logging.
	Logger *log.Logger

	// Now lets tests inject a clock. Defaults to time.Now.
	Now func() time.Time
}

// Payload is the JSON body POSTed to Config.URL.
type Payload struct {
	Version        int    `json:"version"`
	NodeName       string `json:"node_name,omitempty"`
	UptimeSec      uint64 `json:"uptime_s"`
	IntervalSec    uint64 `json:"interval_s"`
	RequestsPerHr  uint64 `json:"requests_per_hour"`
	PacketsRxTotal uint64 `json:"packets_received_total"`
	PacketsTxTotal uint64 `json:"packets_sent_total"`
	HandlerErrors  uint64 `json:"handler_errors_total"`
}

// Publisher periodically POSTs an anonymized counter snapshot.
type Publisher struct {
	cfg      Config
	client   *http.Client
	start    time.Time
	history  []sample
}

type sample struct {
	at    time.Time
	count uint64
}

// NewPublisher validates cfg and returns a runnable Publisher.
// An opt-out config (Enabled=false or empty URL) is not an error —
// the returned Publisher's Run is a no-op.
func NewPublisher(cfg Config) (*Publisher, error) {
	if cfg.Enabled {
		if cfg.URL == "" {
			return nil, errors.New("stats: URL required when Enabled")
		}
		if cfg.Source == nil {
			return nil, errors.New("stats: Source required when Enabled")
		}
	}
	if cfg.Interval <= 0 {
		cfg.Interval = DefaultInterval
	}
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	client := cfg.HTTPClient
	if client == nil {
		client = &http.Client{Timeout: DefaultHTTPTimeout}
	}
	return &Publisher{
		cfg:    cfg,
		client: client,
		start:  cfg.Now(),
	}, nil
}

// Run posts on Config.Interval until ctx is cancelled. Returns
// ctx.Err() on cancellation, or nil for an opt-out config (where Run
// is intentionally a no-op).
func (p *Publisher) Run(ctx context.Context) error {
	if !p.cfg.Enabled {
		return nil
	}
	t := time.NewTicker(p.cfg.Interval)
	defer t.Stop()
	// One immediate tick so dashboards see liveness quickly after start.
	if err := p.publishOnce(ctx); err != nil && !errors.Is(err, context.Canceled) {
		p.logf("stats: initial publish: %v", err)
	}
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-t.C:
			if err := p.publishOnce(ctx); err != nil && !errors.Is(err, context.Canceled) {
				p.logf("stats: publish: %v", err)
			}
		}
	}
}

// publishOnce builds a Payload from the current snapshot and POSTs it.
// Errors are returned to Run, which decides whether to log.
func (p *Publisher) publishOnce(ctx context.Context) error {
	snap := p.cfg.Source.Snapshot()
	now := p.cfg.Now()
	payload := Payload{
		Version:        PayloadVersion,
		NodeName:       p.cfg.NodeName,
		UptimeSec:      uint64(now.Sub(p.start).Seconds()),
		IntervalSec:    uint64(p.cfg.Interval.Seconds()),
		RequestsPerHr:  p.requestsPerHour(now, snap.MessagesAssembled),
		PacketsRxTotal: snap.PacketsReceived,
		PacketsTxTotal: snap.PacketsSent,
		HandlerErrors:  snap.HandlerErrors,
	}
	body, err := json.Marshal(payload)
	if err != nil {
		return fmt.Errorf("marshal: %w", err)
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, p.cfg.URL, bytes.NewReader(body))
	if err != nil {
		return fmt.Errorf("build request: %w", err)
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("User-Agent", "pageros-exit-node-stats/1")
	resp, err := p.client.Do(req)
	if err != nil {
		return fmt.Errorf("post: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode/100 != 2 {
		return fmt.Errorf("dashboard returned %d", resp.StatusCode)
	}
	return nil
}

// requestsPerHour returns the delta of MessagesAssembled over the
// most recent ~1h. Samples are appended each publish; old samples
// older than 1h+interval are pruned. Returns 0 until at least one
// prior sample exists.
func (p *Publisher) requestsPerHour(now time.Time, total uint64) uint64 {
	const window = time.Hour
	// Find the oldest sample still inside the window. If none, the
	// "rate" is what we have over the elapsed wall time.
	cutoff := now.Add(-window)
	keep := p.history[:0]
	for _, s := range p.history {
		if !s.at.Before(cutoff) {
			keep = append(keep, s)
		}
	}
	p.history = keep
	var rate uint64
	if len(p.history) > 0 {
		oldest := p.history[0]
		if total >= oldest.count {
			delta := total - oldest.count
			elapsed := now.Sub(oldest.at)
			if elapsed > 0 {
				// Scale to one hour. Use int math to avoid float noise
				// at low rates (1 request in 5 minutes → 12/hour).
				rate = delta * uint64(time.Hour) / uint64(elapsed)
			}
		}
	}
	p.history = append(p.history, sample{at: now, count: total})
	return rate
}

func (p *Publisher) logf(format string, args ...any) {
	if p.cfg.Logger == nil {
		return
	}
	p.cfg.Logger.Printf(format, args...)
}
