// SPDX-License-Identifier: Apache-2.0
//
// PagerOS persistent storage — FW-003.
//
// Brings up the microSD card on the shared SPI bus and mounts it as a
// FAT filesystem at `/sd`. On first boot, creates the directory tree
// from SPEC.md §7.4 if it doesn't yet exist:
//
//   /system/{fonts,tones}
//   /cache/{frames,images,tiles}
//   /apps
//   /notifications
//   /logs
//
// Hardware (LILYGO T-LoRa Pager, ESP32-S3):
//   - SD over SPI; MOSI=34, MISO=33, SCK=35, CS=21 (shared bus with
//     LoRa, NFC, and display — bus init is idempotent here).
//   - SD power gate: XL9555 expander pin P1.4 (EXPANDS_SD_EN, bit 12)
//     must be driven high before the card responds.
//   - Card detect:   XL9555 expander pin P1.2 (EXPANDS_SD_DET, bit 10).
//
// The shell's "recovery without SD" UX is out of scope here; if the
// card is absent we return ESP_ERR_NOT_FOUND and the caller (main)
// logs and continues so the rest of the device still boots — same
// pattern as the GPS driver.

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Mount point of the SD card filesystem. All `pageros_storage_path`
// outputs are absolute paths under this prefix.
#define PAGEROS_SD_MOUNT  "/sd"

// PagerOS directories on the SD card (SPEC.md §7.4). Use these
// constants rather than hard-coding strings so any future relayout is
// a single-file change.
#define PAGEROS_DIR_SYSTEM         PAGEROS_SD_MOUNT "/system"
#define PAGEROS_DIR_FONTS          PAGEROS_SD_MOUNT "/system/fonts"
#define PAGEROS_DIR_TONES          PAGEROS_SD_MOUNT "/system/tones"
#define PAGEROS_DIR_CACHE          PAGEROS_SD_MOUNT "/cache"
#define PAGEROS_DIR_CACHE_FRAMES   PAGEROS_SD_MOUNT "/cache/frames"
#define PAGEROS_DIR_CACHE_IMAGES   PAGEROS_SD_MOUNT "/cache/images"
#define PAGEROS_DIR_CACHE_TILES    PAGEROS_SD_MOUNT "/cache/tiles"
#define PAGEROS_DIR_APPS           PAGEROS_SD_MOUNT "/apps"
#define PAGEROS_DIR_NOTIFICATIONS  PAGEROS_SD_MOUNT "/notifications"
#define PAGEROS_DIR_LOGS           PAGEROS_SD_MOUNT "/logs"

// Initialise the SPI bus (if not already), power the SD slot via the
// XL9555 expander, mount the card at `/sd`, and create the SPEC §7.4
// directory tree if any of the directories are missing.
//
// Returns:
//   ESP_OK              — card mounted and directory tree present
//   ESP_ERR_NOT_FOUND   — no card detected
//   ESP_ERR_INVALID_STATE — XL9555 / SPI bring-up failed before mount
//   other esp_err_t      — surfaced from esp_vfs_fat_sdspi_mount
esp_err_t pageros_storage_init(void);

// Tear the mount down and re-power-gate the SD slot. Mainly for tests
// and low-power flows.
esp_err_t pageros_storage_shutdown(void);

// True if `pageros_storage_init` has reported success since the last
// `shutdown` / boot.
bool pageros_storage_is_mounted(void);

// Recursively create directory `path` with mkdir-p semantics (intermediate
// directories are created as needed; an existing directory is not an
// error). Exposed so other components (cache writers, log rotators) can
// lazily materialise their own subtrees without depending on mkdir.
esp_err_t pageros_storage_mkdir_p(const char *path);

#ifdef __cplusplus
}
#endif
