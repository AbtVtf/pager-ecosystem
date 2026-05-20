# PagerOS CBOR codec — FW-016.
#
# Public API in `include/pageros_cbor.h`. Pure C (no ESP-IDF symbols) so the
# same translation unit feeds the on-device build and the host-side
# conformance tests under `firmware/host-tests/cbor_codec/`.

idf_component_register(
    SRCS "src/pageros_cbor.c"
    INCLUDE_DIRS "include"
    REQUIRES tinycbor
)
