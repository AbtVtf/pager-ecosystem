// PagerOS firmware entry point.
//
// Skeleton (FW-001) + bootloader self-test (FW-002). Subsequent tasks
// (FW-003 filesystem mount, FW-005 display driver, etc.) bolt their
// initialization in here in dependency order. The self-test must run
// first per SPEC.md §7.2 boot flow.

#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pageros_gps.h"
#include "pageros_input.h"
#include "pageros_input_router.h"
#include "pageros_keyboard.h"
#include "selftest.h"

static const char *TAG = "pageros";

static const char *router_nav_name(pageros_router_nav_t n)
{
    switch (n) {
        case PAGEROS_ROUTER_NAV_ENTER:     return "ENTER";
        case PAGEROS_ROUTER_NAV_BACK:      return "BACK";
        case PAGEROS_ROUTER_NAV_BACK_LONG: return "BACK_LONG";
        default:                           return "?";
    }
}

// Default shell handler: accept and log everything the focused widget
// declines. Until the real Shell (PagerOS UI DSL renderer) lands this
// stands in as the "app shell" referenced by the FW-024 acceptance — it
// proves the bubble-up path end-to-end on the device.
static bool default_shell_handler(const pageros_router_event_t *ev, void *ctx)
{
    (void)ctx;
    switch (ev->kind) {
        case PAGEROS_ROUTER_EVT_ENCODER:
            ESP_LOGI("shell", "encoder %s",
                     ev->as.enc == PAGEROS_ROUTER_ENC_CW ? "CW" : "CCW");
            return true;
        case PAGEROS_ROUTER_EVT_NAV:
            ESP_LOGI("shell", "nav %s", router_nav_name(ev->as.nav));
            return true;
        case PAGEROS_ROUTER_EVT_KEY:
            ESP_LOGI("shell", "key row=%u col=%u %s",
                     ev->as.key.row, ev->as.key.col,
                     ev->as.key.pressed ? "DOWN" : "UP");
            return true;
        default:
            return false;
    }
}

static void on_gps_fix(const pageros_gps_fix_t *fix, void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI("gps",
             "fix: lat=%.6f lon=%.6f acc=%.1fm sats=%u utc_ms=%llu",
             fix->latitude_deg, fix->longitude_deg,
             (double)fix->accuracy_m, (unsigned)fix->satellites,
             (unsigned long long)fix->utc_epoch_ms);
}

void app_main(void)
{
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    ESP_LOGI(TAG, "PagerOS %s starting", app ? app->version : "?");
    ESP_LOGI(TAG, "chip: ESP32-S%d, cores=%d, rev=%d", chip.model, chip.cores, chip.revision);
    ESP_LOGI(TAG, "flash: %lu MB", (unsigned long)(flash_size / (1024 * 1024)));
    ESP_LOGI(TAG, "running partition: %s @ 0x%08lx (%lu KB)",
             running ? running->label : "?",
             running ? (unsigned long)running->address : 0UL,
             running ? (unsigned long)(running->size / 1024) : 0UL);

    // Boot self-test — see SPEC.md §7.2 step 2 / TASKS.md FW-002.
    selftest_result_t st = selftest_run();
    selftest_log_result(&st);
    selftest_halt_if_hard_fail(&st);

    // GPS bring-up (FW-011). Soft-fails: if the receiver isn't present
    // or the I2C expander mis-detects, log and continue so the rest of
    // the device still boots. Foreground apps that need location query
    // pageros_gps_get_last_fix(); subscribed apps can register their own
    // callback via pageros_gps_set_fix_callback().
    esp_err_t gps_err = pageros_gps_init();
    if (gps_err == ESP_OK) {
        pageros_gps_set_fix_callback(on_gps_fix, NULL);
    } else {
        ESP_LOGW(TAG, "GPS init skipped: %s", esp_err_to_name(gps_err));
    }

    // Input bring-up (FW-007 encoder + ENTER/BACK).
    esp_err_t input_err = pageros_input_init();
    if (input_err != ESP_OK) {
        ESP_LOGW(TAG, "input init skipped: %s", esp_err_to_name(input_err));
    }

    // Keyboard bring-up (FW-006). Keymap translation lives at the
    // shell/widget layer; the driver only emits raw (row,col) events.
    esp_err_t kbd_err = pageros_kbd_init();
    if (kbd_err != ESP_OK) {
        ESP_LOGW(TAG, "kbd init skipped: %s", esp_err_to_name(kbd_err));
    }

    // Input router (FW-024): joins both driver queues into one dispatcher
    // and routes events to the focused widget, falling back to the app
    // shell on bubble. Until a real foreground app installs a focus
    // handler, every event flows to `default_shell_handler` directly.
    if (input_err == ESP_OK || kbd_err == ESP_OK) {
        esp_err_t r = pageros_router_init();
        if (r == ESP_OK) {
            pageros_router_set_shell_handler(default_shell_handler, NULL);
        } else {
            ESP_LOGW(TAG, "router init skipped: %s", esp_err_to_name(r));
        }
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
