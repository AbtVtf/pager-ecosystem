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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "driver/gpio.h"
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
#include "pageros_nfc.h"
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

    // Lock screen + built-in app pages — rendered directly (no Frame).
    SHELL_PAGE_LOCK,
    SHELL_PAGE_NOTIF,            // notification center
    SHELL_PAGE_QUICK,            // quick-settings overlay
    SHELL_PAGE_NOTES_LIST,
    SHELL_PAGE_NOTES_EDIT,
    SHELL_PAGE_SYSMON,
    SHELL_PAGE_IDQR,
    SHELL_PAGE_NFC,
    SHELL_PAGE_SET_PIN,
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

    // --- Lock screen ---------------------------------------------- //
    char  lock_pin_buf[5];        // user-entered PIN (4 digits max)
    char  lock_pin_stored[5];     // loaded from NVS on boot
    bool  lock_pin_required;      // true if a PIN is set
    int   lock_attempts;          // wrong-PIN streak (for back-off / lockout)

    // --- Notifications -------------------------------------------- //
    struct notif_entry {
        char    text[80];
        uint8_t level;            // pageros_toast_level_t
        int64_t timestamp_us;
    } notifs[20];
    int notif_count;
    int notif_focus;

    // --- Quick settings ------------------------------------------- //
    int  quick_focus;             // index in quick-toggle list

    // --- Notes ---------------------------------------------------- //
    char notes_files[12][32];
    int  notes_count;
    int  notes_focus;
    char notes_edit_buf[2048];
    char notes_edit_filename[32];
    int  notes_edit_scroll;

    // --- NFC ------------------------------------------------------ //
    char nfc_last_uid[32];
    char nfc_status[64];

    // --- Rail close-mode -------------------------------------------- //
    bool rail_close_mode;

    // --- Per-app cache (state preserved across rail switches) ------ //
    struct app_cache {
        char     id[96];                  // "" = unused slot
        bool     is_builtin;              // true → use last_page; false → frame_bytes
        uint8_t *frame_bytes;             // owned, malloc'd; external apps only
        size_t   frame_len;
        int      last_page;               // shell_page_t; built-in's last sub-page
        pageros_widgets_focus_t vp_focus;
        int64_t  last_touched_us;
    } app_cache[6];

    // Chrome cache — system uptime at boot, used for the clock display.
    uint64_t boot_unix_us;
} s = {
    .page         = SHELL_PAGE_DESKTOP,
    .focus_mode   = FOCUS_VIEWPORT,
    .tile_index   = 0,
    .rail_index   = 0,
    .text_input_widget = -1,
};

static void render_screen(void);

#define HUNT_PINS_MAX 2
extern const int g_hunt_pins[HUNT_PINS_MAX];
extern int       g_hunt_now[HUNT_PINS_MAX];

// --- Notifications ring buffer ------------------------------------- //
//
// System events (Wi-Fi connects, installs, errors) get appended here
// and surfaced both as transient toasts *and* persistently in the
// notification center page. Capacity is 20; oldest gets evicted.

static void notif_push(const char *text, pageros_toast_level_t level)
{
    if (!text || !*text) return;
    if (s.notif_count >= (int)(sizeof(s.notifs) / sizeof(s.notifs[0]))) {
        // Shift up.
        for (int i = 1; i < s.notif_count; i++) {
            s.notifs[i - 1] = s.notifs[i];
        }
        s.notif_count--;
    }
    strncpy(s.notifs[s.notif_count].text, text,
            sizeof(s.notifs[0].text) - 1);
    s.notifs[s.notif_count].text[sizeof(s.notifs[0].text) - 1] = '\0';
    s.notifs[s.notif_count].level        = (uint8_t)level;
    s.notifs[s.notif_count].timestamp_us = esp_timer_get_time();
    s.notif_count++;
}

// Convenience: push to ring AND show a toast at the same time. Most
// system events use this — it keeps the two stores in sync.
static void notif_announce(const char *text, pageros_toast_level_t level,
                           int toast_ms)
{
    notif_push(text, level);
    pageros_toast(text, level, toast_ms);
}

// --- Lock screen PIN storage -------------------------------------- //

#include "nvs.h"

static void lock_load_pin(void)
{
    s.lock_pin_buf[0]    = '\0';
    s.lock_pin_stored[0] = '\0';
    s.lock_pin_required  = false;
    nvs_handle_t h;
    if (nvs_open("pageros_lock", NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(s.lock_pin_stored);
    if (nvs_get_str(h, "pin", s.lock_pin_stored, &sz) == ESP_OK
            && s.lock_pin_stored[0] != '\0') {
        s.lock_pin_required = true;
    }
    nvs_close(h);
}

static esp_err_t lock_save_pin(const char *pin)
{
    nvs_handle_t h;
    esp_err_t r = nvs_open("pageros_lock", NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    if (!pin || !*pin) {
        nvs_erase_key(h, "pin");
        s.lock_pin_required = false;
        s.lock_pin_stored[0] = '\0';
    } else {
        nvs_set_str(h, "pin", pin);
        s.lock_pin_required = true;
        strncpy(s.lock_pin_stored, pin, sizeof(s.lock_pin_stored) - 1);
        s.lock_pin_stored[sizeof(s.lock_pin_stored) - 1] = '\0';
    }
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

// --- App cache + state preservation -------------------------------- //
//
// Rail-driven switching pretends each "open" app stays alive, but apprt
// only has one foreground slot at a time. The cache backs that illusion:
// when the user switches away from app X, we snapshot X's frame bytes
// (or current built-in sub-page) into a slot keyed by id; when they come
// back to X, we restore from the slot instead of re-fetching.

#define APP_CACHE_MAX (int)(sizeof(s.app_cache) / sizeof(s.app_cache[0]))

static struct app_cache *cache_find(const char *id)
{
    if (!id || !*id) return NULL;
    for (int i = 0; i < APP_CACHE_MAX; i++) {
        if (s.app_cache[i].id[0] != '\0'
                && strcmp(s.app_cache[i].id, id) == 0) {
            return &s.app_cache[i];
        }
    }
    return NULL;
}

static void cache_free_slot(struct app_cache *slot)
{
    if (!slot) return;
    if (slot->frame_bytes) {
        free(slot->frame_bytes);
        slot->frame_bytes = NULL;
    }
    slot->frame_len = 0;
    slot->id[0] = '\0';
    slot->is_builtin = false;
    slot->last_page = 0;
    memset(&slot->vp_focus, 0, sizeof(slot->vp_focus));
    slot->last_touched_us = 0;
}

static struct app_cache *cache_alloc(const char *id, bool is_builtin)
{
    if (!id || !*id) return NULL;
    struct app_cache *existing = cache_find(id);
    if (existing) {
        existing->last_touched_us = esp_timer_get_time();
        return existing;
    }
    // Empty slot.
    for (int i = 0; i < APP_CACHE_MAX; i++) {
        if (s.app_cache[i].id[0] == '\0') {
            strncpy(s.app_cache[i].id, id, sizeof(s.app_cache[i].id) - 1);
            s.app_cache[i].id[sizeof(s.app_cache[i].id) - 1] = '\0';
            s.app_cache[i].is_builtin = is_builtin;
            s.app_cache[i].last_touched_us = esp_timer_get_time();
            return &s.app_cache[i];
        }
    }
    // LRU eviction.
    int oldest = 0;
    for (int i = 1; i < APP_CACHE_MAX; i++) {
        if (s.app_cache[i].last_touched_us < s.app_cache[oldest].last_touched_us) {
            oldest = i;
        }
    }
    cache_free_slot(&s.app_cache[oldest]);
    strncpy(s.app_cache[oldest].id, id, sizeof(s.app_cache[oldest].id) - 1);
    s.app_cache[oldest].id[sizeof(s.app_cache[oldest].id) - 1] = '\0';
    s.app_cache[oldest].is_builtin = is_builtin;
    s.app_cache[oldest].last_touched_us = esp_timer_get_time();
    return &s.app_cache[oldest];
}

// Identify the cache id of whatever is foregrounded right now. NULL
// means "not cacheable" (desktop, settings, wifi, addapp, lock, etc.)
static const char *current_cache_id(bool *out_is_builtin)
{
    if (out_is_builtin) *out_is_builtin = false;
    switch (s.page) {
    case SHELL_PAGE_APP:
        return pageros_apprt_foreground_id();
    case SHELL_PAGE_NOTES_LIST:
    case SHELL_PAGE_NOTES_EDIT:
        if (out_is_builtin) *out_is_builtin = true;
        return "builtin:notes";
    case SHELL_PAGE_SYSMON:
        if (out_is_builtin) *out_is_builtin = true;
        return "builtin:sysmon";
    case SHELL_PAGE_IDQR:
        if (out_is_builtin) *out_is_builtin = true;
        return "builtin:idqr";
    case SHELL_PAGE_NFC:
        if (out_is_builtin) *out_is_builtin = true;
        return "builtin:nfc";
    case SHELL_PAGE_NOTIF:
        if (out_is_builtin) *out_is_builtin = true;
        return "builtin:notif";
    case SHELL_PAGE_QUICK:
        if (out_is_builtin) *out_is_builtin = true;
        return "builtin:quick";
    default:
        return NULL;
    }
}

static void cache_snapshot_current(void)
{
    bool is_builtin = false;
    const char *id = current_cache_id(&is_builtin);
    if (!id || !*id) return;
    struct app_cache *slot = cache_alloc(id, is_builtin);
    if (!slot) return;
    slot->vp_focus = s.vp_focus;
    slot->last_page = (int)s.page;
    slot->last_touched_us = esp_timer_get_time();
    if (!is_builtin) {
        // External app — also keep the raw frame bytes so we can rehydrate
        // apprt without a network round-trip. We don't have direct access
        // to apprt's internal raw buffer, so this gets populated by the
        // launch path that fetched the bytes in the first place.
    }
}

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

    // Built-in tiles always come first so the user can find them
    // before any sideloaded apps clutter the grid. Each maps to a
    // "builtin:X" href routed by handle_button_href.
    struct { const char *href; const char *name; } builtin[] = {
        { "shell:settings",  "SETTINGS" },
        { "builtin:notif",   "NOTIFS"   },
        { "builtin:quick",   "QUICK"    },
        { "builtin:notes",   "NOTES"    },
        { "builtin:sysmon",  "SYSMON"   },
        { "builtin:idqr",    "IDENTITY" },
        { "builtin:nfc",     "NFC"      },
    };
    int n_builtin = (int)(sizeof(builtin) / sizeof(builtin[0]));
    for (int i = 0; i < n_builtin && s.tile_count < MAX_DESKTOP_TILES; i++) {
        strncpy(s.tile_app_ids[s.tile_count], builtin[i].href,
                sizeof(s.tile_app_ids[0]) - 1);
        s.tile_app_ids[s.tile_count][sizeof(s.tile_app_ids[0]) - 1] = '\0';
        strncpy(s.tile_names[s.tile_count], builtin[i].name,
                sizeof(s.tile_names[0]) - 1);
        s.tile_names[s.tile_count][sizeof(s.tile_names[0]) - 1] = '\0';
        s.tiles[s.tile_count].name   = s.tile_names[s.tile_count];
        // Surface the unread count as a badge on the NOTIFS tile.
        s.tiles[s.tile_count].unread =
            (strcmp(builtin[i].href, "builtin:notif") == 0) ? s.notif_count : 0;
        s.tile_count++;
    }

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
        .close_mode  = s.rail_close_mode && s.focus_mode == FOCUS_RAIL,
    };
    for (int i = 0; i < s.rail_count; i++) {
        rail.items[i] = s.rail_items[i];
    }
    pageros_widgets_chrome_rail(canvas, &g_palette, &rail);
}

static void render_desktop(pageros_fonts_canvas_t *canvas,
                           pageros_chrome_state_t *cs)
{
    pageros_widgets_fill_rect(canvas, 0, 0, canvas->w, canvas->h, g_palette.bg);

    // When the viewport is focused on the desktop, show the focused
    // tile's name in the bottom-bar's right slot instead of PWR mode —
    // gives the user a live "you are hovering X" indicator while they
    // scroll through icon-only tiles.
    if (s.focus_mode == FOCUS_VIEWPORT &&
        s.tile_index >= 0 && s.tile_index < s.tile_count) {
        cs->focused_label = s.tiles[s.tile_index].name;
    }

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
                               pageros_chrome_state_t *cs)
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
                       pageros_chrome_state_t *cs)
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

// --- Built-in app + lock screen renders ---------------------------- //

static void render_lock(pageros_fonts_canvas_t *canvas)
{
    pageros_widgets_fill_rect(canvas, 0, 0, canvas->w, canvas->h, g_palette.bg);
    pageros_widgets_chrome_scanlines(canvas, 0, 0, canvas->w, canvas->h, 3);

    // Clock huge & centered.
    int64_t up_us = esp_timer_get_time();
    int up_min   = (int)(up_us / 1000000 / 60);
    int hh       = (up_min / 60) % 24;
    int mm       = up_min % 60;
    char clock[8];
    snprintf(clock, sizeof(clock), "%02d:%02d", hh, mm);
    int clock_w = 16 * 3 * (int)strlen(clock);
    int cx = (canvas->w - clock_w) / 2;
    pageros_fonts_draw_text_scaled_16(canvas, cx, 30, clock, -1,
                                      g_palette.info, 3);

    // Identity below clock.
    char fp[PAGEROS_IDENTITY_FP_LEN] = {0};
    if (pageros_identity_fingerprint(fp) == ESP_OK && fp[0]) {
        char buf[24];
        snprintf(buf, sizeof(buf), "ID:%s", fp);
        int bw = pageros_fonts_measure_text_16(buf, -1);
        pageros_fonts_draw_text_16(canvas, (canvas->w - bw) / 2,
                                   30 + 48 + 8, buf, -1,
                                   g_palette.dim, canvas->w);
    }

    // PIN entry box (centered, below identity).
    int box_y = 30 + 48 + 8 + 28;
    if (s.lock_pin_required) {
        // 4 cells, 32px wide each, 8px gap. Total width = 4*32 + 3*8 = 152.
        int total = 4 * 32 + 3 * 8;
        int x0 = (canvas->w - total) / 2;
        int filled = (int)strlen(s.lock_pin_buf);
        for (int i = 0; i < 4; i++) {
            int cx0 = x0 + i * (32 + 8);
            pageros_widgets_outline_rect(canvas, cx0, box_y, 32, 32,
                                         g_palette.info);
            if (i < filled) {
                // Filled dot.
                pageros_widgets_fill_rect(canvas, cx0 + 10, box_y + 10,
                                          12, 12, g_palette.accent);
            }
        }
        const char *prompt = "ENTER PIN";
        int pw = pageros_fonts_measure_text(prompt, -1);
        pageros_fonts_draw_text(canvas, (canvas->w - pw) / 2,
                                box_y + 32 + 12, prompt, -1,
                                g_palette.dim, canvas->w);
    } else {
        const char *prompt = "PRESS ENTER TO UNLOCK";
        int pw = pageros_fonts_measure_text_16(prompt, -1);
        pageros_fonts_draw_text_16(canvas, (canvas->w - pw) / 2,
                                   box_y + 8, prompt, -1,
                                   g_palette.accent, canvas->w);
    }
}

static void render_notif(pageros_fonts_canvas_t *canvas,
                         pageros_chrome_state_t *cs)
{
    pageros_widgets_fill_rect(canvas, 0, 0, canvas->w, canvas->h, g_palette.bg);
    cs->focused_label = NULL;
    pageros_widgets_chrome_topbar(canvas, &g_palette, cs);
    pageros_widgets_chrome_botbar(canvas, &g_palette, cs);
    draw_rail(canvas);

    int x = PAGEROS_CHROME_RAIL_W + 12;
    int y = PAGEROS_CHROME_BAR_H + 8;
    const char *title = "NOTIFICATIONS";
    pageros_fonts_draw_text_16(canvas, x, y, title, -1, g_palette.accent,
                               canvas->w - 20);
    y += 22;
    // Underline.
    pageros_widgets_fill_rect(canvas, x, y - 4, canvas->w - 24, 1,
                              g_palette.info);

    if (s.notif_count == 0) {
        pageros_fonts_draw_text_16(canvas, x, y + 16, "NO MESSAGES", -1,
                                   g_palette.dim, canvas->w);
        return;
    }
    int row_h = 14;
    int max_rows = (canvas->h - y - PAGEROS_CHROME_BAR_H - 8) / row_h;
    if (max_rows < 1) max_rows = 1;
    int start = s.notif_count - max_rows;
    if (start < 0) start = 0;
    // Show newest at top → iterate from end.
    int row = 0;
    for (int i = s.notif_count - 1; i >= 0 && row < max_rows; i--, row++) {
        bool focused = (i == s.notif_focus);
        if (focused) {
            pageros_widgets_fill_rect(canvas, x - 4, y + row * row_h - 2,
                                      canvas->w - 16, row_h,
                                      g_palette.accent);
        }
        uint16_t fg = focused ? g_palette.bg : g_palette.fg;
        if (s.notifs[i].level == PAGEROS_TOAST_OK)    fg = focused ? g_palette.bg : g_palette.info;
        if (s.notifs[i].level == PAGEROS_TOAST_WARN)  fg = focused ? g_palette.bg : g_palette.warn;
        if (s.notifs[i].level == PAGEROS_TOAST_ERROR) fg = focused ? g_palette.bg : g_palette.error;
        // Timestamp (uptime mins).
        int mins = (int)(s.notifs[i].timestamp_us / 1000000 / 60);
        char prefix[24];
        snprintf(prefix, sizeof(prefix), "[%02d:%02d] ",
                 (mins / 60) % 24, mins % 60);
        int px = pageros_fonts_draw_text(canvas, x, y + row * row_h + 3,
                                         prefix, -1, fg, 80);
        pageros_fonts_draw_text(canvas, px, y + row * row_h + 3,
                                s.notifs[i].text, -1, fg,
                                canvas->w - x - 16);
    }
}

static void render_quick(pageros_fonts_canvas_t *canvas,
                         pageros_chrome_state_t *cs)
{
    pageros_widgets_fill_rect(canvas, 0, 0, canvas->w, canvas->h, g_palette.bg);
    cs->focused_label = NULL;
    pageros_widgets_chrome_topbar(canvas, &g_palette, cs);
    pageros_widgets_chrome_botbar(canvas, &g_palette, cs);
    draw_rail(canvas);

    int x = PAGEROS_CHROME_RAIL_W + 12;
    int y = PAGEROS_CHROME_BAR_H + 8;
    pageros_fonts_draw_text_16(canvas, x, y, "QUICK SETTINGS", -1,
                               g_palette.accent, canvas->w - 20);
    y += 24;
    pageros_widgets_fill_rect(canvas, x, y - 4, canvas->w - 24, 1,
                              g_palette.info);

    struct { const char *label; bool state; const char *on; const char *off; }
        toggles[] = {
            { "SILENT MODE",     pageros_audio_is_muted(),  "ON",      "OFF" },
            { "WI-FI",           pageros_wifi_is_connected(),"ON",     "OFF" },
            { "LORA RADIO",      pageros_lora_is_ready(),   "READY",   "OFF" },
            { "LOCK NOW",        false,                     "—",       "—"   },
            { "PIN",             s.lock_pin_required,       "SET",     "OFF" },
        };
    int n = sizeof(toggles) / sizeof(toggles[0]);
    int row_h = 24;
    for (int i = 0; i < n; i++) {
        int row_y = y + i * row_h;
        bool focused = (i == s.quick_focus);
        if (focused) {
            pageros_widgets_fill_rect(canvas, x - 4, row_y - 2,
                                      canvas->w - 16, row_h,
                                      g_palette.accent);
        }
        uint16_t fg = focused ? g_palette.bg : g_palette.fg;
        pageros_fonts_draw_text_16(canvas, x, row_y + 2,
                                   toggles[i].label, -1, fg, canvas->w);
        const char *val = toggles[i].state ? toggles[i].on : toggles[i].off;
        int vw = pageros_fonts_measure_text_16(val, -1);
        pageros_fonts_draw_text_16(canvas, canvas->w - vw - 24,
                                   row_y + 2, val, -1,
                                   focused ? g_palette.bg : g_palette.info,
                                   canvas->w);
    }
}

#include <dirent.h>
static void notes_refresh_list(void)
{
    s.notes_count = 0;
    DIR *d = opendir("/sd/notes");
    if (!d) {
        // Try to create the directory.
        mkdir("/sd/notes", 0777);
        d = opendir("/sd/notes");
    }
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s.notes_count < 12) {
        if (e->d_name[0] == '.') continue;
        strncpy(s.notes_files[s.notes_count], e->d_name,
                sizeof(s.notes_files[0]) - 1);
        s.notes_files[s.notes_count][sizeof(s.notes_files[0]) - 1] = '\0';
        s.notes_count++;
    }
    closedir(d);
    if (s.notes_focus >= s.notes_count + 1) s.notes_focus = 0;
}

static void notes_load(const char *fname)
{
    strncpy(s.notes_edit_filename, fname,
            sizeof(s.notes_edit_filename) - 1);
    s.notes_edit_filename[sizeof(s.notes_edit_filename) - 1] = '\0';
    s.notes_edit_buf[0] = '\0';
    char path[96];
    snprintf(path, sizeof(path), "/sd/notes/%s", fname);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t n = fread(s.notes_edit_buf, 1, sizeof(s.notes_edit_buf) - 1, f);
    s.notes_edit_buf[n] = '\0';
    fclose(f);
}

static void notes_save_current(void)
{
    if (!s.notes_edit_filename[0]) return;
    char path[96];
    snprintf(path, sizeof(path), "/sd/notes/%s", s.notes_edit_filename);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(s.notes_edit_buf, 1, strlen(s.notes_edit_buf), f);
    fclose(f);
}

static void render_notes_list(pageros_fonts_canvas_t *canvas,
                              pageros_chrome_state_t *cs)
{
    pageros_widgets_fill_rect(canvas, 0, 0, canvas->w, canvas->h, g_palette.bg);
    cs->focused_label = NULL;
    pageros_widgets_chrome_topbar(canvas, &g_palette, cs);
    pageros_widgets_chrome_botbar(canvas, &g_palette, cs);
    draw_rail(canvas);

    int x = PAGEROS_CHROME_RAIL_W + 12, y = PAGEROS_CHROME_BAR_H + 8;
    pageros_fonts_draw_text_16(canvas, x, y, "NOTES", -1,
                               g_palette.accent, canvas->w - 20);
    y += 24;
    pageros_widgets_fill_rect(canvas, x, y - 4, canvas->w - 24, 1,
                              g_palette.info);

    int row_h = 18;
    // Row 0 is "+ NEW NOTE". Rows 1..n are files.
    int total = 1 + s.notes_count;
    for (int i = 0; i < total; i++) {
        int row_y = y + i * row_h;
        if (row_y + row_h > canvas->h - PAGEROS_CHROME_BAR_H) break;
        bool focused = (i == s.notes_focus);
        if (focused) {
            pageros_widgets_fill_rect(canvas, x - 4, row_y - 2,
                                      canvas->w - 16, row_h,
                                      g_palette.accent);
        }
        uint16_t fg = focused ? g_palette.bg : g_palette.fg;
        const char *label = (i == 0) ? "+ NEW NOTE" : s.notes_files[i - 1];
        pageros_fonts_draw_text(canvas, x, row_y + 4, label, -1, fg,
                                canvas->w - x - 16);
    }
}

static void render_notes_edit(pageros_fonts_canvas_t *canvas,
                              pageros_chrome_state_t *cs)
{
    pageros_widgets_fill_rect(canvas, 0, 0, canvas->w, canvas->h, g_palette.bg);
    cs->focused_label = NULL;
    pageros_widgets_chrome_topbar(canvas, &g_palette, cs);
    pageros_widgets_chrome_botbar(canvas, &g_palette, cs);
    draw_rail(canvas);

    int x = PAGEROS_CHROME_RAIL_W + 8, y = PAGEROS_CHROME_BAR_H + 4;
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "[%s]", s.notes_edit_filename);
    pageros_fonts_draw_text(canvas, x, y, hdr, -1, g_palette.accent,
                            canvas->w - 16);
    y += 12;
    pageros_widgets_fill_rect(canvas, x, y - 2, canvas->w - 16, 1,
                              g_palette.info);

    // Render buffer as wrapped text. Simple: 60 chars per line.
    const char *p = s.notes_edit_buf;
    int line_h = 10;
    int line_y = y + 2;
    char line[64];
    int col = 0;
    while (*p) {
        if (*p == '\n' || col >= 60) {
            line[col] = '\0';
            if (line_y + line_h < canvas->h - PAGEROS_CHROME_BAR_H) {
                pageros_fonts_draw_text(canvas, x, line_y, line, -1,
                                        g_palette.fg, canvas->w - 16);
            }
            line_y += line_h;
            col = 0;
            if (*p == '\n') p++;
            continue;
        }
        line[col++] = *p++;
    }
    if (col > 0) {
        line[col] = '\0';
        if (line_y + line_h < canvas->h - PAGEROS_CHROME_BAR_H) {
            pageros_fonts_draw_text(canvas, x, line_y, line, -1,
                                    g_palette.fg, canvas->w - 16);
        }
        line_y += line_h;
    }
    // Cursor at end of buffer.
    pageros_widgets_fill_rect(canvas, x + col * 8, line_y - line_h, 1,
                              line_h, g_palette.accent);
}

#include "esp_heap_caps.h"

static void render_sysmon(pageros_fonts_canvas_t *canvas,
                          pageros_chrome_state_t *cs)
{
    pageros_widgets_fill_rect(canvas, 0, 0, canvas->w, canvas->h, g_palette.bg);
    cs->focused_label = NULL;
    pageros_widgets_chrome_topbar(canvas, &g_palette, cs);
    pageros_widgets_chrome_botbar(canvas, &g_palette, cs);
    draw_rail(canvas);

    int x = PAGEROS_CHROME_RAIL_W + 12, y = PAGEROS_CHROME_BAR_H + 8;
    pageros_fonts_draw_text_16(canvas, x, y, "SYSTEM MONITOR", -1,
                               g_palette.accent, canvas->w - 20);
    y += 24;
    pageros_widgets_fill_rect(canvas, x, y - 4, canvas->w - 24, 1,
                              g_palette.info);

    int line_h = 12;
    char buf[80];

    // Uptime
    int64_t up_us = esp_timer_get_time();
    int up_s = (int)(up_us / 1000000);
    snprintf(buf, sizeof(buf), "UPTIME    %02d:%02d:%02d",
             up_s / 3600, (up_s / 60) % 60, up_s % 60);
    pageros_fonts_draw_text(canvas, x, y, buf, -1, g_palette.fg, canvas->w);
    y += line_h;

    // Heap
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t total_internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    snprintf(buf, sizeof(buf), "HEAP      %u / %u KB",
             (unsigned)(free_internal / 1024),
             (unsigned)(total_internal / 1024));
    pageros_fonts_draw_text(canvas, x, y, buf, -1, g_palette.fg, canvas->w);
    y += line_h;

    // PSRAM
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    snprintf(buf, sizeof(buf), "PSRAM     %u / %u KB",
             (unsigned)(free_psram / 1024),
             (unsigned)(total_psram / 1024));
    pageros_fonts_draw_text(canvas, x, y, buf, -1, g_palette.fg, canvas->w);
    y += line_h;

    // Wi-Fi
    snprintf(buf, sizeof(buf), "WI-FI     %s",
             pageros_wifi_is_connected() ? "CONNECTED" : "OFF");
    pageros_fonts_draw_text(canvas, x, y, buf, -1, g_palette.fg, canvas->w);
    y += line_h;

    // LoRa
    snprintf(buf, sizeof(buf), "LORA      %s",
             pageros_lora_is_ready() ? "READY" : "OFF");
    pageros_fonts_draw_text(canvas, x, y, buf, -1, g_palette.fg, canvas->w);
    y += line_h;

    // GPS
    pageros_gps_fix_t fix = {0};
    bool fixed = (pageros_gps_get_last_fix(&fix) == ESP_OK
                  && fix.accuracy_m > 0.0f);
    snprintf(buf, sizeof(buf), "GPS       %s",
             fixed ? "FIX" : "NO FIX");
    pageros_fonts_draw_text(canvas, x, y, buf, -1, g_palette.fg, canvas->w);
    y += line_h;

    // Tasks
    snprintf(buf, sizeof(buf), "TASKS     %u",
             (unsigned)uxTaskGetNumberOfTasks());
    pageros_fonts_draw_text(canvas, x, y, buf, -1, g_palette.fg, canvas->w);
    y += line_h;

    // Notifications
    snprintf(buf, sizeof(buf), "NOTIFS    %d", s.notif_count);
    pageros_fonts_draw_text(canvas, x, y, buf, -1, g_palette.fg, canvas->w);
    y += line_h;

    // Open apps
    snprintf(buf, sizeof(buf), "OPEN APPS %d", s.rail_count);
    pageros_fonts_draw_text(canvas, x, y, buf, -1, g_palette.fg, canvas->w);
    y += line_h;

    // GPIO hunter — live levels of the candidate "right button" pins.
    // Press the unknown button and watch which value flips.
    pageros_widgets_fill_rect(canvas, x, y + 4, canvas->w - x - 12, 1,
                              g_palette.dim);
    y += 8;
    pageros_fonts_draw_text(canvas, x, y, "GPIO HUNT", -1,
                            g_palette.accent, canvas->w);
    y += line_h;
    for (int i = 0; i < HUNT_PINS_MAX; i++) {
        snprintf(buf, sizeof(buf), "  GPIO%-2d   %s",
                 g_hunt_pins[i], g_hunt_now[i] ? "HIGH" : "LOW");
        uint16_t col = g_hunt_now[i] ? g_palette.fg : g_palette.accent;
        pageros_fonts_draw_text(canvas, x, y, buf, -1, col, canvas->w);
        y += line_h;
    }
}

static void render_idqr(pageros_fonts_canvas_t *canvas,
                        pageros_chrome_state_t *cs)
{
    pageros_widgets_fill_rect(canvas, 0, 0, canvas->w, canvas->h, g_palette.bg);
    cs->focused_label = NULL;
    pageros_widgets_chrome_topbar(canvas, &g_palette, cs);
    pageros_widgets_chrome_botbar(canvas, &g_palette, cs);
    draw_rail(canvas);

    int x = PAGEROS_CHROME_RAIL_W + 12, y = PAGEROS_CHROME_BAR_H + 8;
    pageros_fonts_draw_text_16(canvas, x, y, "DEVICE IDENTITY", -1,
                               g_palette.accent, canvas->w - 20);
    y += 24;
    pageros_widgets_fill_rect(canvas, x, y - 4, canvas->w - 24, 1,
                              g_palette.info);

    // Fingerprint big and chunky.
    char fp[PAGEROS_IDENTITY_FP_LEN] = {0};
    pageros_identity_fingerprint(fp);
    if (fp[0]) {
        int fp_w = 16 * 2 * (int)strlen(fp);
        int fpx = (canvas->w - fp_w) / 2;
        pageros_fonts_draw_text_scaled_16(canvas, fpx, y + 4, fp, -1,
                                          g_palette.info, 2);
    }
    y += 44;
    // Label
    const char *lbl = "FINGERPRINT (BASE32)";
    int lw = pageros_fonts_measure_text(lbl, -1);
    pageros_fonts_draw_text(canvas, (canvas->w - lw) / 2, y,
                            lbl, -1, g_palette.dim, canvas->w);
    y += 14;

    // Pubkey as hex (4 rows of 16 hex chars = 64 hex chars = 32 bytes)
    uint8_t pub[PAGEROS_IDENTITY_PUBKEY_LEN] = {0};
    if (pageros_identity_pubkey(pub) == ESP_OK) {
        char line[64];
        for (int row = 0; row < 4; row++) {
            int pos = 0;
            for (int col = 0; col < 8; col++) {
                int off = row * 8 + col;
                pos += snprintf(line + pos, sizeof(line) - pos,
                                "%02X ", pub[off]);
            }
            int lwi = pageros_fonts_measure_text(line, -1);
            pageros_fonts_draw_text(canvas, (canvas->w - lwi) / 2,
                                    y, line, -1, g_palette.fg, canvas->w);
            y += 11;
        }
    }
    // Footer hint
    const char *hint = "SCAN OR COPY TO PAIR";
    int hw = pageros_fonts_measure_text(hint, -1);
    pageros_fonts_draw_text(canvas, (canvas->w - hw) / 2,
                            canvas->h - PAGEROS_CHROME_BAR_H - 14,
                            hint, -1, g_palette.dim, canvas->w);
}

static void render_set_pin(pageros_fonts_canvas_t *canvas,
                           pageros_chrome_state_t *cs)
{
    pageros_widgets_fill_rect(canvas, 0, 0, canvas->w, canvas->h, g_palette.bg);
    cs->focused_label = NULL;
    pageros_widgets_chrome_topbar(canvas, &g_palette, cs);
    pageros_widgets_chrome_botbar(canvas, &g_palette, cs);

    int y = PAGEROS_CHROME_BAR_H + 16;
    const char *title = s.lock_pin_required ? "CHANGE PIN" : "SET NEW PIN";
    int tw = pageros_fonts_measure_text_16(title, -1);
    pageros_fonts_draw_text_16(canvas, (canvas->w - tw) / 2, y, title, -1,
                               g_palette.accent, canvas->w);
    y += 30;
    const char *sub = "TYPE 4 DIGITS, THEN ENTER";
    int sw = pageros_fonts_measure_text(sub, -1);
    pageros_fonts_draw_text(canvas, (canvas->w - sw) / 2, y, sub, -1,
                            g_palette.dim, canvas->w);
    y += 26;

    int total = 4 * 32 + 3 * 8;
    int x0 = (canvas->w - total) / 2;
    int filled = (int)strlen(s.lock_pin_buf);
    for (int i = 0; i < 4; i++) {
        int cx0 = x0 + i * (32 + 8);
        pageros_widgets_outline_rect(canvas, cx0, y, 32, 32, g_palette.info);
        if (i < filled) {
            // Echo the digit (it's not secret yet — user is setting it).
            char ch[2] = { s.lock_pin_buf[i], '\0' };
            pageros_fonts_draw_glyph_scaled_16(canvas, cx0 + 8, y + 8,
                                               (uint32_t)ch[0],
                                               g_palette.fg, 1);
            pageros_widgets_fill_rect(canvas, cx0 + 8, y + 24, 16, 2,
                                      g_palette.accent);
        }
    }
    y += 32 + 16;
    const char *hint = "BACK = CANCEL · BACKSPACE = CLEAR";
    int hw = pageros_fonts_measure_text(hint, -1);
    pageros_fonts_draw_text(canvas, (canvas->w - hw) / 2, y, hint, -1,
                            g_palette.dim, canvas->w);
}

static void render_nfc(pageros_fonts_canvas_t *canvas,
                       pageros_chrome_state_t *cs)
{
    pageros_widgets_fill_rect(canvas, 0, 0, canvas->w, canvas->h, g_palette.bg);
    cs->focused_label = NULL;
    pageros_widgets_chrome_topbar(canvas, &g_palette, cs);
    pageros_widgets_chrome_botbar(canvas, &g_palette, cs);
    draw_rail(canvas);

    int x = PAGEROS_CHROME_RAIL_W + 12, y = PAGEROS_CHROME_BAR_H + 8;
    pageros_fonts_draw_text_16(canvas, x, y, "NFC SCANNER", -1,
                               g_palette.accent, canvas->w - 20);
    y += 24;
    pageros_widgets_fill_rect(canvas, x, y - 4, canvas->w - 24, 1,
                              g_palette.info);

    // Centered "TAP A TAG" instruction or last UID.
    if (s.nfc_last_uid[0]) {
        const char *lbl = "LAST TAG UID";
        int lw = pageros_fonts_measure_text(lbl, -1);
        pageros_fonts_draw_text(canvas, (canvas->w - lw) / 2,
                                y + 12, lbl, -1, g_palette.dim, canvas->w);
        int uw = 16 * 2 * (int)strlen(s.nfc_last_uid);
        if (uw > canvas->w - 40) uw = canvas->w - 40;
        pageros_fonts_draw_text_scaled_16(canvas,
                                          (canvas->w - uw) / 2, y + 30,
                                          s.nfc_last_uid, -1,
                                          g_palette.info, 2);
    } else {
        const char *prompt = "TAP A TAG TO SCAN";
        int pw = pageros_fonts_measure_text_16(prompt, -1);
        pageros_fonts_draw_text_16(canvas, (canvas->w - pw) / 2,
                                   y + 32, prompt, -1,
                                   g_palette.accent, canvas->w);
    }

    // Status line.
    if (s.nfc_status[0]) {
        int sw = pageros_fonts_measure_text(s.nfc_status, -1);
        pageros_fonts_draw_text(canvas, (canvas->w - sw) / 2,
                                canvas->h - PAGEROS_CHROME_BAR_H - 14,
                                s.nfc_status, -1, g_palette.dim, canvas->w);
    }
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
    case SHELL_PAGE_LOCK:      render_lock(&canvas); break;
    case SHELL_PAGE_NOTIF:     render_notif(&canvas, &cs); break;
    case SHELL_PAGE_QUICK:     render_quick(&canvas, &cs); break;
    case SHELL_PAGE_NOTES_LIST: render_notes_list(&canvas, &cs); break;
    case SHELL_PAGE_NOTES_EDIT: render_notes_edit(&canvas, &cs); break;
    case SHELL_PAGE_SYSMON:    render_sysmon(&canvas, &cs); break;
    case SHELL_PAGE_IDQR:      render_idqr(&canvas, &cs); break;
    case SHELL_PAGE_NFC:       render_nfc(&canvas, &cs); break;
    case SHELL_PAGE_SET_PIN:   render_set_pin(&canvas, &cs); break;
    }
    // Toast banner floats above everything (after content + scanlines).
    pageros_widgets_chrome_toast(&canvas, &g_palette);
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
    s.rail_close_mode = false;
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

static void mount_lock(void)
{
    ESP_LOGI("shell", "mount_lock");
    s.page = SHELL_PAGE_LOCK;
    s.focus_mode = FOCUS_VIEWPORT;
    s.lock_pin_buf[0] = '\0';
    s.text_input_widget = -1;
    s.text_buf = NULL; s.text_cap = 0;
    render_screen();
}

static void mount_notif(void)
{
    ESP_LOGI("shell", "mount_notif");
    s.page = SHELL_PAGE_NOTIF;
    s.focus_mode = FOCUS_VIEWPORT;
    s.notif_focus = (s.notif_count > 0) ? (s.notif_count - 1) : 0;
    s.text_input_widget = -1;
    s.text_buf = NULL; s.text_cap = 0;
    render_screen();
}

static void mount_quick(void)
{
    ESP_LOGI("shell", "mount_quick");
    s.page = SHELL_PAGE_QUICK;
    s.focus_mode = FOCUS_VIEWPORT;
    s.quick_focus = 0;
    s.text_input_widget = -1;
    s.text_buf = NULL; s.text_cap = 0;
    render_screen();
}

static void mount_notes_list(void)
{
    ESP_LOGI("shell", "mount_notes_list");
    pageros_storage_init();
    notes_refresh_list();
    s.page = SHELL_PAGE_NOTES_LIST;
    s.focus_mode = FOCUS_VIEWPORT;
    s.notes_focus = 0;
    s.text_input_widget = -1;
    s.text_buf = NULL; s.text_cap = 0;
    render_screen();
}

static void mount_notes_edit(const char *filename)
{
    ESP_LOGI("shell", "mount_notes_edit: %s", filename ? filename : "(new)");
    pageros_storage_init();
    if (filename && *filename) {
        notes_load(filename);
    } else {
        // New note — name by uptime tick.
        int64_t us = esp_timer_get_time();
        snprintf(s.notes_edit_filename, sizeof(s.notes_edit_filename),
                 "note_%llu.txt", (unsigned long long)(us / 1000));
        s.notes_edit_buf[0] = '\0';
    }
    s.page = SHELL_PAGE_NOTES_EDIT;
    s.focus_mode = FOCUS_VIEWPORT;
    s.text_input_widget = 0;
    s.text_buf = s.notes_edit_buf;
    s.text_cap = sizeof(s.notes_edit_buf);
    render_screen();
}

static void mount_sysmon(void)
{
    ESP_LOGI("shell", "mount_sysmon");
    s.page = SHELL_PAGE_SYSMON;
    s.focus_mode = FOCUS_VIEWPORT;
    s.text_input_widget = -1;
    s.text_buf = NULL; s.text_cap = 0;
    render_screen();
}

static void mount_idqr(void)
{
    ESP_LOGI("shell", "mount_idqr");
    s.page = SHELL_PAGE_IDQR;
    s.focus_mode = FOCUS_VIEWPORT;
    s.text_input_widget = -1;
    s.text_buf = NULL; s.text_cap = 0;
    render_screen();
}

static void mount_nfc(void)
{
    ESP_LOGI("shell", "mount_nfc");
    s.page = SHELL_PAGE_NFC;
    s.focus_mode = FOCUS_VIEWPORT;
    strncpy(s.nfc_status, "RADIO READY", sizeof(s.nfc_status));
    s.nfc_status[sizeof(s.nfc_status) - 1] = '\0';
    s.text_input_widget = -1;
    s.text_buf = NULL; s.text_cap = 0;
    render_screen();
}

static void mount_set_pin(void)
{
    ESP_LOGI("shell", "mount_set_pin");
    s.page = SHELL_PAGE_SET_PIN;
    s.focus_mode = FOCUS_VIEWPORT;
    s.lock_pin_buf[0] = '\0';
    s.text_input_widget = -1;
    s.text_buf = NULL; s.text_cap = 0;
    render_screen();
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

// Switch to a built-in app via the cache. Used both when launching from
// the desktop tile grid AND when picking the entry from the rail; the
// only difference is whether a cache slot already exists.
static void switch_to_builtin(const char *id)
{
    if (!id || !*id) return;
    cache_snapshot_current();
    struct app_cache *slot = cache_find(id);
    int target_page = -1;
    if (slot && slot->is_builtin) target_page = slot->last_page;

    // Map id to the default entry page; cache may have us deeper in.
    if      (strcmp(id, "builtin:notes") == 0) {
        if (target_page == SHELL_PAGE_NOTES_EDIT) {
            // Resume the editor where the user left off. The buffer +
            // filename already live in static `s`.
            s.page = SHELL_PAGE_NOTES_EDIT;
            s.focus_mode = FOCUS_VIEWPORT;
            s.text_input_widget = 0;
            s.text_buf = s.notes_edit_buf;
            s.text_cap = sizeof(s.notes_edit_buf);
        } else {
            mount_notes_list();
        }
    }
    else if (strcmp(id, "builtin:sysmon") == 0) mount_sysmon();
    else if (strcmp(id, "builtin:idqr")   == 0) mount_idqr();
    else if (strcmp(id, "builtin:nfc")    == 0) mount_nfc();
    else if (strcmp(id, "builtin:notif")  == 0) mount_notif();
    else if (strcmp(id, "builtin:quick")  == 0) mount_quick();
    else { ESP_LOGW("shell", "switch_to_builtin: unknown %s", id); return; }

    if (slot) s.vp_focus = slot->vp_focus;
    s.rail_close_mode = false;
    pageros_apprt_open_mark(id);   // also tracked in the open list
    struct app_cache *touched = cache_alloc(id, true);
    if (touched) touched->last_touched_us = esp_timer_get_time();
    render_screen();
}

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
            notif_announce("SSID IS EMPTY", PAGEROS_TOAST_WARN, 2500);
        } else {
            strncpy(s.wifi_status, "Connecting…", sizeof(s.wifi_status));
            s.wifi_status[sizeof(s.wifi_status) - 1] = '\0';
            esp_err_t r = pageros_wifi_connect(s.ssid, s.psk, 15000);
            if (r == ESP_OK) {
                strncpy(s.wifi_status, "Connected", sizeof(s.wifi_status));
                pageros_wifi_creds_save(s.ssid, s.psk);
                notif_announce("WI-FI CONNECTED", PAGEROS_TOAST_OK, 2500);
            } else {
                snprintf(s.wifi_status, sizeof(s.wifi_status), "Failed: %s",
                         esp_err_to_name(r));
                notif_announce("WI-FI FAILED", PAGEROS_TOAST_ERROR, 3000);
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
                    notif_announce("APP INSTALLED", PAGEROS_TOAST_OK, 2500);
                } else {
                    snprintf(s.addapp_msg, sizeof(s.addapp_msg),
                             "Install failed: %s", esp_err_to_name(r));
                    strncpy(s.addapp_msg_level, "error",
                            sizeof(s.addapp_msg_level));
                    notif_announce("INSTALL FAILED", PAGEROS_TOAST_ERROR, 3000);
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
    if (strncmp(href, "builtin:", 8) == 0) {
        switch_to_builtin(href);
        return true;
    }
    return false;
}

static void launch_app(const char *app_id)
{
    if (!app_id || !*app_id) return;
    ESP_LOGI("shell", "launch_app: %s", app_id);

    // Save whatever is currently foregrounded so we can return to it
    // later from the rail without losing state.
    cache_snapshot_current();

    // Fast path — cached Frame bytes mean no HTTP round-trip.
    struct app_cache *slot = cache_find(app_id);
    if (slot && !slot->is_builtin && slot->frame_bytes && slot->frame_len) {
        ESP_LOGI("shell", "launch_app cached: %s (%u bytes)", app_id,
                 (unsigned)slot->frame_len);
        pageros_apprt_open(app_id, slot->frame_bytes, slot->frame_len);
        pageros_apprt_open_mark(app_id);
        s.page = SHELL_PAGE_APP;
        s.focus_mode = FOCUS_VIEWPORT;
        s.vp_focus = slot->vp_focus;
        s.text_input_widget = -1;
        s.text_buf = NULL; s.text_cap = 0;
        slot->last_touched_us = esp_timer_get_time();
        render_screen();
        return;
    }

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

    // Cache the freshly-fetched Frame so subsequent rail switches stay
    // instant. We hold our own copy in the cache rather than peering
    // into apprt's internal buffer.
    struct app_cache *new_slot = cache_alloc(app_id, false);
    if (new_slot) {
        if (new_slot->frame_bytes) { free(new_slot->frame_bytes); new_slot->frame_bytes = NULL; }
        new_slot->frame_bytes = (uint8_t *)malloc(r.body_len);
        if (new_slot->frame_bytes) {
            memcpy(new_slot->frame_bytes, buf, r.body_len);
            new_slot->frame_len = r.body_len;
        }
        new_slot->is_builtin = false;
        new_slot->vp_focus = (pageros_widgets_focus_t){0};
    }
    free(buf);

    s.page = SHELL_PAGE_APP;
    s.focus_mode = FOCUS_VIEWPORT;
    s.rail_close_mode = false;
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
        const pgr_cbor_value_t *fr = pageros_apprt_foreground_frame();
        int n = focused_list_len(fr, s.vp_focus.widget_index);
        if (n > 0) {
            s.vp_focus.item_index = (s.vp_focus.item_index + step + n) % n;
        }
        return;
    }
    case SHELL_PAGE_NOTIF: {
        if (s.notif_count <= 0) return;
        s.notif_focus = (s.notif_focus + step + s.notif_count) % s.notif_count;
        return;
    }
    case SHELL_PAGE_QUICK: {
        int n_quick = 5;
        s.quick_focus = (s.quick_focus + step + n_quick) % n_quick;
        return;
    }
    case SHELL_PAGE_NOTES_LIST: {
        int n_items = 1 + s.notes_count;
        if (n_items <= 0) return;
        s.notes_focus = (s.notes_focus + step + n_items) % n_items;
        return;
    }
    case SHELL_PAGE_NOTES_EDIT:
    case SHELL_PAGE_SYSMON:
    case SHELL_PAGE_IDQR:
    case SHELL_PAGE_NFC:
    case SHELL_PAGE_LOCK:
    case SHELL_PAGE_SET_PIN:
        return;
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

// Returns true if the event was handled in a page-specific way (no
// generic handling needed). When true, the caller skips its normal
// dispatch.
static bool handle_special_page_input(const pageros_router_event_t *ev)
{
    // SET PIN page — digit entry then ENTER saves.
    if (s.page == SHELL_PAGE_SET_PIN) {
        if (ev->kind == PAGEROS_ROUTER_EVT_NAV) {
            if (ev->as.nav == PAGEROS_ROUTER_NAV_ENTER) {
                if (strlen(s.lock_pin_buf) >= 4) {
                    if (lock_save_pin(s.lock_pin_buf) == ESP_OK) {
                        notif_announce("PIN UPDATED", PAGEROS_TOAST_OK, 2000);
                    } else {
                        notif_announce("PIN SAVE FAILED",
                                       PAGEROS_TOAST_ERROR, 2500);
                    }
                    s.lock_pin_buf[0] = '\0';
                    mount_quick();
                    return true;
                }
                pageros_toast("ENTER 4 DIGITS", PAGEROS_TOAST_WARN, 1500);
                render_screen();
                return true;
            }
            if (ev->as.nav == PAGEROS_ROUTER_NAV_BACK_LONG) {
                s.lock_pin_buf[0] = '\0';
                mount_desktop();
                return true;
            }
            if (ev->as.nav == PAGEROS_ROUTER_NAV_BACK) {
                if (s.lock_pin_buf[0] != '\0') {
                    // Backspace one digit.
                    size_t L = strlen(s.lock_pin_buf);
                    s.lock_pin_buf[L - 1] = '\0';
                    pageros_audio_play_ui_scroll();
                    render_screen();
                } else {
                    mount_quick();
                }
                return true;
            }
        }
        if (ev->kind == PAGEROS_ROUTER_EVT_KEY && ev->as.key.pressed) {
            char ch = kb_decode(ev->as.key.row, ev->as.key.col);
            if (ch >= '0' && ch <= '9') {
                size_t L = strlen(s.lock_pin_buf);
                if (L < 4) {
                    s.lock_pin_buf[L] = ch;
                    s.lock_pin_buf[L + 1] = '\0';
                    pageros_audio_play_ui_scroll();
                }
                render_screen();
                return true;
            }
            if (ch == '\b' && s.lock_pin_buf[0]) {
                size_t L = strlen(s.lock_pin_buf);
                s.lock_pin_buf[L - 1] = '\0';
                pageros_audio_play_ui_scroll();
                render_screen();
                return true;
            }
        }
        return true;
    }

    // Lock screen — own input rules. PIN entry digits, ENTER validates.
    if (s.page == SHELL_PAGE_LOCK) {
        pageros_audio_play_ui_scroll();
        if (ev->kind == PAGEROS_ROUTER_EVT_NAV) {
            if (ev->as.nav == PAGEROS_ROUTER_NAV_ENTER) {
                if (!s.lock_pin_required ||
                    strcmp(s.lock_pin_buf, s.lock_pin_stored) == 0) {
                    pageros_audio_play_ui_click();
                    s.lock_pin_buf[0] = '\0';
                    s.lock_attempts = 0;
                    mount_desktop();
                    return true;
                }
                s.lock_attempts++;
                pageros_toast("WRONG PIN", PAGEROS_TOAST_ERROR, 1500);
                s.lock_pin_buf[0] = '\0';
                render_screen();
                return true;
            }
            if (ev->as.nav == PAGEROS_ROUTER_NAV_BACK) {
                s.lock_pin_buf[0] = '\0';
                render_screen();
                return true;
            }
        }
        if (ev->kind == PAGEROS_ROUTER_EVT_KEY && ev->as.key.pressed) {
            char ch = kb_decode(ev->as.key.row, ev->as.key.col);
            if (s.lock_pin_required && ch >= '0' && ch <= '9') {
                size_t L = strlen(s.lock_pin_buf);
                if (L < 4) {
                    s.lock_pin_buf[L] = ch;
                    s.lock_pin_buf[L + 1] = '\0';
                    if (L + 1 == 4) {
                        // Auto-submit when 4 digits entered.
                        if (strcmp(s.lock_pin_buf, s.lock_pin_stored) == 0) {
                            pageros_audio_play_ui_click();
                            s.lock_pin_buf[0] = '\0';
                            mount_desktop();
                            return true;
                        }
                        s.lock_attempts++;
                        pageros_toast("WRONG PIN", PAGEROS_TOAST_ERROR, 1500);
                        s.lock_pin_buf[0] = '\0';
                    }
                }
            }
            render_screen();
            return true;
        }
        return true;
    }
    return false;
}

static bool default_shell_handler(const pageros_router_event_t *ev, void *ctx)
{
    (void)ctx;
    // Detect wake-from-sleep: if we were screen-off or deeper *before*
    // kicking the power manager, the user is unlocking the device, so
    // bounce them through the lock screen (or straight to desktop if
    // no PIN is required) instead of resuming the previous page.
    pageros_power_state_t prev_pwr = pageros_power_state();
    pageros_power_kick();
    if (prev_pwr >= PAGEROS_POWER_SCREEN_OFF
            && s.page != SHELL_PAGE_LOCK) {
        mount_lock();
        return true;
    }

    // Lock screen owns its own input model.
    if (handle_special_page_input(ev)) return true;

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
    bool rail_visible =
        (s.page == SHELL_PAGE_DESKTOP ||
         s.page == SHELL_PAGE_APP     ||
         s.page == SHELL_PAGE_NOTES_LIST || s.page == SHELL_PAGE_NOTES_EDIT ||
         s.page == SHELL_PAGE_SYSMON  || s.page == SHELL_PAGE_IDQR ||
         s.page == SHELL_PAGE_NFC     || s.page == SHELL_PAGE_NOTIF ||
         s.page == SHELL_PAGE_QUICK);

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
                    if (s.page != SHELL_PAGE_DESKTOP) mount_desktop();
                    else {
                        s.focus_mode = FOCUS_VIEWPORT;
                        dirty = true;
                    }
                    break;
                }
                int idx = s.rail_index - 1;
                if (idx < 0 || idx >= s.rail_count || !s.rail_items[idx]) break;

                char app_id[96];
                strncpy(app_id, s.rail_items[idx], sizeof(app_id) - 1);
                app_id[sizeof(app_id) - 1] = '\0';

                if (s.rail_close_mode) {
                    // Close mode — drop the app from open-list + cache.
                    pageros_apprt_open_close(app_id);
                    struct app_cache *slot = cache_find(app_id);
                    if (slot) cache_free_slot(slot);
                    // Snap focus back into rail HOME so we don't index
                    // off the end of a shrinking list.
                    s.rail_index = 0;
                    s.rail_close_mode = false;
                    pageros_toast("APP CLOSED", PAGEROS_TOAST_INFO, 1200);
                    render_screen();
                    return true;
                }

                if (strncmp(app_id, "builtin:", 8) == 0) {
                    switch_to_builtin(app_id);
                } else {
                    launch_app(app_id);
                }
                return true;
            }
            // Viewport activation.
            if (s.page == SHELL_PAGE_DESKTOP) {
                if (s.tile_index >= 0 && s.tile_index < s.tile_count) {
                    handle_button_href(s.tile_app_ids[s.tile_index]);
                    return true;
                }
                break;
            }
            if (s.page == SHELL_PAGE_QUICK) {
                switch (s.quick_focus) {
                case 0:  // Silent
                    pageros_audio_set_muted(!pageros_audio_is_muted());
                    pageros_toast(pageros_audio_is_muted()
                                      ? "SILENT MODE ON" : "SILENT MODE OFF",
                                  PAGEROS_TOAST_INFO, 1500);
                    break;
                case 1:  // Wi-Fi toggle
                    if (pageros_wifi_is_connected()) {
                        pageros_wifi_disconnect();
                        pageros_toast("WI-FI DISCONNECTED",
                                      PAGEROS_TOAST_INFO, 1500);
                    } else {
                        pageros_toast("USE SETTINGS → WI-FI",
                                      PAGEROS_TOAST_INFO, 1500);
                    }
                    break;
                case 2:  // LoRa toggle — disabled for v1 (no API yet)
                    pageros_toast("LORA TOGGLE NOT WIRED",
                                  PAGEROS_TOAST_WARN, 1500);
                    break;
                case 3:  // Lock now
                    mount_lock();
                    return true;
                case 4:  // Set PIN
                    mount_set_pin();
                    return true;
                }
                render_screen();
                return true;
            }
            if (s.page == SHELL_PAGE_NOTES_LIST) {
                if (s.notes_focus == 0) {
                    mount_notes_edit(NULL);
                } else if (s.notes_focus - 1 < s.notes_count) {
                    mount_notes_edit(s.notes_files[s.notes_focus - 1]);
                }
                return true;
            }
            if (s.page == SHELL_PAGE_NOTES_EDIT) {
                // ENTER in edit = newline.
                size_t L = strlen(s.notes_edit_buf);
                if (L + 1 < sizeof(s.notes_edit_buf)) {
                    s.notes_edit_buf[L] = '\n';
                    s.notes_edit_buf[L + 1] = '\0';
                    dirty = true;
                }
                break;
            }
            if (s.page == SHELL_PAGE_NFC) {
                // The poll task auto-detects tags via on_nfc_scan; the
                // user just needs to tap a tag near the antenna. ENTER
                // is a no-op here, but we surface a hint so it doesn't
                // feel dead.
                pageros_toast("HOLD A TAG NEAR THE PAGER",
                              PAGEROS_TOAST_INFO, 1800);
                strncpy(s.nfc_status, "WAITING…", sizeof(s.nfc_status));
                s.nfc_status[sizeof(s.nfc_status) - 1] = '\0';
                render_screen();
                return true;
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
            // Modal + built-in pages back-stack.
            switch (s.page) {
            case SHELL_PAGE_SETTINGS: mount_desktop();  dirty = true; break;
            case SHELL_PAGE_WIFI:
            case SHELL_PAGE_ADD_APP:  mount_settings(); dirty = true; break;
            case SHELL_PAGE_NOTIF:
            case SHELL_PAGE_QUICK:
            case SHELL_PAGE_SYSMON:
            case SHELL_PAGE_IDQR:
            case SHELL_PAGE_NFC:
            case SHELL_PAGE_NOTES_LIST:
                mount_desktop();
                dirty = true;
                break;
            case SHELL_PAGE_NOTES_EDIT:
                notes_save_current();
                pageros_toast("NOTE SAVED", PAGEROS_TOAST_OK, 1500);
                notif_push("Note saved", PAGEROS_TOAST_OK);
                mount_notes_list();
                dirty = true;
                break;
            default: break;
            }
        }
        break;
    }
    case PAGEROS_ROUTER_EVT_KEY: {
        if (!ev->as.key.pressed) return true;
        char ch = kb_decode(ev->as.key.row, ev->as.key.col);
        // SPACE on the rail toggles close-mode regardless of text-input
        // state. This is a global rail gesture, so it shadows the
        // normal "append to focused text buffer" path.
        if (rail_visible && s.focus_mode == FOCUS_RAIL && ch == ' ') {
            s.rail_close_mode = !s.rail_close_mode;
            pageros_audio_play_ui_click();
            pageros_toast(s.rail_close_mode ? "CLOSE MODE — ENTER TO CLOSE"
                                            : "SELECT MODE",
                          s.rail_close_mode ? PAGEROS_TOAST_WARN
                                            : PAGEROS_TOAST_INFO,
                          1500);
            render_screen();
            return true;
        }
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
            else if (s.page == SHELL_PAGE_NOTES_EDIT) {
                size_t L = strlen(s.text_buf);
                if (L + 1 < s.text_cap) {
                    s.text_buf[L] = '\n';
                    s.text_buf[L + 1] = '\0';
                    dirty = true;
                }
                if (dirty) render_screen();
            }
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

// --- GPIO button hunter ------------------------------------------- //
//
// We don't know which GPIO (if any) is wired to the third physical
// button at the bottom of the device. This background task polls a
// short list of GPIOs that the pin file leaves unmapped, logs any
// level transitions, and stores the latest levels so SYSMON can show
// them. Press the right button while running `idf.py monitor` and we
// should see "hunt: GPIOxx → LOW" pop up.

// Conservative: only pins that are *definitely* not on the SPI PSRAM
// or octal flash bus. GPIO 26-32 are PSRAM data lines on the ESP32-S3
// module variant this board uses (touching them rebooted the device);
// GPIO 9 sometimes hosts PSRAM data too. 15 + 16 are GPIO-only on
// every ESP32-S3 variant we care about.
const int g_hunt_pins[HUNT_PINS_MAX] = { 15, 16 };
static int g_hunt_last[HUNT_PINS_MAX] = { 1, 1 };
int g_hunt_now[HUNT_PINS_MAX]  = { 1, 1 };

static void button_hunt_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < HUNT_PINS_MAX; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << g_hunt_pins[i],
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        // Soft-ignore failures — a pin in use by another driver will
        // refuse this reconfig and just stay at its existing level.
        (void)gpio_config(&cfg);
        g_hunt_last[i] = gpio_get_level((gpio_num_t)g_hunt_pins[i]);
        g_hunt_now[i]  = g_hunt_last[i];
    }
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(80));
        for (int i = 0; i < HUNT_PINS_MAX; i++) {
            int lvl = gpio_get_level((gpio_num_t)g_hunt_pins[i]);
            g_hunt_now[i] = lvl;
            if (lvl != g_hunt_last[i]) {
                ESP_LOGI("hunt", "GPIO%d -> %s",
                         g_hunt_pins[i], lvl ? "HIGH" : "LOW");
                g_hunt_last[i] = lvl;
                // Push a notif so the user sees something on-device too.
                char msg[40];
                snprintf(msg, sizeof(msg), "GPIO%d %s",
                         g_hunt_pins[i], lvl ? "HIGH" : "LOW");
                notif_push(msg, PAGEROS_TOAST_INFO);
            }
        }
    }
}

// --- GPS callback ----------------------------------------------- //

// --- NFC scan callback ----------------------------------------- //

static void on_nfc_scan(const pageros_nfc_scan_t *scan, void *user)
{
    (void)user;
    if (!scan || scan->uid_len == 0) return;
    char uid_hex[32];
    size_t off = 0;
    int copy = scan->uid_len > 8 ? 8 : scan->uid_len;
    for (int i = 0; i < copy && off + 3 < sizeof(uid_hex); i++) {
        off += snprintf(uid_hex + off, sizeof(uid_hex) - off,
                        "%02X%s", scan->uid[i], (i == copy - 1) ? "" : ":");
    }
    strncpy(s.nfc_last_uid, uid_hex, sizeof(s.nfc_last_uid) - 1);
    s.nfc_last_uid[sizeof(s.nfc_last_uid) - 1] = '\0';

    char msg[64];
    snprintf(msg, sizeof(msg), "TAG %s", uid_hex);
    notif_announce(msg, PAGEROS_TOAST_INFO, 2500);

    // Persist the scan to /sd/nfc/<uid>.bin so the device builds a
    // local library of seen tags. The first PAGEROS_NFC_MAX_UID_BYTES
    // bytes are the UID, the rest are any NDEF payload.
    if (pageros_storage_init() == ESP_OK) {
        mkdir("/sd/nfc", 0777);
        char path[96];
        // Sanitised filename: strip colons.
        char clean[32]; int ci = 0;
        for (int i = 0; uid_hex[i] && ci < (int)sizeof(clean) - 1; i++) {
            if (uid_hex[i] != ':') clean[ci++] = uid_hex[i];
        }
        clean[ci] = '\0';
        snprintf(path, sizeof(path), "/sd/nfc/%s.bin", clean);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(scan->uid, 1, scan->uid_len, f);
            if (scan->ndef_len > 0) {
                fwrite(scan->ndef, 1, scan->ndef_len, f);
            }
            fclose(f);
        }
    }
    if (s.page == SHELL_PAGE_NFC) {
        strncpy(s.nfc_status, "SAVED TO /sd/nfc/",
                sizeof(s.nfc_status) - 1);
        s.nfc_status[sizeof(s.nfc_status) - 1] = '\0';
        render_screen();
    }
}

static void on_gps_fix(const pageros_gps_fix_t *fix, void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI("gps", "fix: lat=%.6f lon=%.6f acc=%.1fm sats=%u",
             fix->latitude_deg, fix->longitude_deg,
             (double)fix->accuracy_m, (unsigned)fix->satellites);
}

// --- Boot splash ------------------------------------------------- //
//
// Terminal-style log roll shown during the second half of boot (after
// the display is up). Each subsystem appends a line as it comes online,
// then mount_desktop() takes over once everything is ready.

#define BOOT_LOG_MAX 14

typedef enum {
    BL_INFO = 0,
    BL_OK,
    BL_WARN,
    BL_ERROR,
} boot_log_level_t;

static struct {
    char  text[BOOT_LOG_MAX][56];
    uint8_t level[BOOT_LOG_MAX];
    int   n;
} g_boot_log;

static void boot_log_add(boot_log_level_t lvl, const char *fmt, ...)
{
    char line[56];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (g_boot_log.n >= BOOT_LOG_MAX) {
        // Scroll: drop oldest.
        for (int i = 1; i < BOOT_LOG_MAX; i++) {
            memcpy(g_boot_log.text[i - 1], g_boot_log.text[i], sizeof(g_boot_log.text[0]));
            g_boot_log.level[i - 1] = g_boot_log.level[i];
        }
        g_boot_log.n = BOOT_LOG_MAX - 1;
    }
    strncpy(g_boot_log.text[g_boot_log.n], line, sizeof(g_boot_log.text[0]) - 1);
    g_boot_log.text[g_boot_log.n][sizeof(g_boot_log.text[0]) - 1] = '\0';
    g_boot_log.level[g_boot_log.n] = (uint8_t)lvl;
    g_boot_log.n++;
}

static void boot_splash_render(void)
{
    uint16_t *fb = (uint16_t *)pageros_display_framebuffer();
    if (!fb) return;
    pageros_fonts_canvas_t canvas = {
        .pixels = fb,
        .w = PAGEROS_DISPLAY_WIDTH,
        .h = PAGEROS_DISPLAY_HEIGHT,
    };
    pageros_widgets_fill_rect(&canvas, 0, 0, canvas.w, canvas.h, g_palette.bg);

    // Header — "PAGEROS" at 2x scale (32 px chunky) centered.
    const char *hdr = "PAGEROS";
    int hdr_w = 16 * 2 * (int)strlen(hdr);
    int hdr_x = (canvas.w - hdr_w) / 2;
    pageros_fonts_draw_text_scaled_16(&canvas, hdr_x, 14, hdr, -1,
                                      g_palette.info, 2);
    // Magenta accent dot to the right of the header.
    pageros_widgets_fill_rect(&canvas, hdr_x + hdr_w + 4, 24, 6, 6,
                              g_palette.accent);

    // Identity below the header (small, dim).
    char fp[PAGEROS_IDENTITY_FP_LEN] = {0};
    if (pageros_identity_fingerprint(fp) == ESP_OK && fp[0]) {
        char buf[20];
        snprintf(buf, sizeof(buf), "ID: %s", fp);
        int bw = pageros_fonts_measure_text(buf, -1);
        pageros_fonts_draw_text(&canvas, (canvas.w - bw) / 2, 52,
                                buf, -1, g_palette.dim, canvas.w);
    }

    // Divider line.
    pageros_widgets_fill_rect(&canvas, 24, 66, canvas.w - 48, 1, g_palette.info);

    // Log lines (small font, 11 px line height).
    int y = 74;
    int line_h = 11;
    for (int i = 0; i < g_boot_log.n; i++) {
        uint16_t col = g_palette.fg;
        switch (g_boot_log.level[i]) {
        case BL_OK:    col = g_palette.info;   break;
        case BL_WARN:  col = g_palette.warn;   break;
        case BL_ERROR: col = g_palette.error;  break;
        case BL_INFO:
        default:       col = g_palette.fg;     break;
        }
        pageros_fonts_draw_text(&canvas, 24, y,
                                g_boot_log.text[i], -1, col,
                                canvas.w - 48);
        y += line_h;
        if (y > canvas.h - 12) break;
    }

    pageros_display_present();
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

    // Palette is initialised early so the boot splash + every other
    // render uses it; the cyberpunk look is the device's identity.
    pageros_widgets_cyberpunk_palette(&g_palette);

    // Splash begins as soon as the display is alive. We backfill log
    // lines for the silent pre-display init steps (NVS, identity,
    // I2C/XL9555) so the user sees a complete boot story.
    if (disp_err == ESP_OK) {
        boot_log_add(BL_OK, "[OK] BOOT SELFTEST");
        boot_log_add(BL_OK, "[OK] NVS");
        boot_log_add(id_err == ESP_OK ? BL_OK : BL_WARN,
                     id_err == ESP_OK ? "[OK] IDENTITY" : "[!!] IDENTITY: %s",
                     esp_err_to_name(id_err));
        boot_log_add(BL_OK, "[OK] I2C BUS + XL9555 ENABLES");
        boot_log_add(BL_OK, "[OK] DISPLAY ONLINE");
        boot_splash_render();
    }

    // SD mount remains lazy — first sideload/launch will mount it.
    ESP_LOGW(TAG, "[diag] SD init skipped — shell + display work without it");
    boot_log_add(BL_INFO, "[..] SD MOUNT DEFERRED");
    boot_splash_render();

    esp_err_t log_err = pageros_logger_init();
    if (log_err != ESP_OK) {
        ESP_LOGW(TAG, "logger init failed: %s", esp_err_to_name(log_err));
        boot_log_add(BL_WARN, "[!!] LOGGER: %s", esp_err_to_name(log_err));
    } else {
        LOG_INFO(TAG, "logger online, sd=%s",
                 pageros_logger_is_writing_to_sd() ? "yes" : "no");
        boot_log_add(BL_OK, "[OK] LOGGER");
    }
    boot_splash_render();

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
        boot_log_add(BL_OK, "[OK] LORA SX1262 SF7/BW125 868MHZ");
    } else {
        LOG_ERROR(TAG, "LoRa init failed: %s", esp_err_to_name(lora_err));
        boot_log_add(BL_WARN, "[!!] LORA: %s", esp_err_to_name(lora_err));
    }
    boot_splash_render();

    esp_err_t wifi_err = pageros_wifi_init();
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "wifi init failed: %s", esp_err_to_name(wifi_err));
        boot_log_add(BL_WARN, "[!!] WI-FI: %s", esp_err_to_name(wifi_err));
        boot_splash_render();
    } else {
        boot_log_add(BL_OK, "[OK] WI-FI STA INIT");
        boot_splash_render();
        char saved_ssid[33] = {0}, saved_psk[65] = {0};
        if (pageros_wifi_creds_load(saved_ssid, sizeof(saved_ssid),
                                    saved_psk, sizeof(saved_psk)) == ESP_OK
                && saved_ssid[0]) {
            ESP_LOGI(TAG, "Wi-Fi auto-connect to '%s'", saved_ssid);
            boot_log_add(BL_INFO, "[..] WI-FI CONNECTING %s", saved_ssid);
            boot_splash_render();
            esp_err_t wc = pageros_wifi_connect(saved_ssid, saved_psk, 15000);
            if (wc != ESP_OK) {
                ESP_LOGW(TAG, "auto-connect failed: %s", esp_err_to_name(wc));
                boot_log_add(BL_WARN, "[!!] WI-FI: %s", esp_err_to_name(wc));
            } else {
                boot_log_add(BL_OK, "[OK] WI-FI CONNECTED");
            }
            boot_splash_render();
        } else {
            boot_log_add(BL_INFO, "[..] WI-FI NO CREDS");
            boot_splash_render();
        }
    }

    // Audio bring-up (FW-013) — required for click/scroll UI sounds.
    esp_err_t audio_err = pageros_audio_init();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "audio init failed: %s — UI sounds disabled",
                 esp_err_to_name(audio_err));
        boot_log_add(BL_WARN, "[!!] AUDIO: %s", esp_err_to_name(audio_err));
    } else {
        boot_log_add(BL_OK, "[OK] AUDIO ES8311 + UI SOUNDS");
    }
    boot_splash_render();

    pageros_fonts_init(NULL);
    pageros_apprt_init(NULL);
    pageros_shell_init();
    lock_load_pin();
    boot_log_add(BL_OK, "[OK] FONTS + APPRT + SHELL");
    boot_splash_render();

    if (disp_err == ESP_OK) {
        (void)pageros_display_panel_reinit();
        boot_log_add(BL_OK, s.lock_pin_required
                                ? "[OK] LOCK SCREEN (PIN SET)"
                                : "[OK] LOCK SCREEN (NO PIN)");
        boot_splash_render();
        // Brief hold so the last log line reads before the lock takes
        // over — feels deliberate.
        vTaskDelay(pdMS_TO_TICKS(600));
        // Seed a welcome notif so the user sees something in NOTIFS
        // first time they open it.
        notif_push("PagerOS online", PAGEROS_TOAST_OK);
        mount_lock();
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

    // NFC bring-up + scan polling. Tags are saved to /sd/nfc/<uid>.bin.
    esp_err_t nfc_err = pageros_nfc_init();
    if (nfc_err == ESP_OK) {
        pageros_nfc_start(on_nfc_scan, NULL);
    } else {
        ESP_LOGW(TAG, "NFC init skipped: %s", esp_err_to_name(nfc_err));
    }

    pageros_power_init(NULL);

    // Background GPIO transition logger — kept running so we can hunt
    // the elusive third bottom button. Cheap (~50 µs every 80 ms).
    xTaskCreate(button_hunt_task, "gpio_hunt", 3072, NULL, 3, NULL);

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
