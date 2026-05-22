// SPDX-License-Identifier: Apache-2.0
//
// PagerOS input router — see `pageros_input_router.h`.

#include "pageros_input_router.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "router";

static struct {
    bool                       initialised;
    TaskHandle_t               task;
    QueueSetHandle_t           qset;
    QueueHandle_t              kbd_q;
    QueueHandle_t              input_q;
    volatile bool              shutdown;
    pageros_router_dispatch_t  handlers;
} s_state;

// ---------------------------------------------------------------------------
// Adapters: raw driver event → router event.
// ---------------------------------------------------------------------------

void pageros_router_event_from_input(pageros_router_event_t *out,
                                     pageros_input_event_t   in,
                                     int64_t                 ts_us)
{
    memset(out, 0, sizeof(*out));
    out->ts_us = ts_us;
    switch (in) {
        case PAGEROS_INPUT_ROTARY_CW:
            out->kind   = PAGEROS_ROUTER_EVT_ENCODER;
            out->as.enc = PAGEROS_ROUTER_ENC_CW;
            return;
        case PAGEROS_INPUT_ROTARY_CCW:
            out->kind   = PAGEROS_ROUTER_EVT_ENCODER;
            out->as.enc = PAGEROS_ROUTER_ENC_CCW;
            return;
        case PAGEROS_INPUT_ENTER:
            out->kind   = PAGEROS_ROUTER_EVT_NAV;
            out->as.nav = PAGEROS_ROUTER_NAV_ENTER;
            return;
        case PAGEROS_INPUT_ENTER_LONG:
            out->kind   = PAGEROS_ROUTER_EVT_NAV;
            out->as.nav = PAGEROS_ROUTER_NAV_ENTER_LONG;
            return;
        case PAGEROS_INPUT_BACK:
            out->kind   = PAGEROS_ROUTER_EVT_NAV;
            out->as.nav = PAGEROS_ROUTER_NAV_BACK;
            return;
        case PAGEROS_INPUT_BACK_LONG:
            out->kind   = PAGEROS_ROUTER_EVT_NAV;
            out->as.nav = PAGEROS_ROUTER_NAV_BACK_LONG;
            return;
        case PAGEROS_INPUT_NONE:
        default:
            out->kind = PAGEROS_ROUTER_EVT_NONE;
            return;
    }
}

void pageros_router_event_from_kbd(pageros_router_event_t    *out,
                                   const pageros_kbd_event_t *in)
{
    memset(out, 0, sizeof(*out));
    out->kind           = PAGEROS_ROUTER_EVT_KEY;
    out->ts_us          = in->ts_us;
    out->as.key.row     = in->row;
    out->as.key.col     = in->col;
    out->as.key.pressed = in->pressed;
}

// ---------------------------------------------------------------------------
// Handler installation.
// ---------------------------------------------------------------------------

void pageros_router_set_focus_handler(pageros_router_handler_t fn, void *ctx)
{
    s_state.handlers.focus     = fn;
    s_state.handlers.focus_ctx = ctx;
}

void pageros_router_set_shell_handler(pageros_router_handler_t fn, void *ctx)
{
    s_state.handlers.shell     = fn;
    s_state.handlers.shell_ctx = ctx;
}

// ---------------------------------------------------------------------------
// Dispatcher task.
// ---------------------------------------------------------------------------

static void router_task(void *arg)
{
    (void)arg;
    while (!s_state.shutdown) {
        // 100 ms timeout doubles as a shutdown poll.
        QueueSetMemberHandle_t hit =
            xQueueSelectFromSet(s_state.qset, pdMS_TO_TICKS(100));
        if (!hit) continue;

        pageros_router_event_t ev = {0};
        if (hit == s_state.input_q) {
            pageros_input_event_t raw = PAGEROS_INPUT_NONE;
            if (xQueueReceive(s_state.input_q, &raw, 0) != pdTRUE) continue;
            pageros_router_event_from_input(&ev, raw, esp_timer_get_time());
        } else if (hit == s_state.kbd_q) {
            pageros_kbd_event_t raw;
            if (xQueueReceive(s_state.kbd_q, &raw, 0) != pdTRUE) continue;
            pageros_router_event_from_kbd(&ev, &raw);
        } else {
            // Shouldn't happen — only members of the set fire.
            continue;
        }

        (void)pageros_router_dispatch(&s_state.handlers, &ev);
    }
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public init / shutdown.
// ---------------------------------------------------------------------------

static UBaseType_t queue_capacity(QueueHandle_t q)
{
    if (!q) return 0;
    // ESP-IDF FreeRTOS: queue total length = spaces + messages waiting.
    return uxQueueSpacesAvailable(q) + uxQueueMessagesWaiting(q);
}

esp_err_t pageros_router_init(void)
{
    if (s_state.initialised) return ESP_OK;

    s_state.input_q = pageros_input_queue();
    s_state.kbd_q   = pageros_kbd_queue();
    if (!s_state.input_q && !s_state.kbd_q) {
        ESP_LOGE(TAG, "no input queues available; init drivers first");
        return ESP_ERR_INVALID_STATE;
    }

    // FreeRTOS requires the queue set's event-queue length to be ≥ the
    // sum of the constituent queue lengths.
    UBaseType_t capacity =
        queue_capacity(s_state.input_q) + queue_capacity(s_state.kbd_q);
    if (capacity == 0) capacity = 16;

    s_state.qset = xQueueCreateSet(capacity);
    if (!s_state.qset) return ESP_ERR_NO_MEM;

    if (s_state.input_q
        && xQueueAddToSet(s_state.input_q, s_state.qset) != pdPASS) {
        vQueueDelete(s_state.qset);
        s_state.qset = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state.kbd_q
        && xQueueAddToSet(s_state.kbd_q, s_state.qset) != pdPASS) {
        if (s_state.input_q) xQueueRemoveFromSet(s_state.input_q, s_state.qset);
        vQueueDelete(s_state.qset);
        s_state.qset = NULL;
        return ESP_ERR_INVALID_STATE;
    }

    s_state.shutdown = false;
    // Larger stack so the shell handler can do heavy work inline —
    // SD mount, HTTPS fetch (mbedTLS + crt bundle), CBOR parse,
    // widget render. Stock 3 KB overflows the moment `addapp_install`
    // touches the network stack.
    BaseType_t ok = xTaskCreate(router_task, "router", 12 * 1024, NULL, 6,
                                &s_state.task);
    if (ok != pdPASS) {
        if (s_state.input_q) xQueueRemoveFromSet(s_state.input_q, s_state.qset);
        if (s_state.kbd_q)   xQueueRemoveFromSet(s_state.kbd_q,   s_state.qset);
        vQueueDelete(s_state.qset);
        s_state.qset = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_state.initialised = true;
    ESP_LOGI(TAG, "input router up (encoder_q=%p kbd_q=%p, set capacity=%u)",
             s_state.input_q, s_state.kbd_q, (unsigned)capacity);
    return ESP_OK;
}

esp_err_t pageros_router_shutdown(void)
{
    if (!s_state.initialised) return ESP_OK;
    s_state.shutdown = true;
    vTaskDelay(pdMS_TO_TICKS(150));

    if (s_state.input_q) xQueueRemoveFromSet(s_state.input_q, s_state.qset);
    if (s_state.kbd_q)   xQueueRemoveFromSet(s_state.kbd_q,   s_state.qset);
    vQueueDelete(s_state.qset);
    s_state.qset = NULL;

    memset(&s_state.handlers, 0, sizeof(s_state.handlers));
    s_state.initialised = false;
    return ESP_OK;
}
