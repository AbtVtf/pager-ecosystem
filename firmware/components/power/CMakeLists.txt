idf_component_register(
    SRCS "src/power.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_timer esp_hw_support display
    PRIV_REQUIRES log freertos
)
