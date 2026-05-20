package lora

import (
	"bytes"
	"context"
	"errors"
	"io"
	"sync"
	"testing"
	"time"
)

// chunkedBuffer captures discrete writes (each ticker fires one Write).
type chunkedBuffer struct {
	mu     sync.Mutex
	writes [][]byte
	errOn  int // 1-indexed write number to return an error on (0 = never)
}

func (b *chunkedBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.errOn > 0 && len(b.writes)+1 == b.errOn {
		return 0, io.ErrShortWrite
	}
	cp := make([]byte, len(p))
	copy(cp, p)
	b.writes = append(b.writes, cp)
	return len(p), nil
}

func (b *chunkedBuffer) Count() int {
	b.mu.Lock()
	defer b.mu.Unlock()
	return len(b.writes)
}

func (b *chunkedBuffer) At(i int) []byte {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.writes[i]
}

func TestAdvertiserValidation(t *testing.T) {
	tests := []struct {
		name string
		cfg  AdvertiserConfig
	}{
		{"no writer", AdvertiserConfig{Identity: pk32(0x01)}},
		{"zero identity", AdvertiserConfig{Writer: &bytes.Buffer{}}},
		{"too-short interval", AdvertiserConfig{Writer: &bytes.Buffer{}, Identity: pk32(0x01), Interval: time.Second}},
		{"too-long interval", AdvertiserConfig{Writer: &bytes.Buffer{}, Identity: pk32(0x01), Interval: time.Hour}},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := NewAdvertiser(tc.cfg); err == nil {
				t.Errorf("want error, got nil")
			}
		})
	}
}

func TestAdvertiserSendOnceEmitsValidEnvelope(t *testing.T) {
	buf := &chunkedBuffer{}
	pk := pk32(0x42)
	adv, err := NewAdvertiser(AdvertiserConfig{
		Writer:   buf,
		Identity: pk,
		BWClass:  BWHigh,
		Load:     func() uint8 { return 99 },
		Interval: DefaultAdvertInterval,
		MsgID:    func() uint32 { return 0xDEADBEEF },
	})
	if err != nil {
		t.Fatalf("NewAdvertiser: %v", err)
	}
	if err := adv.SendOnce(); err != nil {
		t.Fatalf("SendOnce: %v", err)
	}
	if buf.Count() != 1 {
		t.Fatalf("writes: got %d want 1", buf.Count())
	}
	env, err := Decode(buf.At(0))
	if err != nil {
		t.Fatalf("Decode envelope: %v", err)
	}
	if env.Type != TypeExitNodeAdvert {
		t.Errorf("envelope type: got %v want TypeExitNodeAdvert", env.Type)
	}
	if env.MsgID != 0xDEADBEEF {
		t.Errorf("msg_id: got %08x want deadbeef", env.MsgID)
	}
	a, err := DecodeAdvert(env.Payload)
	if err != nil {
		t.Fatalf("DecodeAdvert: %v", err)
	}
	if a.Pubkey != pk || a.BWClass != BWHigh || a.Load != 99 {
		t.Errorf("advert mismatch: %+v", a)
	}
}

func TestAdvertiserSendOnceNilLoadDefaultsToZero(t *testing.T) {
	buf := &chunkedBuffer{}
	adv, err := NewAdvertiser(AdvertiserConfig{
		Writer:   buf,
		Identity: pk32(0x01),
		BWClass:  BWMedium,
		// Load is nil
	})
	if err != nil {
		t.Fatalf("NewAdvertiser: %v", err)
	}
	if err := adv.SendOnce(); err != nil {
		t.Fatalf("SendOnce: %v", err)
	}
	env, _ := Decode(buf.At(0))
	a, _ := DecodeAdvert(env.Payload)
	if a.Load != 0 {
		t.Errorf("Load: got %d want 0 (nil LoadFunc default)", a.Load)
	}
}

func TestAdvertiserSendOnceSurfacesWriteError(t *testing.T) {
	buf := &chunkedBuffer{errOn: 1}
	adv, _ := NewAdvertiser(AdvertiserConfig{
		Writer:   buf,
		Identity: pk32(0x07),
		BWClass:  BWHigh,
	})
	if err := adv.SendOnce(); err == nil {
		t.Fatalf("expected write error, got nil")
	}
}

// TestAdvertiserRunTicks confirms Run emits an immediate advert plus one
// per Interval, and shuts down cleanly on ctx cancel.
func TestAdvertiserRunTicks(t *testing.T) {
	if testing.Short() {
		t.Skip("skip in -short mode")
	}
	buf := &chunkedBuffer{}
	// Use the minimum allowed interval (10s) — but for tests we want
	// something far smaller. Override via a custom field by skipping
	// validation: construct cfg with interval=10s and use SendOnce
	// instead of running the real ticker.
	//
	// Here we deliberately run Run for ~12s with the lowest valid
	// interval (10s) so we exercise the real time.Ticker path once.
	adv, err := NewAdvertiser(AdvertiserConfig{
		Writer:   buf,
		Identity: pk32(0x33),
		BWClass:  BWLow,
		Interval: 10 * time.Second,
		Load:     func() uint8 { return 1 },
	})
	if err != nil {
		t.Fatalf("NewAdvertiser: %v", err)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 12*time.Second)
	defer cancel()

	errCh := make(chan error, 1)
	go func() { errCh <- adv.Run(ctx) }()

	select {
	case err := <-errCh:
		if !errors.Is(err, context.DeadlineExceeded) {
			t.Fatalf("Run returned unexpected err: %v", err)
		}
	case <-time.After(15 * time.Second):
		t.Fatal("Run did not return within 15s")
	}

	// One immediate send + one tick at +10s = 2 envelopes.
	if got := buf.Count(); got < 2 {
		t.Fatalf("writes: got %d want >= 2", got)
	}
}
