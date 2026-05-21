# PagerOS Wi-Fi + HTTPS client — FW-009.
#
# Brings up esp_wifi station + an HTTPS client (esp_http_client + mbedTLS)
# attached to the Mozilla CA bundle.

idf_component_register(
    SRCS
        "src/network.c"
        "src/url.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_wifi esp_event esp_netif esp_http_client esp-tls mbedtls
             nvs_flash log freertos esp_common
)
