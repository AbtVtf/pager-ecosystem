package lora

import (
	"bytes"
	"errors"
	"testing"
)

func TestAdvertRoundTrip(t *testing.T) {
	var pk [32]byte
	for i := range pk {
		pk[i] = byte(i)
	}
	in := Advert{Pubkey: pk, BWClass: BWHigh, Load: 42}
	b := EncodeAdvert(in)
	if len(b) != AdvertSize {
		t.Fatalf("encoded size: got %d, want %d", len(b), AdvertSize)
	}
	out, err := DecodeAdvert(b)
	if err != nil {
		t.Fatalf("DecodeAdvert: %v", err)
	}
	if out != in {
		t.Errorf("round-trip mismatch:\n got  %+v\n want %+v", out, in)
	}
}

func TestAdvertWireLayout(t *testing.T) {
	var pk [32]byte
	pk[0] = 0xAA
	pk[31] = 0xBB
	b := EncodeAdvert(Advert{Pubkey: pk, BWClass: BWMedium, Load: 0x80})

	if b[0] != AdvertVersion {
		t.Errorf("version byte: got %02x want %02x", b[0], AdvertVersion)
	}
	if !bytes.Equal(b[1:33], pk[:]) {
		t.Errorf("pubkey region mismatch:\n got  %x\n want %x", b[1:33], pk[:])
	}
	if b[33] != byte(BWMedium) {
		t.Errorf("bw_class byte: got %02x want %02x", b[33], byte(BWMedium))
	}
	if b[34] != 0x80 {
		t.Errorf("load byte: got %02x want %02x", b[34], 0x80)
	}
}

func TestAdvertDecodeShort(t *testing.T) {
	cases := [][]byte{
		nil,
		{},
		make([]byte, AdvertSize-1),
	}
	for i, b := range cases {
		if _, err := DecodeAdvert(b); !errors.Is(err, ErrAdvertShort) {
			t.Errorf("case %d: want ErrAdvertShort, got %v", i, err)
		}
	}
}

func TestAdvertDecodeWrongVersion(t *testing.T) {
	b := make([]byte, AdvertSize)
	b[0] = 0xFE
	if _, err := DecodeAdvert(b); !errors.Is(err, ErrAdvertVersion) {
		t.Fatalf("want ErrAdvertVersion, got %v", err)
	}
}

func TestAdvertDecodeForwardCompatTrailingBytes(t *testing.T) {
	// A future version may append fields. Today's decoder must accept
	// the prefix and ignore the trailing bytes.
	var pk [32]byte
	for i := range pk {
		pk[i] = byte(0x11 + i)
	}
	base := EncodeAdvert(Advert{Pubkey: pk, BWClass: BWLow, Load: 7})
	extended := append(base, 0xDE, 0xAD, 0xBE, 0xEF)
	out, err := DecodeAdvert(extended)
	if err != nil {
		t.Fatalf("DecodeAdvert(extended): %v", err)
	}
	if out.Pubkey != pk || out.BWClass != BWLow || out.Load != 7 {
		t.Errorf("decoded fields mismatch: %+v", out)
	}
}

func TestAdvertCarriedInEnvelope(t *testing.T) {
	// Sanity: an advert payload survives a LoRa envelope round-trip.
	var pk [32]byte
	pk[5] = 0x77
	payload := EncodeAdvert(Advert{Pubkey: pk, BWClass: BWGigabit, Load: 200})

	enc, err := Encode(Envelope{Version: 1, Type: TypeExitNodeAdvert, MsgID: 0xCAFEBABE, Payload: payload})
	if err != nil {
		t.Fatalf("Encode envelope: %v", err)
	}
	env, err := Decode(enc)
	if err != nil {
		t.Fatalf("Decode envelope: %v", err)
	}
	if env.Type != TypeExitNodeAdvert {
		t.Fatalf("envelope type: got %v want TypeExitNodeAdvert", env.Type)
	}
	got, err := DecodeAdvert(env.Payload)
	if err != nil {
		t.Fatalf("DecodeAdvert: %v", err)
	}
	if got.Pubkey != pk || got.BWClass != BWGigabit || got.Load != 200 {
		t.Errorf("inner advert mismatch: %+v", got)
	}
}
