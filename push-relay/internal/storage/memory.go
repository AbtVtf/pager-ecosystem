package storage

import (
	"context"
	"encoding/json"
	"sort"
	"sync"
	"time"
)

// memoryStore is an in-memory Store implementation. It is the canonical
// reference for the queue cap / TTL eviction semantics — the Redis backend
// is expected to produce identical observable behaviour. Suitable for unit
// tests and dev usage; not for production (no persistence).
type memoryStore struct {
	mu     sync.Mutex
	queues map[string][]Notification
	now    func() time.Time
}

// NewMemory returns an in-memory Store using time.Now for timestamps.
func NewMemory() Store {
	return newMemoryWithClock(time.Now)
}

// newMemoryWithClock is exposed package-internally so tests can drive
// deterministic time without modifying the public API.
func newMemoryWithClock(now func() time.Time) *memoryStore {
	if now == nil {
		now = time.Now
	}
	return &memoryStore{
		queues: make(map[string][]Notification),
		now:    now,
	}
}

func (m *memoryStore) Ping(ctx context.Context) error {
	return ctx.Err()
}

func (m *memoryStore) Close() error { return nil }

func (m *memoryStore) Enqueue(ctx context.Context, devicePubkey string, n Notification) (Notification, error) {
	if err := ctx.Err(); err != nil {
		return Notification{}, err
	}
	now := m.now()
	if err := validateForEnqueue(devicePubkey, &n, now); err != nil {
		return Notification{}, err
	}

	encoded, err := json.Marshal(n)
	if err != nil {
		return Notification{}, err
	}
	if len(encoded) > MaxQueueBytes {
		return Notification{}, ErrPayloadTooLarge
	}

	m.mu.Lock()
	defer m.mu.Unlock()

	q := m.pruneExpiredLocked(devicePubkey, now)
	q = append(q, n)

	// Cap by count: drop oldest first.
	if len(q) > MaxQueueLen {
		q = q[len(q)-MaxQueueLen:]
	}
	// Cap by total bytes: drop oldest until total <= MaxQueueBytes. The
	// just-inserted entry is guaranteed to fit on its own (checked above), so
	// this loop always terminates with at least n remaining.
	for totalBytes(q) > MaxQueueBytes && len(q) > 1 {
		q = q[1:]
	}

	m.queues[devicePubkey] = q
	return n, nil
}

func (m *memoryStore) List(ctx context.Context, devicePubkey string) ([]Notification, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	if devicePubkey == "" {
		return nil, ErrInvalidDevice
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	q := m.pruneExpiredLocked(devicePubkey, m.now())
	out := make([]Notification, len(q))
	copy(out, q)
	return out, nil
}

func (m *memoryStore) Delete(ctx context.Context, devicePubkey, id string) (bool, error) {
	if err := ctx.Err(); err != nil {
		return false, err
	}
	if devicePubkey == "" {
		return false, ErrInvalidDevice
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	q := m.queues[devicePubkey]
	for i, entry := range q {
		if entry.ID == id {
			m.queues[devicePubkey] = append(q[:i], q[i+1:]...)
			return true, nil
		}
	}
	return false, nil
}

// pruneExpiredLocked drops entries older than QueueTTL from the device's queue
// and rewrites the slice. Caller must hold m.mu. Returns the pruned slice
// (also stored back into m.queues).
func (m *memoryStore) pruneExpiredLocked(devicePubkey string, now time.Time) []Notification {
	q := m.queues[devicePubkey]
	if len(q) == 0 {
		return q
	}
	cutoff := now.Add(-QueueTTL)
	// Queue is kept in insert order; sort by EnqueuedAt as a defensive measure
	// in case external callers insert with backdated timestamps.
	sort.SliceStable(q, func(i, j int) bool {
		return q[i].EnqueuedAt.Before(q[j].EnqueuedAt)
	})
	kept := q[:0]
	for _, entry := range q {
		if entry.EnqueuedAt.After(cutoff) {
			kept = append(kept, entry)
		}
	}
	m.queues[devicePubkey] = kept
	return kept
}

// totalBytes returns the JSON-encoded byte size of the entire queue. Errors
// are treated as zero bytes — a JSON failure here would mean the in-memory
// data is itself corrupt, which is a bigger problem than a slightly-off cap.
func totalBytes(q []Notification) int {
	total := 0
	for _, entry := range q {
		b, err := json.Marshal(entry)
		if err != nil {
			continue
		}
		total += len(b)
	}
	return total
}
