// SPDX-License-Identifier: Apache-2.0

#include "path.h"

#include <string.h>

bool pageros_path_walk_prefixes(const char *path,
                                int (*cb)(const char *prefix, void *ctx),
                                void *ctx,
                                char *scratch,
                                size_t scratch_len)
{
    if (!path || !cb || !scratch || scratch_len == 0) return true;
    size_t plen = strlen(path);
    if (plen + 1 > scratch_len) return true;

    // Iterate over each '/' that starts a new component.
    // Skip a leading "/" so we don't try to mkdir an empty prefix.
    for (size_t i = 1; i < plen; i++) {
        if (path[i] == '/') {
            memcpy(scratch, path, i);
            scratch[i] = '\0';
            if (cb(scratch, ctx) != 0) return true;
        }
    }
    // Final prefix is the whole path. Skip if it ends with '/'
    // (already emitted by the loop above with the trailing-slash
    // truncated).
    if (path[plen - 1] != '/') {
        memcpy(scratch, path, plen);
        scratch[plen] = '\0';
        if (cb(scratch, ctx) != 0) return true;
    }
    return false;
}
