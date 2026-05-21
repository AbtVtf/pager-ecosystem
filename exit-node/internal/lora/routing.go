// Flood routing with TTL hops (LORA-004 / SPEC §6.2.5).
//
// PagerOS v1 uses Meshtastic-style flood routing: each packet carries a
// TTL hop counter, every node that receives a packet for the first time
// (by MsgID) decrements TTL and re-broadcasts it if TTL > 0. Duplicate
// packets (same MsgID) are dropped — that's the loop-detection
// mechanism: a packet can take many paths through the mesh but each
// node only forwards it once.
//
// This module is wire-agnostic on purpose. The Router takes any
// (MsgID, TTL, Payload) tuple and decides:
//
//   - drop (already seen, or TTL exhausted, or it's our own original),
//   - accept locally (it's addressed to / destined for this node), and
//   - forward (re-broadcast with decremented TTL).
//
// Wiring this into the on-wire format — i.e. where TTL lives in the
// outer envelope — is a follow-up that bumps the wire version. The
// router itself is the load-bearing piece this task delivers; the
// 3-node test demonstrates end-to-end propagation + loop detection.

package lora

import (
	"sync"
	"time"
)

// DefaultTTL is the v1 hop budget per SPEC §6.2.5.
const DefaultTTL uint8 = 3

// Routable is anything the Router can process. Fields mirror what the
// outer envelope already carries (MsgID is on the wire today) plus the
// future-proofing TTL byte.
type Routable struct {
	MsgID   uint32
	TTL     uint8
	Payload []byte
}

// Action is the Router's verdict on a received packet.
type Action int

const (
	// Drop the packet — do nothing.
	ActionDrop Action = iota
	// AcceptOnly: deliver locally; do NOT forward (e.g. TTL hit 0 here).
	ActionAcceptOnly
	// ForwardOnly: re-broadcast with decremented TTL; do NOT deliver
	// locally (the packet is in transit, not for us).
	ActionForwardOnly
	// AcceptAndForward: deliver locally AND re-broadcast. Per spec,
	// flood routing delivers to every reachable node, so any node that
	// might be an addressee should both consume and forward.
	ActionAcceptAndForward
)

// Decision pairs an Action with the rewritten Routable that callers
// should re-transmit (only meaningful for the Forward actions).
type Decision struct {
	Action  Action
	Forward Routable // valid only when Action is *Forward*
}

// RouterOptions configures a Router.
type RouterOptions struct {
	// SeenWindow controls how long a MsgID is remembered as
	// already-forwarded. Once a MsgID falls out of the window, the
	// router treats it as new (which is fine in practice: TTL bounds
	// the diameter so a stale loop can't restart). Defaults to 60s.
	SeenWindow time.Duration

	// MaxSeen caps the seen-MsgID table size. Defaults to 4096.
	MaxSeen int

	// Now returns the current time. Inject in tests for determinism.
	Now func() time.Time
}

// Router is the per-node flood-routing engine. Safe for concurrent use.
//
// A node creates one Router and calls Process for every received
// packet. The decision tells the node what to do.
type Router struct {
	mu         sync.Mutex
	seenWindow time.Duration
	maxSeen    int
	now        func() time.Time
	seen       map[uint32]time.Time
}

// NewRouter constructs a Router with the given options.
func NewRouter(opts RouterOptions) *Router {
	if opts.SeenWindow <= 0 {
		opts.SeenWindow = 60 * time.Second
	}
	if opts.MaxSeen <= 0 {
		opts.MaxSeen = 4096
	}
	if opts.Now == nil {
		opts.Now = time.Now
	}
	return &Router{
		seenWindow: opts.SeenWindow,
		maxSeen:    opts.MaxSeen,
		now:        opts.Now,
		seen:       make(map[uint32]time.Time, opts.MaxSeen),
	}
}

// Process consumes one received packet and returns a Decision.
//
// Semantics (SPEC §6.2.5):
//
//   - If we've already seen this MsgID within SeenWindow, drop. This is
//     the loop-detection rule.
//   - Otherwise record the MsgID.
//   - If TTL == 0 (or 1 after decrement), accept locally only — the
//     packet has exhausted its hop budget and stops here.
//   - If TTL > 1 after decrement, accept locally AND forward with the
//     decremented TTL. Flood routing means every reachable node sees
//     the packet, so we both deliver and re-broadcast.
func (r *Router) Process(pkt Routable) Decision {
	r.mu.Lock()
	defer r.mu.Unlock()

	now := r.now()
	r.gcLocked(now)

	if seenAt, ok := r.seen[pkt.MsgID]; ok && now.Sub(seenAt) < r.seenWindow {
		return Decision{Action: ActionDrop}
	}
	r.seen[pkt.MsgID] = now
	r.evictIfFullLocked()

	if pkt.TTL == 0 {
		// Packet arrived with no remaining hops; deliver locally only.
		return Decision{Action: ActionAcceptOnly}
	}

	decremented := pkt
	decremented.TTL = pkt.TTL - 1
	if decremented.TTL == 0 {
		// We are the last hop. Deliver locally, no rebroadcast.
		return Decision{Action: ActionAcceptOnly}
	}
	return Decision{
		Action:  ActionAcceptAndForward,
		Forward: decremented,
	}
}

// Saw lets a node pre-seed its own originated MsgID so the local copy
// is dropped if it loops back. Originators should call this before
// transmitting.
func (r *Router) Saw(msgID uint32) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.seen[msgID] = r.now()
	r.evictIfFullLocked()
}

// SeenCount is the current number of remembered MsgIDs. Exposed for
// tests + the stats surface.
func (r *Router) SeenCount() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return len(r.seen)
}

func (r *Router) gcLocked(now time.Time) {
	if len(r.seen) == 0 {
		return
	}
	// Walk + delete expired. Cheap because we cap MaxSeen.
	for k, t := range r.seen {
		if now.Sub(t) >= r.seenWindow {
			delete(r.seen, k)
		}
	}
}

func (r *Router) evictIfFullLocked() {
	if len(r.seen) <= r.maxSeen {
		return
	}
	// Simple eviction: drop the entry with the oldest timestamp. We
	// only run this on overflow so the O(n) scan is acceptable.
	var oldestK uint32
	var oldestT time.Time
	first := true
	for k, t := range r.seen {
		if first || t.Before(oldestT) {
			oldestK, oldestT = k, t
			first = false
		}
	}
	delete(r.seen, oldestK)
}
