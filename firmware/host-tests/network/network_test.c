// SPDX-License-Identifier: Apache-2.0
//
// Host-side unit tests for FW-009. The Wi-Fi + esp_http_client paths
// only make sense on real hardware; what *is* portable is the HTTPS-only
// URL guard the client uses to refuse plain-HTTP downgrades.

#include "url.h"

#include <stdio.h>

static int fail_count = 0;

#define CHECK(cond, ...) do {                                       \
    if (!(cond)) {                                                  \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
        fprintf(stderr, __VA_ARGS__);                               \
        fprintf(stderr, "\n");                                      \
        fail_count++;                                               \
    }                                                               \
} while (0)

static void test_https_accepted(void)
{
    CHECK(pageros_url_is_https("https://example.com"), "bare host");
    CHECK(pageros_url_is_https("https://example.com/"), "host + slash");
    CHECK(pageros_url_is_https("https://example.com/path/here"),
          "host + path");
    CHECK(pageros_url_is_https("https://a"), "single-char host");
    CHECK(pageros_url_is_https("https://example.com:8443/x"),
          "host + port + path");
}

static void test_http_rejected(void)
{
    CHECK(!pageros_url_is_https("http://example.com"),
          "plain http must be rejected (downgrade protection)");
    CHECK(!pageros_url_is_https("HTTP://example.com"),
          "uppercase http still rejected (no case-folded equivalence)");
}

static void test_garbage_rejected(void)
{
    CHECK(!pageros_url_is_https(NULL), "NULL");
    CHECK(!pageros_url_is_https(""), "empty");
    CHECK(!pageros_url_is_https("https://"), "no host");
    CHECK(!pageros_url_is_https("https:///path"), "empty host with path");
    CHECK(!pageros_url_is_https("ftp://example.com"), "ftp");
    CHECK(!pageros_url_is_https("example.com"), "no scheme");
    CHECK(!pageros_url_is_https("https"), "scheme prefix only");
    CHECK(!pageros_url_is_https("//example.com"), "scheme-relative");
}

// Spelling matters — "Https://" (capital H) does not satisfy our exact
// prefix match. The HTTP RFC says schemes are case-insensitive, but the
// PagerOS client is the only place generating URLs internally, and
// outside that we only consume URLs from the spec's manifest schema
// which mandates lowercase. Being strict here also surfaces wonky
// inputs early instead of silently accepting them.
static void test_case_strict(void)
{
    CHECK(!pageros_url_is_https("Https://example.com"), "capital H");
    CHECK(!pageros_url_is_https("HTTPS://example.com"), "all caps");
}

int main(void)
{
    test_https_accepted();
    test_http_rejected();
    test_garbage_rejected();
    test_case_strict();

    if (fail_count == 0) {
        printf("OK (4 test cases)\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d assertion(s)\n", fail_count);
    return 1;
}
