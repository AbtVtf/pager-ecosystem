# PagerOS input driver — FW-007 (rotary encoder + ENTER + BACK).
#
# Public API in `include/pageros_input.h`. The quadrature decoder is
# split out into its own translation unit so the host tests under
# `firmware/host-tests/input/` can link it with native gcc.

idf_component_register(
    SRCS
        "src/input.c"
        "src/quad.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES driver esp_common esp_timer log freertos
)
