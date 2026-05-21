package server

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strconv"
	"testing"
	"time"

	"github.com/pageros/pageros/push-relay/internal/admin"
	"github.com/pageros/pageros/push-relay/internal/pageossig"
	"github.com/pageros/pageros/push-relay/internal/ratelimit"
	"github.com/pageros/pageros/push-relay/internal/storage"
)

// groupTestRig wires up a full server pointed at in-memory backends so each
// group_push case starts from a clean slate.
type groupTestRig struct {
	srv          *Server
	store        storage.Store
	lookup       *fakeLookup
	admin        *admin.Store
	pushLimiter  ratelimit.Limiter
	groupLimiter ratelimit.Limiter
}

func newGroupRig(t *testing.T, groupHour, groupDay int) *groupTestRig {
	t.Helper()
	store := storage.NewMemory()
	lookup := &fakeLookup{keys: map[string]ed25519.PublicKey{}}
	adm := admin.New()
	push := ratelimit.NewMemory()
	group := ratelimit.NewMemoryWithLimits(groupHour, groupDay)
	srv := New(Options{
		Storage:      store,
		Manifest:     lookup,
		Admin:        adm,
		Limiter:      push,
		GroupLimiter: group,
		BuildTag:     "test",
	})
	return &groupTestRig{
		srv:          srv,
		store:        store,
		lookup:       lookup,
		admin:        adm,
		pushLimiter:  push,
		groupLimiter: group,
	}
}

// twoDevices returns two distinct device pubkey strings for fan-out tests.
func twoDevices(t *testing.T) (string, string) {
	t.Helper()
	pubA, _ := newDeviceKey(t, "2222222222222222222222222222222222222222222222222222222222222222")
	pubB, _ := newDeviceKey(t, "3333333333333333333333333333333333333333333333333333333333333333")
	return pageossig.EncodePubkey(pubA), pageossig.EncodePubkey(pubB)
}

// signedGroupPush builds a POST /group_push request signed by `priv` over the
// serialized body. `payloads` is keyed by device pubkey → opaque bytes.
func signedGroupPush(t *testing.T, priv ed25519.PrivateKey, appID string, payloads map[string][]byte, at time.Time) *http.Request {
	t.Helper()
	recipients := make([]groupRecipient, 0, len(payloads))
	for dev, p := range payloads {
		recipients = append(recipients, groupRecipient{
			DevicePubkey: dev,
			PayloadB64:   base64.RawURLEncoding.EncodeToString(p),
		})
	}
	body, err := json.Marshal(groupPushRequest{Recipients: recipients})
	if err != nil {
		t.Fatalf("marshal request: %v", err)
	}
	req := httptest.NewRequest(http.MethodPost, "/group_push", bytes.NewReader(body))
	if at.IsZero() {
		at = time.Now()
	}
	tsStr := strconv.FormatInt(at.Unix(), 10)
	input := pageossig.BuildSigningInput(http.MethodPost, req.URL.RequestURI(), tsStr, body)
	sig := ed25519.Sign(priv, input)
	req.Header.Set(HeaderApp, appID)
	req.Header.Set(pageossig.HeaderSig, base64.RawURLEncoding.EncodeToString(sig))
	req.Header.Set(pageossig.HeaderTimestamp, tsStr)
	req.Header.Set("Content-Type", "application/json")
	return req
}

// --- happy path ---------------------------------------------------------- //

func TestGroupPushFansOutToAllDevices(t *testing.T) {
	rig := newGroupRig(t, ratelimit.HourLimit, ratelimit.DayLimit)
	devA, devB := twoDevices(t)

	_, appPriv := newAppKey(t, sigVectorSeedHex)
	rig.lookup.keys["chat.mafu.dev"] = appPriv.Public().(ed25519.PublicKey)

	payloads := map[string][]byte{
		devA: []byte("ciphertext-for-A"),
		devB: []byte("ciphertext-for-B"),
	}
	req := signedGroupPush(t, appPriv, "chat.mafu.dev", payloads, time.Now())
	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusAccepted {
		t.Fatalf("status = %d; body=%s", rec.Code, rec.Body.String())
	}

	var resp groupPushResponse
	if err := json.NewDecoder(rec.Body).Decode(&resp); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(resp.Results) != 2 {
		t.Fatalf("results len = %d, want 2", len(resp.Results))
	}
	for _, r := range resp.Results {
		if r.Status != GroupResultAccepted {
			t.Fatalf("recipient %s status = %s, want accepted", r.DevicePubkey, r.Status)
		}
		if r.ID == "" || r.EnqueuedAt == 0 {
			t.Fatalf("recipient %s missing id/enqueued_at: %+v", r.DevicePubkey, r)
		}
	}

	// Storage layer should hold one kind:group_event per device with the
	// matching payload roundtripped byte-for-byte (relay never decrypts).
	for dev, want := range payloads {
		stored, err := rig.store.List(context.Background(), dev)
		if err != nil {
			t.Fatalf("List(%s): %v", dev, err)
		}
		if len(stored) != 1 {
			t.Fatalf("device %s: got %d stored, want 1", dev, len(stored))
		}
		if stored[0].Kind != storage.KindGroupEvent {
			t.Fatalf("device %s kind = %q, want %q", dev, stored[0].Kind, storage.KindGroupEvent)
		}
		if !bytes.Equal(stored[0].Payload, want) {
			t.Fatalf("device %s payload roundtrip mismatch", dev)
		}
	}
}

// --- per-recipient errors don't abort the rest of the batch -------------- //

func TestGroupPushReportsInvalidRecipientPerEntry(t *testing.T) {
	rig := newGroupRig(t, ratelimit.HourLimit, ratelimit.DayLimit)
	devA, _ := twoDevices(t)

	_, appPriv := newAppKey(t, sigVectorSeedHex)
	rig.lookup.keys["chat.mafu.dev"] = appPriv.Public().(ed25519.PublicKey)

	payloads := map[string][]byte{
		devA:             []byte("ciphertext-for-A"),
		"not-base64!!!!": []byte("won't reach storage"),
	}
	req := signedGroupPush(t, appPriv, "chat.mafu.dev", payloads, time.Now())
	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusAccepted {
		t.Fatalf("status = %d", rec.Code)
	}

	var resp groupPushResponse
	_ = json.NewDecoder(rec.Body).Decode(&resp)
	var aOK, badRejected bool
	for _, r := range resp.Results {
		if r.DevicePubkey == devA && r.Status == GroupResultAccepted {
			aOK = true
		}
		if r.DevicePubkey == "not-base64!!!!" && r.Status == GroupResultBadDevice {
			badRejected = true
		}
	}
	if !aOK || !badRejected {
		t.Fatalf("partial fan-out results mismatched: %+v", resp.Results)
	}
}

// --- separate rate-limit bucket from /push (PUSH-007 acceptance) -------- //

func TestGroupPushBucketIndependentFromPushBucket(t *testing.T) {
	// Set group limit to 1/hour; /push limit is the spec default. A single
	// group send must not exhaust /push's budget, and a single /push must
	// not exhaust the group budget.
	rig := newGroupRig(t, 1, 5)
	devA, _ := twoDevices(t)

	_, appPriv := newAppKey(t, sigVectorSeedHex)
	rig.lookup.keys["chat.mafu.dev"] = appPriv.Public().(ed25519.PublicKey)

	// First, exhaust the group bucket for (chat.mafu.dev, devA).
	req := signedGroupPush(t, appPriv, "chat.mafu.dev",
		map[string][]byte{devA: []byte("x")}, time.Now())
	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusAccepted {
		t.Fatalf("warmup status = %d", rec.Code)
	}

	// Second group send to the same device → rate_limited per-recipient.
	req2 := signedGroupPush(t, appPriv, "chat.mafu.dev",
		map[string][]byte{devA: []byte("y")}, time.Now())
	rec2 := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec2, req2)
	if rec2.Code != http.StatusAccepted {
		t.Fatalf("second group status = %d", rec2.Code)
	}
	var resp groupPushResponse
	_ = json.NewDecoder(rec2.Body).Decode(&resp)
	if len(resp.Results) != 1 || resp.Results[0].Status != GroupResultRateLimited {
		t.Fatalf("expected rate_limited, got %+v", resp.Results)
	}
	if resp.Results[0].RetryAfter <= 0 {
		t.Fatalf("expected positive retry_after, got %d", resp.Results[0].RetryAfter)
	}

	// A direct /push to the same tuple must still succeed — the buckets are
	// independent.
	pushReq := signedPush(t, appPriv, "chat.mafu.dev", devA, []byte("z"), time.Now())
	pushRec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(pushRec, pushReq)
	if pushRec.Code != http.StatusAccepted {
		t.Fatalf("/push after group-bucket exhaust: status = %d", pushRec.Code)
	}
}

// --- auth + sig failures reject the whole batch -------------------------- //

func TestGroupPushUnknownAppReturns403(t *testing.T) {
	rig := newGroupRig(t, ratelimit.HourLimit, ratelimit.DayLimit)
	devA, _ := twoDevices(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)

	req := signedGroupPush(t, appPriv, "ghost.example.dev",
		map[string][]byte{devA: []byte("x")}, time.Now())
	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want 403", rec.Code)
	}
}

func TestGroupPushBannedAppReturns403(t *testing.T) {
	rig := newGroupRig(t, ratelimit.HourLimit, ratelimit.DayLimit)
	devA, _ := twoDevices(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)
	rig.lookup.keys["chat.mafu.dev"] = appPriv.Public().(ed25519.PublicKey)
	rig.admin.Ban("chat.mafu.dev", "abuse")

	req := signedGroupPush(t, appPriv, "chat.mafu.dev",
		map[string][]byte{devA: []byte("x")}, time.Now())
	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want 403", rec.Code)
	}
}

func TestGroupPushBadSignatureReturns401(t *testing.T) {
	rig := newGroupRig(t, ratelimit.HourLimit, ratelimit.DayLimit)
	devA, _ := twoDevices(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)
	rig.lookup.keys["chat.mafu.dev"] = appPriv.Public().(ed25519.PublicKey)

	req := signedGroupPush(t, appPriv, "chat.mafu.dev",
		map[string][]byte{devA: []byte("x")}, time.Now())
	// Flip a byte in the sig.
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

// --- request-shape validation ------------------------------------------- //

func TestGroupPushEmptyRecipientsReturns400(t *testing.T) {
	rig := newGroupRig(t, ratelimit.HourLimit, ratelimit.DayLimit)
	_, appPriv := newAppKey(t, sigVectorSeedHex)
	rig.lookup.keys["chat.mafu.dev"] = appPriv.Public().(ed25519.PublicKey)

	req := signedGroupPush(t, appPriv, "chat.mafu.dev", map[string][]byte{}, time.Now())
	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", rec.Code)
	}
}

func TestGroupPushMalformedJSONReturns400(t *testing.T) {
	rig := newGroupRig(t, ratelimit.HourLimit, ratelimit.DayLimit)
	_, appPriv := newAppKey(t, sigVectorSeedHex)
	rig.lookup.keys["chat.mafu.dev"] = appPriv.Public().(ed25519.PublicKey)

	body := []byte("not-json")
	req := httptest.NewRequest(http.MethodPost, "/group_push", bytes.NewReader(body))
	ts := strconv.FormatInt(time.Now().Unix(), 10)
	input := pageossig.BuildSigningInput(http.MethodPost, req.URL.RequestURI(), ts, body)
	sig := ed25519.Sign(appPriv, input)
	req.Header.Set(HeaderApp, "chat.mafu.dev")
	req.Header.Set(pageossig.HeaderSig, base64.RawURLEncoding.EncodeToString(sig))
	req.Header.Set(pageossig.HeaderTimestamp, ts)

	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", rec.Code)
	}
}

func TestGroupPushTooManyRecipientsReturns413(t *testing.T) {
	store := storage.NewMemory()
	lookup := &fakeLookup{keys: map[string]ed25519.PublicKey{}}
	adm := admin.New()
	// Allocate a fresh handler with a tight recipient cap.
	groupLim := ratelimit.NewMemory()
	srv := New(Options{Storage: store, Manifest: lookup, Admin: adm, GroupLimiter: groupLim, BuildTag: "test"})

	_, appPriv := newAppKey(t, sigVectorSeedHex)
	lookup.keys["chat.mafu.dev"] = appPriv.Public().(ed25519.PublicKey)

	// Build a request with 65 recipients — one over the default cap of 64.
	payloads := make(map[string][]byte, DefaultMaxGroupRecipients+1)
	// Generate deterministic distinct device pubkeys.
	for i := 0; i <= DefaultMaxGroupRecipients; i++ {
		seed := bytes.Repeat([]byte{byte(i + 1)}, 32)
		pub, _ := newDeviceKey(t, hexN(seed))
		payloads[pageossig.EncodePubkey(pub)] = []byte("x")
	}
	req := signedGroupPush(t, appPriv, "chat.mafu.dev", payloads, time.Now())
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusRequestEntityTooLarge {
		t.Fatalf("status = %d, want 413", rec.Code)
	}
}

func TestGroupPushEmptyPayloadRecordedPerRecipient(t *testing.T) {
	rig := newGroupRig(t, ratelimit.HourLimit, ratelimit.DayLimit)
	devA, _ := twoDevices(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)
	rig.lookup.keys["chat.mafu.dev"] = appPriv.Public().(ed25519.PublicKey)

	req := signedGroupPush(t, appPriv, "chat.mafu.dev",
		map[string][]byte{devA: nil}, time.Now())
	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusAccepted {
		t.Fatalf("status = %d", rec.Code)
	}
	var resp groupPushResponse
	_ = json.NewDecoder(rec.Body).Decode(&resp)
	if len(resp.Results) != 1 || resp.Results[0].Status != GroupResultPayloadEmpty {
		t.Fatalf("expected payload_empty, got %+v", resp.Results)
	}
}

func TestGroupPushAdminVolumesRecorded(t *testing.T) {
	rig := newGroupRig(t, ratelimit.HourLimit, ratelimit.DayLimit)
	devA, devB := twoDevices(t)
	_, appPriv := newAppKey(t, sigVectorSeedHex)
	rig.lookup.keys["chat.mafu.dev"] = appPriv.Public().(ed25519.PublicKey)

	req := signedGroupPush(t, appPriv, "chat.mafu.dev", map[string][]byte{
		devA: []byte("a"), devB: []byte("b"),
	}, time.Now())
	rec := httptest.NewRecorder()
	rig.srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusAccepted {
		t.Fatalf("status = %d", rec.Code)
	}
	snap := rig.admin.Snapshot()
	if len(snap.Apps) != 1 || snap.Apps[0].Total != 2 {
		t.Fatalf("admin snapshot did not record both sends: %+v", snap.Apps)
	}
}

// hexN renders 32 bytes as 64 hex chars without pulling in encoding/hex at the
// top of the file — we already use base64 there.
func hexN(b []byte) string {
	const hex = "0123456789abcdef"
	out := make([]byte, 0, len(b)*2)
	for _, x := range b {
		out = append(out, hex[x>>4], hex[x&0x0f])
	}
	return string(out)
}
