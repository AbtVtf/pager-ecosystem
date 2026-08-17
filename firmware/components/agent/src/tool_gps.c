// SPDX-License-Identifier: Apache-2.0
//
// GPS tool: gps_fix — returns the most recent NMEA fix.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "pageros_gps.h"

#include "agent_internal.h"

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

static esp_err_t gps_fix_fn(const char *args, char *out, size_t cap, void *ctx)
{
    (void)args; (void)ctx;
    pageros_gps_fix_t fix = {0};
    esp_err_t r = pageros_gps_get_last_fix(&fix);
    if (r != ESP_OK || !fix.valid) {
        return encode_err("no fix yet", out, cap);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o,   "valid",       true);
    cJSON_AddNumberToObject(o, "latitude",    fix.latitude_deg);
    cJSON_AddNumberToObject(o, "longitude",   fix.longitude_deg);
    if (!isnan(fix.altitude_m)) {
        cJSON_AddNumberToObject(o, "altitude_m", fix.altitude_m);
    }
    cJSON_AddNumberToObject(o, "accuracy_m",  fix.accuracy_m);
    cJSON_AddNumberToObject(o, "satellites",  fix.satellites);
    if (fix.utc_epoch_ms != 0) {
        cJSON_AddNumberToObject(o, "utc_epoch_ms", (double)fix.utc_epoch_ms);
    }
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s) return ESP_ERR_NO_MEM;
    strncpy(out, s, cap - 1); out[cap - 1] = '\0';
    free(s);
    return ESP_OK;
}

void agent_tools_register_gps(void)
{
    pageros_agent_tool_register(
        "gps_fix",
        "Get the current GPS fix from the u-blox MIA-M10Q. Returns "
        "{valid, latitude, longitude, altitude_m, accuracy_m, satellites, "
        "utc_epoch_ms}. Returns an error if no fix has been observed yet "
        "(the receiver may need several minutes outdoors to acquire one).",
        "{\"type\":\"object\",\"properties\":{}}",
        gps_fix_fn, NULL);
}
