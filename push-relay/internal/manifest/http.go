package manifest

import (
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"path"
	"strings"
	"time"
)

const defaultLookupTimeout = 5 * time.Second

// HTTPClient resolves app pubkeys by hitting the marketplace registry REST API
// (MKT-002) at `GET {Base}/apps/{appID}` and reading the manifest's `pubkey`
// field. Safe for concurrent use.
type HTTPClient struct {
	base string
	http *http.Client
}

// NewHTTPClient builds a client pointing at the registry root (e.g.
// "https://market.pageros.org"). A nil http.Client defaults to a sensible
// timeout — callers MUST NOT pass http.DefaultClient unmodified in production
// because it has no timeout.
func NewHTTPClient(base string, httpc *http.Client) *HTTPClient {
	if httpc == nil {
		httpc = &http.Client{Timeout: defaultLookupTimeout}
	}
	return &HTTPClient{base: strings.TrimRight(base, "/"), http: httpc}
}

// registry response shape — only the fields we care about. Matches MKT-002's
// AppRecord (registry/schemas.py): `{ "manifest": { "pubkey": "..." }, ... }`.
type appRecord struct {
	Manifest struct {
		Pubkey string `json:"pubkey"`
	} `json:"manifest"`
}

// Pubkey fetches the manifest for appID and returns its registered Ed25519
// signing pubkey. Returns ErrAppNotFound on a 404 response and
// ErrLookupUnavailable on any other failure (network, 5xx, malformed body,
// missing pubkey).
func (c *HTTPClient) Pubkey(ctx context.Context, appID string) (ed25519.PublicKey, error) {
	if appID == "" {
		return nil, fmt.Errorf("%w: empty app id", ErrLookupUnavailable)
	}
	if c.base == "" {
		return nil, fmt.Errorf("%w: no registry base URL configured", ErrLookupUnavailable)
	}
	endpoint, err := joinURL(c.base, "apps", appID)
	if err != nil {
		return nil, fmt.Errorf("%w: build URL: %v", ErrLookupUnavailable, err)
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
	if err != nil {
		return nil, fmt.Errorf("%w: build request: %v", ErrLookupUnavailable, err)
	}
	req.Header.Set("Accept", "application/json")

	resp, err := c.http.Do(req)
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrLookupUnavailable, err)
	}
	defer func() {
		_, _ = io.Copy(io.Discard, resp.Body)
		_ = resp.Body.Close()
	}()

	switch resp.StatusCode {
	case http.StatusOK:
		// fall through
	case http.StatusNotFound:
		return nil, ErrAppNotFound
	default:
		return nil, fmt.Errorf("%w: registry returned %d", ErrLookupUnavailable, resp.StatusCode)
	}

	var rec appRecord
	if err := json.NewDecoder(io.LimitReader(resp.Body, 1<<20)).Decode(&rec); err != nil {
		return nil, fmt.Errorf("%w: decode body: %v", ErrLookupUnavailable, err)
	}
	pk, err := decodeManifestPubkey(rec.Manifest.Pubkey)
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrLookupUnavailable, err)
	}
	return pk, nil
}

func joinURL(base string, segs ...string) (string, error) {
	u, err := url.Parse(base)
	if err != nil {
		return "", err
	}
	// path.Join collapses ".." but we control inputs; this is safe for our use
	// because callers only ever pass appID matching the marketplace regex.
	u.Path = path.Join(append([]string{u.Path}, segs...)...)
	return u.String(), nil
}

// decodeManifestPubkey parses the marketplace's base64-encoded pubkey field
// into a 32-byte Ed25519 public key. Accepts both base64-std and base64url,
// with or without padding, since the marketplace manifest schema only fixes
// the byte length (32), not the alphabet (see manifest.py validator).
func decodeManifestPubkey(s string) (ed25519.PublicKey, error) {
	s = strings.TrimSpace(s)
	if s == "" {
		return nil, errors.New("manifest pubkey is empty")
	}
	decoders := []func(string) ([]byte, error){
		base64.StdEncoding.DecodeString,
		base64.RawStdEncoding.DecodeString,
		base64.URLEncoding.DecodeString,
		base64.RawURLEncoding.DecodeString,
	}
	var lastErr error
	for _, dec := range decoders {
		raw, err := dec(s)
		if err != nil {
			lastErr = err
			continue
		}
		if len(raw) != ed25519.PublicKeySize {
			lastErr = fmt.Errorf("expected %d-byte pubkey, got %d", ed25519.PublicKeySize, len(raw))
			continue
		}
		return ed25519.PublicKey(raw), nil
	}
	return nil, lastErr
}
