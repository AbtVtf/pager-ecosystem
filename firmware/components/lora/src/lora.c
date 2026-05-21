// SPDX-License-Identifier: Apache-2.0
//
// PagerOS LoRa wrapper — Semtech SX1262 driver. See `pageros_lora.h`.

#include "pageros_lora.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lora_params.h"

static const char *TAG = "lora";

// ---------------------------------------------------------------------------
// Board wiring — matches arduino-esp32 variant `lilygo_tlora_pager`.
// Shared SPI bus pins re-declared (and not pulled from storage.c) so
// the lora component stays buildable without dragging in storage's
// header.
// ---------------------------------------------------------------------------

#define LORA_PIN_MOSI       34
#define LORA_PIN_MISO       33
#define LORA_PIN_SCK        35
#define LORA_PIN_CS         36
#define LORA_PIN_RST        47
#define LORA_PIN_BUSY       48
#define LORA_PIN_DIO1       14
#define LORA_SPI_HOST       SPI2_HOST
#define LORA_SPI_FREQ_HZ    8000000   // SX1262 max is 16 MHz; 8 leaves margin

// XL9555 — same address as storage's instance. P0.3 = LoRa power.
#define XL_I2C_PORT         I2C_NUM_0
#define XL_I2C_SDA_GPIO     3
#define XL_I2C_SCL_GPIO     2
#define XL_I2C_FREQ_HZ      400000
#define XL_I2C_ADDR         0x20
#define XL_REG_OUTPUT_0     0x02
#define XL_REG_CONFIG_0     0x06
#define XL_BIT_LORA_EN      (1u << 3)  // EXPANDS_LORA_EN = 3 → P0.3

// ---------------------------------------------------------------------------
// SX1262 opcodes (datasheet §13)
// ---------------------------------------------------------------------------

#define OP_SET_SLEEP             0x84
#define OP_SET_STANDBY           0x80
#define OP_SET_RX                0x82
#define OP_SET_TX                0x83
#define OP_SET_PACKET_TYPE       0x8A
#define OP_SET_RF_FREQUENCY      0x86
#define OP_SET_PA_CONFIG         0x95
#define OP_SET_TX_PARAMS         0x8E
#define OP_SET_BUFFER_BASE_ADDR  0x8F
#define OP_SET_MODULATION_PARAMS 0x8B
#define OP_SET_PACKET_PARAMS     0x8C
#define OP_SET_DIO_IRQ_PARAMS    0x08
#define OP_WRITE_REGISTER        0x0D
#define OP_READ_REGISTER         0x1D
#define OP_WRITE_BUFFER          0x0E
#define OP_READ_BUFFER           0x1E
#define OP_CLEAR_IRQ_STATUS      0x02
#define OP_GET_IRQ_STATUS        0x12
#define OP_GET_RX_BUFFER_STATUS  0x13
#define OP_GET_PACKET_STATUS     0x14
#define OP_GET_STATUS            0xC0
#define OP_SET_REGULATOR_MODE    0x96

#define STANDBY_RC               0x00
#define STANDBY_XOSC             0x01
#define PACKET_TYPE_LORA         0x01
#define REG_LORA_SYNCWORD_MSB    0x0740
#define REG_LORA_SYNCWORD_LSB    0x0741
#define LORA_SYNC_PRIVATE        0x1424   // value at 0x0740..0x0741
#define IRQ_TX_DONE              (1u << 0)
#define IRQ_RX_DONE              (1u << 1)
#define IRQ_HEADER_ERR           (1u << 5)
#define IRQ_CRC_ERR              (1u << 6)
#define IRQ_TIMEOUT              (1u << 9)

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------

typedef struct {
    spi_device_handle_t   spi;
    SemaphoreHandle_t     lock;
    pageros_lora_config_t cfg;
    bool                  initialised;
    bool                  spi_bus_up;
} lora_state_t;

static lora_state_t s_state = {0};

// ---------------------------------------------------------------------------
// XL9555 helpers
// ---------------------------------------------------------------------------

static esp_err_t xl_i2c_init_once(void)
{
    static bool i2c_up = false;
    if (i2c_up) return ESP_OK;
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = XL_I2C_SDA_GPIO,
        .scl_io_num       = XL_I2C_SCL_GPIO,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = XL_I2C_FREQ_HZ,
    };
    // Other components share this port: install first, then param-config;
    // accept INVALID_STATE / FAIL on subsequent installs (matches the
    // pattern recorded in `esp_idf_shared_i2c.md`).
    esp_err_t err = i2c_driver_install(XL_I2C_PORT, cfg.mode, 0, 0, 0);
    if (err == ESP_OK) {
        err = i2c_param_config(XL_I2C_PORT, &cfg);
        if (err != ESP_OK) return err;
    } else if (err != ESP_ERR_INVALID_STATE && err != ESP_FAIL) {
        return err;
    }
    i2c_up = true;
    return ESP_OK;
}

static esp_err_t xl_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_write_read_device(XL_I2C_PORT, XL_I2C_ADDR,
                                        &reg, 1, value, 1,
                                        pdMS_TO_TICKS(50));
}

static esp_err_t xl_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_write_to_device(XL_I2C_PORT, XL_I2C_ADDR,
                                      buf, 2, pdMS_TO_TICKS(50));
}

static esp_err_t xl_set_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t cur = 0;
    esp_err_t err = xl_read(reg, &cur);
    if (err != ESP_OK) return err;
    cur = (uint8_t)((cur & ~mask) | (value & mask));
    return xl_write(reg, cur);
}

static esp_err_t xl_power_lora(bool on)
{
    esp_err_t err = xl_set_bits(XL_REG_CONFIG_0, XL_BIT_LORA_EN, 0);  // output
    if (err != ESP_OK) return err;
    return xl_set_bits(XL_REG_OUTPUT_0, XL_BIT_LORA_EN,
                       on ? XL_BIT_LORA_EN : 0);
}

// ---------------------------------------------------------------------------
// SPI / BUSY plumbing
// ---------------------------------------------------------------------------

static esp_err_t spi_bus_up_once(void)
{
    if (s_state.spi_bus_up) return ESP_OK;
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = LORA_PIN_MOSI,
        .miso_io_num     = LORA_PIN_MISO,
        .sclk_io_num     = LORA_PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(LORA_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        // INVALID_STATE = bus already brought up by storage/display; the
        // pin map is the same so reuse is correct, not a conflict.
        s_state.spi_bus_up = true;
        return ESP_OK;
    }
    return err;
}

static esp_err_t wait_busy_low(uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (gpio_get_level(LORA_PIN_BUSY) == 1) {
        if (esp_timer_get_time() > deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
    return ESP_OK;
}

static esp_err_t sx_cmd(uint8_t opcode, const uint8_t *params, size_t plen,
                        uint8_t *rx, size_t rxlen)
{
    if (wait_busy_low(20) != ESP_OK) {
        ESP_LOGW(TAG, "busy stuck high before opcode 0x%02x", opcode);
    }
    // We split a small "send opcode + params" then "read response into
    // rx" into two transactions because the SX1262 takes the opcode on
    // the first byte and clocks back status starting at byte 0 of the
    // next call (datasheet §13.5). For commands without a read phase
    // (the common case) the second xfer is skipped.
    uint8_t tx[64];
    if (plen + 1 > sizeof(tx)) return ESP_ERR_INVALID_SIZE;
    tx[0] = opcode;
    if (plen) memcpy(&tx[1], params, plen);

    spi_transaction_t t = {0};
    t.length = (plen + 1) * 8;
    t.tx_buffer = tx;
    if (rxlen == 0) {
        esp_err_t err = spi_device_polling_transmit(s_state.spi, &t);
        if (err != ESP_OK) return err;
        return wait_busy_low(20);
    }

    // Read path: opcode + NOP for status byte + zeros for rxlen reads.
    uint8_t rxbuf[64];
    uint8_t txbuf[64];
    size_t total = 1 + rxlen + 1;  // opcode + NOP(status) + rxlen data bytes
    if (total > sizeof(rxbuf)) return ESP_ERR_INVALID_SIZE;
    memset(txbuf, 0, total);
    txbuf[0] = opcode;
    if (plen > 0 && plen <= rxlen) {
        // (params write before read — used by ReadRegister / ReadBuffer)
        memcpy(&txbuf[1], params, plen);
    }
    spi_transaction_t r = {0};
    r.length = total * 8;
    r.tx_buffer = txbuf;
    r.rx_buffer = rxbuf;
    esp_err_t err = spi_device_polling_transmit(s_state.spi, &r);
    if (err != ESP_OK) return err;
    // First returned byte is the status (datasheet §13.5.1); subsequent
    // bytes are the requested payload.
    memcpy(rx, &rxbuf[2], rxlen);
    return wait_busy_low(20);
}

static esp_err_t sx_write_buffer(uint8_t offset, const uint8_t *data, size_t len)
{
    if (len > PAGEROS_LORA_MAX_PACKET) return ESP_ERR_INVALID_ARG;
    uint8_t tx[2 + PAGEROS_LORA_MAX_PACKET];
    tx[0] = OP_WRITE_BUFFER;
    tx[1] = offset;
    memcpy(&tx[2], data, len);
    if (wait_busy_low(20) != ESP_OK) return ESP_ERR_TIMEOUT;
    spi_transaction_t t = {
        .length    = (2 + len) * 8,
        .tx_buffer = tx,
    };
    esp_err_t err = spi_device_polling_transmit(s_state.spi, &t);
    if (err != ESP_OK) return err;
    return wait_busy_low(20);
}

static esp_err_t sx_read_buffer(uint8_t offset, uint8_t *data, size_t len)
{
    if (len > PAGEROS_LORA_MAX_PACKET) return ESP_ERR_INVALID_ARG;
    uint8_t tx[3 + PAGEROS_LORA_MAX_PACKET];
    uint8_t rx[3 + PAGEROS_LORA_MAX_PACKET];
    memset(tx, 0, sizeof(tx));
    tx[0] = OP_READ_BUFFER;
    tx[1] = offset;
    // tx[2] = NOP (status byte returned here)
    if (wait_busy_low(20) != ESP_OK) return ESP_ERR_TIMEOUT;
    spi_transaction_t t = {
        .length    = (3 + len) * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_polling_transmit(s_state.spi, &t);
    if (err != ESP_OK) return err;
    memcpy(data, &rx[3], len);
    return wait_busy_low(20);
}

static esp_err_t sx_write_register(uint16_t addr, const uint8_t *data, size_t len)
{
    uint8_t buf[3 + 16];
    if (len > sizeof(buf) - 3) return ESP_ERR_INVALID_SIZE;
    buf[0] = (uint8_t)((addr >> 8) & 0xFF);
    buf[1] = (uint8_t)(addr & 0xFF);
    memcpy(&buf[2], data, len);
    return sx_cmd(OP_WRITE_REGISTER, buf, 2 + len, NULL, 0);
}

// ---------------------------------------------------------------------------
// High-level configuration
// ---------------------------------------------------------------------------

static esp_err_t hw_reset(void)
{
    gpio_set_level(LORA_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(LORA_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    return wait_busy_low(50);
}

static esp_err_t apply_frequency(pageros_lora_band_t band)
{
    uint32_t hz = pageros_lora_band_to_hz(band);
    if (hz == 0) return ESP_ERR_INVALID_ARG;
    uint32_t pll = pageros_lora_freq_to_pll(hz);
    uint8_t p[4] = {
        (uint8_t)((pll >> 24) & 0xFF),
        (uint8_t)((pll >> 16) & 0xFF),
        (uint8_t)((pll >> 8) & 0xFF),
        (uint8_t)(pll & 0xFF),
    };
    return sx_cmd(OP_SET_RF_FREQUENCY, p, 4, NULL, 0);
}

static esp_err_t apply_modulation(pageros_lora_bw_t bw, uint8_t sf,
                                  pageros_lora_cr_t cr)
{
    uint8_t bw_reg = pageros_lora_bw_to_reg(bw);
    if (bw_reg == 0xFF) return ESP_ERR_INVALID_ARG;
    if (sf < PAGEROS_LORA_SF_MIN || sf > PAGEROS_LORA_SF_MAX)
        return ESP_ERR_INVALID_ARG;

    // LowDataRateOptimize on (SF11/SF12 at 125 kHz, SF12 at 250 kHz)
    // — datasheet §6.1.1.4.
    uint8_t ldro = 0;
    if ((sf >= 11 && bw == PAGEROS_LORA_BW_125_KHZ)
            || (sf == 12 && bw == PAGEROS_LORA_BW_250_KHZ)) {
        ldro = 1;
    }
    uint8_t p[4] = { sf, bw_reg, (uint8_t)cr, ldro };
    return sx_cmd(OP_SET_MODULATION_PARAMS, p, 4, NULL, 0);
}

static esp_err_t apply_packet_params(uint8_t payload_len)
{
    uint16_t preamble = s_state.cfg.preamble_symbols;
    uint8_t p[6] = {
        (uint8_t)((preamble >> 8) & 0xFF),
        (uint8_t)(preamble & 0xFF),
        s_state.cfg.explicit_header ? 0x00 : 0x01,  // 0=explicit, 1=implicit
        payload_len,
        s_state.cfg.crc_on ? 0x01 : 0x00,
        s_state.cfg.iq_inverted ? 0x01 : 0x00,
    };
    return sx_cmd(OP_SET_PACKET_PARAMS, p, 6, NULL, 0);
}

static esp_err_t apply_tx_power(int8_t dbm)
{
    dbm = pageros_lora_clamp_power(dbm);
    // SX1262 +22 dBm PA config (datasheet §13.1.14).
    uint8_t pa[4] = { 0x04, 0x07, 0x00, 0x01 };
    esp_err_t err = sx_cmd(OP_SET_PA_CONFIG, pa, 4, NULL, 0);
    if (err != ESP_OK) return err;
    uint8_t tx[2] = { (uint8_t)dbm, 0x04 };  // ramp time = 200us
    return sx_cmd(OP_SET_TX_PARAMS, tx, 2, NULL, 0);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t pageros_lora_init(const pageros_lora_config_t *cfg)
{
    if (!pageros_lora_config_is_valid(cfg)) return ESP_ERR_INVALID_ARG;

    if (s_state.initialised) return ESP_OK;

    s_state.lock = xSemaphoreCreateMutex();
    if (s_state.lock == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err = xl_i2c_init_once();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c init: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }
    err = xl_power_lora(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "xl power-on: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    err = spi_bus_up_once();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi bus: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << LORA_PIN_RST),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << LORA_PIN_BUSY) | (1ULL << LORA_PIN_DIO1),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&in);

    spi_device_interface_config_t dev_cfg = {
        .command_bits    = 0,
        .address_bits    = 0,
        .mode            = 0,
        .clock_speed_hz  = LORA_SPI_FREQ_HZ,
        .spics_io_num    = LORA_PIN_CS,
        .queue_size      = 1,
    };
    err = spi_bus_add_device(LORA_SPI_HOST, &dev_cfg, &s_state.spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi add device: %s", esp_err_to_name(err));
        return err;
    }

    err = hw_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reset / busy: %s", esp_err_to_name(err));
        return err;
    }

    // Probe before doing any configuration — a status byte of 0x00 or
    // 0xFF means the chip isn't on the bus (typically a power-gate or
    // wiring fault). Bail early with a clear error.
    if (!pageros_lora_probe()) {
        ESP_LOGE(TAG, "SX1262 not detected on SPI%d", LORA_SPI_HOST);
        spi_bus_remove_device(s_state.spi);
        s_state.spi = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t reg = 0x01;  // DC-DC + LDO
    err = sx_cmd(OP_SET_REGULATOR_MODE, &reg, 1, NULL, 0);
    if (err != ESP_OK) return err;
    uint8_t standby = STANDBY_RC;
    err = sx_cmd(OP_SET_STANDBY, &standby, 1, NULL, 0);
    if (err != ESP_OK) return err;
    uint8_t ptype = PACKET_TYPE_LORA;
    err = sx_cmd(OP_SET_PACKET_TYPE, &ptype, 1, NULL, 0);
    if (err != ESP_OK) return err;

    s_state.cfg = *cfg;

    err = apply_frequency(cfg->band);
    if (err != ESP_OK) return err;
    err = apply_modulation(cfg->bandwidth, cfg->spreading_factor, cfg->coding_rate);
    if (err != ESP_OK) return err;
    err = apply_packet_params(0);
    if (err != ESP_OK) return err;
    err = apply_tx_power(cfg->tx_power_dbm);
    if (err != ESP_OK) return err;

    // Public sync word, per Semtech AN1200.22.
    uint8_t sync[2] = {
        (uint8_t)((LORA_SYNC_PRIVATE >> 8) & 0xFF),
        (uint8_t)(LORA_SYNC_PRIVATE & 0xFF),
    };
    err = sx_write_register(REG_LORA_SYNCWORD_MSB, sync, 2);
    if (err != ESP_OK) return err;

    uint8_t base[2] = { 0x00, 0x00 };
    err = sx_cmd(OP_SET_BUFFER_BASE_ADDR, base, 2, NULL, 0);
    if (err != ESP_OK) return err;

    // IRQ mask: TX done, RX done, timeout. Route them all to DIO1 so
    // the GPIO line can be polled by the TX/RX paths.
    uint16_t mask = IRQ_TX_DONE | IRQ_RX_DONE | IRQ_TIMEOUT
                  | IRQ_HEADER_ERR | IRQ_CRC_ERR;
    uint8_t irq[8] = {
        (uint8_t)((mask >> 8) & 0xFF), (uint8_t)(mask & 0xFF),
        (uint8_t)((mask >> 8) & 0xFF), (uint8_t)(mask & 0xFF),
        0, 0,
        0, 0,
    };
    err = sx_cmd(OP_SET_DIO_IRQ_PARAMS, irq, 8, NULL, 0);
    if (err != ESP_OK) return err;

    s_state.initialised = true;
    ESP_LOGI(TAG, "SX1262 ready, band=%s SF%u BW=%u kHz",
             cfg->band == PAGEROS_LORA_BAND_868_MHZ ? "868" : "915",
             cfg->spreading_factor,
             cfg->bandwidth == PAGEROS_LORA_BW_125_KHZ ? 125 :
             cfg->bandwidth == PAGEROS_LORA_BW_250_KHZ ? 250 : 500);
    return ESP_OK;
}

esp_err_t pageros_lora_shutdown(void)
{
    if (!s_state.initialised) return ESP_OK;

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    if (s_state.spi != NULL) {
        uint8_t sleep[1] = { 0x00 };
        (void)sx_cmd(OP_SET_SLEEP, sleep, 1, NULL, 0);
        spi_bus_remove_device(s_state.spi);
        s_state.spi = NULL;
    }
    (void)xl_power_lora(false);
    s_state.initialised = false;
    xSemaphoreGive(s_state.lock);
    return ESP_OK;
}

bool pageros_lora_is_ready(void)
{
    return s_state.initialised;
}

esp_err_t pageros_lora_set_band(pageros_lora_band_t band)
{
    if (!s_state.initialised) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    uint8_t standby = STANDBY_RC;
    esp_err_t err = sx_cmd(OP_SET_STANDBY, &standby, 1, NULL, 0);
    if (err == ESP_OK) {
        err = apply_frequency(band);
        if (err == ESP_OK) s_state.cfg.band = band;
    }
    xSemaphoreGive(s_state.lock);
    return err;
}

esp_err_t pageros_lora_set_modulation(pageros_lora_bw_t bw, uint8_t sf,
                                      pageros_lora_cr_t cr)
{
    if (!s_state.initialised) return ESP_ERR_INVALID_STATE;
    if (sf < PAGEROS_LORA_SF_MIN || sf > PAGEROS_LORA_SF_MAX
            || pageros_lora_bw_to_reg(bw) == 0xFF) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    esp_err_t err = apply_modulation(bw, sf, cr);
    if (err == ESP_OK) {
        s_state.cfg.bandwidth = bw;
        s_state.cfg.spreading_factor = sf;
        s_state.cfg.coding_rate = cr;
    }
    xSemaphoreGive(s_state.lock);
    return err;
}

esp_err_t pageros_lora_set_tx_power(int8_t dbm)
{
    if (!s_state.initialised) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    esp_err_t err = apply_tx_power(dbm);
    if (err == ESP_OK) s_state.cfg.tx_power_dbm = pageros_lora_clamp_power(dbm);
    xSemaphoreGive(s_state.lock);
    return err;
}

esp_err_t pageros_lora_get_status(uint8_t *out_status)
{
    if (out_status == NULL) return ESP_ERR_INVALID_ARG;
    if (s_state.spi == NULL) return ESP_ERR_INVALID_STATE;
    return sx_cmd(OP_GET_STATUS, NULL, 0, out_status, 1);
}

bool pageros_lora_probe(void)
{
    uint8_t status = 0;
    if (sx_cmd(OP_GET_STATUS, NULL, 0, &status, 1) != ESP_OK) return false;
    return status != 0x00 && status != 0xFF;
}

// ---------------------------------------------------------------------------
// I/O
// ---------------------------------------------------------------------------

static esp_err_t clear_all_irqs(void)
{
    uint8_t p[2] = { 0xFF, 0xFF };
    return sx_cmd(OP_CLEAR_IRQ_STATUS, p, 2, NULL, 0);
}

static esp_err_t get_irq_status(uint16_t *out)
{
    uint8_t buf[2] = {0};
    esp_err_t err = sx_cmd(OP_GET_IRQ_STATUS, NULL, 0, buf, 2);
    if (err != ESP_OK) return err;
    *out = ((uint16_t)buf[0] << 8) | buf[1];
    return ESP_OK;
}

esp_err_t pageros_lora_tx(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (data == NULL || len == 0 || len > PAGEROS_LORA_MAX_PACKET)
        return ESP_ERR_INVALID_ARG;
    if (!s_state.initialised) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_state.lock, portMAX_DELAY);

    esp_err_t err = apply_packet_params((uint8_t)len);
    if (err != ESP_OK) goto out;
    err = sx_write_buffer(0, data, len);
    if (err != ESP_OK) goto out;
    err = clear_all_irqs();
    if (err != ESP_OK) goto out;

    // SetTx with single-shot timeout = 0 means "no timeout" — we'll
    // poll IRQ ourselves and bail on caller's `timeout_ms`.
    uint8_t txp[3] = { 0x00, 0x00, 0x00 };
    err = sx_cmd(OP_SET_TX, txp, 3, NULL, 0);
    if (err != ESP_OK) goto out;

    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (1) {
        if (gpio_get_level(LORA_PIN_DIO1) == 1) {
            uint16_t irq = 0;
            err = get_irq_status(&irq);
            if (err != ESP_OK) goto out;
            (void)clear_all_irqs();
            if (irq & IRQ_TX_DONE) { err = ESP_OK; goto out; }
            if (irq & IRQ_TIMEOUT) { err = ESP_ERR_TIMEOUT; goto out; }
        }
        if (esp_timer_get_time() > deadline) {
            // Force the radio back to standby so a half-transmitted
            // packet doesn't keep the PA hot.
            uint8_t standby = STANDBY_RC;
            (void)sx_cmd(OP_SET_STANDBY, &standby, 1, NULL, 0);
            err = ESP_ERR_TIMEOUT;
            goto out;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

out:
    xSemaphoreGive(s_state.lock);
    return err;
}

esp_err_t pageros_lora_rx(uint8_t *buf, size_t buf_capacity,
                          size_t *out_len,
                          int16_t *out_rssi_dbm,
                          int16_t *out_snr_db_q4,
                          uint32_t timeout_ms)
{
    if (buf == NULL || out_len == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_state.initialised) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_state.lock, portMAX_DELAY);

    // RX needs payload_len set to the maximum we're willing to accept
    // when in implicit-header mode; in explicit-header (default) the
    // header carries the length and this field is ignored.
    esp_err_t err = apply_packet_params(
        (uint8_t)(buf_capacity > PAGEROS_LORA_MAX_PACKET
                      ? PAGEROS_LORA_MAX_PACKET : buf_capacity));
    if (err != ESP_OK) goto out;
    err = clear_all_irqs();
    if (err != ESP_OK) goto out;

    uint8_t rxp[3] = { 0x00, 0x00, 0x00 };  // single-shot, no HW timeout
    err = sx_cmd(OP_SET_RX, rxp, 3, NULL, 0);
    if (err != ESP_OK) goto out;

    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (1) {
        if (gpio_get_level(LORA_PIN_DIO1) == 1) {
            uint16_t irq = 0;
            err = get_irq_status(&irq);
            if (err != ESP_OK) goto out;
            (void)clear_all_irqs();

            if (irq & (IRQ_HEADER_ERR | IRQ_CRC_ERR | IRQ_TIMEOUT)) {
                err = ESP_ERR_TIMEOUT;
                goto out;
            }
            if (irq & IRQ_RX_DONE) {
                uint8_t st[2] = {0};
                err = sx_cmd(OP_GET_RX_BUFFER_STATUS, NULL, 0, st, 2);
                if (err != ESP_OK) goto out;
                uint8_t plen = st[0];
                uint8_t poff = st[1];
                if (plen > buf_capacity) {
                    *out_len = plen;
                    err = ESP_ERR_INVALID_SIZE;
                    goto out;
                }
                err = sx_read_buffer(poff, buf, plen);
                if (err != ESP_OK) goto out;
                *out_len = plen;

                uint8_t ps[3] = {0};
                err = sx_cmd(OP_GET_PACKET_STATUS, NULL, 0, ps, 3);
                if (err != ESP_OK) goto out;
                // ps[0] = RssiPkt (raw, divide by 2 → -dBm)
                // ps[1] = SnrPkt  (signed, raw / 4 → dB; we keep q4)
                if (out_rssi_dbm) {
                    *out_rssi_dbm = -((int16_t)ps[0] / 2);
                }
                if (out_snr_db_q4) {
                    *out_snr_db_q4 = (int16_t)(int8_t)ps[1];
                }
                err = ESP_OK;
                goto out;
            }
        }
        if (esp_timer_get_time() > deadline) {
            uint8_t standby = STANDBY_RC;
            (void)sx_cmd(OP_SET_STANDBY, &standby, 1, NULL, 0);
            err = ESP_ERR_TIMEOUT;
            goto out;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

out:
    xSemaphoreGive(s_state.lock);
    return err;
}
