// PagerOS firmware entry point.
//
// Hosts the boot sequence (selftest, NVS, identity, peripherals) and the
// "cyberpunk desktop" shell — a persistent chrome of top + bottom status
// bars plus a left sidebar rail tracking open apps, a tile-grid desktop
// home, and a viewport into the currently-focused foreground app.
//
// The shell is conceptually a built-in app (SPEC §10.4 / FW-029): its
// internal pages (DESKTOP / SETTINGS / WIFI / ADD_APP) are mounted into
// apprt as Frames, and external apps mount their own. The desktop's
// HOME tile grid is the one exception — it's drawn directly into the
// framebuffer rather than going through the Frame renderer, because
// the tile layout is shell-policy rather than app-content.

#include <stdio.h>
#include <string.h>
#include <dirent.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"

#include "pageros_apprt.h"
#include "pageros_audio.h"
#include "pageros_cbor.h"
#include "pageros_display.h"
#include "pageros_fonts.h"
#include "pageros_gps.h"
#include "pageros_i2c_bus.h"
#include "pageros_identity.h"
#include "pageros_imu.h"
#include "pageros_input.h"
#include "pageros_input_router.h"
#include "pageros_keyboard.h"
#include "pageros_logger.h"
#include "pageros_lora.h"
#include "pageros_network.h"
#include "pageros_power.h"
#include "pageros_shell.h"
#include "pageros_sideload.h"
#include "pageros_storage.h"
#include "pageros_widgets.h"
#include "pageros_xl9555.h"
#include "selftest.h"

static const char *TAG = "pageros";

__attribute__((unused)) static const char *router_nav_name(pageros_router_nav_t n)
{
    switch (n) {
        case PAGEROS_ROUTER_NAV_ENTER:     return "ENTER";
        case PAGEROS_ROUTER_NAV_BACK:      return "BACK";
        case PAGEROS_ROUTER_NAV_BACK_LONG: return "BACK_LONG";
        default:                           return "?";
    }
}

// --- Shell state -------------------------------------------------- //

typedef enum {
    SHELL_PAGE_DESKTOP = 0,   // tile-grid HOME — drawn directly, not via Frame
    SHELL_PAGE_APP,           // foreground external app, rendered via widgets
    SHELL_PAGE_SETTINGS,
    SHELL_PAGE_WIFI,
    SHELL_PAGE_ADD_APP,
} shell_page_t;

typedef enum {
    FOCUS_VIEWPORT = 0,       // encoder navigates content in the viewport
    FOCUS_RAIL,               // encoder cycles HOME + open-apps on the rail
} focus_mode_t;

#define MAX_DESKTOP_TILES 12

static struct {
    shell_page_t page;
    focus_mode_t focus_mode;

    // Tile grid focus (only on DESKTOP).
    int tile_index;
    int tile_count;
    pageros_chrome_tile_t tiles[MAX_DESKTOP_TILES];
    char tile_app_ids[MAX_DESKTOP_TILES][96];
    char tile_names[MAX_DESKTOP_TILES][32];

    // Rail focus (used in both DESKTOP + APP). 0 = HOME, 1..n = open app idx+1.
    int rail_index;
    int rail_count;
    const char *rail_items[PAGEROS_CHROME_RAIL_MAX_APPS];

    // Viewport widget focus (apps, settings, wifi, addapp).
    pageros_widgets_focus_t vp_focus;

    // Text input plumbing for settings/wifi/addapp.
    char ssid[33];
    char psk[65];
    char wifi_status[48];

    char url[160];
    char addapp_msg[80];
    char addapp_msg_level[8];

    int  text_input_widget;
    char  *text_buf;
    size_t text_cap;

    // Chrome cache — system uptime at boot, used for the clock display.
    uint64_t boot_unix_us;
} s = {
    .page         = SHELL_PAGE_DESKTOP,
    .focus_mode   = FOCUS_VIEWPORT,
    .tile_index   = 0,
    .rail_index   = 0,
    .text_input_widget = -1,
};

// --- Chrome state collection -------------------------------------- //

static void format_identity_short(char out[10])
{
    char fp[PAGEROS_IDENTITY_FP_LEN] = {0};
    if (pageros_identity_fingerprint(fp) != ESP_OK || fp[0] == '\0') {
        snprintf(out, 10, "????????");
        out[4] = '.';
        return;
    }
    // 12-char base32 fingerprint → "XXXX.XXXX" (8 + dot)
    out[0] = fp[0]; out[1] = fp[1]; out[2] = fp[2]; out[3] = fp[3];
    out[4] = '.';
    out[5] = fp[4]; out[6] = fp[5]; out[7] = fp[6]; out[8] = fp[7];
    out[9] = '\0';
}

static const char *power_state_str(void)
{
    switch (pageros_power_state()) {
    case PAGEROS_POWER_ACTIVE:      return "ACT";
    case PAGEROS_POWER_DIM:         return "DIM";
    case PAGEROS_POWER_SCREEN_OFF:  return "OFF";
    case PAGEROS_POWER_LIGHT_SLEEP: return "LSP";
    case PAGEROS_POWER_DEEP_SLEEP:  return "DSP";
    }
    return "?";
}

static void collect_chrome_state(pageros_chrome_state_t *out, char id_buf[10])
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    // No RTC source yet — derive HH:MM from uptime so the clock at
    // least moves visibly. Once a time-sync source lands we swap this
    // to time(NULL) / gmtime().
    int64_t up_us = esp_timer_get_time();
    int up_min   = (int)(up_us / 1000000 / 60);
    out->hh      = (up_min / 60) % 24;
    out->mm      = up_min % 60;

    if (id_buf) {
        format_identity_short(id_buf);
        out->identity_short = id_buf;
    }
    // Battery: no fuel-gauge driver yet — pin to 75% so the bar shows
    // a sensible value. Replace when BAT_MON lands.
    out->battery_pct = 75;
    out->wifi_state  = pageros_wifi_is_connected() ? 2 : 0;
    out->lora_state  = pageros_lora_is_ready() ? 1 : 0;
    pageros_gps_fix_t fix = {0};
    out->gps_state   = (pageros_gps_get_last_fix(&fix) == ESP_OK
                        && fix.accuracy_m > 0.0f) ? 1 : 0;
    out->power_mode  = power_state_str();
}

// --- Tile grid (desktop home) ------------------------------------- //

#ifndef PAGEROS_DIR_APPS
#define PAGEROS_DIR_APPS  "/sd/apps"
#endif

static void desktop_refresh_tiles(void)
{
    s.tile_count = 0;

    // "Settings" is always tile 0 — it's the gateway to Wi-Fi / Add app.
    snprintf(s.tile_app_ids[s.tile_count], sizeof(s.tile_app_ids[0]),
             "shell:settings");
    snprintf(s.tile_names[s.tile_count], sizeof(s.tile_names[0]),
             "SETTINGS");
    s.tiles[s.tile_count].name   = s.tile_names[s.tile_count];
    s.tiles[s.tile_count].unread = 0;
    s.tile_count++;

    // Walk /sd/apps if SD is mounted. If not, we just show Settings —
    // installing apps requires the SD anyway.
    DIR *d = opendir(PAGEROS_DIR_APPS);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && s.tile_count < MAX_DESKTOP_TILES) {
            if (e->d_name[0] == '.') continue;
            // Use last segment of dotted id as the display name.
            const char *dot = strrchr(e->d_name, '.');
            const char *name = dot ? dot + 1 : e->d_name;
            // "open:<id>" href; renderer will upper-case for display.
            char short_id[80];
            strncpy(short_id, e->d_name, sizeof(short_id) - 1);
            short_id[sizeof(short_id) - 1] = '\0';
            snprintf(s.tile_app_ids[s.tile_count],
                     sizeof(s.tile_app_ids[0]),
                     "open:%s", short_id);
            strncpy(s.tile_names[s.tile_count], name,
                    sizeof(s.tile_names[0]) - 1);
            s.tile_names[s.tile_count][sizeof(s.tile_names[0]) - 1] = '\0';
            s.tiles[s.tile_count].name   = s.tile_names[s.tile_count];
            s.tiles[s.tile_count].unread = 0;
            s.tile_count++;
        }
        closedir(d);
    }

    if (s.tile_index >= s.tile_count) s.tile_index = 0;
}

static void rail_refresh(void)
{
    s.rail_count = (int)pageros_apprt_open_list(s.rail_items,
                                                PAGEROS_CHROME_RAIL_MAX_APPS);
    if (s.rail_index > s.rail_count) s.rail_index = 0;
}

// --- Rendering ----------------------------------------------------- //

static pageros_widgets_palette_t g_palette;

static void draw_chrome_bars(pageros_fonts_canvas_t *canvas,
                             const pageros_chrome_state_t *cs)
{
    pageros_widgets_chrome_topbar(canvas, &g_palette, cs);
    pageros_widgets_chrome_botbar(canvas, &g_palette, cs);
}

static void draw_rail(pageros_fonts_canvas_t *canvas)
{
    pageros_chrome_rail_t rail = {
        .n_items     = s.rail_count,
        .focus_index = s.focus_mode == FOCUS_RAIL ? s.rail_index : -1,
        .rail_active = s.focus_mode == FOCUS_RAIL,
    };
    for (int i = 0; i < s.rail_count; i++) {
        rail.items[i] = s.rail_items[i];
    }
    pageros_widgets_chrome_rail(canvas, &g_palette, &rail);
}

static void render_desktop(pageros_fonts_canvas_t *canvas,
                           const pageros_chrome_state_t *cs)
{
    pageros_widgets_fill_rect(canvas, 0, 0, canvas->w, canvas->h, g_palette.bg);
    draw_chrome_bars(canvas, cs);
    draw_rail(canvas);

    // Tile-grid viewport: right of rail, between top/bot bars.
    int vp_x = PAGEROS_CHROME_RAIL_W;
    int vp_y = PAGEROS_CHROME_BAR_H;
    int vp_w = canvas->w - PAGEROS_CHROME_RAIL_W;
    int vp_h = canvas->h - 2 * PAGEROS_CHROME_BAR_H;

    int focus = (s.focus_mode == FOCUS_VIEWPORT) ? s.tile_index : -1;
    pageros_widgets_chrome_tile_grid(canvas, &g_palette,
                                     s.tiles, s.tile_count,
                                     focus, vp_x, vp_y, vp_w, vp_h);

    // Light scanline overlay for the cyberpunk vibe — only in the
    // viewport so the chrome stays crisp.
    pageros_widgets_chrome_scanlines(canvas, vp_x, vp_y, vp_w, vp_h, 4);
}

static void render_modal_frame(pageros_fonts_canvas_t *canvas,
                               const pageros_chrome_state_t *cs)
{
    // Settings / Wi-Fi / Add-app — full-width, no rail, with chrome bars.
    pageros_widgets_fill_rect(canvas, 0, 0, canvas->w, canvas->h, g_palette.bg);
    draw_chrome_bars(canvas, cs);

    pageros_widgets_ctx_t ctx = {
        .canvas      = *canvas,
        .palette     = g_palette,
        .focus       = s.vp_focus,
        .vx = 0,
        .vy = PAGEROS_CHROME_BAR_H,
        .vw = canvas->w,
        .vh = canvas->h - 2 * PAGEROS_CHROME_BAR_H,
        .skip_chrome = true,
        .title       = NULL,
        .help        = NULL,
    };
    const pgr_cbor_value_t *fr = pageros_apprt_foreground_frame();
    if (!fr) return;
    const pgr_cbor_value_t *body = NULL;
    if (fr->kind == PGR_CBOR_KIND_MAP) {
        for (size_t i = 0; i < fr->v.map.len; i++) {
            const pgr_cbor_pair_t *p = &fr->v.map.items[i];
            if (p->key.kind == PGR_CBOR_KIND_TEXT &&
                p->key.v.bytes.len == 4 &&
                memcmp(p->key.v.bytes.data, "body", 4) == 0) {
                body = &p->val; break;
            }
        }
    }
    pageros_widgets_render_screen(&ctx, body);
}

static void render_app(pageros_fonts_canvas_t *canvas,
                       const pageros_chrome_state_t *cs)
{
    // External app — render with chrome bars + rail visible.
    pageros_widgets_fill_rect(canvas, 0, 0, canvas->w, canvas->h, g_palette.bg);
    draw_chrome_bars(canvas, cs);
    draw_rail(canvas);

    pageros_widgets_ctx_t ctx = {
        .canvas      = *canvas,
        .palette     = g_palette,
        .focus       = s.vp_focus,
        .vx = PAGEROS_CHROME_RAIL_W,
        .vy = PAGEROS_CHROME_BAR_H,
        .vw = canvas->w - PAGEROS_CHROME_RAIL_W,
        .vh = canvas->h - 2 * PAGEROS_CHROME_BAR_H,
        .skip_chrome = true,
        .title       = NULL,
        .help        = NULL,
    };
    const pgr_cbor_value_t *fr = pageros_apprt_foreground_frame();
    if (!fr) {
        const char *msg = "(no frame)";
        pageros_fonts_draw_text_16(&ctx.canvas,
                                   ctx.vx + 10, ctx.vy + ctx.vh / 2,
                                   msg, -1, g_palette.dim, ctx.vw);
        return;
    }
    const pgr_cbor_value_t *body = NULL;
    if (fr->kind == PGR_CBOR_KIND_MAP) {
        for (size_t i = 0; i < fr->v.map.len; i++) {
            const pgr_cbor_pair_t *p = &fr->v.map.items[i];
            if (p->key.kind == PGR_CBOR_KIND_TEXT &&
                p->key.v.bytes.len == 4 &&
                memcmp(p->key.v.bytes.data, "body", 4) == 0) {
                body = &p->val; break;
            }
        }
    }
    pageros_widgets_render_screen(&ctx, body);
}

static void render_screen(void)
{
    rail_refresh();
    pageros_chrome_state_t cs;
    char id_buf[10];
    collect_chrome_state(&cs, id_buf);

    pageros_fonts_canvas_t canvas = {
        .pixels = (uint16_t *)pageros_display_framebuffer(),
        .w      = PAGEROS_DISPLAY_WIDTH,
        .h      = PAGEROS_DISPLAY_HEIGHT,
    };

    switch (s.page) {
    case SHELL_PAGE_DESKTOP:   render_desktop(&canvas, &cs); break;
    case SHELL_PAGE_APP:       render_app(&canvas, &cs);     break;
    case SHELL_PAGE_SETTINGS:
    case SHELL_PAGE_WIFI:
    case SHELL_PAGE_ADD_APP:   render_modal_frame(&canvas, &cs); break;
    }
    pageros_display_present();
}

// --- Mount helpers ------------------------------------------------ //

static esp_err_t mount_and_set(esp_err_t (*emit)(uint8_t *, size_t, size_t *))
{
    uint8_t buf[768]; size_t n = 0;
    esp_err_t r = emit(buf, sizeof(buf), &n);
    if (r != ESP_OK) return r;
    return pageros_apprt_set_frame(buf, n);
}

static void mount_desktop(void)
{
    ESP_LOGI("shell", "mount_desktop");
    s.page = SHELL_PAGE_DESKTOP;
    s.focus_mode = FOCUS_VIEWPORT;
    s.text_input_widget = -1;
    s.text_buf = NULL; s.text_cap = 0;
    desktop_refresh_tiles();
    render_screen();
}

static void mount_settings(void)
{
    ESP_LOGI("shell", "mount_settings");
    s.page = SHELL_PAGE_SETTINGS;
    s.focus_mode = FOCUS_VIEWPORT;
    s.vp_focus.widget_index = 1; s.vp_focus.item_index = 0;
    s.text_input_widget = -1;
    s.text_buf = NULL; s.text_cap = 0;
    uint8_t buf[768]; size_t n = 0;
    if (pageros_shell_emit_settings(buf, sizeof(buf), &n) == ESP_OK) {
        pageros_apprt_set_frame(buf, n);
    }
    render_screen();
}

static esp_err_t emit_wifi_thunk(uint8_t *b, size_t c, size_t *n)
{
    return pageros_shell_emit_wifi_config(b, c, n, s.ssid, s.psk, s.wifi_status);
}

static void mount_wifi(void)
{
    ESP_LOGI("shell", "mount_wifi");
    s.page = SHELL_PAGE_WIFI;
    s.focus_mode = FOCUS_VIEWPORT;
    s.vp_focus.widget_index = 2;
    s.vp_focus.item_index = 0;
    s.text_input_widget = 2;
    s.text_buf = s.ssid;
    s.text_cap = sizeof(s.ssid);
    pageros_wifi_creds_load(s.ssid, sizeof(s.ssid), s.psk, sizeof(s.psk));
    if (pageros_wifi_is_connected()) {
        strncpy(s.wifi_status, "Connected", sizeof(s.wifi_status));
    } else if (s.wifi_status[0] == '\0') {
        strncpy(s.wifi_status, "Disconnected", sizeof(s.wifi_status));
    }
    s.wifi_status[sizeof(s.wifi_status) - 1] = '\0';
    mount_and_set(emit_wifi_thunk);
    render_screen();
}

static esp_err_t emit_addapp_thunk(uint8_t *b, size_t c, size_t *n)
{
    return pageros_shell_emit_add_app(b, c, n, s.url,
                                      s.addapp_msg[0] ? s.addapp_msg : NULL,
                                      s.addapp_msg_level[0] ? s.addapp_msg_level : NULL);
}

static void mount_add_app(void)
{
    ESP_LOGI("shell", "mount_add_app");
    s.page = SHELL_PAGE_ADD_APP;
    s.focus_mode = FOCUS_VIEWPORT;
    int notif = s.addapp_msg[0] ? 1 : 0;
    s.vp_focus.widget_index = 2 + notif;
    s.vp_focus.item_index = 0;
    s.text_input_widget = s.vp_focus.widget_index;
    s.text_buf = s.url;
    s.text_cap = sizeof(s.url);
    mount_and_set(emit_addapp_thunk);
    render_screen();
}

// --- TCA8418 keymap ---------------------------------------------- //
//
// LILYGO T-LoRa Pager keyboard. Bytes verbatim from LilyGoLib's
// `LilyGo_LoRa_Pager.cpp` rotation_config/keymap definitions.
static const char kb_keymap[4][10] = {
    {'q','w','e','r','t','y','u','i','o','p'},
    {'a','s','d','f','g','h','j','k','l','\n'},
    { 0 ,'z','x','c','v','b','n','m', 0 , 0 },
    {' ', 0 , 0 , 0 , 0 , 0 , 0 , 0 , 0 , 0 },
};

static const char kb_symbol_map[4][10] = {
    {'1','2','3','4','5','6','7','8','9','0'},
    {'*','/','+','-','=',':','\'','"','@', 0 },
    { 0 ,'_','$',';','?','!',',','.', 0 , 0 },
    {' ', 0 , 0 , 0 , 0 , 0 , 0 , 0 , 0 , 0 },
};

#define KB_ENTER_K      20
#define KB_SYMBOL_K     21
#define KB_CAPS_K       29
#define KB_BACKSPACE_K  30

static bool g_caps_on   = false;
static bool g_symbol_on = false;

static char kb_decode(uint8_t row, uint8_t col)
{
    if (row >= 4 || col >= 10) return 0;
    uint8_t k = (uint8_t)(row * 10 + col + 1);
    if (k == KB_CAPS_K)      { g_caps_on   = !g_caps_on;   return 0; }
    if (k == KB_SYMBOL_K)    { g_symbol_on = !g_symbol_on; return 0; }
    if (k == KB_ENTER_K)     return '\n';
    if (k == KB_BACKSPACE_K) return '\b';
    uint8_t r = (k - 1) / 10;
    uint8_t c = (k - 1) % 10;
    if (r >= 4 || c >= 10) return 0;
    char ch = g_symbol_on ? kb_symbol_map[r][c] : kb_keymap[r][c];
    if (g_caps_on && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    return ch;
}

// --- Frame field lookup ------------------------------------------ //

static const char *focused_href(const pgr_cbor_value_t *frame,
                                int widget_index, int item_index,
                                char *out, size_t out_cap)
{
    if (!frame || frame->kind != PGR_CBOR_KIND_MAP || !out) return NULL;
    const pgr_cbor_value_t *body = NULL;
    for (size_t i = 0; i < frame->v.map.len; i++) {
        const pgr_cbor_pair_t *p = &frame->v.map.items[i];
        if (p->key.kind == PGR_CBOR_KIND_TEXT &&
            p->key.v.bytes.len == 4 &&
            memcmp(p->key.v.bytes.data, "body", 4) == 0) { body = &p->val; break; }
    }
    if (!body || body->kind != PGR_CBOR_KIND_ARRAY) return NULL;
    if (widget_index < 0 || widget_index >= (int)body->v.arr.len) return NULL;
    const pgr_cbor_value_t *w = &body->v.arr.items[widget_index];
    if (w->kind != PGR_CBOR_KIND_MAP) return NULL;
    // 1. Try list items.
    const pgr_cbor_value_t *items = NULL;
    for (size_t i = 0; i < w->v.map.len; i++) {
        const pgr_cbor_pair_t *p = &w->v.map.items[i];
        if (p->key.kind == PGR_CBOR_KIND_TEXT &&
            p->key.v.bytes.len == 5 &&
            memcmp(p->key.v.bytes.data, "items", 5) == 0) { items = &p->val; break; }
    }
    if (items && items->kind == PGR_CBOR_KIND_ARRAY &&
        item_index >= 0 && item_index < (int)items->v.arr.len) {
        const pgr_cbor_value_t *it = &items->v.arr.items[item_index];
        if (it->kind == PGR_CBOR_KIND_MAP) {
            for (size_t i = 0; i < it->v.map.len; i++) {
                const pgr_cbor_pair_t *p = &it->v.map.items[i];
                if (p->key.kind == PGR_CBOR_KIND_TEXT &&
                    p->key.v.bytes.len == 4 &&
                    memcmp(p->key.v.bytes.data, "href", 4) == 0 &&
                    p->val.kind == PGR_CBOR_KIND_TEXT) {
                    size_t l = p->val.v.bytes.len;
                    if (l >= out_cap) l = out_cap - 1;
                    memcpy(out, p->val.v.bytes.data, l);
                    out[l] = '\0';
                    return out;
                }
            }
        }
    }
    // 2. Try standalone button/link widget.
    for (size_t i = 0; i < w->v.map.len; i++) {
        const pgr_cbor_pair_t *p = &w->v.map.items[i];
        if (p->key.kind == PGR_CBOR_KIND_TEXT &&
            p->key.v.bytes.len == 4 &&
            memcmp(p->key.v.bytes.data, "href", 4) == 0 &&
            p->val.kind == PGR_CBOR_KIND_TEXT) {
            size_t l = p->val.v.bytes.len;
            if (l >= out_cap) l = out_cap - 1;
            memcpy(out, p->val.v.bytes.data, l);
            out[l] = '\0';
            return out;
        }
    }
    return NULL;
}

static int focused_list_len(const pgr_cbor_value_t *frame, int widget_index)
{
    if (!frame || frame->kind != PGR_CBOR_KIND_MAP) return -1;
    const pgr_cbor_value_t *body = NULL;
    for (size_t i = 0; i < frame->v.map.len; i++) {
        const pgr_cbor_pair_t *p = &frame->v.map.items[i];
        if (p->key.kind == PGR_CBOR_KIND_TEXT &&
            p->key.v.bytes.len == 4 &&
            memcmp(p->key.v.bytes.data, "body", 4) == 0) { body = &p->val; break; }
    }
    if (!body || body->kind != PGR_CBOR_KIND_ARRAY) return -1;
    if (widget_index < 0 || widget_index >= (int)body->v.arr.len) return -1;
    const pgr_cbor_value_t *w = &body->v.arr.items[widget_index];
    if (w->kind != PGR_CBOR_KIND_MAP) return -1;
    for (size_t i = 0; i < w->v.map.len; i++) {
        const pgr_cbor_pair_t *p = &w->v.map.items[i];
        if (p->key.kind == PGR_CBOR_KIND_TEXT &&
            p->key.v.bytes.len == 5 &&
            memcmp(p->key.v.bytes.data, "items", 5) == 0 &&
            p->val.kind == PGR_CBOR_KIND_ARRAY) {
            return (int)p->val.v.arr.len;
        }
    }
    return -1;
}

// --- Action dispatch -------------------------------------------- //

static void launch_app(const char *app_id);

static bool handle_button_href(const char *href)
{
    if (!href) return false;
    if (strcmp(href, "shell:home") == 0)      { mount_desktop();  return true; }
    if (strcmp(href, "shell:settings") == 0)  { mount_settings(); return true; }
    if (strcmp(href, "shell:wifi") == 0)      { mount_wifi();     return true; }
    if (strcmp(href, "shell:add-app") == 0)   { mount_add_app();  return true; }
    if (strcmp(href, "shell:about") == 0) {
        strncpy(s.addapp_msg, "PagerOS pre-alpha · CC0", sizeof(s.addapp_msg));
        strncpy(s.addapp_msg_level, "info", sizeof(s.addapp_msg_level));
        return true;
    }
    if (strcmp(href, "shell:wifi-connect") == 0) {
        if (s.ssid[0] == '\0') {
            strncpy(s.wifi_status, "SSID is empty", sizeof(s.wifi_status));
        } else {
            strncpy(s.wifi_status, "Connecting…", sizeof(s.wifi_status));
            s.wifi_status[sizeof(s.wifi_status) - 1] = '\0';
            esp_err_t r = pageros_wifi_connect(s.ssid, s.psk, 15000);
            if (r == ESP_OK) {
                strncpy(s.wifi_status, "Connected", sizeof(s.wifi_status));
                pageros_wifi_creds_save(s.ssid, s.psk);
            } else {
                snprintf(s.wifi_status, sizeof(s.wifi_status), "Failed: %s",
                         esp_err_to_name(r));
            }
        }
        s.wifi_status[sizeof(s.wifi_status) - 1] = '\0';
        mount_and_set(emit_wifi_thunk);
        return true;
    }
    if (strcmp(href, "shell:install") == 0) {
        if (s.url[0] == '\0') {
            strncpy(s.addapp_msg, "URL is empty", sizeof(s.addapp_msg));
            strncpy(s.addapp_msg_level, "warn", sizeof(s.addapp_msg_level));
        } else if (!pageros_wifi_is_connected()) {
            strncpy(s.addapp_msg, "Wi-Fi not connected", sizeof(s.addapp_msg));
            strncpy(s.addapp_msg_level, "warn", sizeof(s.addapp_msg_level));
        } else {
            esp_err_t sm = pageros_storage_init();
            if (sm != ESP_OK) {
                snprintf(s.addapp_msg, sizeof(s.addapp_msg),
                         "SD mount failed: %s", esp_err_to_name(sm));
                strncpy(s.addapp_msg_level, "error", sizeof(s.addapp_msg_level));
            } else {
                char app_id[PAGEROS_SIDELOAD_MAX_APP_ID];
                esp_err_t r = pageros_sideload_install_from_url(s.url,
                                                                app_id,
                                                                sizeof(app_id));
                if (r == ESP_OK) {
                    snprintf(s.addapp_msg, sizeof(s.addapp_msg),
                             "Installed %.48s", app_id);
                    strncpy(s.addapp_msg_level, "info", sizeof(s.addapp_msg_level));
                } else {
                    snprintf(s.addapp_msg, sizeof(s.addapp_msg),
                             "Install failed: %s", esp_err_to_name(r));
                    strncpy(s.addapp_msg_level, "error",
                            sizeof(s.addapp_msg_level));
                }
            }
        }
        s.addapp_msg_level[sizeof(s.addapp_msg_level) - 1] = '\0';
        mount_and_set(emit_addapp_thunk);
        return true;
    }
    if (strncmp(href, "open:", 5) == 0) {
        launch_app(href + 5);
        return true;
    }
    return false;
}

static void launch_app(const char *app_id)
{
    if (!app_id || !*app_id) return;
    ESP_LOGI("shell", "launch_app: %s", app_id);

    esp_err_t sm = pageros_storage_init();
    if (sm != ESP_OK) {
        snprintf(s.addapp_msg, sizeof(s.addapp_msg),
                 "SD mount failed: %s", esp_err_to_name(sm));
        strncpy(s.addapp_msg_level, "error", sizeof(s.addapp_msg_level));
        return;
    }

    char url_path[256];
    snprintf(url_path, sizeof(url_path), "%s/%s/.source_url",
             PAGEROS_DIR_APPS, app_id);
    FILE *f = fopen(url_path, "rb");
    if (!f) {
        snprintf(s.addapp_msg, sizeof(s.addapp_msg),
                 "no .source_url for %s", app_id);
        strncpy(s.addapp_msg_level, "warn", sizeof(s.addapp_msg_level));
        return;
    }
    char base_url[160];
    size_t base_len = fread(base_url, 1, sizeof(base_url) - 1, f);
    fclose(f);
    while (base_len > 0 && (base_url[base_len - 1] == '\n' ||
                            base_url[base_len - 1] == '\r' ||
                            base_url[base_len - 1] == ' ')) {
        base_len--;
    }
    base_url[base_len] = '\0';

    char full_url[256];
    int n = snprintf(full_url, sizeof(full_url), "%s%s",
                     base_url,
                     base_url[base_len - 1] == '/' ? "" : "/");
    if (n < 0 || n >= (int)sizeof(full_url)) return;

    ESP_LOGI("shell", "GET %s", full_url);

    uint8_t *buf = (uint8_t *)malloc(8 * 1024);
    if (!buf) return;
    pageros_https_response_t r = {0};
    esp_err_t e = pageros_https_get(full_url, buf, 8 * 1024, &r);
    if (e != ESP_OK || r.status_code / 100 != 2 || r.body_len == 0) {
        ESP_LOGW("shell", "fetch %s: e=%s status=%d body=%u",
                 full_url, esp_err_to_name(e), r.status_code,
                 (unsigned)r.body_len);
        snprintf(s.addapp_msg, sizeof(s.addapp_msg),
                 "fetch failed: %s (HTTP %d)",
                 esp_err_to_name(e), r.status_code);
        strncpy(s.addapp_msg_level, "error", sizeof(s.addapp_msg_level));
        free(buf);
        return;
    }

    pageros_apprt_open(app_id, buf, r.body_len);
    pageros_apprt_open_mark(app_id);
    free(buf);

    s.page = SHELL_PAGE_APP;
    s.focus_mode = FOCUS_VIEWPORT;
    s.vp_focus.widget_index = 1;
    s.vp_focus.item_index = 0;
    s.text_input_widget = -1;
    s.text_buf = NULL; s.text_cap = 0;
    render_screen();
    ESP_LOGI("shell", "launched %s", app_id);
}

// --- Focus advance ----------------------------------------------- //

static void viewport_advance_focus(int step)
{
    int stops[8]; int n_stops = 0;
    switch (s.page) {
    case SHELL_PAGE_DESKTOP: {
        if (s.tile_count <= 0) return;
        s.tile_index = (s.tile_index + step + s.tile_count) % s.tile_count;
        return;
    }
    case SHELL_PAGE_SETTINGS: {
        const pgr_cbor_value_t *fr = pageros_apprt_foreground_frame();
        int n = focused_list_len(fr, 1);
        if (n > 0) {
            s.vp_focus.widget_index = 1;
            s.vp_focus.item_index = (s.vp_focus.item_index + step + n) % n;
        }
        return;
    }
    case SHELL_PAGE_WIFI:
        stops[n_stops++] = 2;
        stops[n_stops++] = 3;
        stops[n_stops++] = 4;
        stops[n_stops++] = 5;
        break;
    case SHELL_PAGE_ADD_APP: {
        int notif = s.addapp_msg[0] ? 1 : 0;
        int base = 2 + notif;
        stops[n_stops++] = base;
        stops[n_stops++] = base + 1;
        stops[n_stops++] = base + 2;
        break;
    }
    case SHELL_PAGE_APP: {
        // Generic: walk the body, focus advances by widget index, but
        // for list widgets we cycle items inside the same widget.
        const pgr_cbor_value_t *fr = pageros_apprt_foreground_frame();
        int n = focused_list_len(fr, s.vp_focus.widget_index);
        if (n > 0) {
            s.vp_focus.item_index = (s.vp_focus.item_index + step + n) % n;
        }
        return;
    }
    }
    if (n_stops == 0) return;
    int cur = 0;
    for (int i = 0; i < n_stops; i++) {
        if (stops[i] == s.vp_focus.widget_index) { cur = i; break; }
    }
    cur = (cur + step + n_stops) % n_stops;
    s.vp_focus.widget_index = stops[cur];
    s.vp_focus.item_index = 0;
    // Re-bind text-input buffer if the new focus is a text field.
    s.text_input_widget = -1; s.text_buf = NULL; s.text_cap = 0;
    if (s.page == SHELL_PAGE_WIFI) {
        if (s.vp_focus.widget_index == 2) { s.text_input_widget = 2; s.text_buf = s.ssid; s.text_cap = sizeof(s.ssid); }
        else if (s.vp_focus.widget_index == 3) { s.text_input_widget = 3; s.text_buf = s.psk; s.text_cap = sizeof(s.psk); }
    } else if (s.page == SHELL_PAGE_ADD_APP) {
        int url_idx = 2 + (s.addapp_msg[0] ? 1 : 0);
        if (s.vp_focus.widget_index == url_idx) {
            s.text_input_widget = url_idx; s.text_buf = s.url; s.text_cap = sizeof(s.url);
        }
    }
}

static void rail_advance(int step)
{
    int n = 1 + s.rail_count;          // HOME + open apps
    s.rail_index = (s.rail_index + step + n) % n;
}

// --- Input handler ----------------------------------------------- //

static bool default_shell_handler(const pageros_router_event_t *ev, void *ctx)
{
    (void)ctx;
    pageros_power_kick();

    const char *kind = "?";
    switch (ev->kind) {
    case PAGEROS_ROUTER_EVT_ENCODER: kind = "ENC"; break;
    case PAGEROS_ROUTER_EVT_NAV:     kind = "NAV"; break;
    case PAGEROS_ROUTER_EVT_KEY:     kind = "KEY"; break;
    default: kind = "NONE";
    }
    int detail = 0;
    if (ev->kind == PAGEROS_ROUTER_EVT_ENCODER) detail = ev->as.enc;
    else if (ev->kind == PAGEROS_ROUTER_EVT_NAV) detail = ev->as.nav;
    ESP_LOGI("shell", "EVT %s (%d) page=%d mode=%d rail=%d tile=%d",
             kind, detail, (int)s.page, (int)s.focus_mode,
             s.rail_index, s.tile_index);

    bool dirty = false;
    bool rail_visible = (s.page == SHELL_PAGE_DESKTOP || s.page == SHELL_PAGE_APP);

    switch (ev->kind) {
    case PAGEROS_ROUTER_EVT_ENCODER: {
        int step = (ev->as.enc == PAGEROS_ROUTER_ENC_CW) ? +1 : -1;
        if (rail_visible && s.focus_mode == FOCUS_RAIL) {
            rail_advance(step);
        } else {
            viewport_advance_focus(step);
        }
        pageros_audio_play_ui_scroll();
        dirty = true;
        break;
    }
    case PAGEROS_ROUTER_EVT_NAV: {
        if (ev->as.nav == PAGEROS_ROUTER_NAV_BACK_LONG) {
            pageros_audio_play_ui_click();
            mount_desktop();
            return true;
        }
        if (ev->as.nav == PAGEROS_ROUTER_NAV_ENTER) {
            pageros_audio_play_ui_click();
            if (rail_visible && s.focus_mode == FOCUS_RAIL) {
                // Activate rail item.
                if (s.rail_index == 0) {
                    // HOME — switch to desktop (or stay on it if already there).
                    if (s.page != SHELL_PAGE_DESKTOP) mount_desktop();
                    else {
                        s.focus_mode = FOCUS_VIEWPORT;
                        dirty = true;
                    }
                } else {
                    // Launch the focused open app.
                    int idx = s.rail_index - 1;
                    if (idx >= 0 && idx < s.rail_count && s.rail_items[idx]) {
                        char app_id[96];
                        strncpy(app_id, s.rail_items[idx], sizeof(app_id) - 1);
                        app_id[sizeof(app_id) - 1] = '\0';
                        launch_app(app_id);
                        return true;
                    }
                }
                break;
            }
            // Viewport activation.
            if (s.page == SHELL_PAGE_DESKTOP) {
                if (s.tile_index >= 0 && s.tile_index < s.tile_count) {
                    handle_button_href(s.tile_app_ids[s.tile_index]);
                    return true;
                }
                break;
            }
            const pgr_cbor_value_t *fr = pageros_apprt_foreground_frame();
            char href_buf[96];
            const char *href = focused_href(fr, s.vp_focus.widget_index,
                                            s.vp_focus.item_index,
                                            href_buf, sizeof(href_buf));
            if (href) handle_button_href(href);
            break;
        }
        if (ev->as.nav == PAGEROS_ROUTER_NAV_BACK) {
            // Text input mode: BACK = backspace.
            if (s.text_input_widget == s.vp_focus.widget_index && s.text_buf) {
                size_t L = strlen(s.text_buf);
                if (L > 0) { s.text_buf[L - 1] = '\0'; dirty = true; }
                pageros_audio_play_ui_scroll();
                break;
            }
            pageros_audio_play_ui_click();
            // From viewport on desktop/app → swap to rail focus.
            if (rail_visible && s.focus_mode == FOCUS_VIEWPORT) {
                s.focus_mode = FOCUS_RAIL;
                dirty = true;
                break;
            }
            // From rail focus on app → close app (drop fg, back to desktop).
            if (s.focus_mode == FOCUS_RAIL && s.page == SHELL_PAGE_APP) {
                mount_desktop();
                return true;
            }
            // Modal pages back-stack.
            switch (s.page) {
            case SHELL_PAGE_SETTINGS: mount_desktop();  dirty = true; break;
            case SHELL_PAGE_WIFI:
            case SHELL_PAGE_ADD_APP:  mount_settings(); dirty = true; break;
            default: break;
            }
        }
        break;
    }
    case PAGEROS_ROUTER_EVT_KEY: {
        if (!ev->as.key.pressed) return true;
        char ch = kb_decode(ev->as.key.row, ev->as.key.col);
        if (!s.text_buf) break;
        if (ch == '\b') {
            size_t L = strlen(s.text_buf);
            if (L > 0) { s.text_buf[L - 1] = '\0'; dirty = true; }
            pageros_audio_play_ui_scroll();
            break;
        }
        if (ch == '\n') {
            pageros_audio_play_ui_click();
            if (s.page == SHELL_PAGE_WIFI)      handle_button_href("shell:wifi-connect");
            else if (s.page == SHELL_PAGE_ADD_APP) handle_button_href("shell:install");
            return true;
        }
        if (ch == 0) break;
        size_t L = strlen(s.text_buf);
        if (L + 1 < s.text_cap) {
            s.text_buf[L] = ch;
            s.text_buf[L + 1] = '\0';
            dirty = true;
            pageros_audio_play_ui_scroll();
        }
        break;
    }
    default:
        return false;
    }

    if (dirty) {
        // Re-emit frame-backed pages so input updates show up.
        if (s.page == SHELL_PAGE_WIFI)    mount_and_set(emit_wifi_thunk);
        if (s.page == SHELL_PAGE_ADD_APP) mount_and_set(emit_addapp_thunk);
        render_screen();
    }
    return true;
}

// --- GPS callback ----------------------------------------------- //

static void on_gps_fix(const pageros_gps_fix_t *fix, void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI("gps", "fix: lat=%.6f lon=%.6f acc=%.1fm sats=%u",
             fix->latitude_deg, fix->longitude_deg,
             (double)fix->accuracy_m, (unsigned)fix->satellites);
}

// --- Boot --------------------------------------------------------- //

void app_main(void)
{
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    ESP_LOGI(TAG, "PagerOS %s starting", app ? app->version : "?");
    ESP_LOGI(TAG, "chip: ESP32-S%d, cores=%d, rev=%d", chip.model, chip.cores, chip.revision);
    ESP_LOGI(TAG, "flash: %lu MB", (unsigned long)(flash_size / (1024 * 1024)));
    ESP_LOGI(TAG, "running partition: %s @ 0x%08lx (%lu KB)",
             running ? running->label : "?",
             running ? (unsigned long)running->address : 0UL,
             running ? (unsigned long)(running->size / 1024) : 0UL);

    selftest_result_t st = selftest_run();
    selftest_log_result(&st);
    selftest_halt_if_hard_fail(&st);

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES
            || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs needs erase (%s); reformatting",
                 esp_err_to_name(nvs_err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    esp_err_t id_err = pageros_identity_init();
    if (id_err != ESP_OK) {
        ESP_LOGE(TAG, "identity init failed: %s", esp_err_to_name(id_err));
    }

    esp_err_t i2c_err = pageros_i2c_bus_init();
    if (i2c_err != ESP_OK) {
        ESP_LOGW(TAG, "shared I2C bus init failed: %s", esp_err_to_name(i2c_err));
    }
    esp_err_t xl_err = pageros_xl9555_init();
    if (xl_err == ESP_OK) {
        pageros_xl9555_set(PAGEROS_XL_LORA_EN,   true);
        pageros_xl9555_set(PAGEROS_XL_GPS_EN,    true);
        pageros_xl9555_set(PAGEROS_XL_GPS_RST,   true);
        pageros_xl9555_set(PAGEROS_XL_NFC_EN,    true);
        pageros_xl9555_set(PAGEROS_XL_SD_EN,     true);
        pageros_xl9555_set(PAGEROS_XL_SD_PULLEN, true);
        pageros_xl9555_set(PAGEROS_XL_KB_RST,    true);
        pageros_xl9555_set(PAGEROS_XL_KB_EN,     true);
        pageros_xl9555_set(PAGEROS_XL_AMP_EN,    true);
        pageros_xl9555_set(PAGEROS_XL_DRV_EN,    true);
        pageros_xl9555_set(PAGEROS_XL_GPIO_EN,   true);
        vTaskDelay(pdMS_TO_TICKS(80));
        ESP_LOGI(TAG, "XL9555 enables asserted");
    } else {
        ESP_LOGW(TAG, "XL9555 init failed: %s", esp_err_to_name(xl_err));
    }

    esp_err_t disp_err = pageros_display_init();
    if (disp_err == ESP_OK) {
        pageros_display_fill(pageros_display_rgb565(0x00, 0x00, 0x00));
        pageros_display_present();
    } else {
        ESP_LOGW(TAG, "display init failed: %s", esp_err_to_name(disp_err));
    }

    // SD mount remains lazy — first sideload/launch will mount it.
    ESP_LOGW(TAG, "[diag] SD init skipped — shell + display work without it");

    esp_err_t log_err = pageros_logger_init();
    if (log_err != ESP_OK) {
        ESP_LOGW(TAG, "logger init failed: %s", esp_err_to_name(log_err));
    } else {
        LOG_INFO(TAG, "logger online, sd=%s",
                 pageros_logger_is_writing_to_sd() ? "yes" : "no");
    }

    pageros_lora_config_t lora_cfg = {
        .band             = PAGEROS_LORA_BAND_868_MHZ,
        .bandwidth        = PAGEROS_LORA_BW_125_KHZ,
        .spreading_factor = 7,
        .coding_rate      = PAGEROS_LORA_CR_4_5,
        .tx_power_dbm     = 14,
        .preamble_symbols = 8,
        .explicit_header  = true,
        .crc_on           = true,
        .iq_inverted      = false,
    };
    esp_err_t lora_err = pageros_lora_init(&lora_cfg);
    if (lora_err == ESP_OK) {
        LOG_INFO(TAG, "LoRa ready (SX1262, 868 MHz, SF7/BW125)");
    } else {
        LOG_ERROR(TAG, "LoRa init failed: %s", esp_err_to_name(lora_err));
    }

    esp_err_t wifi_err = pageros_wifi_init();
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "wifi init failed: %s", esp_err_to_name(wifi_err));
    } else {
        char saved_ssid[33] = {0}, saved_psk[65] = {0};
        if (pageros_wifi_creds_load(saved_ssid, sizeof(saved_ssid),
                                    saved_psk, sizeof(saved_psk)) == ESP_OK
                && saved_ssid[0]) {
            ESP_LOGI(TAG, "Wi-Fi auto-connect to '%s'", saved_ssid);
            esp_err_t wc = pageros_wifi_connect(saved_ssid, saved_psk, 15000);
            if (wc != ESP_OK) {
                ESP_LOGW(TAG, "auto-connect failed: %s", esp_err_to_name(wc));
            }
        }
    }

    // Audio bring-up (FW-013) — required for click/scroll UI sounds.
    esp_err_t audio_err = pageros_audio_init();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "audio init failed: %s — UI sounds disabled",
                 esp_err_to_name(audio_err));
    }

    pageros_fonts_init(NULL);
    pageros_apprt_init(NULL);
    pageros_shell_init();
    pageros_widgets_cyberpunk_palette(&g_palette);

    if (disp_err == ESP_OK) {
        (void)pageros_display_panel_reinit();
        mount_desktop();
    }

    esp_err_t gps_err = pageros_gps_init();
    if (gps_err == ESP_OK) {
        pageros_gps_set_fix_callback(on_gps_fix, NULL);
    } else {
        ESP_LOGW(TAG, "GPS init skipped: %s", esp_err_to_name(gps_err));
    }

    esp_err_t input_err = pageros_input_init();
    if (input_err != ESP_OK) {
        ESP_LOGW(TAG, "input init skipped: %s", esp_err_to_name(input_err));
    }
    esp_err_t kbd_err = pageros_kbd_init();
    if (kbd_err != ESP_OK) {
        ESP_LOGW(TAG, "kbd init skipped: %s", esp_err_to_name(kbd_err));
    }
    esp_err_t imu_err = pageros_imu_init();
    if (imu_err != ESP_OK) {
        ESP_LOGW(TAG, "IMU init skipped: %s", esp_err_to_name(imu_err));
    }
    pageros_power_init(NULL);

    if (input_err == ESP_OK || kbd_err == ESP_OK) {
        esp_err_t r = pageros_router_init();
        if (r == ESP_OK) {
            pageros_router_set_shell_handler(default_shell_handler, NULL);
        } else {
            ESP_LOGW(TAG, "router init skipped: %s", esp_err_to_name(r));
        }
    }

    // The shell re-renders on every input event (default_shell_handler).
    // The clock + status indicators advance whenever the user interacts;
    // they're not animated between keystrokes. A periodic background
    // render would race the input-driven render on the framebuffer and
    // the shared SPI bus, so we keep rendering single-threaded.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
