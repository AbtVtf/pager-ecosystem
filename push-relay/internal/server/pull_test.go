package server

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/pageros/pageros/push-relay/internal/pageossig"
	"github.com/pageros/pageros/push-relay/internal/storage"
)

const sigVectorSeedHex = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"

func newDeviceKey(t *testing.T, seedHex string) (ed25519.PublicKey, ed25519.PrivateKey) {
	t.Helper()
	seed, err := hex.DecodeString(seedHex)
	if err != nil {
		t.Fatalf("seed: %v", err)
	}
	priv := ed25519.NewKeyFromSeed(seed)
	return priv.Public().(ed25519.PublicKey), priv
}

func newTestServer(t *testing.T) (*Server, storage.Store) {
	t.Helper()
	store := storage.NewMemory()
	return New(Options{Storage: store, BuildTag: "test"}), store
}

func signedPull(t *testing.T, priv ed25519.PrivateKey, target string, at time.Time) *http.Request {
	t.Helper()
	req := httptest.NewRequest(http.MethodGet, target, nil)
	headers, _ := pageossig.Sign(http.MethodGet, req.URL.RequestURI(), nil, priv, at)
	for k, v := range headers {
		req.Header.Set(k, v)
	}
	return req
}

func TestPullHappyPath(t *testing.T) {
	pub, priv := newDeviceKey(t, sigVectorSeedHex)
	srv, store := newTestServer(t)
	deviceKey := pageossig.EncodePubkey(pub)

	want1, err := store.Enqueue(context.Background(), deviceKey, storage.Notification{
		Kind: storage.KindNotification, AppID: "app.test", Payload: []byte("payload-one"),
	})
	if err != nil {
		t.Fatalf("Enqueue 1: %v", err)
	}
	want2, err := store.Enqueue(context.Background(), deviceKey, storage.Notification{
		Kind: storage.KindGroupEvent, AppID: "app.chat", Payload: []byte("payload-two"),
	})
	if err != nil {
		t.Fatalf("Enqueue 2: %v", err)
	}

	req := signedPull(t, priv, "/pull/"+deviceKey, time.Now())
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, body=%s", rec.Code, rec.Body.String())
	}
	var resp pullResponse
	if err := json.NewDecoder(rec.Body).Decode(&resp); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(resp.Notifications) != 2 {
		t.Fatalf("got %d notifications, want 2", len(resp.Notifications))
	}
	if resp.Notifications[0].ID != want1.ID || resp.Notifications[1].ID != want2.ID {
		t.Fatalf("order or ids wrong: %+v", resp.Notifications)
	}
	if resp.Notifications[1].Kind != storage.KindGroupEvent {
		t.Fatalf("kind not propagated: %+v", resp.Notifications[1])
	}
	decoded, err := base64.StdEncoding.DecodeString(resp.Notifications[0].Payload)
	if err != nil {
		t.Fatalf("payload b64 decode: %v", err)
	}
	if !bytes.Equal(decoded, []byte("payload-one")) {
		t.Fatalf("payload roundtrip mismatch")
	}
}

func TestPullEmptyQueue(t *testing.T) {
	pub, priv := newDeviceKey(t, sigVectorSeedHex)
	srv, _ := newTestServer(t)
	deviceKey := pageossig.EncodePubkey(pub)

	req := signedPull(t, priv, "/pull/"+deviceKey, time.Now())
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d", rec.Code)
	}
	var resp pullResponse
	if err := json.NewDecoder(rec.Body).Decode(&resp); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(resp.Notifications) != 0 {
		t.Fatalf("expected empty list, got %d", len(resp.Notifications))
	}
}

func TestPullMissingHeadersReturns401(t *testing.T) {
	pub, _ := newDeviceKey(t, sigVectorSeedHex)
	srv, _ := newTestServer(t)
	deviceKey := pageossig.EncodePubkey(pub)

	req := httptest.NewRequest(http.MethodGet, "/pull/"+deviceKey, nil)
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", rec.Code)
	}
}

func TestPullBadSignatureReturns401(t *testing.T) {
	pub, priv := newDeviceKey(t, sigVectorSeedHex)
	srv, _ := newTestServer(t)
	deviceKey := pageossig.EncodePubkey(pub)

	req := signedPull(t, priv, "/pull/"+deviceKey, time.Now())
	// Tamper signature.
	sig := req.Header.Get(pageossig.HeaderSig)
	mut := []byte(sig)
	if mut[0] == 'A' {
		mut[0] = 'B'
	} else {
		mut[0] = 'A'
	}
	req.Header.Set(pageossig.HeaderSig, string(mut))

	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", rec.Code)
	}
}

func TestPullStaleTimestampReturns401(t *testing.T) {
	pub, priv := newDeviceKey(t, sigVectorSeedHex)
	srv, _ := newTestServer(t)
	deviceKey := pageossig.EncodePubkey(pub)

	// Sign with a timestamp ~1 hour in the past.
	req := signedPull(t, priv, "/pull/"+deviceKey, time.Now().Add(-1*time.Hour))
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", rec.Code)
	}
}

func TestPullCrossDeviceSignatureReturns401(t *testing.T) {
	pubA, privA := newDeviceKey(t, sigVectorSeedHex)
	_, _ = newDeviceKey(t, "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20")
	pubB, _ := newDeviceKey(t, "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20")

	srv, _ := newTestServer(t)
	deviceKeyB := pageossig.EncodePubkey(pubB)

	// Sign request for /pull/<deviceKeyB> but using deviceA's key. The signing
	// over deviceB's URL will validate against deviceA's pubkey (which is in
	// the headers), but the URL pubkey says deviceB → mismatch → 401.
	req := signedPull(t, privA, "/pull/"+deviceKeyB, time.Now())
	if got := req.Header.Get(pageossig.HeaderDevice); got != pageossig.EncodePubkey(pubA) {
		t.Fatalf("setup: expected device A header, got %s", got)
	}
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusUnauthorized {
		body, _ := io.ReadAll(rec.Body)
		t.Fatalf("status = %d, want 401; body=%s", rec.Code, string(body))
	}
}

func TestPullInvalidDeviceKeyInURLReturns401(t *testing.T) {
	srv, _ := newTestServer(t)
	req := httptest.NewRequest(http.MethodGet, "/pull/not-base64!!!", nil)
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", rec.Code)
	}
}

func TestPullPOSTReturnsMethodNotAllowed(t *testing.T) {
	pub, priv := newDeviceKey(t, sigVectorSeedHex)
	srv, _ := newTestServer(t)
	deviceKey := pageossig.EncodePubkey(pub)

	req := httptest.NewRequest(http.MethodPost, "/pull/"+deviceKey, nil)
	headers, _ := pageossig.Sign(http.MethodPost, req.URL.RequestURI(), nil, priv, time.Now())
	for k, v := range headers {
		req.Header.Set(k, v)
	}
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	// Go 1.22+ method-aware mux returns 405 for unmatched methods on a matched path.
	if rec.Code != http.StatusMethodNotAllowed {
		t.Fatalf("status = %d, want 405", rec.Code)
	}
}
