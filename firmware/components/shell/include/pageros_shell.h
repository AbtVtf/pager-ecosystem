// SPDX-License-Identifier: Apache-2.0
//
// PagerOS Shell — FW-029.
//
// Built-in app that renders the home screen, the app drawer, and the
// settings screens. Per SPEC §10.4 the Shell is conceptually a PagerOS
// app that lives at `market.pageros.org/`; for the v0 firmware path it
// has a bundled local fallback so the device boots to *something* even
// without internet + before any user apps are installed.
//
// API surface:
//
//   - pageros_shell_init() — call after apprt_init.
//   - pageros_shell_mount_home() — make the shell the foreground app
//     and render its home Frame.
//   - pageros_shell_emit_home(buf, cap, out_len) — emit the home Frame
//     as canonical CBOR into the caller's buffer. Used by the device
//     boot path + by tests.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAGEROS_SHELL_APP_ID  "pageros.shell"

esp_err_t pageros_shell_init(void);

// Emit the canonical "home" Frame. Returns ESP_OK on success and
// writes the length into *out_len. ESP_ERR_INVALID_SIZE if `cap` is
// too small.
esp_err_t pageros_shell_emit_home(uint8_t *buf, size_t cap, size_t *out_len);

// Convenience: emit + apprt_open in one call.
esp_err_t pageros_shell_mount_home(void);

#ifdef __cplusplus
}
#endif
