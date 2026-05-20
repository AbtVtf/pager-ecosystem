// SPDX-License-Identifier: Apache-2.0
//
// PagerOS CBOR codec — FW-016.
//
// Thin wrapper around TinyCBOR that exposes the two operations the rest of
// the firmware actually needs:
//
//   1. Decode an arbitrary CBOR byte string into a value tree
//      (`pgr_cbor_decode`).
//   2. Re-encode a value tree into RFC 8949 §4.2 canonical CBOR
//      (`pgr_cbor_encode_canonical`).
//
// The canonical encoder is the half that diverges from TinyCBOR: TinyCBOR
// emits whatever order the caller writes map keys in, whereas PagerOS frames
// MUST be lex-sorted on the wire so that the cross-language conformance
// vectors in `protocol/test-vectors/ui/` are byte-reproducible. Sorting and
// the smallest-int / shortest-float reductions all live in
// `pageros_cbor.c`.
//
// Memory model: a `pgr_cbor_arena_t` is a caller-owned bump allocator that
// backs every node and every byte buffer in the resulting tree. Free the
// whole arena to free the whole tree; individual nodes are never freed.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PGR_CBOR_OK = 0,
    PGR_CBOR_ERR_PARSE,           // malformed CBOR bytes
    PGR_CBOR_ERR_DEPTH,           // nesting exceeded PGR_CBOR_MAX_DEPTH
    PGR_CBOR_ERR_OOM,             // arena exhausted
    PGR_CBOR_ERR_BUFFER,          // output buffer too small for encode
    PGR_CBOR_ERR_UNSUPPORTED,     // input uses a CBOR feature we don't emit
                                  // (indefinite-length, semantic tag, etc.)
    PGR_CBOR_ERR_TRAILING,        // bytes left over after the top-level item
} pgr_cbor_err_t;

const char *pgr_cbor_strerror(pgr_cbor_err_t e);

// Hard upper bound on nesting depth. Keeps stack usage bounded on-device
// and matches TinyCBOR's `CBOR_PARSER_MAX_RECURSIONS` default.
#define PGR_CBOR_MAX_DEPTH 16

// Bump-allocator arena. Caller owns the backing buffer; the arena writes
// only into that buffer and never calls malloc/free.
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   used;
} pgr_cbor_arena_t;

void pgr_cbor_arena_init(pgr_cbor_arena_t *a, void *backing, size_t cap);
void pgr_cbor_arena_reset(pgr_cbor_arena_t *a);

// Value kinds. Mirrors RFC 8949 major types, collapsed for callers:
// uint and negint stay distinct so the round-trip preserves the wire form
// (a negint can hold values smaller than INT64_MIN, see §3.4).
typedef enum {
    PGR_CBOR_KIND_UINT,        // major 0
    PGR_CBOR_KIND_NEGINT,      // major 1 (value = -(stored+1))
    PGR_CBOR_KIND_BYTES,       // major 2
    PGR_CBOR_KIND_TEXT,        // major 3
    PGR_CBOR_KIND_ARRAY,       // major 4
    PGR_CBOR_KIND_MAP,         // major 5
    PGR_CBOR_KIND_TAG,         // major 6
    PGR_CBOR_KIND_BOOL,        // major 7, simple 20/21
    PGR_CBOR_KIND_NULL,        // major 7, simple 22
    PGR_CBOR_KIND_UNDEFINED,   // major 7, simple 23
    PGR_CBOR_KIND_FLOAT,       // major 7, double-precision after promotion
} pgr_cbor_kind_t;

typedef struct pgr_cbor_value pgr_cbor_value_t;
typedef struct pgr_cbor_pair  pgr_cbor_pair_t;

struct pgr_cbor_value {
    pgr_cbor_kind_t kind;
    union {
        uint64_t u64;                                // UINT, NEGINT (raw)
        bool     boolean;                            // BOOL
        double   dbl;                                // FLOAT
        struct { const uint8_t *data; size_t len; } bytes;   // BYTES / TEXT
        struct { pgr_cbor_value_t *items; size_t len; } arr; // ARRAY
        struct { pgr_cbor_pair_t  *items; size_t len; } map; // MAP
        struct { uint64_t tag; pgr_cbor_value_t *content; } tagged; // TAG
    } v;
};

struct pgr_cbor_pair {
    pgr_cbor_value_t key;
    pgr_cbor_value_t val;
};

// Decode `buf[0..len)` into a tree allocated in `arena`. On success
// `*out_root` points into the arena. On error the arena state is undefined
// (caller should reset it).
//
// Bytes beyond the single top-level item produce PGR_CBOR_ERR_TRAILING —
// PagerOS frames are always exactly one CBOR item per envelope.
pgr_cbor_err_t pgr_cbor_decode(const uint8_t   *buf,
                               size_t           len,
                               pgr_cbor_arena_t *arena,
                               pgr_cbor_value_t **out_root);

// Encode `root` into `out_buf` using RFC 8949 §4.2 canonical rules:
//   - Definite-length arrays and maps.
//   - Map keys sorted lexicographically by encoded bytes (§4.2.1).
//   - Smallest unsigned-integer form (TinyCBOR default).
//   - Floats demoted to binary32 when they round-trip, otherwise binary64.
//
// `*out_written` is set to the number of bytes produced on success.
// `arena` is used for scratch allocations (per-map sort keys); it can be
// the same arena that backs the tree.
pgr_cbor_err_t pgr_cbor_encode_canonical(const pgr_cbor_value_t *root,
                                         uint8_t          *out_buf,
                                         size_t            out_cap,
                                         size_t           *out_written,
                                         pgr_cbor_arena_t *arena);

#ifdef __cplusplus
}
#endif
