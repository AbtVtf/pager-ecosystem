package metrics

import (
	"bytes"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
)

func TestCounterIncEmitsLabeledSeries(t *testing.T) {
	r := NewRegistry()
	c := r.NewCounter("test_total", "test counter")
	c.Inc("result", "accepted", "endpoint", "/push")
	c.Inc("result", "accepted", "endpoint", "/push")
	c.Inc("result", "rate_limited", "endpoint", "/push")
	c.Inc()

	var buf bytes.Buffer
	if err := r.WriteExposition(&buf); err != nil {
		t.Fatalf("WriteExposition: %v", err)
	}
	out := buf.String()

	for _, want := range []string{
		`# HELP test_total test counter`,
		`# TYPE test_total counter`,
		`test_total 1`,
		`test_total{endpoint="/push",result="accepted"} 2`,
		`test_total{endpoint="/push",result="rate_limited"} 1`,
	} {
		if !strings.Contains(out, want) {
			t.Errorf("missing line %q in:\n%s", want, out)
		}
	}
}

func TestGaugeSetReplacesValue(t *testing.T) {
	r := NewRegistry()
	g := r.NewGauge("queue_depth", "depth")
	g.Set(10)
	g.Set(3)
	g.Set(99, "backend", "redis")

	var buf bytes.Buffer
	if err := r.WriteExposition(&buf); err != nil {
		t.Fatalf("WriteExposition: %v", err)
	}
	out := buf.String()
	if !strings.Contains(out, "queue_depth 3\n") {
		t.Errorf("expected unlabeled gauge to be 3, got:\n%s", out)
	}
	if !strings.Contains(out, `queue_depth{backend="redis"} 99`) {
		t.Errorf("expected labeled gauge, got:\n%s", out)
	}
}

func TestLabelEscaping(t *testing.T) {
	r := NewRegistry()
	c := r.NewCounter("t", "h")
	c.Inc("k", `quote "inside" and \back`+"\nnl")

	var buf bytes.Buffer
	_ = r.WriteExposition(&buf)
	out := buf.String()
	// Want: k="quote \"inside\" and \\back\nnl"
	if !strings.Contains(out, `k="quote \"inside\" and \\back\nnl"`) {
		t.Errorf("label not escaped properly:\n%s", out)
	}
}

func TestConcurrentIncIsRaceFree(t *testing.T) {
	r := NewRegistry()
	c := r.NewCounter("conc_total", "concurrent counter")

	const goroutines = 64
	const incPerGoroutine = 1000
	var wg sync.WaitGroup
	wg.Add(goroutines)
	for i := 0; i < goroutines; i++ {
		go func() {
			defer wg.Done()
			for j := 0; j < incPerGoroutine; j++ {
				c.Inc("worker", "w")
			}
		}()
	}
	wg.Wait()

	var buf bytes.Buffer
	_ = r.WriteExposition(&buf)
	want := goroutines * incPerGoroutine
	// The series is single (worker="w"), so we should see exactly one
	// sample line with the expected total.
	for _, line := range strings.Split(buf.String(), "\n") {
		if strings.HasPrefix(line, "conc_total{") {
			var n int
			fields := strings.Fields(line)
			if len(fields) != 2 {
				t.Fatalf("unexpected sample line %q", line)
			}
			if _, err := fmtScanf(fields[1], &n); err != nil {
				t.Fatalf("parse %q: %v", fields[1], err)
			}
			if n != want {
				t.Errorf("counter total = %d, want %d", n, want)
			}
			return
		}
	}
	t.Fatalf("no labeled series found in:\n%s", buf.String())
}

// fmtScanf is a tiny strconv-style helper to keep the test free of strconv imports
// (the test file already pulls in enough). Returns count, error.
func fmtScanf(s string, dst *int) (int, error) {
	n := 0
	for _, r := range s {
		if r < '0' || r > '9' {
			break
		}
		n = n*10 + int(r-'0')
	}
	*dst = n
	return 1, nil
}

func TestHTTPHandlerEmitsTextPlainContentType(t *testing.T) {
	r := NewRegistry()
	r.NewCounter("hits_total", "hits").Inc()

	req := httptest.NewRequest("GET", "/metrics", nil)
	rr := httptest.NewRecorder()
	NewHandler(r).ServeHTTP(rr, req)

	if rr.Code != 200 {
		t.Errorf("status = %d, want 200", rr.Code)
	}
	if ct := rr.Header().Get("Content-Type"); !strings.HasPrefix(ct, "text/plain") {
		t.Errorf("Content-Type = %q, want text/plain prefix", ct)
	}
	if !strings.Contains(rr.Body.String(), "hits_total 1") {
		t.Errorf("body missing counter sample:\n%s", rr.Body.String())
	}
}

func TestHTTPHandlerRejectsNonGet(t *testing.T) {
	r := NewRegistry()
	req := httptest.NewRequest("POST", "/metrics", nil)
	rr := httptest.NewRecorder()
	NewHandler(r).ServeHTTP(rr, req)
	if rr.Code != 405 {
		t.Errorf("status = %d, want 405", rr.Code)
	}
	if got := rr.Header().Get("Allow"); got != "GET, HEAD" {
		t.Errorf("Allow = %q, want %q", got, "GET, HEAD")
	}
}
