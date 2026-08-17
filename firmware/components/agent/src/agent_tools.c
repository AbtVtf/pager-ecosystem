// SPDX-License-Identifier: Apache-2.0
//
// Tool registry + JSON-schema serialization for the agent harness.
// Tools register themselves at init (see tool_wifi_scan.c). The
// inference loop calls agent_tools_to_json_array() to build the
// "tools" field of the API request, and agent_tools_find() +
// .fn(args, buf, cap, ctx) to dispatch calls.

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "cJSON.h"

#include "agent_internal.h"

static const char *TAG = "agent_tools";

static agent_tool_t g_tools[AGENT_MAX_TOOLS];
static int g_count = 0;

esp_err_t pageros_agent_tool_register(const char *name,
                                       const char *description,
                                       const char *json_schema,
                                       pageros_agent_tool_fn_t fn,
                                       void *user_ctx)
{
    if (!name || !*name || !fn) return ESP_ERR_INVALID_ARG;
    if (g_count >= AGENT_MAX_TOOLS) return ESP_ERR_NO_MEM;
    // Don't double-register.
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_tools[i].name, name) == 0) {
            ESP_LOGW(TAG, "tool %s already registered", name);
            return ESP_ERR_INVALID_STATE;
        }
    }
    agent_tool_t *t = &g_tools[g_count];
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
    t->description = description ? description : "";
    t->json_schema = json_schema ? json_schema
                                 : "{\"type\":\"object\",\"properties\":{}}";
    t->fn          = fn;
    t->user_ctx    = user_ctx;
    g_count++;
    ESP_LOGI(TAG, "registered %s (%d/%d)", name, g_count, AGENT_MAX_TOOLS);
    return ESP_OK;
}

int agent_tools_count(void) { return g_count; }

const agent_tool_t *agent_tools_get(int idx)
{
    if (idx < 0 || idx >= g_count) return NULL;
    return &g_tools[idx];
}

const agent_tool_t *agent_tools_find(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_tools[i].name, name) == 0) return &g_tools[i];
    }
    return NULL;
}

cJSON *agent_tools_to_json_array(void)
{
    if (g_count == 0) return NULL;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    for (int i = 0; i < g_count; i++) {
        const agent_tool_t *t = &g_tools[i];
        cJSON *entry = cJSON_CreateObject();
        cJSON *fn    = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "type", "function");
        cJSON_AddStringToObject(fn, "name",        t->name);
        cJSON_AddStringToObject(fn, "description", t->description);
        // Schema is a JSON string — parse it back into a real object
        // so the API sees structured data, not a stringified blob.
        cJSON *params = cJSON_Parse(t->json_schema);
        if (!params) {
            params = cJSON_CreateObject();
            cJSON_AddStringToObject(params, "type", "object");
        }
        cJSON_AddItemToObject(fn, "parameters", params);
        cJSON_AddItemToObject(entry, "function", fn);
        cJSON_AddItemToArray(arr, entry);
    }
    return arr;
}
