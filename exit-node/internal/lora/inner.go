package lora

// Inner envelope — encrypted body, end-to-end-authenticated routing
// (SPEC.md §6.2.2).
//
// Wire form (CBOR map, sits in the outer envelope's payload after
// defragmentation):
//
//	{
//	  "to":    tstr URL,                        // cleartext, Exit Node reads to route
//	  "from":  bstr 32B sender X25519 pubkey,   // cleartext, used for ECDH at recipient
//	  "nonce": bstr 16B,                        // see "Nonce layout" below
//	  "sig":   bstr 64B Ed25519 sig (see "Signature" below),
//	  "body":  bstr ChaCha20-Poly1305(plaintext, ...) || 16B tag
//	}
//
// Encryption: ChaCha20-Poly1305 IETF, with key
//
//	key = HKDF-SHA256(
//	        ikm  = X25519(sender_priv, recipient_pub),
//	        salt = 32 zero bytes,
//	        info = "pageros/v1/e2e",
//	        L    = 32,
//	      )
//
// per docs/spec/crypto-suite.md (SEC-001) §1.1.
//
// Nonce layout (the 16-byte `nonce` field):
//
//	bytes 0..11  — SEC-001 §1.2 AEAD nonce: sender_id_byte || u64_be(counter) || 3 zero bytes
//	bytes 12..15 — 4 random bytes that extend the signature-binding randomness
//
// The first 12 bytes are passed directly to the AEAD as its nonce.
// The full 16 bytes go into the Ed25519 signature input, so even if a
// counter repeats (e.g. after device key rotation) the signature
// covers extra entropy. sender_id_byte is 0x02 for device→app and 0x01
// for app→device.
//
// Signature: Ed25519 over the concatenation
//
//	u16_be(len(to)) || to_bytes || u32_be(len(body)) || body_bytes || nonce_bytes
//
// Length-prefixing the variable-length fields prevents extension
// attacks across field boundaries. `body` is the ciphertext (including
// the 16-byte Poly1305 tag) — this is encrypt-then-sign, so the
// recipient can verify the sig before touching the cipher.
//
// AAD passed to the AEAD is `to_bytes || from_bytes` so the routing
// fields are bound to the ciphertext: an Exit Node cannot replay the
// body against a different `to` without breaking authentication.

import (
	"bytes"
	"crypto/ed25519"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"fmt"
	"net/url"

	"github.com/fxamacker/cbor/v2"
	"golang.org/x/crypto/chacha20poly1305"
	"golang.org/x/crypto/curve25519"
)

// X25519KeyLen, AEADNonceLen, AEADTagLen, SigLen are SPEC + SEC-001 constants.
const (
	X25519KeyLen    = 32
	AEADKeyLen      = 32
	AEADNonceLen    = 12
	AEADTagLen      = 16
	InnerNonceLen   = 16
	SigLen          = 64
	Ed25519PubLen   = 32
	Ed25519PrivLen  = ed25519.PrivateKeySize // 64
)

// Sender ID bytes per docs/spec/crypto-suite.md §1.2.
const (
	SenderAppToDevice byte = 0x01
	SenderDeviceToApp byte = 0x02
)

// HKDFInfo and HKDFSalt are pinned by SEC-001 §1.1.
var (
	HKDFInfo = []byte("pageros/v1/e2e")
	HKDFSalt = bytes.Repeat([]byte{0x00}, 32)
)

// Sentinel errors.
var (
	ErrInnerDecode   = errors.New("lora: inner envelope decode failed")
	ErrInnerEncode   = errors.New("lora: inner envelope encode failed")
	ErrBadKeyLen     = errors.New("lora: key length wrong")
	ErrBadNonceLen   = errors.New("lora: nonce length wrong")
	ErrBadSigLen     = errors.New("lora: signature length wrong")
	ErrDecryptFailed = errors.New("lora: inner body authentication failed")
	ErrSigInvalid    = errors.New("lora: inner signature did not verify")
	ErrZeroSharedSec = errors.New("lora: X25519 yielded all-zero shared secret")
	ErrTargetNotHTTPS = errors.New("lora: inner `to` is not an https URL")
	ErrEmptyTo       = errors.New("lora: inner `to` is empty")
)

// InnerEnvelope is the parsed cleartext+ciphertext form of an inner
// envelope. To/From/Nonce/Sig are all cleartext on the wire; Body is
// ciphertext+tag (or plaintext before Seal, depending on direction).
type InnerEnvelope struct {
	To    string `cbor:"to"`
	From  []byte `cbor:"from"`
	Nonce []byte `cbor:"nonce"`
	Sig   []byte `cbor:"sig"`
	Body  []byte `cbor:"body"`
}

// EncodeInner CBOR-encodes the envelope to wire bytes.
//
// Required field lengths: From=32, Nonce=16, Sig=64. Body may be any
// length (including zero).
func EncodeInner(e InnerEnvelope) ([]byte, error) {
	if e.To == "" {
		return nil, ErrEmptyTo
	}
	if len(e.From) != X25519KeyLen {
		return nil, fmt.Errorf("%w: from must be %d bytes, got %d", ErrBadKeyLen, X25519KeyLen, len(e.From))
	}
	if len(e.Nonce) != InnerNonceLen {
		return nil, fmt.Errorf("%w: nonce must be %d bytes, got %d", ErrBadNonceLen, InnerNonceLen, len(e.Nonce))
	}
	if len(e.Sig) != SigLen {
		return nil, fmt.Errorf("%w: sig must be %d bytes, got %d", ErrBadSigLen, SigLen, len(e.Sig))
	}
	em, err := cbor.CoreDetEncOptions().EncMode()
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrInnerEncode, err)
	}
	b, err := em.Marshal(e)
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrInnerEncode, err)
	}
	return b, nil
}

// DecodeInner parses wire bytes into an InnerEnvelope. Performs only
// shape checks (field lengths). Cryptographic verification is the
// caller's job via Verify and Open.
func DecodeInner(b []byte) (InnerEnvelope, error) {
	var e InnerEnvelope
	if err := cbor.Unmarshal(b, &e); err != nil {
		return InnerEnvelope{}, fmt.Errorf("%w: %v", ErrInnerDecode, err)
	}
	if e.To == "" {
		return e, ErrEmptyTo
	}
	if len(e.From) != X25519KeyLen {
		return e, fmt.Errorf("%w: from %d bytes", ErrBadKeyLen, len(e.From))
	}
	if len(e.Nonce) != InnerNonceLen {
		return e, fmt.Errorf("%w: nonce %d bytes", ErrBadNonceLen, len(e.Nonce))
	}
	if len(e.Sig) != SigLen {
		return e, fmt.Errorf("%w: sig %d bytes", ErrBadSigLen, len(e.Sig))
	}
	return e, nil
}

// BuildNonce constructs the 16-byte inner-envelope nonce.
//
// Bytes 0..11 are the SEC-001 §1.2 AEAD nonce
// (senderIDByte || u64_be(counter) || 3 zero bytes). Bytes 12..15 are
// caller-supplied entropy, included in the Ed25519 signature input but
// not in the AEAD nonce. Pass `nil` or fewer than 4 bytes and the tail
// is zero-padded; pass more and BuildNonce returns an error.
func BuildNonce(senderIDByte byte, counter uint64, sigSalt []byte) ([16]byte, error) {
	var out [16]byte
	out[0] = senderIDByte
	binary.BigEndian.PutUint64(out[1:9], counter)
	// out[9..12] left as zero per SEC-001.
	if len(sigSalt) > 4 {
		return out, fmt.Errorf("lora: sigSalt must be <= 4 bytes, got %d", len(sigSalt))
	}
	copy(out[12:16], sigSalt)
	return out, nil
}

// AEADNonceFromInnerNonce returns the 12-byte AEAD nonce embedded in
// an inner-envelope 16-byte nonce. Callers that hold an InnerEnvelope
// use this to feed the AEAD.
func AEADNonceFromInnerNonce(nonce []byte) ([]byte, error) {
	if len(nonce) != InnerNonceLen {
		return nil, fmt.Errorf("%w: %d bytes", ErrBadNonceLen, len(nonce))
	}
	out := make([]byte, AEADNonceLen)
	copy(out, nonce[:AEADNonceLen])
	return out, nil
}

// DeriveSessionKey runs X25519 ECDH then HKDF-SHA256 per SEC-001 §1.1
// to produce a 32-byte AEAD key. priv is the local X25519 scalar;
// peerPub is the peer's X25519 pubkey.
func DeriveSessionKey(priv, peerPub []byte) ([]byte, error) {
	if len(priv) != X25519KeyLen {
		return nil, fmt.Errorf("%w: priv %d bytes", ErrBadKeyLen, len(priv))
	}
	if len(peerPub) != X25519KeyLen {
		return nil, fmt.Errorf("%w: peerPub %d bytes", ErrBadKeyLen, len(peerPub))
	}
	shared, err := curve25519.X25519(priv, peerPub)
	if err != nil {
		return nil, fmt.Errorf("lora: X25519: %w", err)
	}
	zero := true
	for _, b := range shared {
		if b != 0 {
			zero = false
			break
		}
	}
	if zero {
		return nil, ErrZeroSharedSec
	}
	return hkdfSHA256(shared, HKDFSalt, HKDFInfo, AEADKeyLen), nil
}

// hkdfSHA256 is a tiny HKDF-SHA256 (RFC 5869). We only ever want L<=32
// so the expand stage is a single block.
func hkdfSHA256(ikm, salt, info []byte, length int) []byte {
	mac := hmac.New(sha256.New, salt)
	mac.Write(ikm)
	prk := mac.Sum(nil)
	out := make([]byte, 0, length)
	var t []byte
	for counter := byte(1); len(out) < length; counter++ {
		mac = hmac.New(sha256.New, prk)
		mac.Write(t)
		mac.Write(info)
		mac.Write([]byte{counter})
		t = mac.Sum(nil)
		out = append(out, t...)
	}
	return out[:length]
}

// SigningInput is the byte string Ed25519 signs over (and Verify
// recomputes). Exported so cross-impl test vectors can pin its layout.
//
//	u16_be(len(to)) || to_bytes || u32_be(len(body)) || body_bytes || nonce_bytes
func SigningInput(to string, body, nonce []byte) []byte {
	toBytes := []byte(to)
	out := make([]byte, 0, 2+len(toBytes)+4+len(body)+len(nonce))
	var lenBuf [4]byte
	binary.BigEndian.PutUint16(lenBuf[:2], uint16(len(toBytes)))
	out = append(out, lenBuf[:2]...)
	out = append(out, toBytes...)
	binary.BigEndian.PutUint32(lenBuf[:4], uint32(len(body)))
	out = append(out, lenBuf[:4]...)
	out = append(out, body...)
	out = append(out, nonce...)
	return out
}

// Seal encrypts plaintext under (senderX25519Priv ↔ recipientX25519Pub)
// and signs the resulting envelope with senderEd25519Priv. fromPub is
// the X25519 pubkey of the sender (carried in the `from` field).
//
// counter is the SEC-001 AEAD counter; sigSalt is the optional 4-byte
// tail of the inner nonce (pass nil for zero).
func Seal(
	to string,
	plaintext []byte,
	fromPub []byte,
	recipientX25519Pub []byte,
	senderX25519Priv []byte,
	senderEd25519Priv ed25519.PrivateKey,
	senderIDByte byte,
	counter uint64,
	sigSalt []byte,
) (InnerEnvelope, error) {
	if to == "" {
		return InnerEnvelope{}, ErrEmptyTo
	}
	if len(fromPub) != X25519KeyLen {
		return InnerEnvelope{}, fmt.Errorf("%w: fromPub %d bytes", ErrBadKeyLen, len(fromPub))
	}
	if len(senderEd25519Priv) != ed25519.PrivateKeySize {
		return InnerEnvelope{}, fmt.Errorf("%w: ed25519 priv %d bytes", ErrBadKeyLen, len(senderEd25519Priv))
	}
	nonce16, err := BuildNonce(senderIDByte, counter, sigSalt)
	if err != nil {
		return InnerEnvelope{}, err
	}
	key, err := DeriveSessionKey(senderX25519Priv, recipientX25519Pub)
	if err != nil {
		return InnerEnvelope{}, err
	}
	aead, err := chacha20poly1305.New(key)
	if err != nil {
		return InnerEnvelope{}, fmt.Errorf("lora: chacha20poly1305.New: %w", err)
	}
	aad := make([]byte, 0, len(to)+len(fromPub))
	aad = append(aad, []byte(to)...)
	aad = append(aad, fromPub...)
	ciphertext := aead.Seal(nil, nonce16[:AEADNonceLen], plaintext, aad)
	sig := ed25519.Sign(senderEd25519Priv, SigningInput(to, ciphertext, nonce16[:]))
	return InnerEnvelope{
		To:    to,
		From:  append([]byte(nil), fromPub...),
		Nonce: append([]byte(nil), nonce16[:]...),
		Sig:   sig,
		Body:  ciphertext,
	}, nil
}

// OpenInner decrypts e.Body under (recipientX25519Priv ↔ e.From). It
// does NOT verify the Ed25519 signature — callers MUST call Verify
// first (or use OpenAndVerify, which composes the two).
//
// Returns ErrDecryptFailed if the AEAD authentication fails — the
// most common reason is "the recipient is not the intended target":
// decrypting only with target's key (acceptance criterion).
func OpenInner(e InnerEnvelope, recipientX25519Priv []byte) ([]byte, error) {
	if len(e.From) != X25519KeyLen {
		return nil, fmt.Errorf("%w: from %d bytes", ErrBadKeyLen, len(e.From))
	}
	if len(e.Nonce) != InnerNonceLen {
		return nil, fmt.Errorf("%w: nonce %d bytes", ErrBadNonceLen, len(e.Nonce))
	}
	key, err := DeriveSessionKey(recipientX25519Priv, e.From)
	if err != nil {
		return nil, err
	}
	aead, err := chacha20poly1305.New(key)
	if err != nil {
		return nil, fmt.Errorf("lora: chacha20poly1305.New: %w", err)
	}
	aad := make([]byte, 0, len(e.To)+len(e.From))
	aad = append(aad, []byte(e.To)...)
	aad = append(aad, e.From...)
	plaintext, err := aead.Open(nil, e.Nonce[:AEADNonceLen], e.Body, aad)
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrDecryptFailed, err)
	}
	return plaintext, nil
}

// VerifyInner checks e.Sig against senderEd25519Pub over SigningInput. The
// caller looks up the sender's Ed25519 pubkey out-of-band (from the
// app manifest, or by deriving from a known X25519 pubkey if the
// sender bound the two — out of scope here).
func VerifyInner(e InnerEnvelope, senderEd25519Pub ed25519.PublicKey) error {
	if len(e.Sig) != SigLen {
		return fmt.Errorf("%w: sig %d bytes", ErrBadSigLen, len(e.Sig))
	}
	if len(senderEd25519Pub) != ed25519.PublicKeySize {
		return fmt.Errorf("%w: pub %d bytes", ErrBadKeyLen, len(senderEd25519Pub))
	}
	if !ed25519.Verify(senderEd25519Pub, SigningInput(e.To, e.Body, e.Nonce), e.Sig) {
		return ErrSigInvalid
	}
	return nil
}

// OpenAndVerifyInner verifies the signature then decrypts. This is the
// usual recipient-side path.
func OpenAndVerifyInner(e InnerEnvelope, recipientX25519Priv []byte, senderEd25519Pub ed25519.PublicKey) ([]byte, error) {
	if err := VerifyInner(e, senderEd25519Pub); err != nil {
		return nil, err
	}
	return OpenInner(e, recipientX25519Priv)
}

// ValidateHTTPSTarget reports nil if e.To parses as an absolute
// https:// URL. EXIT-003 requires this gate before forwarding to the
// public internet; surfaced here so the same check is reusable by any
// caller that decapsulates an envelope.
func ValidateHTTPSTarget(e InnerEnvelope) error {
	if e.To == "" {
		return ErrEmptyTo
	}
	u, err := url.Parse(e.To)
	if err != nil {
		return fmt.Errorf("lora: parse `to`: %w", err)
	}
	if u.Scheme != "https" {
		return fmt.Errorf("%w: scheme=%q", ErrTargetNotHTTPS, u.Scheme)
	}
	if u.Host == "" {
		return fmt.Errorf("%w: missing host in `to`", ErrTargetNotHTTPS)
	}
	return nil
}
