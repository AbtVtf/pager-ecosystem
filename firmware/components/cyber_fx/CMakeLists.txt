# PagerOS cyberpunk fx engine.
#
# Procedural visual layer: matrix rain, CRT glitch overlay, focus sweep,
# page-transition wipe, decorative katakana glyphs. Driven by a
# self-managed FreeRTOS task at ~25 fps; render callback is installed by
# the shell so the engine can request a repaint when an animation
# advances.

idf_component_register(
    SRCS "src/cyber_fx.c"
    INCLUDE_DIRS "include"
    REQUIRES fonts widgets esp_timer freertos
    PRIV_REQUIRES log
)
