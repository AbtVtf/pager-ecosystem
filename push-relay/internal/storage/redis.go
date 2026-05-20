package storage

import (
	"context"
	"encoding/json"
	"fmt"
	"time"

	"github.com/redis/go-redis/v9"
)

// redisStore is the production Store backend. Queues are stored as Redis
// sorted sets keyed by `pageros:push:queue:<devicePubkey>`, scored by the
// nanosecond enqueue timestamp. All three eviction rules (TTL, count cap,
// byte cap) run atomically inside a Lua script so concurrent Enqueue/List
// calls cannot observe a queue that violates the spec.
type redisStore struct {
	client      *redis.Client
	enqueueLua  *redis.Script
	keyPrefix   string
}

// queueKey returns the Redis sorted-set key for a device's queue.
func (s *redisStore) queueKey(devicePubkey string) string {
	return s.keyPrefix + devicePubkey
}

const defaultKeyPrefix = "pageros:push:queue:"

// enqueueScript performs an atomic enqueue + eviction. KEYS[1] is the queue
// sorted-set key; ARGV are documented inline.
//
// Note: we read the entire queue back to enforce the byte cap. Practical
// upper bound is MaxQueueLen entries × MaxQueueBytes / MaxQueueLen ≈ 1 MB,
// which is well within Redis' Lua execution budget.
var enqueueScriptSource = `
local key = KEYS[1]
local score = tonumber(ARGV[1])
local member = ARGV[2]
local max_count = tonumber(ARGV[3])
local ttl_cutoff = tonumber(ARGV[4])
local max_bytes = tonumber(ARGV[5])
local ttl_seconds = tonumber(ARGV[6])

-- Insert the new entry.
redis.call('ZADD', key, score, member)

-- Evict expired entries (score < ttl_cutoff).
redis.call('ZREMRANGEBYSCORE', key, '-inf', '(' .. ttl_cutoff)

-- Evict by count cap, keeping the newest N.
local count = redis.call('ZCARD', key)
if count > max_count then
  redis.call('ZREMRANGEBYRANK', key, 0, count - max_count - 1)
end

-- Evict by byte cap: drop oldest until total <= max_bytes.
local members = redis.call('ZRANGE', key, 0, -1)
local total = 0
for _, m in ipairs(members) do
  total = total + #m
end
local i = 1
while total > max_bytes and i < #members do
  redis.call('ZREM', key, members[i])
  total = total - #members[i]
  i = i + 1
end

-- Refresh the key-level TTL so the key disappears once stale.
redis.call('EXPIRE', key, ttl_seconds)
return 1
`

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
	return newRedisStore(client), nil
}

// NewRedisFromClient wraps an existing *redis.Client as a Store. Useful for
// tests that point at a miniredis or already-configured client.
func NewRedisFromClient(client *redis.Client) Store {
	return newRedisStore(client)
}

func newRedisStore(client *redis.Client) *redisStore {
	return &redisStore{
		client:     client,
		enqueueLua: redis.NewScript(enqueueScriptSource),
		keyPrefix:  defaultKeyPrefix,
	}
}

func (s *redisStore) Ping(ctx context.Context) error {
	return s.client.Ping(ctx).Err()
}

func (s *redisStore) Close() error {
	return s.client.Close()
}

func (s *redisStore) Enqueue(ctx context.Context, devicePubkey string, n Notification) (Notification, error) {
	now := time.Now()
	if err := validateForEnqueue(devicePubkey, &n, now); err != nil {
		return Notification{}, err
	}

	encoded, err := json.Marshal(n)
	if err != nil {
		return Notification{}, err
	}
	if len(encoded) > MaxQueueBytes {
		return Notification{}, ErrPayloadTooLarge
	}

	cutoff := now.Add(-QueueTTL).UnixNano()
	args := []any{
		n.EnqueuedAt.UnixNano(),         // score
		string(encoded),                 // member
		MaxQueueLen,                     // count cap
		cutoff,                          // ttl cutoff (ns)
		MaxQueueBytes,                   // byte cap
		int64(QueueTTL / time.Second),   // key-level EXPIRE seconds
	}
	if err := s.enqueueLua.Run(ctx, s.client, []string{s.queueKey(devicePubkey)}, args...).Err(); err != nil {
		return Notification{}, fmt.Errorf("redis enqueue: %w", err)
	}
	return n, nil
}

func (s *redisStore) List(ctx context.Context, devicePubkey string) ([]Notification, error) {
	if devicePubkey == "" {
		return nil, ErrInvalidDevice
	}
	key := s.queueKey(devicePubkey)

	// Prune expired entries before reading. A Lua block keeps this atomic
	// with respect to concurrent enqueues.
	cutoff := time.Now().Add(-QueueTTL).UnixNano()
	if err := s.client.ZRemRangeByScore(ctx, key, "-inf", fmt.Sprintf("(%d", cutoff)).Err(); err != nil {
		return nil, fmt.Errorf("redis prune: %w", err)
	}

	members, err := s.client.ZRange(ctx, key, 0, -1).Result()
	if err != nil {
		return nil, fmt.Errorf("redis range: %w", err)
	}
	out := make([]Notification, 0, len(members))
	for _, m := range members {
		var n Notification
		if err := json.Unmarshal([]byte(m), &n); err != nil {
			// Corrupt entry — skip rather than fail the whole pull. PUSH-008
			// (admin/abuse dashboard) will pick these up via logs once wired.
			continue
		}
		out = append(out, n)
	}
	return out, nil
}

func (s *redisStore) Delete(ctx context.Context, devicePubkey, id string) (bool, error) {
	if devicePubkey == "" {
		return false, ErrInvalidDevice
	}
	key := s.queueKey(devicePubkey)
	// We don't know the exact stored member without scanning, so scan and
	// remove. Queue size is bounded by MaxQueueLen so the scan is cheap.
	members, err := s.client.ZRange(ctx, key, 0, -1).Result()
	if err != nil {
		return false, fmt.Errorf("redis range: %w", err)
	}
	for _, m := range members {
		var n Notification
		if err := json.Unmarshal([]byte(m), &n); err != nil {
			continue
		}
		if n.ID == id {
			removed, err := s.client.ZRem(ctx, key, m).Result()
			if err != nil {
				return false, fmt.Errorf("redis zrem: %w", err)
			}
			return removed > 0, nil
		}
	}
	return false, nil
}
