// Package proxy is the EXIT-003 HTTPS forwarder.
//
// It implements lora.Handler: each call takes a reassembled inner-envelope
// CBOR blob (the loop hands it off after LORA-002 reassembly), validates
// that the `to` URL is https://, forwards the inner envelope to that URL
// over HTTPS, and returns the upstream response body as the LoRa response
// payload. The lora.Loop fragments and outer-envelope-wraps that payload
// back to the device (SPEC.md §6.2.1, §6.2.2, §3.3 step 3-4).
//
// The exit-node never decrypts the inner envelope body — only the
// recipient app (which holds the matching X25519 private key) can. We
// only inspect the cleartext routing fields (`to`, `from`, `nonce`,
// `sig`) and forward the CBOR blob verbatim. That keeps the exit-node
// untrusted-by-design per SPEC.md §3.2.
package proxy

import (
	"bytes"
	"context"
	"crypto/tls"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"strconv"
	"time"

	"github.com/pageros/pager-ecosystem/exit-node/internal/lora"
)

// Defaults — picked to match SPEC §6.4 (LoRa transport budget) and to
// fit comfortably inside the device's retry window.
const (
	DefaultRequestTimeout   = 30 * time.Second
	DefaultMaxResponseBytes = 64 * 1024
	DefaultUserAgent        = "pageros-exit-node/1"
)

// HTTP headers the exit-node attaches so the upstream app server can
// recognise a LoRa-borne request and pull the routing fields without
// re-decoding the CBOR.
//
// These mirror the device-direct PagerOS-Device / PagerOS-Sig pattern
// from SPEC §5.4 so SDKs can share one auth path. The body itself is
// the unchanged inner-envelope CBOR.
const (
	HeaderTransport  = "PagerOS-Transport"
	HeaderDevice     = "PagerOS-Device"
	HeaderSignature  = "PagerOS-Sig"
	HeaderNonce      = "PagerOS-Nonce"
	HeaderMsgID      = "PagerOS-Msg-Id"
	TransportValue   = "lora/1"
	ContentTypeInner = "application/cbor; pageros=inner/1"
)

// Sentinel errors. Wrapped in Handle's return so callers (and tests)
// can branch on the cause via errors.Is.
var (
	ErrInnerDecode      = errors.New("proxy: inner envelope decode failed")
	ErrNonHTTPS         = errors.New("proxy: inner `to` not https")
	ErrBuildRequest     = errors.New("proxy: build http request")
	ErrUpstreamFailed   = errors.New("proxy: upstream HTTPS request failed")
	ErrReadResponse     = errors.New("proxy: read upstream response")
	ErrResponseTooLarge = errors.New("proxy: upstream response exceeded MaxResponseBytes")
)

// Config parameterises a Proxy.
type Config struct {
	// HTTPClient overrides the default client. Tests use httptest's
	// client (which trusts the test server's self-signed cert);
	// production leaves this nil and gets a strict-TLS client.
	HTTPClient *http.Client

	// RequestTimeout caps a single upstream request (connect + body
	// read). Defaults to DefaultRequestTimeout. Ignored when HTTPClient
	// is supplied — the caller owns its timeouts there.
	RequestTimeout time.Duration

	// MaxResponseBytes caps the upstream response body we'll relay.
	// Anything larger surfaces as ErrResponseTooLarge so the device
	// can time out and retry with a different node. Defaults to
	// DefaultMaxResponseBytes.
	MaxResponseBytes int64

	// UserAgent attached to outbound requests. Defaults to
	// DefaultUserAgent. Operators can set a node-specific value so app
	// servers can identify which exit-node is forwarding.
	UserAgent string

	// Logger for non-fatal diagnostics (upstream error bodies, oversize
	// truncations). nil disables logging.
	Logger *log.Logger
}

// Proxy implements lora.Handler.
type Proxy struct {
	client    *http.Client
	maxBytes  int64
	userAgent string
	logger    *log.Logger
}

// New builds a Proxy. Defaults are filled per the constants above.
func New(cfg Config) *Proxy {
	if cfg.RequestTimeout <= 0 {
		cfg.RequestTimeout = DefaultRequestTimeout
	}
	if cfg.MaxResponseBytes <= 0 {
		cfg.MaxResponseBytes = DefaultMaxResponseBytes
	}
	if cfg.UserAgent == "" {
		cfg.UserAgent = DefaultUserAgent
	}
	client := cfg.HTTPClient
	if client == nil {
		client = &http.Client{
			Timeout: cfg.RequestTimeout,
			Transport: &http.Transport{
				// Strict TLS by default. SPEC.md §6.1 mandates TLS 1.2+ /
				// Mozilla CA bundle for the device's Wi-Fi path; the
				// exit-node is the device's surrogate here, so same bar.
				TLSClientConfig: &tls.Config{MinVersion: tls.VersionTLS12},
			},
		}
	}
	return &Proxy{
		client:    client,
		maxBytes:  cfg.MaxResponseBytes,
		userAgent: cfg.UserAgent,
		logger:    cfg.Logger,
	}
}

// Handle implements lora.Handler. The Body must be a CBOR-encoded inner
// envelope (LORA-003); the loop has already reassembled fragments by
// the time it lands here.
func (p *Proxy) Handle(ctx context.Context, req lora.Request) (lora.Response, error) {
	inner, err := lora.DecodeInner(req.Body)
	if err != nil {
		return lora.Response{}, fmt.Errorf("%w: %v", ErrInnerDecode, err)
	}
	if err := lora.ValidateHTTPSTarget(inner); err != nil {
		return lora.Response{}, fmt.Errorf("%w: %v", ErrNonHTTPS, err)
	}

	httpReq, err := http.NewRequestWithContext(ctx, http.MethodPost, inner.To, bytes.NewReader(req.Body))
	if err != nil {
		return lora.Response{}, fmt.Errorf("%w: %v", ErrBuildRequest, err)
	}
	httpReq.Header.Set("Content-Type", ContentTypeInner)
	httpReq.Header.Set("Accept", ContentTypeInner)
	httpReq.Header.Set("User-Agent", p.userAgent)
	httpReq.Header.Set(HeaderTransport, TransportValue)
	httpReq.Header.Set(HeaderDevice, base64.StdEncoding.EncodeToString(inner.From))
	httpReq.Header.Set(HeaderSignature, base64.StdEncoding.EncodeToString(inner.Sig))
	httpReq.Header.Set(HeaderNonce, hex.EncodeToString(inner.Nonce))
	httpReq.Header.Set(HeaderMsgID, strconv.FormatUint(uint64(req.MsgID), 10))

	resp, err := p.client.Do(httpReq)
	if err != nil {
		return lora.Response{}, fmt.Errorf("%w: %v", ErrUpstreamFailed, err)
	}
	defer resp.Body.Close()

	// LimitReader+1 trick: read up to max+1 so we can detect overflow
	// without buffering an unbounded body.
	body, err := io.ReadAll(io.LimitReader(resp.Body, p.maxBytes+1))
	if err != nil {
		return lora.Response{}, fmt.Errorf("%w: %v", ErrReadResponse, err)
	}
	if int64(len(body)) > p.maxBytes {
		p.logf("upstream %s returned > %d bytes; dropping", inner.To, p.maxBytes)
		return lora.Response{}, fmt.Errorf("%w: got %d, max %d", ErrResponseTooLarge, len(body), p.maxBytes)
	}

	// Non-2xx is still a "response" the device should see — the app
	// owns its error semantics. We log but pass through.
	if resp.StatusCode/100 != 2 {
		p.logf("upstream %s -> %d (%d bytes), passing through", inner.To, resp.StatusCode, len(body))
	}

	return lora.Response{Body: body}, nil
}

func (p *Proxy) logf(format string, args ...any) {
	if p.logger == nil {
		return
	}
	p.logger.Printf(format, args...)
}
