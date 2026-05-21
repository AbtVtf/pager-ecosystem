package proxy

import (
	"bytes"
	"context"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/pageros/pager-ecosystem/exit-node/internal/lora"
)

// fixedInner builds a CBOR-encoded inner envelope with the given `to`.
// The cipher/sig/nonce fields are arbitrary but shaped correctly so
// DecodeInner accepts the bytes — the exit-node never decrypts, so we
// don't need a real seal.
func fixedInner(t *testing.T, to string) (raw []byte, env lora.InnerEnvelope) {
	t.Helper()
	env = lora.InnerEnvelope{
		To:    to,
		From:  bytes.Repeat([]byte{0xAA}, lora.X25519KeyLen),
		Nonce: bytes.Repeat([]byte{0x02}, lora.InnerNonceLen),
		Sig:   bytes.Repeat([]byte{0xCD}, lora.SigLen),
		Body:  []byte("ciphertext+tag"),
	}
	raw, err := lora.EncodeInner(env)
	if err != nil {
		t.Fatalf("EncodeInner: %v", err)
	}
	return raw, env
}

func TestHandleForwardsToHTTPSTarget(t *testing.T) {
	var (
		gotBody    atomic.Value // []byte
		gotMethod  atomic.Value // string
		gotPath    atomic.Value // string
		gotHeaders atomic.Value // http.Header
	)
	upstream := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		b, _ := io.ReadAll(r.Body)
		gotBody.Store(b)
		gotMethod.Store(r.Method)
		gotPath.Store(r.URL.Path)
		gotHeaders.Store(r.Header.Clone())
		w.Header().Set("Content-Type", ContentTypeInner)
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte("upstream-response-bytes"))
	}))
	defer upstream.Close()

	raw, env := fixedInner(t, upstream.URL+"/save")

	p := New(Config{HTTPClient: upstream.Client()})
	resp, err := p.Handle(context.Background(), lora.Request{MsgID: 0xCAFEBABE, Body: raw})
	if err != nil {
		t.Fatalf("Handle: %v", err)
	}
	if !bytes.Equal(resp.Body, []byte("upstream-response-bytes")) {
		t.Errorf("response body: got %q", string(resp.Body))
	}
	if got := gotMethod.Load().(string); got != http.MethodPost {
		t.Errorf("upstream method: got %s want POST", got)
	}
	if got := gotPath.Load().(string); got != "/save" {
		t.Errorf("upstream path: got %s want /save", got)
	}
	if !bytes.Equal(gotBody.Load().([]byte), raw) {
		t.Errorf("upstream body != inner envelope CBOR")
	}
	hdr := gotHeaders.Load().(http.Header)
	if got := hdr.Get(HeaderTransport); got != TransportValue {
		t.Errorf("Transport header: got %q want %q", got, TransportValue)
	}
	if hdr.Get(HeaderDevice) == "" {
		t.Errorf("Device header missing")
	}
	if hdr.Get(HeaderSignature) == "" {
		t.Errorf("Sig header missing")
	}
	if hdr.Get(HeaderNonce) == "" {
		t.Errorf("Nonce header missing")
	}
	if got := hdr.Get(HeaderMsgID); got != "3405691582" { // 0xCAFEBABE
		t.Errorf("Msg-Id header: got %q", got)
	}
	if got := hdr.Get("Content-Type"); got != ContentTypeInner {
		t.Errorf("Content-Type: got %q", got)
	}
	_ = env
}

func TestHandleRejectsNonHTTPS(t *testing.T) {
	cases := []string{
		"http://example.com/",
		"ftp://example.com/",
		"file:///etc/passwd",
		"javascript:alert(1)",
	}
	for _, to := range cases {
		t.Run(to, func(t *testing.T) {
			raw, _ := fixedInner(t, to)
			p := New(Config{})
			_, err := p.Handle(context.Background(), lora.Request{Body: raw})
			if !errors.Is(err, ErrNonHTTPS) {
				t.Errorf("to=%s: want ErrNonHTTPS, got %v", to, err)
			}
		})
	}
}

func TestHandleRejectsBadInnerEnvelope(t *testing.T) {
	p := New(Config{})
	_, err := p.Handle(context.Background(), lora.Request{Body: []byte{0xff, 0xff, 0xff}})
	if !errors.Is(err, ErrInnerDecode) {
		t.Errorf("want ErrInnerDecode, got %v", err)
	}
}

func TestHandleSurfacesUpstreamConnectError(t *testing.T) {
	// Pick a free port on localhost then close it so the dial fails.
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {}))
	deadURL := srv.URL
	srv.Close()

	raw, _ := fixedInner(t, deadURL+"/save")
	p := New(Config{HTTPClient: &http.Client{Timeout: 500 * time.Millisecond}})
	_, err := p.Handle(context.Background(), lora.Request{Body: raw})
	if !errors.Is(err, ErrUpstreamFailed) {
		t.Errorf("want ErrUpstreamFailed, got %v", err)
	}
}

func TestHandleTruncatesOversizeResponse(t *testing.T) {
	upstream := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write(bytes.Repeat([]byte{0x42}, 4096))
	}))
	defer upstream.Close()

	raw, _ := fixedInner(t, upstream.URL+"/")
	p := New(Config{HTTPClient: upstream.Client(), MaxResponseBytes: 256})
	_, err := p.Handle(context.Background(), lora.Request{Body: raw})
	if !errors.Is(err, ErrResponseTooLarge) {
		t.Errorf("want ErrResponseTooLarge, got %v", err)
	}
}

func TestHandleRespectsContextCancellation(t *testing.T) {
	releaseHandler := make(chan struct{})
	upstream := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		<-releaseHandler
		_, _ = w.Write([]byte("ok"))
	}))
	defer func() {
		close(releaseHandler)
		upstream.Close()
	}()

	raw, _ := fixedInner(t, upstream.URL+"/")
	p := New(Config{HTTPClient: upstream.Client()})

	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	_, err := p.Handle(ctx, lora.Request{Body: raw})
	if err == nil {
		t.Fatal("want context.Canceled-wrapped error, got nil")
	}
	if !errors.Is(err, ErrUpstreamFailed) {
		t.Errorf("want ErrUpstreamFailed (wrapping cancel), got %v", err)
	}
}

func TestHandleRelaysNon2xxResponseBody(t *testing.T) {
	// The app's 404 frame is still a response the device should see —
	// the exit-node is a transparent forwarder, not an HTTPS validator.
	upstream := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNotFound)
		_, _ = w.Write([]byte("not found frame"))
	}))
	defer upstream.Close()

	raw, _ := fixedInner(t, upstream.URL+"/missing")
	p := New(Config{HTTPClient: upstream.Client()})
	resp, err := p.Handle(context.Background(), lora.Request{Body: raw})
	if err != nil {
		t.Fatalf("Handle: %v", err)
	}
	if !bytes.Equal(resp.Body, []byte("not found frame")) {
		t.Errorf("response body: got %q", string(resp.Body))
	}
}

func TestNewFillsDefaults(t *testing.T) {
	p := New(Config{})
	if p.client == nil {
		t.Fatal("default http client should be set")
	}
	if p.maxBytes != DefaultMaxResponseBytes {
		t.Errorf("maxBytes default: got %d want %d", p.maxBytes, DefaultMaxResponseBytes)
	}
	if p.userAgent != DefaultUserAgent {
		t.Errorf("userAgent default: got %q want %q", p.userAgent, DefaultUserAgent)
	}
	if p.client.Timeout != DefaultRequestTimeout {
		t.Errorf("http timeout default: got %s want %s", p.client.Timeout, DefaultRequestTimeout)
	}
}

// stubLimiter is a deterministic RateLimiter the tests can drive
// without pulling in the real package (keeps the test focused on what
// Proxy does with a denial, not on bucket math).
type stubLimiter struct {
	allow      bool
	limit      int
	remaining  int
	retryAfter time.Duration
	keys       []string
}

func (s *stubLimiter) Allow(key string) (bool, int, int, time.Duration) {
	s.keys = append(s.keys, key)
	return s.allow, s.limit, s.remaining, s.retryAfter
}

func TestHandleAllowedRequestForwardsThroughLimiter(t *testing.T) {
	upstreamHit := false
	upstream := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		upstreamHit = true
		_, _ = w.Write([]byte("ok"))
	}))
	defer upstream.Close()

	limiter := &stubLimiter{allow: true, limit: 60, remaining: 59}
	raw, env := fixedInner(t, upstream.URL+"/")
	p := New(Config{HTTPClient: upstream.Client(), RateLimiter: limiter})

	resp, err := p.Handle(context.Background(), lora.Request{Body: raw})
	if err != nil {
		t.Fatalf("Handle: %v", err)
	}
	if !upstreamHit {
		t.Error("limiter allowed but upstream was not called")
	}
	if !bytes.Equal(resp.Body, []byte("ok")) {
		t.Errorf("response body: got %q want ok", string(resp.Body))
	}
	if len(limiter.keys) != 1 {
		t.Fatalf("limiter called %d times, want 1", len(limiter.keys))
	}
	if limiter.keys[0] != string(env.From) {
		t.Errorf("limiter key: got %x want device pubkey %x", limiter.keys[0], env.From)
	}
}

func TestHandleDeniedRequestSkipsUpstreamAndReturnsErrorEnvelope(t *testing.T) {
	upstreamHit := false
	upstream := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		upstreamHit = true
	}))
	defer upstream.Close()

	limiter := &stubLimiter{allow: false, limit: 60, remaining: 0, retryAfter: 7 * time.Second}
	raw, _ := fixedInner(t, upstream.URL+"/")
	p := New(Config{HTTPClient: upstream.Client(), RateLimiter: limiter})

	resp, err := p.Handle(context.Background(), lora.Request{Body: raw})
	if err != nil {
		t.Fatalf("Handle: %v", err)
	}
	if upstreamHit {
		t.Fatal("limiter denied but upstream was still called")
	}
	env, ok := DecodeErrorEnvelope(resp.Body)
	if !ok {
		t.Fatalf("response body is not an exit-node error envelope: %x", resp.Body)
	}
	if env.Code != ErrCodeRateLimited {
		t.Errorf("code: got %q want %q", env.Code, ErrCodeRateLimited)
	}
	if env.RetryAfterSec != 7 {
		t.Errorf("retry_after_s: got %d want 7", env.RetryAfterSec)
	}
	if !strings.Contains(env.Msg, "60 req/min/device") {
		t.Errorf("msg should reference cap: got %q", env.Msg)
	}
}

func TestHandleNoLimiterAlwaysForwards(t *testing.T) {
	upstream := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte("ok"))
	}))
	defer upstream.Close()

	raw, _ := fixedInner(t, upstream.URL+"/")
	p := New(Config{HTTPClient: upstream.Client()}) // no RateLimiter

	resp, err := p.Handle(context.Background(), lora.Request{Body: raw})
	if err != nil {
		t.Fatalf("Handle: %v", err)
	}
	if !bytes.Equal(resp.Body, []byte("ok")) {
		t.Errorf("body: got %q want ok", string(resp.Body))
	}
}

func TestDecodeErrorEnvelopeRejectsNonErrorBodies(t *testing.T) {
	// A real upstream response (arbitrary bytes) must not be mistaken
	// for an error envelope. This is the discriminator contract for the
	// device SDK: missing/false `exit_node_error` ⇒ pass-through body.
	if _, ok := DecodeErrorEnvelope([]byte("not cbor at all")); ok {
		t.Error("non-CBOR body decoded as error envelope")
	}
	// A CBOR map without the discriminator must also be rejected.
	other, _ := EncodeErrorEnvelope(ErrorEnvelope{ExitNodeError: false, Code: "x"})
	if _, ok := DecodeErrorEnvelope(other); ok {
		t.Error("CBOR map without exit_node_error=true decoded as error envelope")
	}
}

func TestUserAgentOverrideSent(t *testing.T) {
	var gotUA atomic.Value
	upstream := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotUA.Store(r.Header.Get("User-Agent"))
	}))
	defer upstream.Close()

	raw, _ := fixedInner(t, upstream.URL+"/")
	p := New(Config{HTTPClient: upstream.Client(), UserAgent: "edge-node-7/1"})
	if _, err := p.Handle(context.Background(), lora.Request{Body: raw}); err != nil {
		t.Fatalf("Handle: %v", err)
	}
	if got, _ := gotUA.Load().(string); !strings.Contains(got, "edge-node-7/1") {
		t.Errorf("user-agent: got %q want substring edge-node-7/1", got)
	}
}
