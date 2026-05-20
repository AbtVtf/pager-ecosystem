package publish

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

// fakeMarketplace exposes the publish-related endpoints with the same wire
// shape as the real FastAPI service. It tracks which calls were made so tests
// can assert on flow.
type fakeMarketplace struct {
	mu        *sync.Mutex
	verified  bool
	consumed  bool
	wantToken string
	wantID    string
	wantHost  string

	registerStatus int
	registerBody   string
	verifyStatus   int
	verifyBody     string
	challengeBody  string
}

func newFakeMarketplace(t *testing.T) (*fakeMarketplace, *httptest.Server) {
	t.Helper()
	m := &fakeMarketplace{
		mu:        &sync.Mutex{},
		wantToken: "tok-abc",
		wantID:    "notes.mafu.dev",
		wantHost:  "notes.app",
	}
	srv := httptest.NewServer(http.HandlerFunc(m.ServeHTTP))
	t.Cleanup(srv.Close)
	return m, srv
}

func (f *fakeMarketplace) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	f.mu.Lock()
	defer f.mu.Unlock()

	switch {
	case r.Method == http.MethodPost && r.URL.Path == "/apps/challenges":
		if f.challengeBody != "" {
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusBadRequest)
			io.WriteString(w, f.challengeBody)
			return
		}
		ch := Challenge{
			ID:        "ch-1",
			AppID:     f.wantID,
			Host:      f.wantHost,
			Token:     f.wantToken,
			TxtName:   "_pageros-challenge." + f.wantHost,
			TxtValue:  "v=pageros1;t=" + f.wantToken,
			ExpiresAt: time.Now().Add(15 * time.Minute),
		}
		writeJSON(w, http.StatusCreated, ch)

	case r.Method == http.MethodPost && strings.HasPrefix(r.URL.Path, "/apps/challenges/") && strings.HasSuffix(r.URL.Path, "/verify"):
		if f.verifyStatus != 0 {
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(f.verifyStatus)
			io.WriteString(w, f.verifyBody)
			return
		}
		f.verified = true
		ts := time.Now()
		ch := Challenge{
			ID:         "ch-1",
			AppID:      f.wantID,
			Host:       f.wantHost,
			Token:      f.wantToken,
			TxtName:    "_pageros-challenge." + f.wantHost,
			TxtValue:   "v=pageros1;t=" + f.wantToken,
			ExpiresAt:  time.Now().Add(15 * time.Minute),
			VerifiedAt: &ts,
		}
		writeJSON(w, http.StatusOK, ch)

	case r.Method == http.MethodPost && r.URL.Path == "/apps":
		if f.registerStatus != 0 {
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(f.registerStatus)
			io.WriteString(w, f.registerBody)
			return
		}
		if got := r.Header.Get("X-Challenge-Token"); got != f.wantToken {
			http.Error(w, `{"error":"challenge_required","detail":"missing or wrong token"}`, http.StatusForbidden)
			return
		}
		var m Manifest
		if err := json.NewDecoder(r.Body).Decode(&m); err != nil {
			http.Error(w, `{"error":"bad_body"}`, http.StatusBadRequest)
			return
		}
		f.consumed = true
		rec := AppRecord{
			Manifest:  m,
			Tags:      []string{},
			CreatedAt: time.Now(),
			UpdatedAt: time.Now(),
		}
		writeJSON(w, http.StatusCreated, rec)

	default:
		http.NotFound(w, r)
	}
}

func writeJSON(w http.ResponseWriter, status int, body any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(body)
}

func TestRun_HappyPath(t *testing.T) {
	_, srv := newFakeMarketplace(t)
	path := writeManifest(t, minimalManifest)
	opts := Options{
		ManifestPath: path,
		RegistryURL:  srv.URL,
		ListingBase:  "https://market.example/apps/",
		SkipPrompt:   true,
	}
	var out bytes.Buffer
	res, err := Run(context.Background(), opts, NewClient(srv.URL), nil, &out)
	if err != nil {
		t.Fatalf("Run: %v\n%s", err, out.String())
	}
	if res.AppID != "notes.mafu.dev" {
		t.Errorf("AppID = %q", res.AppID)
	}
	if res.ListingURL != "https://market.example/apps/notes.mafu.dev" {
		t.Errorf("ListingURL = %q", res.ListingURL)
	}
	want := []string{
		"validated locally",
		"DNS TXT challenge",
		"DNS TXT challenge verified",
		"Published notes.mafu.dev",
		"https://market.example/apps/notes.mafu.dev",
	}
	for _, s := range want {
		if !strings.Contains(out.String(), s) {
			t.Errorf("stdout missing %q\n---\n%s", s, out.String())
		}
	}
}

func TestRun_RejectsInvalidManifest(t *testing.T) {
	_, srv := newFakeMarketplace(t)
	body := strings.Replace(minimalManifest, "id: notes.mafu.dev\n", "id: NotReverseDNS\n", 1)
	path := writeManifest(t, body)
	opts := Options{
		ManifestPath: path,
		RegistryURL:  srv.URL,
		SkipPrompt:   true,
	}
	var out bytes.Buffer
	_, err := Run(context.Background(), opts, NewClient(srv.URL), nil, &out)
	if err == nil {
		t.Fatal("expected validation error")
	}
	var ve *ValidationError
	if !errors.As(err, &ve) {
		t.Fatalf("want ValidationError, got %T: %v", err, err)
	}
	if !strings.Contains(err.Error(), "id") {
		t.Errorf("error should call out id: %v", err)
	}
}

func TestRun_VerifyFails(t *testing.T) {
	fake, srv := newFakeMarketplace(t)
	fake.verifyStatus = http.StatusForbidden
	fake.verifyBody = `{"error":"dns_txt_not_found","detail":"no TXT record at _pageros-challenge.notes.app"}`
	path := writeManifest(t, minimalManifest)
	opts := Options{
		ManifestPath: path,
		RegistryURL:  srv.URL,
		SkipPrompt:   true,
	}
	var out bytes.Buffer
	_, err := Run(context.Background(), opts, NewClient(srv.URL), nil, &out)
	if err == nil {
		t.Fatal("expected verify error")
	}
	if !strings.Contains(err.Error(), "dns_txt_not_found") {
		t.Errorf("error should surface api code: %v", err)
	}
}

func TestRun_RegisterReturnsFieldErrors(t *testing.T) {
	fake, srv := newFakeMarketplace(t)
	fake.registerStatus = http.StatusBadRequest
	fake.registerBody = `{"error":"manifest_invalid","detail":"manifest failed schema validation","fields":[{"field":"pubkey","message":"must be base64 encoded 32 bytes"}]}`
	path := writeManifest(t, minimalManifest)
	opts := Options{
		ManifestPath: path,
		RegistryURL:  srv.URL,
		SkipPrompt:   true,
	}
	var out bytes.Buffer
	_, err := Run(context.Background(), opts, NewClient(srv.URL), nil, &out)
	if err == nil {
		t.Fatal("expected register error")
	}
	var ve *ValidationError
	if !errors.As(err, &ve) {
		t.Fatalf("want ValidationError, got %T: %v", err, err)
	}
	if len(ve.Errors) == 0 || ve.Errors[0].Field != "pubkey" {
		t.Errorf("expected pubkey field error, got %+v", ve.Errors)
	}
}

func TestRun_PromptsForConfirmation(t *testing.T) {
	_, srv := newFakeMarketplace(t)
	path := writeManifest(t, minimalManifest)
	opts := Options{
		ManifestPath: path,
		RegistryURL:  srv.URL,
		ListingBase:  "https://market.example/apps/",
	}
	stdin := strings.NewReader("\n")
	var out bytes.Buffer
	res, err := Run(context.Background(), opts, NewClient(srv.URL), stdin, &out)
	if err != nil {
		t.Fatalf("Run: %v", err)
	}
	if res.AppID != "notes.mafu.dev" {
		t.Errorf("AppID = %q", res.AppID)
	}
	if !strings.Contains(out.String(), "Press Enter") {
		t.Errorf("expected prompt in stdout, got:\n%s", out.String())
	}
}

func TestRun_CreateChallengeError(t *testing.T) {
	fake, srv := newFakeMarketplace(t)
	fake.challengeBody = `{"error":"invalid_url","detail":"url has no host"}`
	path := writeManifest(t, minimalManifest)
	opts := Options{
		ManifestPath: path,
		RegistryURL:  srv.URL,
		SkipPrompt:   true,
	}
	var out bytes.Buffer
	_, err := Run(context.Background(), opts, NewClient(srv.URL), nil, &out)
	if err == nil {
		t.Fatal("expected challenge error")
	}
	if !strings.Contains(err.Error(), "invalid_url") {
		t.Errorf("error should surface api code: %v", err)
	}
}
