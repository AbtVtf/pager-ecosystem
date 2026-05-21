# PagerOS persistent storage — FW-003.
#
# Brings up the microSD card on the shared SPI bus and creates the
# SPEC.md §7.4 directory tree on first boot.

idf_component_register(
    SRCS
        "src/storage.c"
        "src/path.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES driver esp_common log freertos fatfs sdmmc esp_driver_sdspi vfs
)
