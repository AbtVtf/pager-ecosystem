# PagerOS audio driver + notification tones — FW-013 / FW-032.

idf_component_register(
    SRCS "src/audio.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_driver_i2s esp_driver_i2c driver
    PRIV_REQUIRES log freertos
)
