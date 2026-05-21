// SPDX-License-Identifier: Apache-2.0
//
// PagerOS persistent storage — see `pageros_storage.h`.

#include "pageros_storage.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include "path.h"

static const char *TAG = "storage";

// ---------------------------------------------------------------------------
// Board wiring (LILYGO T-LoRa Pager — see arduino-esp32 variant
// `lilygo_tlora_pager/pins_arduino.h`)
// ---------------------------------------------------------------------------

#define SD_PIN_MOSI         34
#define SD_PIN_MISO         33
#define SD_PIN_SCK          35
#define SD_PIN_CS           21
#define SD_SPI_HOST         SPI2_HOST
#define SD_SPI_FREQ_KHZ     4000

// XL9555 expander control pins, used by the SD slot. The XL9555 sits on
// the same shared I2C bus as the keyboard and the codec/IMU (SDA=3,
// SCL=2). We inline the helpers because gps.c and keyboard.c already
// duplicate this code — three call sites is the cue to refactor into a
// shared `io_expander` component, but that's a follow-up task.
#define XL9555_I2C_PORT     I2C_NUM_0
#define XL9555_I2C_SDA_GPIO 3
#define XL9555_I2C_SCL_GPIO 2
#define XL9555_I2C_FREQ_HZ  400000
#define XL9555_I2C_ADDR     0x20

#define XL9555_REG_OUTPUT_1 0x03  // P1.x output port
#define XL9555_REG_INPUT_1  0x01  // P1.x input port (for SD_DET)
#define XL9555_REG_CONFIG_1 0x07  // P1.x direction (1 = input)

// EXPANDS_SD_EN  = bit 12 → P1.4
// EXPANDS_SD_DET = bit 10 → P1.2
#define XL9555_BIT_SD_EN    (1u << 4)
#define XL9555_BIT_SD_DET   (1u << 2)

// ---------------------------------------------------------------------------
// Directory tree (SPEC.md §7.4)
// ---------------------------------------------------------------------------

static const char *const PAGEROS_DIRS[] = {
    PAGEROS_DIR_SYSTEM,
    PAGEROS_DIR_FONTS,
    PAGEROS_DIR_TONES,
    PAGEROS_DIR_CACHE,
    PAGEROS_DIR_CACHE_FRAMES,
    PAGEROS_DIR_CACHE_IMAGES,
    PAGEROS_DIR_CACHE_TILES,
    PAGEROS_DIR_APPS,
    PAGEROS_DIR_NOTIFICATIONS,
    PAGEROS_DIR_LOGS,
};

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------

static struct {
    bool          mounted;
    bool          spi_bus_up;
    sdmmc_card_t *card;
} s_state;

// ---------------------------------------------------------------------------
// XL9555 helpers (inline — see comment above)
// ---------------------------------------------------------------------------

static esp_err_t xl_i2c_init_once(void)
{
    static bool i2c_up = false;
    if (i2c_up) return ESP_OK;
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = XL9555_I2C_SDA_GPIO,
        .scl_io_num       = XL9555_I2C_SCL_GPIO,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = XL9555_I2C_FREQ_HZ,
    };
    // Install first. If another component beat us to it we get ESP_FAIL
    // (IDF v5.3) or ESP_ERR_INVALID_STATE — either way the bus is up.
    esp_err_t err = i2c_driver_install(XL9555_I2C_PORT, cfg.mode, 0, 0, 0);
    if (err == ESP_OK) {
        err = i2c_param_config(XL9555_I2C_PORT, &cfg);
        if (err != ESP_OK) return err;
    } else if (err != ESP_ERR_INVALID_STATE && err != ESP_FAIL) {
        return err;
    }
    i2c_up = true;
    return ESP_OK;
}

static esp_err_t xl_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_write_read_device(XL9555_I2C_PORT, XL9555_I2C_ADDR,
                                        &reg, 1, value, 1,
                                        pdMS_TO_TICKS(50));
}

static esp_err_t xl_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_write_to_device(XL9555_I2C_PORT, XL9555_I2C_ADDR,
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

static esp_err_t xl_power_sd(bool on)
{
    esp_err_t err = xl_set_bits(XL9555_REG_CONFIG_1, XL9555_BIT_SD_EN, 0);
    if (err != ESP_OK) return err;
    return xl_set_bits(XL9555_REG_OUTPUT_1, XL9555_BIT_SD_EN,
                       on ? XL9555_BIT_SD_EN : 0);
}

static esp_err_t xl_card_present(bool *present)
{
    esp_err_t err = xl_set_bits(XL9555_REG_CONFIG_1, XL9555_BIT_SD_DET,
                                XL9555_BIT_SD_DET);  // input
    if (err != ESP_OK) return err;
    uint8_t reg = 0;
    err = xl_read(XL9555_REG_INPUT_1, &reg);
    if (err != ESP_OK) return err;
    // SD_DET on this board is active-low when a card is inserted (matches
    // LilyGoLib's `installSD()` behaviour: it returns early if the pin
    // reads high).
    *present = (reg & XL9555_BIT_SD_DET) == 0;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// mkdir -p
// ---------------------------------------------------------------------------

static int mkdir_one(const char *prefix, void *ctx)
{
    (void)ctx;
    if (mkdir(prefix, 0755) == 0) return 0;
    if (errno == EEXIST) return 0;
    ESP_LOGE(TAG, "mkdir(%s) failed: %s", prefix, strerror(errno));
    return -1;
}

esp_err_t pageros_storage_mkdir_p(const char *path)
{
    char scratch[128];
    if (pageros_path_walk_prefixes(path, mkdir_one, NULL,
                                    scratch, sizeof(scratch))) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t ensure_dir_tree(void)
{
    for (size_t i = 0; i < sizeof(PAGEROS_DIRS) / sizeof(PAGEROS_DIRS[0]); i++) {
        esp_err_t err = pageros_storage_mkdir_p(PAGEROS_DIRS[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// SPI + SD mount
// ---------------------------------------------------------------------------

static esp_err_t spi_bus_up_once(void)
{
    if (s_state.spi_bus_up) return ESP_OK;
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = SD_PIN_MOSI,
        .miso_io_num     = SD_PIN_MISO,
        .sclk_io_num     = SD_PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err == ESP_ERR_INVALID_STATE) {
        // Bus already up — another sharer (LoRa / NFC / display) got
        // here first. That's fine.
        s_state.spi_bus_up = true;
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    s_state.spi_bus_up = true;
    return ESP_OK;
}

esp_err_t pageros_storage_init(void)
{
    if (s_state.mounted) return ESP_OK;

    esp_err_t err = xl_i2c_init_once();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    err = xl_power_sd(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "xl power-on: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }
    // Datasheet-ish ramp time before the card sees a valid clock.
    vTaskDelay(pdMS_TO_TICKS(30));

    bool present = false;
    err = xl_card_present(&present);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "xl card-detect read: %s", esp_err_to_name(err));
        // Don't abort — some boards leave SD_DET floating; still try
        // mounting and let the SD response confirm presence.
    } else if (!present) {
        ESP_LOGW(TAG, "no SD card detected (XL9555 SD_DET)");
        (void)xl_power_sd(false);
        return ESP_ERR_NOT_FOUND;
    }

    err = spi_bus_up_once();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi bus: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot         = SD_SPI_HOST;
    host.max_freq_khz = SD_SPI_FREQ_KHZ;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_PIN_CS;
    slot.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(PAGEROS_SD_MOUNT, &host, &slot,
                                  &mount_cfg, &s_state.card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdspi mount: %s", esp_err_to_name(err));
        (void)xl_power_sd(false);
        return err;
    }

    err = ensure_dir_tree();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dir tree: %s", esp_err_to_name(err));
        esp_vfs_fat_sdcard_unmount(PAGEROS_SD_MOUNT, s_state.card);
        s_state.card = NULL;
        (void)xl_power_sd(false);
        return err;
    }

    s_state.mounted = true;
    ESP_LOGI(TAG, "SD mounted at %s (%lluMB)",
             PAGEROS_SD_MOUNT,
             ((uint64_t)s_state.card->csd.capacity)
                 * s_state.card->csd.sector_size / (1024ULL * 1024ULL));
    return ESP_OK;
}

esp_err_t pageros_storage_shutdown(void)
{
    if (!s_state.mounted) return ESP_OK;
    esp_err_t err = esp_vfs_fat_sdcard_unmount(PAGEROS_SD_MOUNT, s_state.card);
    s_state.card    = NULL;
    s_state.mounted = false;
    (void)xl_power_sd(false);
    return err;
}

bool pageros_storage_is_mounted(void)
{
    return s_state.mounted;
}
