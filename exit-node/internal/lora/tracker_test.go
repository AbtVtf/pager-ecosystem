package lora

import (
	"testing"
	"time"
)

// pk32 returns a 32-byte pubkey filled with the given byte, useful for
// distinguishing simulated nodes in tests.
func pk32(b byte) [32]byte {
	var pk [32]byte
	for i := range pk {
		pk[i] = b
	}
	return pk
}

// LORA-005 acceptance: three simulated Exit Nodes, devices maintain a
// ranked list keyed on RSSI and load.
//
// nodeA — load 100, rssi -90 → mid load, weak signal
// nodeB — load  50, rssi -70 → low load, strong signal  → BEST
// nodeC — load 100, rssi -60 → mid load, strongest signal (loses to B on load)
func TestTrackerRanksThreeSimulatedNodes(t *testing.T) {
	tr := NewTracker()

	a := Advert{Pubkey: pk32(0xAA), BWClass: BWMedium, Load: 100}
	b := Advert{Pubkey: pk32(0xBB), BWClass: BWHigh, Load: 50}
	c := Advert{Pubkey: pk32(0xCC), BWClass: BWHigh, Load: 100}

	tr.Observe(a, -90)
	tr.Observe(b, -70)
	tr.Observe(c, -60)

	got := tr.Ranked()
	if len(got) != 3 {
		t.Fatalf("ranked len: got %d want 3", len(got))
	}
	wantOrder := [][32]byte{b.Pubkey, c.Pubkey, a.Pubkey}
	for i, want := range wantOrder {
		if got[i].Pubkey != want {
			t.Errorf("rank %d: got pubkey starting %02x, want %02x",
				i, got[i].Pubkey[0], want[0])
		}
	}

	best, ok := tr.Best()
	if !ok {
		t.Fatalf("Best: got !ok")
	}
	if best.Pubkey != b.Pubkey {
		t.Errorf("Best pubkey: got starting %02x, want %02x", best.Pubkey[0], b.Pubkey[0])
	}
}

// Same load → tie-broken by RSSI.
func TestTrackerTieBreakByRSSI(t *testing.T) {
	tr := NewTracker()

	a := Advert{Pubkey: pk32(0x01), BWClass: BWHigh, Load: 80}
	b := Advert{Pubkey: pk32(0x02), BWClass: BWHigh, Load: 80}

	tr.Observe(a, -85)
	tr.Observe(b, -75) // stronger

	best, _ := tr.Best()
	if best.Pubkey != b.Pubkey {
		t.Errorf("Best: got starting %02x, want %02x (stronger RSSI wins tie)",
			best.Pubkey[0], b.Pubkey[0])
	}
}

// Same load + RSSI → tie-broken by bandwidth class (higher wins).
func TestTrackerTieBreakByBandwidth(t *testing.T) {
	tr := NewTracker()

	a := Advert{Pubkey: pk32(0x01), BWClass: BWMedium, Load: 10}
	b := Advert{Pubkey: pk32(0x02), BWClass: BWGigabit, Load: 10}

	tr.Observe(a, -80)
	tr.Observe(b, -80)

	best, _ := tr.Best()
	if best.Pubkey != b.Pubkey {
		t.Errorf("Best: got starting %02x, want %02x (higher bw_class wins)",
			best.Pubkey[0], b.Pubkey[0])
	}
}

// Re-observing the same node updates load/RSSI rather than duplicating.
func TestTrackerRefreshes(t *testing.T) {
	tr := NewTracker()

	a := Advert{Pubkey: pk32(0x01), BWClass: BWHigh, Load: 200}
	tr.Observe(a, -100)
	a.Load = 5
	tr.Observe(a, -50)

	if tr.Len() != 1 {
		t.Errorf("Len: got %d want 1 (re-observe must not duplicate)", tr.Len())
	}
	best, _ := tr.Best()
	if best.Load != 5 || best.RSSI != -50 {
		t.Errorf("re-observe did not refresh: got %+v", best)
	}
}

// Stale entries are excluded from the ranked list.
func TestTrackerExpiresStale(t *testing.T) {
	tr := NewTracker()
	clock := time.Unix(1_700_000_000, 0)
	tr.SetClock(func() time.Time { return clock })
	tr.SetMaxAge(100 * time.Second)

	fresh := Advert{Pubkey: pk32(0xFE), BWClass: BWHigh, Load: 10}
	stale := Advert{Pubkey: pk32(0x5A), BWClass: BWHigh, Load: 5}

	tr.Observe(stale, -60)
	clock = clock.Add(200 * time.Second)
	tr.Observe(fresh, -60)

	got := tr.Ranked()
	if len(got) != 1 || got[0].Pubkey != fresh.Pubkey {
		t.Fatalf("expected only fresh entry, got %d entries: %+v", len(got), got)
	}

	tr.Prune()
	if tr.Len() != 1 {
		t.Errorf("Prune: got %d want 1 stale entry removed", tr.Len())
	}
}

func TestTrackerEmpty(t *testing.T) {
	tr := NewTracker()
	if _, ok := tr.Best(); ok {
		t.Errorf("Best on empty tracker: want !ok")
	}
	if got := tr.Ranked(); len(got) != 0 {
		t.Errorf("Ranked on empty tracker: got %d want 0", len(got))
	}
}
