// SPDX-License-Identifier: Apache-2.0
//
// FW-024 dispatch rule. See `dispatch.h` for the contract.

#include "dispatch.h"

pageros_router_dispatch_result_t
pageros_router_dispatch(const pageros_router_dispatch_t *h,
                        const pageros_router_event_t   *ev)
{
    if (!h || !ev || ev->kind == PAGEROS_ROUTER_EVT_NONE) {
        return PAGEROS_ROUTER_DISPATCH_DROPPED;
    }
    if (h->focus && h->focus(ev, h->focus_ctx)) {
        return PAGEROS_ROUTER_DISPATCH_FOCUS;
    }
    if (h->shell) {
        return h->shell(ev, h->shell_ctx)
                 ? PAGEROS_ROUTER_DISPATCH_SHELL
                 : PAGEROS_ROUTER_DISPATCH_UNHANDLED;
    }
    return PAGEROS_ROUTER_DISPATCH_DROPPED;
}
