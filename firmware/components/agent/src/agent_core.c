// SPDX-License-Identifier: Apache-2.0
//
// Agent state-machine + inference task.
//
// Lifecycle:
//   pageros_agent_init() loads config, seeds the system message, spawns
//   the agent_task at priority below the shell render task. The task
//   blocks on g_input_queue. Each submitted user line drives the
//   inner loop:
//
//     append_user(input)
//     repeat up to max_iterations:
//         http_complete(&res)
//         if res has tool_calls:
//             append_assistant(content, tool_calls_json, summary)
//             for each call: dispatch → append_tool(id, name, result)
//             continue
//         else:
//             append_assistant(content, NULL, NULL)
//             break
//
// Observer callbacks fire at every state edge so the UI can render a
// live "THINKING / TOOL: wifi_scan / IDLE" status line.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "cJSON.h"

#include "agent_internal.h"

static const char *TAG = "agent_core";

#define AGENT_TASK_STACK   (12 * 1024)
#define AGENT_TASK_PRIO    4
#define AGENT_INPUT_QLEN   4

static QueueHandle_t g_input_q = NULL;
static TaskHandle_t  g_task    = NULL;
static volatile bool g_busy    = false;
static bool          g_ready   = false;

static pageros_agent_observer_t g_obs_fn  = NULL;
static void                    *g_obs_ctx = NULL;

// --- observer plumbing -------------------------------------------- //

void agent_emit_event(pageros_agent_event_kind_t kind, const char *text)
{
    if (!g_obs_fn) return;
    pageros_agent_event_t evt = { .kind = kind, .text = text };
    g_obs_fn(&evt, g_obs_ctx);
}

void agent_set_busy(bool b) { g_busy = b; }

void pageros_agent_set_observer(pageros_agent_observer_t fn, void *ctx)
{
    g_obs_fn  = fn;
    g_obs_ctx = ctx;
}

bool pageros_agent_is_busy(void)  { return g_busy; }
bool pageros_agent_is_ready(void) { return g_ready; }

// --- transcript read API ----------------------------------------- //

int pageros_agent_transcript_count(void)
{
    return agent_messages_count();
}

bool pageros_agent_transcript_get(int idx, pageros_agent_turn_t *out)
{
    if (!out) return false;
    const agent_message_t *m = agent_messages_get(idx);
    if (!m) return false;
    out->role         = m->role;
    out->content      = m->content;
    out->tool_summary = m->tool_summary
                        ? m->tool_summary
                        : (m->role == PAGEROS_AGENT_ROLE_TOOL ? m->tool_name : NULL);
    out->ts_us        = m->ts_us;
    return true;
}

void pageros_agent_reset(void)
{
    // Safe to call while busy — the inference task notices the system-
    // only state on its next iteration. In practice callers should wait
    // for IDLE first.
    agent_messages_reset_to_system();
}

// --- submission queue --------------------------------------------- //

typedef struct {
    char text[AGENT_USER_INPUT_MAX];
} input_item_t;

esp_err_t pageros_agent_submit(const char *user_input)
{
    if (!g_ready || !g_input_q) return ESP_ERR_INVALID_STATE;
    if (g_busy) return ESP_ERR_INVALID_STATE;
    if (!user_input || !*user_input) return ESP_ERR_INVALID_ARG;
    input_item_t item = {0};
    strncpy(item.text, user_input, sizeof(item.text) - 1);
    BaseType_t r = xQueueSend(g_input_q, &item, 0);
    return (r == pdTRUE) ? ESP_OK : ESP_ERR_NO_MEM;
}

// --- inner loop --------------------------------------------------- //

// Build a short, screen-friendly summary of a tool_calls payload.
// "wifi_scan, get_time" — used as the assistant turn's UI label.
static char *summarize_tool_calls(const char *tool_calls_json)
{
    cJSON *arr = cJSON_Parse(tool_calls_json);
    if (!arr) return NULL;
    char buf[160] = {0};
    int off = 0;
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        cJSON *call = cJSON_GetArrayItem(arr, i);
        const cJSON *fn   = cJSON_GetObjectItemCaseSensitive(call, "function");
        const cJSON *name = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
        if (cJSON_IsString(name) && name->valuestring) {
            int w = snprintf(buf + off, sizeof(buf) - off,
                             "%s%s", i == 0 ? "" : ", ", name->valuestring);
            if (w < 0 || w >= (int)(sizeof(buf) - off)) break;
            off += w;
        }
    }
    cJSON_Delete(arr);
    if (off == 0) return NULL;
    return strdup(buf);
}

// Dispatch every tool call in a parsed tool_calls array; append each
// result as a tool message. Emits TOOL events for the observer.
static void dispatch_tool_calls(const char *tool_calls_json)
{
    cJSON *arr = cJSON_Parse(tool_calls_json);
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        return;
    }
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        cJSON *call = cJSON_GetArrayItem(arr, i);
        const cJSON *id   = cJSON_GetObjectItemCaseSensitive(call, "id");
        const cJSON *fn   = cJSON_GetObjectItemCaseSensitive(call, "function");
        const cJSON *name = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name")      : NULL;
        const cJSON *args = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;

        const char *id_s   = (cJSON_IsString(id)   && id->valuestring)   ? id->valuestring   : "";
        const char *name_s = (cJSON_IsString(name) && name->valuestring) ? name->valuestring : "";
        // The OpenAI spec hands args back as a *string* of JSON. Some
        // backends emit a parsed object; handle both.
        const char *args_s = "{}";
        char *args_alloc = NULL;
        if (cJSON_IsString(args) && args->valuestring) {
            args_s = args->valuestring;
        } else if (args && (cJSON_IsObject(args) || cJSON_IsArray(args))) {
            args_alloc = cJSON_PrintUnformatted(args);
            if (args_alloc) args_s = args_alloc;
        }

        ESP_LOGI(TAG, "tool call: %s(%s)", name_s, args_s);
        agent_emit_event(PAGEROS_AGENT_EVT_TOOL, name_s);

        char *result = (char *)calloc(1, AGENT_TOOL_RESULT_MAX);
        if (!result) {
            free(args_alloc);
            continue;
        }
        const agent_tool_t *t = agent_tools_find(name_s);
        if (!t) {
            snprintf(result, AGENT_TOOL_RESULT_MAX,
                     "{\"error\":\"unknown tool: %s\"}", name_s);
        } else {
            esp_err_t r = t->fn(args_s, result, AGENT_TOOL_RESULT_MAX, t->user_ctx);
            if (r != ESP_OK && result[0] == '\0') {
                snprintf(result, AGENT_TOOL_RESULT_MAX,
                         "{\"error\":\"%s\"}", esp_err_to_name(r));
            }
        }
        agent_messages_append_tool(id_s, name_s, result);
        free(result);
        free(args_alloc);
    }
    cJSON_Delete(arr);
}

static void run_one_turn(const char *user_input)
{
    agent_set_busy(true);
    agent_messages_append_user(user_input);
    agent_emit_event(PAGEROS_AGENT_EVT_THINKING, NULL);

    const agent_config_t *cfg = agent_config_get();
    int max_iter = (cfg && cfg->max_iterations > 0) ? cfg->max_iterations : 6;

    for (int iter = 0; iter < max_iter; iter++) {
        agent_http_result_t res = {0};
        esp_err_t r = agent_http_complete(&res);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "http: %s (%s)", esp_err_to_name(r), res.error);
            agent_emit_event(PAGEROS_AGENT_EVT_ERROR, res.error);
            agent_http_result_free(&res);
            break;
        }

        if (res.tool_calls_json) {
            char *summary = summarize_tool_calls(res.tool_calls_json);
            agent_messages_append_assistant(res.content,
                                            res.tool_calls_json,
                                            summary);
            free(summary);
            dispatch_tool_calls(res.tool_calls_json);
            agent_http_result_free(&res);
            agent_emit_event(PAGEROS_AGENT_EVT_THINKING, NULL);
            continue;
        }

        // Terminal text reply.
        const char *final_text = res.content ? res.content : "";
        agent_messages_append_assistant(final_text, NULL, NULL);
        agent_emit_event(PAGEROS_AGENT_EVT_MESSAGE, final_text);
        agent_http_result_free(&res);
        break;
    }

    agent_emit_event(PAGEROS_AGENT_EVT_IDLE, NULL);
    agent_set_busy(false);
}

static void agent_task(void *param)
{
    (void)param;
    input_item_t item;
    for (;;) {
        if (xQueueReceive(g_input_q, &item, portMAX_DELAY) != pdTRUE) continue;
        run_one_turn(item.text);
    }
}

// --- init --------------------------------------------------------- //

esp_err_t pageros_agent_init(void)
{
    esp_err_t r = agent_config_load();
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "config load: %s — agent will not run",
                 esp_err_to_name(r));
        return r;
    }

    const agent_config_t *cfg = agent_config_get();
    agent_messages_init(cfg ? cfg->system_prompt : "");

    agent_register_builtin_tools();

    if (!g_input_q) {
        g_input_q = xQueueCreate(AGENT_INPUT_QLEN, sizeof(input_item_t));
        if (!g_input_q) return ESP_ERR_NO_MEM;
    }
    if (!g_task) {
        BaseType_t t = xTaskCreate(agent_task, "agent",
                                   AGENT_TASK_STACK, NULL,
                                   AGENT_TASK_PRIO, &g_task);
        if (t != pdPASS) return ESP_ERR_NO_MEM;
    }

    g_ready = true;
    ESP_LOGI(TAG, "agent ready; %d tool(s) registered",
             agent_tools_count());
    return ESP_OK;
}
