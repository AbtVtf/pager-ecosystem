// SPDX-License-Identifier: Apache-2.0
//
// Host-side unit tests for the FW-024 input router dispatch contract.
//
// Verifies the acceptance from TASKS.md: "Encoder/keyboard events reach
// the focused widget; widgets bubble unhandled events up to the app shell."
//
// These tests link `dispatch.c` directly with native gcc — no FreeRTOS,
// no ESP-IDF — to exercise just the routing rule (the FreeRTOS plumbing
// around it is exercised on real hardware).

#include "dispatch.h"

#include <stdio.h>
#include <string.h>

static int fail_count = 0;

#define CHECK(cond, ...) do {                                       \
    if (!(cond)) {                                                  \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
        fprintf(stderr, __VA_ARGS__);                               \
        fprintf(stderr, "\n");                                      \
        fail_count++;                                               \
    }                                                               \
} while (0)

typedef struct {
    int                     calls;
    bool                    consume;     // what to return
    pageros_router_event_t  last;
    void                   *seen_ctx;
} probe_t;

static bool probe_handler(const pageros_router_event_t *ev, void *ctx)
{
    probe_t *p = (probe_t *)ctx;
    p->calls++;
    p->last     = *ev;
    p->seen_ctx = ctx;
    return p->consume;
}

static pageros_router_event_t key_ev(uint8_t r, uint8_t c, bool pressed)
{
    pageros_router_event_t e = {0};
    e.kind          = PAGEROS_ROUTER_EVT_KEY;
    e.as.key.row    = r;
    e.as.key.col    = c;
    e.as.key.pressed = pressed;
    return e;
}

static pageros_router_event_t enc_ev(pageros_router_enc_t d)
{
    pageros_router_event_t e = {0};
    e.kind   = PAGEROS_ROUTER_EVT_ENCODER;
    e.as.enc = d;
    return e;
}

static pageros_router_event_t nav_ev(pageros_router_nav_t n)
{
    pageros_router_event_t e = {0};
    e.kind   = PAGEROS_ROUTER_EVT_NAV;
    e.as.nav = n;
    return e;
}

// Focused widget consumes → shell never sees it.
static void test_focus_consumes(void)
{
    probe_t focus = { .consume = true };
    probe_t shell = { .consume = true };
    pageros_router_dispatch_t h = {
        .focus = probe_handler, .focus_ctx = &focus,
        .shell = probe_handler, .shell_ctx = &shell,
    };
    pageros_router_event_t e = key_ev(1, 2, true);
    pageros_router_dispatch_result_t r = pageros_router_dispatch(&h, &e);
    CHECK(r == PAGEROS_ROUTER_DISPATCH_FOCUS, "expected FOCUS, got %d", r);
    CHECK(focus.calls == 1, "focus.calls=%d", focus.calls);
    CHECK(shell.calls == 0, "shell should not have been called; got %d",
          shell.calls);
    CHECK(focus.last.kind == PAGEROS_ROUTER_EVT_KEY, "focus saw wrong kind");
    CHECK(focus.last.as.key.row == 1 && focus.last.as.key.col == 2,
          "row/col mangled");
}

// Focus declines → shell consumes. This is the FW-024 bubble-up acceptance.
static void test_focus_bubbles_to_shell(void)
{
    probe_t focus = { .consume = false };
    probe_t shell = { .consume = true };
    pageros_router_dispatch_t h = {
        .focus = probe_handler, .focus_ctx = &focus,
        .shell = probe_handler, .shell_ctx = &shell,
    };
    pageros_router_event_t e = nav_ev(PAGEROS_ROUTER_NAV_BACK);
    pageros_router_dispatch_result_t r = pageros_router_dispatch(&h, &e);
    CHECK(r == PAGEROS_ROUTER_DISPATCH_SHELL, "expected SHELL, got %d", r);
    CHECK(focus.calls == 1 && shell.calls == 1,
          "both should fire (focus=%d, shell=%d)", focus.calls, shell.calls);
    CHECK(shell.last.kind == PAGEROS_ROUTER_EVT_NAV
          && shell.last.as.nav == PAGEROS_ROUTER_NAV_BACK,
          "shell saw wrong nav event");
}

// No focus handler → shell sees everything (matches "Shell is foreground").
static void test_no_focus_handler(void)
{
    probe_t shell = { .consume = true };
    pageros_router_dispatch_t h = {
        .shell = probe_handler, .shell_ctx = &shell,
    };
    pageros_router_event_t e = enc_ev(PAGEROS_ROUTER_ENC_CW);
    pageros_router_dispatch_result_t r = pageros_router_dispatch(&h, &e);
    CHECK(r == PAGEROS_ROUTER_DISPATCH_SHELL, "expected SHELL, got %d", r);
    CHECK(shell.calls == 1, "shell.calls=%d", shell.calls);
    CHECK(shell.last.as.enc == PAGEROS_ROUTER_ENC_CW, "shell saw wrong enc");
}

// Neither handler consumes → result UNHANDLED. Caller can log/drop without
// confusing it with "no handlers installed at all".
static void test_unhandled(void)
{
    probe_t focus = { .consume = false };
    probe_t shell = { .consume = false };
    pageros_router_dispatch_t h = {
        .focus = probe_handler, .focus_ctx = &focus,
        .shell = probe_handler, .shell_ctx = &shell,
    };
    pageros_router_event_t e = enc_ev(PAGEROS_ROUTER_ENC_CCW);
    pageros_router_dispatch_result_t r = pageros_router_dispatch(&h, &e);
    CHECK(r == PAGEROS_ROUTER_DISPATCH_UNHANDLED,
          "expected UNHANDLED, got %d", r);
    CHECK(focus.calls == 1 && shell.calls == 1,
          "both must be called for bubble verification");
}

// No handlers at all → DROPPED.
static void test_dropped_when_no_handlers(void)
{
    pageros_router_dispatch_t h = {0};
    pageros_router_event_t e = key_ev(0, 0, true);
    pageros_router_dispatch_result_t r = pageros_router_dispatch(&h, &e);
    CHECK(r == PAGEROS_ROUTER_DISPATCH_DROPPED,
          "expected DROPPED, got %d", r);
}

// kind == NONE never fires either handler — prevents an accidental
// memset-initialised event from being treated as a real one.
static void test_none_event_is_dropped(void)
{
    probe_t focus = { .consume = true };
    probe_t shell = { .consume = true };
    pageros_router_dispatch_t h = {
        .focus = probe_handler, .focus_ctx = &focus,
        .shell = probe_handler, .shell_ctx = &shell,
    };
    pageros_router_event_t e = {0};  // kind == NONE
    pageros_router_dispatch_result_t r = pageros_router_dispatch(&h, &e);
    CHECK(r == PAGEROS_ROUTER_DISPATCH_DROPPED,
          "expected DROPPED, got %d", r);
    CHECK(focus.calls == 0 && shell.calls == 0,
          "NONE event must not fire handlers (focus=%d, shell=%d)",
          focus.calls, shell.calls);
}

// NULL inputs are tolerated.
static void test_null_safety(void)
{
    pageros_router_event_t e = key_ev(0, 0, true);
    CHECK(pageros_router_dispatch(NULL, &e)
            == PAGEROS_ROUTER_DISPATCH_DROPPED, "null handlers");
    pageros_router_dispatch_t h = {0};
    CHECK(pageros_router_dispatch(&h, NULL)
            == PAGEROS_ROUTER_DISPATCH_DROPPED, "null event");
}

// Per-handler ctx is plumbed through unchanged — a single shared handler
// implementation can identify its caller by ctx alone.
static void test_ctx_threading(void)
{
    probe_t focus = { .consume = false };
    probe_t shell = { .consume = true };
    pageros_router_dispatch_t h = {
        .focus = probe_handler, .focus_ctx = &focus,
        .shell = probe_handler, .shell_ctx = &shell,
    };
    pageros_router_event_t e = key_ev(3, 4, false);
    (void)pageros_router_dispatch(&h, &e);
    CHECK(focus.seen_ctx == &focus, "focus ctx not threaded");
    CHECK(shell.seen_ctx == &shell, "shell ctx not threaded");
}

// Encoder direction is preserved end-to-end.
static void test_encoder_direction_preserved(void)
{
    probe_t shell = { .consume = true };
    pageros_router_dispatch_t h = {
        .shell = probe_handler, .shell_ctx = &shell,
    };
    pageros_router_event_t cw  = enc_ev(PAGEROS_ROUTER_ENC_CW);
    pageros_router_event_t ccw = enc_ev(PAGEROS_ROUTER_ENC_CCW);
    (void)pageros_router_dispatch(&h, &cw);
    CHECK(shell.last.as.enc == PAGEROS_ROUTER_ENC_CW, "CW lost");
    (void)pageros_router_dispatch(&h, &ccw);
    CHECK(shell.last.as.enc == PAGEROS_ROUTER_ENC_CCW, "CCW lost");
}

int main(void)
{
    test_focus_consumes();
    test_focus_bubbles_to_shell();
    test_no_focus_handler();
    test_unhandled();
    test_dropped_when_no_handlers();
    test_none_event_is_dropped();
    test_null_safety();
    test_ctx_threading();
    test_encoder_direction_preserved();

    if (fail_count == 0) {
        printf("OK (9 test cases)\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d assertion(s)\n", fail_count);
    return 1;
}
