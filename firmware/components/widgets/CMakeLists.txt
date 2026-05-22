# PagerOS widget renderer — FW-020 .. FW-023.

idf_component_register(
    SRCS "src/widgets.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common cbor_codec fonts esp_timer
    PRIV_REQUIRES log
)
