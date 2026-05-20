package pageossig

import (
	"bytes"
	"crypto/ed25519"
	"encoding/hex"
	"errors"
	"net/http"
	"net/http/httptest"
	"strconv"
	"testing"
	"time"
)

// TestPageosSigSampleVector cross-checks the construction against the shared
// SPEC §9.2 test vector (docs/spec/crypto-test-vectors.json,
// id="ed25519-pageros-sig-sample"). Only the msg-assembly half of the vector
// is checked here — the sig field itself is still "PAGEROS_SIG_PENDING_FROM_IMPL"
// in the v0.2 draft. SEC-003 will lock in the sig bytes.
//
// The vector's body_hash_hex is sha256("{}") — i.e. the vector chose a 2-byte
// JSON-empty-object body, matching the Python reference test.
func TestPageosSigSampleVectorMessageBytes(t *testing.T) {
	method := "POST"
	url := "/v1/events"
	ts := "1700000000"
	body := []byte("{}")
	bodyHashHex := "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"

	expectedHex := hex.EncodeToString([]byte(method)) +
		hex.EncodeToString([]byte(url)) +
		hex.EncodeToString([]byte(ts)) +
		bodyHashHex

	got := BuildSigningInput(method, url, ts, body)
	if hex.EncodeToString(got) != expectedHex {
		t.Fatalf("signing-input mismatch\n got: %s\nwant: %s",
			hex.EncodeToString(got), expectedHex)
	}
}

func newKeypair(t *testing.T, seedHex string) (ed25519.PublicKey, ed25519.PrivateKey) {
	t.Helper()
	seed, err := hex.DecodeString(seedHex)
	if err != nil {
		t.Fatalf("seed decode: %v", err)
	}
	priv := ed25519.NewKeyFromSeed(seed)
	return priv.Public().(ed25519.PublicKey), priv
}

func signedRequest(t *testing.T, priv ed25519.PrivateKey, method, target string, body []byte, ts time.Time) *http.Request {
	t.Helper()
	req := httptest.NewRequest(method, target, bytes.NewReader(body))
	headers, _ := Sign(method, req.URL.RequestURI(), body, priv, ts)
	for k, v := range headers {
		req.Header.Set(k, v)
	}
	return req
}

func TestVerifyRoundTrip(t *testing.T) {
	pub, priv := newKeypair(t, "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
	ts := time.Unix(1700000000, 0)
	req := signedRequest(t, priv, http.MethodGet, "/pull/"+EncodePubkey(pub), nil, ts)

	v, err := Verify(req, nil, DefaultMaxSkew, func() time.Time { return ts })
	if err != nil {
		t.Fatalf("Verify: %v", err)
	}
	if !bytes.Equal(v.DevicePubkey, pub) {
		t.Fatalf("pubkey mismatch: got %x want %x", []byte(v.DevicePubkey), []byte(pub))
	}
	if v.Timestamp != ts.Unix() {
		t.Fatalf("timestamp = %d, want %d", v.Timestamp, ts.Unix())
	}
}

func TestVerifyRejectsTamperedURL(t *testing.T) {
	pub, priv := newKeypair(t, "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
	ts := time.Unix(1700000000, 0)

	headers, _ := Sign(http.MethodGet, "/pull/"+EncodePubkey(pub), nil, priv, ts)
	req := httptest.NewRequest(http.MethodGet, "/pull/different-pubkey", nil)
	for k, v := range headers {
		req.Header.Set(k, v)
	}
	_, err := Verify(req, nil, DefaultMaxSkew, func() time.Time { return ts })
	if !errors.Is(err, ErrBadSignature) {
		t.Fatalf("want ErrBadSignature, got %v", err)
	}
}

func TestVerifyRejectsBadSignature(t *testing.T) {
	pub, priv := newKeypair(t, "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
	ts := time.Unix(1700000000, 0)
	req := signedRequest(t, priv, http.MethodGet, "/pull/"+EncodePubkey(pub), nil, ts)

	// Flip a byte in the signature header.
	sig := req.Header.Get(HeaderSig)
	flipped := []byte(sig)
	if flipped[0] == 'A' {
		flipped[0] = 'B'
	} else {
		flipped[0] = 'A'
	}
	req.Header.Set(HeaderSig, string(flipped))

	_, err := Verify(req, nil, DefaultMaxSkew, func() time.Time { return ts })
	if !errors.Is(err, ErrBadSignature) && !errors.Is(err, ErrInvalidEncoding) {
		t.Fatalf("want ErrBadSignature or ErrInvalidEncoding, got %v", err)
	}
}

func TestVerifyRejectsExpiredTimestamp(t *testing.T) {
	pub, priv := newKeypair(t, "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
	signedAt := time.Unix(1700000000, 0)
	req := signedRequest(t, priv, http.MethodGet, "/pull/"+EncodePubkey(pub), nil, signedAt)

	serverNow := signedAt.Add(10 * time.Minute)
	_, err := Verify(req, nil, DefaultMaxSkew, func() time.Time { return serverNow })
	if !errors.Is(err, ErrTimestampSkew) {
		t.Fatalf("want ErrTimestampSkew, got %v", err)
	}
}

func TestVerifyRejectsMissingHeader(t *testing.T) {
	pub, priv := newKeypair(t, "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
	ts := time.Unix(1700000000, 0)

	for _, drop := range []string{HeaderDevice, HeaderSig, HeaderTimestamp} {
		t.Run(drop, func(t *testing.T) {
			req := signedRequest(t, priv, http.MethodGet, "/pull/"+EncodePubkey(pub), nil, ts)
			req.Header.Del(drop)
			_, err := Verify(req, nil, DefaultMaxSkew, func() time.Time { return ts })
			if !errors.Is(err, ErrMissingHeader) {
				t.Fatalf("want ErrMissingHeader, got %v", err)
			}
		})
	}
}

func TestVerifyRejectsMalformedHeaders(t *testing.T) {
	pub, priv := newKeypair(t, "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
	ts := time.Unix(1700000000, 0)

	t.Run("device", func(t *testing.T) {
		req := signedRequest(t, priv, http.MethodGet, "/pull/"+EncodePubkey(pub), nil, ts)
		req.Header.Set(HeaderDevice, "!!!not base64!!!")
		_, err := Verify(req, nil, DefaultMaxSkew, func() time.Time { return ts })
		if !errors.Is(err, ErrInvalidEncoding) {
			t.Fatalf("want ErrInvalidEncoding, got %v", err)
		}
	})
	t.Run("timestamp", func(t *testing.T) {
		req := signedRequest(t, priv, http.MethodGet, "/pull/"+EncodePubkey(pub), nil, ts)
		req.Header.Set(HeaderTimestamp, "not-a-number")
		_, err := Verify(req, nil, DefaultMaxSkew, func() time.Time { return ts })
		if !errors.Is(err, ErrInvalidEncoding) {
			t.Fatalf("want ErrInvalidEncoding, got %v", err)
		}
	})
}

func TestVerifyHonoursBodyHash(t *testing.T) {
	pub, priv := newKeypair(t, "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
	ts := time.Unix(1700000000, 0)

	body := []byte("hello world")
	headers, _ := Sign(http.MethodPost, "/push/"+EncodePubkey(pub), body, priv, ts)

	req := httptest.NewRequest(http.MethodPost, "/push/"+EncodePubkey(pub), bytes.NewReader(body))
	for k, v := range headers {
		req.Header.Set(k, v)
	}
	if _, err := Verify(req, body, DefaultMaxSkew, func() time.Time { return ts }); err != nil {
		t.Fatalf("Verify with body: %v", err)
	}

	// Now verify with a different body — must fail.
	if _, err := Verify(req, []byte("different body"), DefaultMaxSkew, func() time.Time { return ts }); !errors.Is(err, ErrBadSignature) {
		t.Fatalf("want ErrBadSignature on body mismatch, got %v", err)
	}
}

func TestDecodePubkeyTolerantOfPadding(t *testing.T) {
	pub, _ := newKeypair(t, "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
	// Force-pad version.
	padded := EncodePubkey(pub) + "="
	got, err := DecodePubkey(padded)
	if err != nil {
		t.Fatalf("DecodePubkey padded: %v", err)
	}
	if !bytes.Equal(got, pub) {
		t.Fatal("decoded pubkey != original")
	}
}

// TestTimestampParsingMatchesSpec just guards against accidental int formatting
// changes (Python uses str(ts), Go uses strconv.FormatInt).
func TestTimestampParsingMatchesSpec(t *testing.T) {
	for _, ts := range []int64{0, 1, 1700000000, 9999999999} {
		s := strconv.FormatInt(ts, 10)
		parsed, err := strconv.ParseInt(s, 10, 64)
		if err != nil || parsed != ts {
			t.Fatalf("roundtrip %d failed", ts)
		}
	}
}
