// SPDX-License-Identifier: Apache-2.0
//
// Pure-C path helpers used by the storage component. Split so the host
// tests can link mkdir_p_split without dragging in newlib `<sys/stat.h>`.

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Walk `path` and call `cb(prefix, ctx)` once for each non-empty
// directory prefix from shortest to longest:
//   "/a/b/c" → "/a", "/a/b", "/a/b/c"
// Returns false on success, true if `cb` returned non-zero or the
// internal scratch buffer overflows.
//
// `cb` may be called with `prefix == path` for the final iteration; the
// buffer it receives must be treated as read-only.
bool pageros_path_walk_prefixes(const char *path,
                                int (*cb)(const char *prefix, void *ctx),
                                void *ctx,
                                char *scratch,
                                size_t scratch_len);

#ifdef __cplusplus
}
#endif
