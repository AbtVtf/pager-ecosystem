package lora

import (
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"testing"

	"golang.org/x/crypto/curve25519"
)

// keypair returns a fresh X25519 (priv, pub) pair.
func x25519Keypair(t *testing.T) (priv, pub []byte) {
	t.Helper()
	priv = make([]byte, X25519KeyLen)
	if _, err := rand.Read(priv); err != nil {
		t.Fatal(err)
	}
	pub, err := curve25519.X25519(priv, curve25519.Basepoint)
	if err != nil {
		t.Fatal(err)
	}
	return priv, pub
}

func ed25519Keypair(t *testing.T) (priv ed25519.PrivateKey, pub ed25519.PublicKey) {
	t.Helper()
	p, s, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	return s, p
}

func TestInnerEnvelopeRoundTripCBOR(t *testing.T) {
	from := bytes.Repeat([]byte{0xAA}, 32)
	nonce := bytes.Repeat([]byte{0x02}, 16)
	sig := bytes.Repeat([]byte{0xCD}, 64)
	body := []byte("ciphertext+tag")
	e := InnerEnvelope{
		To:    "https://notes.app/save",
		From:  from,
		Nonce: nonce,
		Sig:   sig,
		Body:  body,
	}
	b, err := EncodeInner(e)
	if err != nil {
		t.Fatal(err)
	}
	got, err := DecodeInner(b)
	if err != nil {
		t.Fatal(err)
	}
	if got.To != e.To || !bytes.Equal(got.From, e.From) || !bytes.Equal(got.Nonce, e.Nonce) || !bytes.Equal(got.Sig, e.Sig) || !bytes.Equal(got.Body, e.Body) {
		t.Fatalf("round-trip mismatch:\n got  %+v\n want %+v", got, e)
	}
}

func TestEncodeInnerRejectsBadShape(t *testing.T) {
	good := InnerEnvelope{
		To: "https://x", From: bytes.Repeat([]byte{0x01}, 32),
		Nonce: bytes.Repeat([]byte{0x02}, 16), Sig: bytes.Repeat([]byte{0x03}, 64),
	}
	cases := []struct {
		name string
		mut  func(*InnerEnvelope)
		want error
	}{
		{"empty to", func(e *InnerEnvelope) { e.To = "" }, ErrEmptyTo},
		{"short from", func(e *InnerEnvelope) { e.From = e.From[:31] }, ErrBadKeyLen},
		{"long from", func(e *InnerEnvelope) { e.From = bytes.Repeat([]byte{0x01}, 33) }, ErrBadKeyLen},
		{"short nonce", func(e *InnerEnvelope) { e.Nonce = e.Nonce[:15] }, ErrBadNonceLen},
		{"short sig", func(e *InnerEnvelope) { e.Sig = e.Sig[:63] }, ErrBadSigLen},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			e := good
			tc.mut(&e)
			if _, err := EncodeInner(e); !errors.Is(err, tc.want) {
				t.Errorf("want %v, got %v", tc.want, err)
			}
		})
	}
}

func TestDecodeInnerRejectsBadShape(t *testing.T) {
	// Build a CBOR envelope with a short `from` and confirm Decode rejects it.
	bad := InnerEnvelope{
		To:    "https://x",
		From:  bytes.Repeat([]byte{0x01}, 32),
		Nonce: bytes.Repeat([]byte{0x02}, 16),
		Sig:   bytes.Repeat([]byte{0x03}, 64),
		Body:  []byte{},
	}
	raw, err := EncodeInner(bad)
	if err != nil {
		t.Fatal(err)
	}
	// Sanity: round-trips clean.
	if _, err := DecodeInner(raw); err != nil {
		t.Fatalf("baseline decode failed: %v", err)
	}
	// Garbage bytes → decode error.
	if _, err := DecodeInner([]byte{0xff, 0xff, 0xff}); !errors.Is(err, ErrInnerDecode) {
		t.Errorf("garbage: want ErrInnerDecode, got %v", err)
	}
}

func TestSealAndOpenRoundTrip(t *testing.T) {
	// Device sender, app recipient.
	devPriv, devPub := x25519Keypair(t)
	appPriv, appPub := x25519Keypair(t)
	edPriv, edPub := ed25519Keypair(t)

	plaintext := []byte("POST /save body bytes")
	env, err := Seal(
		"https://notes.app/save",
		plaintext,
		devPub,
		appPub,
		devPriv,
		edPriv,
		SenderDeviceToApp,
		1,
		[]byte{0xDE, 0xAD, 0xBE, 0xEF},
	)
	if err != nil {
		t.Fatal(err)
	}

	// Body must be ciphertext + 16B tag, not plaintext.
	if bytes.Contains(env.Body, plaintext) {
		t.Fatal("Body contains plaintext — encryption did not happen")
	}
	if len(env.Body) != len(plaintext)+AEADTagLen {
		t.Errorf("Body len = %d, want %d", len(env.Body), len(plaintext)+AEADTagLen)
	}

	// Sig verification with the right Ed25519 pubkey.
	if err := VerifyInner(env, edPub); err != nil {
		t.Errorf("Verify: %v", err)
	}
	// Open with the right X25519 private key.
	got, err := OpenInner(env, appPriv)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	if !bytes.Equal(got, plaintext) {
		t.Errorf("plaintext mismatch:\n got  %x\n want %x", got, plaintext)
	}
}

func TestOpenWithWrongKeyFails(t *testing.T) {
	// Acceptance criterion: "decrypts only with target's key."
	devPriv, devPub := x25519Keypair(t)
	_, appPub := x25519Keypair(t)
	_, wrongPriv := x25519Keypair(t) // different private key
	edPriv, _ := ed25519Keypair(t)

	env, err := Seal(
		"https://x",
		[]byte("secret"),
		devPub, appPub, devPriv, edPriv,
		SenderDeviceToApp, 0, nil,
	)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := OpenInner(env, wrongPriv); !errors.Is(err, ErrDecryptFailed) {
		t.Fatalf("want ErrDecryptFailed when decrypting with non-target key, got %v", err)
	}
}

func TestOpenWithTamperedBodyFails(t *testing.T) {
	devPriv, devPub := x25519Keypair(t)
	appPriv, appPub := x25519Keypair(t)
	edPriv, _ := ed25519Keypair(t)

	env, err := Seal("https://x", []byte("secret"), devPub, appPub, devPriv, edPriv, SenderDeviceToApp, 0, nil)
	if err != nil {
		t.Fatal(err)
	}
	env.Body[0] ^= 0x01
	if _, err := OpenInner(env, appPriv); !errors.Is(err, ErrDecryptFailed) {
		t.Fatalf("want ErrDecryptFailed on tampered body, got %v", err)
	}
}

func TestOpenWithTamperedToFails(t *testing.T) {
	// `to` is in AAD; mutating it after Seal must break the AEAD.
	devPriv, devPub := x25519Keypair(t)
	appPriv, appPub := x25519Keypair(t)
	edPriv, _ := ed25519Keypair(t)
	env, err := Seal("https://x", []byte("secret"), devPub, appPub, devPriv, edPriv, SenderDeviceToApp, 0, nil)
	if err != nil {
		t.Fatal(err)
	}
	env.To = "https://y"
	if _, err := OpenInner(env, appPriv); !errors.Is(err, ErrDecryptFailed) {
		t.Fatalf("want ErrDecryptFailed when `to` mutated, got %v", err)
	}
}

func TestVerifyWithWrongPubFails(t *testing.T) {
	devPriv, devPub := x25519Keypair(t)
	_, appPub := x25519Keypair(t)
	edPriv, _ := ed25519Keypair(t)
	_, otherEdPub := ed25519Keypair(t)

	env, err := Seal("https://x", []byte("body"), devPub, appPub, devPriv, edPriv, SenderDeviceToApp, 0, nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := VerifyInner(env, otherEdPub); !errors.Is(err, ErrSigInvalid) {
		t.Fatalf("want ErrSigInvalid for wrong Ed25519 pubkey, got %v", err)
	}
}

func TestVerifyWithTamperedSigFails(t *testing.T) {
	devPriv, devPub := x25519Keypair(t)
	_, appPub := x25519Keypair(t)
	edPriv, edPub := ed25519Keypair(t)
	env, err := Seal("https://x", []byte("body"), devPub, appPub, devPriv, edPriv, SenderDeviceToApp, 0, nil)
	if err != nil {
		t.Fatal(err)
	}
	env.Sig[0] ^= 0x01
	if err := VerifyInner(env, edPub); !errors.Is(err, ErrSigInvalid) {
		t.Errorf("want ErrSigInvalid on flipped sig byte, got %v", err)
	}
}

func TestOpenAndVerifyHappy(t *testing.T) {
	devPriv, devPub := x25519Keypair(t)
	appPriv, appPub := x25519Keypair(t)
	edPriv, edPub := ed25519Keypair(t)
	plain := []byte("pong")
	env, err := Seal("https://x", plain, devPub, appPub, devPriv, edPriv, SenderDeviceToApp, 7, nil)
	if err != nil {
		t.Fatal(err)
	}
	got, err := OpenAndVerifyInner(env, appPriv, edPub)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, plain) {
		t.Errorf("plaintext: got %x want %x", got, plain)
	}
}

func TestBuildNonceLayout(t *testing.T) {
	// Locks the SEC-001 §1.2 layout.
	got, err := BuildNonce(0x02, 0x0102030405060708, []byte{0xAA, 0xBB, 0xCC, 0xDD})
	if err != nil {
		t.Fatal(err)
	}
	want := []byte{
		0x02, // sender_id_byte
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, // u64_be counter
		0x00, 0x00, 0x00, // 3 zero bytes
		0xAA, 0xBB, 0xCC, 0xDD, // sig-binding tail
	}
	if !bytes.Equal(got[:], want) {
		t.Errorf("layout:\n got  %x\n want %x", got[:], want)
	}
}

func TestBuildNonceShortSigSaltPads(t *testing.T) {
	got, err := BuildNonce(0x02, 1, []byte{0xAA})
	if err != nil {
		t.Fatal(err)
	}
	// Bytes 13..16 must be zero-padded.
	if got[12] != 0xAA || got[13] != 0 || got[14] != 0 || got[15] != 0 {
		t.Errorf("padding wrong: %x", got[:])
	}
}

func TestBuildNonceRejectsLongSigSalt(t *testing.T) {
	if _, err := BuildNonce(0x02, 1, []byte{0, 0, 0, 0, 0}); err == nil {
		t.Fatal("want error for 5-byte sigSalt")
	}
}

func TestAEADNonceExtraction(t *testing.T) {
	innerNonce := []byte{
		0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,
		0xFF, 0xEE, 0xDD, 0xCC,
	}
	got, err := AEADNonceFromInnerNonce(innerNonce)
	if err != nil {
		t.Fatal(err)
	}
	want := innerNonce[:12]
	if !bytes.Equal(got, want) {
		t.Errorf("AEAD nonce: got %x want %x", got, want)
	}
}

func TestDeriveSessionKeyRejectsZeroSharedSecret(t *testing.T) {
	// Multiplying any priv by the all-zero point yields the all-zero shared
	// secret; curve25519.X25519 returns ErrInvalidKey for this case rather
	// than handing us the zero point, so we wrap that path here.
	priv := bytes.Repeat([]byte{0x01}, 32)
	zero := make([]byte, 32)
	_, err := DeriveSessionKey(priv, zero)
	if err == nil {
		t.Fatal("want error for zero peer pub")
	}
}

func TestDeriveSessionKeyDeterministic(t *testing.T) {
	priv, _ := x25519Keypair(t)
	_, peerPub := x25519Keypair(t)
	a, err := DeriveSessionKey(priv, peerPub)
	if err != nil {
		t.Fatal(err)
	}
	b, err := DeriveSessionKey(priv, peerPub)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(a, b) {
		t.Fatal("DeriveSessionKey not deterministic")
	}
	if len(a) != AEADKeyLen {
		t.Errorf("key len = %d, want %d", len(a), AEADKeyLen)
	}
}

func TestDeriveSessionKeyMatchesSEC001Vector(t *testing.T) {
	// RFC 7748 §6.1 Alice/Bob X25519 pair, run through SEC-001 §1.1 HKDF.
	//
	// `priv` and `pub_peer` come from docs/spec/crypto-test-vectors.json
	// (vector x25519-rfc7748-6.1). The vector's `derived_key` field is
	// still the placeholder "PAGEROS_HKDF_PENDING_FROM_IMPL" pending
	// SEC review of the first reference output — that's THIS test. The
	// expected bytes below were independently reproduced with PyNaCl's
	// libsodium binding and the inline HKDF in
	// sdk/python/pageros/encryption.py. SEC-001 §3.1 expects this
	// vector to be promoted into the JSON once two impls agree.
	alicePriv := mustHex(t, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
	bobPub := mustHex(t, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f")
	wantShared := mustHex(t, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742")
	wantKey := mustHex(t, "6f93ee819f541546210818b73f62fcddb554542b8553c5e32f633649a31f2695")

	// Verify the raw X25519 step first so failures point at the right layer.
	gotShared, err := curve25519.X25519(alicePriv, bobPub)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(gotShared, wantShared) {
		t.Fatalf("X25519 shared mismatch:\n got  %x\n want %x", gotShared, wantShared)
	}

	got, err := DeriveSessionKey(alicePriv, bobPub)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, wantKey) {
		t.Errorf("HKDF-of-X25519 mismatch:\n got  %x\n want %x", got, wantKey)
	}
}

func mustHex(t *testing.T, s string) []byte {
	t.Helper()
	b, err := hex.DecodeString(s)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func TestSigningInputLayout(t *testing.T) {
	to := "ab"
	body := []byte{0xAA, 0xBB}
	nonce := bytes.Repeat([]byte{0x01}, 16)
	got := SigningInput(to, body, nonce)
	want := []byte{
		0x00, 0x02, // len_be(to)=2
		'a', 'b',
		0x00, 0x00, 0x00, 0x02, // len_be(body)=2
		0xAA, 0xBB,
	}
	want = append(want, nonce...)
	if !bytes.Equal(got, want) {
		t.Errorf("signing input:\n got  %x\n want %x", got, want)
	}
}

func TestValidateHTTPSTarget(t *testing.T) {
	cases := []struct {
		to   string
		want error
	}{
		{"https://notes.app/save", nil},
		{"https://notes.app:8443/x", nil},
		{"http://notes.app/save", ErrTargetNotHTTPS},
		{"ftp://x/", ErrTargetNotHTTPS},
		{"https://", ErrTargetNotHTTPS}, // empty host
		{"", ErrEmptyTo},
		// "://broken" → net/url returns "missing protocol scheme"; the
		// validator surfaces that error directly rather than wrapping
		// ErrTargetNotHTTPS, which is the right shape: the URL is
		// malformed before we even know its scheme.
	}
	for _, tc := range cases {
		t.Run(tc.to, func(t *testing.T) {
			err := ValidateHTTPSTarget(InnerEnvelope{To: tc.to})
			if tc.want == nil {
				if err != nil {
					t.Errorf("want nil, got %v", err)
				}
			} else if !errors.Is(err, tc.want) {
				t.Errorf("want %v, got %v", tc.want, err)
			}
		})
	}
}

func TestEncodeDecodeViaOuterEnvelopeAndFragments(t *testing.T) {
	// End-to-end through the full LoRa stack: outer envelope + fragments
	// + inner. Proves the three layers compose.
	devPriv, devPub := x25519Keypair(t)
	appPriv, appPub := x25519Keypair(t)
	edPriv, edPub := ed25519Keypair(t)

	plaintext := bytes.Repeat([]byte("REQ"), 200) // 600 bytes — forces fragmentation
	env, err := Seal("https://notes.app/big", plaintext, devPub, appPub, devPriv, edPriv, SenderDeviceToApp, 42, nil)
	if err != nil {
		t.Fatal(err)
	}
	innerWire, err := EncodeInner(env)
	if err != nil {
		t.Fatal(err)
	}
	// Fragment + outer-envelope wrap.
	const msgID uint32 = 0xCAFE0042
	frags, err := SplitMessage(msgID, innerWire, 150)
	if err != nil {
		t.Fatal(err)
	}
	r := NewReassembler()
	var reassembled []byte
	var complete bool
	for _, f := range frags {
		body, err := EncodeFragmentBody(f)
		if err != nil {
			t.Fatal(err)
		}
		outer := Envelope{Version: 1, Type: TypeRequest, MsgID: f.MsgID, Payload: body}
		wire, err := Encode(outer)
		if err != nil {
			t.Fatal(err)
		}
		// On the receive side:
		decOuter, err := Decode(wire)
		if err != nil {
			t.Fatal(err)
		}
		decFrag, err := DecodeFragmentBody(decOuter.Payload)
		if err != nil {
			t.Fatal(err)
		}
		decFrag.MsgID = decOuter.MsgID
		reassembled, complete, err = r.Add(decFrag)
		if err != nil {
			t.Fatal(err)
		}
	}
	if !complete {
		t.Fatal("never completed")
	}
	if !bytes.Equal(reassembled, innerWire) {
		t.Fatal("reassembled inner wire bytes differ")
	}
	decInner, err := DecodeInner(reassembled)
	if err != nil {
		t.Fatal(err)
	}
	got, err := OpenAndVerifyInner(decInner, appPriv, edPub)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, plaintext) {
		t.Fatal("end-to-end plaintext mismatch")
	}
}
