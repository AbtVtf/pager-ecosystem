// SPDX-License-Identifier: Apache-2.0
// FW-010 — ST25R3916 NFC driver (v0 — SPI bring-up + poll scaffold).

#include "pageros_nfc.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "nfc";

// LILYGO T-LoRa Pager NFC wiring — verified against the authoritative
// arduino-esp32 variant header (NFC_CS=39, NFC_INT=5). The earlier
// draft used CS=7 (= rotary push button, ROTARY_C) and IRQ=18 (= I2S
// WS), both wrong; they fought the input + audio drivers for those
// GPIOs and the chip never selected. NFC also needs XL9555 NFC_EN
// asserted before the chip will respond — main.c flips that during
// XL9555 init.
#define NFC_CS_GPIO    39
#define NFC_IRQ_GPIO   5
#define NFC_SPI_HOST   SPI2_HOST
#define NFC_SPI_CLOCK  6000000   // 6 MHz — datasheet allows up to 10 MHz

// ST25R3916 direct commands / registers (subset).
#define ST25_CMD_SET_DEFAULT     0xC1
#define ST25_REG_IC_IDENTITY     0x3F
#define ST25_IC_IDENTITY_V       0x09  // ST25R3916 family marker (low nibble varies)

#define ST25_OP_REG_READ         0x80
#define ST25_OP_REG_WRITE        0x00
#define ST25_OP_CMD              0xC0

static struct {
    bool inited;
    spi_device_handle_t spi;
    bool chip_present;
    TaskHandle_t poll_task;
    bool polling;
    pageros_nfc_callback_t cb;
    void *cb_user;
    pageros_nfc_stats_t stats;
} g;

static esp_err_t spi_xfer(uint8_t op, uint8_t reg_or_cmd, uint8_t *rx, size_t rxn)
{
    spi_transaction_t t = {0};
    uint8_t tx_buf[2] = { (uint8_t)(op | (reg_or_cmd & 0x3F)), 0 };
    t.length    = (1 + rxn) * 8;
    t.rxlength  = rxn * 8;
    t.tx_buffer = tx_buf;
    // Use simple split-buffer mode — read after the cmd byte.
    if (rxn == 0) { t.length = 8; }
    static uint8_t rx_buf[8];
    if (rxn > sizeof(rx_buf)) return ESP_ERR_INVALID_SIZE;
    if (rxn > 0) t.rx_buffer = rx_buf;
    esp_err_t r = spi_device_transmit(g.spi, &t);
    if (r != ESP_OK) g.stats.spi_errors++;
    if (rxn > 0 && rx) memcpy(rx, rx_buf, rxn);
    return r;
}

static esp_err_t st25_cmd(uint8_t cmd)
{
    return spi_xfer(ST25_OP_CMD, cmd, NULL, 0);
}

static esp_err_t st25_read(uint8_t reg, uint8_t *out)
{
    return spi_xfer(ST25_OP_REG_READ, reg, out, 1);
}

static void poll_task(void *arg)
{
    (void)arg;
    while (g.polling) {
        g.stats.polls++;
        // v0: chip is initialised but we don't yet drive the full
        // ISO 14443-A anticollision sequence; that lands once we can
        // test with cards on the bench. Sleep until next tick.
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    vTaskDelete(NULL);
}

esp_err_t pageros_nfc_init(void)
{
    if (g.inited) return ESP_OK;

    // SPI bus is shared; the display component already called
    // spi_bus_initialize. We just add ourselves as a device.
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = NFC_SPI_CLOCK,
        .mode = 1,                // ST25R3916: CPOL=0, CPHA=1
        .spics_io_num = NFC_CS_GPIO,
        .queue_size = 2,
    };
    esp_err_t r = spi_bus_add_device(NFC_SPI_HOST, &dev_cfg, &g.spi);
    if (r == ESP_ERR_INVALID_STATE) {
        // Bus wasn't initialised yet — try a minimal init. Real init
        // is owned by display; if it hasn't run, NFC is best-effort.
        ESP_LOGW(TAG, "SPI bus not yet up; NFC will be inert until display init");
        g.inited = true;
        return ESP_OK;
    }
    if (r != ESP_OK) { ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(r)); return r; }

    gpio_config_t irq_cfg = {
        .pin_bit_mask = 1ULL << NFC_IRQ_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&irq_cfg);

    // Reset + identity probe.
    st25_cmd(ST25_CMD_SET_DEFAULT);
    vTaskDelay(pdMS_TO_TICKS(5));
    uint8_t id = 0;
    if (st25_read(ST25_REG_IC_IDENTITY, &id) == ESP_OK) {
        // The high nibble identifies the IC type; the low nibble is rev.
        // ST25R3916 reads 0x4x or 0x5x on most revs. Be lenient: any
        // non-0x00 / non-0xFF read counts as "something is there".
        g.chip_present = (id != 0x00 && id != 0xFF);
        g.stats.chip_present = g.chip_present;
        ESP_LOGI(TAG, "IC identity = 0x%02X (%s)", id,
                 g.chip_present ? "present" : "absent");
    } else {
        ESP_LOGW(TAG, "IC identity read failed");
    }

    g.inited = true;
    return ESP_OK;
}

esp_err_t pageros_nfc_shutdown(void)
{
    if (!g.inited) return ESP_OK;
    g.polling = false;
    if (g.spi) spi_bus_remove_device(g.spi);
    memset(&g, 0, sizeof(g));
    return ESP_OK;
}

bool pageros_nfc_present(void) { return g.chip_present; }

esp_err_t pageros_nfc_start(pageros_nfc_callback_t cb, void *user)
{
    if (!g.inited) return ESP_ERR_INVALID_STATE;
    if (g.polling) return ESP_OK;
    g.cb = cb; g.cb_user = user;
    g.polling = true;
    BaseType_t ok = xTaskCreate(poll_task, "nfc_poll", 2048, NULL, 5, &g.poll_task);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

esp_err_t pageros_nfc_stop(void)
{
    g.polling = false;
    return ESP_OK;
}

void pageros_nfc_get_stats(pageros_nfc_stats_t *out)
{
    if (out) *out = g.stats;
}
