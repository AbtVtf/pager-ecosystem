// Package ratelimit enforces the per-(app, device) push quotas defined in
// SPEC §6.6.4: 60 notifications/hour and 1,000/day. The push handler calls
// Allow(appID, devicePubkey) after signature verification; on overflow it
// returns 429 with a Retry-After value.
//
// Implementation notes:
//
//   - Sliding window. We keep an ordered slice of recent send timestamps per
//     (app, device) tuple. On every check we drop stamps older than the day
//     window, then count entries inside the hour and day windows. Sliding
//     beats fixed-window because a malicious sender cannot dump 2x the limit
//     across a window boundary.
//   - Memory cost. The day cap is 1,000 timestamps × ~24 B ≈ 24 KiB per
//     active tuple. We GC idle tuples whenever their slice empties out
//     during pruning, so the universe is bounded by the active sender×device
//     fan-out within any 24h window. That is acceptable for v1; PUSH-009 can
//     migrate this to Redis if the relay scales horizontally.
//   - Group events. PUSH-007 will need a separate bucket per SPEC §6.6.6;
//     this package exposes a `Bucket` constant so a second limiter can be
//     instantiated with its own state without conflating counts.
package ratelimit

import (
	"sync"
	"time"
)

// Limits are the SPEC §6.6.4 thresholds. Exposed so tests and callers can
// reference them without re-declaring magic numbers.
const (
	HourLimit = 60
	DayLimit  = 1000
	HourSpan  = time.Hour
	DaySpan   = 24 * time.Hour
)

// Limiter is the public surface the push handler uses. Implementations MUST
// be safe for concurrent use.
type Limiter interface {
	// Allow returns true if a send for (appID, devicePubkey) fits within the
	// hour and day windows at `at`. When allowed, it records the send and
	// returns retryAfter == 0. When denied, it returns the smallest duration
	// after which a retry would fit (in seconds granularity — never zero).
	Allow(appID, devicePubkey string, at time.Time) (allowed bool, retryAfter time.Duration)
}

// MemoryLimiter is the in-memory sliding-window implementation.
type MemoryLimiter struct {
	mu      sync.Mutex
	buckets map[string][]time.Time
	hourMax int
	dayMax  int
}

// NewMemory returns a limiter using the SPEC §6.6.4 limits.
func NewMemory() *MemoryLimiter {
	return NewMemoryWithLimits(HourLimit, DayLimit)
}

// NewMemoryWithLimits is useful for tests and for the group-event bucket
// (PUSH-007) once that lands with its own thresholds.
func NewMemoryWithLimits(hourMax, dayMax int) *MemoryLimiter {
	if hourMax <= 0 || dayMax <= 0 {
		panic("ratelimit: limits must be positive")
	}
	return &MemoryLimiter{
		buckets: make(map[string][]time.Time),
		hourMax: hourMax,
		dayMax:  dayMax,
	}
}

func bucketKey(appID, devicePubkey string) string {
	// `\x00` is never valid in either an app id (reverse-DNS, lowercase) or a
	// base64url device key, so it is a safe separator.
	return appID + "\x00" + devicePubkey
}

// Allow implements Limiter.
func (l *MemoryLimiter) Allow(appID, devicePubkey string, at time.Time) (bool, time.Duration) {
	if appID == "" || devicePubkey == "" {
		// Empty inputs would all share a bucket — refuse rather than corrupt
		// state. The push handler validates these before calling us so this
		// is purely a defensive guard.
		return false, time.Second
	}
	key := bucketKey(appID, devicePubkey)
	dayCutoff := at.Add(-DaySpan)
	hourCutoff := at.Add(-HourSpan)

	l.mu.Lock()
	defer l.mu.Unlock()

	stamps := pruneOlderThan(l.buckets[key], dayCutoff)

	hourCount := 0
	for i := len(stamps) - 1; i >= 0; i-- {
		if stamps[i].Before(hourCutoff) {
			break
		}
		hourCount++
	}
	dayCount := len(stamps)

	if hourCount >= l.hourMax {
		// First send in the hour window will expire at stamps[len-hourMax] +
		// HourSpan. Note: hourCount >= hourMax so stamps has at least hourMax
		// recent entries.
		expiringAt := stamps[len(stamps)-l.hourMax].Add(HourSpan)
		retry := expiringAt.Sub(at)
		l.buckets[key] = stamps
		return false, ceilSeconds(retry)
	}
	if dayCount >= l.dayMax {
		// Oldest entry in the day window expires at stamps[len-dayMax]+DaySpan.
		expiringAt := stamps[len(stamps)-l.dayMax].Add(DaySpan)
		retry := expiringAt.Sub(at)
		l.buckets[key] = stamps
		return false, ceilSeconds(retry)
	}

	stamps = append(stamps, at)
	l.buckets[key] = stamps
	return true, 0
}

// Snapshot returns the current (hour, day) counts for a bucket at `at`. Used
// by tests; not exposed via the Limiter interface to keep the contract small.
func (l *MemoryLimiter) Snapshot(appID, devicePubkey string, at time.Time) (int, int) {
	key := bucketKey(appID, devicePubkey)
	l.mu.Lock()
	defer l.mu.Unlock()
	stamps := pruneOlderThan(l.buckets[key], at.Add(-DaySpan))
	l.buckets[key] = stamps
	hour := 0
	hourCutoff := at.Add(-HourSpan)
	for i := len(stamps) - 1; i >= 0; i-- {
		if stamps[i].Before(hourCutoff) {
			break
		}
		hour++
	}
	return hour, len(stamps)
}

// pruneOlderThan returns the suffix of stamps with timestamps >= cutoff.
// Caller must hold the limiter mutex when passing a slice owned by the map.
// If the resulting slice is empty, returns nil so the caller can drop the
// map entry by writing the nil back.
func pruneOlderThan(stamps []time.Time, cutoff time.Time) []time.Time {
	if len(stamps) == 0 {
		return nil
	}
	// Binary search would be slightly faster but slices are bounded by
	// DayLimit (1,000); a linear scan is fine and keeps the code obviously
	// correct.
	cut := 0
	for cut < len(stamps) && stamps[cut].Before(cutoff) {
		cut++
	}
	if cut == 0 {
		return stamps
	}
	if cut == len(stamps) {
		return nil
	}
	// Re-slice in place; safe because we hold the lock and we won't reuse the
	// dropped prefix elsewhere.
	return stamps[cut:]
}

// ceilSeconds rounds a duration up to the next whole second and clamps to a
// positive minimum. The Retry-After HTTP header is seconds-granularity, and
// we never want to return 0 (which would mean "retry now") to a client whose
// request we just rejected.
func ceilSeconds(d time.Duration) time.Duration {
	if d <= 0 {
		return time.Second
	}
	secs := d / time.Second
	if d%time.Second != 0 {
		secs++
	}
	if secs < 1 {
		secs = 1
	}
	return secs * time.Second
}
