package lora

// LoRa RX/TX loop for the Exit Node (EXIT-002).
//
// This file connects the on-wire primitives (envelope codec — LORA-001,
// fragmentation — LORA-002, inner envelope — LORA-003) to a request
// handler. The loop:
//
//   - reads packets from a PacketTransceiver,
//   - decodes the outer LoRa envelope and drops malformed / unknown
//     packets without killing the loop,
//   - reassembles fragments by outer MsgID,
//   - delivers the assembled inner-envelope bytes to a Handler,
//   - fragments the handler's response and writes each piece back
//     under the same MsgID (the device correlates by MsgID per
//     LORA-001),
//   - times out stale reassembly groups so memory does not grow
//     unbounded under loss,
//   - and survives sustained load — the loop never blocks indefinitely
//     on a slow handler because RX work is bounded by MaxInFlight.
//
// The transport layer for fragment retransmission lives above this
// loop (it can drive retries by polling Reassembler.Missing); EXIT-002's
// acceptance is the loop itself, not the retry policy.

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log"
	"sync"
	"sync/atomic"
	"time"
)

// PacketTransceiver is a packet-oriented LoRa abstraction. Production
// implementations wrap a USB LoRa modem with a framing protocol
// (length-prefix or COBS depending on the modem firmware); tests use
// an in-memory channel pair. The exit-node Loop does not care which.
type PacketTransceiver interface {
	// ReadPacket blocks until a packet arrives, ctx is cancelled, or
	// the underlying transport closes. On close it returns io.EOF.
	ReadPacket(ctx context.Context) ([]byte, error)
	// WritePacket transmits one packet. It MAY block briefly while
	// the radio is busy; callers should not assume zero-latency.
	WritePacket(ctx context.Context, pkt []byte) error
	// Close releases the transport. ReadPacket then returns io.EOF.
	Close() error
}

// Handler processes a reassembled inbound request. Body is the
// reassembled inner-envelope CBOR bytes (still encrypted — the handler
// is expected to call DecodeInner / OpenInner if it wants the
// plaintext; EXIT-003 will do that). MsgID is the outer-envelope ID
// the response will be sent back under.
//
// A handler that returns (nil, nil) signals "no response" — used by
// e.g. fire-and-forget message types.
type Handler interface {
	Handle(ctx context.Context, req Request) (Response, error)
}

// HandlerFunc is the func-typed adapter to Handler.
type HandlerFunc func(ctx context.Context, req Request) (Response, error)

// Handle implements Handler.
func (f HandlerFunc) Handle(ctx context.Context, req Request) (Response, error) {
	return f(ctx, req)
}

// Request is the input to a Handler.
type Request struct {
	MsgID uint32
	Body  []byte // reassembled inner-envelope CBOR (LORA-003)
}

// Response is the output of a Handler.
type Response struct {
	// Body is the inner-envelope CBOR bytes to send back. The loop
	// fragments and outer-envelope-wraps it. An empty Body skips TX.
	Body []byte
}

// LoopOpts configures a Loop.
type LoopOpts struct {
	// MaxInFlight bounds the number of handler goroutines that may
	// run concurrently. Defaults to 16. Set higher for high-throughput
	// nodes; lower for memory-constrained ones.
	MaxInFlight int
	// FragmentChunkSize is the chunk size used for outbound
	// fragmentation. Defaults to DefaultFragmentDataSize (200 B).
	FragmentChunkSize int
	// ReassembleTimeout is how long a partial inbound message stays
	// in the Reassembler before being dropped. Defaults to 60 s
	// (matches SPEC.md §6.4 LoRa retry budget).
	ReassembleTimeout time.Duration
	// JanitorInterval controls how often the reassembly janitor runs.
	// Defaults to ReassembleTimeout / 2.
	JanitorInterval time.Duration
	// Logger is used for non-fatal diagnostics (malformed packets,
	// dropped fragments, handler errors). nil disables logging.
	Logger *log.Logger
	// Now lets tests inject a clock. Defaults to time.Now.
	Now func() time.Time
}

func (o *LoopOpts) withDefaults() {
	if o.MaxInFlight <= 0 {
		o.MaxInFlight = 16
	}
	if o.FragmentChunkSize <= 0 {
		o.FragmentChunkSize = DefaultFragmentDataSize
	}
	if o.ReassembleTimeout <= 0 {
		o.ReassembleTimeout = 60 * time.Second
	}
	if o.JanitorInterval <= 0 {
		o.JanitorInterval = o.ReassembleTimeout / 2
		if o.JanitorInterval <= 0 {
			o.JanitorInterval = time.Second
		}
	}
	if o.Now == nil {
		o.Now = time.Now
	}
}

// Stats is a snapshot of Loop counters. Useful for /metrics, the
// EXIT-007 stats publisher, and load tests.
type Stats struct {
	PacketsReceived  uint64 // outer-envelope packets read from the wire
	PacketsDropped   uint64 // bad magic / unknown type / decode error / handler-side errors
	PacketsSent      uint64 // outer-envelope packets written to the wire
	MessagesAssembled uint64 // reassembled inbound messages handed to a Handler
	ReassemblyTimedOut uint64 // partial reassemblies dropped by the janitor
	HandlerErrors    uint64
}

// Loop drives a LoRa transceiver, handling fragmentation, reassembly,
// and response transmission. Construct with NewLoop; run with Run.
type Loop struct {
	tx      PacketTransceiver
	h       Handler
	opts    LoopOpts
	reasm   *Reassembler
	reasmMu sync.Mutex
	// pendingAt[msgID] = first-seen time, used by the janitor.
	pendingAt map[uint32]time.Time

	// Counters (atomic).
	cntReceived         atomic.Uint64
	cntDropped          atomic.Uint64
	cntSent             atomic.Uint64
	cntAssembled        atomic.Uint64
	cntReassembleTimeout atomic.Uint64
	cntHandlerErrors    atomic.Uint64

	// Backpressure semaphore for in-flight handlers.
	sem chan struct{}
}

// NewLoop returns a Loop wired to the given transceiver and handler.
func NewLoop(tx PacketTransceiver, h Handler, opts LoopOpts) *Loop {
	opts.withDefaults()
	return &Loop{
		tx:        tx,
		h:         h,
		opts:      opts,
		reasm:     NewReassembler(),
		pendingAt: make(map[uint32]time.Time),
		sem:       make(chan struct{}, opts.MaxInFlight),
	}
}

// PendingReassemblies returns the number of partial inbound messages
// currently held by the loop's Reassembler. Useful for tests and
// memory-pressure metrics. Safe for concurrent use.
func (l *Loop) PendingReassemblies() int {
	l.reasmMu.Lock()
	defer l.reasmMu.Unlock()
	return l.reasm.Pending()
}

// Stats returns a snapshot of the loop's counters.
func (l *Loop) Stats() Stats {
	return Stats{
		PacketsReceived:    l.cntReceived.Load(),
		PacketsDropped:     l.cntDropped.Load(),
		PacketsSent:        l.cntSent.Load(),
		MessagesAssembled:  l.cntAssembled.Load(),
		ReassemblyTimedOut: l.cntReassembleTimeout.Load(),
		HandlerErrors:      l.cntHandlerErrors.Load(),
	}
}

// Run blocks until ctx is cancelled or the transceiver closes (EOF).
// It returns ctx.Err() on cancellation, nil on EOF, or the underlying
// read error otherwise.
//
// Run is safe to call exactly once per Loop.
func (l *Loop) Run(ctx context.Context) error {
	var wg sync.WaitGroup
	defer wg.Wait()

	janitorCtx, cancelJanitor := context.WithCancel(ctx)
	defer cancelJanitor()

	wg.Add(1)
	go func() {
		defer wg.Done()
		l.runJanitor(janitorCtx)
	}()

	for {
		pkt, err := l.tx.ReadPacket(ctx)
		if err != nil {
			if errors.Is(err, io.EOF) {
				return nil
			}
			if errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) {
				return ctx.Err()
			}
			return fmt.Errorf("lora.Loop: read: %w", err)
		}
		l.cntReceived.Add(1)
		l.handlePacket(ctx, &wg, pkt)
	}
}

// handlePacket processes one inbound packet. It decodes the outer
// envelope, drops malformed packets, and either reassembles or
// dispatches the message.
func (l *Loop) handlePacket(ctx context.Context, wg *sync.WaitGroup, pkt []byte) {
	env, err := Decode(pkt)
	if err != nil {
		if errors.Is(err, ErrUnknownType) {
			l.logf("loop: unknown outer type 0x%02x, dropping", env.Type)
			l.cntDropped.Add(1)
			return
		}
		l.logf("loop: bad outer envelope: %v", err)
		l.cntDropped.Add(1)
		return
	}
	// We only act on inbound Requests. Ack and ExitNodeAdvert types are
	// out of scope for EXIT-002 (LORA-005 handles advert TX; ACKs are
	// not in v1 — the protocol relies on response packets for
	// confirmation per SPEC.md §6.2.1).
	if env.Type != TypeRequest {
		l.logf("loop: ignoring non-request type 0x%02x", env.Type)
		l.cntDropped.Add(1)
		return
	}
	frag, err := DecodeFragmentBody(env.Payload)
	if err != nil {
		l.logf("loop: bad fragment header: %v", err)
		l.cntDropped.Add(1)
		return
	}
	frag.MsgID = env.MsgID

	l.reasmMu.Lock()
	if _, seen := l.pendingAt[frag.MsgID]; !seen {
		l.pendingAt[frag.MsgID] = l.opts.Now()
	}
	full, complete, err := l.reasm.Add(frag)
	if complete || err != nil {
		delete(l.pendingAt, frag.MsgID)
	}
	l.reasmMu.Unlock()

	if err != nil {
		l.logf("loop: reassemble: %v", err)
		l.cntDropped.Add(1)
		return
	}
	if !complete {
		return
	}

	l.cntAssembled.Add(1)
	l.dispatch(ctx, wg, frag.MsgID, full)
}

// dispatch schedules a Handler call on a goroutine, respecting the
// MaxInFlight semaphore so we never blow up under load.
func (l *Loop) dispatch(ctx context.Context, wg *sync.WaitGroup, msgID uint32, body []byte) {
	select {
	case l.sem <- struct{}{}:
	case <-ctx.Done():
		return
	}
	wg.Add(1)
	go func() {
		defer wg.Done()
		defer func() { <-l.sem }()
		req := Request{MsgID: msgID, Body: body}
		resp, err := l.h.Handle(ctx, req)
		if err != nil {
			l.cntHandlerErrors.Add(1)
			l.logf("loop: handler error for msg %#x: %v", msgID, err)
			return
		}
		if len(resp.Body) == 0 {
			return
		}
		if err := l.sendResponse(ctx, msgID, resp.Body); err != nil {
			l.logf("loop: send response for msg %#x: %v", msgID, err)
		}
	}()
}

// sendResponse fragments resp and writes each piece back under the
// request's MsgID and TypeResponse. The recipient correlates by MsgID.
func (l *Loop) sendResponse(ctx context.Context, msgID uint32, body []byte) error {
	frags, err := SplitMessage(msgID, body, l.opts.FragmentChunkSize)
	if err != nil {
		return fmt.Errorf("split: %w", err)
	}
	for _, f := range frags {
		fragBody, err := EncodeFragmentBody(f)
		if err != nil {
			return fmt.Errorf("encode frag: %w", err)
		}
		wire, err := Encode(Envelope{
			Version: 1,
			Type:    TypeResponse,
			MsgID:   msgID,
			Payload: fragBody,
		})
		if err != nil {
			return fmt.Errorf("encode envelope: %w", err)
		}
		if err := l.tx.WritePacket(ctx, wire); err != nil {
			return fmt.Errorf("write packet: %w", err)
		}
		l.cntSent.Add(1)
	}
	return nil
}

// runJanitor periodically drops reassembly groups older than
// ReassembleTimeout. Without this, lost fragments would leak memory.
func (l *Loop) runJanitor(ctx context.Context) {
	t := time.NewTicker(l.opts.JanitorInterval)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			l.sweepReassemblies()
		}
	}
}

func (l *Loop) sweepReassemblies() {
	cutoff := l.opts.Now().Add(-l.opts.ReassembleTimeout)
	l.reasmMu.Lock()
	defer l.reasmMu.Unlock()
	for id, t := range l.pendingAt {
		if t.Before(cutoff) {
			l.reasm.Drop(id)
			delete(l.pendingAt, id)
			l.cntReassembleTimeout.Add(1)
		}
	}
}

func (l *Loop) logf(format string, args ...any) {
	if l.opts.Logger == nil {
		return
	}
	l.opts.Logger.Printf(format, args...)
}
