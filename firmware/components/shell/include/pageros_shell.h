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

// Settings menu Frame: Wi-Fi · Add app by URL · About · Back. Items
// have shell-internal hrefs ("shell:wifi", "shell:add-app", ...) the
// input handler interprets to mount the corresponding sub-screen.
esp_err_t pageros_shell_emit_settings(uint8_t *buf, size_t cap, size_t *out_len);

// Wi-Fi config Frame: status line, SSID input, password input,
// Connect button, Back button. `ssid` and `psk` populate the inputs'
// `value` fields so the renderer can show what the user has typed
// so far. `status` is a short string (e.g. "Connected" / "12 dBm" /
// "Bad PSK") rendered as a `text` widget.
esp_err_t pageros_shell_emit_wifi_config(uint8_t *buf, size_t cap, size_t *out_len,
                                         const char *ssid,
                                         const char *psk_masked,
                                         const char *status);

// Add-app-by-URL Frame: heading, URL input, Install button, Back,
// plus an optional error/success message rendered as a notification.
esp_err_t pageros_shell_emit_add_app(uint8_t *buf, size_t cap, size_t *out_len,
                                     const char *url,
                                     const char *message,
                                     const char *message_level);  // "info"|"warn"|"error"|NULL

#ifdef __cplusplus
}
#endif
