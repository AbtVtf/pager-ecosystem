idf_component_register(
    SRCS "src/xl9555.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common i2c_bus
    PRIV_REQUIRES log freertos
)
