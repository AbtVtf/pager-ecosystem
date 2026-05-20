# Vendored TinyCBOR v0.6.0 (MIT) — see LICENSES/LICENSE.tinycbor.
#
# Upstream: https://github.com/intel/tinycbor
# Used by the PagerOS firmware CBOR codec (FW-016) and the cross-language
# conformance vectors (PROTO-003).
#
# We deliberately ship only the encode + decode core. Pretty-printing,
# CBOR↔JSON, and full validation helpers are omitted to keep flash usage
# minimal; the codec wrapper in `cbor_codec/` is what firmware code calls.

idf_component_register(
    SRCS
        "src/cborencoder.c"
        "src/cborencoder_close_container_checked.c"
        "src/cborencoder_float.c"
        "src/cborerrorstrings.c"
        "src/cborparser.c"
        "src/cborparser_dup_string.c"
        "src/cborparser_float.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
)

# TinyCBOR's helpers reference `assert` unconditionally — keep them live in
# release builds too so a malformed inbound CBOR frame can't slip past us
# silently.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    -DCBOR_PARSER_MAX_RECURSIONS=16
)
