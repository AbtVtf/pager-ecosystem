// Response caching at the Exit Node (EXIT-006 / SPEC §6.5).
//
// Per SPEC §6.5, exit-nodes MAY cache upstream responses keyed by
// destination URL + a hash of the request body, for the duration the
// upstream advertised via `Cache-Control: max-age=…`. The goal is to
// cut traffic for popular shared content (e.g. a weather app's frame
// requested by dozens of devices in the same minute) without making the
// per-device path slower or less private.
//
// What this file implements:
//
//   - A small `ResponseCache` interface so the proxy doesn't bake in a
//     specific backend (an operator running across multiple nodes can
//     plug in Redis/memcached later; the in-process LRU here is the
//     reference implementation).
//   - An in-process LRU bounded by entry count and total cached bytes,
//     with per-entry TTL derived from the upstream `Cache-Control`.
//   - A pure helper to parse `Cache-Control` directives — `no-store` and
//     `private` skip caching entirely; `max-age=N` (or `s-maxage=N`,
//     which the exit-node is a shared cache for) bounds the TTL.
//
// What this file does NOT do (yet):
//
//   - Device-side opt-out via a request flag. The spec calls for it
//     ("Privacy: opt-out via request flag") but the in-flight LORA-003
//     `InnerEnvelope` has fixed fields; adding `NoCache` to it touches
//     every SDK and firmware path. Tracked for a follow-up. The current
//     opt-out is upstream-side: any app that returns `Cache-Control:
//     no-store` or `private` is never cached.
//   - Distributed invalidation. A single node's LRU is per-process.

package proxy

import (
	"container/list"
	"crypto/sha256"
	"encoding/hex"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Cache key format: "<url>|<sha256(body)>". URL first so an admin
// scanning the cache table can see hot endpoints at a glance.
func cacheKey(url string, body []byte) string {
	sum := sha256.Sum256(body)
	return url + "|" + hex.EncodeToString(sum[:])
}

// ResponseCache is the swappable cache backend. Implementations must be
// safe for concurrent use. A nil cache disables caching at the call site.
type ResponseCache interface {
	// Get returns (body, true) if the key is present and not expired.
	Get(key string) ([]byte, bool)

	// Put stores body under key with the given TTL. Implementations
	// must treat ttl <= 0 as a no-op (the spec says no max-age = no
	// cache).
	Put(key string, body []byte, ttl time.Duration)
}

// CachePolicy is the parsed view of an upstream `Cache-Control` header
// that the proxy needs to decide what to do with a response.
type CachePolicy struct {
	// NoStore is true when the upstream said `no-store` or `private`.
	// We never cache those even if max-age is set.
	NoStore bool

	// MaxAge is the cap on how long this response may be cached. Zero
	// means "do not cache" (matches spec language; `max-age=0` is a
	// revalidation request, not a cache offer).
	MaxAge time.Duration
}

// ParseCacheControl picks the directives the exit-node cares about out
// of a (possibly comma-separated) Cache-Control header value. Unknown
// directives are ignored. The exit-node is a *shared* cache, so
// `s-maxage` wins over `max-age` when both are present, per RFC 9111.
func ParseCacheControl(header string) CachePolicy {
	var p CachePolicy
	var sMax, max time.Duration
	var sawSMax, sawMax bool

	for _, raw := range strings.Split(header, ",") {
		d := strings.TrimSpace(strings.ToLower(raw))
		if d == "" {
			continue
		}
		switch {
		case d == "no-store", d == "private", d == "no-cache":
			// `no-cache` lets us cache but requires revalidation — we
			// don't do conditional GETs yet, so treat it as no-store
			// for the v1 implementation.
			p.NoStore = true
		case strings.HasPrefix(d, "max-age="):
			if n, ok := parseSeconds(d[len("max-age="):]); ok {
				max, sawMax = time.Duration(n)*time.Second, true
			}
		case strings.HasPrefix(d, "s-maxage="):
			if n, ok := parseSeconds(d[len("s-maxage="):]); ok {
				sMax, sawSMax = time.Duration(n)*time.Second, true
			}
		}
	}
	switch {
	case sawSMax:
		p.MaxAge = sMax
	case sawMax:
		p.MaxAge = max
	}
	return p
}

func parseSeconds(s string) (int64, bool) {
	n, err := strconv.ParseInt(strings.TrimSpace(s), 10, 64)
	if err != nil || n < 0 {
		return 0, false
	}
	return n, true
}

// --- LRU implementation ------------------------------------------------- //

// LRUCache is an in-process LRU response cache bounded by entry count
// and total cached bytes. Bytes-bound matters because exit-nodes run on
// constrained hardware (a Raspberry Pi or a tethered T-LoRa Pager) —
// SPEC §11.1.
type LRUCache struct {
	mu sync.Mutex

	maxEntries int
	maxBytes   int64
	now        func() time.Time

	bytes int64
	order *list.List // front = most recently used
	by    map[string]*list.Element
}

type lruEntry struct {
	key       string
	body      []byte
	expiresAt time.Time
}

// LRUOptions configures an LRUCache.
type LRUOptions struct {
	MaxEntries int           // default 1024
	MaxBytes   int64         // default 8 MiB
	Now        func() time.Time
}

// NewLRUCache builds an LRUCache with sane defaults.
func NewLRUCache(opts LRUOptions) *LRUCache {
	if opts.MaxEntries <= 0 {
		opts.MaxEntries = 1024
	}
	if opts.MaxBytes <= 0 {
		opts.MaxBytes = 8 * 1024 * 1024
	}
	if opts.Now == nil {
		opts.Now = time.Now
	}
	return &LRUCache{
		maxEntries: opts.MaxEntries,
		maxBytes:   opts.MaxBytes,
		now:        opts.Now,
		order:      list.New(),
		by:         make(map[string]*list.Element, opts.MaxEntries),
	}
}

// Get returns (body, true) when the key is present and not expired.
// Expired entries are evicted on access (lazy expiry).
func (c *LRUCache) Get(key string) ([]byte, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	el, ok := c.by[key]
	if !ok {
		return nil, false
	}
	e := el.Value.(*lruEntry)
	if !c.now().Before(e.expiresAt) {
		c.evictLocked(el)
		return nil, false
	}
	c.order.MoveToFront(el)
	// Return a copy so callers can't mutate the cached bytes.
	out := make([]byte, len(e.body))
	copy(out, e.body)
	return out, true
}

// Put stores body for ttl. ttl <= 0 is a no-op (matches SPEC §6.5
// "stated max-age" semantics). Bodies larger than MaxBytes are not
// stored — they'd evict the entire cache for one entry.
func (c *LRUCache) Put(key string, body []byte, ttl time.Duration) {
	if ttl <= 0 {
		return
	}
	bsize := int64(len(body))
	if bsize > c.maxBytes {
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()

	// Replace existing entry under the same key cleanly.
	if el, ok := c.by[key]; ok {
		c.evictLocked(el)
	}

	// Evict to make room.
	for c.bytes+bsize > c.maxBytes || c.order.Len() >= c.maxEntries {
		oldest := c.order.Back()
		if oldest == nil {
			break
		}
		c.evictLocked(oldest)
	}

	cpy := make([]byte, len(body))
	copy(cpy, body)
	e := &lruEntry{key: key, body: cpy, expiresAt: c.now().Add(ttl)}
	el := c.order.PushFront(e)
	c.by[key] = el
	c.bytes += bsize
}

// Len returns the current entry count. Exposed for tests + the
// admin/stats surface (EXIT-007).
func (c *LRUCache) Len() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.order.Len()
}

// Bytes returns the current total cached body size. Useful for caps +
// dashboards.
func (c *LRUCache) Bytes() int64 {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.bytes
}

func (c *LRUCache) evictLocked(el *list.Element) {
	e := el.Value.(*lruEntry)
	c.order.Remove(el)
	delete(c.by, e.key)
	c.bytes -= int64(len(e.body))
}
