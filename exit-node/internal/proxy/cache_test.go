package proxy

import (
	"sync/atomic"
	"testing"
	"time"
)

func TestParseCacheControl(t *testing.T) {
	cases := []struct {
		in       string
		wantNo   bool
		wantTTLs int64 // seconds
	}{
		{"", false, 0},
		{"max-age=60", false, 60},
		{"public, max-age=30", false, 30},
		{"no-store", true, 0},
		{"no-store, max-age=60", true, 60},
		{"private", true, 0},
		{"no-cache", true, 0}, // treated as no-store in v1
		{"s-maxage=120, max-age=30", false, 120},
		{"max-age=invalid", false, 0},
		{"max-age=-5", false, 0},
		{"MAX-AGE=15", false, 15},          // case-insensitive
		{"public,  max-age=42  ,xyz", false, 42},
	}
	for _, tc := range cases {
		got := ParseCacheControl(tc.in)
		if got.NoStore != tc.wantNo {
			t.Errorf("ParseCacheControl(%q).NoStore=%v want %v", tc.in, got.NoStore, tc.wantNo)
		}
		if int64(got.MaxAge/time.Second) != tc.wantTTLs {
			t.Errorf("ParseCacheControl(%q).MaxAge=%v want %ds", tc.in, got.MaxAge, tc.wantTTLs)
		}
	}
}

func TestCacheKeyDifferentiatesByBody(t *testing.T) {
	a := cacheKey("https://app.example/", []byte("one"))
	b := cacheKey("https://app.example/", []byte("two"))
	c := cacheKey("https://other.example/", []byte("one"))
	if a == b {
		t.Error("different bodies should produce different keys")
	}
	if a == c {
		t.Error("different URLs should produce different keys")
	}
}

func TestLRUCache_HitMissAndExpiry(t *testing.T) {
	now := int64(1_000_000)
	c := NewLRUCache(LRUOptions{
		MaxEntries: 4,
		MaxBytes:   1 << 20,
		Now:        func() time.Time { return time.Unix(atomic.LoadInt64(&now), 0) },
	})

	// Miss on empty cache
	if _, ok := c.Get("k"); ok {
		t.Fatal("empty cache should miss")
	}

	c.Put("k", []byte("hello"), 10*time.Second)
	if b, ok := c.Get("k"); !ok || string(b) != "hello" {
		t.Fatalf("expected hit, got %v %v", string(b), ok)
	}

	// Advance past TTL → expired
	atomic.StoreInt64(&now, 1_000_011)
	if _, ok := c.Get("k"); ok {
		t.Fatal("expected miss after TTL")
	}
	if c.Len() != 0 {
		t.Errorf("expired entry should be evicted on Get, Len=%d", c.Len())
	}
}

func TestLRUCache_ZeroTTLIsNoop(t *testing.T) {
	c := NewLRUCache(LRUOptions{})
	c.Put("k", []byte("x"), 0)
	if c.Len() != 0 {
		t.Fatal("ttl=0 should not store")
	}
	c.Put("k", []byte("x"), -5*time.Second)
	if c.Len() != 0 {
		t.Fatal("negative ttl should not store")
	}
}

func TestLRUCache_EvictsLeastRecentlyUsed(t *testing.T) {
	c := NewLRUCache(LRUOptions{MaxEntries: 2})
	c.Put("a", []byte("1"), time.Minute)
	c.Put("b", []byte("2"), time.Minute)
	// Touch "a" so "b" is now LRU
	c.Get("a")
	c.Put("c", []byte("3"), time.Minute)
	if _, ok := c.Get("a"); !ok {
		t.Error("a should still be cached")
	}
	if _, ok := c.Get("b"); ok {
		t.Error("b should have been evicted")
	}
	if _, ok := c.Get("c"); !ok {
		t.Error("c should be cached")
	}
}

func TestLRUCache_EvictsByByteBudget(t *testing.T) {
	c := NewLRUCache(LRUOptions{MaxEntries: 100, MaxBytes: 10})
	c.Put("a", []byte("12345"), time.Minute) // 5 bytes
	c.Put("b", []byte("67890"), time.Minute) // 5 bytes → 10 total, at cap
	c.Put("c", []byte("X"), time.Minute)     // forces eviction of "a" (LRU)
	if _, ok := c.Get("a"); ok {
		t.Error("a should have been evicted by byte budget")
	}
	if got := c.Bytes(); got > 10 {
		t.Errorf("bytes total %d > cap 10", got)
	}
}

func TestLRUCache_OversizedBodyNotStored(t *testing.T) {
	c := NewLRUCache(LRUOptions{MaxBytes: 4})
	c.Put("k", []byte("12345"), time.Minute)
	if c.Len() != 0 {
		t.Error("body larger than MaxBytes should be rejected")
	}
}

func TestLRUCache_PutReplacesExistingKey(t *testing.T) {
	c := NewLRUCache(LRUOptions{})
	c.Put("k", []byte("old"), time.Minute)
	c.Put("k", []byte("new"), time.Minute)
	if b, _ := c.Get("k"); string(b) != "new" {
		t.Errorf("expected new value, got %q", string(b))
	}
	if c.Len() != 1 {
		t.Errorf("replace should not duplicate, Len=%d", c.Len())
	}
}

func TestLRUCache_GetReturnsCopy(t *testing.T) {
	c := NewLRUCache(LRUOptions{})
	c.Put("k", []byte("data"), time.Minute)
	b1, _ := c.Get("k")
	b1[0] = 'X'
	b2, _ := c.Get("k")
	if string(b2) != "data" {
		t.Errorf("mutation through returned slice leaked into cache: %q", string(b2))
	}
}
