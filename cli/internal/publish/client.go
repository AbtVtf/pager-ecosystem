package publish

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

// Client talks to the Marketplace REST API.
type Client struct {
	BaseURL   string
	HTTP      *http.Client
	UserAgent string
}

// NewClient builds a Client with sensible HTTP defaults.
func NewClient(baseURL string) *Client {
	return &Client{
		BaseURL: strings.TrimRight(baseURL, "/"),
		HTTP: &http.Client{
			Timeout: 60 * time.Second,
		},
		UserAgent: "pagerctl/0.1 (+https://pageros.org)",
	}
}

// Challenge mirrors marketplace ChallengeRecord (subset we need).
type Challenge struct {
	ID         string     `json:"id"`
	AppID      string     `json:"app_id"`
	Host       string     `json:"host"`
	Token      string     `json:"token"`
	TxtName    string     `json:"txt_name"`
	TxtValue   string     `json:"txt_value"`
	ExpiresAt  time.Time  `json:"expires_at"`
	VerifiedAt *time.Time `json:"verified_at,omitempty"`
}

// AppRecord mirrors marketplace AppRecord (subset we need).
type AppRecord struct {
	Manifest  Manifest  `json:"manifest"`
	Tags      []string  `json:"tags"`
	CreatedAt time.Time `json:"created_at"`
	UpdatedAt time.Time `json:"updated_at"`
}

// APIError is what the Marketplace returns for any 4xx/5xx. The shape mirrors
// pageros_marketplace.registry.schemas.ErrorResponse.
type APIError struct {
	Status  int          `json:"-"`
	Code    string       `json:"error"`
	Detail  string       `json:"detail,omitempty"`
	Fields  []FieldError `json:"fields,omitempty"`
	rawBody string
}

func (e *APIError) Error() string {
	var b strings.Builder
	fmt.Fprintf(&b, "marketplace api %d", e.Status)
	if e.Code != "" {
		fmt.Fprintf(&b, " %s", e.Code)
	}
	if e.Detail != "" {
		fmt.Fprintf(&b, ": %s", e.Detail)
	}
	for _, f := range e.Fields {
		b.WriteString("\n  - ")
		b.WriteString(f.Error())
	}
	if e.Code == "" && e.Detail == "" && e.rawBody != "" {
		fmt.Fprintf(&b, ": %s", strings.TrimSpace(e.rawBody))
	}
	return b.String()
}

// IsValidationFailure reports whether the API rejected the manifest as
// schema-invalid (HTTP 400 with structured field errors). The CLI uses this to
// render the same diagnostic shape as local validation.
func (e *APIError) IsValidationFailure() bool {
	return e != nil && e.Status == http.StatusBadRequest && len(e.Fields) > 0
}

// CreateChallenge opens a DNS TXT challenge for {app_id, url}.
func (c *Client) CreateChallenge(ctx context.Context, appID, manifestURL string) (*Challenge, error) {
	body, _ := json.Marshal(map[string]string{"app_id": appID, "url": manifestURL})
	var out Challenge
	if err := c.doJSON(ctx, http.MethodPost, "/apps/challenges", body, nil, &out); err != nil {
		return nil, err
	}
	return &out, nil
}

// VerifyChallenge asks the Marketplace to perform the DNS TXT lookup.
func (c *Client) VerifyChallenge(ctx context.Context, challengeID string) (*Challenge, error) {
	path := "/apps/challenges/" + url.PathEscape(challengeID) + "/verify"
	var out Challenge
	if err := c.doJSON(ctx, http.MethodPost, path, nil, nil, &out); err != nil {
		return nil, err
	}
	return &out, nil
}

// RegisterApp posts the manifest with the consumed challenge token.
func (c *Client) RegisterApp(ctx context.Context, manifest *Manifest, challengeToken string) (*AppRecord, error) {
	body, err := json.Marshal(manifest)
	if err != nil {
		return nil, fmt.Errorf("marshal manifest: %w", err)
	}
	headers := http.Header{"X-Challenge-Token": []string{challengeToken}}
	var out AppRecord
	if err := c.doJSON(ctx, http.MethodPost, "/apps", body, headers, &out); err != nil {
		return nil, err
	}
	return &out, nil
}

// doJSON performs a JSON request and decodes the response. It returns an
// *APIError on non-2xx responses.
func (c *Client) doJSON(ctx context.Context, method, path string, body []byte, headers http.Header, out any) error {
	if c.BaseURL == "" {
		return errors.New("marketplace base url is empty")
	}
	var reader io.Reader
	if body != nil {
		reader = bytes.NewReader(body)
	}
	req, err := http.NewRequestWithContext(ctx, method, c.BaseURL+path, reader)
	if err != nil {
		return err
	}
	req.Header.Set("Accept", "application/json")
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	if c.UserAgent != "" {
		req.Header.Set("User-Agent", c.UserAgent)
	}
	for k, vs := range headers {
		for _, v := range vs {
			req.Header.Add(k, v)
		}
	}
	resp, err := c.HTTP.Do(req)
	if err != nil {
		return fmt.Errorf("%s %s: %w", method, path, err)
	}
	defer resp.Body.Close()

	raw, _ := io.ReadAll(resp.Body)
	if resp.StatusCode >= 200 && resp.StatusCode < 300 {
		if out == nil || len(raw) == 0 || resp.StatusCode == http.StatusNoContent {
			return nil
		}
		if err := json.Unmarshal(raw, out); err != nil {
			return fmt.Errorf("decode %s response: %w", path, err)
		}
		return nil
	}

	apiErr := &APIError{Status: resp.StatusCode, rawBody: string(raw)}
	if len(raw) > 0 {
		_ = json.Unmarshal(raw, apiErr)
	}
	return apiErr
}
