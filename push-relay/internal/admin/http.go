package admin

import (
	"crypto/subtle"
	"encoding/json"
	"log/slog"
	"net/http"
	"regexp"
	"strings"
)

// appIDPattern mirrors server.appIDPattern so the admin API can validate ids
// before storing them. Kept independent so the admin package doesn't import
// the server package (which would loop the dependency graph).
var appIDPattern = regexp.MustCompile(`^[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+$`)

// MountConfig wires admin handlers onto a mux.
type MountConfig struct {
	// Store is the backing recorder + ban list.
	Store *Store
	// Token is the shared bearer token. If empty, all admin routes return 503
	// — the operator has not enabled the dashboard yet.
	Token  string
	Logger *slog.Logger
}

// Mount registers /admin/* routes onto mux.
//
// Routes:
//
//	GET    /admin/metrics       — full snapshot (per-app + per-device)
//	GET    /admin/bans          — list bans
//	POST   /admin/bans          — add ban ({"app_id":"x.y","reason":"..."})
//	DELETE /admin/bans/{app_id} — remove ban
//
// Auth: `Authorization: Bearer <token>` matching MountConfig.Token.
func Mount(mux *http.ServeMux, cfg MountConfig) {
	logger := cfg.Logger
	if logger == nil {
		logger = slog.Default()
	}

	guard := func(h http.HandlerFunc) http.HandlerFunc {
		return func(w http.ResponseWriter, r *http.Request) {
			if cfg.Token == "" {
				writeErr(w, http.StatusServiceUnavailable, "admin disabled")
				return
			}
			if !checkBearer(r, cfg.Token) {
				// Constant-time compare already happened inside checkBearer;
				// return a generic 401 to avoid leaking which check failed
				// (matches push.go's writePushError oracle defence).
				w.Header().Set("WWW-Authenticate", `Bearer realm="push-relay-admin"`)
				writeErr(w, http.StatusUnauthorized, "")
				return
			}
			h(w, r)
		}
	}

	mux.HandleFunc("GET /admin/metrics", guard(func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, http.StatusOK, cfg.Store.Snapshot())
	}))
	mux.HandleFunc("GET /admin/bans", guard(func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, http.StatusOK, struct {
			Bans []Ban `json:"bans"`
		}{Bans: cfg.Store.ListBans()})
	}))
	mux.HandleFunc("POST /admin/bans", guard(func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			AppID  string `json:"app_id"`
			Reason string `json:"reason"`
		}
		if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 4096)).Decode(&body); err != nil {
			writeErr(w, http.StatusBadRequest, "invalid json")
			return
		}
		if !appIDPattern.MatchString(body.AppID) {
			writeErr(w, http.StatusBadRequest, "invalid app_id")
			return
		}
		b := cfg.Store.Ban(body.AppID, body.Reason)
		logger.Info("admin ban added", "app_id", b.AppID, "reason", b.Reason)
		writeJSON(w, http.StatusCreated, b)
	}))
	mux.HandleFunc("DELETE /admin/bans/{app_id}", guard(func(w http.ResponseWriter, r *http.Request) {
		appID := r.PathValue("app_id")
		if !appIDPattern.MatchString(appID) {
			writeErr(w, http.StatusBadRequest, "invalid app_id")
			return
		}
		if !cfg.Store.Unban(appID) {
			writeErr(w, http.StatusNotFound, "not banned")
			return
		}
		logger.Info("admin ban removed", "app_id", appID)
		w.WriteHeader(http.StatusNoContent)
	}))
}

// checkBearer returns true if the Authorization header carries the configured
// bearer token. Uses constant-time compare so failed checks don't expose a
// timing oracle.
func checkBearer(r *http.Request, expected string) bool {
	auth := r.Header.Get("Authorization")
	const prefix = "Bearer "
	if !strings.HasPrefix(auth, prefix) {
		return false
	}
	got := auth[len(prefix):]
	if len(got) != len(expected) {
		// Still run the compare to keep timing roughly constant for short
		// tokens, but the unequal-length branch short-circuits the result.
		subtle.ConstantTimeCompare([]byte(got), []byte(got))
		return false
	}
	return subtle.ConstantTimeCompare([]byte(got), []byte(expected)) == 1
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

func writeErr(w http.ResponseWriter, code int, _ string) {
	http.Error(w, http.StatusText(code), code)
}
