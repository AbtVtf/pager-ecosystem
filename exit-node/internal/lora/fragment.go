package lora

// Fragmentation & reassembly for payloads that exceed the LoRa MTU
// (SPEC.md §6.2.3).
//
// Each fragment travels inside an outer envelope (envelope.go); the
// outer envelope's MsgID groups fragments of one logical message. The
// envelope payload of a fragment is:
//
//	┌────────────┬───────────┬──────────────┐
//	│ frag_id 1B │ total 1B  │ data (var.)  │
//	└────────────┴───────────┴──────────────┘
//
// frag_id is 0-indexed; total is the count of fragments in the message
// (1..255). A message that fits in a single packet uses total=1,
// frag_id=0 — i.e. fragmentation is always present in the wire format,
// so receivers have one code path. This keeps the format faithful to
// the spec's "numbered fragments with a frag_id and total" wording
// without inventing a separate single-packet path.

import (
	"errors"
	"fmt"
)

// FragmentHeaderSize is the fixed per-fragment header (frag_id + total).
const FragmentHeaderSize = 2

// MaxFragments is the largest message size in fragments (total is u8).
const MaxFragments = 255

// DefaultFragmentDataSize is the default per-fragment data byte count
// used by SplitMessage. Chosen to keep an envelope-wrapped fragment
// comfortably under typical LoRa packet limits (255B at SF7-12) and
// in line with SPEC.md §14's 200B Frame target. Callers MAY override
// when they know their radio parameters.
const DefaultFragmentDataSize = 200

// Fragment is one piece of a fragmented logical message.
type Fragment struct {
	// MsgID is the outer envelope MsgID that groups this fragment with
	// its siblings. Carried by the envelope, surfaced here for clarity.
	MsgID  uint32
	FragID uint8
	Total  uint8
	Data   []byte
}

// Sentinel errors.
var (
	ErrFragShort     = errors.New("lora: fragment too short")
	ErrFragZeroTotal = errors.New("lora: fragment total must be >= 1")
	ErrFragOutOfRange = errors.New("lora: fragment id >= total")
	ErrFragMismatch  = errors.New("lora: fragment total disagrees with prior fragments")
	ErrFragTooLarge  = errors.New("lora: message too large to fragment (would exceed 255 fragments)")
	ErrFragChunkSize = errors.New("lora: chunk size must be >= 1")
)

// EncodeFragmentBody serializes the fragment header + data to the bytes
// that sit in an outer envelope's Payload field. MsgID is not encoded
// here — it lives in the outer envelope.
func EncodeFragmentBody(f Fragment) ([]byte, error) {
	if f.Total == 0 {
		return nil, ErrFragZeroTotal
	}
	if f.FragID >= f.Total {
		return nil, ErrFragOutOfRange
	}
	out := make([]byte, FragmentHeaderSize+len(f.Data))
	out[0] = f.FragID
	out[1] = f.Total
	copy(out[FragmentHeaderSize:], f.Data)
	return out, nil
}

// DecodeFragmentBody parses fragment bytes from an outer envelope's
// Payload. The caller must populate MsgID from the envelope.
func DecodeFragmentBody(b []byte) (Fragment, error) {
	if len(b) < FragmentHeaderSize {
		return Fragment{}, ErrFragShort
	}
	f := Fragment{FragID: b[0], Total: b[1]}
	if f.Total == 0 {
		return f, ErrFragZeroTotal
	}
	if f.FragID >= f.Total {
		return f, ErrFragOutOfRange
	}
	if n := len(b) - FragmentHeaderSize; n > 0 {
		f.Data = make([]byte, n)
		copy(f.Data, b[FragmentHeaderSize:])
	}
	return f, nil
}

// SplitMessage splits payload into fragments of at most chunkSize bytes
// of data each. The last fragment may be shorter. MsgID is set on every
// returned fragment so callers can place each into an outer envelope
// without rederiving it.
//
// An empty payload yields one zero-length fragment (total=1, frag_id=0)
// so the wire format always carries at least one fragment.
func SplitMessage(msgID uint32, payload []byte, chunkSize int) ([]Fragment, error) {
	if chunkSize < 1 {
		return nil, ErrFragChunkSize
	}
	n := len(payload)
	if n == 0 {
		return []Fragment{{MsgID: msgID, FragID: 0, Total: 1, Data: nil}}, nil
	}
	count := (n + chunkSize - 1) / chunkSize
	if count > MaxFragments {
		return nil, fmt.Errorf("%w: %d fragments needed, max %d", ErrFragTooLarge, count, MaxFragments)
	}
	out := make([]Fragment, count)
	for i := 0; i < count; i++ {
		start := i * chunkSize
		end := start + chunkSize
		if end > n {
			end = n
		}
		// Copy so callers can mutate payload without aliasing fragments.
		data := make([]byte, end-start)
		copy(data, payload[start:end])
		out[i] = Fragment{
			MsgID:  msgID,
			FragID: uint8(i),
			Total:  uint8(count),
			Data:   data,
		}
	}
	return out, nil
}

// Reassembler accumulates fragments by MsgID and yields the full payload
// once all `total` fragments for a message have been observed.
//
// Reassembler is not safe for concurrent use.
type Reassembler struct {
	groups map[uint32]*reassemblyGroup
}

type reassemblyGroup struct {
	total  uint8
	frags  [][]byte // indexed by frag_id; nil entries are still missing
	have   int      // count of non-nil entries
}

// NewReassembler returns a ready-to-use Reassembler.
func NewReassembler() *Reassembler {
	return &Reassembler{groups: make(map[uint32]*reassemblyGroup)}
}

// Add records f. If f completes a message, Add returns the assembled
// payload and complete=true; the group is also removed from internal
// state. Duplicate fragments (same MsgID + FragID) are silently
// accepted as long as their total matches.
//
// Errors:
//   - ErrFragZeroTotal   — total=0
//   - ErrFragOutOfRange  — frag_id >= total
//   - ErrFragMismatch    — total differs from a previously-seen fragment for the same MsgID
func (r *Reassembler) Add(f Fragment) (payload []byte, complete bool, err error) {
	if f.Total == 0 {
		return nil, false, ErrFragZeroTotal
	}
	if f.FragID >= f.Total {
		return nil, false, ErrFragOutOfRange
	}
	g, ok := r.groups[f.MsgID]
	if !ok {
		g = &reassemblyGroup{total: f.Total, frags: make([][]byte, f.Total)}
		r.groups[f.MsgID] = g
	} else if g.total != f.Total {
		return nil, false, fmt.Errorf("%w: msg %#x had total=%d, now %d", ErrFragMismatch, f.MsgID, g.total, f.Total)
	}
	if g.frags[f.FragID] == nil {
		// Copy so later mutations of f.Data don't corrupt internal state.
		buf := make([]byte, len(f.Data))
		copy(buf, f.Data)
		g.frags[f.FragID] = buf
		g.have++
	}
	if g.have < int(g.total) {
		return nil, false, nil
	}
	// Complete: concatenate in order.
	size := 0
	for _, p := range g.frags {
		size += len(p)
	}
	out := make([]byte, 0, size)
	for _, p := range g.frags {
		out = append(out, p...)
	}
	delete(r.groups, f.MsgID)
	return out, true, nil
}

// Missing returns the frag_ids that have not yet arrived for msgID, or
// nil if msgID is unknown or already complete. The slice is sorted
// ascending. Callers use this to drive per-fragment retransmission
// requests (SPEC.md §6.2.3 "Lost fragments retried individually").
func (r *Reassembler) Missing(msgID uint32) []uint8 {
	g, ok := r.groups[msgID]
	if !ok {
		return nil
	}
	out := make([]uint8, 0, int(g.total)-g.have)
	for i, p := range g.frags {
		if p == nil {
			out = append(out, uint8(i))
		}
	}
	return out
}

// Drop discards any partial state for msgID. Use when a message times
// out (e.g. SPEC.md §6.4 LoRa retry exhausted) so the Reassembler does
// not leak memory.
func (r *Reassembler) Drop(msgID uint32) {
	delete(r.groups, msgID)
}

// Pending returns the number of in-flight (incomplete) messages held by
// the Reassembler. Useful for tests and metrics.
func (r *Reassembler) Pending() int {
	return len(r.groups)
}
