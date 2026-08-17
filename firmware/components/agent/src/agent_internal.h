// SPDX-License-Identifier: Apache-2.0
//
// Private types shared between the agent component's translation units.
// Not exposed to other components.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#include "pageros_agent.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- config ------------------------------------------------------- //

typedef struct {
    char  api_key[256];
    char  base_url[128];     // e.g. https://openrouter.ai/api/v1
    char  model[96];
    char  system_prompt[1024];
    int   max_iterations;    // safety cap on tool-call loop
    int   request_timeout_ms;
    bool  loaded;
} agent_config_t;

esp_err_t agent_config_load(void);
const agent_config_t *agent_config_get(void);

// --- messages ----------------------------------------------------- //
//
// Append-only rolling history. The oldest non-system turns are dropped
// when the count grows past AGENT_MAX_MESSAGES.

#define AGENT_MAX_MESSAGES 32
#define AGENT_USER_INPUT_MAX 256

typedef struct {
    pageros_agent_role_t role;
    char *content;            // heap or NULL
    char *tool_calls_json;    // assistant turns with tool_calls — raw JSON array as a string
    char *tool_call_id;       // tool turns
    char *tool_name;          // tool turns (for UI label)
    char *tool_summary;       // assistant turns — single-line UI string ("wifi_scan, …")
    int64_t ts_us;
} agent_message_t;

void agent_messages_init(const char *system_prompt);
void agent_messages_reset_to_system(void);

// Returns the new message index, or -1 on out-of-memory.
int  agent_messages_append_user(const char *content);
int  agent_messages_append_assistant(const char *content,
                                     const char *tool_calls_json,
                                     const char *tool_summary);
int  agent_messages_append_tool(const char *tool_call_id,
                                const char *tool_name,
                                const char *content);

int  agent_messages_count(void);
const agent_message_t *agent_messages_get(int idx);

// Build the "messages" array for the API request, including the system
// prompt as messages[0]. Caller owns the returned cJSON; pass to
// cJSON_Delete when done. Returns NULL on OOM.
cJSON *agent_messages_to_json_array(void);

// --- tools -------------------------------------------------------- //

typedef struct {
    char name[32];
    const char *description;     // borrowed pointer
    const char *json_schema;     // borrowed pointer (string)
    pageros_agent_tool_fn_t fn;
    void *user_ctx;
} agent_tool_t;

#define AGENT_MAX_TOOLS 16
#define AGENT_TOOL_RESULT_MAX 2048

int  agent_tools_count(void);
const agent_tool_t *agent_tools_get(int idx);
const agent_tool_t *agent_tools_find(const char *name);

// Build the "tools" array for the API request. NULL if no tools
// registered. Caller deletes.
cJSON *agent_tools_to_json_array(void);

// --- http --------------------------------------------------------- //

typedef struct {
    char  *content;            // assistant text (heap), or NULL
    char  *tool_calls_json;    // raw JSON array as a string (heap), or NULL
    int    status_code;
    char   error[160];
} agent_http_result_t;

esp_err_t agent_http_complete(agent_http_result_t *out);
void      agent_http_result_free(agent_http_result_t *r);

// --- core --------------------------------------------------------- //

// Register the built-in tool set. Implemented in tools_register.c —
// fans out to per-group registration functions below.
void agent_register_builtin_tools(void);

// Per-group registration. Defined in tool_*.c.
void agent_tools_register_wifi(void);
void agent_tools_register_gps(void);
void agent_tools_register_lora(void);
void agent_tools_register_nfc(void);
void agent_tools_register_fs(void);
void agent_tools_register_net(void);
void agent_tools_register_sys(void);

// Emit an event to the observer (if installed). Safe to call from the
// inference task.
void agent_emit_event(pageros_agent_event_kind_t kind, const char *text);

// True while inference task is mid-loop.
void agent_set_busy(bool b);

#ifdef __cplusplus
}
#endif
