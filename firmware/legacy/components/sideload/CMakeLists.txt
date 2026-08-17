idf_component_register(
    SRCS "src/sideload.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common cbor_codec network storage
    PRIV_REQUIRES log
)
