# PagerOS app runtime — FW-028.

idf_component_register(
    SRCS "src/apprt.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common cbor_codec
    PRIV_REQUIRES log nvs_flash
)
