package storage

import (
	"bytes"
	"context"
	"strings"
	"testing"
	"time"
)

const testDevice = "dev_test_pubkey"

func newClockedStore(t *testing.T) (*memoryStore, *fakeClock) {
	t.Helper()
	clk := &fakeClock{now: time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC)}
	return newMemoryWithClock(clk.Now), clk
}

type fakeClock struct {
	now time.Time
}

func (c *fakeClock) Now() time.Time { return c.now }
func (c *fakeClock) advance(d time.Duration) {
	c.now = c.now.Add(d)
}

func mustEnqueue(t *testing.T, s Store, payload []byte) Notification {
	t.Helper()
	n, err := s.Enqueue(context.Background(), testDevice, Notification{Payload: payload})
	if err != nil {
		t.Fatalf("Enqueue: %v", err)
	}
	return n
}

func TestEnqueueAssignsIDAndTimestamp(t *testing.T) {
	s, clk := newClockedStore(t)
	n, err := s.Enqueue(context.Background(), testDevice, Notification{Payload: []byte("hi")})
	if err != nil {
		t.Fatalf("Enqueue: %v", err)
	}
	if n.ID == "" {
		t.Fatal("expected server-assigned ID")
	}
	if !n.EnqueuedAt.Equal(clk.now) {
		t.Fatalf("EnqueuedAt = %v, want %v", n.EnqueuedAt, clk.now)
	}
	if n.Kind != KindNotification {
		t.Fatalf("Kind = %q, want %q", n.Kind, KindNotification)
	}
}

func TestEnqueueRejectsEmptyDevice(t *testing.T) {
	s, _ := newClockedStore(t)
	if _, err := s.Enqueue(context.Background(), "", Notification{Payload: []byte("hi")}); err != ErrInvalidDevice {
		t.Fatalf("want ErrInvalidDevice, got %v", err)
	}
}

func TestEnqueueRejectsUnknownKind(t *testing.T) {
	s, _ := newClockedStore(t)
	_, err := s.Enqueue(context.Background(), testDevice, Notification{Kind: "weird", Payload: []byte("hi")})
	if err != ErrInvalidKind {
		t.Fatalf("want ErrInvalidKind, got %v", err)
	}
}

func TestEnqueueRejectsOversizedPayload(t *testing.T) {
	s, _ := newClockedStore(t)
	big := bytes.Repeat([]byte{'x'}, MaxQueueBytes+1)
	_, err := s.Enqueue(context.Background(), testDevice, Notification{Payload: big})
	if err != ErrPayloadTooLarge {
		t.Fatalf("want ErrPayloadTooLarge, got %v", err)
	}
}

func TestListReturnsOldestFirst(t *testing.T) {
	s, clk := newClockedStore(t)
	for i := 0; i < 3; i++ {
		mustEnqueue(t, s, []byte{byte('a' + i)})
		clk.advance(time.Second)
	}
	got, err := s.List(context.Background(), testDevice)
	if err != nil {
		t.Fatalf("List: %v", err)
	}
	if len(got) != 3 {
		t.Fatalf("len = %d, want 3", len(got))
	}
	for i := 1; i < len(got); i++ {
		if !got[i-1].EnqueuedAt.Before(got[i].EnqueuedAt) {
			t.Fatalf("not sorted oldest-first: %v", got)
		}
	}
}

func TestQueueCappedAtMaxQueueLen(t *testing.T) {
	s, clk := newClockedStore(t)
	var firstID string
	for i := 0; i < MaxQueueLen+5; i++ {
		n := mustEnqueue(t, s, []byte{byte(i)})
		if i == 0 {
			firstID = n.ID
		}
		clk.advance(time.Second)
	}
	got, err := s.List(context.Background(), testDevice)
	if err != nil {
		t.Fatalf("List: %v", err)
	}
	if len(got) != MaxQueueLen {
		t.Fatalf("len = %d, want %d", len(got), MaxQueueLen)
	}
	for _, entry := range got {
		if entry.ID == firstID {
			t.Fatalf("oldest entry %s should have been evicted", firstID)
		}
	}
}

func TestTTLEvictionOnEnqueue(t *testing.T) {
	s, clk := newClockedStore(t)
	old := mustEnqueue(t, s, []byte("old"))
	clk.advance(QueueTTL + time.Second)
	fresh := mustEnqueue(t, s, []byte("fresh"))

	got, err := s.List(context.Background(), testDevice)
	if err != nil {
		t.Fatalf("List: %v", err)
	}
	if len(got) != 1 || got[0].ID != fresh.ID {
		t.Fatalf("expected only fresh entry, got %+v (old id=%s)", got, old.ID)
	}
}

func TestTTLEvictionOnList(t *testing.T) {
	s, clk := newClockedStore(t)
	mustEnqueue(t, s, []byte("one"))
	clk.advance(QueueTTL + time.Hour)
	got, err := s.List(context.Background(), testDevice)
	if err != nil {
		t.Fatalf("List: %v", err)
	}
	if len(got) != 0 {
		t.Fatalf("expected empty queue after TTL, got %d entries", len(got))
	}
}

func TestByteCapDropsOldestUntilUnderLimit(t *testing.T) {
	s, clk := newClockedStore(t)
	// 256 KiB chunks: 4 fit under the 1 MiB cap, the 5th forces eviction.
	chunk := bytes.Repeat([]byte{'p'}, 256*1024)
	var ids []string
	for i := 0; i < 5; i++ {
		n := mustEnqueue(t, s, chunk)
		ids = append(ids, n.ID)
		clk.advance(time.Second)
	}
	got, err := s.List(context.Background(), testDevice)
	if err != nil {
		t.Fatalf("List: %v", err)
	}
	if totalBytes(got) > MaxQueueBytes {
		t.Fatalf("queue is %d bytes, exceeds cap %d", totalBytes(got), MaxQueueBytes)
	}
	if len(got) >= 5 {
		t.Fatalf("expected eviction to reduce queue length, got %d", len(got))
	}
	// First inserted entry must have been dropped.
	for _, entry := range got {
		if entry.ID == ids[0] {
			t.Fatalf("oldest entry should have been evicted by byte cap")
		}
	}
}

func TestDelete(t *testing.T) {
	s, _ := newClockedStore(t)
	n := mustEnqueue(t, s, []byte("doomed"))

	removed, err := s.Delete(context.Background(), testDevice, n.ID)
	if err != nil {
		t.Fatalf("Delete: %v", err)
	}
	if !removed {
		t.Fatal("expected removed=true")
	}

	got, err := s.List(context.Background(), testDevice)
	if err != nil {
		t.Fatalf("List: %v", err)
	}
	if len(got) != 0 {
		t.Fatalf("expected empty queue after Delete, got %d", len(got))
	}

	// Second delete is a no-op.
	removed, err = s.Delete(context.Background(), testDevice, n.ID)
	if err != nil {
		t.Fatalf("Delete (second): %v", err)
	}
	if removed {
		t.Fatal("expected removed=false on second Delete")
	}
}

func TestDeleteAcrossDevicesIsolated(t *testing.T) {
	s, _ := newClockedStore(t)
	n, err := s.Enqueue(context.Background(), "device-a", Notification{Payload: []byte("x")})
	if err != nil {
		t.Fatalf("Enqueue: %v", err)
	}
	removed, err := s.Delete(context.Background(), "device-b", n.ID)
	if err != nil {
		t.Fatalf("Delete: %v", err)
	}
	if removed {
		t.Fatal("cross-device delete must not affect another device's queue")
	}
}

func TestListEmptyQueue(t *testing.T) {
	s, _ := newClockedStore(t)
	got, err := s.List(context.Background(), testDevice)
	if err != nil {
		t.Fatalf("List: %v", err)
	}
	if len(got) != 0 {
		t.Fatalf("expected empty, got %d", len(got))
	}
}

func TestEnqueueGroupEventKind(t *testing.T) {
	s, _ := newClockedStore(t)
	n, err := s.Enqueue(context.Background(), testDevice, Notification{
		Kind:    KindGroupEvent,
		AppID:   "app.chat",
		Payload: []byte("group"),
	})
	if err != nil {
		t.Fatalf("Enqueue: %v", err)
	}
	if n.Kind != KindGroupEvent {
		t.Fatalf("Kind = %q, want %q", n.Kind, KindGroupEvent)
	}
}

func TestNewNotificationIDFormat(t *testing.T) {
	id := newNotificationID()
	if len(id) != 32 {
		t.Fatalf("len(id) = %d, want 32", len(id))
	}
	if strings.TrimLeft(id, "0123456789abcdef") != "" {
		t.Fatalf("non-hex char in id %q", id)
	}
}
