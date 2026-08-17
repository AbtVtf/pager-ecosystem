# PagerOS Shell — FW-029.

idf_component_register(
    SRCS "src/shell.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common apprt storage
    PRIV_REQUIRES log
)
