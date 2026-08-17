idf_component_register(
    SRCS "src/permissions.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES log nvs_flash
)
