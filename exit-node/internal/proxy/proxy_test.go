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
