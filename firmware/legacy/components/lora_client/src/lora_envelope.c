// SPDX-License-Identifier: Apache-2.0
// FW-026 — outer envelope codec (LORA-001).

#include "pageros_lora_client.h"

#include <string.h>

esp_err_t pageros_lora_envelope_encode(uint8_t version,
                                       pageros_lora_type_t type,
                                       uint32_t msg_id,
                                       const uint8_t *payload, size_t payload_len,
                                       uint8_t *out, size_t cap, size_t *out_len)
{
    if (!out || !out_len) return ESP_ERR_INVALID_ARG;
    if (version == 0) return ESP_ERR_INVALID_ARG;
    if (cap < PAGEROS_LORA_HEADER_LEN + payload_len) return ESP_ERR_INVALID_SIZE;
    out[0] = PAGEROS_LORA_MAGIC0;
    out[1] = PAGEROS_LORA_MAGIC1;
    out[2] = version;
    out[3] = (uint8_t)type;
    out[4] = (uint8_t)(msg_id >> 24);
    out[5] = (uint8_t)(msg_id >> 16);
    out[6] = (uint8_t)(msg_id >> 8);
    out[7] = (uint8_t)msg_id;
    if (payload && payload_len) memcpy(out + PAGEROS_LORA_HEADER_LEN, payload, payload_len);
    *out_len = PAGEROS_LORA_HEADER_LEN + payload_len;
    return ESP_OK;
}

esp_err_t pageros_lora_envelope_decode(const uint8_t *buf, size_t len,
                                       pageros_lora_envelope_t *out)
{
    if (!buf || !out) return ESP_ERR_INVALID_ARG;
    if (len < PAGEROS_LORA_HEADER_LEN) return ESP_ERR_INVALID_SIZE;
    if (buf[0] != PAGEROS_LORA_MAGIC0 || buf[1] != PAGEROS_LORA_MAGIC1) {
        return ESP_ERR_INVALID_CRC;
    }
    out->version = buf[2];
    out->type    = buf[3];
    out->msg_id  = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16)
                 | ((uint32_t)buf[6] <<  8) |  (uint32_t)buf[7];
    out->payload     = (len > PAGEROS_LORA_HEADER_LEN) ? buf + PAGEROS_LORA_HEADER_LEN : NULL;
    out->payload_len = len - PAGEROS_LORA_HEADER_LEN;
    return ESP_OK;
}
