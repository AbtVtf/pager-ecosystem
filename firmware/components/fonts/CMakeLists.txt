# PagerOS fonts + Unicode renderer — FW-019.
#
# v0 ships one bundled font (public-domain `font8x8_basic`). The API
# leaves room for additional Noto Sans subsets loaded from
# `/system/fonts/` as a follow-up — see pageros_fonts.h.

idf_component_register(
    SRCS
        "src/fonts.c"
        "src/fonts_utf8.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src" "vendor/font8x8" "vendor/font16x16"
    REQUIRES esp_common
    PRIV_REQUIRES log
)
