package server

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/pageros/pageros/push-relay/internal/admin"
	"github.com/pageros/pageros/push-relay/internal/manifest"
	"github.com/pageros/pageros/push-relay/internal/pageossig"
	"github.com/pageros/pageros/push-relay/internal/ratelimit"
	"github.com/pageros/pageros/push-relay/internal/storage"
)

// fakeLookup is an in-memory manifest.Lookup. The relay never sees a real
// marketplace under test; this keeps push_test isolated from MKT-002.
type fakeLookup struct {
	keys map[string]ed25519.PublicKey
	// unavailableFor lets one specific id return ErrLookupUnavailable so we can
	// exercise the 503 path without bringing up an HTTP server.
	unavailableFor string
}

func (f *fakeLookup) Pubkey(_ context.Context, appID string) (ed25519.PublicKey, error) {
	if appID == f.unavailableFor {
		return nil, manifest.ErrLookupUnavailable
	}
	pk, ok := f.keys[appID]
	if !ok {
		return nil, manifest.ErrAppNotFound
	}
	return pk, nil
}

// pushTestRig wires up a server with an in-memory store and lookup so each
// test starts from a clean slate.
type pushTestRig struct {
	srv    *Server
	store  storage.Store
	lookup *fakeLookup
	admin  *admin.Store
}

func newPushRig(t *testing.T) *pushTestRig {
	t.Helper()
	store := storage.NewMemory()
	lookup := &fakeLookup{keys: map[string]ed25519.PublicKey{}}
	adm := admin.New()
	srv := New(Options{Storage: store, Manifest: lookup, Admin: adm, BuildTag: "test"})
	return &pushTestRig{srv: srv, store: store, lookup: lookup, admin: adm}
}

func (r *pushTestRig) registerApp(t *testing.T, appID string, priv ed25519.PrivateKey) {
	t.Helper()
	r.lookup.keys[appID] = priv.Public().(ed25519.PublicKey)
}

// newAppKey returns a deterministic Ed25519 keypair seeded from `seedHex`.
// Reusing the helper from pull_test isn't an option because that one was
// declared inside that file's test scope (`newDeviceKey`); the helper here
// reads identically but is locally scoped to keep the test files independent.
func newAppKey(t *testing.T, seedHex string) (ed25519.PublicKey, ed25519.PrivateKey) {
	t.Helper()
	return newDeviceKey(t, seedHex)
}

// signedPush builds a POST /push/<deviceKey> request signed by `priv` as the
// app key. `at` is the embedded PagerOS-Timestamp.
func signedPush(t *testing.T, priv ed25519.PrivateKey, appID, deviceKey string, body []byte, at time.Time) *http.Request {
	t.Helper()
	target := "/push/" + deviceKey
	req := httptest.NewRequest(http.MethodPost, target, bytes.NewReader(body))
	if at.IsZero() {
		at = time.Now()
	}
	tsStr := strconv.FormatInt(at.Unix(), 10)
	input := pageossig.BuildSigningInput(http.MethodPost, req.URL.RequestURI(), tsStr, body)
	sig := ed25519.Sign(priv, input)

	req.Header.Set(HeaderApp, appID)
	req.Header.Set(pageossig.HeaderSig, base64.RawURLEncoding.EncodeToString(sig))
	req.Header.Set(pageossig.HeaderTimestamp, tsStr)
	req.Header.Set("Content-Type", "application/octet-stream")
	return req
}

func devicePubkeyForTest(t *testing.T) string {
	t.Helper()
	// Distinct seed from pull_test's; doesn't matter for /push since the
	// device key is just a URL path on this endpoint.
	pub, _ := newDeviceKey(t, "1111111111111111111111111111111111111111111111111111111111111111")
	return pageossig.EncodePubkey(pub)
}

// --- happy path ---------------------------------------------------------- //

func TestPushHappyPath(t *testing.T) {
	rig := newPushRig(t)
	deviceKey := devicePubkeyForTest(t)

	_, appPriv := newAppKey(t, sigVectorSeedHex)
	rig.registerApp(t, "notes.mafu.dev", appPriv)

	body := []byte("encrypted-envelope-bytes")
	req := signedPush(t, appPriv, "notes.mafu.dev", deviceKey, body, time.Now())

	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusAccepted {
		t.Fatalf("status = %d, body=%s", rec.Code, rec.Body.String())
	}

	var resp pushResponse
	if err := json.NewDecoder(rec.Body).Decode(&resp); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if resp.ID == "" {
		t.Fatalf("expected non-empty notification id")
	}
	if resp.EnqueuedAt == 0 {
		t.Fatalf("expected non-zero enqueued_at")
	}

	// The notification should be retrievable via the storage layer with the
	// exact payload bytes preserved (relay never decrypts).
	stored, err := rig.store.List(context.Background(), deviceKey)
	if err != nil {
		t.Fatalf("List: %v", err)
	}
	if len(stored) != 1 {
		t.Fatalf("got %d stored, want 1", len(stored))
	}
	if stored[0].AppID != "notes.mafu.dev" {
		t.Fatalf("app id not propagated: %q", stored[0].AppID)
	}
	if !bytes.Equal(stored[0].Payload, body) {
		t.Fatalf("payload roundtrip mismatch")
	}
	if stored[0].ID != resp.ID {
		t.Fatalf("response id %q != stored id %q", resp.ID, stored[0].ID)
	}
}

// --- unknown app → 403 (SPEC §6.6.2) ------------------------------------- //

func TestPushUnknownAppReturns403(t *testing.T) {
	rig := newPushRig(t)
	deviceKey := devicePubkeyForTest(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)

	body := []byte("hello")
	req := signedPush(t, appPriv, "ghost.example.dev", deviceKey, body, time.Now())

	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want 403; body=%s", rec.Code, rec.Body.String())
	}
	// Nothing should be enqueued.
	stored, _ := rig.store.List(context.Background(), deviceKey)
	if len(stored) != 0 {
		t.Fatalf("unknown app should not enqueue; got %d", len(stored))
	}
}

func TestPushMalformedAppIDReturns403(t *testing.T) {
	rig := newPushRig(t)
	deviceKey := devicePubkeyForTest(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)

	req := signedPush(t, appPriv, "NotReverseDNS", deviceKey, []byte("x"), time.Now())
	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want 403", rec.Code)
	}
}

// --- bad sig / replay / missing headers → 401 ---------------------------- //

func TestPushBadSignatureReturns401(t *testing.T) {
	rig := newPushRig(t)
	deviceKey := devicePubkeyForTest(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)
	rig.registerApp(t, "notes.mafu.dev", appPriv)

	req := signedPush(t, appPriv, "notes.mafu.dev", deviceKey, []byte("payload"), time.Now())
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
	rig.srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", rec.Code)
	}
}

func TestPushWrongAppKeyReturns401(t *testing.T) {
	rig := newPushRig(t)
	deviceKey := devicePubkeyForTest(t)

	// Two apps: registered as `notes.mafu.dev`, but sign with the *other*
	// app's private key. The marketplace lookup succeeds (the app id is
	// known), but the sig won't verify → 401.
	_, registeredPriv := newAppKey(t, sigVectorSeedHex)
	rig.registerApp(t, "notes.mafu.dev", registeredPriv)

	_, attackerPriv := newAppKey(t, "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20")

	req := signedPush(t, attackerPriv, "notes.mafu.dev", deviceKey, []byte("x"), time.Now())
	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", rec.Code)
	}
}

func TestPushStaleTimestampReturns401(t *testing.T) {
	rig := newPushRig(t)
	deviceKey := devicePubkeyForTest(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)
	rig.registerApp(t, "notes.mafu.dev", appPriv)

	req := signedPush(t, appPriv, "notes.mafu.dev", deviceKey, []byte("x"), time.Now().Add(-1*time.Hour))
	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", rec.Code)
	}
}

func TestPushMissingAppHeaderReturns401(t *testing.T) {
	rig := newPushRig(t)
	deviceKey := devicePubkeyForTest(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)

	req := signedPush(t, appPriv, "notes.mafu.dev", deviceKey, []byte("x"), time.Now())
	req.Header.Del(HeaderApp)

	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", rec.Code)
	}
}

func TestPushMissingSigHeadersReturns401(t *testing.T) {
	rig := newPushRig(t)
	deviceKey := devicePubkeyForTest(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)
	rig.registerApp(t, "notes.mafu.dev", appPriv)

	// Try each individual missing header in turn.
	for _, header := range []string{pageossig.HeaderSig, pageossig.HeaderTimestamp} {
		t.Run(header, func(t *testing.T) {
			req := signedPush(t, appPriv, "notes.mafu.dev", deviceKey, []byte("x"), time.Now())
			req.Header.Del(header)
			rec := httptest.NewRecorder()
			rig.srv.Handler().ServeHTTP(rec, req)
			if rec.Code != http.StatusUnauthorized {
				t.Fatalf("status = %d, want 401", rec.Code)
			}
		})
	}
}

// --- request shape / limits ---------------------------------------------- //

func TestPushEmptyBodyReturns400(t *testing.T) {
	rig := newPushRig(t)
	deviceKey := devicePubkeyForTest(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)
	rig.registerApp(t, "notes.mafu.dev", appPriv)

	req := signedPush(t, appPriv, "notes.mafu.dev", deviceKey, nil, time.Now())
	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", rec.Code)
	}
}

func TestPushBodyTooLargeReturns413(t *testing.T) {
	store := storage.NewMemory()
	lookup := &fakeLookup{keys: map[string]ed25519.PublicKey{}}
	srv := New(Options{Storage: store, Manifest: lookup, MaxPushBytes: 32, BuildTag: "test"})

	deviceKey := devicePubkeyForTest(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)
	lookup.keys["notes.mafu.dev"] = appPriv.Public().(ed25519.PublicKey)

	body := bytes.Repeat([]byte("A"), 512)
	req := signedPush(t, appPriv, "notes.mafu.dev", deviceKey, body, time.Now())

	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusRequestEntityTooLarge {
		t.Fatalf("status = %d, want 413; body=%s", rec.Code, rec.Body.String())
	}
}

func TestPushInvalidDeviceKeyReturns400(t *testing.T) {
	rig := newPushRig(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)
	rig.registerApp(t, "notes.mafu.dev", appPriv)

	req := signedPush(t, appPriv, "notes.mafu.dev", "not-base64!!!", []byte("x"), time.Now())
	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", rec.Code)
	}
}

// --- registry availability ------------------------------------------------ //

func TestPushMarketplaceUnavailableReturns503(t *testing.T) {
	rig := newPushRig(t)
	rig.lookup.unavailableFor = "notes.mafu.dev"

	deviceKey := devicePubkeyForTest(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)

	req := signedPush(t, appPriv, "notes.mafu.dev", deviceKey, []byte("x"), time.Now())
	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusServiceUnavailable {
		t.Fatalf("status = %d, want 503", rec.Code)
	}
}

func TestPushNoLookupConfiguredReturns503(t *testing.T) {
	store := storage.NewMemory()
	srv := New(Options{Storage: store, BuildTag: "test"}) // no Manifest

	deviceKey := devicePubkeyForTest(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)

	req := signedPush(t, appPriv, "notes.mafu.dev", deviceKey, []byte("x"), time.Now())
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusServiceUnavailable {
		t.Fatalf("status = %d, want 503", rec.Code)
	}
}

// --- method routing ------------------------------------------------------- //

func TestPushGETReturnsMethodNotAllowed(t *testing.T) {
	rig := newPushRig(t)
	deviceKey := devicePubkeyForTest(t)

	req := httptest.NewRequest(http.MethodGet, "/push/"+deviceKey, nil)
	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusMethodNotAllowed {
		t.Fatalf("status = %d, want 405", rec.Code)
	}
}

// --- regression guard: error body should not leak details --------------- //

func TestPushErrorBodiesAreGeneric(t *testing.T) {
	rig := newPushRig(t)
	deviceKey := devicePubkeyForTest(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)
	rig.registerApp(t, "notes.mafu.dev", appPriv)

	// Tamper the sig — server should answer 401 with just "Unauthorized",
	// not a reason string. This guards the same oracle defence pull.go has.
	req := signedPush(t, appPriv, "notes.mafu.dev", deviceKey, []byte("x"), time.Now())
	req.Header.Set(pageossig.HeaderSig, strings.Repeat("A", 86)) // valid b64 length, wrong content
	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", rec.Code)
	}
	body := strings.TrimSpace(rec.Body.String())
	if body != "Unauthorized" {
		t.Fatalf("expected generic body %q, got %q", "Unauthorized", body)
	}
}

// --- PUSH-008: ban check + volume recording ----------------------------- //

func TestPushBannedAppReturns403(t *testing.T) {
	rig := newPushRig(t)
	deviceKey := devicePubkeyForTest(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)
	rig.registerApp(t, "notes.mafu.dev", appPriv)
	rig.admin.Ban("notes.mafu.dev", "spam")

	req := signedPush(t, appPriv, "notes.mafu.dev", deviceKey, []byte("x"), time.Now())
	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want 403; body=%s", rec.Code, rec.Body.String())
	}
	// Nothing should be enqueued.
	stored, _ := rig.store.List(context.Background(), deviceKey)
	if len(stored) != 0 {
		t.Fatalf("banned app should not enqueue; got %d", len(stored))
	}
}

func TestPushRecordsSendOnSuccess(t *testing.T) {
	rig := newPushRig(t)
	deviceKey := devicePubkeyForTest(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)
	rig.registerApp(t, "notes.mafu.dev", appPriv)

	req := signedPush(t, appPriv, "notes.mafu.dev", deviceKey, []byte("hello"), time.Now())
	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusAccepted {
		t.Fatalf("status = %d", rec.Code)
	}
	snap := rig.admin.Snapshot()
	if len(snap.Apps) != 1 || snap.Apps[0].Total != 1 {
		t.Fatalf("admin store did not record send: %+v", snap)
	}
	if snap.Devices[0].DevicePubkey != deviceKey {
		t.Fatalf("device pubkey mismatch in admin store: %+v", snap.Devices)
	}
}

func TestPushDoesNotRecordOnFailure(t *testing.T) {
	rig := newPushRig(t)
	deviceKey := devicePubkeyForTest(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)
	// No app registered → 403, must not record.

	req := signedPush(t, appPriv, "notes.mafu.dev", deviceKey, []byte("x"), time.Now())
	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusForbidden {
		t.Fatalf("status = %d", rec.Code)
	}
	if snap := rig.admin.Snapshot(); len(snap.Apps) != 0 {
		t.Fatalf("admin store should not record failed sends: %+v", snap)
	}
}

// --- PUSH-006: rate-limit overflow returns 429 + Retry-After ------------ //

func TestPushRateLimitOverflowReturns429(t *testing.T) {
	// Tighten the limit so we don't have to make 60 signed requests.
	store := storage.NewMemory()
	lookup := &fakeLookup{keys: map[string]ed25519.PublicKey{}}
	adm := admin.New()
	lim := ratelimit.NewMemoryWithLimits(1, 5)
	srv := New(Options{Storage: store, Manifest: lookup, Admin: adm, Limiter: lim, BuildTag: "test"})

	deviceKey := devicePubkeyForTest(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)
	lookup.keys["notes.mafu.dev"] = appPriv.Public().(ed25519.PublicKey)

	// 1st send fits the bucket.
	req := signedPush(t, appPriv, "notes.mafu.dev", deviceKey, []byte("a"), time.Now())
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusAccepted {
		t.Fatalf("warmup status = %d", rec.Code)
	}

	// 2nd is over cap → 429 with Retry-After.
	req2 := signedPush(t, appPriv, "notes.mafu.dev", deviceKey, []byte("b"), time.Now())
	rec2 := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec2, req2)
	if rec2.Code != http.StatusTooManyRequests {
		t.Fatalf("overflow status = %d, want 429; body=%s", rec2.Code, rec2.Body.String())
	}
	if ra := rec2.Header().Get("Retry-After"); ra == "" {
		t.Fatalf("missing Retry-After on 429")
	}
	// Storage should still hold just the one accepted send.
	stored, _ := store.List(context.Background(), deviceKey)
	if len(stored) != 1 {
		t.Fatalf("expected exactly 1 stored notification, got %d", len(stored))
	}
}

// Rate limit must fire AFTER sig verify so a forger can't drain the bucket.
func TestPushRateLimitNotBurnedByBadSignatures(t *testing.T) {
	store := storage.NewMemory()
	lookup := &fakeLookup{keys: map[string]ed25519.PublicKey{}}
	adm := admin.New()
	lim := ratelimit.NewMemoryWithLimits(1, 5)
	srv := New(Options{Storage: store, Manifest: lookup, Admin: adm, Limiter: lim, BuildTag: "test"})

	deviceKey := devicePubkeyForTest(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)
	lookup.keys["notes.mafu.dev"] = appPriv.Public().(ed25519.PublicKey)

	// Send 5 requests with bad sigs — none should consume budget.
	for i := 0; i < 5; i++ {
		req := signedPush(t, appPriv, "notes.mafu.dev", deviceKey, []byte("x"), time.Now())
		// Replace sig with a syntactically-valid garbage sig.
		req.Header.Set(pageossig.HeaderSig, strings.Repeat("A", 86))
		rec := httptest.NewRecorder()
		srv.Handler().ServeHTTP(rec, req)
		if rec.Code != http.StatusUnauthorized {
			t.Fatalf("iter %d: status = %d, want 401", i, rec.Code)
		}
	}
	// Now the legitimate request must still succeed (budget intact).
	req := signedPush(t, appPriv, "notes.mafu.dev", deviceKey, []byte("ok"), time.Now())
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusAccepted {
		t.Fatalf("legit send after bad-sig burst: status = %d", rec.Code)
	}
}

// --- guard: ensure manifest errors flow correctly through errors.Is ----- //

func TestPushManifestErrorsAreDistinct(t *testing.T) {
	if !errors.Is(manifest.ErrAppNotFound, manifest.ErrAppNotFound) {
		t.Fatalf("errors.Is sanity check failed")
	}
	if errors.Is(manifest.ErrAppNotFound, manifest.ErrLookupUnavailable) {
		t.Fatalf("ErrAppNotFound should NOT be ErrLookupUnavailable")
	}
}
