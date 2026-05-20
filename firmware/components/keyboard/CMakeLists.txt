# PagerOS keyboard driver — FW-006.
#
# Public API in `include/pageros_keyboard.h`. The TCA8418 event-byte
# decoder is split out into its own translation unit so the host tests
# under `firmware/host-tests/keyboard/` can link it with native gcc.

idf_component_register(
    SRCS
        "src/keyboard.c"
        "src/tca8418.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES driver esp_common esp_timer log freertos
)
