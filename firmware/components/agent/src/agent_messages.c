// SPDX-License-Identifier: Apache-2.0
//
// Rolling conversation history. Slot 0 is the system prompt and is
// preserved across resets and compaction. When the history overflows
// AGENT_MAX_MESSAGES, we drop the oldest non-system turn pairs
// (assistant + the tool replies that immediately follow it, or
// user + assistant). The compaction is intentionally simple: this is
// a tiny ESP32, not a frontier-model context window.

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "cJSON.h"

#include "agent_internal.h"

static const char *TAG = "agent_msg";

static agent_message_t g_msgs[AGENT_MAX_MESSAGES];
static int g_count = 0;

static char *dupstr(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n + 1);
    return out;
}

static void free_message(agent_message_t *m)
{
    free(m->content);          m->content = NULL;
    free(m->tool_calls_json);  m->tool_calls_json = NULL;
    free(m->tool_call_id);     m->tool_call_id = NULL;
    free(m->tool_name);        m->tool_name = NULL;
    free(m->tool_summary);     m->tool_summary = NULL;
}

void agent_messages_init(const char *system_prompt)
{
    for (int i = 0; i < g_count; i++) free_message(&g_msgs[i]);
    memset(g_msgs, 0, sizeof(g_msgs));
    g_msgs[0].role    = PAGEROS_AGENT_ROLE_SYSTEM;
    g_msgs[0].content = dupstr(system_prompt ? system_prompt : "");
    g_msgs[0].ts_us   = esp_timer_get_time();
    g_count = 1;
}

void agent_messages_reset_to_system(void)
{
    if (g_count <= 1) return;
    for (int i = 1; i < g_count; i++) free_message(&g_msgs[i]);
    memset(&g_msgs[1], 0, sizeof(g_msgs[0]) * (AGENT_MAX_MESSAGES - 1));
    g_count = 1;
}

// Drop the oldest non-system message to make room. If a dropped slot is
// an assistant turn that has tool_calls, also drop any directly-following
// tool turns so the next assistant message doesn't reference an unbacked
// tool_call_id.
static void compact_one(void)
{
    if (g_count <= 1) return;
    int drop_from = 1;
    int drop_to   = 2;
    // If we're dropping an assistant w/ tool_calls, also drop the tools.
    if (g_msgs[1].role == PAGEROS_AGENT_ROLE_ASSISTANT &&
        g_msgs[1].tool_calls_json != NULL) {
        while (drop_to < g_count &&
               g_msgs[drop_to].role == PAGEROS_AGENT_ROLE_TOOL) {
            drop_to++;
        }
    }
    int span = drop_to - drop_from;
    for (int i = drop_from; i < drop_to; i++) free_message(&g_msgs[i]);
    memmove(&g_msgs[drop_from], &g_msgs[drop_to],
            sizeof(g_msgs[0]) * (g_count - drop_to));
    memset(&g_msgs[g_count - span], 0, sizeof(g_msgs[0]) * span);
    g_count -= span;
    ESP_LOGD(TAG, "compacted: dropped %d turn(s), count=%d", span, g_count);
}

static int alloc_slot(void)
{
    while (g_count >= AGENT_MAX_MESSAGES) compact_one();
    return g_count++;
}

int agent_messages_append_user(const char *content)
{
    int i = alloc_slot();
    g_msgs[i].role    = PAGEROS_AGENT_ROLE_USER;
    g_msgs[i].content = dupstr(content);
    g_msgs[i].ts_us   = esp_timer_get_time();
    return i;
}

int agent_messages_append_assistant(const char *content,
                                    const char *tool_calls_json,
                                    const char *tool_summary)
{
    int i = alloc_slot();
    g_msgs[i].role             = PAGEROS_AGENT_ROLE_ASSISTANT;
    g_msgs[i].content          = dupstr(content);
    g_msgs[i].tool_calls_json  = dupstr(tool_calls_json);
    g_msgs[i].tool_summary     = dupstr(tool_summary);
    g_msgs[i].ts_us            = esp_timer_get_time();
    return i;
}

int agent_messages_append_tool(const char *tool_call_id,
                               const char *tool_name,
                               const char *content)
{
    int i = alloc_slot();
    g_msgs[i].role         = PAGEROS_AGENT_ROLE_TOOL;
    g_msgs[i].tool_call_id = dupstr(tool_call_id);
    g_msgs[i].tool_name    = dupstr(tool_name);
    g_msgs[i].content      = dupstr(content);
    g_msgs[i].ts_us        = esp_timer_get_time();
    return i;
}

int agent_messages_count(void) { return g_count; }

const agent_message_t *agent_messages_get(int idx)
{
    if (idx < 0 || idx >= g_count) return NULL;
    return &g_msgs[idx];
}

static const char *role_name(pageros_agent_role_t r)
{
    switch (r) {
    case PAGEROS_AGENT_ROLE_SYSTEM:    return "system";
    case PAGEROS_AGENT_ROLE_USER:      return "user";
    case PAGEROS_AGENT_ROLE_ASSISTANT: return "assistant";
    case PAGEROS_AGENT_ROLE_TOOL:      return "tool";
    }
    return "user";
}

cJSON *agent_messages_to_json_array(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    for (int i = 0; i < g_count; i++) {
        const agent_message_t *m = &g_msgs[i];
        cJSON *obj = cJSON_CreateObject();
        if (!obj) { cJSON_Delete(arr); return NULL; }
        cJSON_AddStringToObject(obj, "role", role_name(m->role));

        if (m->role == PAGEROS_AGENT_ROLE_TOOL) {
            if (m->tool_call_id) {
                cJSON_AddStringToObject(obj, "tool_call_id", m->tool_call_id);
            }
            if (m->tool_name) {
                cJSON_AddStringToObject(obj, "name", m->tool_name);
            }
            cJSON_AddStringToObject(obj, "content",
                                    m->content ? m->content : "");
        } else if (m->role == PAGEROS_AGENT_ROLE_ASSISTANT) {
            // Content may legitimately be empty when tool_calls is present.
            cJSON_AddStringToObject(obj, "content",
                                    m->content ? m->content : "");
            if (m->tool_calls_json && m->tool_calls_json[0]) {
                cJSON *tc = cJSON_Parse(m->tool_calls_json);
                if (tc) {
                    cJSON_AddItemToObject(obj, "tool_calls", tc);
                }
            }
        } else {
            cJSON_AddStringToObject(obj, "content",
                                    m->content ? m->content : "");
        }
        cJSON_AddItemToArray(arr, obj);
    }
    return arr;
}
