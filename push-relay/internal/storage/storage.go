// Package storage owns the push-relay queue backend.
//
// PUSH-001 landed the connection plumbing for a Redis backend; PUSH-005 adds
// the per-device queue semantics required by SPEC §6.6:
//
//   - Each device has its own queue of opaque encrypted envelopes.
//   - Queues are capped at MaxQueueLen entries.
//   - Each entry has a QueueTTL lifetime; expired entries are evicted lazily on
//     enqueue and list.
//   - The total stored byte size per device is capped at MaxQueueBytes; when
//     exceeded the oldest entries are dropped first.
//
// The package exposes a single Store interface implemented by an in-memory
// backend (used for tests and dev) and a Redis backend (used in production).
package storage

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"net/url"
	"time"
)

// Queue cap constants (SPEC §6.6.1, §6.6.4).
const (
	MaxQueueLen   = 16
	MaxQueueBytes = 1 << 20 // 1 MiB
	QueueTTL      = 7 * 24 * time.Hour
)

// Notification kinds carried over the relay.
const (
	KindNotification = "notification"
	KindGroupEvent   = "group_event"
)

// Notification is a single envelope stored on the relay. Payload is opaque,
// already encrypted end-to-end from the sending app to the destination device
// (SPEC §6.6.3); the relay never decrypts it.
type Notification struct {
	ID         string    `json:"id"`
	Kind       string    `json:"kind"`
	AppID      string    `json:"app_id,omitempty"`
	Payload    []byte    `json:"payload"`
	EnqueuedAt time.Time `json:"enqueued_at"`
}

// Errors returned by Store implementations.
var (
	ErrInvalidDevice   = errors.New("storage: device pubkey is empty")
	ErrInvalidKind     = errors.New("storage: unknown notification kind")
	ErrPayloadTooLarge = errors.New("storage: payload exceeds per-device byte cap")
)

// Store is the abstract queue backend. Implementations MUST enforce the queue
// caps and TTL described in the package doc and SPEC §6.6.
type Store interface {
	// Ping verifies the backend is reachable. Used by /healthz.
	Ping(ctx context.Context) error
	// Close releases any underlying resources (connections, goroutines).
	Close() error

	// Enqueue appends n to the queue for devicePubkey. The implementation:
	//   - Assigns n.ID if empty.
	//   - Sets n.EnqueuedAt to now if zero.
	//   - Evicts expired entries (>QueueTTL old).
	//   - After insert, evicts the oldest entries until the queue contains
	//     at most MaxQueueLen entries AND at most MaxQueueBytes stored bytes.
	// Returns the stored Notification (with server-assigned fields filled in)
	// or ErrPayloadTooLarge if n alone exceeds MaxQueueBytes.
	Enqueue(ctx context.Context, devicePubkey string, n Notification) (Notification, error)

	// List returns all currently-stored notifications for devicePubkey, in
	// oldest-first order. Expired entries are pruned as a side effect.
	List(ctx context.Context, devicePubkey string) ([]Notification, error)

	// Delete removes the notification with the given id from the device's
	// queue. Returns true if the entry existed and was removed.
	Delete(ctx context.Context, devicePubkey, id string) (bool, error)
}

// newNotificationID returns a 128-bit random hex id. The relay assigns this on
// Enqueue when the caller does not supply one (SPEC §6.6.5: "Each notification
// has a server-assigned id; device dedupes on read.").
func newNotificationID() string {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		// crypto/rand failure is a process-level invariant violation.
		panic(fmt.Sprintf("storage: crypto/rand failure: %v", err))
	}
	return hex.EncodeToString(b[:])
}

// validateForEnqueue normalises and validates a Notification just before it is
// written to the backend. Mutates n's ID/EnqueuedAt fields if unset.
func validateForEnqueue(devicePubkey string, n *Notification, now time.Time) error {
	if devicePubkey == "" {
		return ErrInvalidDevice
	}
	if n.Kind == "" {
		n.Kind = KindNotification
	}
	if n.Kind != KindNotification && n.Kind != KindGroupEvent {
		return ErrInvalidKind
	}
	if n.ID == "" {
		n.ID = newNotificationID()
	}
	if n.EnqueuedAt.IsZero() {
		n.EnqueuedAt = now
	}
	return nil
}

// ParseRedisURL is a thin re-export used by main.go for safe URL logging.
func ParseRedisURL(rawURL string) (*url.URL, error) {
	return url.Parse(rawURL)
}
