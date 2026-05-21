package server

import (
	"encoding/base64"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"time"

	"github.com/pageros/pageros/push-relay/internal/admin"
	"github.com/pageros/pageros/push-relay/internal/manifest"
	"github.com/pageros/pageros/push-relay/internal/metrics"
	"github.com/pageros/pageros/push-relay/internal/pageossig"
	"github.com/pageros/pageros/push-relay/internal/ratelimit"
	"github.com/pageros/pageros/push-relay/internal/storage"
)

// DefaultMaxGroupRecipients caps the per-request fanout. Group widgets
// (`presence_list`, `chat`) generally bind to a single group at a time, and
// the device's SHOULD cap is 8 group ids per Screen (proto §6.2.3) — but the
// *app server* may broadcast a single event to many devices simultaneously.
// 64 is a comfortable upper bound that keeps a request body well under
// DefaultMaxGroupBytes even at the per-recipient payload cap.
const DefaultMaxGroupRecipients = 64

// DefaultMaxGroupBytes caps the entire JSON envelope. 64 recipients × 64 KiB
// per-payload would be 4 MiB before base64 inflation; in practice the app
// server uses a much smaller per-event payload (a few hundred bytes for a
// presence update or chat message). Keep this generous so a legitimate batch
// of N recipients with a 16-32 KiB payload each fits, but small enough to
// reject obvious abuse.
const DefaultMaxGroupBytes int64 = 1 << 21 // 2 MiB

// DefaultMaxGroupPayloadBytes caps a single recipient's opaque payload.
// Mirrors DefaultMaxPushBytes (64 KiB) so the per-recipient cost matches the
// single-recipient /push endpoint.
const DefaultMaxGroupPayloadBytes = 64 * 1024

// groupRecipient is one entry in the POST /group_push request body.
type groupRecipient struct {
	DevicePubkey string `json:"device_pubkey"`
	PayloadB64   string `json:"payload_b64"`
}

// groupPushRequest is the shape of the JSON envelope for the multi-recipient
// fan-out endpoint. `recipients` is REQUIRED and MUST contain at least one
// entry; an empty array is rejected as a malformed request.
type groupPushRequest struct {
	Recipients []groupRecipient `json:"recipients"`
}

// groupResultStatus enumerates the per-recipient outcomes the relay can
// report. Kept as exported string constants so SDKs can compare without
// guessing at server-side phrasing.
const (
	GroupResultAccepted     = "accepted"
	GroupResultRateLimited  = "rate_limited"
	GroupResultPayloadEmpty = "payload_empty"
	GroupResultPayloadLarge = "payload_too_large"
	GroupResultBadPayload   = "invalid_payload"
	GroupResultBadDevice    = "invalid_device_pubkey"
	GroupResultStorageError = "storage_error"
)

// groupRecipientResult is one row in the response array. Either ID + EnqueuedAt
// are set (accepted) or RetryAfter / no further fields are set (rejected).
type groupRecipientResult struct {
	DevicePubkey string `json:"device_pubkey"`
	Status       string `json:"status"`
	ID           string `json:"id,omitempty"`
	EnqueuedAt   int64  `json:"enqueued_at,omitempty"`
	RetryAfter   int64  `json:"retry_after,omitempty"`
}

type groupPushResponse struct {
	Results []groupRecipientResult `json:"results"`
}

// newGroupPushHandler returns the POST /group_push handler.
//
// Flow (mirrors push.go where possible):
//  1. Read JSON body (capped at maxBodyBytes).
//  2. Validate request shape (recipients present, count under cap).
//  3. Verify the PagerOS-App + PagerOS-Sig + PagerOS-Timestamp triplet against
//     the marketplace registry, exactly as /push does.
//  4. Reject 403 if the app is banned (admin store).
//  5. For each recipient:
//     - Decode device pubkey + base64 payload; record per-recipient status
//       on parse failure (no early exit — the rest of the batch is unaffected).
//     - Consume one slot from the group-event rate limiter for the
//       (app, device) tuple. Limiter denial yields `rate_limited` for that
//       recipient only.
//     - Enqueue with Kind = KindGroupEvent so /pull surfaces the right kind.
//     - Record the send on the admin store.
//  6. Reply 202 with the per-recipient result array.
//
// Auth + ban + signature verification are all-or-nothing: those failures
// reject the whole batch with the same status codes as /push, since the
// relay can't trust *any* recipient bytes if it can't trust the sender.
func newGroupPushHandler(store storage.Store, lookup manifest.Lookup, recorder admin.Recorder, limiter ratelimit.Limiter, logger *slog.Logger, maxSkew time.Duration, maxBodyBytes int64, maxRecipients, maxPayloadBytes int, recipientCounter *metrics.Counter) http.Handler {
	if logger == nil {
		logger = slog.Default()
	}
	if maxBodyBytes <= 0 {
		maxBodyBytes = DefaultMaxGroupBytes
	}
	if maxSkew <= 0 {
		maxSkew = pageossig.DefaultMaxSkew
	}
	if maxRecipients <= 0 {
		maxRecipients = DefaultMaxGroupRecipients
	}
	if maxPayloadBytes <= 0 {
		maxPayloadBytes = DefaultMaxGroupPayloadBytes
	}

	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			w.Header().Set("Allow", http.MethodPost)
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}

		r.Body = http.MaxBytesReader(w, r.Body, maxBodyBytes)
		body, err := io.ReadAll(r.Body)
		if err != nil {
			var mb *http.MaxBytesError
			if errors.As(err, &mb) {
				http.Error(w, "payload too large", http.StatusRequestEntityTooLarge)
				return
			}
			writePushError(w, logger, http.StatusBadRequest, "read body failed", err)
			return
		}
		if len(body) == 0 {
			writePushError(w, logger, http.StatusBadRequest, "empty request body", nil)
			return
		}

		var req groupPushRequest
		if err := json.Unmarshal(body, &req); err != nil {
			writePushError(w, logger, http.StatusBadRequest, "invalid json", err)
			return
		}
		if len(req.Recipients) == 0 {
			writePushError(w, logger, http.StatusBadRequest, "recipients required", nil)
			return
		}
		if len(req.Recipients) > maxRecipients {
			writePushError(w, logger, http.StatusRequestEntityTooLarge, "too many recipients", nil)
			return
		}

		appID := r.Header.Get(HeaderApp)
		sigHdr := r.Header.Get(pageossig.HeaderSig)
		tsHdr := r.Header.Get(pageossig.HeaderTimestamp)

		if appID == "" {
			writePushError(w, logger, http.StatusUnauthorized, "missing app id header", nil)
			return
		}
		if !appIDPattern.MatchString(appID) {
			writePushError(w, logger, http.StatusForbidden, "invalid app id format", nil)
			return
		}
		if recorder != nil && recorder.IsBanned(appID) {
			writePushError(w, logger, http.StatusForbidden, "app is banned", nil)
			return
		}
		if sigHdr == "" {
			writePushError(w, logger, http.StatusUnauthorized, "missing signature header", nil)
			return
		}
		if tsHdr == "" {
			writePushError(w, logger, http.StatusUnauthorized, "missing timestamp header", nil)
			return
		}

		if lookup == nil {
			writePushError(w, logger, http.StatusServiceUnavailable, "manifest lookup not configured", nil)
			return
		}
		appPubkey, err := lookup.Pubkey(r.Context(), appID)
		if err != nil {
			switch {
			case errors.Is(err, manifest.ErrAppNotFound):
				writePushError(w, logger, http.StatusForbidden, "unknown app", err)
			case errors.Is(err, manifest.ErrLookupUnavailable):
				writePushError(w, logger, http.StatusServiceUnavailable, "marketplace lookup unavailable", err)
			default:
				writePushError(w, logger, http.StatusInternalServerError, "manifest lookup failed", err)
			}
			return
		}

		// Verify the sig over the raw body bytes. We do this after parsing so
		// a 400 (malformed JSON) doesn't masquerade as 401, but the signed
		// bytes are still the on-the-wire body — the same shape /push uses.
		if err := verifyAppSig(r, body, appPubkey, sigHdr, tsHdr, maxSkew, time.Now); err != nil {
			writePushError(w, logger, http.StatusUnauthorized, "signature verification failed", err)
			return
		}

		results := make([]groupRecipientResult, 0, len(req.Recipients))
		now := time.Now()
		for _, rec := range req.Recipients {
			res := groupRecipientResult{DevicePubkey: rec.DevicePubkey}
			if _, err := pageossig.DecodePubkey(rec.DevicePubkey); err != nil {
				res.Status = GroupResultBadDevice
				recordGroupResult(recipientCounter, res.Status)
				results = append(results, res)
				continue
			}
			payload, err := decodeGroupPayload(rec.PayloadB64)
			if err != nil {
				res.Status = GroupResultBadPayload
				recordGroupResult(recipientCounter, res.Status)
				results = append(results, res)
				continue
			}
			if len(payload) == 0 {
				res.Status = GroupResultPayloadEmpty
				recordGroupResult(recipientCounter, res.Status)
				results = append(results, res)
				continue
			}
			if len(payload) > maxPayloadBytes {
				res.Status = GroupResultPayloadLarge
				recordGroupResult(recipientCounter, res.Status)
				results = append(results, res)
				continue
			}

			// Group events use their own limiter bucket (SPEC §6.6.6); no
			// interaction with the /push limiter.
			if limiter != nil {
				if ok, retryAfter := limiter.Allow(appID, rec.DevicePubkey, now); !ok {
					res.Status = GroupResultRateLimited
					secs := int64(retryAfter / time.Second)
					if secs < 1 {
						secs = 1
					}
					res.RetryAfter = secs
					recordGroupResult(recipientCounter, res.Status)
					results = append(results, res)
					continue
				}
			}

			stored, err := store.Enqueue(r.Context(), rec.DevicePubkey, storage.Notification{
				Kind:    storage.KindGroupEvent,
				AppID:   appID,
				Payload: payload,
			})
			if err != nil {
				if errors.Is(err, storage.ErrPayloadTooLarge) {
					res.Status = GroupResultPayloadLarge
				} else {
					logger.Error("group enqueue failed", "err", err, "app", appID, "device", rec.DevicePubkey)
					res.Status = GroupResultStorageError
				}
				recordGroupResult(recipientCounter, res.Status)
				results = append(results, res)
				continue
			}

			if recorder != nil {
				recorder.RecordSend(appID, rec.DevicePubkey)
			}
			res.Status = GroupResultAccepted
			res.ID = stored.ID
			res.EnqueuedAt = stored.EnqueuedAt.Unix()
			recordGroupResult(recipientCounter, res.Status)
			results = append(results, res)
		}

		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusAccepted)
		_ = json.NewEncoder(w).Encode(groupPushResponse{Results: results})
	})
}

// recordGroupResult increments the per-recipient outcome counter if one is
// wired up. Kept here (not in metrics.go) because the status enum strings live
// alongside the handler that produces them.
func recordGroupResult(c *metrics.Counter, status string) {
	if c == nil {
		return
	}
	c.Inc("status", status)
}

// decodeGroupPayload accepts the canonical raw-base64url form produced by the
// Python SDK (no padding) and also the padded urlsafe form for forgiving
// clients. Standard base64 is rejected — URL-safe is the only encoding the
// rest of the protocol uses for opaque blobs.
func decodeGroupPayload(s string) ([]byte, error) {
	if s == "" {
		return nil, nil
	}
	if raw, err := base64.RawURLEncoding.DecodeString(s); err == nil {
		return raw, nil
	}
	raw, err := base64.URLEncoding.DecodeString(s)
	if err != nil {
		return nil, err
	}
	return raw, nil
}

