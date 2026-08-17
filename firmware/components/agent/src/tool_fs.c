// SPDX-License-Identifier: Apache-2.0
//
// Filesystem tools: ls / read / write / mkdir on the SD card.
//
// Safety: writes and mkdirs are refused under /sd/system/ (font/tone
// blobs) so the agent can't accidentally brick the device. Reads have
// no such restriction.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"

#include "pageros_storage.h"

#include "agent_internal.h"

#define FS_READ_MAX_BYTES   4096
#define FS_WRITE_MAX_BYTES  4096

static esp_err_t encode_err(const char *msg, char *out, size_t cap)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg ? msg : "unknown");
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s) return ESP_ERR_NO_MEM;
    strncpy(out, s, cap - 1); out[cap - 1] = '\0';
    free(s);
    return ESP_OK;
}

static esp_err_t encode_ok(cJSON *obj, char *out, size_t cap)
{
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!s) return ESP_ERR_NO_MEM;
    strncpy(out, s, cap - 1); out[cap - 1] = '\0';
    free(s);
    return ESP_OK;
}

// Allow reads anywhere under /sd/. Block writes/mkdirs under /sd/system.
static bool path_is_under_sd(const char *p)
{
    return p && strncmp(p, "/sd/", 4) == 0;
}

static bool path_is_writable(const char *p)
{
    if (!path_is_under_sd(p)) return false;
    if (strncmp(p, "/sd/system", 10) == 0) return false;
    return true;
}

// --- fs_ls ------------------------------------------------------- //

static esp_err_t fs_ls_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(args ? args : "{}");
    const cJSON *p = root ? cJSON_GetObjectItemCaseSensitive(root, "path") : NULL;
    const char *path = (cJSON_IsString(p) && p->valuestring) ? p->valuestring : "/sd";
    if (!path_is_under_sd(path) && strcmp(path, "/sd") != 0) {
        if (root) cJSON_Delete(root);
        return encode_err("path must be under /sd", out, cap);
    }
    DIR *d = opendir(path);
    if (!d) {
        if (root) cJSON_Delete(root);
        return encode_err("opendir failed", out, cap);
    }
    cJSON *arr = cJSON_CreateArray();
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", e->d_name);

        char full[320];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            cJSON_AddBoolToObject(entry,   "is_dir", S_ISDIR(st.st_mode));
            cJSON_AddNumberToObject(entry, "size",   (double)st.st_size);
        }
        cJSON_AddItemToArray(arr, entry);
    }
    closedir(d);
    if (root) cJSON_Delete(root);
    return encode_ok(arr, out, cap);
}

// --- fs_read ----------------------------------------------------- //

static esp_err_t fs_read_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(args ? args : "{}");
    if (!root) return encode_err("invalid args", out, cap);
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "path");
    if (!cJSON_IsString(p) || !path_is_under_sd(p->valuestring)) {
        cJSON_Delete(root);
        return encode_err("path under /sd required", out, cap);
    }
    FILE *f = fopen(p->valuestring, "rb");
    if (!f) { cJSON_Delete(root); return encode_err("open failed", out, cap); }
    char buf[FS_READ_MAX_BYTES + 1];
    size_t n = fread(buf, 1, FS_READ_MAX_BYTES, f);
    bool truncated = !feof(f);
    fclose(f);
    buf[n] = '\0';
    // Treat as text — non-printable bytes are replaced with '.'.
    for (size_t i = 0; i < n; i++) {
        unsigned char b = (unsigned char)buf[i];
        if (b != '\n' && b != '\r' && b != '\t' && (b < 0x20 || b >= 0x7f)) {
            buf[i] = '.';
        }
    }
    cJSON_Delete(root);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o,   "ok",        true);
    cJSON_AddStringToObject(o, "content",   buf);
    cJSON_AddNumberToObject(o, "bytes",     (int)n);
    cJSON_AddBoolToObject(o,   "truncated", truncated);
    return encode_ok(o, out, cap);
}

// --- fs_write ---------------------------------------------------- //

static esp_err_t fs_write_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(args ? args : "{}");
    if (!root) return encode_err("invalid args", out, cap);
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "path");
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "content");
    if (!cJSON_IsString(p) || !cJSON_IsString(c)) {
        cJSON_Delete(root);
        return encode_err("path + content required", out, cap);
    }
    if (!path_is_writable(p->valuestring)) {
        cJSON_Delete(root);
        return encode_err("path not writable (must be under /sd/ but not /sd/system/)", out, cap);
    }
    size_t len = strlen(c->valuestring);
    if (len > FS_WRITE_MAX_BYTES) {
        cJSON_Delete(root);
        return encode_err("content too large (max 4096 bytes)", out, cap);
    }
    FILE *f = fopen(p->valuestring, "wb");
    if (!f) { cJSON_Delete(root); return encode_err("open failed", out, cap); }
    fwrite(c->valuestring, 1, len, f);
    fclose(f);
    cJSON_Delete(root);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o,   "ok",    true);
    cJSON_AddNumberToObject(o, "bytes", (int)len);
    return encode_ok(o, out, cap);
}

// --- fs_mkdir ---------------------------------------------------- //

static esp_err_t fs_mkdir_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(args ? args : "{}");
    if (!root) return encode_err("invalid args", out, cap);
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "path");
    if (!cJSON_IsString(p) || !path_is_writable(p->valuestring)) {
        cJSON_Delete(root);
        return encode_err("writable path required", out, cap);
    }
    esp_err_t r = pageros_storage_mkdir_p(p->valuestring);
    cJSON_Delete(root);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o,   "ok",     r == ESP_OK);
    cJSON_AddStringToObject(o, "status", esp_err_to_name(r));
    return encode_ok(o, out, cap);
}

void agent_tools_register_fs(void)
{
    pageros_agent_tool_register(
        "fs_ls",
        "List files in a directory on the SD card. Returns an array of "
        "{name, is_dir, size}. Default path is /sd.",
        "{\"type\":\"object\","
         "\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"absolute path under /sd\"}"
         "}}",
        fs_ls_fn, NULL);

    pageros_agent_tool_register(
        "fs_read",
        "Read up to 4096 bytes from a file on the SD card. Non-printable "
        "bytes are replaced with '.'. Returns {ok, content, bytes, truncated}.",
        "{\"type\":\"object\","
         "\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"absolute path under /sd\"}"
         "},"
         "\"required\":[\"path\"]}",
        fs_read_fn, NULL);

    pageros_agent_tool_register(
        "fs_write",
        "Write text content to a file on the SD card (up to 4096 bytes). "
        "Refuses paths under /sd/system/. Overwrites if the file exists.",
        "{\"type\":\"object\","
         "\"properties\":{"
            "\"path\":{\"type\":\"string\"},"
            "\"content\":{\"type\":\"string\"}"
         "},"
         "\"required\":[\"path\",\"content\"]}",
        fs_write_fn, NULL);

    pageros_agent_tool_register(
        "fs_mkdir",
        "Create a directory (with parents) on the SD card. Refuses paths "
        "under /sd/system/.",
        "{\"type\":\"object\","
         "\"properties\":{"
            "\"path\":{\"type\":\"string\"}"
         "},"
         "\"required\":[\"path\"]}",
        fs_mkdir_fn, NULL);
}
