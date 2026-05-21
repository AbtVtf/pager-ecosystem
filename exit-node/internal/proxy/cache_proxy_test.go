package proxy

import (
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
	"time"

	"github.com/pageros/pager-ecosystem/exit-node/internal/lora"
)

// TestHandle_CacheHit verifies a second identical Handle call is served
// from the cache without touching the upstream.
func TestHandle_CacheHit(t *testing.T) {
	var hits atomic.Int64
	upstream := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		hits.Add(1)
		w.Header().Set("Cache-Control", "max-age=60")
		w.Header().Set("Content-Type", ContentTypeInner)
		_, _ = w.Write([]byte("cached-response"))
	}))
	defer upstream.Close()

	p := New(Config{
		HTTPClient: upstream.Client(),
		Cache:      NewLRUCache(LRUOptions{}),
	})
	raw, _ := fixedInner(t, upstream.URL+"/frame")

	resp1, err := p.Handle(context.Background(), lora.Request{Body: raw})
	if err != nil {
		t.Fatalf("first Handle: %v", err)
	}
	resp2, err := p.Handle(context.Background(), lora.Request{Body: raw})
	if err != nil {
		t.Fatalf("second Handle: %v", err)
	}
	if hits.Load() != 1 {
		t.Errorf("upstream should be hit once, got %d", hits.Load())
	}
	if string(resp1.Body) != "cached-response" || string(resp2.Body) != "cached-response" {
		t.Errorf("responses: %q %q", resp1.Body, resp2.Body)
	}
}

// TestHandle_NoCacheControl_NotCached: without max-age, the proxy never
// stores the response.
func TestHandle_NoCacheControl_NotCached(t *testing.T) {
	var hits atomic.Int64
	upstream := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		hits.Add(1)
		_, _ = w.Write([]byte("uncacheable"))
	}))
	defer upstream.Close()

	cache := NewLRUCache(LRUOptions{})
	p := New(Config{HTTPClient: upstream.Client(), Cache: cache})
	raw, _ := fixedInner(t, upstream.URL+"/x")

	for i := 0; i < 3; i++ {
		if _, err := p.Handle(context.Background(), lora.Request{Body: raw}); err != nil {
			t.Fatalf("Handle: %v", err)
		}
	}
	if hits.Load() != 3 {
		t.Errorf("no max-age → no caching; want 3 upstream hits, got %d", hits.Load())
	}
	if cache.Len() != 0 {
		t.Errorf("cache should be empty, got %d entries", cache.Len())
	}
}

// TestHandle_NoStore_NotCached: upstream `Cache-Control: no-store`
// suppresses caching even if max-age is also set.
func TestHandle_NoStore_NotCached(t *testing.T) {
	var hits atomic.Int64
	upstream := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		hits.Add(1)
		w.Header().Set("Cache-Control", "no-store, max-age=60")
		_, _ = w.Write([]byte("private"))
	}))
	defer upstream.Close()

	cache := NewLRUCache(LRUOptions{})
	p := New(Config{HTTPClient: upstream.Client(), Cache: cache})
	raw, _ := fixedInner(t, upstream.URL+"/secret")

	for i := 0; i < 2; i++ {
		if _, err := p.Handle(context.Background(), lora.Request{Body: raw}); err != nil {
			t.Fatalf("Handle: %v", err)
		}
	}
	if hits.Load() != 2 {
		t.Errorf("no-store should bypass cache; want 2 hits, got %d", hits.Load())
	}
	if cache.Len() != 0 {
		t.Error("no-store response was cached")
	}
}

// TestHandle_5xx_NotCached: errors must not be memoised.
func TestHandle_5xx_NotCached(t *testing.T) {
	var hits atomic.Int64
	upstream := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		hits.Add(1)
		w.Header().Set("Cache-Control", "max-age=300")
		w.WriteHeader(http.StatusInternalServerError)
		_, _ = w.Write([]byte("oh no"))
	}))
	defer upstream.Close()

	cache := NewLRUCache(LRUOptions{})
	p := New(Config{HTTPClient: upstream.Client(), Cache: cache})
	raw, _ := fixedInner(t, upstream.URL+"/oops")

	for i := 0; i < 2; i++ {
		_, _ = p.Handle(context.Background(), lora.Request{Body: raw})
	}
	if hits.Load() != 2 {
		t.Errorf("5xx must not be cached; got %d hits", hits.Load())
	}
}

// TestHandle_DifferentBodies_DifferentCacheEntries: cache key includes
// the body hash, so two different request bodies to the same URL are
// independent cache entries.
func TestHandle_DifferentBodies_DifferentCacheEntries(t *testing.T) {
	count := atomic.Int64{}
	upstream := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		i := count.Add(1)
		w.Header().Set("Cache-Control", "max-age=60")
		fmt.Fprintf(w, "response-%d", i)
	}))
	defer upstream.Close()

	cache := NewLRUCache(LRUOptions{})
	p := New(Config{HTTPClient: upstream.Client(), Cache: cache})
	r1, _ := fixedInner(t, upstream.URL+"/x")
	// Build a second envelope by changing the inner Body.
	env := lora.InnerEnvelope{
		To:    upstream.URL + "/x",
		From:  r1[0:0], // we'll rebuild from scratch
	}
	_ = env
	// Easier: just change a byte in r1 — DecodeInner will still parse
	// the CBOR but the bytes hash differently. Mutate a payload byte.
	r2 := append([]byte(nil), r1...)
	// Find the body's last byte index — for the test we can just flip the
	// last byte; CBOR decode will give a different inner.Body but to is
	// the same.
	if len(r2) > 0 {
		r2[len(r2)-1] ^= 0xFF
	}

	resp1, _ := p.Handle(context.Background(), lora.Request{Body: r1})
	resp2, _ := p.Handle(context.Background(), lora.Request{Body: r2})
	if string(resp1.Body) == string(resp2.Body) {
		t.Errorf("different bodies got merged: %q %q", resp1.Body, resp2.Body)
	}
	if cache.Len() != 2 {
		t.Errorf("expected 2 cache entries, got %d", cache.Len())
	}
}

// TestHandle_ExpiredEntry_RefetchesUpstream simulates the TTL elapsing
// and verifies the proxy goes back to the upstream.
func TestHandle_ExpiredEntry_RefetchesUpstream(t *testing.T) {
	var hits atomic.Int64
	upstream := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		hits.Add(1)
		w.Header().Set("Cache-Control", "max-age=1")
		fmt.Fprintf(w, "hit-%d", hits.Load())
	}))
	defer upstream.Close()

	now := atomic.Int64{}
	now.Store(time.Now().Unix())
	cache := NewLRUCache(LRUOptions{Now: func() time.Time { return time.Unix(now.Load(), 0) }})
	p := New(Config{HTTPClient: upstream.Client(), Cache: cache})
	raw, _ := fixedInner(t, upstream.URL+"/x")

	if _, err := p.Handle(context.Background(), lora.Request{Body: raw}); err != nil {
		t.Fatalf("first: %v", err)
	}
	// Within TTL: cache hit
	if _, err := p.Handle(context.Background(), lora.Request{Body: raw}); err != nil {
		t.Fatalf("second: %v", err)
	}
	if hits.Load() != 1 {
		t.Errorf("within TTL: expected 1 upstream hit, got %d", hits.Load())
	}
	// Advance past TTL: cache miss, new upstream hit
	now.Add(5)
	if _, err := p.Handle(context.Background(), lora.Request{Body: raw}); err != nil {
		t.Fatalf("third: %v", err)
	}
	if hits.Load() != 2 {
		t.Errorf("after TTL: expected 2 upstream hits, got %d", hits.Load())
	}
}
