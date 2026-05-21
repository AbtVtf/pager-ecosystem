# PagerOS display driver — FW-005.
#
# Vendors the Espressif Apache-2.0 ST7796 panel driver from LilyGoLib's
# bsp_lcd (same license as PagerOS firmware) and wraps it with a small
# fill/gradient/blit/present API on top of a PSRAM framebuffer.

idf_component_register(
    SRCS
        "src/display.c"
        "src/gradient.c"
        "src/esp_lcd_st7796.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES driver esp_common esp_lcd esp_driver_spi esp_driver_ledc
             esp_driver_gpio esp_psram log freertos
)
