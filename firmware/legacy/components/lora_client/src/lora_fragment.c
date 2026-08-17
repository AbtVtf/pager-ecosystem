// SPDX-License-Identifier: Apache-2.0
// FW-026 — fragmentation + reassembly (LORA-002).

#include "pageros_lora_client.h"

#include <string.h>

esp_err_t pageros_lora_fragment(const uint8_t *payload, size_t payload_len,
                                uint8_t *out, size_t cap,
                                size_t *out_total_len, uint16_t *out_n_fragments)
{
    if (!payload || !out || !out_total_len || !out_n_fragments) return ESP_ERR_INVALID_ARG;
    if (payload_len == 0) return ESP_ERR_INVALID_ARG;
    size_t per = PAGEROS_LORA_MAX_FRAG_PAYLOAD;
    size_t n = (payload_len + per - 1) / per;
    if (n > 0xFFFF) return ESP_ERR_INVALID_SIZE;
    size_t total_size = 0;
    for (size_t i = 0; i < n; i++) {
        size_t this_data = (i == n - 1) ? (payload_len - i * per) : per;
        total_size += 4 + this_data;
    }
    if (cap < total_size) return ESP_ERR_INVALID_SIZE;

    size_t off = 0;
    for (size_t i = 0; i < n; i++) {
        size_t this_data = (i == n - 1) ? (payload_len - i * per) : per;
        out[off++] = (uint8_t)((i + 1) >> 8);
        out[off++] = (uint8_t)(i + 1);
        out[off++] = (uint8_t)(n >> 8);
        out[off++] = (uint8_t)(n);
        memcpy(out + off, payload + i * per, this_data);
        off += this_data;
    }
    *out_total_len = off;
    *out_n_fragments = (uint16_t)n;
    return ESP_OK;
}

esp_err_t pageros_lora_reassemble(const uint8_t * const *fragments,
                                  const size_t *fragment_lens,
                                  uint16_t n_fragments,
                                  uint8_t *out, size_t cap, size_t *out_len)
{
    if (!fragments || !fragment_lens || !out || !out_len) return ESP_ERR_INVALID_ARG;
    if (n_fragments == 0) return ESP_ERR_INVALID_ARG;
    size_t out_off = 0;
    for (uint16_t i = 0; i < n_fragments; i++) {
        const uint8_t *f = fragments[i];
        size_t flen = fragment_lens[i];
        if (flen < 4) return ESP_ERR_INVALID_SIZE;
        uint16_t frag_id = ((uint16_t)f[0] << 8) | f[1];
        uint16_t total   = ((uint16_t)f[2] << 8) | f[3];
        if (frag_id != i + 1 || total != n_fragments) return ESP_ERR_INVALID_RESPONSE;
        size_t data_len = flen - 4;
        if (out_off + data_len > cap) return ESP_ERR_INVALID_SIZE;
        memcpy(out + out_off, f + 4, data_len);
        out_off += data_len;
    }
    *out_len = out_off;
    return ESP_OK;
}
