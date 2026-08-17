// SPDX-License-Identifier: Apache-2.0
//
// PagerOS app runtime — FW-028 lifecycle.
//
// One foreground app slot at a time (SPEC §7.3). The runtime owns:
//
//   - the currently-mounted app's identity + cached current Frame,
//   - a 5-deep MRU "recents" list persisted to NVS,
//   - the open/switch/close transitions including memory cleanup,
//   - the hold-BACK force-quit edge.
//
// The runtime does NOT own:
//
//   - Frame fetching/decoding — that's the transport selector (FW-025)
//     + the cbor_codec (FW-016). The runtime hands the runtime a
//     decoded Frame and asks it to make this app the foreground.
//   - Rendering — widget renderer (FW-020..023).
//   - Input — the input_router (FW-024) forwards events to the runtime.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "pageros_cbor.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAGEROS_APPRT_MAX_APP_ID    128
#define PAGEROS_APPRT_RECENTS_MAX     5

// Lifecycle phases — exposed for tests + the Settings → diagnostics
// screen.
typedef enum {
    PAGEROS_APPRT_STATE_IDLE,         // no foreground app
    PAGEROS_APPRT_STATE_LOADING,      // mount in progress
    PAGEROS_APPRT_STATE_FOREGROUND,   // app rendered + interactive
    PAGEROS_APPRT_STATE_QUITTING,     // tearing down
} pageros_apprt_state_t;

typedef struct {
    // Wallclock seconds, NULL = time(NULL). Injected for tests.
    uint64_t (*now_unix)(void);

    // NVS namespace for the recents list. Defaults to "pageros_apprt".
    const char *nvs_namespace;
} pageros_apprt_opts_t;

esp_err_t pageros_apprt_init(const pageros_apprt_opts_t *opts);
esp_err_t pageros_apprt_shutdown(void);

// Bring `app_id` to the foreground using `initial_frame` as its first
// Screen. The frame bytes are owned by the runtime after this call
// (it makes its own copy + arena). Closes any prior foreground app.
esp_err_t pageros_apprt_open(const char *app_id,
                             const uint8_t *initial_frame_bytes,
                             size_t initial_frame_len);

// Replace the foreground app's current Frame (the post-navigation case).
// The runtime drops the prior arena/Frame and rebuilds.
esp_err_t pageros_apprt_set_frame(const uint8_t *frame_bytes, size_t len);

// Read-only access for renderers + input router.
const char *pageros_apprt_foreground_id(void);
const pgr_cbor_value_t *pageros_apprt_foreground_frame(void);
pageros_apprt_state_t pageros_apprt_state(void);

// Close the foreground app and return to the previous one in the
// recents stack. Idempotent if nothing is mounted. Returns the new
// foreground id (or NULL if there's no other app).
const char *pageros_apprt_back(void);

// Force-quit (hold-BACK). Drops the foreground without consulting recents.
esp_err_t pageros_apprt_kill(void);

// MRU recents list (most-recent first). Out-array must hold at least
// PAGEROS_APPRT_RECENTS_MAX `char *` pointers; entries are statically-
// owned strings valid until the next mutator call.
size_t pageros_apprt_recents(const char *out_app_ids[], size_t cap);

// --- Open-apps stack (cyberpunk desktop) ---------------------------- //
//
// Layered on top of `recents` to give the sidebar rail a notion of
// "open" apps. v1 semantics: an app is "open" iff it has an entry in
// this list. Switching to an open app re-fetches its home Frame (apps
// are stateless server-rendered) — but the list of open ids persists
// across switches and reboots so the rail stays stable.

#define PAGEROS_APPRT_OPEN_MAX 6

// Mark `app_id` as open. Idempotent — if it's already in the list it
// gets promoted to the front. Persists to NVS.
esp_err_t pageros_apprt_open_mark(const char *app_id);

// Remove `app_id` from the open list. Idempotent.
esp_err_t pageros_apprt_open_close(const char *app_id);

// Read the open list (most-recent first). Returns count.
size_t pageros_apprt_open_list(const char *out_app_ids[], size_t cap);

// Heap snapshot — exposed for the diagnostics screen.
typedef struct {
    pageros_apprt_state_t state;
    size_t   foreground_arena_bytes;
    uint64_t opens;
    uint64_t closes;
    uint64_t kills;
    uint64_t back_pops;
} pageros_apprt_stats_t;

void pageros_apprt_get_stats(pageros_apprt_stats_t *out);

#ifdef __cplusplus
}
#endif
