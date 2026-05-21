# PagerOS rotating SD logger — FW-004.
#
# Wraps the SD-mounted log file (/sd/logs/pageros.log) with `LOG_INFO`/
# `LOG_ERROR` macros that rotate at 1 MB. Uses `storage` for path
# constants (PAGEROS_DIR_LOGS) and ESP-IDF log+vfs subsystems for
# console parity and POSIX file ops.

idf_component_register(
    SRCS
        "src/logger.c"
        "src/logger_format.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common log
    PRIV_REQUIRES freertos vfs
)
