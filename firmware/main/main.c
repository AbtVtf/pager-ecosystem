// PagerOS firmware entry point.
//
// Skeleton (FW-001) + bootloader self-test (FW-002). Subsequent tasks
// (FW-003 filesystem mount, FW-005 display driver, etc.) bolt their
// initialization in here in dependency order. The self-test must run
// first per SPEC.md §7.2 boot flow.

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"

#include "pageros_apprt.h"
#include "pageros_cbor.h"
#include "pageros_display.h"
#include "pageros_fonts.h"
#include "pageros_gps.h"
#include "pageros_identity.h"
#include "pageros_imu.h"
#include "pageros_input.h"
#include "pageros_input_router.h"
#include "pageros_keyboard.h"
#include "pageros_logger.h"
#include "pageros_lora.h"
#include "pageros_network.h"
#include "pageros_shell.h"
#include "pageros_storage.h"
#include "pageros_widgets.h"
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

    // NVS must be up before identity load (FW-014). If the partition is
    // truncated or full from a prior boot we erase + re-init so first-
    // boot identity gen still succeeds — destroying existing pageros_id
    // state is the lesser evil vs. wedging the device.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES
            || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs needs erase (%s); reformatting",
                 esp_err_to_name(nvs_err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    // Identity load (FW-014). First boot generates an Ed25519 keypair;
    // subsequent boots load it from NVS. The seed is the only thing
    // stored — the pubkey is derived each boot via FW-015 crypto.
    esp_err_t id_err = pageros_identity_init();
    if (id_err != ESP_OK) {
        ESP_LOGE(TAG, "identity init failed: %s — continuing without "
                      "stable identity", esp_err_to_name(id_err));
    }

    // Storage mount (FW-003). Creates the §7.4 directory tree on first
    // boot. Soft-fails — the shell renders a recovery screen when no
    // card is present (SPEC §7.2 step 4 + docs/user/troubleshooting),
    // and that's a shell-level concern, not the storage driver's.
    esp_err_t sd_err = pageros_storage_init();
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s — proceeding without SD",
                 esp_err_to_name(sd_err));
    }

    // Logger (FW-004). Initialise after storage so the first LOG_INFO
    // line lands on /sd/logs/pageros.log; falls back to console-only
    // when the SD mount above failed. Any earlier code paths that need
    // a durable line can also call pageros_logger_init() again later —
    // it's idempotent and will pick up SD the moment it appears.
    esp_err_t log_err = pageros_logger_init();
    if (log_err != ESP_OK) {
        ESP_LOGW(TAG, "logger init failed: %s", esp_err_to_name(log_err));
    } else {
        LOG_INFO(TAG, "logger online, sd=%s",
                 pageros_logger_is_writing_to_sd() ? "yes" : "no");
    }

    // LoRa bring-up (FW-008). 868 MHz / SF7 / 125 kHz is a quiet
    // boot-time default — actual channel choice belongs to the
    // higher-level mesh stack (LORA-* tasks). Soft-fails so a missing
    // SX1262 (e.g. dev board with the LoRa power gate off) doesn't
    // block the rest of the system from booting.
    pageros_lora_config_t lora_cfg = {
        .band             = PAGEROS_LORA_BAND_868_MHZ,
        .bandwidth        = PAGEROS_LORA_BW_125_KHZ,
        .spreading_factor = 7,
        .coding_rate      = PAGEROS_LORA_CR_4_5,
        .tx_power_dbm     = 14,
        .preamble_symbols = 8,
        .explicit_header  = true,
        .crc_on           = true,
        .iq_inverted      = false,
    };
    esp_err_t lora_err = pageros_lora_init(&lora_cfg);
    if (lora_err == ESP_OK) {
        LOG_INFO(TAG, "LoRa ready (SX1262, 868 MHz, SF7/BW125)");
    } else {
        LOG_ERROR(TAG, "LoRa init failed: %s", esp_err_to_name(lora_err));
    }

    // Wi-Fi station bring-up (FW-009). We init the radio so the HTTPS
    // client and the shell's settings flow can both reach the API; the
    // actual `pageros_wifi_connect(ssid, pwd, ...)` call is owned by
    // the shell once it has credentials in NVS — there's no SSID source
    // here yet (FW-029 owns the Wi-Fi config UX).
    esp_err_t wifi_err = pageros_wifi_init();
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "wifi init failed: %s", esp_err_to_name(wifi_err));
    }

    // Display bring-up (FW-005).
    esp_err_t disp_err = pageros_display_init();
    if (disp_err == ESP_OK) {
        // Paint a solid background first so the panel is fully cleared
        // before the renderer takes over — otherwise stale PSRAM bytes
        // bleed through any region the renderer doesn't touch.
        pageros_display_fill(pageros_display_rgb565(0x00, 0x00, 0x00));
        pageros_display_present();
    } else {
        ESP_LOGW(TAG, "display init failed: %s", esp_err_to_name(disp_err));
    }

    // FW-019 / FW-020 / FW-028 / FW-029 — render the Shell home Frame
    // into the display's framebuffer and present. This replaces the
    // boot-splash gradient with the actual home screen so the user sees
    // something legible at first boot.
    pageros_fonts_init(NULL);
    pageros_apprt_init(NULL);
    pageros_shell_init();
    if (disp_err == ESP_OK) {
        if (pageros_shell_mount_home() == ESP_OK) {
            pageros_widgets_palette_t pal;
            pageros_widgets_default_palette(&pal);
            pageros_widgets_ctx_t ctx = {
                .canvas  = {
                    .pixels = (uint16_t *)pageros_display_framebuffer(),
                    .w      = PAGEROS_DISPLAY_WIDTH,
                    .h      = PAGEROS_DISPLAY_HEIGHT,
                },
                .palette = pal,
                .focus   = { .widget_index = 0, .item_index = 0, .scroll = 0 },
                .title   = "PagerOS",
                .help    = "MENU = up/down  ENTER = open",
            };
            const pgr_cbor_value_t *fr = pageros_apprt_foreground_frame();
            if (fr) {
                const pgr_cbor_value_t *body = NULL;
                // Pull the "body" array out of the Frame map.
                if (fr->kind == PGR_CBOR_KIND_MAP) {
                    for (size_t i = 0; i < fr->v.map.len; i++) {
                        const pgr_cbor_pair_t *p = &fr->v.map.items[i];
                        if (p->key.kind == PGR_CBOR_KIND_TEXT &&
                            p->key.v.bytes.len == 4 &&
                            memcmp(p->key.v.bytes.data, "body", 4) == 0) {
                            body = &p->val;
                            break;
                        }
                    }
                }
                if (pageros_widgets_render_screen(&ctx, body) == ESP_OK) {
                    pageros_display_present();
                    ESP_LOGI(TAG, "rendered Shell home Frame to display");
                } else {
                    ESP_LOGW(TAG, "Shell render failed");
                }
            } else {
                ESP_LOGW(TAG, "Shell mounted but no foreground frame");
            }
        } else {
            ESP_LOGW(TAG, "Shell mount_home failed");
        }
    }

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

    // IMU stub (FW-012). Confirms the BHI260AP is present on the bus
    // so future fusion-firmware work has a real probe to start from;
    // no event dispatch in v1 per TASKS.md.
    esp_err_t imu_err = pageros_imu_init();
    if (imu_err != ESP_OK) {
        ESP_LOGW(TAG, "IMU init skipped: %s", esp_err_to_name(imu_err));
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
