// SPDX-License-Identifier: Apache-2.0
//
// Host-side conformance harness for the PagerOS CBOR codec (FW-016).
//
// Usage:
//   cbor_roundtrip <input.cbor>            # encode-mode test
//                                          #   decodes, re-encodes canonical,
//                                          #   writes the result to stdout.
//                                          #   exit 0 on success.
//   cbor_roundtrip --negative <input.cbor> # decode must fail
//                                          #   exit 0 only when decode errors.
//
// Pure host build; links the same `pageros_cbor.c` translation unit the
// firmware uses, plus the vendored TinyCBOR sources. See ../Makefile.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "pageros_cbor.h"

static int slurp(const char *path, uint8_t **out, size_t *out_len)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "stat %s: %s\n", path, strerror(errno));
        return 1;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); return 1; }
    size_t n = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(n + 1);
    if (!buf) { fclose(f); return 1; }
    if (n > 0 && fread(buf, 1, n, f) != n) { fclose(f); free(buf); return 1; }
    fclose(f);
    *out = buf;
    *out_len = n;
    return 0;
}

int main(int argc, char **argv)
{
    bool negative = false;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--negative") == 0) negative = true;
        else if (path == NULL) path = argv[i];
        else { fprintf(stderr, "extra arg: %s\n", argv[i]); return 2; }
    }
    if (!path) { fprintf(stderr, "usage: cbor_roundtrip [--negative] FILE\n"); return 2; }

    uint8_t *in = NULL;
    size_t   in_len = 0;
    if (slurp(path, &in, &in_len) != 0) return 2;

    // Generously sized arena. PROTO-003 vectors top out around 1 KB.
    enum { ARENA_BYTES = 1 << 20 };
    static uint8_t arena_backing[ARENA_BYTES];
    pgr_cbor_arena_t arena;
    pgr_cbor_arena_init(&arena, arena_backing, ARENA_BYTES);

    pgr_cbor_value_t *root = NULL;
    pgr_cbor_err_t r = pgr_cbor_decode(in, in_len, &arena, &root);

    if (negative) {
        if (r == PGR_CBOR_OK) {
            fprintf(stderr, "NEGATIVE: decode of %s unexpectedly succeeded\n", path);
            free(in);
            return 1;
        }
        free(in);
        return 0;
    }

    if (r != PGR_CBOR_OK) {
        fprintf(stderr, "decode %s: %s\n", path, pgr_cbor_strerror(r));
        free(in);
        return 1;
    }

    size_t out_cap = in_len + 256;
    if (out_cap < 1024) out_cap = 1024;
    uint8_t *out = (uint8_t *)malloc(out_cap);
    size_t out_len = 0;
    r = pgr_cbor_encode_canonical(root, out, out_cap, &out_len, &arena);
    if (r != PGR_CBOR_OK) {
        fprintf(stderr, "encode %s: %s\n", path, pgr_cbor_strerror(r));
        free(in); free(out);
        return 1;
    }

    if (fwrite(out, 1, out_len, stdout) != out_len) {
        free(in); free(out);
        return 1;
    }

    free(in);
    free(out);
    return 0;
}
