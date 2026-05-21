# PagerOS NFC driver — FW-010. ST25R3916 over shared SPI2_HOST.

idf_component_register(
    SRCS "src/nfc.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_driver_spi esp_driver_gpio driver
    PRIV_REQUIRES log freertos
)
