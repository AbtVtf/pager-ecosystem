// SPDX-License-Identifier: Apache-2.0
// FW-035 — power management.

#include "pageros_power.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pageros_display.h"

static const char *TAG = "power";

#define DEFAULT_DIM_MS          30000u
#define DEFAULT_SCREEN_OFF_MS   60000u
#define DEFAULT_LIGHT_SLEEP_MS  300000u
#define DEFAULT_BL_ACTIVE       255
#define DEFAULT_BL_DIM          64

static struct {
    bool inited;
    pageros_power_opts_t opts;
    pageros_power_state_t state;
    int64_t last_kick_us;
} g;

static int64_t now_us(void) { return esp_timer_get_time(); }

static const char *state_name(pageros_power_state_t s)
{
    switch (s) {
        case PAGEROS_POWER_ACTIVE:      return "ACTIVE";
        case PAGEROS_POWER_DIM:         return "DIM";
        case PAGEROS_POWER_SCREEN_OFF:  return "SCREEN_OFF";
        case PAGEROS_POWER_LIGHT_SLEEP: return "LIGHT_SLEEP";
        case PAGEROS_POWER_DEEP_SLEEP:  return "DEEP_SLEEP";
        default:                        return "?";
    }
}

static void apply_state(pageros_power_state_t to)
{
    if (g.state == to) return;
    pageros_power_state_t from = g.state;
    g.state = to;
    ESP_LOGI(TAG, "%s -> %s", state_name(from), state_name(to));

    switch (to) {
        case PAGEROS_POWER_ACTIVE:
            pageros_display_backlight(g.opts.bl_active);
            break;
        case PAGEROS_POWER_DIM:
            pageros_display_backlight(g.opts.bl_dim);
            break;
        case PAGEROS_POWER_SCREEN_OFF:
            pageros_display_backlight(0);
            break;
        case PAGEROS_POWER_LIGHT_SLEEP:
            // Don't actually invoke esp_light_sleep_start() from this
            // task — that would block the manager task itself. The
            // intended pattern is for an idle hook to enter the
            // sleep when no work is pending. For v0 we just gate the
            // backlight and let the system idle into its own
            // automatic light sleep (esp_pm_configure has it enabled
            // via sdkconfig when present).
            pageros_display_backlight(0);
            break;
        case PAGEROS_POWER_DEEP_SLEEP:
            pageros_display_backlight(0);
            ESP_LOGI(TAG, "entering deep sleep");
            esp_deep_sleep_start();
            // No return.
            break;
    }
}

// Manager task: every 1 s, check elapsed-since-kick and demote the
// state if a threshold has been crossed.
static void manager_task(void *arg)
{
    (void)arg;
    while (true) {
        int64_t elapsed_ms = (now_us() - g.last_kick_us) / 1000;
        pageros_power_state_t want = g.state;
        if (elapsed_ms >= (int64_t)g.opts.light_sleep_after_ms)      want = PAGEROS_POWER_LIGHT_SLEEP;
        else if (elapsed_ms >= (int64_t)g.opts.screen_off_after_ms)  want = PAGEROS_POWER_SCREEN_OFF;
        else if (elapsed_ms >= (int64_t)g.opts.dim_after_ms)         want = PAGEROS_POWER_DIM;
        else                                                          want = PAGEROS_POWER_ACTIVE;
        // Only demote here — promotion is owned by `pageros_power_kick`
        // so a stale check doesn't override a fresh kick.
        if ((int)want > (int)g.state) apply_state(want);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t pageros_power_init(const pageros_power_opts_t *opts)
{
    if (g.inited) return ESP_OK;
    pageros_power_opts_t resolved = {
        .dim_after_ms         = DEFAULT_DIM_MS,
        .screen_off_after_ms  = DEFAULT_SCREEN_OFF_MS,
        .light_sleep_after_ms = DEFAULT_LIGHT_SLEEP_MS,
        .bl_active            = DEFAULT_BL_ACTIVE,
        .bl_dim               = DEFAULT_BL_DIM,
    };
    if (opts) {
        if (opts->dim_after_ms)         resolved.dim_after_ms         = opts->dim_after_ms;
        if (opts->screen_off_after_ms)  resolved.screen_off_after_ms  = opts->screen_off_after_ms;
        if (opts->light_sleep_after_ms) resolved.light_sleep_after_ms = opts->light_sleep_after_ms;
        if (opts->bl_active)            resolved.bl_active            = opts->bl_active;
        // bl_dim may legitimately be 0 (off in DIM); copy as-is when caller passes opts.
        resolved.bl_dim = opts->bl_dim;
    }
    g.opts = resolved;
    g.state = PAGEROS_POWER_ACTIVE;
    g.last_kick_us = now_us();
    g.inited = true;
    pageros_display_backlight(g.opts.bl_active);

    xTaskCreate(manager_task, "power_mgr", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "init: dim=%us screen_off=%us light_sleep=%us",
             (unsigned)(g.opts.dim_after_ms / 1000),
             (unsigned)(g.opts.screen_off_after_ms / 1000),
             (unsigned)(g.opts.light_sleep_after_ms / 1000));
    return ESP_OK;
}

void pageros_power_kick(void)
{
    if (!g.inited) return;
    g.last_kick_us = now_us();
    if (g.state != PAGEROS_POWER_ACTIVE) apply_state(PAGEROS_POWER_ACTIVE);
}

pageros_power_state_t pageros_power_state(void)
{
    return g.inited ? g.state : PAGEROS_POWER_ACTIVE;
}

esp_err_t pageros_power_set_state(pageros_power_state_t state)
{
    if (!g.inited) return ESP_ERR_INVALID_STATE;
    apply_state(state);
    return ESP_OK;
}

uint32_t pageros_power_ms_since_kick(void)
{
    if (!g.inited) return 0;
    return (uint32_t)((now_us() - g.last_kick_us) / 1000);
}
