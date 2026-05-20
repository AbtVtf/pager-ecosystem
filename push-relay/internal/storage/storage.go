// Package storage owns the push-relay queue backend. PUSH-001 lands the
// connection plumbing only; per-device queue semantics (cap, TTL, eviction)
// arrive with PUSH-005.
package storage

import (
	"context"
	"fmt"
	"net/url"
	"time"

	"github.com/redis/go-redis/v9"
)

// Store is the abstract backend interface. Kept intentionally minimal for the
// skeleton; PUSH-002..PUSH-005 will extend it (enqueue, list, delete, evict).
type Store interface {
	Ping(ctx context.Context) error
	Close() error
}

type redisStore struct {
	client *redis.Client
}

// OpenRedis connects to Redis using a redis:// or rediss:// URL and verifies
// the connection with PING.
func OpenRedis(ctx context.Context, rawURL string) (Store, error) {
	opts, err := redis.ParseURL(rawURL)
	if err != nil {
		return nil, fmt.Errorf("parse redis url: %w", err)
	}
	opts.DialTimeout = 5 * time.Second
	opts.ReadTimeout = 3 * time.Second
	opts.WriteTimeout = 3 * time.Second

	client := redis.NewClient(opts)
	if err := client.Ping(ctx).Err(); err != nil {
		_ = client.Close()
		return nil, fmt.Errorf("redis ping: %w", err)
	}
	return &redisStore{client: client}, nil
}

func (s *redisStore) Ping(ctx context.Context) error {
	return s.client.Ping(ctx).Err()
}

func (s *redisStore) Close() error {
	return s.client.Close()
}

// ParseRedisURL is a thin re-export used by main.go for safe URL logging.
func ParseRedisURL(rawURL string) (*url.URL, error) {
	return url.Parse(rawURL)
}
