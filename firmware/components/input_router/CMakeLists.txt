# PagerOS input router — FW-024.
#
# Public API in `include/pageros_input_router.h`. The dispatch contract
# (focus → shell bubble) is split into its own translation unit so the
# host tests under `firmware/host-tests/router/` can link it with native
# gcc, matching the FW-006/FW-007 layout.

idf_component_register(
    SRCS
        "src/router.c"
        "src/dispatch.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES driver esp_common esp_timer log freertos input keyboard
)
