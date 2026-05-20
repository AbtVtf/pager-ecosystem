package server

import (
	"crypto/subtle"
	"encoding/base64"
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"
	"time"

	"github.com/pageros/pageros/push-relay/internal/pageossig"
	"github.com/pageros/pageros/push-relay/internal/storage"
)

// pullResponse is the JSON body returned by GET /pull/<device_pubkey>.
// Each entry carries the server-assigned id (for the subsequent DELETE ack),
// the envelope kind, the originating app id, and the opaque encrypted payload
// (base64-std). The payload is end-to-end encrypted from the sending app to
// the device; the relay never decrypts it (SPEC §6.6.3).
type pullResponse struct {
	Notifications []pullEntry `json:"notifications"`
}

type pullEntry struct {
	ID         string `json:"id"`
	Kind       string `json:"kind"`
	AppID      string `json:"app_id,omitempty"`
	Payload    string `json:"payload"` // base64-std of the opaque ciphertext
	EnqueuedAt int64  `json:"enqueued_at"`
}

// newPullHandler returns the /pull/{device_pubkey} handler. Verifies the
// PagerOS-Sig headers against the URL path, then returns all queued
// notifications for the device in oldest-first order.
func newPullHandler(store storage.Store, logger *slog.Logger, maxSkew time.Duration) http.Handler {
	if logger == nil {
		logger = slog.Default()
	}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			w.Header().Set("Allow", http.MethodGet)
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}

		// Go 1.22+ pattern: device pubkey is the {device_pubkey} segment.
		pubkeyStr := r.PathValue("device_pubkey")
		if pubkeyStr == "" {
			writeUnauthorized(w, logger, "missing device pubkey in path", nil)
			return
		}
		urlPubkey, err := pageossig.DecodePubkey(pubkeyStr)
		if err != nil {
			writeUnauthorized(w, logger, "invalid device pubkey", err)
			return
		}

		verified, err := pageossig.Verify(r, nil, pageossig.DefaultMaxSkew, time.Now)
		if err != nil {
			writeUnauthorized(w, logger, "signature verification failed", err)
			return
		}

		// Defense in depth: even if the signature is valid, the signed device
		// key must match the one in the URL path — otherwise device A could
		// pull device B's queue by re-signing with A's key.
		if subtle.ConstantTimeCompare(urlPubkey, verified.DevicePubkey) != 1 {
			writeUnauthorized(w, logger, "device pubkey mismatch between URL and signature", nil)
			return
		}

		notifications, err := store.List(r.Context(), pubkeyStr)
		if err != nil {
			logger.Error("storage.List failed", "err", err, "device", pubkeyStr)
			http.Error(w, "internal error", http.StatusInternalServerError)
			return
		}

		resp := pullResponse{Notifications: make([]pullEntry, 0, len(notifications))}
		for _, n := range notifications {
			resp.Notifications = append(resp.Notifications, pullEntry{
				ID:         n.ID,
				Kind:       n.Kind,
				AppID:      n.AppID,
				Payload:    base64.StdEncoding.EncodeToString(n.Payload),
				EnqueuedAt: n.EnqueuedAt.Unix(),
			})
		}

		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusOK)
		_ = json.NewEncoder(w).Encode(resp)
	})
}

// writeUnauthorized emits a uniform 401 response. The exact reason is logged
// server-side but only "unauthorized" is leaked to the client to avoid handing
// attackers a tighter oracle on what step of the auth check failed.
func writeUnauthorized(w http.ResponseWriter, logger *slog.Logger, msg string, cause error) {
	attrs := []any{"reason", msg}
	if cause != nil {
		attrs = append(attrs, "err", cause.Error())
		// Sentinel-aware bucketing for ops alerts.
		switch {
		case errors.Is(cause, pageossig.ErrTimestampSkew):
			attrs = append(attrs, "bucket", "skew")
		case errors.Is(cause, pageossig.ErrMissingHeader):
			attrs = append(attrs, "bucket", "missing_header")
		case errors.Is(cause, pageossig.ErrInvalidEncoding):
			attrs = append(attrs, "bucket", "bad_encoding")
		case errors.Is(cause, pageossig.ErrBadSignature):
			attrs = append(attrs, "bucket", "bad_sig")
		}
	}
	logger.Info("auth failed", attrs...)
	http.Error(w, "unauthorized", http.StatusUnauthorized)
}
