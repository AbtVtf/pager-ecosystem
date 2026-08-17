// SPDX-License-Identifier: Apache-2.0
//
// PagerOS on-device AI agent harness.
//
// A conversational tool-use loop. The user submits text; a background
// FreeRTOS task POSTs the rolling transcript to an OpenAI-compatible
// `/chat/completions` endpoint (default: OpenRouter), then either
// returns plaintext to the UI or dispatches tool calls into the
// registry and feeds the results back to the model until it produces a
// terminal text reply.
//
// Config (api key, model, system prompt) is loaded from
// `/sd/agent/config.json` on init. Tools register themselves with
// `pageros_agent_tool_register()` — see `tool_wifi_scan.c` for the
// canonical example.
//
// All state is owned by the agent component; the shell consumes
// transcript turns via the read API and reacts to runtime events via
// an installed observer callback.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- lifecycle ----------------------------------------------------- //

// Load `/sd/agent/config.json`, register the built-in tool set, spawn
// the inference task. Safe to call more than once — subsequent calls
// reload the config file but do not respawn the task. Returns
// ESP_ERR_NOT_FOUND when the SD card has no config file (the UI can
// still surface a "no config" state to the user).
esp_err_t pageros_agent_init(void);

// True once init has succeeded and a config is loaded.
bool pageros_agent_is_ready(void);

// --- transcript ---------------------------------------------------- //

typedef enum {
    PAGEROS_AGENT_ROLE_SYSTEM = 0,
    PAGEROS_AGENT_ROLE_USER,
    PAGEROS_AGENT_ROLE_ASSISTANT,   // may carry tool_calls instead of/plus content
    PAGEROS_AGENT_ROLE_TOOL,        // result of a single tool call
} pageros_agent_role_t;

typedef struct {
    pageros_agent_role_t role;
    const char *content;        // plain text (NULL if the assistant only emitted tool_calls)
    const char *tool_summary;   // human-readable single-line summary, e.g. "wifi_scan -> 8 APs"
    int64_t ts_us;
} pageros_agent_turn_t;

// Number of turns currently in the transcript. Includes the system
// prompt as turn 0.
int pageros_agent_transcript_count(void);

// Fill `out` with turn `idx`. Returns false on out-of-range or before
// init. The pointers inside `out` are valid until the next mutation of
// the transcript (i.e. the next submit / tool result / reset).
bool pageros_agent_transcript_get(int idx, pageros_agent_turn_t *out);

// Drop every turn except the system prompt. Safe to call while the
// agent is busy — the reset will be picked up at the next idle point.
void pageros_agent_reset(void);

// --- submit / runtime -------------------------------------------- //

// Enqueue a user message. Returns ESP_ERR_INVALID_STATE if the agent
// is already inferring (the caller should disable input until the
// observer reports IDLE).
esp_err_t pageros_agent_submit(const char *user_input);

typedef enum {
    PAGEROS_AGENT_EVT_THINKING = 0, // posted to the model, awaiting reply
    PAGEROS_AGENT_EVT_TOOL,         // dispatched a tool; .text = tool name
    PAGEROS_AGENT_EVT_MESSAGE,      // assistant produced a terminal text reply
    PAGEROS_AGENT_EVT_ERROR,        // .text = human-readable error
    PAGEROS_AGENT_EVT_IDLE,         // loop finished, ready for next submit
} pageros_agent_event_kind_t;

typedef struct {
    pageros_agent_event_kind_t kind;
    const char *text;  // ephemeral — copy if you need to keep it
} pageros_agent_event_t;

typedef void (*pageros_agent_observer_t)(const pageros_agent_event_t *evt, void *ctx);

// Install the observer callback — invoked from the inference task. Pass
// (NULL, NULL) to clear. Replaces any existing observer.
void pageros_agent_set_observer(pageros_agent_observer_t fn, void *ctx);

// True while the inference task is mid-loop. The UI uses this to gate
// new submits and to render a "thinking" indicator.
bool pageros_agent_is_busy(void);

// --- tool registry ------------------------------------------------ //

// A tool implementation. Reads JSON arguments (the string the model
// emitted, already parsed as an arg-object JSON string), writes a
// result string into `result_buf`, returns ESP_OK on success. Errors
// should be encoded as JSON in `result_buf` and still return ESP_OK so
// the model sees them as a tool result rather than a transport error.
typedef esp_err_t (*pageros_agent_tool_fn_t)(const char *args_json,
                                              char       *result_buf,
                                              size_t      result_cap,
                                              void       *user_ctx);

// Register a tool. `json_schema` is the JSON Schema for the `arguments`
// object, as a string — passed verbatim into the API request's
// `tools[].function.parameters` field. The harness retains the pointers
// (so they must outlive the agent — string literals are fine).
esp_err_t pageros_agent_tool_register(const char *name,
                                       const char *description,
                                       const char *json_schema,
                                       pageros_agent_tool_fn_t fn,
                                       void *user_ctx);

#ifdef __cplusplus
}
#endif
