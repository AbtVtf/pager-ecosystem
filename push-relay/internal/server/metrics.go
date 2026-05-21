package server

import (
	"context"
	"errors"
	"log/slog"
	"net/http"
	"sync/atomic"
	"time"

	"github.com/pageros/pageros/push-relay/internal/metrics"
	"github.com/pageros/pageros/push-relay/internal/storage"
)

// MetricSet is the closed universe of metrics emitted by /metrics. Wiring it
// as a struct (rather than module-level globals) lets tests inspect a fresh
// registry per case without leaking state between runs.
type MetricSet struct {
	Registry *metrics.Registry

	BuildInfo  *metrics.Gauge
	StartedAt  *metrics.Gauge
	StorageUp  *metrics.Gauge
	QueueDevices *metrics.Gauge
	QueueEntries *metrics.Gauge
	QueueDepthMax *metrics.Gauge

	PushRequests  *metrics.Counter
	PullRequests  *metrics.Counter
	AckRequests   *metrics.Counter
	GroupRequests *metrics.Counter
	GroupRecipients *metrics.Counter
}

// NewMetricSet declares every counter + gauge on a fresh registry.
func NewMetricSet(buildTag string) *MetricSet {
	r := metrics.NewRegistry()
	m := &MetricSet{
		Registry:      r,
		BuildInfo:     r.NewGauge("push_relay_build_info", "Build tag exposed as a label; value is always 1."),
		StartedAt:     r.NewGauge("push_relay_started_at_seconds", "Unix timestamp of process start."),
		StorageUp:     r.NewGauge("push_relay_storage_up", "1 if storage Ping succeeded on last probe, 0 otherwise."),
		QueueDevices:  r.NewGauge("push_relay_queue_devices", "Number of devices currently holding at least one queued notification."),
		QueueEntries:  r.NewGauge("push_relay_queue_entries", "Total queued notifications across all devices."),
		QueueDepthMax: r.NewGauge("push_relay_queue_depth_max", "Largest single-device queue depth in the last snapshot."),

		PushRequests:    r.NewCounter("push_relay_push_requests_total", "POST /push outcomes, labelled by result enum."),
		PullRequests:    r.NewCounter("push_relay_pull_requests_total", "GET /pull outcomes, labelled by result enum."),
		AckRequests:     r.NewCounter("push_relay_ack_requests_total", "DELETE /pull/{id} outcomes, labelled by result enum."),
		GroupRequests:   r.NewCounter("push_relay_group_requests_total", "POST /group_push outcomes, labelled by result enum."),
		GroupRecipients: r.NewCounter("push_relay_group_recipients_total", "Per-recipient outcomes inside accepted group pushes, labelled by status."),
	}
	m.BuildInfo.Set(1, "tag", buildTag)
	m.StartedAt.Set(time.Now().Unix())
	return m
}

// Result enums (kept stable for alert routing).
const (
	ResultAccepted        = "accepted"
	ResultBadRequest      = "bad_request"
	ResultUnauthorized    = "unauthorized"
	ResultForbidden       = "forbidden"
	ResultPayloadTooLarge = "payload_too_large"
	ResultRateLimited     = "rate_limited"
	ResultLookupDown      = "lookup_unavailable"
	ResultStorageError    = "storage_error"
	ResultNotFound        = "not_found"
)

// recordPushOutcome looks at the response status the handler emitted and
// translates it into a Prometheus label. Decoupling the counter call from
// the handler's many error paths means we only have to update the mapping
// in one place when a new status appears.
func recordPushOutcome(c *metrics.Counter, status int) {
	if c == nil {
		return
	}
	c.Inc("result", resultFromStatus(status))
}

func resultFromStatus(status int) string {
	switch status {
	case http.StatusOK, http.StatusAccepted, http.StatusNoContent:
		return ResultAccepted
	case http.StatusBadRequest:
		return ResultBadRequest
	case http.StatusUnauthorized:
		return ResultUnauthorized
	case http.StatusForbidden:
		return ResultForbidden
	case http.StatusRequestEntityTooLarge:
		return ResultPayloadTooLarge
	case http.StatusTooManyRequests:
		return ResultRateLimited
	case http.StatusServiceUnavailable:
		return ResultLookupDown
	case http.StatusNotFound:
		return ResultNotFound
	case http.StatusInternalServerError:
		return ResultStorageError
	default:
		return "other"
	}
}

// statusRecorder wraps an http.ResponseWriter to capture the status code
// the handler writes, so we can label the request counter after the handler
// returns. The middleware below uses it; handlers are otherwise untouched.
type statusRecorder struct {
	http.ResponseWriter
	status int
	wrote  bool
}

func (s *statusRecorder) WriteHeader(code int) {
	if !s.wrote {
		s.status = code
		s.wrote = true
	}
	s.ResponseWriter.WriteHeader(code)
}

func (s *statusRecorder) Write(b []byte) (int, error) {
	if !s.wrote {
		// http.ResponseWriter contract: a Write before WriteHeader implies 200.
		s.status = http.StatusOK
		s.wrote = true
	}
	return s.ResponseWriter.Write(b)
}

// instrument wraps h so each call increments counter with a label derived from
// the response status. Counter MAY be nil.
func instrument(h http.Handler, counter *metrics.Counter) http.Handler {
	if counter == nil {
		return h
	}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		sr := &statusRecorder{ResponseWriter: w}
		h.ServeHTTP(sr, r)
		if sr.status == 0 {
			sr.status = http.StatusOK
		}
		recordPushOutcome(counter, sr.status)
	})
}

// StatsRefresher periodically updates the queue + storage gauges. Run in a
// goroutine from main.go; cancel its context to stop.
type StatsRefresher struct {
	store    storage.Store
	set      *MetricSet
	interval time.Duration
	timeout  time.Duration
	logger   *slog.Logger
	running  atomic.Bool
}

// DefaultStatsInterval is how often the refresher samples queue depth.
// Prometheus typically scrapes every 15-30s; sampling at 30s keeps the
// scan budget on Redis bounded.
const DefaultStatsInterval = 30 * time.Second

// DefaultStatsTimeout is the per-snapshot timeout. Larger than ReadTimeout so
// a slow scan doesn't fail the listener, but small enough that a hung Redis
// surfaces as StorageUp=0 within one scrape window.
const DefaultStatsTimeout = 10 * time.Second

// NewStatsRefresher returns a refresher that ticks at interval. Pass zero to
// use DefaultStatsInterval.
func NewStatsRefresher(store storage.Store, set *MetricSet, interval, timeout time.Duration, logger *slog.Logger) *StatsRefresher {
	if interval <= 0 {
		interval = DefaultStatsInterval
	}
	if timeout <= 0 {
		timeout = DefaultStatsTimeout
	}
	if logger == nil {
		logger = slog.Default()
	}
	return &StatsRefresher{store: store, set: set, interval: interval, timeout: timeout, logger: logger}
}

// Run blocks until ctx is cancelled, sampling queue depth + Ping every
// interval. The first sample fires immediately so /metrics has fresh data
// well before the first scrape.
func (s *StatsRefresher) Run(ctx context.Context) {
	if !s.running.CompareAndSwap(false, true) {
		return
	}
	defer s.running.Store(false)

	s.sampleOnce(ctx)
	t := time.NewTicker(s.interval)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			s.sampleOnce(ctx)
		}
	}
}

func (s *StatsRefresher) sampleOnce(parent context.Context) {
	ctx, cancel := context.WithTimeout(parent, s.timeout)
	defer cancel()

	if s.store == nil {
		s.set.StorageUp.Set(0)
		s.set.QueueDevices.Set(0)
		s.set.QueueEntries.Set(0)
		s.set.QueueDepthMax.Set(0)
		return
	}

	if err := s.store.Ping(ctx); err != nil {
		s.set.StorageUp.Set(0)
		// Don't trust queue gauges when storage is down — leave them at
		// their last value rather than zero them, so an alert dashboard
		// can see "storage down" without queue-depth alerts also firing.
		s.logger.Warn("storage ping failed", "err", err)
		return
	}
	s.set.StorageUp.Set(1)

	stats, err := s.store.Stats(ctx)
	if err != nil {
		// Ping succeeded but stats scan failed; possibly cancelled by
		// timeout. Mark storage degraded so ops sees something.
		if errors.Is(err, context.DeadlineExceeded) {
			s.logger.Warn("stats scan timed out", "timeout", s.timeout)
		} else {
			s.logger.Error("stats scan failed", "err", err)
		}
		return
	}
	s.set.QueueDevices.Set(int64(stats.Devices))
	s.set.QueueEntries.Set(int64(stats.Entries))
	s.set.QueueDepthMax.Set(int64(stats.MaxDepth))
}
