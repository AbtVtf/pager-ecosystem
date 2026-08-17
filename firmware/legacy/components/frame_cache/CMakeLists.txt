# PagerOS Frame cache — FW-017.
#
# Two-tier LRU cache (PSRAM L1, SD L2) for Frames. Pulls in mbedtls for
# the SHA-256 used in the L2 filename derivation and `storage` for path
# constants + mkdir helpers.

idf_component_register(
    SRCS
        "src/frame_cache.c"
        "src/frame_cache_key.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common storage
    PRIV_REQUIRES mbedtls esp_timer freertos
)
