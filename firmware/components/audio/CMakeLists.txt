# PagerOS audio driver + notification tones — FW-013 / FW-032.

idf_component_register(
    SRCS "src/audio.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_driver_i2s i2c_bus
    PRIV_REQUIRES log freertos
)
