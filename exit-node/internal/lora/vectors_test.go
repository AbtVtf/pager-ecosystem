package lora

import (
	"bytes"
	"encoding/hex"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// readVector loads a single-line hex test vector from protocol/test-vectors/lora.
func readVector(t *testing.T, name string) []byte {
	t.Helper()
	// exit-node/internal/lora → ../../../protocol/test-vectors/lora
	p := filepath.Join("..", "..", "..", "protocol", "test-vectors", "lora", name)
	raw, err := os.ReadFile(p)
	if err != nil {
		t.Fatalf("read %s: %v", name, err)
	}
	b, err := hex.DecodeString(strings.TrimSpace(string(raw)))
	if err != nil {
		t.Fatalf("decode hex %s: %v", name, err)
	}
	return b
}

// TestVectorsMatch locks the Go codec to the on-disk cross-impl vectors.
// If this fails, either the vectors or the Go codec drifted from the spec —
// fix whichever is wrong before merging.
func TestVectorsMatch(t *testing.T) {
	type pos struct {
		file string
		env  Envelope
	}
	positives := []pos{
		{"01_request_empty.hex", Envelope{Version: 1, Type: TypeRequest, MsgID: 0xCAFEBABE}},
		{"02_response_hello.hex", Envelope{Version: 1, Type: TypeResponse, MsgID: 0x00000001, Payload: []byte("hello")}},
		{"03_ack_maxid.hex", Envelope{Version: 1, Type: TypeAck, MsgID: 0xFFFFFFFF, Payload: []byte{0xDE, 0xAD, 0xBE, 0xEF}}},
		{"04_advert_run.hex", Envelope{Version: 1, Type: TypeExitNodeAdvert, MsgID: 42, Payload: bytes.Repeat([]byte{0x5A}, 200)}},
	}
	for _, p := range positives {
		t.Run(p.file, func(t *testing.T) {
			want := readVector(t, p.file)
			got, err := Encode(p.env)
			if err != nil {
				t.Fatalf("Encode: %v", err)
			}
			if !bytes.Equal(got, want) {
				t.Errorf("encode mismatch\n got  %x\n want %x", got, want)
			}
			dec, err := Decode(want)
			if err != nil {
				t.Fatalf("Decode: %v", err)
			}
			if dec.Version != p.env.Version || dec.Type != p.env.Type || dec.MsgID != p.env.MsgID || !bytes.Equal(dec.Payload, p.env.Payload) {
				t.Errorf("decode mismatch:\n got  %+v\n want %+v", dec, p.env)
			}
		})
	}

	t.Run("05_unknown_type.hex", func(t *testing.T) {
		raw := readVector(t, "05_unknown_type.hex")
		env, err := Decode(raw)
		if !errors.Is(err, ErrUnknownType) {
			t.Fatalf("want ErrUnknownType, got %v", err)
		}
		if env.Type != 0x7F || env.MsgID != 5 || !bytes.Equal(env.Payload, []byte{0x01, 0x02}) {
			t.Errorf("unexpected decode: %+v", env)
		}
	})

	t.Run("neg_01_swapped_magic.hex", func(t *testing.T) {
		if _, err := Decode(readVector(t, "neg_01_swapped_magic.hex")); !errors.Is(err, ErrBadMagic) {
			t.Fatalf("want ErrBadMagic, got %v", err)
		}
	})
	t.Run("neg_02_wrong_magic.hex", func(t *testing.T) {
		if _, err := Decode(readVector(t, "neg_02_wrong_magic.hex")); !errors.Is(err, ErrBadMagic) {
			t.Fatalf("want ErrBadMagic, got %v", err)
		}
	})
	t.Run("neg_03_short.hex", func(t *testing.T) {
		if _, err := Decode(readVector(t, "neg_03_short.hex")); !errors.Is(err, ErrShort) {
			t.Fatalf("want ErrShort, got %v", err)
		}
	})
}

// TestFragmentVectorsMatch locks the fragment-body codec to the on-disk
// cross-impl vectors (LORA-002). Each vector is the byte content that
// rides in an outer envelope's Payload field for one fragment.
func TestFragmentVectorsMatch(t *testing.T) {
	type pos struct {
		file string
		frag Fragment
	}
	positives := []pos{
		{"frag_01_single.hex", Fragment{FragID: 0, Total: 1, Data: []byte("hello")}},
		{"frag_02_first_of_three.hex", Fragment{FragID: 0, Total: 3, Data: bytes.Repeat([]byte{0xAB}, 50)}},
		{"frag_03_last_of_three.hex", Fragment{FragID: 2, Total: 3, Data: []byte{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A}}},
	}
	for _, p := range positives {
		t.Run(p.file, func(t *testing.T) {
			want := readVector(t, p.file)
			got, err := EncodeFragmentBody(p.frag)
			if err != nil {
				t.Fatalf("EncodeFragmentBody: %v", err)
			}
			if !bytes.Equal(got, want) {
				t.Errorf("encode mismatch\n got  %x\n want %x", got, want)
			}
			dec, err := DecodeFragmentBody(want)
			if err != nil {
				t.Fatalf("DecodeFragmentBody: %v", err)
			}
			if dec.FragID != p.frag.FragID || dec.Total != p.frag.Total || !bytes.Equal(dec.Data, p.frag.Data) {
				t.Errorf("decode mismatch:\n got  %+v\n want %+v", dec, p.frag)
			}
		})
	}

	t.Run("neg_frag_01_total_zero.hex", func(t *testing.T) {
		if _, err := DecodeFragmentBody(readVector(t, "neg_frag_01_total_zero.hex")); !errors.Is(err, ErrFragZeroTotal) {
			t.Fatalf("want ErrFragZeroTotal, got %v", err)
		}
	})
	t.Run("neg_frag_02_id_out_of_range.hex", func(t *testing.T) {
		if _, err := DecodeFragmentBody(readVector(t, "neg_frag_02_id_out_of_range.hex")); !errors.Is(err, ErrFragOutOfRange) {
			t.Fatalf("want ErrFragOutOfRange, got %v", err)
		}
	})
}
