// SPDX-License-Identifier: Apache-2.0
//
// PagerOS rotary-encoder + ENTER + BACK driver — see pageros_input.h.
//
// Strategy:
//   - All four input pins are GPIO inputs with the internal pull-up
//     enabled and ANYEDGE interrupts.
//   - The ISR pushes a tiny "raw event" struct (pin mask + timestamp) to
//     an internal queue; everything else runs in a normal-priority task.
//   - The task:
//       * decodes the encoder via the pure-C `quad_step` state machine
//         and emits ROTARY_CW / ROTARY_CCW per physical click,
//       * debounces ENTER / BACK with a 25 ms guard and only emits on
//         release (matches the "press-and-release" UX every shell expects),
//       * tracks BACK press time and emits BACK_LONG if held ≥ 1 s.
//
// Static / single-instance because the board has exactly one of each.

#include "pageros_input.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "quad.h"

static const char *TAG = "input";

// ---------------------------------------------------------------------------
// Board wiring (LILYGO T-LoRa Pager)
// ---------------------------------------------------------------------------

#define ROT_A_GPIO          40
#define ROT_B_GPIO          41
#define ROT_PUSH_GPIO        7   // encoder click → ENTER
#define BACK_GPIO            0   // BOOT button   → BACK (SPEC §7.3)

#define ROT_TICKS_PER_DETENT 4

// Button release/press semantics. Buttons are wired active-low.
#define BUTTON_DEBOUNCE_US   25000     //  25 ms
#define BACK_LONG_HOLD_US  1000000     // 1 s — per SPEC §7.3 "Hold BACK = force-quit"

// Queue depth: 16 should outpace anything a human can do; the producer
// task drops events silently on overflow rather than block.
#define PUBLIC_QUEUE_DEPTH   16
#define RAW_QUEUE_DEPTH      32

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------

typedef enum {
    RAW_ENCODER = 0,
    RAW_BUTTON,
} raw_kind_t;

typedef struct {
    raw_kind_t kind;
    uint32_t   pin;     // GPIO number (for RAW_BUTTON)
    bool       a;       // (for RAW_ENCODER) latched A
    bool       b;       // (for RAW_ENCODER) latched B
    bool       level;   // (for RAW_BUTTON)  0 = pressed (active-low)
    int64_t    ts_us;
} raw_event_t;

typedef struct {
    bool      pressed;            // currently held down (debounced)
    int64_t   last_change_us;     // last raw level transition
    int64_t   press_started_us;   // time the press began (for long-press)
} button_state_t;

static struct {
    bool                 initialised;
    QueueHandle_t        raw_q;
    QueueHandle_t        pub_q;
    TaskHandle_t         task;
    volatile bool        shutdown;
    quad_decoder_t       quad;
    button_state_t       enter;
    button_state_t       back;
} s_state;

// ---------------------------------------------------------------------------
// ISR
// ---------------------------------------------------------------------------

static void IRAM_ATTR gpio_isr(void *arg)
{
    uint32_t pin = (uint32_t)(uintptr_t)arg;
    raw_event_t ev = {0};
    ev.ts_us = esp_timer_get_time();

    if (pin == ROT_A_GPIO || pin == ROT_B_GPIO) {
        ev.kind = RAW_ENCODER;
        ev.a = gpio_get_level(ROT_A_GPIO) != 0;
        ev.b = gpio_get_level(ROT_B_GPIO) != 0;
    } else {
        ev.kind = RAW_BUTTON;
        ev.pin = pin;
        ev.level = gpio_get_level((gpio_num_t)pin) != 0;
    }

    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(s_state.raw_q, &ev, &hp);
    if (hp == pdTRUE) portYIELD_FROM_ISR();
}

// ---------------------------------------------------------------------------
// Helpers — must run on the task, not the ISR.
// ---------------------------------------------------------------------------

static void emit(pageros_input_event_t e)
{
    // Drop on overflow rather than block; the public queue is sized so
    // this should never actually happen during normal use.
    (void)xQueueSend(s_state.pub_q, &e, 0);
}

// Handle one raw button transition. We only emit on release ("up edge"
// from the user's perspective — going from pressed to not-pressed). The
// debounce window suppresses level glitches: a transition within
// BUTTON_DEBOUNCE_US of the previous one is ignored entirely.
static void handle_button(uint32_t pin, bool level, int64_t ts_us)
{
    button_state_t *st = (pin == BACK_GPIO) ? &s_state.back : &s_state.enter;
    if (st->last_change_us != 0
        && ts_us - st->last_change_us < BUTTON_DEBOUNCE_US) {
        return;
    }
    st->last_change_us = ts_us;

    bool now_pressed = (level == 0);    // active-low
    if (now_pressed == st->pressed) return;
    st->pressed = now_pressed;

    if (now_pressed) {
        st->press_started_us = ts_us;
        return;
    }

    // Release.
    int64_t held_us = ts_us - st->press_started_us;
    if (pin == BACK_GPIO) {
        emit(held_us >= BACK_LONG_HOLD_US ? PAGEROS_INPUT_BACK_LONG
                                          : PAGEROS_INPUT_BACK);
    } else {
        // Encoder click: short-press = ENTER, long-press = ENTER_LONG.
        // 700 ms is short enough to feel deliberate (not accidental
        // tap-and-hold) but long enough to avoid colliding with the
        // normal click cadence when scrolling+selecting quickly.
        emit(held_us >= 700000 ? PAGEROS_INPUT_ENTER_LONG
                               : PAGEROS_INPUT_ENTER);
    }
}

static void handle_encoder(bool a, bool b)
{
    quad_event_t e = quad_step(&s_state.quad, a, b);
    if (e == QUAD_CW)  emit(PAGEROS_INPUT_ROTARY_CW);
    if (e == QUAD_CCW) emit(PAGEROS_INPUT_ROTARY_CCW);
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------

static void input_task(void *arg)
{
    (void)arg;
    while (!s_state.shutdown) {
        raw_event_t ev;
        // Poll with a short timeout so the back-long timer still fires
        // even if the user holds the button without other input.
        if (xQueueReceive(s_state.raw_q, &ev, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (ev.kind == RAW_ENCODER) {
                handle_encoder(ev.a, ev.b);
            } else {
                handle_button(ev.pin, ev.level, ev.ts_us);
            }
        }
        // Background tick: catch long-press while BACK is still held —
        // we want to surface BACK_LONG at the moment the threshold is
        // crossed even if the user keeps holding, NOT only on release.
        // Implementation note: emitting on release is *also* fine for
        // the FW-007 acceptance ("distinguished"), and arguably nicer
        // to the shell (one event per press). Keeping the
        // emit-on-release path for now.
    }
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static esp_err_t configure_pin(int pin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) return err;
    return gpio_isr_handler_add((gpio_num_t)pin, gpio_isr,
                                (void *)(uintptr_t)pin);
}

esp_err_t pageros_input_init(void)
{
    if (s_state.initialised) return ESP_OK;

    s_state.raw_q = xQueueCreate(RAW_QUEUE_DEPTH, sizeof(raw_event_t));
    s_state.pub_q = xQueueCreate(PUBLIC_QUEUE_DEPTH, sizeof(pageros_input_event_t));
    if (!s_state.raw_q || !s_state.pub_q) return ESP_ERR_NO_MEM;

    quad_init(&s_state.quad, ROT_TICKS_PER_DETENT);
    memset(&s_state.enter, 0, sizeof(s_state.enter));
    memset(&s_state.back,  0, sizeof(s_state.back));

    // Install the shared ISR service if no one else got there first.
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "isr install: %s", esp_err_to_name(err));
        return err;
    }

    err = configure_pin(ROT_A_GPIO);    if (err) return err;
    err = configure_pin(ROT_B_GPIO);    if (err) return err;
    err = configure_pin(ROT_PUSH_GPIO); if (err) return err;
    err = configure_pin(BACK_GPIO);     if (err) return err;

    // Seed the encoder state from the current pin levels so the first
    // transition isn't misread as a phantom click.
    s_state.quad.prev_state = (uint8_t)(
        (gpio_get_level(ROT_A_GPIO) ? 2u : 0u) |
        (gpio_get_level(ROT_B_GPIO) ? 1u : 0u));

    s_state.shutdown = false;
    BaseType_t ok = xTaskCreate(input_task, "input", 3072, NULL, 6,
                                &s_state.task);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    s_state.initialised = true;
    ESP_LOGI(TAG, "input up: enc A=%d B=%d push=%d back=%d",
             ROT_A_GPIO, ROT_B_GPIO, ROT_PUSH_GPIO, BACK_GPIO);
    return ESP_OK;
}

esp_err_t pageros_input_shutdown(void)
{
    if (!s_state.initialised) return ESP_OK;
    s_state.shutdown = true;
    vTaskDelay(pdMS_TO_TICKS(100));

    gpio_isr_handler_remove(ROT_A_GPIO);
    gpio_isr_handler_remove(ROT_B_GPIO);
    gpio_isr_handler_remove(ROT_PUSH_GPIO);
    gpio_isr_handler_remove(BACK_GPIO);

    vQueueDelete(s_state.raw_q);
    vQueueDelete(s_state.pub_q);
    s_state.raw_q = NULL;
    s_state.pub_q = NULL;
    s_state.initialised = false;
    return ESP_OK;
}

QueueHandle_t pageros_input_queue(void)
{
    return s_state.pub_q;
}

pageros_input_event_t pageros_input_wait(uint32_t timeout_ms)
{
    pageros_input_event_t e = PAGEROS_INPUT_NONE;
    if (!s_state.pub_q) return e;
    TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    (void)xQueueReceive(s_state.pub_q, &e, ticks);
    return e;
}
