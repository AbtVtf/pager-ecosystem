package lora

import (
	"sort"
	"sync"
	"time"
)

// DefaultAdvertInterval is the default per-Exit-Node advert cadence
// (SPEC.md §6.2.4 / LORA-005).
const DefaultAdvertInterval = 30 * time.Second

// DefaultMaxAdvertAge is how long a known Exit Node entry is considered
// fresh; older entries are dropped from the ranking. Five intervals at
// 30 s = 150 s of tolerance for transient packet loss.
const DefaultMaxAdvertAge = 5 * DefaultAdvertInterval

// KnownNode is one ranked Exit Node entry in a Tracker.
type KnownNode struct {
	Pubkey   [32]byte
	BWClass  BWClass
	Load     uint8
	RSSI     int  // dBm, signed; less-negative = stronger
	LastSeen time.Time
}

// Tracker is the device-side ranked list of Exit Nodes seen via
// exit-node-advertise broadcasts (LORA-005 §4).
//
// Tracker is safe for concurrent use. The intended client lives in
// firmware/; this Go implementation is the reference for tests and
// future ports.
type Tracker struct {
	mu     sync.Mutex
	nodes  map[[32]byte]KnownNode
	maxAge time.Duration
	now    func() time.Time
}

// NewTracker returns an empty Tracker with default freshness window.
func NewTracker() *Tracker {
	return &Tracker{
		nodes:  make(map[[32]byte]KnownNode),
		maxAge: DefaultMaxAdvertAge,
		now:    time.Now,
	}
}

// SetMaxAge overrides the freshness window (entries older than this are
// excluded from Ranked / Best). Useful for tests.
func (t *Tracker) SetMaxAge(d time.Duration) {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.maxAge = d
}

// SetClock overrides the wall-clock source. Useful for tests.
func (t *Tracker) SetClock(now func() time.Time) {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.now = now
}

// Observe records or refreshes an advert from one Exit Node, with the
// RSSI reported by the radio at receive time.
func (t *Tracker) Observe(a Advert, rssi int) {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.nodes[a.Pubkey] = KnownNode{
		Pubkey:   a.Pubkey,
		BWClass:  a.BWClass,
		Load:     a.Load,
		RSSI:     rssi,
		LastSeen: t.now(),
	}
}

// Ranked returns a snapshot of fresh Exit Nodes ordered by §4 selection
// rules: lower load → stronger RSSI → higher bw_class.
func (t *Tracker) Ranked() []KnownNode {
	t.mu.Lock()
	defer t.mu.Unlock()
	cutoff := t.now().Add(-t.maxAge)
	out := make([]KnownNode, 0, len(t.nodes))
	for _, n := range t.nodes {
		if n.LastSeen.Before(cutoff) {
			continue
		}
		out = append(out, n)
	}
	sort.SliceStable(out, func(i, j int) bool {
		if out[i].Load != out[j].Load {
			return out[i].Load < out[j].Load
		}
		if out[i].RSSI != out[j].RSSI {
			return out[i].RSSI > out[j].RSSI
		}
		return out[i].BWClass > out[j].BWClass
	})
	return out
}

// Best returns the top-ranked fresh Exit Node and true, or a zero
// KnownNode and false if none are known.
func (t *Tracker) Best() (KnownNode, bool) {
	r := t.Ranked()
	if len(r) == 0 {
		return KnownNode{}, false
	}
	return r[0], true
}

// Prune drops entries older than maxAge. Called automatically by
// Ranked / Best — exposed for explicit cleanup from a maintenance loop.
func (t *Tracker) Prune() {
	t.mu.Lock()
	defer t.mu.Unlock()
	cutoff := t.now().Add(-t.maxAge)
	for k, n := range t.nodes {
		if n.LastSeen.Before(cutoff) {
			delete(t.nodes, k)
		}
	}
}

// Len returns the total number of tracked entries, including stale ones
// not yet pruned. Mostly useful for tests.
func (t *Tracker) Len() int {
	t.mu.Lock()
	defer t.mu.Unlock()
	return len(t.nodes)
}
