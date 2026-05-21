// Package status implements the public uptime page served at
// status.pageros.org (PUSH-010). It queries the relay's Prometheus instance
// (added in PUSH-009) for the SLO-relevant metrics and renders an HTML page
// plus a JSON endpoint that downstream uptime aggregators can scrape.
//
// The renderer and Prometheus client are kept dependency-free (stdlib only)
// — the status page must stay buildable even if the rest of the deploy
// stack is unavailable.
package status

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
)

// PromClient is a minimal Prometheus HTTP API v1 client. We only need
// `query` (instant vector); the package intentionally avoids a third-party
// dep so the status page binary stays tiny.
type PromClient struct {
	base string
	http *http.Client
}

// NewPromClient builds a client against the Prometheus base URL
// (e.g. http://prometheus:9090). A nil httpc gets a 5s timeout.
func NewPromClient(base string, httpc *http.Client) *PromClient {
	if httpc == nil {
		httpc = &http.Client{Timeout: 5 * time.Second}
	}
	return &PromClient{base: strings.TrimRight(base, "/"), http: httpc}
}

// promResponse matches Prometheus' HTTP API envelope.
// https://prometheus.io/docs/prometheus/latest/querying/api/#instant-queries
type promResponse struct {
	Status    string          `json:"status"`
	Data      promResponseDat `json:"data"`
	ErrorType string          `json:"errorType,omitempty"`
	Error     string          `json:"error,omitempty"`
}

type promResponseDat struct {
	ResultType string            `json:"resultType"`
	Result     []json.RawMessage `json:"result"`
}

// ErrNoSamples means the query succeeded but returned an empty vector.
// Callers map this to "metric not yet observed" — e.g. a freshly-deployed
// status server before Prometheus has scraped the relay.
var ErrNoSamples = errors.New("status: prometheus query returned no samples")

// Query evaluates a PromQL instant vector and returns the single sample
// value. If the query returns more than one sample, we take the first —
// the caller is expected to write queries that produce a scalar
// (e.g. with `sum(...)` / `max(...)`). Returns ErrNoSamples on an empty
// result set.
func (c *PromClient) Query(ctx context.Context, expr string) (float64, error) {
	if c.base == "" {
		return 0, errors.New("status: no prometheus base URL configured")
	}
	q := url.Values{}
	q.Set("query", expr)
	endpoint := c.base + "/api/v1/query?" + q.Encode()

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
	if err != nil {
		return 0, fmt.Errorf("status: build query: %w", err)
	}
	req.Header.Set("Accept", "application/json")

	resp, err := c.http.Do(req)
	if err != nil {
		return 0, fmt.Errorf("status: prometheus query: %w", err)
	}
	defer func() {
		_, _ = io.Copy(io.Discard, resp.Body)
		_ = resp.Body.Close()
	}()

	if resp.StatusCode != http.StatusOK {
		return 0, fmt.Errorf("status: prometheus returned %d", resp.StatusCode)
	}

	var pr promResponse
	if err := json.NewDecoder(io.LimitReader(resp.Body, 1<<20)).Decode(&pr); err != nil {
		return 0, fmt.Errorf("status: decode prometheus response: %w", err)
	}
	if pr.Status != "success" {
		return 0, fmt.Errorf("status: prometheus query error: %s: %s", pr.ErrorType, pr.Error)
	}
	if pr.Data.ResultType != "vector" {
		return 0, fmt.Errorf("status: expected vector result, got %q", pr.Data.ResultType)
	}
	if len(pr.Data.Result) == 0 {
		return 0, ErrNoSamples
	}

	// Instant-vector result: each entry is `{"metric": {...}, "value": [t, "v"]}`.
	var entry struct {
		Value [2]json.RawMessage `json:"value"`
	}
	if err := json.Unmarshal(pr.Data.Result[0], &entry); err != nil {
		return 0, fmt.Errorf("status: decode sample: %w", err)
	}
	// entry.Value[1] is a JSON string holding the float as text.
	var raw string
	if err := json.Unmarshal(entry.Value[1], &raw); err != nil {
		return 0, fmt.Errorf("status: decode sample value: %w", err)
	}
	v, err := strconv.ParseFloat(raw, 64)
	if err != nil {
		return 0, fmt.Errorf("status: parse sample value %q: %w", raw, err)
	}
	return v, nil
}
