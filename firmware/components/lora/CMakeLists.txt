# PagerOS LoRa wrapper — FW-008. SX1262 over shared SPI2 + I2C
# power-gate (XL9555). Pure-C parameter helpers live in src/lora_params.c
# so they can be reused by the host-test harness.

idf_component_register(
    SRCS
        "src/lora.c"
        "src/lora_params.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common
    PRIV_REQUIRES driver esp_driver_spi esp_driver_i2c esp_timer log freertos
)
