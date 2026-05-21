package ratelimit

import (
	"sync"
	"testing"
	"time"
)

func TestAllowFreshKeyConsumesOneToken(t *testing.T) {
	l := New(3)
	d := l.Allow("dev-a")
	if !d.Allowed {
		t.Fatal("first call must be allowed")
	}
	if d.Limit != 3 || d.Remaining != 2 {
		t.Errorf("limit/remaining: got (%d,%d) want (3,2)", d.Limit, d.Remaining)
	}
}

func TestAllowExhaustsBucketThenDenies(t *testing.T) {
	now := time.Unix(1_700_000_000, 0)
	l := NewWithClock(2, func() time.Time { return now })

	if d := l.Allow("k"); !d.Allowed || d.Remaining != 1 {
		t.Fatalf("call 1: got %+v", d)
	}
	if d := l.Allow("k"); !d.Allowed || d.Remaining != 0 {
		t.Fatalf("call 2: got %+v", d)
	}
	d := l.Allow("k")
	if d.Allowed {
		t.Fatal("call 3 should have been denied")
	}
	if d.RetryAfter <= 0 {
		t.Errorf("RetryAfter must be positive, got %s", d.RetryAfter)
	}
}

func TestAllowRefillsOverTime(t *testing.T) {
	now := time.Unix(1_700_000_000, 0)
	clock := func() time.Time { return now }
	l := NewWithClock(60, clock) // 1 token / sec

	// Burn the full bucket immediately.
	for i := 0; i < 60; i++ {
		if d := l.Allow("k"); !d.Allowed {
			t.Fatalf("burst call %d: not allowed", i)
		}
	}
	if d := l.Allow("k"); d.Allowed {
		t.Fatal("post-burst call must be denied")
	}

	// Advance 1.5 s — one full token plus change.
	now = now.Add(1500 * time.Millisecond)
	if d := l.Allow("k"); !d.Allowed {
		t.Fatalf("after 1.5s refill: should allow, got %+v", d)
	}
	// Second call immediately after should still be denied.
	if d := l.Allow("k"); d.Allowed {
		t.Errorf("second immediate call should be denied (only ~0.5 tokens left)")
	}

	// Advance another full minute — bucket should be back at cap.
	now = now.Add(time.Minute)
	for i := 0; i < 60; i++ {
		if d := l.Allow("k"); !d.Allowed {
			t.Fatalf("post-refill call %d: not allowed", i)
		}
	}
}

func TestRetryAfterClampedToSecondMinimum(t *testing.T) {
	now := time.Unix(1_700_000_000, 0)
	// 600 req/min = 100 ms / token — tiny.
	l := NewWithClock(600, func() time.Time { return now })
	for i := 0; i < 600; i++ {
		l.Allow("k")
	}
	d := l.Allow("k")
	if d.Allowed {
		t.Fatal("should be over-limit")
	}
	if d.RetryAfter < time.Second {
		t.Errorf("RetryAfter must be clamped to >= 1s, got %s", d.RetryAfter)
	}
}

func TestPerKeyBucketsAreIndependent(t *testing.T) {
	l := New(1)
	if d := l.Allow("a"); !d.Allowed {
		t.Fatal("a: first call must be allowed")
	}
	if d := l.Allow("b"); !d.Allowed {
		t.Fatal("b: first call must be allowed even though a is empty")
	}
	if d := l.Allow("a"); d.Allowed {
		t.Fatal("a: second call must be denied")
	}
	if d := l.Allow("b"); d.Allowed {
		t.Fatal("b: second call must be denied")
	}
}

func TestZeroCapDisablesLimiting(t *testing.T) {
	l := New(0)
	for i := 0; i < 1000; i++ {
		if d := l.Allow("k"); !d.Allowed {
			t.Fatalf("call %d: zero-cap should always allow", i)
		}
	}
	if l.Len() != 0 {
		t.Errorf("zero-cap should not store buckets, got %d", l.Len())
	}
}

func TestSweepDropsIdleBuckets(t *testing.T) {
	now := time.Unix(1_700_000_000, 0)
	l := NewWithClock(10, func() time.Time { return now })
	l.SetIdleTTL(time.Minute)

	l.Allow("active")
	l.Allow("stale")
	if l.Len() != 2 {
		t.Fatalf("setup: Len=%d want 2", l.Len())
	}

	// Advance well past the TTL.
	now = now.Add(2 * time.Minute)
	// Touch "active" so it stays warm.
	l.Allow("active")

	removed := l.Sweep()
	if removed != 1 {
		t.Errorf("Sweep removed %d, want 1", removed)
	}
	if l.Len() != 1 {
		t.Errorf("Len after sweep: got %d want 1", l.Len())
	}
}

func TestAllowIsConcurrencySafe(t *testing.T) {
	l := New(1_000_000) // generous cap so we only test the lock, not denial
	var wg sync.WaitGroup
	const workers, perWorker = 32, 200
	for i := 0; i < workers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := 0; j < perWorker; j++ {
				l.Allow("shared")
			}
		}()
	}
	wg.Wait()
	if l.Len() != 1 {
		t.Errorf("expected exactly 1 bucket for shared key, got %d", l.Len())
	}
}

func TestCapReturnsConfiguredValue(t *testing.T) {
	if got := New(42).Cap(); got != 42 {
		t.Errorf("Cap: got %d want 42", got)
	}
	if got := New(0).Cap(); got != 0 {
		t.Errorf("Cap: got %d want 0 (disabled)", got)
	}
}
