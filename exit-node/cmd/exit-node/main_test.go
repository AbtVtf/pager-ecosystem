package main

import (
	"bytes"
	"context"
	"errors"
	"io"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/pageros/pager-ecosystem/exit-node/internal/config"
	"github.com/pageros/pager-ecosystem/exit-node/internal/lora"
)

// captureWriter records every Write call so the test can decode the
// emitted envelope without touching real hardware.
type captureWriter struct {
	mu     sync.Mutex
	writes [][]byte
}

func (w *captureWriter) Write(p []byte) (int, error) {
	w.mu.Lock()
	defer w.mu.Unlock()
	cp := make([]byte, len(p))
	copy(cp, p)
	w.writes = append(w.writes, cp)
	return len(p), nil
}

func (w *captureWriter) First() []byte {
	w.mu.Lock()
	defer w.mu.Unlock()
	if len(w.writes) == 0 {
		return nil
	}
	return w.writes[0]
}

// TestRunAdvertiseEmitsExpectedEnvelope confirms EXIT-005 wires the
// configured pubkey + bw_class through to a real LoRa envelope.
func TestRunAdvertiseEmitsExpectedEnvelope(t *testing.T) {
	cfg := config.Advertise{
		Enabled:           true,
		IntervalSec:       30,
		BWClass:           "medium",
		IdentityPubkeyHex: strings.Repeat("ab", 32),
	}

	w := &captureWriter{}
	ctx, cancel := context.WithCancel(context.Background())
	// Cancel as soon as the immediate first send lands.
	go func() {
		deadline := time.Now().Add(2 * time.Second)
		for time.Now().Before(deadline) {
			if w.First() != nil {
				cancel()
				return
			}
			time.Sleep(5 * time.Millisecond)
		}
		cancel()
	}()

	if err := runAdvertise(ctx, cfg, w); err != nil && !errors.Is(err, context.Canceled) {
		t.Fatalf("runAdvertise: %v", err)
	}

	pkt := w.First()
	if pkt == nil {
		t.Fatal("no envelope emitted")
	}
	env, err := lora.Decode(pkt)
	if err != nil {
		t.Fatalf("Decode envelope: %v", err)
	}
	if env.Type != lora.TypeExitNodeAdvert {
		t.Errorf("envelope type: got %v want TypeExitNodeAdvert", env.Type)
	}
	a, err := lora.DecodeAdvert(env.Payload)
	if err != nil {
		t.Fatalf("DecodeAdvert: %v", err)
	}
	if a.BWClass != lora.BWMedium {
		t.Errorf("bw_class: got %d want %d", a.BWClass, lora.BWMedium)
	}
	want := bytes.Repeat([]byte{0xAB}, 32)
	if !bytes.Equal(a.Pubkey[:], want) {
		t.Errorf("pubkey mismatch:\n got  %x\n want %x", a.Pubkey, want)
	}
	if a.Load != 0 {
		t.Errorf("load: got %d want 0 (no source wired yet)", a.Load)
	}
}

// TestRunAdvertiseRejectsBadPubkey ensures DecodePubkey errors surface
// before NewAdvertiser is called.
func TestRunAdvertiseRejectsBadPubkey(t *testing.T) {
	cfg := config.Advertise{
		Enabled:           true,
		IntervalSec:       30,
		BWClass:           "low",
		IdentityPubkeyHex: "not-hex",
	}
	err := runAdvertise(context.Background(), cfg, io.Discard)
	if err == nil {
		t.Fatal("expected error from bad pubkey, got nil")
	}
	if !strings.Contains(err.Error(), "identity_pubkey_hex") {
		t.Errorf("error not about pubkey: %v", err)
	}
}
