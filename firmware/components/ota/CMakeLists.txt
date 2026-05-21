idf_component_register(
    SRCS "src/ota.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common app_update esp_https_ota esp_http_client esp-tls
    PRIV_REQUIRES log
)
