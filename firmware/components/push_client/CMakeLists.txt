# PagerOS push notification client — FW-030.

idf_component_register(
    SRCS "src/push_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common network storage
    PRIV_REQUIRES log
)
