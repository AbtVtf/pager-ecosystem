// Package admin owns the relay's abuse-control surface: send-volume metrics
// and a ban list of app IDs whose pushes are rejected at /push (PUSH-008).
//
// The push handler is expected to:
//  1. Check Recorder.IsBanned(appID) BEFORE enqueueing; reject with 403 if
//     banned.
//  2. Call Recorder.RecordSend(appID, devicePubkey) AFTER a successful enqueue.
//
// The HTTP handlers in this package expose an authenticated read view of
// recorded volumes and CRUD for the ban list. Auth is a single shared bearer
// token (PUSH_RELAY_ADMIN_TOKEN) — the dashboard is for operators, not end
// users, so we deliberately keep the surface small (no per-user accounts).
//
// Storage is in-memory for now. Send counters reset on process restart; that
// matches the M3 acceptance scope ("authenticated view of per-app/per-device
// send volumes; ability to ban a sender pubkey"). Persisting them across
// restarts is left for PUSH-009 (deployment + monitoring) where the right
// venue is a metrics export, not the admin HTTP body.
package admin

import (
	"sort"
	"sync"
	"time"
)

// Recorder is the abstract interface the push handler uses for both the ban
// check and the volume counters. The Store type below is the canonical
// implementation; the interface exists so the push handler can be tested with
// a nil-safe stub.
type Recorder interface {
	IsBanned(appID string) bool
	RecordSend(appID, devicePubkey string)
}

// Store is the in-memory implementation of Recorder plus the read/write
// surface used by the admin HTTP handlers.
type Store struct {
	mu     sync.RWMutex
	now    func() time.Time
	app    map[string]*appCounter
	device map[string]*deviceCounter
	bans   map[string]Ban
}

type appCounter struct {
	total       uint64
	devices     map[string]uint64 // per-device count from this app
	firstSeenAt time.Time
	lastSeenAt  time.Time
}

type deviceCounter struct {
	total      uint64
	apps       map[string]uint64 // per-app count to this device
	lastSeenAt time.Time
}

// Ban is one entry in the ban list.
type Ban struct {
	AppID    string    `json:"app_id"`
	Reason   string    `json:"reason,omitempty"`
	BannedAt time.Time `json:"banned_at"`
}

// New returns a Store using time.Now for timestamps.
func New() *Store {
	return NewWithClock(time.Now)
}

// NewWithClock lets tests inject a deterministic clock.
func NewWithClock(now func() time.Time) *Store {
	if now == nil {
		now = time.Now
	}
	return &Store{
		now:    now,
		app:    make(map[string]*appCounter),
		device: make(map[string]*deviceCounter),
		bans:   make(map[string]Ban),
	}
}

// IsBanned reports whether appID is currently on the ban list.
func (s *Store) IsBanned(appID string) bool {
	if s == nil || appID == "" {
		return false
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	_, ok := s.bans[appID]
	return ok
}

// RecordSend increments per-app and per-device counters for a successful push.
// Empty appID or devicePubkey are silently ignored — the push handler already
// validates both before reaching this point, but we don't want a future caller
// to corrupt the maps with empty keys.
func (s *Store) RecordSend(appID, devicePubkey string) {
	if s == nil || appID == "" || devicePubkey == "" {
		return
	}
	now := s.now()
	s.mu.Lock()
	defer s.mu.Unlock()

	a, ok := s.app[appID]
	if !ok {
		a = &appCounter{devices: make(map[string]uint64), firstSeenAt: now}
		s.app[appID] = a
	}
	a.total++
	a.devices[devicePubkey]++
	a.lastSeenAt = now

	d, ok := s.device[devicePubkey]
	if !ok {
		d = &deviceCounter{apps: make(map[string]uint64)}
		s.device[devicePubkey] = d
	}
	d.total++
	d.apps[appID]++
	d.lastSeenAt = now
}

// AppStat is the per-app row exposed by the admin dashboard.
type AppStat struct {
	AppID       string    `json:"app_id"`
	Total       uint64    `json:"total"`
	UniqueDevs  int       `json:"unique_devices"`
	FirstSeenAt time.Time `json:"first_seen_at"`
	LastSeenAt  time.Time `json:"last_seen_at"`
	Banned      bool      `json:"banned"`
}

// DeviceStat is the per-device row exposed by the admin dashboard.
type DeviceStat struct {
	DevicePubkey string    `json:"device_pubkey"`
	Total        uint64    `json:"total"`
	UniqueApps   int       `json:"unique_apps"`
	LastSeenAt   time.Time `json:"last_seen_at"`
}

// Metrics is the aggregate snapshot returned by /admin/metrics.
type Metrics struct {
	Apps    []AppStat    `json:"apps"`
	Devices []DeviceStat `json:"devices"`
}

// Snapshot returns a defensive copy of the current send volumes, sorted by
// descending total. Holds the read lock for the full traversal; the maps are
// bounded by the active app/device universe so allocation is cheap.
func (s *Store) Snapshot() Metrics {
	if s == nil {
		return Metrics{}
	}
	s.mu.RLock()
	defer s.mu.RUnlock()

	apps := make([]AppStat, 0, len(s.app))
	for id, c := range s.app {
		_, banned := s.bans[id]
		apps = append(apps, AppStat{
			AppID:       id,
			Total:       c.total,
			UniqueDevs:  len(c.devices),
			FirstSeenAt: c.firstSeenAt,
			LastSeenAt:  c.lastSeenAt,
			Banned:      banned,
		})
	}
	sort.Slice(apps, func(i, j int) bool {
		if apps[i].Total != apps[j].Total {
			return apps[i].Total > apps[j].Total
		}
		return apps[i].AppID < apps[j].AppID
	})

	devices := make([]DeviceStat, 0, len(s.device))
	for pk, c := range s.device {
		devices = append(devices, DeviceStat{
			DevicePubkey: pk,
			Total:        c.total,
			UniqueApps:   len(c.apps),
			LastSeenAt:   c.lastSeenAt,
		})
	}
	sort.Slice(devices, func(i, j int) bool {
		if devices[i].Total != devices[j].Total {
			return devices[i].Total > devices[j].Total
		}
		return devices[i].DevicePubkey < devices[j].DevicePubkey
	})

	return Metrics{Apps: apps, Devices: devices}
}

// Ban adds appID to the ban list. Idempotent — banning an already-banned id
// updates the reason and timestamp.
func (s *Store) Ban(appID, reason string) Ban {
	now := s.now()
	s.mu.Lock()
	defer s.mu.Unlock()
	b := Ban{AppID: appID, Reason: reason, BannedAt: now}
	s.bans[appID] = b
	return b
}

// Unban removes appID from the ban list. Returns true if a ban existed.
func (s *Store) Unban(appID string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok := s.bans[appID]; !ok {
		return false
	}
	delete(s.bans, appID)
	return true
}

// ListBans returns the current ban list sorted by app id.
func (s *Store) ListBans() []Ban {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]Ban, 0, len(s.bans))
	for _, b := range s.bans {
		out = append(out, b)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].AppID < out[j].AppID })
	return out
}
