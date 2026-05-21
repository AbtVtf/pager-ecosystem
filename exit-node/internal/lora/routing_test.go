package lora

import (
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestRouter_FirstSeenForwardsWithDecrementedTTL(t *testing.T) {
	r := NewRouter(RouterOptions{})
	d := r.Process(Routable{MsgID: 100, TTL: 3, Payload: []byte("x")})
	if d.Action != ActionAcceptAndForward {
		t.Fatalf("action: %v", d.Action)
	}
	if d.Forward.TTL != 2 {
		t.Errorf("TTL not decremented: %d", d.Forward.TTL)
	}
	if d.Forward.MsgID != 100 {
		t.Errorf("MsgID changed: %d", d.Forward.MsgID)
	}
	if string(d.Forward.Payload) != "x" {
		t.Errorf("Payload mutated: %q", d.Forward.Payload)
	}
}

func TestRouter_DuplicateIsDropped(t *testing.T) {
	r := NewRouter(RouterOptions{})
	r.Process(Routable{MsgID: 5, TTL: 3})
	d2 := r.Process(Routable{MsgID: 5, TTL: 3})
	if d2.Action != ActionDrop {
		t.Errorf("duplicate should drop, got %v", d2.Action)
	}
}

func TestRouter_TTLOneAcceptsButDoesNotForward(t *testing.T) {
	r := NewRouter(RouterOptions{})
	d := r.Process(Routable{MsgID: 7, TTL: 1})
	if d.Action != ActionAcceptOnly {
		t.Errorf("TTL=1 should accept-only, got %v", d.Action)
	}
}

func TestRouter_TTLZeroAcceptsOnly(t *testing.T) {
	r := NewRouter(RouterOptions{})
	d := r.Process(Routable{MsgID: 9, TTL: 0})
	if d.Action != ActionAcceptOnly {
		t.Errorf("TTL=0 should accept-only, got %v", d.Action)
	}
}

func TestRouter_OriginatorSeesOwnLoopbackDropped(t *testing.T) {
	r := NewRouter(RouterOptions{})
	r.Saw(42)
	d := r.Process(Routable{MsgID: 42, TTL: 3})
	if d.Action != ActionDrop {
		t.Errorf("originator should drop own loopback, got %v", d.Action)
	}
}

func TestRouter_GCExpiresOldEntries(t *testing.T) {
	t0 := time.Unix(1_000_000, 0)
	now := atomic.Int64{}
	now.Store(t0.Unix())
	r := NewRouter(RouterOptions{
		SeenWindow: 10 * time.Second,
		Now:        func() time.Time { return time.Unix(now.Load(), 0) },
	})
	r.Process(Routable{MsgID: 1, TTL: 2})
	if r.SeenCount() != 1 {
		t.Errorf("seen count: %d", r.SeenCount())
	}
	// Advance past the window.
	now.Add(11)
	r.Process(Routable{MsgID: 2, TTL: 2}) // triggers GC
	if r.SeenCount() != 1 {
		t.Errorf("old entry should be GC'd; count=%d", r.SeenCount())
	}
}

func TestRouter_ConcurrentProcess(t *testing.T) {
	r := NewRouter(RouterOptions{})
	var wg sync.WaitGroup
	for i := 0; i < 100; i++ {
		wg.Add(1)
		go func(id uint32) {
			defer wg.Done()
			r.Process(Routable{MsgID: id, TTL: 3})
		}(uint32(i))
	}
	wg.Wait()
	if r.SeenCount() != 100 {
		t.Errorf("expected 100 unique entries, got %d", r.SeenCount())
	}
}

// --- 3-node mesh integration test (LORA-004 acceptance) -------------- //

// node simulates a single mesh participant: it has a Router (for
// flood-routing decisions) and a slice tracking which payloads it
// "delivered" locally during the run.
type node struct {
	name      string
	router    *Router
	delivered [][]byte
}

func newNode(name string) *node {
	return &node{name: name, router: NewRouter(RouterOptions{})}
}

// receive is what the radio driver would call on incoming traffic.
// Returns the rebroadcast packet (if any) and whether the node
// delivered locally.
func (n *node) receive(pkt Routable) (rebroadcast *Routable, delivered bool) {
	d := n.router.Process(pkt)
	switch d.Action {
	case ActionDrop:
		return nil, false
	case ActionAcceptOnly:
		n.delivered = append(n.delivered, pkt.Payload)
		return nil, true
	case ActionForwardOnly:
		return &d.Forward, false
	case ActionAcceptAndForward:
		n.delivered = append(n.delivered, pkt.Payload)
		return &d.Forward, true
	}
	return nil, false
}

// mesh is a simple adjacency model: each node knows its neighbours.
type mesh struct {
	neighbours map[string][]*node
}

// broadcast simulates the originator (`from`) putting a packet on the
// air; every neighbour receives it. The simulation walks the BFS until
// the queue empties — that's the entire flood propagation, and any
// loops are caught by each node's Router.
func (m *mesh) broadcast(from *node, pkt Routable) {
	from.router.Saw(pkt.MsgID) // originator drops its own loopback
	type send struct {
		from *node
		pkt  Routable
	}
	queue := []send{}
	for _, n := range m.neighbours[from.name] {
		queue = append(queue, send{from: from, pkt: pkt})
		_ = n
	}
	// Initial broadcast: every neighbour of `from`.
	for _, n := range m.neighbours[from.name] {
		queue = append(queue, send{from: from, pkt: pkt})
		_ = n
	}
	// Restart: the above attempt was wrong. Rebuild properly.
	queue = queue[:0]
	for _, n := range m.neighbours[from.name] {
		if rebr, _ := n.receive(pkt); rebr != nil {
			queue = append(queue, send{from: n, pkt: *rebr})
		}
	}
	for len(queue) > 0 {
		s := queue[0]
		queue = queue[1:]
		for _, n := range m.neighbours[s.from.name] {
			if n == s.from {
				continue // don't send back to the sender (radio model)
			}
			if rebr, _ := n.receive(s.pkt); rebr != nil {
				queue = append(queue, send{from: n, pkt: *rebr})
			}
		}
	}
}

func TestRouter_3NodeMeshChain_PropagatesAndStops(t *testing.T) {
	// Topology: A -- B -- C (B is a relay)
	a, b, c := newNode("A"), newNode("B"), newNode("C")
	m := &mesh{
		neighbours: map[string][]*node{
			"A": {b},
			"B": {a, c},
			"C": {b},
		},
	}

	m.broadcast(a, Routable{MsgID: 42, TTL: 3, Payload: []byte("hello")})

	if len(b.delivered) != 1 || string(b.delivered[0]) != "hello" {
		t.Errorf("B should have delivered exactly once: %v", b.delivered)
	}
	if len(c.delivered) != 1 || string(c.delivered[0]) != "hello" {
		t.Errorf("C should have delivered exactly once: %v", c.delivered)
	}
	if len(a.delivered) != 0 {
		t.Errorf("A is the originator; it should not deliver loopback. got: %v", a.delivered)
	}
}

func TestRouter_3NodeMeshTriangle_NoLoops(t *testing.T) {
	// Topology: triangle A-B-C-A; every node hears every other directly.
	a, b, c := newNode("A"), newNode("B"), newNode("C")
	m := &mesh{
		neighbours: map[string][]*node{
			"A": {b, c},
			"B": {a, c},
			"C": {a, b},
		},
	}

	m.broadcast(a, Routable{MsgID: 99, TTL: 3, Payload: []byte("ping")})

	for _, n := range []*node{b, c} {
		if len(n.delivered) != 1 {
			t.Errorf("%s should have delivered exactly once, got %d", n.name, len(n.delivered))
		}
	}
	if len(a.delivered) != 0 {
		t.Errorf("A originator should not have delivered loopback, got %d", len(a.delivered))
	}
}

func TestRouter_TTLExhausts_3NodeChain(t *testing.T) {
	// Same chain A--B--C but TTL=1: should reach B and stop there.
	a, b, c := newNode("A"), newNode("B"), newNode("C")
	m := &mesh{
		neighbours: map[string][]*node{
			"A": {b},
			"B": {a, c},
			"C": {b},
		},
	}
	m.broadcast(a, Routable{MsgID: 7, TTL: 1, Payload: []byte("short")})

	if len(b.delivered) != 1 {
		t.Errorf("B should receive the packet, got %d", len(b.delivered))
	}
	if len(c.delivered) != 0 {
		t.Errorf("C should NOT receive (TTL exhausted at B), got %v", c.delivered)
	}
}
