// SPDX-License-Identifier: Apache-2.0
//
// Pure-C dispatch contract for the PagerOS input router — FW-024.
//
// Split from the public `pageros_input_router.h` so the host tests under
// `firmware/host-tests/router/` can link the dispatch logic with native
// gcc without dragging in FreeRTOS or ESP-IDF headers (same split as
// `quad.h` for FW-007 and `tca8418.h` for FW-006).

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PAGEROS_ROUTER_EVT_NONE = 0,
    PAGEROS_ROUTER_EVT_ENCODER,
    PAGEROS_ROUTER_EVT_NAV,
    PAGEROS_ROUTER_EVT_KEY,
} pageros_router_event_kind_t;

typedef enum {
    PAGEROS_ROUTER_ENC_NONE = 0,
    PAGEROS_ROUTER_ENC_CW,
    PAGEROS_ROUTER_ENC_CCW,
} pageros_router_enc_t;

typedef enum {
    PAGEROS_ROUTER_NAV_NONE = 0,
    PAGEROS_ROUTER_NAV_ENTER,
    PAGEROS_ROUTER_NAV_ENTER_LONG,
    PAGEROS_ROUTER_NAV_BACK,
    PAGEROS_ROUTER_NAV_BACK_LONG,
} pageros_router_nav_t;

typedef struct {
    pageros_router_event_kind_t kind;
    int64_t ts_us;
    union {
        pageros_router_enc_t enc;
        pageros_router_nav_t nav;
        struct {
            uint8_t row;
            uint8_t col;
            bool    pressed;
        } key;
    } as;
} pageros_router_event_t;

// Handler returns true if it consumed the event, false to bubble.
typedef bool (*pageros_router_handler_t)(const pageros_router_event_t *ev,
                                         void *ctx);

typedef struct {
    pageros_router_handler_t focus;
    void                    *focus_ctx;
    pageros_router_handler_t shell;
    void                    *shell_ctx;
} pageros_router_dispatch_t;

typedef enum {
    PAGEROS_ROUTER_DISPATCH_DROPPED = 0,  // no handlers, or kind == NONE
    PAGEROS_ROUTER_DISPATCH_FOCUS,        // focused widget consumed it
    PAGEROS_ROUTER_DISPATCH_SHELL,        // bubbled and shell consumed it
    PAGEROS_ROUTER_DISPATCH_UNHANDLED,    // shell saw it but did not consume
} pageros_router_dispatch_result_t;

// FW-024 acceptance rule, in one function:
//   - if focus is set and returns true   → FOCUS
//   - else if shell is set and consumes  → SHELL
//   - else if shell is set, didn't       → UNHANDLED
//   - else                                → DROPPED
//
// kind == NONE and NULL inputs always return DROPPED.
pageros_router_dispatch_result_t
pageros_router_dispatch(const pageros_router_dispatch_t *handlers,
                        const pageros_router_event_t   *ev);

#ifdef __cplusplus
}
#endif
