package status

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"net/url"
	"testing"
)

func newPromServer(t *testing.T, handler http.HandlerFunc) *httptest.Server {
	t.Helper()
	srv := httptest.NewServer(handler)
	t.Cleanup(srv.Close)
	return srv
}

func TestPromClientQueryInstantVector(t *testing.T) {
	var sawQuery string
	srv := newPromServer(t, func(w http.ResponseWriter, r *http.Request) {
		sawQuery = r.URL.Query().Get("query")
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{
			"status":"success",
			"data":{"resultType":"vector","result":[
				{"metric":{"__name__":"up"},"value":[1715000000, "1"]}
			]}
		}`))
	})
	c := NewPromClient(srv.URL, nil)

	got, err := c.Query(context.Background(), `up{job="push-relay"}`)
	if err != nil {
		t.Fatalf("Query: %v", err)
	}
	if got != 1 {
		t.Fatalf("got %v, want 1", got)
	}
	if sawQuery != `up{job="push-relay"}` {
		t.Fatalf("query not forwarded: %q", sawQuery)
	}
}

func TestPromClientQueryEmptyVector(t *testing.T) {
	srv := newPromServer(t, func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte(`{"status":"success","data":{"resultType":"vector","result":[]}}`))
	})
	c := NewPromClient(srv.URL, nil)

	_, err := c.Query(context.Background(), "missing_metric")
	if !errors.Is(err, ErrNoSamples) {
		t.Fatalf("expected ErrNoSamples, got %v", err)
	}
}

func TestPromClientQueryErrorResponse(t *testing.T) {
	srv := newPromServer(t, func(w http.ResponseWriter, _ *http.Request) {
		// Note: Prometheus returns 400 for query parse errors.
		w.WriteHeader(http.StatusBadRequest)
		_, _ = w.Write([]byte(`{"status":"error","errorType":"bad_data","error":"parse error"}`))
	})
	c := NewPromClient(srv.URL, nil)

	_, err := c.Query(context.Background(), "bogus(")
	if err == nil {
		t.Fatalf("expected error")
	}
}

func TestPromClientQueryEscapesQuery(t *testing.T) {
	// PromQL with quotes / spaces needs to round-trip cleanly.
	const expr = `sum by (job) (rate(push_relay_push_requests_total{result="accepted"}[5m]))`
	var saw string
	srv := newPromServer(t, func(w http.ResponseWriter, r *http.Request) {
		saw = r.URL.Query().Get("query")
		_, _ = w.Write([]byte(`{"status":"success","data":{"resultType":"vector","result":[{"value":[1,"0.42"]}]}}`))
	})
	c := NewPromClient(srv.URL, nil)

	v, err := c.Query(context.Background(), expr)
	if err != nil {
		t.Fatalf("Query: %v", err)
	}
	if v != 0.42 {
		t.Fatalf("value = %v, want 0.42", v)
	}
	// Server should see the original expression, not a URL-mangled one.
	if saw != expr {
		t.Fatalf("got query %q want %q", saw, expr)
	}
	// And the wire encoding must be RFC-3986 safe.
	if _, err := url.Parse(srv.URL + "/api/v1/query?query=" + url.QueryEscape(expr)); err != nil {
		t.Fatalf("encoded URL should parse: %v", err)
	}
}

func TestPromClientNoBase(t *testing.T) {
	c := NewPromClient("", nil)
	if _, err := c.Query(context.Background(), "up"); err == nil {
		t.Fatalf("expected error when no base URL configured")
	}
}
