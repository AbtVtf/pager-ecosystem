package server

import (
	"context"
	"encoding/json"
	"net/http"
	"time"

	"github.com/pageros/pageros/push-relay/internal/storage"
)

type healthResponse struct {
	Status   string `json:"status"`
	Storage  string `json:"storage"`
	BuildTag string `json:"build_tag,omitempty"`
}

func newHealthzHandler(store storage.Store, buildTag string) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		ctx, cancel := context.WithTimeout(r.Context(), 2*time.Second)
		defer cancel()

		resp := healthResponse{Status: "ok", Storage: "ok", BuildTag: buildTag}
		code := http.StatusOK
		if store != nil {
			if err := store.Ping(ctx); err != nil {
				resp.Status = "degraded"
				resp.Storage = "unreachable"
				code = http.StatusServiceUnavailable
			}
		} else {
			resp.Storage = "absent"
		}

		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(code)
		_ = json.NewEncoder(w).Encode(resp)
	})
}
