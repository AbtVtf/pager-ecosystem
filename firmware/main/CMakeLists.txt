idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        selftest
        app_update
        esp_hw_support
        spi_flash
        log
)
