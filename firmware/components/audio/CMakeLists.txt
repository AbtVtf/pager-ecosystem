# PagerOS audio driver + notification tones — FW-013 / FW-032.
#
# UI sound effects (click + scroll) are transcoded from MP3 to raw
# 8 kHz / mono / s16le PCM at build prep time (see /audio/*.mp3 → tones/*.pcm)
# and embedded straight into the binary via EMBED_FILES so the audio driver
# can play them without an MP3 decoder or SD read on every interaction.

idf_component_register(
    SRCS "src/audio.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_driver_i2s i2c_bus
    PRIV_REQUIRES log freertos
    EMBED_FILES
        "${CMAKE_SOURCE_DIR}/tones/click.pcm"
        "${CMAKE_SOURCE_DIR}/tones/scroll.pcm"
)
