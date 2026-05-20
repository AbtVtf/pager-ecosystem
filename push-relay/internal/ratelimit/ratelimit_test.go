package ratelimit

import (
	"sync"
	"testing"
	"time"
)

func TestAllowUnderLimit(t *testing.T) {
	l := NewMemory()
	now := time.Date(2026, 5, 21, 12, 0, 0, 0, time.UTC)
	for i := 0; i < HourLimit; i++ {
		ok, retry := l.Allow("notes.mafu.dev", "deviceA", now.Add(time.Duration(i)*time.Second))
		if !ok {
			t.Fatalf("send %d unexpectedly denied (retry=%s)", i, retry)
		}
	}
	hour, day := l.Snapshot("notes.mafu.dev", "deviceA", now.Add(HourLimit*time.Second))
	if hour != HourLimit || day != HourLimit {
		t.Fatalf("snapshot = (%d, %d), want (%d, %d)", hour, day, HourLimit, HourLimit)
	}
}

func TestAllowAtHourLimitDenies(t *testing.T) {
	l := NewMemoryWithLimits(3, 10) // tighten for readable test
	t0 := time.Date(2026, 5, 21, 12, 0, 0, 0, time.UTC)
	for i := 0; i < 3; i++ {
		if ok, _ := l.Allow("a.b", "d", t0.Add(time.Duration(i)*time.Minute)); !ok {
			t.Fatalf("send %d denied", i)
		}
	}
	ok, retry := l.Allow("a.b", "d", t0.Add(4*time.Minute))
	if ok {
		t.Fatalf("send over hour cap should be denied")
	}
	if retry <= 0 {
		t.Fatalf("expected positive Retry-After, got %s", retry)
	}
	// First send was at t0; window slides off at t0+1h. We're at t0+4m.
	want := time.Hour - 4*time.Minute
	if retry < want-time.Second || retry > want+time.Second {
		t.Fatalf("Retry-After = %s, want ≈%s", retry, want)
	}
}

func TestHourWindowSlides(t *testing.T) {
	l := NewMemoryWithLimits(2, 10)
	t0 := time.Date(2026, 5, 21, 12, 0, 0, 0, time.UTC)
	for i := 0; i < 2; i++ {
		if ok, _ := l.Allow("a.b", "d", t0.Add(time.Duration(i)*time.Minute)); !ok {
			t.Fatalf("warmup %d denied", i)
		}
	}
	if ok, _ := l.Allow("a.b", "d", t0.Add(2*time.Minute)); ok {
		t.Fatalf("should be at the hour cap")
	}
	// Walk past the oldest entry's hour boundary: t0+1h+1s.
	if ok, _ := l.Allow("a.b", "d", t0.Add(time.Hour+time.Second)); !ok {
		t.Fatalf("send just after the first slot expired should be allowed")
	}
}

func TestAllowAtDayLimitDenies(t *testing.T) {
	// hourMax >= dayMax so the day cap is the binding constraint.
	l := NewMemoryWithLimits(5, 3)
	t0 := time.Date(2026, 5, 21, 12, 0, 0, 0, time.UTC)
	// Spread well outside the hour window so we don't trip that cap first.
	for i := 0; i < 3; i++ {
		if ok, _ := l.Allow("a.b", "d", t0.Add(time.Duration(i*2)*time.Hour)); !ok {
			t.Fatalf("warmup %d denied", i)
		}
	}
	at := t0.Add(8 * time.Hour)
	ok, retry := l.Allow("a.b", "d", at)
	if ok {
		t.Fatalf("send over day cap should be denied")
	}
	// Oldest send was at t0; expires at t0+24h. We're at t0+8h.
	want := 16 * time.Hour
	if retry < want-time.Second || retry > want+time.Second {
		t.Fatalf("Retry-After = %s, want ≈%s", retry, want)
	}
}

func TestDayWindowEvictsOldEntries(t *testing.T) {
	l := NewMemoryWithLimits(5, 2)
	t0 := time.Date(2026, 5, 21, 0, 0, 0, 0, time.UTC)
	for i := 0; i < 2; i++ {
		if ok, _ := l.Allow("a.b", "d", t0.Add(time.Duration(i)*time.Hour)); !ok {
			t.Fatalf("warmup %d denied", i)
		}
	}
	// At cap.
	if ok, _ := l.Allow("a.b", "d", t0.Add(2*time.Hour)); ok {
		t.Fatalf("expected denial at day cap")
	}
	// After the oldest entry's 24h expires, we should drop back to one entry
	// and the new send should land.
	if ok, _ := l.Allow("a.b", "d", t0.Add(24*time.Hour+time.Second)); !ok {
		t.Fatalf("send after eviction should be allowed")
	}
}

func TestBucketsAreIndependent(t *testing.T) {
	l := NewMemoryWithLimits(1, 5)
	at := time.Date(2026, 5, 21, 12, 0, 0, 0, time.UTC)
	if ok, _ := l.Allow("a.b", "device1", at); !ok {
		t.Fatalf("first send denied")
	}
	// Same app, different device → independent bucket.
	if ok, _ := l.Allow("a.b", "device2", at); !ok {
		t.Fatalf("different device should have independent bucket")
	}
	// Different app, same device → independent bucket.
	if ok, _ := l.Allow("c.d", "device1", at); !ok {
		t.Fatalf("different app should have independent bucket")
	}
	// Hitting (a.b, device1) again is denied.
	if ok, _ := l.Allow("a.b", "device1", at); ok {
		t.Fatalf("re-send to same bucket should be denied")
	}
}

func TestEmptyInputsDenied(t *testing.T) {
	l := NewMemory()
	at := time.Now()
	if ok, _ := l.Allow("", "d", at); ok {
		t.Fatalf("empty app id should be denied")
	}
	if ok, _ := l.Allow("a.b", "", at); ok {
		t.Fatalf("empty device pubkey should be denied")
	}
}

func TestRetryAfterAtLeastOneSecond(t *testing.T) {
	l := NewMemoryWithLimits(1, 5)
	t0 := time.Date(2026, 5, 21, 12, 0, 0, 0, time.UTC)
	if ok, _ := l.Allow("a.b", "d", t0); !ok {
		t.Fatalf("warmup denied")
	}
	// Probe almost-but-not-quite an hour later: the oldest entry expires
	// 500 ms in the future. The reported Retry-After must round up to 1s.
	probe := t0.Add(time.Hour - 500*time.Millisecond)
	ok, retry := l.Allow("a.b", "d", probe)
	if ok {
		t.Fatalf("expected denial")
	}
	if retry != time.Second {
		t.Fatalf("Retry-After = %s, want 1s", retry)
	}
}

func TestConcurrentAllowIsSafe(t *testing.T) {
	l := NewMemoryWithLimits(1000, 5000)
	at := time.Date(2026, 5, 21, 12, 0, 0, 0, time.UTC)

	var wg sync.WaitGroup
	const goroutines = 16
	const sendsPer = 50
	wg.Add(goroutines)
	for g := 0; g < goroutines; g++ {
		go func(g int) {
			defer wg.Done()
			for i := 0; i < sendsPer; i++ {
				l.Allow("a.b", "d", at.Add(time.Duration(i+g*sendsPer)*time.Millisecond))
			}
		}(g)
	}
	wg.Wait()
	hour, day := l.Snapshot("a.b", "d", at.Add(time.Second))
	if hour != goroutines*sendsPer || day != goroutines*sendsPer {
		t.Fatalf("snapshot = (%d, %d), want (%d, %d)", hour, day, goroutines*sendsPer, goroutines*sendsPer)
	}
}

func TestNewMemoryNonpositiveLimitsPanic(t *testing.T) {
	for _, tc := range []struct{ h, d int }{{0, 1}, {1, 0}, {-1, 1}} {
		func() {
			defer func() {
				if recover() == nil {
					t.Fatalf("expected panic for limits (%d,%d)", tc.h, tc.d)
				}
			}()
			NewMemoryWithLimits(tc.h, tc.d)
		}()
	}
}
