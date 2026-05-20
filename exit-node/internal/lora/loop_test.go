package lora

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math/rand"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// fakeTransceiver is a deterministic in-memory PacketTransceiver used
// by the loop tests. It models a packet-oriented transport with two
// bounded channels — one for inbound packets (loop will read), one for
// outbound (loop will write).
type fakeTransceiver struct {
	in     chan []byte
	out    chan []byte
	closed atomic.Bool
}

func newFakeTransceiver(buf int) *fakeTransceiver {
	return &fakeTransceiver{
		in:  make(chan []byte, buf),
		out: make(chan []byte, buf),
	}
}

func (f *fakeTransceiver) ReadPacket(ctx context.Context) ([]byte, error) {
	if f.closed.Load() {
		return nil, io.EOF
	}
	select {
	case <-ctx.Done():
		return nil, ctx.Err()
	case b, ok := <-f.in:
		if !ok {
			return nil, io.EOF
		}
		return b, nil
	}
}

func (f *fakeTransceiver) WritePacket(ctx context.Context, p []byte) error {
	if f.closed.Load() {
		return io.ErrClosedPipe
	}
	select {
	case <-ctx.Done():
		return ctx.Err()
	case f.out <- append([]byte(nil), p...):
		return nil
	}
}

func (f *fakeTransceiver) Close() error {
	if f.closed.Swap(true) {
		return nil
	}
	close(f.in)
	return nil
}

// helper: wrap a payload as one or more outer-envelope+fragment packets.
func framedRequest(t *testing.T, msgID uint32, body []byte, chunk int) [][]byte {
	t.Helper()
	frags, err := SplitMessage(msgID, body, chunk)
	if err != nil {
		t.Fatal(err)
	}
	out := make([][]byte, len(frags))
	for i, f := range frags {
		fb, err := EncodeFragmentBody(f)
		if err != nil {
			t.Fatal(err)
		}
		wire, err := Encode(Envelope{Version: 1, Type: TypeRequest, MsgID: msgID, Payload: fb})
		if err != nil {
			t.Fatal(err)
		}
		out[i] = wire
	}
	return out
}

// helper: collect N outbound packets within deadline.
func collectPackets(t *testing.T, ft *fakeTransceiver, n int, deadline time.Duration) [][]byte {
	t.Helper()
	out := make([][]byte, 0, n)
	timeout := time.After(deadline)
	for len(out) < n {
		select {
		case p := <-ft.out:
			out = append(out, p)
		case <-timeout:
			t.Fatalf("collectPackets: only got %d/%d in %v", len(out), n, deadline)
		}
	}
	return out
}

// helper: reassemble an outbound response back into its inner body.
func reassembleOutbound(t *testing.T, pkts [][]byte) (uint32, []byte) {
	t.Helper()
	r := NewReassembler()
	var msgID uint32
	for i, p := range pkts {
		env, err := Decode(p)
		if err != nil {
			t.Fatalf("decode #%d: %v", i, err)
		}
		if env.Type != TypeResponse {
			t.Fatalf("packet %d has type %#x, want Response", i, env.Type)
		}
		if i == 0 {
			msgID = env.MsgID
		} else if env.MsgID != msgID {
			t.Fatalf("packet %d msgID %#x != first %#x", i, env.MsgID, msgID)
		}
		fb, err := DecodeFragmentBody(env.Payload)
		if err != nil {
			t.Fatalf("decode frag %d: %v", i, err)
		}
		fb.MsgID = env.MsgID
		body, complete, err := r.Add(fb)
		if err != nil {
			t.Fatalf("reassemble: %v", err)
		}
		if complete {
			return env.MsgID, body
		}
	}
	t.Fatal("reassembleOutbound: never completed")
	return 0, nil
}

func TestLoopEchoesSingleFragmentRequest(t *testing.T) {
	ft := newFakeTransceiver(8)
	loop := NewLoop(ft, HandlerFunc(func(ctx context.Context, req Request) (Response, error) {
		return Response{Body: append([]byte("ECHO:"), req.Body...)}, nil
	}), LoopOpts{})
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- loop.Run(ctx) }()

	pkts := framedRequest(t, 0xCAFE, []byte("hello"), 200)
	if len(pkts) != 1 {
		t.Fatalf("expected single-fragment request, got %d", len(pkts))
	}
	ft.in <- pkts[0]

	resp := collectPackets(t, ft, 1, 500*time.Millisecond)
	msgID, body := reassembleOutbound(t, resp)
	if msgID != 0xCAFE {
		t.Errorf("response msgID = %#x, want 0xCAFE", msgID)
	}
	if !bytes.Equal(body, []byte("ECHO:hello")) {
		t.Errorf("response body = %q, want ECHO:hello", body)
	}

	ft.Close()
	<-done
}

func TestLoopReassemblesAndRefragmentsLargeMessage(t *testing.T) {
	ft := newFakeTransceiver(64)
	loop := NewLoop(ft, HandlerFunc(func(ctx context.Context, req Request) (Response, error) {
		// Echo with a length prefix; response is ~ same size as request.
		out := make([]byte, 0, len(req.Body)+4)
		var lb [4]byte
		binary.BigEndian.PutUint32(lb[:], uint32(len(req.Body)))
		out = append(out, lb[:]...)
		out = append(out, req.Body...)
		return Response{Body: out}, nil
	}), LoopOpts{FragmentChunkSize: 50})
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- loop.Run(ctx) }()

	payload := bytes.Repeat([]byte{0xAB}, 470)
	pkts := framedRequest(t, 0x4242, payload, 60) // ceil(470/60) = 8 frags
	if len(pkts) < 5 {
		t.Fatalf("expected fragmentation; got %d packets", len(pkts))
	}
	// Send out of order to also exercise the reassembler under the loop.
	rng := rand.New(rand.NewSource(7))
	rng.Shuffle(len(pkts), func(i, j int) { pkts[i], pkts[j] = pkts[j], pkts[i] })
	for _, p := range pkts {
		ft.in <- p
	}

	// Expect ceil((4+470)/50) = 10 response packets.
	expected := (4 + 470 + 49) / 50
	resp := collectPackets(t, ft, expected, 2*time.Second)
	_, body := reassembleOutbound(t, resp)
	if len(body) != 4+len(payload) {
		t.Fatalf("response len = %d, want %d", len(body), 4+len(payload))
	}
	if !bytes.Equal(body[4:], payload) {
		t.Fatal("response payload bytes do not match request")
	}
	ft.Close()
	<-done
}

func TestLoopDropsBadMagicWithoutCrashing(t *testing.T) {
	ft := newFakeTransceiver(8)
	loop := NewLoop(ft, HandlerFunc(func(ctx context.Context, req Request) (Response, error) {
		return Response{Body: []byte("ok")}, nil
	}), LoopOpts{})
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- loop.Run(ctx) }()

	ft.in <- []byte{0xFF, 0xFF, 0x01, 0x01, 0, 0, 0, 1}                // bad magic
	ft.in <- []byte{0x50, 0x47, 0x01, 0x7F, 0, 0, 0, 2, 0xAA}          // unknown type
	ft.in <- []byte{0x50, 0x47, 0x01, 0x01, 0, 0, 0, 3, 0x05, 0x01, 0xAA} // valid req but frag_id>=total (5 of 1) — dropped post-decode
	// then a good one to prove the loop is still alive
	pkts := framedRequest(t, 0xBEEF, []byte("ok"), 200)
	ft.in <- pkts[0]

	resp := collectPackets(t, ft, 1, 500*time.Millisecond)
	if id, _ := reassembleOutbound(t, resp); id != 0xBEEF {
		t.Errorf("expected response to good request, got msgID %#x", id)
	}
	stats := loop.Stats()
	if stats.PacketsDropped < 3 {
		t.Errorf("expected >=3 drops, got %d", stats.PacketsDropped)
	}
	ft.Close()
	<-done
}

func TestLoopIgnoresNonRequestTypes(t *testing.T) {
	ft := newFakeTransceiver(8)
	called := atomic.Bool{}
	loop := NewLoop(ft, HandlerFunc(func(ctx context.Context, req Request) (Response, error) {
		called.Store(true)
		return Response{Body: []byte("x")}, nil
	}), LoopOpts{})
	ctx, cancel := context.WithTimeout(context.Background(), 1*time.Second)
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- loop.Run(ctx) }()

	// Send an Ack (valid type but not Request).
	wire, _ := Encode(Envelope{Version: 1, Type: TypeAck, MsgID: 1, Payload: []byte{0x00, 0x01, 0xAA}})
	ft.in <- wire

	// Give the loop a moment to process.
	time.Sleep(50 * time.Millisecond)
	if called.Load() {
		t.Error("handler should not be called for Ack type")
	}
	ft.Close()
	<-done
	if loop.Stats().PacketsDropped == 0 {
		t.Error("Ack should have been counted as dropped")
	}
}

func TestLoopHandlerErrorDoesNotKillLoop(t *testing.T) {
	ft := newFakeTransceiver(8)
	loop := NewLoop(ft, HandlerFunc(func(ctx context.Context, req Request) (Response, error) {
		if req.MsgID == 0xFA11 {
			return Response{}, errors.New("boom")
		}
		return Response{Body: []byte("ok")}, nil
	}), LoopOpts{})
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- loop.Run(ctx) }()

	ft.in <- framedRequest(t, 0xFA11, []byte("x"), 200)[0]
	ft.in <- framedRequest(t, 0xCAFE, []byte("y"), 200)[0]

	resp := collectPackets(t, ft, 1, 500*time.Millisecond)
	if id, _ := reassembleOutbound(t, resp); id != 0xCAFE {
		t.Errorf("post-error response msgID = %#x, want 0xCAFE", id)
	}
	// HandlerErrors may not be observable yet if the failing handler
	// is still scheduled; wait briefly for it to settle.
	deadline := time.Now().Add(500 * time.Millisecond)
	for time.Now().Before(deadline) && loop.Stats().HandlerErrors == 0 {
		time.Sleep(2 * time.Millisecond)
	}
	if loop.Stats().HandlerErrors != 1 {
		t.Errorf("HandlerErrors = %d, want 1", loop.Stats().HandlerErrors)
	}
	ft.Close()
	<-done
}

func TestLoopHandlerWithEmptyResponseSendsNothing(t *testing.T) {
	ft := newFakeTransceiver(8)
	loop := NewLoop(ft, HandlerFunc(func(ctx context.Context, req Request) (Response, error) {
		return Response{}, nil // fire-and-forget
	}), LoopOpts{})
	ctx, cancel := context.WithTimeout(context.Background(), 1*time.Second)
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- loop.Run(ctx) }()

	ft.in <- framedRequest(t, 1, []byte("fire"), 200)[0]
	time.Sleep(100 * time.Millisecond)

	select {
	case p := <-ft.out:
		t.Fatalf("did not expect a TX packet, got %x", p)
	default:
	}
	if loop.Stats().MessagesAssembled != 1 {
		t.Error("message should still be counted as assembled")
	}
	ft.Close()
	<-done
}

func TestLoopReassemblyJanitorDropsStale(t *testing.T) {
	ft := newFakeTransceiver(8)
	// Inject a frozen clock we control.
	now := time.Unix(1_700_000_000, 0)
	var mu sync.Mutex
	clock := func() time.Time { mu.Lock(); defer mu.Unlock(); return now }
	loop := NewLoop(ft, HandlerFunc(func(ctx context.Context, req Request) (Response, error) {
		t.Fatal("janitor test should never reach handler")
		return Response{}, nil
	}), LoopOpts{
		ReassembleTimeout: 50 * time.Millisecond,
		JanitorInterval:   10 * time.Millisecond,
		Now:               clock,
	})
	ctx, cancel := context.WithTimeout(context.Background(), 1*time.Second)
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- loop.Run(ctx) }()

	// Send 1 of 3 fragments. The reassembler is now holding partial state.
	frags, _ := SplitMessage(0x7A1F, []byte("never completes please"), 8)
	if len(frags) < 3 {
		t.Fatalf("setup: want >=3 frags, got %d", len(frags))
	}
	first := frags[0]
	fb, _ := EncodeFragmentBody(first)
	wire, _ := Encode(Envelope{Version: 1, Type: TypeRequest, MsgID: first.MsgID, Payload: fb})
	ft.in <- wire

	// Wait until the loop has stored the partial.
	deadline := time.Now().Add(500 * time.Millisecond)
	for time.Now().Before(deadline) {
		if loop.PendingReassemblies() == 1 {
			break
		}
		time.Sleep(2 * time.Millisecond)
	}
	if loop.PendingReassemblies() != 1 {
		t.Fatalf("expected 1 pending reassembly, got %d", loop.PendingReassemblies())
	}

	// Advance clock past the timeout. The janitor should sweep it.
	mu.Lock()
	now = now.Add(1 * time.Second)
	mu.Unlock()
	deadline = time.Now().Add(500 * time.Millisecond)
	for time.Now().Before(deadline) {
		if loop.Stats().ReassemblyTimedOut == 1 {
			break
		}
		time.Sleep(2 * time.Millisecond)
	}
	if loop.Stats().ReassemblyTimedOut != 1 {
		t.Fatalf("janitor never swept; stats = %+v", loop.Stats())
	}
	if loop.PendingReassemblies() != 0 {
		t.Errorf("expected Pending=0 after sweep, got %d", loop.PendingReassemblies())
	}
	ft.Close()
	<-done
}

func TestLoopSustainedLoad(t *testing.T) {
	// Acceptance: "Survives sustained load." 500 concurrent requests
	// through a small in-flight semaphore, every one must round-trip.
	const (
		N           = 500
		maxInFlight = 4
	)
	ft := newFakeTransceiver(N * 2)
	loop := NewLoop(ft, HandlerFunc(func(ctx context.Context, req Request) (Response, error) {
		// Tiny work item; the test is about scheduling, not CPU.
		return Response{Body: append([]byte("r:"), req.Body...)}, nil
	}), LoopOpts{MaxInFlight: maxInFlight})
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- loop.Run(ctx) }()

	// Push N single-fragment requests with distinct msg_ids.
	for i := 0; i < N; i++ {
		body := []byte(fmt.Sprintf("req-%d", i))
		pkts := framedRequest(t, uint32(i+1), body, 200)
		ft.in <- pkts[0]
	}

	seen := make(map[uint32][]byte, N)
	deadline := time.After(8 * time.Second)
	for len(seen) < N {
		select {
		case p := <-ft.out:
			env, err := Decode(p)
			if err != nil {
				t.Fatal(err)
			}
			fb, err := DecodeFragmentBody(env.Payload)
			if err != nil {
				t.Fatal(err)
			}
			// All response bodies fit in one fragment, so we can read directly.
			if fb.Total != 1 || fb.FragID != 0 {
				t.Fatalf("expected single-fragment response, got frag_id=%d total=%d", fb.FragID, fb.Total)
			}
			if _, dup := seen[env.MsgID]; dup {
				t.Fatalf("duplicate response for msg %#x", env.MsgID)
			}
			seen[env.MsgID] = fb.Data
		case <-deadline:
			t.Fatalf("only got %d/%d responses in 8s", len(seen), N)
		}
	}
	for i := 0; i < N; i++ {
		want := []byte(fmt.Sprintf("r:req-%d", i))
		if !bytes.Equal(seen[uint32(i+1)], want) {
			t.Errorf("msg %d: got %q want %q", i+1, seen[uint32(i+1)], want)
		}
	}
	if loop.Stats().PacketsReceived != N {
		t.Errorf("PacketsReceived = %d, want %d", loop.Stats().PacketsReceived, N)
	}
	if loop.Stats().PacketsSent != N {
		t.Errorf("PacketsSent = %d, want %d", loop.Stats().PacketsSent, N)
	}
	if loop.Stats().HandlerErrors != 0 {
		t.Errorf("HandlerErrors = %d, want 0", loop.Stats().HandlerErrors)
	}
	ft.Close()
	<-done
}

func TestLoopReturnsOnEOF(t *testing.T) {
	ft := newFakeTransceiver(1)
	loop := NewLoop(ft, HandlerFunc(func(ctx context.Context, req Request) (Response, error) {
		return Response{}, nil
	}), LoopOpts{})
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- loop.Run(ctx) }()
	ft.Close()
	select {
	case err := <-done:
		if err != nil {
			t.Errorf("Run on EOF returned %v, want nil", err)
		}
	case <-time.After(time.Second):
		t.Fatal("Run did not return after EOF")
	}
}

func TestLoopReturnsOnContextCancel(t *testing.T) {
	ft := newFakeTransceiver(1)
	loop := NewLoop(ft, HandlerFunc(func(ctx context.Context, req Request) (Response, error) {
		return Response{}, nil
	}), LoopOpts{})
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- loop.Run(ctx) }()
	cancel()
	select {
	case err := <-done:
		if !errors.Is(err, context.Canceled) {
			t.Errorf("Run on cancel returned %v, want context.Canceled", err)
		}
	case <-time.After(time.Second):
		t.Fatal("Run did not return after cancel")
	}
	ft.Close()
}
