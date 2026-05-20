idf_component_register(
    SRCS "selftest.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_partition
        esp_psram
        spi_flash
        freertos
        log
)
