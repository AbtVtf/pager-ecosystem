# PagerOS transport selector — FW-025.

idf_component_register(
    SRCS "src/transport.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common network lora_client
    PRIV_REQUIRES log
)
