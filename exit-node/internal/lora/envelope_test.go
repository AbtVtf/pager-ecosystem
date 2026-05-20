package lora

import (
	"bytes"
	"errors"
	"testing"
)

func TestRoundTrip(t *testing.T) {
	cases := []struct {
		name string
		env  Envelope
	}{
		{"request, empty payload", Envelope{Version: 1, Type: TypeRequest, MsgID: 0xCAFEBABE}},
		{"response, short payload", Envelope{Version: 1, Type: TypeResponse, MsgID: 0x00000001, Payload: []byte("hello")}},
		{"ack, max u32 id", Envelope{Version: 1, Type: TypeAck, MsgID: 0xFFFFFFFF, Payload: []byte{0xDE, 0xAD, 0xBE, 0xEF}}},
		{"advert, larger payload", Envelope{Version: 1, Type: TypeExitNodeAdvert, MsgID: 42, Payload: bytes.Repeat([]byte{0x5A}, 200)}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			b, err := Encode(tc.env)
			if err != nil {
				t.Fatalf("Encode: %v", err)
			}
			got, err := Decode(b)
			if err != nil {
				t.Fatalf("Decode: %v", err)
			}
			if got.Version != tc.env.Version || got.Type != tc.env.Type || got.MsgID != tc.env.MsgID {
				t.Errorf("header mismatch: got %+v want %+v", got, tc.env)
			}
			if !bytes.Equal(got.Payload, tc.env.Payload) {
				t.Errorf("payload mismatch: got %x want %x", got.Payload, tc.env.Payload)
			}
		})
	}
}

func TestEncodeWireLayout(t *testing.T) {
	// Locks the byte layout against the spec so cross-impl test vectors stay valid.
	b, err := Encode(Envelope{Version: 1, Type: TypeRequest, MsgID: 0x01020304, Payload: []byte{0xAA, 0xBB}})
	if err != nil {
		t.Fatalf("Encode: %v", err)
	}
	want := []byte{0x50, 0x47, 0x01, 0x01, 0x01, 0x02, 0x03, 0x04, 0xAA, 0xBB}
	if !bytes.Equal(b, want) {
		t.Fatalf("wire layout mismatch:\n got  %x\n want %x", b, want)
	}
}

func TestBadMagicRejected(t *testing.T) {
	good, _ := Encode(Envelope{Version: 1, Type: TypeRequest, MsgID: 1})
	tests := [][]byte{
		nil,
		{},
		{0x50},                 // 1 byte
		{0x50, 0x48, 0x01, 0x01, 0, 0, 0, 1}, // wrong second magic byte
		{0x47, 0x50, 0x01, 0x01, 0, 0, 0, 1}, // swapped magic
		{0xFF, 0xFF, 0x01, 0x01, 0, 0, 0, 1},
	}
	for i, b := range tests {
		_, err := Decode(b)
		if err == nil {
			t.Errorf("case %d: expected error, got none", i)
			continue
		}
		// short inputs return ErrShort; everything else must be ErrBadMagic.
		if len(b) < HeaderSize {
			if !errors.Is(err, ErrShort) {
				t.Errorf("case %d: want ErrShort, got %v", i, err)
			}
		} else if !errors.Is(err, ErrBadMagic) {
			t.Errorf("case %d: want ErrBadMagic, got %v", i, err)
		}
	}
	// Sanity: the good packet still decodes.
	if _, err := Decode(good); err != nil {
		t.Fatalf("good packet failed to decode: %v", err)
	}
}

func TestUnknownTypeIgnored(t *testing.T) {
	// Forge a packet with a type byte not in v1.
	raw := []byte{0x50, 0x47, 0x01, 0x7F /* unknown */, 0x00, 0x00, 0x00, 0x05, 0x01, 0x02}
	env, err := Decode(raw)
	if !errors.Is(err, ErrUnknownType) {
		t.Fatalf("want ErrUnknownType, got %v", err)
	}
	// Header is still parsed so callers can log/diagnose.
	if env.Version != 1 || env.MsgID != 5 || env.Type != 0x7F {
		t.Errorf("header not parsed on unknown type: %+v", env)
	}
	if !bytes.Equal(env.Payload, []byte{0x01, 0x02}) {
		t.Errorf("payload not parsed on unknown type: %x", env.Payload)
	}
	if env.Type.IsKnown() {
		t.Errorf("Type(0x7F).IsKnown() should be false")
	}
}

func TestEncodeRejectsZeroVersion(t *testing.T) {
	if _, err := Encode(Envelope{Version: 0, Type: TypeRequest}); err == nil {
		t.Fatal("expected error for version=0")
	}
}

func TestDecodeCopiesPayload(t *testing.T) {
	raw := []byte{0x50, 0x47, 0x01, 0x01, 0, 0, 0, 1, 0xAA, 0xBB}
	env, err := Decode(raw)
	if err != nil {
		t.Fatalf("Decode: %v", err)
	}
	raw[8] = 0x00 // mutate input
	if env.Payload[0] != 0xAA {
		t.Errorf("payload aliased input buffer: got %x", env.Payload)
	}
}
