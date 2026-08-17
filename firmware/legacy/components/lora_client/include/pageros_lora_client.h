// SPDX-License-Identifier: Apache-2.0
//
// PagerOS LoRa client — FW-026.
//
// Sits on top of the SX1262 driver (FW-008) and implements the device
// side of SPEC §6.2: outer envelope codec (LORA-001), fragmentation +
// reassembly (LORA-002), inner envelope build + encrypt (LORA-003).
// Together these expose a single request/response API:
//
//   pageros_lora_client_request(target_url, body, &response, ...);
//
// One call sends an encrypted CBOR inner envelope to the nearest exit
// node (the discovery layer FW-027 picks it), waits for the reassembled
// response, and returns it to the caller. Timeouts + retries match
// SPEC §6.4 (initial 30 s, 3 retries, 10/30/90 s backoff).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Wire constants from SPEC §6.2.1.
#define PAGEROS_LORA_MAGIC0     0x50  /* 'P' */
#define PAGEROS_LORA_MAGIC1     0x47  /* 'G' */
#define PAGEROS_LORA_VERSION    1
#define PAGEROS_LORA_HEADER_LEN 8     /* magic(2) + ver(1) + type(1) + msg_id(4) */
#define PAGEROS_LORA_MTU        256   /* SX1262 max packet payload */
#define PAGEROS_LORA_MAX_FRAG_PAYLOAD \
    (PAGEROS_LORA_MTU - PAGEROS_LORA_HEADER_LEN - 4 /* frag_id/total */)

typedef enum {
    PAGEROS_LORA_TYPE_REQUEST   = 0x01,
    PAGEROS_LORA_TYPE_RESPONSE  = 0x02,
    PAGEROS_LORA_TYPE_ACK       = 0x03,
    PAGEROS_LORA_TYPE_ADVERT    = 0x04,
} pageros_lora_type_t;

// Outer envelope (already-parsed view). Payload may be one fragment
// OR a complete inner-envelope payload depending on context.
typedef struct {
    uint8_t version;
    uint8_t type;
    uint32_t msg_id;
    const uint8_t *payload;
    size_t   payload_len;
} pageros_lora_envelope_t;

// Encode an outer envelope. Returns ESP_OK and writes the length into
// *out_len, or ESP_ERR_INVALID_SIZE if cap is too small.
esp_err_t pageros_lora_envelope_encode(uint8_t version,
                                       pageros_lora_type_t type,
                                       uint32_t msg_id,
                                       const uint8_t *payload, size_t payload_len,
                                       uint8_t *out, size_t cap, size_t *out_len);

// Decode an outer envelope from raw bytes. Returns ESP_OK on success.
// On unknown type, returns ESP_OK and the caller decides (per spec
// callers SHOULD drop unknown types). On bad magic returns ESP_ERR_INVALID_CRC.
esp_err_t pageros_lora_envelope_decode(const uint8_t *buf, size_t len,
                                       pageros_lora_envelope_t *out);

// Fragment a logical payload across N packets sized at MTU.
// Each fragment header is `[u16 frag_id][u16 total]` followed by data.
// Returns the number of fragments needed; if cap is < n*MTU returns
// ESP_ERR_INVALID_SIZE without writing.
esp_err_t pageros_lora_fragment(const uint8_t *payload, size_t payload_len,
                                uint8_t *out, size_t cap,
                                size_t *out_total_len, uint16_t *out_n_fragments);

// Reassemble a payload from `n_fragments` ordered fragment buffers.
// `fragments[i]` MUST start with `[u16 frag_id][u16 total]` matching i+1
// and total respectively; fragment_lens[i] is the data length INCLUDING
// the 4-byte header. Returns ESP_OK and writes into out + sets *out_len.
esp_err_t pageros_lora_reassemble(const uint8_t * const *fragments,
                                  const size_t *fragment_lens,
                                  uint16_t n_fragments,
                                  uint8_t *out, size_t cap, size_t *out_len);

// --- Exit Node discovery (FW-027) --------------------------------- //
//
// The SX1262 RX path forwards advertised packets here. Selection is
// best-RSSI-then-lowest-load. Entries time out after 60 s of silence.

void pageros_exit_discovery_on_packet(const uint8_t *payload, size_t len,
                                      int16_t rssi_dbm);
const uint8_t *pageros_exit_discovery_best(int16_t *out_rssi);
int  pageros_exit_discovery_count(void);
void pageros_exit_discovery_reset(void);

// High-level request/response. Builds outer + inner envelope, encrypts
// inner to `app_pubkey`, fragments, transmits via the SX1262, awaits
// the reassembled response. Returns ESP_OK + the decrypted response
// in *out_response (caller frees with free()).
//
// `target_url` is recorded in the inner envelope so the exit node knows
// where to forward; the device only sees the encrypted response.
esp_err_t pageros_lora_client_request(const char *target_url,
                                      const uint8_t *app_pubkey,   // 32 bytes
                                      const uint8_t *body, size_t body_len,
                                      uint8_t **out_response, size_t *out_len,
                                      uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
