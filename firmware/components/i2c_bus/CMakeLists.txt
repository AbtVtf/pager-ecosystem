idf_component_register(
    SRCS "src/i2c_bus.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_driver_i2c driver
    PRIV_REQUIRES log freertos
)
