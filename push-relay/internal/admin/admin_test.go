package admin

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func fixedClock(t time.Time) func() time.Time { return func() time.Time { return t } }

func TestStoreRecordSendAggregates(t *testing.T) {
	t0 := time.Date(2026, 5, 20, 12, 0, 0, 0, time.UTC)
	s := NewWithClock(fixedClock(t0))

	s.RecordSend("notes.mafu.dev", "deviceA")
	s.RecordSend("notes.mafu.dev", "deviceA")
	s.RecordSend("notes.mafu.dev", "deviceB")
	s.RecordSend("chat.mafu.dev", "deviceA")

	snap := s.Snapshot()
	if len(snap.Apps) != 2 {
		t.Fatalf("apps len=%d, want 2", len(snap.Apps))
	}
	// Highest-volume first.
	if snap.Apps[0].AppID != "notes.mafu.dev" || snap.Apps[0].Total != 3 {
		t.Fatalf("top app = %+v, want notes.mafu.dev/3", snap.Apps[0])
	}
	if snap.Apps[0].UniqueDevs != 2 {
		t.Fatalf("unique devs = %d, want 2", snap.Apps[0].UniqueDevs)
	}
	if snap.Apps[1].AppID != "chat.mafu.dev" || snap.Apps[1].Total != 1 {
		t.Fatalf("second app = %+v", snap.Apps[1])
	}
	if !snap.Apps[0].FirstSeenAt.Equal(t0) {
		t.Fatalf("first_seen_at = %v, want %v", snap.Apps[0].FirstSeenAt, t0)
	}

	if len(snap.Devices) != 2 {
		t.Fatalf("devices len=%d, want 2", len(snap.Devices))
	}
	if snap.Devices[0].DevicePubkey != "deviceA" || snap.Devices[0].Total != 3 {
		t.Fatalf("top device = %+v", snap.Devices[0])
	}
	if snap.Devices[0].UniqueApps != 2 {
		t.Fatalf("unique apps for deviceA = %d, want 2", snap.Devices[0].UniqueApps)
	}
}

func TestStoreRecordSendIgnoresEmpty(t *testing.T) {
	s := New()
	s.RecordSend("", "deviceA")
	s.RecordSend("notes.mafu.dev", "")
	if snap := s.Snapshot(); len(snap.Apps) != 0 || len(snap.Devices) != 0 {
		t.Fatalf("empty inputs leaked into store: %+v", snap)
	}
}

func TestStoreBanFlow(t *testing.T) {
	s := New()
	if s.IsBanned("notes.mafu.dev") {
		t.Fatalf("expected not banned initially")
	}
	b := s.Ban("notes.mafu.dev", "spam")
	if b.AppID != "notes.mafu.dev" || b.Reason != "spam" {
		t.Fatalf("ban = %+v", b)
	}
	if !s.IsBanned("notes.mafu.dev") {
		t.Fatalf("expected banned after Ban")
	}
	if bans := s.ListBans(); len(bans) != 1 || bans[0].AppID != "notes.mafu.dev" {
		t.Fatalf("bans = %+v", bans)
	}
	// Idempotent re-ban updates reason.
	b2 := s.Ban("notes.mafu.dev", "renamed reason")
	if b2.Reason != "renamed reason" {
		t.Fatalf("re-ban did not update reason: %+v", b2)
	}
	if bans := s.ListBans(); len(bans) != 1 {
		t.Fatalf("re-ban should not duplicate: %+v", bans)
	}
	if !s.Unban("notes.mafu.dev") {
		t.Fatalf("Unban returned false for known ban")
	}
	if s.Unban("notes.mafu.dev") {
		t.Fatalf("Unban returned true for missing ban")
	}
	if s.IsBanned("notes.mafu.dev") {
		t.Fatalf("still banned after Unban")
	}
}

func TestStoreBannedFlagSurfacesInSnapshot(t *testing.T) {
	s := New()
	s.RecordSend("evil.mafu.dev", "device1")
	s.Ban("evil.mafu.dev", "abuse")
	snap := s.Snapshot()
	if len(snap.Apps) != 1 || !snap.Apps[0].Banned {
		t.Fatalf("expected banned flag set in snapshot: %+v", snap.Apps)
	}
}

// --- HTTP -------------------------------------------------------------- //

func newAdminHandler(t *testing.T, token string) (http.Handler, *Store) {
	t.Helper()
	store := New()
	mux := http.NewServeMux()
	Mount(mux, MountConfig{Store: store, Token: token})
	return mux, store
}

func TestAdminMetricsHappyPath(t *testing.T) {
	h, store := newAdminHandler(t, "supersecrettoken123")
	store.RecordSend("notes.mafu.dev", "deviceA")
	store.Ban("evil.mafu.dev", "spam")

	req := httptest.NewRequest(http.MethodGet, "/admin/metrics", nil)
	req.Header.Set("Authorization", "Bearer supersecrettoken123")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, body=%s", rec.Code, rec.Body.String())
	}
	var m Metrics
	if err := json.NewDecoder(rec.Body).Decode(&m); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(m.Apps) != 1 || m.Apps[0].AppID != "notes.mafu.dev" {
		t.Fatalf("metrics.apps = %+v", m.Apps)
	}
}

func TestAdminMetricsRequiresAuth(t *testing.T) {
	h, _ := newAdminHandler(t, "supersecrettoken123")
	cases := []struct {
		name string
		hdr  string
	}{
		{"missing", ""},
		{"wrong", "Bearer wrongtoken!!!!!!!"},
		{"no_prefix", "supersecrettoken123"},
		{"basic", "Basic c3VwZXJzZWNyZXQ="},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			req := httptest.NewRequest(http.MethodGet, "/admin/metrics", nil)
			if tc.hdr != "" {
				req.Header.Set("Authorization", tc.hdr)
			}
			rec := httptest.NewRecorder()
			h.ServeHTTP(rec, req)
			if rec.Code != http.StatusUnauthorized {
				t.Fatalf("status = %d, want 401", rec.Code)
			}
		})
	}
}

func TestAdminDisabledWhenNoToken(t *testing.T) {
	h, _ := newAdminHandler(t, "")
	req := httptest.NewRequest(http.MethodGet, "/admin/metrics", nil)
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusServiceUnavailable {
		t.Fatalf("status = %d, want 503", rec.Code)
	}
}

func TestAdminBanCreate(t *testing.T) {
	h, store := newAdminHandler(t, "supersecrettoken123")

	body := bytes.NewBufferString(`{"app_id":"evil.mafu.dev","reason":"abuse"}`)
	req := httptest.NewRequest(http.MethodPost, "/admin/bans", body)
	req.Header.Set("Authorization", "Bearer supersecrettoken123")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusCreated {
		t.Fatalf("status = %d, body=%s", rec.Code, rec.Body.String())
	}
	if !store.IsBanned("evil.mafu.dev") {
		t.Fatalf("store does not reflect ban")
	}

	var got Ban
	if err := json.NewDecoder(rec.Body).Decode(&got); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if got.AppID != "evil.mafu.dev" || got.Reason != "abuse" {
		t.Fatalf("response ban = %+v", got)
	}
}

func TestAdminBanInvalidAppID(t *testing.T) {
	h, _ := newAdminHandler(t, "supersecrettoken123")
	body := bytes.NewBufferString(`{"app_id":"NotReverseDNS","reason":"abuse"}`)
	req := httptest.NewRequest(http.MethodPost, "/admin/bans", body)
	req.Header.Set("Authorization", "Bearer supersecrettoken123")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", rec.Code)
	}
}

func TestAdminBanInvalidJSON(t *testing.T) {
	h, _ := newAdminHandler(t, "supersecrettoken123")
	body := bytes.NewBufferString(`not-json`)
	req := httptest.NewRequest(http.MethodPost, "/admin/bans", body)
	req.Header.Set("Authorization", "Bearer supersecrettoken123")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", rec.Code)
	}
}

func TestAdminBanDelete(t *testing.T) {
	h, store := newAdminHandler(t, "supersecrettoken123")
	store.Ban("evil.mafu.dev", "abuse")

	req := httptest.NewRequest(http.MethodDelete, "/admin/bans/evil.mafu.dev", nil)
	req.Header.Set("Authorization", "Bearer supersecrettoken123")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusNoContent {
		t.Fatalf("status = %d, body=%s", rec.Code, rec.Body.String())
	}
	if store.IsBanned("evil.mafu.dev") {
		t.Fatalf("still banned after DELETE")
	}
}

func TestAdminBanDeleteNotFound(t *testing.T) {
	h, _ := newAdminHandler(t, "supersecrettoken123")
	req := httptest.NewRequest(http.MethodDelete, "/admin/bans/evil.mafu.dev", nil)
	req.Header.Set("Authorization", "Bearer supersecrettoken123")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404", rec.Code)
	}
}

func TestAdminBanListSortedByAppID(t *testing.T) {
	h, store := newAdminHandler(t, "supersecrettoken123")
	store.Ban("zeta.mafu.dev", "")
	store.Ban("alpha.mafu.dev", "")

	req := httptest.NewRequest(http.MethodGet, "/admin/bans", nil)
	req.Header.Set("Authorization", "Bearer supersecrettoken123")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d", rec.Code)
	}
	var resp struct {
		Bans []Ban `json:"bans"`
	}
	if err := json.NewDecoder(rec.Body).Decode(&resp); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(resp.Bans) != 2 || resp.Bans[0].AppID != "alpha.mafu.dev" || resp.Bans[1].AppID != "zeta.mafu.dev" {
		t.Fatalf("bans not sorted by id: %+v", resp.Bans)
	}
}

// Regression guard: error responses must not leak token-comparison details
// the way they did in an earlier draft.
func TestAdminAuthErrorIsGeneric(t *testing.T) {
	h, _ := newAdminHandler(t, "supersecrettoken123")
	req := httptest.NewRequest(http.MethodGet, "/admin/metrics", nil)
	req.Header.Set("Authorization", "Bearer wrongtokenwrongtoken!")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	body := strings.TrimSpace(rec.Body.String())
	if body != "Unauthorized" {
		t.Fatalf("expected generic body, got %q", body)
	}
}
