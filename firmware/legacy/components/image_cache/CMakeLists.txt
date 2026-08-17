# PagerOS Image cache + PNG decoder — FW-018.
#
# Content-addressed image store backed by /cache/images on SD. Decode
# via vendored LodePNG (ZLib license).

idf_component_register(
    SRCS
        "src/image_cache.c"
        "vendor/lodepng/lodepng.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src" "vendor/lodepng"
    REQUIRES esp_common storage
    PRIV_REQUIRES mbedtls
)

# LodePNG is C++-by-default but compiles cleanly as C; quiet the
# `-Wsign-compare` / `-Wunused-parameter` warnings it ships with so
# our `-Werror` doesn't fail on a vendored upstream.
set_source_files_properties(
    vendor/lodepng/lodepng.c
    PROPERTIES
    COMPILE_FLAGS "-w -DLODEPNG_NO_COMPILE_ENCODER -DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS"
)
