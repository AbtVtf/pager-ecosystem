// SPDX-License-Identifier: Apache-2.0
//
// FW-027 — Exit Node discovery + ranking.
//
// Maintains a small ranked table of Exit Nodes the device has heard
// `exit-node-advertise` packets from. The table is RSSI-then-load-then-
// pubkey ordered, evicting the worst entry when full. Entries time out
// after a quiet window so a stale node doesn't outlive its actual
// presence on the air.
//
// The discovery loop is driven externally — the RX path in the LoRa
// loop dispatches advertised packets here. Caller-side selection is
// just `pageros_exit_discovery_best()`.

#include "pageros_lora_client.h"

#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>

#include "esp_timer.h"

#define EXIT_TABLE_CAP        8
#define EXIT_STALE_AFTER_US   (60 * 1000 * 1000)  // 60 seconds

typedef struct {
    uint8_t  pubkey[32];
    int16_t  rssi_dbm;
    uint8_t  load_class;      // lower = less loaded
    uint64_t last_seen_us;
    bool     used;
} exit_node_t;

static exit_node_t g_table[EXIT_TABLE_CAP];

// Decode an advert payload:
//   [u8 ver][u8 load_class][u8 reserved×2][u8 pubkey×32]
// Versioning lets us extend without breaking older readers. RSSI is
// recorded by the caller (the SX1262 driver already returns it from rx).
static bool decode_advert(const uint8_t *payload, size_t len,
                          uint8_t pubkey_out[32], uint8_t *load_out)
{
    if (!payload || len < 36) return false;
    if (payload[0] != 1) return false;
    *load_out = payload[1];
    memcpy(pubkey_out, payload + 4, 32);
    return true;
}

static int find_slot(const uint8_t pubkey[32])
{
    for (int i = 0; i < EXIT_TABLE_CAP; i++) {
        if (g_table[i].used && memcmp(g_table[i].pubkey, pubkey, 32) == 0) return i;
    }
    return -1;
}

static int worst_slot(uint64_t now)
{
    int worst = -1;
    int score_worst = INT_MIN / 2;  // very low
    for (int i = 0; i < EXIT_TABLE_CAP; i++) {
        if (!g_table[i].used) return i;  // empty wins
        // Stale entries are worst.
        if (now - g_table[i].last_seen_us > EXIT_STALE_AFTER_US) return i;
        // Lower rssi (more negative) is worse; higher load is worse.
        int score = g_table[i].rssi_dbm - (g_table[i].load_class * 4);
        if (score > score_worst) { score_worst = score; worst = i; }
    }
    return worst < 0 ? 0 : worst;
}

void pageros_exit_discovery_on_packet(const uint8_t *payload, size_t len,
                                      int16_t rssi_dbm)
{
    uint8_t pk[32]; uint8_t load = 0;
    if (!decode_advert(payload, len, pk, &load)) return;
    uint64_t now = (uint64_t)esp_timer_get_time();
    int slot = find_slot(pk);
    if (slot < 0) slot = worst_slot(now);
    g_table[slot].used = true;
    memcpy(g_table[slot].pubkey, pk, 32);
    g_table[slot].rssi_dbm = rssi_dbm;
    g_table[slot].load_class = load;
    g_table[slot].last_seen_us = now;
}

// Returns the best exit node's pubkey, or NULL if none are fresh enough.
const uint8_t *pageros_exit_discovery_best(int16_t *out_rssi)
{
    uint64_t now = (uint64_t)esp_timer_get_time();
    int best = -1;
    int score_best = INT_MIN / 2;
    for (int i = 0; i < EXIT_TABLE_CAP; i++) {
        if (!g_table[i].used) continue;
        if (now - g_table[i].last_seen_us > EXIT_STALE_AFTER_US) continue;
        int score = g_table[i].rssi_dbm - (g_table[i].load_class * 4);
        if (score > score_best) { score_best = score; best = i; }
    }
    if (best < 0) return NULL;
    if (out_rssi) *out_rssi = g_table[best].rssi_dbm;
    return g_table[best].pubkey;
}

int pageros_exit_discovery_count(void)
{
    uint64_t now = (uint64_t)esp_timer_get_time();
    int n = 0;
    for (int i = 0; i < EXIT_TABLE_CAP; i++) {
        if (g_table[i].used && now - g_table[i].last_seen_us <= EXIT_STALE_AFTER_US) n++;
    }
    return n;
}

void pageros_exit_discovery_reset(void)
{
    memset(g_table, 0, sizeof(g_table));
}
