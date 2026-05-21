# PagerOS firmware crypto component.
#
# Vendors Monocypher (BSD-2-Clause / CC0-1.0 dual-licensed) and exposes
# only what the rest of the firmware currently needs — for now just
# Ed25519 (RFC 8032 / SHA-512 variant) public-key derivation for FW-014.
# FW-015 will add wrappers for signing, X25519, and ChaCha20-Poly1305 on
# top of the same vendored primitives.

idf_component_register(
    SRCS
        "src/pageros_crypto.c"
        "src/monocypher.c"
        "src/monocypher-ed25519.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common
)
