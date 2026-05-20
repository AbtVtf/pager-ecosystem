package server

import (
	"context"
	"crypto/ed25519"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/pageros/pageros/push-relay/internal/pageossig"
	"github.com/pageros/pageros/push-relay/internal/storage"
)

// signedAck builds a DELETE /pull/<deviceKey>/<id> request signed by priv.
func signedAck(t *testing.T, priv ed25519.PrivateKey, deviceKey, id string, at time.Time) *http.Request {
	t.Helper()
	target := "/pull/" + deviceKey + "/" + id
	req := httptest.NewRequest(http.MethodDelete, target, nil)
	headers, _ := pageossig.Sign(http.MethodDelete, req.URL.RequestURI(), nil, priv, at)
	for k, v := range headers {
		req.Header.Set(k, v)
	}
	return req
}

// --- happy path + idempotency -------------------------------------------- //

func TestAckHappyPath(t *testing.T) {
	pub, priv := newDeviceKey(t, sigVectorSeedHex)
	srv, store := newTestServer(t)
	deviceKey := pageossig.EncodePubkey(pub)

	got, err := store.Enqueue(context.Background(), deviceKey, storage.Notification{
		Kind: storage.KindNotification, AppID: "app.test", Payload: []byte("payload-one"),
	})
	if err != nil {
		t.Fatalf("Enqueue: %v", err)
	}

	req := signedAck(t, priv, deviceKey, got.ID, time.Now())
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusNoContent {
		t.Fatalf("status = %d, want 204; body=%s", rec.Code, rec.Body.String())
	}

	remaining, err := store.List(context.Background(), deviceKey)
	if err != nil {
		t.Fatalf("List: %v", err)
	}
	if len(remaining) != 0 {
		t.Fatalf("expected queue empty after ack, got %d entries", len(remaining))
	}
}

func TestAckIsIdempotent(t *testing.T) {
	// Acking the same id twice — or acking an id that was never enqueued —
	// MUST be a 204. SPEC PUSH-004: "Removes acked notification from queue;
	// idempotent."
	pub, priv := newDeviceKey(t, sigVectorSeedHex)
	srv, store := newTestServer(t)
	deviceKey := pageossig.EncodePubkey(pub)

	enqueued, err := store.Enqueue(context.Background(), deviceKey, storage.Notification{
		Kind: storage.KindNotification, AppID: "app.test", Payload: []byte("x"),
	})
	if err != nil {
		t.Fatalf("Enqueue: %v", err)
	}

	// First ack: removes.
	req1 := signedAck(t, priv, deviceKey, enqueued.ID, time.Now())
	rec1 := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec1, req1)
	if rec1.Code != http.StatusNoContent {
		t.Fatalf("first ack status = %d, want 204", rec1.Code)
	}

	// Second ack: still 204.
	req2 := signedAck(t, priv, deviceKey, enqueued.ID, time.Now())
	rec2 := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec2, req2)
	if rec2.Code != http.StatusNoContent {
		t.Fatalf("second ack status = %d, want 204 (idempotent)", rec2.Code)
	}

	// Acking an unknown id: still 204.
	req3 := signedAck(t, priv, deviceKey, "deadbeefdeadbeefdeadbeefdeadbeef", time.Now())
	rec3 := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec3, req3)
	if rec3.Code != http.StatusNoContent {
		t.Fatalf("unknown-id ack status = %d, want 204 (idempotent)", rec3.Code)
	}
}

func TestAckLeavesOtherEntriesAlone(t *testing.T) {
	pub, priv := newDeviceKey(t, sigVectorSeedHex)
	srv, store := newTestServer(t)
	deviceKey := pageossig.EncodePubkey(pub)

	a, _ := store.Enqueue(context.Background(), deviceKey, storage.Notification{Payload: []byte("a")})
	b, _ := store.Enqueue(context.Background(), deviceKey, storage.Notification{Payload: []byte("b")})

	req := signedAck(t, priv, deviceKey, a.ID, time.Now())
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusNoContent {
		t.Fatalf("status = %d, want 204", rec.Code)
	}

	remaining, _ := store.List(context.Background(), deviceKey)
	if len(remaining) != 1 || remaining[0].ID != b.ID {
		t.Fatalf("ack should remove only the named entry; remaining=%+v", remaining)
	}
}

// --- auth surface --------------------------------------------------------- //

func TestAckMissingHeadersReturns401(t *testing.T) {
	pub, _ := newDeviceKey(t, sigVectorSeedHex)
	srv, _ := newTestServer(t)
	deviceKey := pageossig.EncodePubkey(pub)

	req := httptest.NewRequest(http.MethodDelete, "/pull/"+deviceKey+"/somenid", nil)
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", rec.Code)
	}
}

func TestAckBadSignatureReturns401(t *testing.T) {
	pub, priv := newDeviceKey(t, sigVectorSeedHex)
	srv, _ := newTestServer(t)
	deviceKey := pageossig.EncodePubkey(pub)

	req := signedAck(t, priv, deviceKey, "abc", time.Now())
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

func TestAckStaleTimestampReturns401(t *testing.T) {
	pub, priv := newDeviceKey(t, sigVectorSeedHex)
	srv, _ := newTestServer(t)
	deviceKey := pageossig.EncodePubkey(pub)

	req := signedAck(t, priv, deviceKey, "abc", time.Now().Add(-1*time.Hour))
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", rec.Code)
	}
}

func TestAckCrossDeviceSignatureReturns401(t *testing.T) {
	// Same defense as TestPullCrossDeviceSignatureReturns401: an otherwise-
	// valid sig signed by device A must not delete entries from device B's
	// queue, even if the URL path encodes deviceB.
	_, privA := newDeviceKey(t, sigVectorSeedHex)
	pubB, _ := newDeviceKey(t, "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20")

	srv, store := newTestServer(t)
	deviceKeyB := pageossig.EncodePubkey(pubB)

	stored, _ := store.Enqueue(context.Background(), deviceKeyB, storage.Notification{
		Payload: []byte("bs-payload"),
	})

	req := signedAck(t, privA, deviceKeyB, stored.ID, time.Now())
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", rec.Code)
	}

	// And the entry must still be there.
	remaining, _ := store.List(context.Background(), deviceKeyB)
	if len(remaining) != 1 {
		t.Fatalf("cross-device ack should not affect target queue; remaining=%d", len(remaining))
	}
}

func TestAckInvalidDeviceKeyInURLReturns401(t *testing.T) {
	srv, _ := newTestServer(t)
	req := httptest.NewRequest(http.MethodDelete, "/pull/not-base64!!!/abc", nil)
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", rec.Code)
	}
}

// --- method routing ------------------------------------------------------- //

func TestAckGETReturnsMethodNotAllowed(t *testing.T) {
	pub, _ := newDeviceKey(t, sigVectorSeedHex)
	srv, _ := newTestServer(t)
	deviceKey := pageossig.EncodePubkey(pub)

	// GET on a /pull/<device>/<id> path doesn't hit the GET /pull/{device}
	// route (different segment count), so Go's method-aware mux returns
	// 405 for the DELETE-only route.
	req := httptest.NewRequest(http.MethodGet, "/pull/"+deviceKey+"/abc", nil)
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusMethodNotAllowed {
		t.Fatalf("status = %d, want 405", rec.Code)
	}
}
