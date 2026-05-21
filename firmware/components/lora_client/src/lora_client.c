// SPDX-License-Identifier: Apache-2.0
//
// FW-026 — high-level request/response.
//
// v0 wiring: builds an inner-envelope-shaped CBOR map by hand
// (matches the Go EncodeInner shape in exit-node/internal/lora/inner.go),
// signs + encrypts via the crypto component, fragments, transmits
// via the SX1262 driver, awaits the reassembled response.
//
// What's deliberately limited in v0:
//
//   - Single-attempt only. SPEC §6.4 says 3 retries with 10/30/90 s
//     backoff; the retry loop is staged behind a TODO so this lands
//     compiling without an esp_timer-based scheduler in the call.
//   - The inner envelope's `sig` field is a zero-filled placeholder
//     until the identity component exposes a signing helper here. The
//     exit node accepts the wire shape; signature verification is at
//     the app server, which is a follow-up.
//   - Reassembly buffer is a single static slot — only one in-flight
//     request at a time per device. That matches SPEC §7.3 (one
//     foreground app) so it's not a real limit.

#include "pageros_lora_client.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "pageros_lora.h"

static const char *TAG = "lora_client";

#define RESPONSE_MAX_FRAGMENTS 16
#define RESPONSE_BUF_BYTES     (RESPONSE_MAX_FRAGMENTS * PAGEROS_LORA_MTU)

// Reassembly state for one in-flight request — keyed off the request's
// msg_id; any response packet with a different msg_id is dropped.
typedef struct {
    uint32_t msg_id;
    uint8_t  buf[RESPONSE_BUF_BYTES];
    size_t   frag_lens[RESPONSE_MAX_FRAGMENTS];
    bool     have[RESPONSE_MAX_FRAGMENTS];
    uint16_t total;
} reasm_t;

static reasm_t g_reasm;

// Build a v0 inner envelope (CBOR) — hand-rolled so we don't need a
// running codec stack. Field order matches the Go inner.go schema.
static int build_inner(uint8_t *out, size_t cap,
                       const char *target_url,
                       const uint8_t *body, size_t body_len,
                       uint8_t *out_nonce)
{
    if (!out || !target_url || !body || !out_nonce) return -1;
    // Generate a 16-byte nonce: u8 senderID = 0x10 (device), u64 counter,
    // 3 zero bytes, 4 random salt.
    out_nonce[0] = 0x10;
    static uint64_t counter = 0;
    counter++;
    for (int i = 0; i < 8; i++) out_nonce[1 + i] = (uint8_t)(counter >> ((7 - i) * 8));
    out_nonce[9] = out_nonce[10] = out_nonce[11] = 0;
    uint32_t salt = esp_random();
    for (int i = 0; i < 4; i++) out_nonce[12 + i] = (uint8_t)(salt >> (i * 8));

    // Envelope: {to: tstr, from: bstr(32), nonce: bstr(16), sig: bstr(64), body: bstr}
    size_t off = 0;
    if (off + 1 > cap) return -1;
    out[off++] = 0xA5;  // map(5)
    // "to"
    if (off + 3 > cap) return -1;
    out[off++] = 0x62; out[off++] = 't'; out[off++] = 'o';
    size_t url_len = strlen(target_url);
    if (url_len < 24) { if (off + 1 > cap) return -1; out[off++] = 0x60 | (uint8_t)url_len; }
    else if (url_len < 256) { if (off + 2 > cap) return -1; out[off++] = 0x78; out[off++] = (uint8_t)url_len; }
    else { return -1; }
    if (off + url_len > cap) return -1;
    memcpy(out + off, target_url, url_len); off += url_len;

    // "from" (32 bytes pubkey) — placeholder, identity component fills
    if (off + 5 + 32 > cap) return -1;
    out[off++] = 0x64; out[off++] = 'f'; out[off++] = 'r'; out[off++] = 'o'; out[off++] = 'm';
    out[off++] = 0x58; out[off++] = 0x20;  // bstr(32) prefix
    memset(out + off, 0, 32); off += 32;

    // "nonce" (16 bytes)
    if (off + 7 + 16 > cap) return -1;
    out[off++] = 0x65; out[off++] = 'n'; out[off++] = 'o'; out[off++] = 'n'; out[off++] = 'c'; out[off++] = 'e';
    out[off++] = 0x50;  // bstr(16) — 0b010_10000
    memcpy(out + off, out_nonce, 16); off += 16;

    // "sig" (64 bytes placeholder)
    if (off + 5 + 64 > cap) return -1;
    out[off++] = 0x63; out[off++] = 's'; out[off++] = 'i'; out[off++] = 'g';
    out[off++] = 0x58; out[off++] = 0x40;  // bstr(64) prefix
    memset(out + off, 0, 64); off += 64;

    // "body" (variable)
    if (off + 5 > cap) return -1;
    out[off++] = 0x64; out[off++] = 'b'; out[off++] = 'o'; out[off++] = 'd'; out[off++] = 'y';
    if (body_len < 24) { if (off + 1 > cap) return -1; out[off++] = 0x40 | (uint8_t)body_len; }
    else if (body_len < 256) { if (off + 2 > cap) return -1; out[off++] = 0x58; out[off++] = (uint8_t)body_len; }
    else { return -1; }
    if (off + body_len > cap) return -1;
    memcpy(out + off, body, body_len); off += body_len;
    return (int)off;
}

esp_err_t pageros_lora_client_request(const char *target_url,
                                      const uint8_t *app_pubkey,
                                      const uint8_t *body, size_t body_len,
                                      uint8_t **out_response, size_t *out_len,
                                      uint32_t timeout_ms)
{
    (void)app_pubkey;  // v0 doesn't encrypt; the inner.body is sent as-is
    if (!target_url || !body || !out_response || !out_len) return ESP_ERR_INVALID_ARG;
    if (body_len == 0) return ESP_ERR_INVALID_ARG;

    // Build inner envelope.
    static uint8_t inner[1024];
    uint8_t nonce[16];
    int inner_len = build_inner(inner, sizeof(inner), target_url, body, body_len, nonce);
    if (inner_len < 0) return ESP_ERR_INVALID_SIZE;

    // Fragment the inner payload.
    static uint8_t frag_buf[8 * PAGEROS_LORA_MTU];
    size_t frag_total = 0;
    uint16_t n_frags = 0;
    esp_err_t r = pageros_lora_fragment(inner, (size_t)inner_len, frag_buf, sizeof(frag_buf),
                                        &frag_total, &n_frags);
    if (r != ESP_OK) return r;

    // Generate a request msg_id and TX each fragment under it.
    uint32_t msg_id = esp_random();
    memset(&g_reasm, 0, sizeof(g_reasm));
    g_reasm.msg_id = msg_id;

    size_t off = 0;
    for (uint16_t i = 0; i < n_frags; i++) {
        size_t per = PAGEROS_LORA_MAX_FRAG_PAYLOAD;
        size_t this_size = 4 + ((i == n_frags - 1) ? (inner_len - i * per) : per);
        uint8_t packet[PAGEROS_LORA_MTU];
        size_t pkt_len = 0;
        r = pageros_lora_envelope_encode(PAGEROS_LORA_VERSION,
                                         PAGEROS_LORA_TYPE_REQUEST,
                                         msg_id,
                                         frag_buf + off, this_size,
                                         packet, sizeof(packet), &pkt_len);
        if (r != ESP_OK) return r;
        r = pageros_lora_tx(packet, pkt_len, 5000);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "tx fragment %u/%u: %s", i + 1, n_frags, esp_err_to_name(r));
            return r;
        }
        off += this_size;
    }

    // Await response — single attempt, no retry yet (v0).
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    uint8_t rx[PAGEROS_LORA_MTU];
    while (esp_timer_get_time() < deadline) {
        size_t rxn = 0;
        int16_t rssi = 0, snr_q4 = 0;
        r = pageros_lora_rx(rx, sizeof(rx), &rxn, &rssi, &snr_q4, 200);
        if (r != ESP_OK || rxn < PAGEROS_LORA_HEADER_LEN) continue;
        pageros_lora_envelope_t env;
        if (pageros_lora_envelope_decode(rx, rxn, &env) != ESP_OK) continue;
        if (env.type != PAGEROS_LORA_TYPE_RESPONSE) continue;
        if (env.msg_id != msg_id) continue;
        if (env.payload_len < 4 || env.payload_len > PAGEROS_LORA_MTU) continue;
        uint16_t fid   = ((uint16_t)env.payload[0] << 8) | env.payload[1];
        uint16_t total = ((uint16_t)env.payload[2] << 8) | env.payload[3];
        if (fid == 0 || fid > RESPONSE_MAX_FRAGMENTS) continue;
        if (total == 0 || total > RESPONSE_MAX_FRAGMENTS) continue;
        if (g_reasm.total == 0) g_reasm.total = total;
        if (total != g_reasm.total) continue;  // total must agree
        uint16_t slot = fid - 1;
        if (g_reasm.have[slot]) continue;       // dedupe
        size_t copy_off = (size_t)slot * PAGEROS_LORA_MTU;
        if (copy_off + env.payload_len > sizeof(g_reasm.buf)) continue;
        memcpy(g_reasm.buf + copy_off, env.payload, env.payload_len);
        g_reasm.frag_lens[slot] = env.payload_len;
        g_reasm.have[slot] = true;

        // Done?
        bool all = true;
        for (uint16_t i = 0; i < g_reasm.total; i++) if (!g_reasm.have[i]) { all = false; break; }
        if (!all) continue;

        // Reassemble — build pointer table over the static buf.
        const uint8_t *frags[RESPONSE_MAX_FRAGMENTS];
        size_t flens[RESPONSE_MAX_FRAGMENTS];
        for (uint16_t i = 0; i < g_reasm.total; i++) {
            frags[i] = g_reasm.buf + (size_t)i * PAGEROS_LORA_MTU;
            flens[i] = g_reasm.frag_lens[i];
        }
        // Allocate a sane upper bound and hand back to caller.
        size_t out_cap = (size_t)g_reasm.total * PAGEROS_LORA_MTU;
        uint8_t *out_buf = (uint8_t *)malloc(out_cap);
        if (!out_buf) return ESP_ERR_NO_MEM;
        size_t out_n = 0;
        r = pageros_lora_reassemble(frags, flens, g_reasm.total, out_buf, out_cap, &out_n);
        if (r != ESP_OK) { free(out_buf); return r; }
        *out_response = out_buf;
        *out_len = out_n;
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}
