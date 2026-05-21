// SPDX-License-Identifier: Apache-2.0
// FW-031 — group session client.

#include "pageros_group_session.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "group_session";

static struct {
    bool inited;
    char groups[PAGEROS_GS_MAX_GROUPS][PAGEROS_GS_MAX_GROUP_ID];
    size_t count;
    pageros_gs_callback_t cb;
    void *cb_user;
    pageros_gs_stats_t stats;
} g;

esp_err_t pageros_group_session_init(void)
{
    if (g.inited) return ESP_OK;
    memset(&g, 0, sizeof(g));
    g.inited = true;
    return ESP_OK;
}

esp_err_t pageros_group_session_shutdown(void)
{
    g.inited = false;
    memset(&g, 0, sizeof(g));
    return ESP_OK;
}

esp_err_t pageros_group_session_subscribe(const char * const *group_ids, size_t n)
{
    if (!g.inited) return ESP_ERR_INVALID_STATE;
    if (n > PAGEROS_GS_MAX_GROUPS) return ESP_ERR_INVALID_SIZE;
    g.count = 0;
    for (size_t i = 0; i < n; i++) {
        if (!group_ids[i]) continue;
        size_t l = strlen(group_ids[i]);
        if (l == 0 || l >= PAGEROS_GS_MAX_GROUP_ID) continue;
        strncpy(g.groups[g.count], group_ids[i], PAGEROS_GS_MAX_GROUP_ID - 1);
        g.groups[g.count][PAGEROS_GS_MAX_GROUP_ID - 1] = '\0';
        g.count++;
    }
    g.stats.subscribed = g.count;
    ESP_LOGI(TAG, "subscribed to %u groups", (unsigned)g.count);
    return ESP_OK;
}

bool pageros_group_session_has(const char *group_id)
{
    if (!g.inited || !group_id) return false;
    for (size_t i = 0; i < g.count; i++) {
        if (strcmp(g.groups[i], group_id) == 0) return true;
    }
    return false;
}

void pageros_group_session_set_callback(pageros_gs_callback_t cb, void *user)
{
    g.cb = cb;
    g.cb_user = user;
}

esp_err_t pageros_group_session_dispatch(const pageros_gs_event_t *event)
{
    if (!g.inited || !event || !event->group_id) return ESP_ERR_INVALID_ARG;
    if (!pageros_group_session_has(event->group_id)) {
        g.stats.dropped_not_subscribed++;
        return ESP_ERR_NOT_FOUND;
    }
    if (g.cb) g.cb(event, g.cb_user);
    g.stats.delivered++;
    return ESP_OK;
}

void pageros_group_session_get_stats(pageros_gs_stats_t *out)
{
    if (out) *out = g.stats;
}
