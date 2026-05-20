package lora

import (
	"bytes"
	"errors"
	"math/rand"
	"reflect"
	"testing"
)

func TestFragmentBodyRoundTrip(t *testing.T) {
	cases := []Fragment{
		{FragID: 0, Total: 1, Data: nil},
		{FragID: 0, Total: 1, Data: []byte("solo")},
		{FragID: 0, Total: 3, Data: bytes.Repeat([]byte{0xAB}, 50)},
		{FragID: 2, Total: 3, Data: []byte{0x00, 0xFF}},
		{FragID: 254, Total: 255, Data: []byte{0xDE, 0xAD}},
	}
	for _, tc := range cases {
		b, err := EncodeFragmentBody(tc)
		if err != nil {
			t.Fatalf("encode %+v: %v", tc, err)
		}
		got, err := DecodeFragmentBody(b)
		if err != nil {
			t.Fatalf("decode %+v: %v", tc, err)
		}
		if got.FragID != tc.FragID || got.Total != tc.Total || !bytes.Equal(got.Data, tc.Data) {
			t.Errorf("round-trip mismatch: got %+v want %+v", got, tc)
		}
	}
}

func TestEncodeFragmentBodyRejectsInvalid(t *testing.T) {
	if _, err := EncodeFragmentBody(Fragment{Total: 0}); !errors.Is(err, ErrFragZeroTotal) {
		t.Errorf("total=0: want ErrFragZeroTotal, got %v", err)
	}
	if _, err := EncodeFragmentBody(Fragment{FragID: 5, Total: 3}); !errors.Is(err, ErrFragOutOfRange) {
		t.Errorf("frag_id>=total: want ErrFragOutOfRange, got %v", err)
	}
}

func TestDecodeFragmentBodyRejectsInvalid(t *testing.T) {
	if _, err := DecodeFragmentBody(nil); !errors.Is(err, ErrFragShort) {
		t.Errorf("nil: want ErrFragShort, got %v", err)
	}
	if _, err := DecodeFragmentBody([]byte{0x00}); !errors.Is(err, ErrFragShort) {
		t.Errorf("1 byte: want ErrFragShort, got %v", err)
	}
	if _, err := DecodeFragmentBody([]byte{0x00, 0x00}); !errors.Is(err, ErrFragZeroTotal) {
		t.Errorf("total=0: want ErrFragZeroTotal, got %v", err)
	}
	if _, err := DecodeFragmentBody([]byte{0x05, 0x03, 0xAA}); !errors.Is(err, ErrFragOutOfRange) {
		t.Errorf("frag_id>=total: want ErrFragOutOfRange, got %v", err)
	}
}

func TestDecodeFragmentBodyCopiesData(t *testing.T) {
	raw := []byte{0x00, 0x01, 0xAA, 0xBB}
	f, err := DecodeFragmentBody(raw)
	if err != nil {
		t.Fatal(err)
	}
	raw[2] = 0x00
	if f.Data[0] != 0xAA {
		t.Errorf("Data aliased input buffer: %x", f.Data)
	}
}

func TestEncodeFragmentBodyWireLayout(t *testing.T) {
	// Locks the on-wire layout for cross-impl conformance.
	b, err := EncodeFragmentBody(Fragment{FragID: 1, Total: 3, Data: []byte{0xAA, 0xBB, 0xCC}})
	if err != nil {
		t.Fatal(err)
	}
	want := []byte{0x01, 0x03, 0xAA, 0xBB, 0xCC}
	if !bytes.Equal(b, want) {
		t.Fatalf("wire layout: got %x want %x", b, want)
	}
}

func TestSplitMessageSingleFragment(t *testing.T) {
	frags, err := SplitMessage(0xCAFEBABE, []byte("short"), 100)
	if err != nil {
		t.Fatal(err)
	}
	if len(frags) != 1 {
		t.Fatalf("want 1 fragment, got %d", len(frags))
	}
	got := frags[0]
	if got.MsgID != 0xCAFEBABE || got.FragID != 0 || got.Total != 1 || !bytes.Equal(got.Data, []byte("short")) {
		t.Errorf("frag: %+v", got)
	}
}

func TestSplitMessageEmpty(t *testing.T) {
	frags, err := SplitMessage(1, nil, 50)
	if err != nil {
		t.Fatal(err)
	}
	if len(frags) != 1 {
		t.Fatalf("want 1 fragment for empty payload, got %d", len(frags))
	}
	if frags[0].FragID != 0 || frags[0].Total != 1 || len(frags[0].Data) != 0 {
		t.Errorf("empty frag: %+v", frags[0])
	}
}

func TestSplitMessageMultipleFragments(t *testing.T) {
	payload := bytes.Repeat([]byte{0xAB}, 250)
	frags, err := SplitMessage(42, payload, 100)
	if err != nil {
		t.Fatal(err)
	}
	if len(frags) != 3 {
		t.Fatalf("want 3 fragments, got %d", len(frags))
	}
	for i, f := range frags {
		if f.MsgID != 42 || f.FragID != uint8(i) || f.Total != 3 {
			t.Errorf("frag %d header: %+v", i, f)
		}
	}
	if len(frags[0].Data) != 100 || len(frags[1].Data) != 100 || len(frags[2].Data) != 50 {
		t.Errorf("data lens: %d %d %d", len(frags[0].Data), len(frags[1].Data), len(frags[2].Data))
	}
}

func TestSplitMessageExactBoundary(t *testing.T) {
	// Exactly chunkSize → still one fragment.
	frags, _ := SplitMessage(1, bytes.Repeat([]byte{0xAB}, 100), 100)
	if len(frags) != 1 {
		t.Errorf("100/100: want 1 frag, got %d", len(frags))
	}
	// chunkSize+1 → two fragments, last one byte.
	frags, _ = SplitMessage(1, bytes.Repeat([]byte{0xAB}, 101), 100)
	if len(frags) != 2 || len(frags[1].Data) != 1 {
		t.Errorf("101/100: want 2 frags last len 1, got %d (last=%d)", len(frags), len(frags[1].Data))
	}
}

func TestSplitMessageRejectsBadChunkSize(t *testing.T) {
	if _, err := SplitMessage(1, []byte("x"), 0); !errors.Is(err, ErrFragChunkSize) {
		t.Errorf("chunkSize=0: want ErrFragChunkSize, got %v", err)
	}
	if _, err := SplitMessage(1, []byte("x"), -5); !errors.Is(err, ErrFragChunkSize) {
		t.Errorf("chunkSize<0: want ErrFragChunkSize, got %v", err)
	}
}

func TestSplitMessageRejectsOversizedMessage(t *testing.T) {
	// 256 chunks of 1 byte → exceeds MaxFragments (255).
	_, err := SplitMessage(1, bytes.Repeat([]byte{0xAB}, 256), 1)
	if !errors.Is(err, ErrFragTooLarge) {
		t.Fatalf("want ErrFragTooLarge, got %v", err)
	}
	// 255 of 1 → exactly at the limit, must succeed.
	frags, err := SplitMessage(1, bytes.Repeat([]byte{0xAB}, 255), 1)
	if err != nil || len(frags) != 255 {
		t.Fatalf("at-limit split: err=%v len=%d", err, len(frags))
	}
}

func TestSplitMessageCopiesPayload(t *testing.T) {
	payload := []byte{0xAA, 0xBB, 0xCC}
	frags, _ := SplitMessage(1, payload, 2)
	payload[0] = 0x00
	if frags[0].Data[0] != 0xAA {
		t.Errorf("Data aliased input: %x", frags[0].Data)
	}
}

func TestReassembleInOrder(t *testing.T) {
	payload := bytes.Repeat([]byte{0xAB}, 250)
	frags, _ := SplitMessage(7, payload, 100)
	r := NewReassembler()
	var full []byte
	var complete bool
	for i, f := range frags {
		full, complete, _ = r.Add(f)
		if i < len(frags)-1 && complete {
			t.Fatalf("complete=true too early at i=%d", i)
		}
	}
	if !complete {
		t.Fatalf("never completed")
	}
	if !bytes.Equal(full, payload) {
		t.Fatalf("reassembled mismatch")
	}
	if r.Pending() != 0 {
		t.Errorf("expected pending=0 after completion, got %d", r.Pending())
	}
}

func TestReassembleOutOfOrder(t *testing.T) {
	payload := bytes.Repeat([]byte{0xAB}, 1000)
	for i := range payload {
		payload[i] = byte(i)
	}
	frags, _ := SplitMessage(99, payload, 60) // ceil(1000/60) = 17 fragments
	if len(frags) != 17 {
		t.Fatalf("setup: want 17 frags, got %d", len(frags))
	}
	r := NewReassembler()
	// Shuffle deterministically.
	rng := rand.New(rand.NewSource(1))
	rng.Shuffle(len(frags), func(i, j int) { frags[i], frags[j] = frags[j], frags[i] })
	var full []byte
	var complete bool
	for _, f := range frags {
		full, complete, _ = r.Add(f)
	}
	if !complete {
		t.Fatalf("did not complete")
	}
	if !bytes.Equal(full, payload) {
		t.Fatalf("out-of-order reassembly produced wrong bytes")
	}
}

func TestReassembleDuplicatesIgnored(t *testing.T) {
	frags, _ := SplitMessage(5, bytes.Repeat([]byte{0xAB}, 200), 80) // 3 frags
	r := NewReassembler()
	// Send frag 0 twice, then 1, 2.
	if _, complete, _ := r.Add(frags[0]); complete {
		t.Fatal("complete after 1")
	}
	if _, complete, _ := r.Add(frags[0]); complete {
		t.Fatal("complete after duplicate of 0")
	}
	if _, complete, _ := r.Add(frags[1]); complete {
		t.Fatal("complete after 2")
	}
	full, complete, _ := r.Add(frags[2])
	if !complete {
		t.Fatal("did not complete after 3")
	}
	if len(full) != 200 {
		t.Errorf("len: %d", len(full))
	}
}

func TestReassembleDuplicateDoesNotOverwrite(t *testing.T) {
	// Make sure a later duplicate with mutated Data doesn't corrupt
	// the already-stored fragment.
	frags, _ := SplitMessage(5, bytes.Repeat([]byte{0xAB}, 200), 80)
	r := NewReassembler()
	r.Add(frags[0])
	dup := frags[0]
	dup.Data = bytes.Repeat([]byte{0xFF}, len(dup.Data))
	r.Add(dup)
	r.Add(frags[1])
	full, _, _ := r.Add(frags[2])
	// First chunk should still be the original 0xAB bytes.
	for i := 0; i < 80; i++ {
		if full[i] != 0xAB {
			t.Fatalf("duplicate overwrote stored fragment at byte %d: %x", i, full[i])
			break
		}
	}
}

func TestReassembleRejectsMismatchedTotal(t *testing.T) {
	r := NewReassembler()
	r.Add(Fragment{MsgID: 1, FragID: 0, Total: 3, Data: []byte{0xAA}})
	_, _, err := r.Add(Fragment{MsgID: 1, FragID: 1, Total: 4, Data: []byte{0xBB}})
	if !errors.Is(err, ErrFragMismatch) {
		t.Errorf("want ErrFragMismatch, got %v", err)
	}
}

func TestReassembleRejectsInvalidFragment(t *testing.T) {
	r := NewReassembler()
	if _, _, err := r.Add(Fragment{MsgID: 1, FragID: 0, Total: 0}); !errors.Is(err, ErrFragZeroTotal) {
		t.Errorf("total=0: want ErrFragZeroTotal, got %v", err)
	}
	if _, _, err := r.Add(Fragment{MsgID: 1, FragID: 5, Total: 3}); !errors.Is(err, ErrFragOutOfRange) {
		t.Errorf("frag_id>=total: want ErrFragOutOfRange, got %v", err)
	}
}

func TestReassembleMissing(t *testing.T) {
	frags, _ := SplitMessage(11, bytes.Repeat([]byte{0xAB}, 250), 100) // 3 frags
	r := NewReassembler()
	r.Add(frags[0])
	r.Add(frags[2])
	missing := r.Missing(11)
	if !reflect.DeepEqual(missing, []uint8{1}) {
		t.Errorf("missing: got %v want [1]", missing)
	}
	// Unknown msgID → nil.
	if got := r.Missing(999); got != nil {
		t.Errorf("unknown msgID: want nil, got %v", got)
	}
	// After completion the group is gone.
	r.Add(frags[1])
	if got := r.Missing(11); got != nil {
		t.Errorf("post-complete: want nil, got %v", got)
	}
}

func TestReassembleDropFreesState(t *testing.T) {
	r := NewReassembler()
	r.Add(Fragment{MsgID: 7, FragID: 0, Total: 2, Data: []byte{0xAA}})
	if r.Pending() != 1 {
		t.Fatalf("pending: %d", r.Pending())
	}
	r.Drop(7)
	if r.Pending() != 0 {
		t.Errorf("after drop: %d", r.Pending())
	}
	// Dropping unknown msgID is a no-op.
	r.Drop(7)
}

func TestReassembleMultipleMessagesInterleaved(t *testing.T) {
	a, _ := SplitMessage(100, []byte("messageA"), 3)
	b, _ := SplitMessage(200, []byte("XXmessageB"), 4)
	r := NewReassembler()
	// Interleave.
	order := []Fragment{a[0], b[0], a[1], b[2], a[2], b[1]}
	for _, f := range order {
		r.Add(f)
	}
	if r.Pending() != 0 {
		t.Errorf("pending after a complete, b 3/3: %d", r.Pending())
	}
	// Reassemble check: re-run and capture.
	r2 := NewReassembler()
	var fullA, fullB []byte
	for _, f := range a {
		fullA, _, _ = r2.Add(f)
	}
	for _, f := range b {
		fullB, _, _ = r2.Add(f)
	}
	if string(fullA) != "messageA" {
		t.Errorf("A: %q", fullA)
	}
	if string(fullB) != "XXmessageB" {
		t.Errorf("B: %q", fullB)
	}
}

func TestSplitAndReassembleViaEnvelope(t *testing.T) {
	// End-to-end through the outer envelope codec to prove the two
	// layers compose cleanly. This is the path a real exit node /
	// firmware will take.
	payload := bytes.Repeat([]byte{0xAB}, 500)
	for i := range payload {
		payload[i] = byte(i * 7)
	}
	const msgID uint32 = 0xDEADBEEF
	frags, err := SplitMessage(msgID, payload, 120)
	if err != nil {
		t.Fatal(err)
	}
	// Encode each fragment into an outer envelope wire packet.
	wire := make([][]byte, len(frags))
	for i, f := range frags {
		body, err := EncodeFragmentBody(f)
		if err != nil {
			t.Fatalf("encode frag %d: %v", i, err)
		}
		env := Envelope{Version: 1, Type: TypeRequest, MsgID: f.MsgID, Payload: body}
		raw, err := Encode(env)
		if err != nil {
			t.Fatalf("encode env %d: %v", i, err)
		}
		wire[i] = raw
	}
	// Decode + reassemble.
	r := NewReassembler()
	var full []byte
	var complete bool
	for _, raw := range wire {
		env, err := Decode(raw)
		if err != nil {
			t.Fatalf("decode env: %v", err)
		}
		f, err := DecodeFragmentBody(env.Payload)
		if err != nil {
			t.Fatalf("decode frag: %v", err)
		}
		f.MsgID = env.MsgID
		full, complete, err = r.Add(f)
		if err != nil {
			t.Fatalf("reassemble: %v", err)
		}
	}
	if !complete {
		t.Fatal("did not complete")
	}
	if !bytes.Equal(full, payload) {
		t.Fatalf("end-to-end payload mismatch (lens %d vs %d)", len(full), len(payload))
	}
}
