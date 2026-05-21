// Package ratelimit caps inbound LoRa requests per device pubkey
// (EXIT-004, SPEC.md §11.2).
//
// The limiter is a simple per-key token bucket. Each bucket has the
// configured capacity (default 60) and refills at capacity tokens per
// minute. A new key is constructed full. Over-budget callers get a
// non-nil Decision with Allow=false and RetryAfter populated.
//
// Buckets are pruned when idle for more than a configurable TTL so the
// map does not grow unbounded under a churning population of devices.
package ratelimit

import (
	"sync"
	"time"
)

// DefaultIdleTTL is how long an idle bucket lingers before being
// dropped by Sweep. Five minutes is long enough to retain warm callers
// across short network pauses but short enough that a one-shot
// passer-by does not pin memory.
const DefaultIdleTTL = 5 * time.Minute

// Decision is the result of a single Allow check.
type Decision struct {
	Allowed    bool
	Limit      int           // bucket capacity (req/min)
	Remaining  int           // tokens left after this call (0 when not Allowed)
	RetryAfter time.Duration // when over-limit: time until the next token; zero otherwise
}

// Limiter is a key→token-bucket map safe for concurrent use.
//
// A zero Limiter is not usable; construct with New.
type Limiter struct {
	mu      sync.Mutex
	buckets map[string]*bucket
	cap     int           // tokens per bucket = req/min cap
	refill  time.Duration // wall time per token
	idleTTL time.Duration
	now     func() time.Time
}

type bucket struct {
	tokens   float64
	lastFill time.Time
}

// New returns a Limiter that allows perMin requests per key per
// minute. perMin <= 0 disables limiting (Allow always returns
// Decision{Allowed:true, Limit:0}).
func New(perMin int) *Limiter {
	return NewWithClock(perMin, time.Now)
}

// NewWithClock is like New but lets callers inject a clock for tests.
func NewWithClock(perMin int, now func() time.Time) *Limiter {
	if now == nil {
		now = time.Now
	}
	l := &Limiter{
		buckets: make(map[string]*bucket),
		cap:     perMin,
		idleTTL: DefaultIdleTTL,
		now:     now,
	}
	if perMin > 0 {
		l.refill = time.Minute / time.Duration(perMin)
	}
	return l
}

// SetIdleTTL overrides DefaultIdleTTL. Buckets idle longer than this
// are removed by Sweep.
func (l *Limiter) SetIdleTTL(d time.Duration) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if d > 0 {
		l.idleTTL = d
	}
}

// Allow consumes one token for key and reports whether the call is
// within the limit. A zero-cap limiter (perMin <= 0) always allows.
func (l *Limiter) Allow(key string) Decision {
	if l.cap <= 0 {
		return Decision{Allowed: true}
	}
	l.mu.Lock()
	defer l.mu.Unlock()

	now := l.now()
	b, ok := l.buckets[key]
	if !ok {
		// New caller starts with cap-1 (we are consuming the first
		// token right now).
		l.buckets[key] = &bucket{tokens: float64(l.cap) - 1, lastFill: now}
		return Decision{
			Allowed:   true,
			Limit:     l.cap,
			Remaining: l.cap - 1,
		}
	}

	// Continuous refill: add (elapsed / refill) tokens, capped at cap.
	if now.After(b.lastFill) {
		add := float64(now.Sub(b.lastFill)) / float64(l.refill)
		b.tokens += add
		if b.tokens > float64(l.cap) {
			b.tokens = float64(l.cap)
		}
		b.lastFill = now
	}

	if b.tokens >= 1 {
		b.tokens -= 1
		return Decision{
			Allowed:   true,
			Limit:     l.cap,
			Remaining: int(b.tokens),
		}
	}

	// Over the cap. Compute time until the next whole token.
	need := 1 - b.tokens
	retry := time.Duration(need * float64(l.refill))
	if retry < time.Second {
		retry = time.Second // device-friendly minimum (§7.4 Retry-After units)
	}
	return Decision{
		Allowed:    false,
		Limit:      l.cap,
		Remaining:  0,
		RetryAfter: retry,
	}
}

// Sweep removes buckets last-touched longer than the idle TTL ago.
// Operators wire this to a periodic ticker. Safe to call concurrently
// with Allow.
func (l *Limiter) Sweep() int {
	l.mu.Lock()
	defer l.mu.Unlock()
	cutoff := l.now().Add(-l.idleTTL)
	removed := 0
	for k, b := range l.buckets {
		if b.lastFill.Before(cutoff) {
			delete(l.buckets, k)
			removed++
		}
	}
	return removed
}

// Len returns the number of tracked keys. Useful for metrics + tests.
func (l *Limiter) Len() int {
	l.mu.Lock()
	defer l.mu.Unlock()
	return len(l.buckets)
}

// Cap returns the configured per-minute cap (0 when disabled).
func (l *Limiter) Cap() int {
	return l.cap
}
