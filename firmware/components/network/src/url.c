// SPDX-License-Identifier: Apache-2.0

#include "url.h"

#include <string.h>

bool pageros_url_is_https(const char *url)
{
    if (!url) return false;
    static const char prefix[] = "https://";
    const size_t plen = sizeof(prefix) - 1;
    if (strncmp(url, prefix, plen) != 0) return false;
    // Host portion must be non-empty (i.e. the next char isn't '/'
    // and isn't NUL).
    char c = url[plen];
    return c != '\0' && c != '/';
}
