package server

import (
	"crypto/subtle"
	"log/slog"
	"net/http"
	"time"

	"github.com/pageros/pageros/push-relay/internal/pageossig"
	"github.com/pageros/pageros/push-relay/internal/storage"
)

// newAckHandler returns the DELETE /pull/{device_pubkey}/{notification_id}
// handler (PUSH-004, SPEC §6.6.1).
//
// Auth: the device signs the request the same way as GET /pull. We re-use
// pageossig.Verify and the URL/signed-pubkey cross-check from pull.go so the
// two endpoints can never diverge in subtle ways (an attacker who can replay
// an ack against the wrong device is just as bad as one who can pull it).
//
// Semantics: idempotent — both "removed" and "did not exist" map to 204 No
// Content. The spec calls this out explicitly: the device may re-ack on
// retry, and a missing entry just means an earlier ack already landed (or
// the entry was evicted by TTL / cap). Clients learn nothing useful from a
// 404 here, and surfacing the distinction makes retries noisier without
// improving correctness.
func newAckHandler(store storage.Store, logger *slog.Logger, maxSkew time.Duration) http.Handler {
	if logger == nil {
		logger = slog.Default()
	}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodDelete {
			w.Header().Set("Allow", http.MethodDelete)
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}

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

		notificationID := r.PathValue("notification_id")
		if notificationID == "" {
			// The mux pattern guarantees a non-empty segment, but defend in
			// depth in case the route is ever rewired without {…} captures.
			http.Error(w, "missing notification id", http.StatusBadRequest)
			return
		}

		if maxSkew <= 0 {
			maxSkew = pageossig.DefaultMaxSkew
		}
		verified, err := pageossig.Verify(r, nil, maxSkew, time.Now)
		if err != nil {
			writeUnauthorized(w, logger, "signature verification failed", err)
			return
		}

		// Same cross-check as pull: a valid sig is necessary but not
		// sufficient — the signer must own the device whose queue is being
		// mutated.
		if subtle.ConstantTimeCompare(urlPubkey, verified.DevicePubkey) != 1 {
			writeUnauthorized(w, logger, "device pubkey mismatch between URL and signature", nil)
			return
		}

		// We intentionally ignore the `existed` return: idempotent ack.
		if _, err := store.Delete(r.Context(), pubkeyStr, notificationID); err != nil {
			logger.Error("storage.Delete failed", "err", err, "device", pubkeyStr, "id", notificationID)
			http.Error(w, "internal error", http.StatusInternalServerError)
			return
		}

		w.WriteHeader(http.StatusNoContent)
	})
}
