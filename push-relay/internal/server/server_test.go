package server

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/pageros/pageros/push-relay/internal/storage"
)

type fakeStore struct {
	pingErr error
}

func (f *fakeStore) Ping(ctx context.Context) error { return f.pingErr }
func (f *fakeStore) Close() error                   { return nil }
func (f *fakeStore) Enqueue(ctx context.Context, _ string, n storage.Notification) (storage.Notification, error) {
	return n, nil
}
func (f *fakeStore) List(ctx context.Context, _ string) ([]storage.Notification, error) {
	return nil, nil
}
func (f *fakeStore) Delete(ctx context.Context, _, _ string) (bool, error) { return false, nil }

func TestHealthzOK(t *testing.T) {
	s := New(Options{Storage: &fakeStore{}, BuildTag: "test"})
	req := httptest.NewRequest(http.MethodGet, "/healthz", nil)
	rec := httptest.NewRecorder()
	s.Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d", rec.Code)
	}
	var body healthResponse
	if err := json.NewDecoder(rec.Body).Decode(&body); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if body.Status != "ok" || body.Storage != "ok" {
		t.Fatalf("unexpected body: %+v", body)
	}
}

func TestHealthzDegradedWhenStorageDown(t *testing.T) {
	s := New(Options{Storage: &fakeStore{pingErr: context.DeadlineExceeded}})
	req := httptest.NewRequest(http.MethodGet, "/healthz", nil)
	rec := httptest.NewRecorder()
	s.Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusServiceUnavailable {
		t.Fatalf("expected 503, got %d", rec.Code)
	}
}
