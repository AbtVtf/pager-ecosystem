// Package metrics is a tiny, dependency-free Prometheus exposition layer for
// the push relay (PUSH-009). It supports counters and gauges with low-
// cardinality string labels and emits the standard text/plain exposition
// format on /metrics.
//
// The deliberate non-goals (so reviewers don't expect them):
//
//   - Histograms / summaries. Per-request latency is logged by slog; ops
//     dashboards aggregate from there. If a future task wants real histograms,
//     swap this package for prometheus/client_golang.
//   - High-cardinality labels. We label on result enum + endpoint, never on
//     device pubkey or app id. That keeps the series count bounded.
//   - Pull / Push parity with the prometheus client. We only emit the small
//     subset of the exposition format actually needed for ops alerts
//     (#HELP, #TYPE, sample lines).
package metrics

import (
	"fmt"
	"io"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
)

// Registry owns a set of declared counters and gauges. Construction order
// in main.go fixes the metric universe; runtime calls only mutate series
// data, never add new metrics.
type Registry struct {
	mu       sync.Mutex
	counters []*Counter
	gauges   []*Gauge
}

// NewRegistry returns an empty registry.
func NewRegistry() *Registry {
	return &Registry{}
}

// NewCounter declares a counter on the registry. Help text is required.
func (r *Registry) NewCounter(name, help string) *Counter {
	c := &Counter{name: name, help: help, series: map[string]*counterSeries{}}
	r.mu.Lock()
	r.counters = append(r.counters, c)
	r.mu.Unlock()
	return c
}

// NewGauge declares a gauge on the registry.
func (r *Registry) NewGauge(name, help string) *Gauge {
	g := &Gauge{name: name, help: help, series: map[string]*gaugeSeries{}}
	r.mu.Lock()
	r.gauges = append(r.gauges, g)
	r.mu.Unlock()
	return g
}

// WriteExposition emits Prometheus text exposition for every declared metric.
// The output is stable across calls for the same data so tests can diff it.
//
// Named `WriteExposition` rather than `WriteTo` so we don't trip the
// io.WriterTo signature check (`WriteTo(io.Writer) (int64, error)`). The two
// have unrelated contracts; sharing the name is just confusing.
func (r *Registry) WriteExposition(w io.Writer) error {
	r.mu.Lock()
	counters := append([]*Counter(nil), r.counters...)
	gauges := append([]*Gauge(nil), r.gauges...)
	r.mu.Unlock()

	for _, c := range counters {
		if err := c.write(w); err != nil {
			return err
		}
	}
	for _, g := range gauges {
		if err := g.write(w); err != nil {
			return err
		}
	}
	return nil
}

// Counter is a monotonically increasing metric. Each (label-set) combo
// observed creates a stable series.
type Counter struct {
	name, help string

	mu     sync.Mutex
	series map[string]*counterSeries
}

type counterSeries struct {
	labelPairs [][2]string
	value      atomic.Uint64
}

// Inc adds 1 to the series identified by the label key/value pairs. The
// argument list is alternating: ("result", "accepted", "endpoint", "/push").
func (c *Counter) Inc(labelKVs ...string) {
	c.Add(1, labelKVs...)
}

// Add adds delta to the series.
func (c *Counter) Add(delta uint64, labelKVs ...string) {
	if c == nil {
		return
	}
	pairs := parsePairs(labelKVs)
	key := seriesKey(pairs)
	s := c.seriesFor(key, pairs)
	s.value.Add(delta)
}

func (c *Counter) seriesFor(key string, pairs [][2]string) *counterSeries {
	c.mu.Lock()
	defer c.mu.Unlock()
	if s, ok := c.series[key]; ok {
		return s
	}
	s := &counterSeries{labelPairs: pairs}
	c.series[key] = s
	return s
}

func (c *Counter) write(w io.Writer) error {
	if _, err := fmt.Fprintf(w, "# HELP %s %s\n# TYPE %s counter\n", c.name, escapeHelp(c.help), c.name); err != nil {
		return err
	}
	c.mu.Lock()
	keys := make([]string, 0, len(c.series))
	for k := range c.series {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		s := c.series[k]
		if _, err := fmt.Fprintf(w, "%s%s %d\n", c.name, formatLabels(s.labelPairs), s.value.Load()); err != nil {
			c.mu.Unlock()
			return err
		}
	}
	c.mu.Unlock()
	return nil
}

// Gauge is an arbitrary instantaneous value. Series semantics mirror Counter.
type Gauge struct {
	name, help string

	mu     sync.Mutex
	series map[string]*gaugeSeries
}

type gaugeSeries struct {
	labelPairs [][2]string
	value      atomic.Int64
}

// Set sets the gauge to v for the given label set. Negative values are
// supported (e.g. a derived skew metric); we use int64 + atomic for lock-free
// updates and don't need fractional precision for the metrics we emit.
func (g *Gauge) Set(v int64, labelKVs ...string) {
	if g == nil {
		return
	}
	pairs := parsePairs(labelKVs)
	key := seriesKey(pairs)
	g.mu.Lock()
	s, ok := g.series[key]
	if !ok {
		s = &gaugeSeries{labelPairs: pairs}
		g.series[key] = s
	}
	g.mu.Unlock()
	s.value.Store(v)
}

func (g *Gauge) write(w io.Writer) error {
	if _, err := fmt.Fprintf(w, "# HELP %s %s\n# TYPE %s gauge\n", g.name, escapeHelp(g.help), g.name); err != nil {
		return err
	}
	g.mu.Lock()
	keys := make([]string, 0, len(g.series))
	for k := range g.series {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		s := g.series[k]
		if _, err := fmt.Fprintf(w, "%s%s %d\n", g.name, formatLabels(s.labelPairs), s.value.Load()); err != nil {
			g.mu.Unlock()
			return err
		}
	}
	g.mu.Unlock()
	return nil
}

// parsePairs converts alternating ("k","v","k2","v2") args into [(k,v),(k2,v2)].
// Odd-length inputs drop the trailing key — callers are expected to pass
// well-formed pairs, but we don't want a programmer error to panic /metrics.
func parsePairs(args []string) [][2]string {
	if len(args) < 2 {
		return nil
	}
	out := make([][2]string, 0, len(args)/2)
	for i := 0; i+1 < len(args); i += 2 {
		out = append(out, [2]string{args[i], args[i+1]})
	}
	return out
}

// seriesKey hashes labels into a stable string. Sorting by label name keeps
// ("a=1","b=2") and ("b=2","a=1") in the same series.
func seriesKey(pairs [][2]string) string {
	if len(pairs) == 0 {
		return ""
	}
	cp := append([][2]string(nil), pairs...)
	sort.Slice(cp, func(i, j int) bool { return cp[i][0] < cp[j][0] })
	var b strings.Builder
	for i, p := range cp {
		if i > 0 {
			b.WriteByte(',')
		}
		b.WriteString(p[0])
		b.WriteByte('=')
		b.WriteString(p[1])
	}
	return b.String()
}

// formatLabels renders label pairs as `{k="v",k2="v2"}` for exposition. Output
// is sorted by key so multi-label series are deterministic in /metrics.
func formatLabels(pairs [][2]string) string {
	if len(pairs) == 0 {
		return ""
	}
	cp := append([][2]string(nil), pairs...)
	sort.Slice(cp, func(i, j int) bool { return cp[i][0] < cp[j][0] })
	var b strings.Builder
	b.WriteByte('{')
	for i, p := range cp {
		if i > 0 {
			b.WriteByte(',')
		}
		b.WriteString(p[0])
		b.WriteByte('=')
		b.WriteByte('"')
		b.WriteString(escapeLabel(p[1]))
		b.WriteByte('"')
	}
	b.WriteByte('}')
	return b.String()
}

// escapeLabel escapes backslashes, double quotes, and newlines per the
// Prometheus exposition format (label value subset).
func escapeLabel(s string) string {
	s = strings.ReplaceAll(s, `\`, `\\`)
	s = strings.ReplaceAll(s, `"`, `\"`)
	s = strings.ReplaceAll(s, "\n", `\n`)
	return s
}

// escapeHelp strips newlines from #HELP — the rest of the format is permissive.
func escapeHelp(s string) string {
	s = strings.ReplaceAll(s, `\`, `\\`)
	s = strings.ReplaceAll(s, "\n", `\n`)
	return s
}
