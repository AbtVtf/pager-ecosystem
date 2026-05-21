// SPDX-License-Identifier: Apache-2.0
//
// PagerOS input router — FW-024.
//
// Consumes the raw event streams from the encoder driver (FW-007) and the
// keyboard driver (FW-006) and dispatches each event to the currently
// focused widget. Events the focused widget declines bubble up to the app
// shell handler, matching SPEC.md §7.1 "Input Router" between the Shell
// and the drivers.
//
// Architecture:
//   - Both driver queues are joined into a FreeRTOS queue set and serviced
//     by a single dispatcher task.
//   - The task converts raw driver events into `pageros_router_event_t`
//     and runs the dispatch contract defined in `dispatch.h`.
//   - The routing rule itself lives in `dispatch.c`, which has zero
//     FreeRTOS / ESP-IDF dependencies so host tests can link it directly
//     (same split pattern as `quad.c` and `tca8418.c`).
//
// Handlers are installed via `pageros_router_set_focus_handler` (focused
// widget — current foreground app's currently focused widget) and
// `pageros_router_set_shell_handler` (app shell — invoked only when the
// focused widget returns false). Either may be NULL: with no focus
// handler the shell sees everything (the home-screen state); with no
// shell handler events the focused widget declines are reported as
// UNHANDLED to the internal dispatcher and silently dropped.

#pragma once

#include "esp_err.h"
#include "pageros_input.h"
#include "pageros_keyboard.h"

#include "dispatch.h"

#ifdef __cplusplus
extern "C" {
#endif

// Spin up the dispatcher task. Idempotent. The encoder driver
// (`pageros_input_init`) and keyboard driver (`pageros_kbd_init`) must
// have succeeded first — the router does not own those bring-ups, it
// only consumes their queues. If neither queue is available the router
// init returns ESP_ERR_INVALID_STATE.
esp_err_t pageros_router_init(void);

// Stop the dispatcher task and clear installed handlers.
esp_err_t pageros_router_shutdown(void);

// Install the focused-widget handler. NULL clears it; with no focus
// handler the shell handler (if any) receives every event directly. The
// handler is invoked from the router task — it must not block.
void pageros_router_set_focus_handler(pageros_router_handler_t fn, void *ctx);

// Install the app-shell handler. NULL clears it.
void pageros_router_set_shell_handler(pageros_router_handler_t fn, void *ctx);

// Convert a raw encoder/ENTER/BACK event into a router event. Exposed so
// the shell or a test harness can inject events without going through
// the dispatcher task.
void pageros_router_event_from_input(pageros_router_event_t *out,
                                     pageros_input_event_t  in,
                                     int64_t                ts_us);

// Convert a raw keyboard event into a router event.
void pageros_router_event_from_kbd(pageros_router_event_t    *out,
                                   const pageros_kbd_event_t *in);

#ifdef __cplusplus
}
#endif
