// Package pageossig verifies the PagerOS-Sig request authentication scheme
// defined in SPEC §9.2. The same construction is implemented in the Python SDK
// (`sdk/python/pageros/signing.py`) and in firmware via libsodium; this Go
// implementation is wire-byte-identical to those.
//
// Wire format (request headers):
//
//	PagerOS-Device     base64url(no padding) of 32-byte Ed25519 public key
//	PagerOS-Sig        base64url(no padding) of 64-byte Ed25519 signature
//	PagerOS-Timestamp  decimal ASCII Unix seconds
//
// Signing input bytes (no separators):
//
//	utf8(method) || utf8(url) || utf8(timestamp) || sha256(body)
//
// The relay only ever needs to *verify* device signatures, so the package
// exposes Verify (and a tiny Sign helper kept only for tests/clients).
package pageossig

import (
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"errors"
	"fmt"
	"net/http"
	"strconv"
	"time"
)

const (
	HeaderDevice    = "PagerOS-Device"
	HeaderSig       = "PagerOS-Sig"
	HeaderTimestamp = "PagerOS-Timestamp"

	// DefaultMaxSkew matches SPEC §9.2 "reject > 5 min skew".
	DefaultMaxSkew = 5 * time.Minute
)

// Errors returned by Verify. Callers may use errors.Is to distinguish them
// from a generic auth failure.
var (
	ErrMissingHeader  = errors.New("pageossig: missing PagerOS-* header")
	ErrInvalidEncoding = errors.New("pageossig: header could not be parsed")
	ErrTimestampSkew  = errors.New("pageossig: timestamp outside skew window")
	ErrBadSignature   = errors.New("pageossig: signature did not verify")
)

// Verified is returned on a successful Verify.
type Verified struct {
	DevicePubkey ed25519.PublicKey
	Timestamp    int64
}

// Verify checks the PagerOS-Sig headers attached to r. method is normally
// r.Method; url is normally r.URL.RequestURI(); body is the raw request body
// bytes (use nil for an empty body). now is the server clock used for skew
// checking — pass nil to use time.Now.
//
// On success it returns the verified device public key and the request's
// embedded timestamp. On failure it returns one of the sentinel errors
// above wrapped with context; callers should respond with HTTP 401.
func Verify(r *http.Request, body []byte, maxSkew time.Duration, now func() time.Time) (Verified, error) {
	if maxSkew <= 0 {
		maxSkew = DefaultMaxSkew
	}
	if now == nil {
		now = time.Now
	}

	deviceHdr := r.Header.Get(HeaderDevice)
	sigHdr := r.Header.Get(HeaderSig)
	tsHdr := r.Header.Get(HeaderTimestamp)

	if deviceHdr == "" {
		return Verified{}, fmt.Errorf("%w: %s", ErrMissingHeader, HeaderDevice)
	}
	if sigHdr == "" {
		return Verified{}, fmt.Errorf("%w: %s", ErrMissingHeader, HeaderSig)
	}
	if tsHdr == "" {
		return Verified{}, fmt.Errorf("%w: %s", ErrMissingHeader, HeaderTimestamp)
	}

	pubkey, err := DecodePubkey(deviceHdr)
	if err != nil {
		return Verified{}, fmt.Errorf("%w: %s: %v", ErrInvalidEncoding, HeaderDevice, err)
	}
	sig, err := decodeBase64URL(sigHdr, ed25519.SignatureSize)
	if err != nil {
		return Verified{}, fmt.Errorf("%w: %s: %v", ErrInvalidEncoding, HeaderSig, err)
	}
	ts, err := strconv.ParseInt(tsHdr, 10, 64)
	if err != nil {
		return Verified{}, fmt.Errorf("%w: %s: %v", ErrInvalidEncoding, HeaderTimestamp, err)
	}

	delta := now().Unix() - ts
	if delta < 0 {
		delta = -delta
	}
	if time.Duration(delta)*time.Second > maxSkew {
		return Verified{}, fmt.Errorf("%w: |Δ|=%ds > %s", ErrTimestampSkew, delta, maxSkew)
	}

	input := BuildSigningInput(r.Method, r.URL.RequestURI(), tsHdr, body)
	if !ed25519.Verify(pubkey, input, sig) {
		return Verified{}, ErrBadSignature
	}
	return Verified{DevicePubkey: pubkey, Timestamp: ts}, nil
}

// BuildSigningInput returns the exact bytes that get signed. Exported so
// clients and tests stay byte-aligned with the Python reference impl.
func BuildSigningInput(method, url, timestamp string, body []byte) []byte {
	bodyHash := sha256.Sum256(body)
	out := make([]byte, 0, len(method)+len(url)+len(timestamp)+sha256.Size)
	out = append(out, method...)
	out = append(out, url...)
	out = append(out, timestamp...)
	out = append(out, bodyHash[:]...)
	return out
}

// EncodePubkey returns the canonical base64url-no-padding form of a 32-byte
// Ed25519 public key as it appears in PagerOS URLs and the PagerOS-Device
// header.
func EncodePubkey(pk ed25519.PublicKey) string {
	return base64.RawURLEncoding.EncodeToString(pk)
}

// DecodePubkey parses the canonical base64url-no-padding form of an Ed25519
// public key into a 32-byte key. Accepts padded inputs as well, since
// proxies sometimes re-encode URL segments.
func DecodePubkey(s string) (ed25519.PublicKey, error) {
	raw, err := decodeBase64URL(s, ed25519.PublicKeySize)
	if err != nil {
		return nil, err
	}
	return ed25519.PublicKey(raw), nil
}

func decodeBase64URL(s string, expectedLen int) ([]byte, error) {
	// Canonical form is unpadded base64url. Accept padded inputs as well —
	// some proxies / clients normalize URL-segment base64 back to a padded
	// representation in headers.
	raw, err := base64.RawURLEncoding.DecodeString(s)
	if err != nil {
		raw, err = base64.URLEncoding.DecodeString(s)
		if err != nil {
			return nil, err
		}
	}
	if len(raw) != expectedLen {
		return nil, fmt.Errorf("expected %d bytes, got %d", expectedLen, len(raw))
	}
	return raw, nil
}

// Sign is a test/client convenience that produces a Set of headers matching
// the Verify input. Returns the headers and the timestamp used.
func Sign(method, url string, body []byte, priv ed25519.PrivateKey, ts time.Time) (map[string]string, int64) {
	if ts.IsZero() {
		ts = time.Now()
	}
	tsStr := strconv.FormatInt(ts.Unix(), 10)
	input := BuildSigningInput(method, url, tsStr, body)
	sig := ed25519.Sign(priv, input)
	pub := priv.Public().(ed25519.PublicKey)
	return map[string]string{
		HeaderDevice:    EncodePubkey(pub),
		HeaderSig:       base64.RawURLEncoding.EncodeToString(sig),
		HeaderTimestamp: tsStr,
	}, ts.Unix()
}
