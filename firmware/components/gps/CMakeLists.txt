# PagerOS GPS driver — FW-011.
#
# Public API in `include/pageros_gps.h`. The NMEA parser is split out into
# its own translation unit so the host tests under
# `firmware/host-tests/gps/` can link it with native gcc.

idf_component_register(
    SRCS
        "src/gps.c"
        "src/nmea.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES driver esp_common log freertos
)
