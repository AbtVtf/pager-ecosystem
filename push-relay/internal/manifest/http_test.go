package manifest

import (
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"testing"
)

// keyFromSeed reuses the deterministic seed convention from server tests so
// fixture pubkeys are stable across the codebase.
func keyFromSeed(t *testing.T, seedHex string) ed25519.PublicKey {
	t.Helper()
	seed, err := hex.DecodeString(seedHex)
	if err != nil {
		t.Fatalf("seed: %v", err)
	}
	return ed25519.NewKeyFromSeed(seed).Public().(ed25519.PublicKey)
}

func newRegistry(t *testing.T, handler http.Handler) *httptest.Server {
	t.Helper()
	srv := httptest.NewServer(handler)
	t.Cleanup(srv.Close)
	return srv
}

func TestHTTPClientPubkeyHappyPath(t *testing.T) {
	pk := keyFromSeed(t, "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
	pkB64 := base64.StdEncoding.EncodeToString(pk)

	mux := http.NewServeMux()
	mux.HandleFunc("/apps/notes.mafu.dev", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprintf(w, `{"manifest": {"id": "notes.mafu.dev", "pubkey": %q}, "tags": [], "created_at": "2026-01-01T00:00:00Z", "updated_at": "2026-01-01T00:00:00Z"}`, pkB64)
	})
	srv := newRegistry(t, mux)
	client := NewHTTPClient(srv.URL, nil)

	got, err := client.Pubkey(context.Background(), "notes.mafu.dev")
	if err != nil {
		t.Fatalf("Pubkey: %v", err)
	}
	if got.Equal(pk) == false {
		t.Fatalf("pubkey mismatch")
	}
}

func TestHTTPClientPubkey404IsAppNotFound(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/apps/", func(w http.ResponseWriter, _ *http.Request) {
		http.Error(w, `{"error":"not_found"}`, http.StatusNotFound)
	})
	srv := newRegistry(t, mux)
	client := NewHTTPClient(srv.URL, nil)

	_, err := client.Pubkey(context.Background(), "ghost.example.dev")
	if !errors.Is(err, ErrAppNotFound) {
		t.Fatalf("expected ErrAppNotFound, got %v", err)
	}
}

func TestHTTPClientPubkey5xxIsLookupUnavailable(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/apps/notes.mafu.dev", func(w http.ResponseWriter, _ *http.Request) {
		http.Error(w, "boom", http.StatusBadGateway)
	})
	srv := newRegistry(t, mux)
	client := NewHTTPClient(srv.URL, nil)

	_, err := client.Pubkey(context.Background(), "notes.mafu.dev")
	if !errors.Is(err, ErrLookupUnavailable) {
		t.Fatalf("expected ErrLookupUnavailable, got %v", err)
	}
}

func TestHTTPClientPubkeyMalformedBody(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/apps/notes.mafu.dev", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte("not json at all"))
	})
	srv := newRegistry(t, mux)
	client := NewHTTPClient(srv.URL, nil)

	_, err := client.Pubkey(context.Background(), "notes.mafu.dev")
	if !errors.Is(err, ErrLookupUnavailable) {
		t.Fatalf("expected ErrLookupUnavailable, got %v", err)
	}
}

func TestHTTPClientPubkeyWrongLength(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/apps/notes.mafu.dev", func(w http.ResponseWriter, _ *http.Request) {
		// Valid base64 but only 16 bytes — not a 32-byte pubkey.
		bad := base64.StdEncoding.EncodeToString([]byte("0123456789abcdef"))
		fmt.Fprintf(w, `{"manifest": {"id": "notes.mafu.dev", "pubkey": %q}}`, bad)
	})
	srv := newRegistry(t, mux)
	client := NewHTTPClient(srv.URL, nil)

	_, err := client.Pubkey(context.Background(), "notes.mafu.dev")
	if !errors.Is(err, ErrLookupUnavailable) {
		t.Fatalf("expected ErrLookupUnavailable, got %v", err)
	}
}

func TestHTTPClientNoBaseURL(t *testing.T) {
	client := NewHTTPClient("", nil)
	_, err := client.Pubkey(context.Background(), "notes.mafu.dev")
	if !errors.Is(err, ErrLookupUnavailable) {
		t.Fatalf("expected ErrLookupUnavailable, got %v", err)
	}
}

func TestHTTPClientAcceptsBase64URLEncoding(t *testing.T) {
	pk := keyFromSeed(t, "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
	pkB64URL := base64.RawURLEncoding.EncodeToString(pk)

	mux := http.NewServeMux()
	mux.HandleFunc("/apps/notes.mafu.dev", func(w http.ResponseWriter, _ *http.Request) {
		fmt.Fprintf(w, `{"manifest": {"pubkey": %q}}`, pkB64URL)
	})
	srv := newRegistry(t, mux)
	client := NewHTTPClient(srv.URL, nil)

	got, err := client.Pubkey(context.Background(), "notes.mafu.dev")
	if err != nil {
		t.Fatalf("Pubkey: %v", err)
	}
	if !got.Equal(pk) {
		t.Fatalf("pubkey mismatch")
	}
}
