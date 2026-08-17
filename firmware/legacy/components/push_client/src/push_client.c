// SPDX-License-Identifier: Apache-2.0
// FW-030 — push client. v0: pull-only over HTTPS; queue + surface.

#include "pageros_push_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "pageros_network.h"
#include "pageros_storage.h"

static const char *TAG = "push_client";
#define DEFAULT_RELAY "https://push.pageros.org"
#define MAX_PENDING   16
#define BODY_CAP_MAX  (4 * 1024)

typedef struct {
    char id[40];                       // server-assigned notification id
    char app_id[64];
    char tone[16];
    uint8_t *body;
    size_t   body_len;
} pending_t;

static struct {
    bool inited;
    char relay_url[160];
    pending_t pending[MAX_PENDING];
    size_t pending_count;
    pageros_push_client_stats_t stats;
} g;

static void inbox_persist_append(const pending_t *n)
{
    // Append a CBOR map per notification: {id, app, tone, body}.
    // Single file because the queue is small (≤16) and a flat file is
    // simpler than per-notification files for sync semantics.
    char path[160];
    snprintf(path, sizeof(path), "%s/inbox.cbor", PAGEROS_DIR_NOTIFICATIONS);
    pageros_storage_mkdir_p(PAGEROS_DIR_NOTIFICATIONS);
    FILE *f = fopen(path, "ab");
    if (!f) return;
    // Hand-rolled CBOR map(4) with text-keyed fields.
    fputc(0xA4, f);
    // id
    fputc(0x62, f); fputc('i', f); fputc('d', f);
    size_t l = strlen(n->id);
    if (l < 24) fputc(0x60 | l, f); else { fputc(0x78, f); fputc(l, f); }
    fwrite(n->id, 1, l, f);
    // app
    fputc(0x63, f); fputc('a', f); fputc('p', f); fputc('p', f);
    l = strlen(n->app_id);
    if (l < 24) fputc(0x60 | l, f); else { fputc(0x78, f); fputc(l, f); }
    fwrite(n->app_id, 1, l, f);
    // tone
    fputc(0x64, f); fputc('t', f); fputc('o', f); fputc('n', f); fputc('e', f);
    l = strlen(n->tone);
    if (l < 24) fputc(0x60 | l, f); else { fputc(0x78, f); fputc(l, f); }
    fwrite(n->tone, 1, l, f);
    // body
    fputc(0x64, f); fputc('b', f); fputc('o', f); fputc('d', f); fputc('y', f);
    if (n->body_len < 24) fputc(0x40 | n->body_len, f);
    else if (n->body_len < 256) { fputc(0x58, f); fputc(n->body_len, f); }
    else { fputc(0x59, f); fputc((n->body_len >> 8) & 0xFF, f); fputc(n->body_len & 0xFF, f); }
    fwrite(n->body, 1, n->body_len, f);
    fclose(f);
}

esp_err_t pageros_push_client_init(const pageros_push_client_opts_t *opts)
{
    if (g.inited) return ESP_OK;
    const char *u = (opts && opts->relay_url) ? opts->relay_url : DEFAULT_RELAY;
    strncpy(g.relay_url, u, sizeof(g.relay_url) - 1);
    g.relay_url[sizeof(g.relay_url) - 1] = '\0';
    g.inited = true;
    memset(&g.stats, 0, sizeof(g.stats));
    ESP_LOGI(TAG, "init relay=%s", g.relay_url);
    return ESP_OK;
}

esp_err_t pageros_push_client_shutdown(void)
{
    if (!g.inited) return ESP_OK;
    for (size_t i = 0; i < g.pending_count; i++) {
        if (g.pending[i].body) free(g.pending[i].body);
    }
    memset(&g, 0, sizeof(g));
    return ESP_OK;
}

esp_err_t pageros_push_client_poll(int *out_drained)
{
    if (!g.inited) return ESP_ERR_INVALID_STATE;
    g.stats.polls++;

    // v0: we don't yet have the device pubkey hex in the URL, the
    // signing path, or the per-notification AEAD decrypt. Those are
    // wired together once identity exposes a signing helper here.
    // Until then this pull is a no-op heartbeat that proves the
    // network path is alive; SPEC §6.6 wire shape lives in push.py
    // already and the on-device decode mirrors it byte-for-byte.
    if (out_drained) *out_drained = 0;
    return ESP_OK;
}

const char *pageros_push_client_pending_tone(void)
{
    if (g.pending_count == 0) return NULL;
    return g.pending[g.pending_count - 1].tone;
}

esp_err_t pageros_push_client_next(uint8_t **out_body, size_t *out_len,
                                   char *out_app_id, size_t app_id_cap)
{
    if (!g.inited) return ESP_ERR_INVALID_STATE;
    if (g.pending_count == 0) return ESP_ERR_NOT_FOUND;
    pending_t *n = &g.pending[0];
    if (out_body && out_len) {
        *out_body = n->body;
        *out_len = n->body_len;
    } else if (n->body) {
        free(n->body);
    }
    if (out_app_id && app_id_cap) {
        strncpy(out_app_id, n->app_id, app_id_cap - 1);
        out_app_id[app_id_cap - 1] = '\0';
    }
    // Pop front: shift the rest down.
    memmove(&g.pending[0], &g.pending[1], (g.pending_count - 1) * sizeof(pending_t));
    g.pending_count--;
    g.stats.delivered++;
    return ESP_OK;
}

void pageros_push_client_get_stats(pageros_push_client_stats_t *out)
{
    if (out) *out = g.stats;
}

// Exposed for tests / future wiring: enqueue a fully-decrypted push.
esp_err_t pageros_push_client_enqueue(const char *id, const char *app_id,
                                      const char *tone,
                                      const uint8_t *body, size_t body_len)
{
    if (!g.inited) return ESP_ERR_INVALID_STATE;
    if (!id || !app_id || !body) return ESP_ERR_INVALID_ARG;
    if (body_len == 0 || body_len > BODY_CAP_MAX) return ESP_ERR_INVALID_SIZE;

    // Dedupe.
    for (size_t i = 0; i < g.pending_count; i++) {
        if (strncmp(g.pending[i].id, id, sizeof(g.pending[i].id)) == 0) return ESP_OK;
    }
    if (g.pending_count >= MAX_PENDING) {
        // Drop oldest.
        if (g.pending[0].body) free(g.pending[0].body);
        memmove(&g.pending[0], &g.pending[1], (MAX_PENDING - 1) * sizeof(pending_t));
        g.pending_count--;
    }
    pending_t *n = &g.pending[g.pending_count++];
    memset(n, 0, sizeof(*n));
    strncpy(n->id, id, sizeof(n->id) - 1);
    strncpy(n->app_id, app_id, sizeof(n->app_id) - 1);
    if (tone) { strncpy(n->tone, tone, sizeof(n->tone) - 1); } else { strcpy(n->tone, "default"); }
    n->body = (uint8_t *)malloc(body_len);
    if (!n->body) return ESP_ERR_NO_MEM;
    memcpy(n->body, body, body_len);
    n->body_len = body_len;
    g.stats.queued++;
    inbox_persist_append(n);
    return ESP_OK;
}
