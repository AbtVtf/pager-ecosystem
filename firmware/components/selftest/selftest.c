// PagerOS bootloader self-test (FW-002) — see selftest.h.
//
// Acceptance (TASKS.md FW-002):
//   "On boot, validates flash + PSRAM + display + radios; logs result;
//    halts on hard fail."
//
// Only flash + PSRAM can be exercised directly today. Display (FW-005),
// LoRa (FW-008), Wi-Fi (FW-009), and NFC (FW-010) drivers are not yet
// in the tree, so those probes are wired as DEFERRED placeholders that
// later tasks will fill in via the weak override hooks below.

#include "selftest.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "selftest";

// Expected flash size in bytes (matches CONFIG_ESPTOOLPY_FLASHSIZE_16MB).
#define EXPECTED_FLASH_BYTES (16U * 1024U * 1024U)

// Minimum acceptable PSRAM size (LILYGO T-LoRa Pager ships 8 MB octal PSRAM).
#define MIN_PSRAM_BYTES (8U * 1024U * 1024U)

// PSRAM read/write self-test buffer size — large enough to cross several
// cache lines, small enough to run quickly during boot.
#define PSRAM_TEST_BYTES (64U * 1024U)

// Static detail strings live in BSS so we can hand pointers back to the
// caller without dynamic allocation during early boot.
static char s_flash_detail[64];
static char s_psram_detail[64];
static char s_display_detail[64];
static char s_radios_detail[64];

const char *selftest_subsys_name(selftest_subsys_t s)
{
    switch (s) {
    case SELFTEST_SUBSYS_FLASH:   return "flash";
    case SELFTEST_SUBSYS_PSRAM:   return "psram";
    case SELFTEST_SUBSYS_DISPLAY: return "display";
    case SELFTEST_SUBSYS_RADIOS:  return "radios";
    default:                      return "?";
    }
}

const char *selftest_status_name(selftest_status_t s)
{
    switch (s) {
    case SELFTEST_PASS:      return "PASS";
    case SELFTEST_FAIL_HARD: return "FAIL_HARD";
    case SELFTEST_FAIL_SOFT: return "FAIL_SOFT";
    case SELFTEST_DEFERRED:  return "DEFERRED";
    default:                 return "?";
    }
}

// ---------------------------------------------------------------------------
// Flash check
//
// 1. Read total flash size and verify it matches the configured 16 MB.
// 2. Walk the partition table and verify every entry can be located and
//    sits within flash bounds.
//
// A mismatched flash size or an unreadable partition table is a hard fail:
// the rest of the OS depends on the layout in partitions.csv.
// ---------------------------------------------------------------------------
static selftest_entry_t check_flash(void)
{
    selftest_entry_t e = {.subsys = SELFTEST_SUBSYS_FLASH};

    uint32_t size = 0;
    esp_err_t err = esp_flash_get_size(NULL, &size);
    if (err != ESP_OK) {
        snprintf(s_flash_detail, sizeof(s_flash_detail),
                 "esp_flash_get_size failed: %s", esp_err_to_name(err));
        e.status = SELFTEST_FAIL_HARD;
        e.detail = s_flash_detail;
        return e;
    }

    if (size != EXPECTED_FLASH_BYTES) {
        snprintf(s_flash_detail, sizeof(s_flash_detail),
                 "flash size %lu B, expected %u B",
                 (unsigned long)size, EXPECTED_FLASH_BYTES);
        e.status = SELFTEST_FAIL_HARD;
        e.detail = s_flash_detail;
        return e;
    }

    int parts = 0;
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if (!p) continue;
        if ((uint64_t)p->address + p->size > EXPECTED_FLASH_BYTES) {
            esp_partition_iterator_release(it);
            snprintf(s_flash_detail, sizeof(s_flash_detail),
                     "partition %s overflows flash", p->label);
            e.status = SELFTEST_FAIL_HARD;
            e.detail = s_flash_detail;
            return e;
        }
        parts++;
    }
    esp_partition_iterator_release(it);

    if (parts == 0) {
        snprintf(s_flash_detail, sizeof(s_flash_detail),
                 "no partitions found");
        e.status = SELFTEST_FAIL_HARD;
        e.detail = s_flash_detail;
        return e;
    }

    snprintf(s_flash_detail, sizeof(s_flash_detail),
             "%lu MB, %d partitions",
             (unsigned long)(size / (1024 * 1024)), parts);
    e.status = SELFTEST_PASS;
    e.detail = s_flash_detail;
    return e;
}

// ---------------------------------------------------------------------------
// PSRAM check
//
// 1. Confirm PSRAM is initialized and reports at least 8 MB.
// 2. Allocate a 64 KB buffer in PSRAM, write a deterministic pattern, read
//    it back, and confirm bytes round-trip cleanly.
//
// A PSRAM failure is hard: the Frame cache, image decoder, and font glyph
// LRU (SPEC §4 / §7) all require PSRAM to function. The OS is unusable
// without it.
// ---------------------------------------------------------------------------
static selftest_entry_t check_psram(void)
{
    selftest_entry_t e = {.subsys = SELFTEST_SUBSYS_PSRAM};

    if (!esp_psram_is_initialized()) {
        snprintf(s_psram_detail, sizeof(s_psram_detail),
                 "psram not initialized");
        e.status = SELFTEST_FAIL_HARD;
        e.detail = s_psram_detail;
        return e;
    }

    size_t psize = esp_psram_get_size();
    if (psize < MIN_PSRAM_BYTES) {
        snprintf(s_psram_detail, sizeof(s_psram_detail),
                 "psram %u B < required %u B",
                 (unsigned)psize, MIN_PSRAM_BYTES);
        e.status = SELFTEST_FAIL_HARD;
        e.detail = s_psram_detail;
        return e;
    }

    uint8_t *buf = heap_caps_malloc(PSRAM_TEST_BYTES, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        snprintf(s_psram_detail, sizeof(s_psram_detail),
                 "psram alloc %u B failed", PSRAM_TEST_BYTES);
        e.status = SELFTEST_FAIL_HARD;
        e.detail = s_psram_detail;
        return e;
    }

    // Deterministic but non-trivial pattern — catches stuck bits and aliasing.
    for (size_t i = 0; i < PSRAM_TEST_BYTES; i++) {
        buf[i] = (uint8_t)((i * 0x9Du) ^ (i >> 8));
    }
    bool ok = true;
    for (size_t i = 0; i < PSRAM_TEST_BYTES; i++) {
        uint8_t want = (uint8_t)((i * 0x9Du) ^ (i >> 8));
        if (buf[i] != want) {
            ok = false;
            break;
        }
    }
    heap_caps_free(buf);

    if (!ok) {
        snprintf(s_psram_detail, sizeof(s_psram_detail),
                 "psram read-back mismatch");
        e.status = SELFTEST_FAIL_HARD;
        e.detail = s_psram_detail;
        return e;
    }

    snprintf(s_psram_detail, sizeof(s_psram_detail),
             "%u MB, %u B r/w ok",
             (unsigned)(psize / (1024 * 1024)), PSRAM_TEST_BYTES);
    e.status = SELFTEST_PASS;
    e.detail = s_psram_detail;
    return e;
}

// ---------------------------------------------------------------------------
// Display + radios — deferred probes.
//
// FW-005 (display) and FW-008/009/010 (radios) will replace these weak
// implementations with real bus-level checks. Until then we report
// DEFERRED so the structure of the self-test framework is exercised and
// logged on every boot.
//
// Drivers that land later should provide non-weak overrides of these
// symbols returning PASS / FAIL_HARD as appropriate.
// ---------------------------------------------------------------------------
__attribute__((weak))
selftest_entry_t selftest_probe_display(void)
{
    selftest_entry_t e = {.subsys = SELFTEST_SUBSYS_DISPLAY};
    snprintf(s_display_detail, sizeof(s_display_detail),
             "deferred — display driver (FW-005) not yet wired");
    e.status = SELFTEST_DEFERRED;
    e.detail = s_display_detail;
    return e;
}

__attribute__((weak))
selftest_entry_t selftest_probe_radios(void)
{
    selftest_entry_t e = {.subsys = SELFTEST_SUBSYS_RADIOS};
    snprintf(s_radios_detail, sizeof(s_radios_detail),
             "deferred — LoRa/Wi-Fi/NFC drivers (FW-008..010) pending");
    e.status = SELFTEST_DEFERRED;
    e.detail = s_radios_detail;
    return e;
}

selftest_result_t selftest_run(void)
{
    selftest_result_t r = {0};
    r.entries[SELFTEST_SUBSYS_FLASH]   = check_flash();
    r.entries[SELFTEST_SUBSYS_PSRAM]   = check_psram();
    r.entries[SELFTEST_SUBSYS_DISPLAY] = selftest_probe_display();
    r.entries[SELFTEST_SUBSYS_RADIOS]  = selftest_probe_radios();

    for (int i = 0; i < SELFTEST_SUBSYS_COUNT; i++) {
        if (r.entries[i].status == SELFTEST_FAIL_HARD) {
            r.any_hard_fail = true;
            break;
        }
    }
    return r;
}

void selftest_log_result(const selftest_result_t *r)
{
    if (r == NULL) return;
    ESP_LOGI(TAG, "self-test summary:");
    for (int i = 0; i < SELFTEST_SUBSYS_COUNT; i++) {
        const selftest_entry_t *e = &r->entries[i];
        const char *name   = selftest_subsys_name(e->subsys);
        const char *status = selftest_status_name(e->status);
        const char *detail = e->detail ? e->detail : "";
        switch (e->status) {
        case SELFTEST_PASS:
            ESP_LOGI(TAG, "  [%-8s] %-8s %s", name, status, detail);
            break;
        case SELFTEST_DEFERRED:
            ESP_LOGI(TAG, "  [%-8s] %-8s %s", name, status, detail);
            break;
        case SELFTEST_FAIL_SOFT:
            ESP_LOGW(TAG, "  [%-8s] %-8s %s", name, status, detail);
            break;
        case SELFTEST_FAIL_HARD:
            ESP_LOGE(TAG, "  [%-8s] %-8s %s", name, status, detail);
            break;
        }
    }
}

void selftest_halt_if_hard_fail(const selftest_result_t *r)
{
    if (r == NULL || !r->any_hard_fail) return;

    ESP_LOGE(TAG, "hard fail detected — halting boot per FW-002 policy");
    ESP_LOGE(TAG, "  (recovery image must be flashed via USB to recover)");
    while (1) {
        // Periodic heartbeat so a human watching the serial console knows the
        // device is wedged in the self-test halt, not actually crashed.
        ESP_LOGE(TAG, "selftest: HALT");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
