// SPDX-License-Identifier: Apache-2.0
//
// Host-side unit tests for FW-003 path utilities. The on-device part
// (SPI bring-up, FAT mount, XL9555 power gate) runs against real
// hardware; the only piece worth host-testing is the path prefix walk
// that powers `pageros_storage_mkdir_p`. If that walk is right, the
// mkdir-p loop over §7.4 paths is trivially right.

#include "path.h"

#include <stdio.h>
#include <string.h>

static int fail_count = 0;

#define CHECK(cond, ...) do {                                       \
    if (!(cond)) {                                                  \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
        fprintf(stderr, __VA_ARGS__);                               \
        fprintf(stderr, "\n");                                      \
        fail_count++;                                               \
    }                                                               \
} while (0)

typedef struct {
    int    nprefixes;
    char   prefixes[16][64];
    int    fail_after;   // return -1 from cb after N prefixes (0 = never)
} recorder_t;

static int record(const char *prefix, void *ctx)
{
    recorder_t *r = (recorder_t *)ctx;
    if (r->fail_after && r->nprefixes >= r->fail_after) return -1;
    if (r->nprefixes < (int)(sizeof(r->prefixes) / sizeof(r->prefixes[0]))) {
        snprintf(r->prefixes[r->nprefixes], sizeof(r->prefixes[0]),
                 "%s", prefix);
    }
    r->nprefixes++;
    return 0;
}

static void test_walk_basic(void)
{
    recorder_t r = {0};
    char scratch[64];
    bool err = pageros_path_walk_prefixes("/sd/system/fonts",
                                          record, &r, scratch, sizeof(scratch));
    CHECK(!err, "walk reported error on valid path");
    CHECK(r.nprefixes == 3, "expected 3 prefixes, got %d", r.nprefixes);
    CHECK(strcmp(r.prefixes[0], "/sd") == 0, "p[0]=%s", r.prefixes[0]);
    CHECK(strcmp(r.prefixes[1], "/sd/system") == 0, "p[1]=%s", r.prefixes[1]);
    CHECK(strcmp(r.prefixes[2], "/sd/system/fonts") == 0,
          "p[2]=%s", r.prefixes[2]);
}

// A deeper path — the §7.4 cache subtree.
static void test_walk_cache(void)
{
    recorder_t r = {0};
    char scratch[64];
    pageros_path_walk_prefixes("/sd/cache/tiles", record, &r,
                                scratch, sizeof(scratch));
    CHECK(r.nprefixes == 3, "got %d", r.nprefixes);
    CHECK(strcmp(r.prefixes[1], "/sd/cache") == 0, "p[1]=%s", r.prefixes[1]);
    CHECK(strcmp(r.prefixes[2], "/sd/cache/tiles") == 0,
          "p[2]=%s", r.prefixes[2]);
}

// Trailing slash should be handled — no empty-tail prefix.
static void test_walk_trailing_slash(void)
{
    recorder_t r = {0};
    char scratch[64];
    pageros_path_walk_prefixes("/sd/logs/", record, &r,
                                scratch, sizeof(scratch));
    CHECK(r.nprefixes == 2, "got %d", r.nprefixes);
    CHECK(strcmp(r.prefixes[0], "/sd") == 0, "%s", r.prefixes[0]);
    CHECK(strcmp(r.prefixes[1], "/sd/logs") == 0, "%s", r.prefixes[1]);
}

// Single-component path emits exactly one prefix.
static void test_walk_single(void)
{
    recorder_t r = {0};
    char scratch[64];
    pageros_path_walk_prefixes("/sd", record, &r, scratch, sizeof(scratch));
    CHECK(r.nprefixes == 1, "got %d", r.nprefixes);
    CHECK(strcmp(r.prefixes[0], "/sd") == 0, "%s", r.prefixes[0]);
}

// Callback returning non-zero short-circuits the walk and the function
// reports an error. Matches `mkdir_one`'s failure propagation.
static void test_walk_cb_failure_propagates(void)
{
    recorder_t r = { .fail_after = 2 };
    char scratch[64];
    bool err = pageros_path_walk_prefixes("/sd/cache/tiles/x", record, &r,
                                           scratch, sizeof(scratch));
    CHECK(err, "walk must report error when cb fails");
    // The recorder counts successful invocations only — the failing
    // third call returns -1 before incrementing. Walk should have
    // stopped before reaching the fourth prefix.
    CHECK(r.nprefixes == 2, "expected 2 successful invocations, got %d",
          r.nprefixes);
}

// Scratch too small → reported as error rather than truncating.
static void test_walk_buffer_overflow(void)
{
    recorder_t r = {0};
    char tiny[4];
    bool err = pageros_path_walk_prefixes("/sd/system/fonts",
                                           record, &r, tiny, sizeof(tiny));
    CHECK(err, "walk must report error when scratch too small");
}

// NULL inputs are tolerated and report error.
static void test_null_safety(void)
{
    recorder_t r = {0};
    char scratch[16];
    CHECK(pageros_path_walk_prefixes(NULL, record, &r, scratch, sizeof(scratch)),
          "NULL path");
    CHECK(pageros_path_walk_prefixes("/sd", NULL, &r, scratch, sizeof(scratch)),
          "NULL cb");
    CHECK(pageros_path_walk_prefixes("/sd", record, &r, NULL, 0),
          "NULL scratch");
}

int main(void)
{
    test_walk_basic();
    test_walk_cache();
    test_walk_trailing_slash();
    test_walk_single();
    test_walk_cb_failure_propagates();
    test_walk_buffer_overflow();
    test_null_safety();

    if (fail_count == 0) {
        printf("OK (7 test cases)\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d assertion(s)\n", fail_count);
    return 1;
}
