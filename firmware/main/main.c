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
#include "pageros_keyboard.h"
#include "selftest.h"

static const char *TAG = "pageros";

static void kbd_log_task(void *arg)
{
    (void)arg;
    QueueHandle_t q = pageros_kbd_queue();
    if (!q) vTaskDelete(NULL);
    while (1) {
        pageros_kbd_event_t e;
        if (xQueueReceive(q, &e, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI("kbd", "event: row=%u col=%u %s",
                     e.row, e.col, e.pressed ? "DOWN" : "UP");
        }
    }
}

static const char *input_event_name(pageros_input_event_t e)
{
    switch (e) {
        case PAGEROS_INPUT_ROTARY_CW:  return "ROTARY_CW";
        case PAGEROS_INPUT_ROTARY_CCW: return "ROTARY_CCW";
        case PAGEROS_INPUT_ENTER:      return "ENTER";
        case PAGEROS_INPUT_BACK:       return "BACK";
        case PAGEROS_INPUT_BACK_LONG:  return "BACK_LONG";
        default:                       return "?";
    }
}

static void input_log_task(void *arg)
{
    (void)arg;
    while (1) {
        pageros_input_event_t e = pageros_input_wait(portMAX_DELAY);
        if (e != PAGEROS_INPUT_NONE) {
            ESP_LOGI("input", "event: %s", input_event_name(e));
        }
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

    // Input bring-up (FW-007). Spawns a tiny logger task so the user can
    // see encoder + ENTER + BACK events on the USB monitor; the real
    // input router (FW-024) will replace this with proper dispatch.
    esp_err_t input_err = pageros_input_init();
    if (input_err == ESP_OK) {
        xTaskCreate(input_log_task, "input_log", 2048, NULL, 4, NULL);
    } else {
        ESP_LOGW(TAG, "input init skipped: %s", esp_err_to_name(input_err));
    }

    // Keyboard bring-up (FW-006). Same shape as input: log raw matrix
    // events so the operator can see key presses on the USB monitor.
    // Keymap translation lives at the shell layer.
    esp_err_t kbd_err = pageros_kbd_init();
    if (kbd_err == ESP_OK) {
        xTaskCreate(kbd_log_task, "kbd_log", 2048, NULL, 4, NULL);
    } else {
        ESP_LOGW(TAG, "kbd init skipped: %s", esp_err_to_name(kbd_err));
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
