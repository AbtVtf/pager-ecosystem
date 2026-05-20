package server

import (
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"regexp"
	"strconv"
	"time"

	"github.com/pageros/pageros/push-relay/internal/manifest"
	"github.com/pageros/pageros/push-relay/internal/pageossig"
	"github.com/pageros/pageros/push-relay/internal/storage"
)

// HeaderApp identifies the sending app in /push requests. Distinct from the
// device-signing flow's PagerOS-Device, because the URL already carries the
// destination device pubkey — the signer here is the *app*, not the device.
const HeaderApp = "PagerOS-App"

// DefaultMaxPushBytes caps a single push payload's size on the wire. The
// per-device queue is capped at 1 MiB total (SPEC §6.6.4 / storage.MaxQueueBytes),
// and the per-device count cap is 16, so a single message materially larger
// than ~64 KiB will immediately evict prior pushes — bad for delivery. 64 KiB
// is plenty for the CBOR notification envelope (title + body + optional
// payload) per §10.2 / SPEC examples.
const DefaultMaxPushBytes int64 = 64 * 1024

// appIDPattern mirrors the marketplace registry's reverse-DNS id constraint
// (marketplace/api/pageros_marketplace/registry/app.py). Keeping it identical
// closes a small attack surface where a malformed app id could trip differing
// validation rules between the registry and the relay.
var appIDPattern = regexp.MustCompile(`^[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+$`)

// pushResponse is the 202 Accepted body of POST /push/<device_pubkey>.
type pushResponse struct {
	ID         string `json:"id"`
	EnqueuedAt int64  `json:"enqueued_at"`
}

// newPushHandler returns the POST /push/{device_pubkey} handler.
//
// Flow:
//  1. Read body (capped at maxBodyBytes).
//  2. Parse + sanity-check headers (PagerOS-App, PagerOS-Sig, PagerOS-Timestamp).
//  3. Look up the app's signing pubkey via the manifest registry.
//     Unknown app → 403 (SPEC §6.6.2: "Rejects unknown apps with 403.").
//     Registry unreachable → 503.
//  4. Verify Ed25519 sig over (method || url || timestamp || sha256(body))
//     using the app pubkey. Failure → 401.
//  5. Enqueue an opaque envelope with the URL device pubkey, AppID, and the
//     payload bytes. Returns 202 with the assigned notification id.
//
// The relay never decrypts the payload — body bytes are stored verbatim
// (SPEC §6.6.3).
func newPushHandler(store storage.Store, lookup manifest.Lookup, logger *slog.Logger, maxSkew time.Duration, maxBodyBytes int64) http.Handler {
	if logger == nil {
		logger = slog.Default()
	}
	if maxBodyBytes <= 0 {
		maxBodyBytes = DefaultMaxPushBytes
	}
	if maxSkew <= 0 {
		maxSkew = pageossig.DefaultMaxSkew
	}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// /push is mounted with `POST /push/{device_pubkey}` so the Go 1.22
		// mux already filters method, but defend in depth in case the route
		// is ever rewired.
		if r.Method != http.MethodPost {
			w.Header().Set("Allow", http.MethodPost)
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}

		devicePubkeyStr := r.PathValue("device_pubkey")
		if devicePubkeyStr == "" {
			writePushError(w, logger, http.StatusBadRequest, "missing device pubkey in path", nil)
			return
		}
		if _, err := pageossig.DecodePubkey(devicePubkeyStr); err != nil {
			writePushError(w, logger, http.StatusBadRequest, "invalid device pubkey", err)
			return
		}

		// Cap body before reading so a malicious sender can't OOM us.
		r.Body = http.MaxBytesReader(w, r.Body, maxBodyBytes)
		body, err := io.ReadAll(r.Body)
		if err != nil {
			// MaxBytesReader returns *http.MaxBytesError when the cap is hit.
			var mb *http.MaxBytesError
			if errors.As(err, &mb) {
				http.Error(w, "payload too large", http.StatusRequestEntityTooLarge)
				return
			}
			writePushError(w, logger, http.StatusBadRequest, "read body failed", err)
			return
		}
		if len(body) == 0 {
			// SPEC §6.6.3: payload is the opaque encrypted envelope. Empty
			// body would be a malformed request; reject before doing crypto
			// so we don't waste a signature check (and the verification of
			// an empty body would still pass — refusing it here is safer).
			writePushError(w, logger, http.StatusBadRequest, "empty payload", nil)
			return
		}

		// Pre-extract & sanity-check signature headers.
		appID := r.Header.Get(HeaderApp)
		sigHdr := r.Header.Get(pageossig.HeaderSig)
		tsHdr := r.Header.Get(pageossig.HeaderTimestamp)

		if appID == "" {
			writePushError(w, logger, http.StatusUnauthorized, "missing app id header", nil)
			return
		}
		if !appIDPattern.MatchString(appID) {
			// 403 rather than 400: a syntactically-malformed app id cannot be
			// registered in the marketplace, so it's a sub-case of "unknown
			// app" per SPEC §6.6.2 and gets the same response. This also
			// avoids leaking a parser-vs-registry distinction to clients.
			writePushError(w, logger, http.StatusForbidden, "invalid app id format", nil)
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

		// Look up the app pubkey BEFORE the crypto verify. The marketplace
		// is authoritative on whether the app exists — if it does not, we
		// must return 403 regardless of whether the sig would have been
		// valid against some other key.
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

		// Verify the request signature against the app's registered pubkey.
		if err := verifyAppSig(r, body, appPubkey, sigHdr, tsHdr, maxSkew, time.Now); err != nil {
			writePushError(w, logger, http.StatusUnauthorized, "signature verification failed", err)
			return
		}

		// Enqueue the opaque envelope. Per SPEC §6.6.3 the relay never
		// decrypts; body bytes are stored verbatim and re-emitted to the
		// device on /pull.
		stored, err := store.Enqueue(r.Context(), devicePubkeyStr, storage.Notification{
			Kind:    storage.KindNotification,
			AppID:   appID,
			Payload: body,
		})
		if err != nil {
			switch {
			case errors.Is(err, storage.ErrPayloadTooLarge):
				http.Error(w, "payload too large", http.StatusRequestEntityTooLarge)
			case errors.Is(err, storage.ErrInvalidKind):
				writePushError(w, logger, http.StatusInternalServerError, "invalid notification kind", err)
			default:
				logger.Error("storage.Enqueue failed", "err", err, "app", appID, "device", devicePubkeyStr)
				http.Error(w, "internal error", http.StatusInternalServerError)
			}
			return
		}

		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusAccepted)
		_ = json.NewEncoder(w).Encode(pushResponse{
			ID:         stored.ID,
			EnqueuedAt: stored.EnqueuedAt.Unix(),
		})
	})
}

// verifyAppSig runs the SPEC §9.2 verification against an externally-resolved
// pubkey. Separate from pageossig.Verify which reads the pubkey out of the
// PagerOS-Device header — for app push the signer key comes from the
// marketplace, not the request.
func verifyAppSig(r *http.Request, body []byte, pubkey ed25519.PublicKey, sigHdr, tsHdr string, maxSkew time.Duration, now func() time.Time) error {
	if now == nil {
		now = time.Now
	}
	sig, err := decodeSigB64(sigHdr)
	if err != nil {
		return fmt.Errorf("%w: %v", pageossig.ErrInvalidEncoding, err)
	}
	ts, err := strconv.ParseInt(tsHdr, 10, 64)
	if err != nil {
		return fmt.Errorf("%w: %s: %v", pageossig.ErrInvalidEncoding, pageossig.HeaderTimestamp, err)
	}
	delta := now().Unix() - ts
	if delta < 0 {
		delta = -delta
	}
	if time.Duration(delta)*time.Second > maxSkew {
		return fmt.Errorf("%w: |Δ|=%ds > %s", pageossig.ErrTimestampSkew, delta, maxSkew)
	}

	input := pageossig.BuildSigningInput(r.Method, r.URL.RequestURI(), tsHdr, body)
	if !ed25519.Verify(pubkey, input, sig) {
		return pageossig.ErrBadSignature
	}
	return nil
}

func decodeSigB64(s string) ([]byte, error) {
	// Accept padded and unpadded base64url; the canonical form is unpadded
	// (matches pageossig.Sign), but some clients re-encode.
	if raw, err := base64.RawURLEncoding.DecodeString(s); err == nil {
		if len(raw) != ed25519.SignatureSize {
			return nil, fmt.Errorf("%s: expected %d bytes, got %d", pageossig.HeaderSig, ed25519.SignatureSize, len(raw))
		}
		return raw, nil
	}
	raw, err := base64.URLEncoding.DecodeString(s)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", pageossig.HeaderSig, err)
	}
	if len(raw) != ed25519.SignatureSize {
		return nil, fmt.Errorf("%s: expected %d bytes, got %d", pageossig.HeaderSig, ed25519.SignatureSize, len(raw))
	}
	return raw, nil
}

// writePushError emits a uniform error response. We log the precise reason
// server-side but only return the generic status text to the client to avoid
// handing attackers a tighter oracle on which check failed (mirrors pull.go's
// writeUnauthorized).
func writePushError(w http.ResponseWriter, logger *slog.Logger, code int, msg string, cause error) {
	attrs := []any{"reason", msg, "status", code}
	if cause != nil {
		attrs = append(attrs, "err", cause.Error())
	}
	logger.Info("push rejected", attrs...)
	http.Error(w, http.StatusText(code), code)
}
