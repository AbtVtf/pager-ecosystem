// PagerOS firmware entry point.
//
// Skeleton (FW-001) + bootloader self-test (FW-002). Subsequent tasks
// (FW-003 filesystem mount, FW-005 display driver, etc.) bolt their
// initialization in here in dependency order. The self-test must run
// first per SPEC.md §7.2 boot flow.

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"

#include "pageros_apprt.h"
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

// --- Shell navigation state --------------------------------------- //
//
// The shell is itself a built-in app (SPEC §10.4 / FW-029). We track
// which "page" of the shell is currently mounted, the focus cursor
// for it, and the text buffers backing the input widgets on the
// Wi-Fi and Add-app screens.

typedef enum {
    SHELL_PAGE_HOME = 0,
    SHELL_PAGE_SETTINGS,
    SHELL_PAGE_WIFI,
    SHELL_PAGE_ADD_APP,
} shell_page_t;

static struct {
    shell_page_t page;
    pageros_widgets_focus_t focus;

    // Per-page state.
    char ssid[33];           // Wi-Fi
    char psk[65];
    char wifi_status[48];

    char url[160];           // Add app
    char addapp_msg[80];
    char addapp_msg_level[8];

    // Which widget index in the current Frame is an `input` we should
    // route key events into. -1 = no input focused (focus is on a list
    // item or a button).
    int  text_input_widget;

    // Editable buffer for the currently focused input. Points to one
    // of the per-page char arrays above (e.g. `ssid`).
    char  *text_buf;
    size_t text_cap;
} s = {
    .page  = SHELL_PAGE_HOME,
    .focus = { .widget_index = 1, .item_index = 0, .scroll = 0 },
    .text_input_widget = -1,
};

// Shorthand kept for legacy callers (back-compat with old field name).
#define s_focus (s.focus)

// Walk the foreground Frame's body looking up the widget at
// `widget_index`; returns the focused item's `href` text (NUL-terminated)
// or NULL if not addressable.
static const char *focused_href(const pgr_cbor_value_t *frame,
                                int widget_index, int item_index,
                                char *out, size_t out_cap)
{
    if (!frame || frame->kind != PGR_CBOR_KIND_MAP || !out) return NULL;
    // Pull body from the Frame.
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
    // Find the "items" array.
    const pgr_cbor_value_t *items = NULL;
    for (size_t i = 0; i < w->v.map.len; i++) {
        const pgr_cbor_pair_t *p = &w->v.map.items[i];
        if (p->key.kind == PGR_CBOR_KIND_TEXT &&
            p->key.v.bytes.len == 5 &&
            memcmp(p->key.v.bytes.data, "items", 5) == 0) { items = &p->val; break; }
    }
    if (!items || items->kind != PGR_CBOR_KIND_ARRAY) return NULL;
    if (item_index < 0 || item_index >= (int)items->v.arr.len) return NULL;
    const pgr_cbor_value_t *it = &items->v.arr.items[item_index];
    if (it->kind != PGR_CBOR_KIND_MAP) return NULL;
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
    return NULL;
}

// How many list rows is the focused widget? Used to bound encoder
// navigation. Returns -1 if the widget isn't a list.
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

// Re-render the foreground app's current Frame with the up-to-date focus.
static void render_foreground(void)
{
    pageros_widgets_palette_t pal;
    pageros_widgets_default_palette(&pal);
    const char *title = "PagerOS";
    const char *help  = "MENU=up/dn  ENTER=open  BACK=back";
    if (s.page == SHELL_PAGE_SETTINGS) title = "Settings";
    if (s.page == SHELL_PAGE_WIFI)     { title = "Wi-Fi";    help = "Type SSID/PSK  ENTER=connect  BACK=erase"; }
    if (s.page == SHELL_PAGE_ADD_APP)  { title = "Add app";  help = "Type URL  ENTER=install  BACK=erase"; }
    pageros_widgets_ctx_t ctx = {
        .canvas  = {
            .pixels = (uint16_t *)pageros_display_framebuffer(),
            .w      = PAGEROS_DISPLAY_WIDTH,
            .h      = PAGEROS_DISPLAY_HEIGHT,
        },
        .palette = pal,
        .focus   = s.focus,
        .title   = title,
        .help    = help,
    };
    const pgr_cbor_value_t *fr = pageros_apprt_foreground_frame();
    if (!fr) return;
    const pgr_cbor_value_t *body = NULL;
    if (fr->kind == PGR_CBOR_KIND_MAP) {
        for (size_t i = 0; i < fr->v.map.len; i++) {
            const pgr_cbor_pair_t *p = &fr->v.map.items[i];
            if (p->key.kind == PGR_CBOR_KIND_TEXT &&
                p->key.v.bytes.len == 4 &&
                memcmp(p->key.v.bytes.data, "body", 4) == 0) { body = &p->val; break; }
        }
    }
    if (pageros_widgets_render_screen(&ctx, body) == ESP_OK) {
        pageros_display_present();
    }
}

// --- QWERTY keymap (TCA8418 row/col → ASCII) ---------------------- //
//
// LILYGO T-LoRa Pager keyboard. The TCA8418 reports a 4x10 matrix.
// This is our best-guess layout based on the printed keycaps; the
// "shell key: row=X col=Y char='?'" log line on every press lets the
// user correct the map if any key lands wrong. Unmapped cells return 0.

// Update once we know the real cells: the back-arrow / delete key
// should map to '\b' (0x08) so the input handler treats it as
// backspace. Every key press logs its row/col over serial — press
// the key you want to be backspace and tell me the numbers.
static const char keymap_lower[4][10] = {
    /* row 0 */ {'q','w','e','r','t','y','u','i','o','p'},
    /* row 1 */ {'a','s','d','f','g','h','j','k','l', '\b'},
    /* row 2 */ {'z','x','c','v','b','n','m',',','.',  0 },
    /* row 3 */ { 0 , 0 ,' ', 0 , 0 , 0 , 0 ,'/','-', 0 },
};

static char keymap_lookup(uint8_t row, uint8_t col)
{
    if (row >= 4 || col >= 10) return 0;
    return keymap_lower[row][col];
}

// --- Per-screen mount + emit helpers ------------------------------- //

// Mount the result of `emit` into apprt and re-render. The emit
// function takes (buf, cap, &len) and writes a CBOR Frame.
static void mount_and_render(esp_err_t (*emit)(uint8_t *, size_t, size_t *))
{
    uint8_t buf[768]; size_t n = 0;
    if (emit(buf, sizeof(buf), &n) != ESP_OK) {
        ESP_LOGW("shell", "emit failed");
        return;
    }
    pageros_apprt_set_frame(buf, n);
    render_foreground();
}

static void mount_home(void)
{
    ESP_LOGI("shell", "mount_home");
    s.page = SHELL_PAGE_HOME;
    s.focus.widget_index = 1; s.focus.item_index = 0;
    s.text_input_widget = -1; s.text_buf = NULL; s.text_cap = 0;
    pageros_shell_mount_home();
    render_foreground();
}

static void mount_settings(void)
{
    ESP_LOGI("shell", "mount_settings");
    s.page = SHELL_PAGE_SETTINGS;
    s.focus.widget_index = 1; s.focus.item_index = 0;
    s.text_input_widget = -1; s.text_buf = NULL; s.text_cap = 0;
    mount_and_render(pageros_shell_emit_settings);
}

static esp_err_t emit_wifi_thunk(uint8_t *b, size_t c, size_t *n)
{
    // Render the password as a row of dots so an over-the-shoulder
    // look doesn't reveal it; the actual `s.psk` buffer keeps the
    // plaintext for the eventual wifi_connect call.
    char masked[sizeof(s.psk)];
    size_t L = strlen(s.psk);
    for (size_t i = 0; i < L && i + 1 < sizeof(masked); i++) masked[i] = '*';
    masked[L < sizeof(masked) - 1 ? L : sizeof(masked) - 1] = '\0';
    return pageros_shell_emit_wifi_config(b, c, n, s.ssid, masked, s.wifi_status);
}

static void mount_wifi(void)
{
    ESP_LOGI("shell", "mount_wifi");
    s.page = SHELL_PAGE_WIFI;
    s.focus.widget_index = 2;       // first input (SSID) in the body
    s.focus.item_index = 0;
    s.text_input_widget = 2;
    s.text_buf = s.ssid;
    s.text_cap = sizeof(s.ssid);
    // Pre-fill with saved creds if present.
    pageros_wifi_creds_load(s.ssid, sizeof(s.ssid), s.psk, sizeof(s.psk));
    if (pageros_wifi_is_connected()) {
        strncpy(s.wifi_status, "Connected", sizeof(s.wifi_status));
    } else if (s.wifi_status[0] == '\0') {
        strncpy(s.wifi_status, "Disconnected", sizeof(s.wifi_status));
    }
    s.wifi_status[sizeof(s.wifi_status) - 1] = '\0';
    mount_and_render(emit_wifi_thunk);
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
    s.focus.widget_index = 3;       // URL input (heading=0, [notif?], hint, input, install, back)
    if (s.addapp_msg[0]) s.focus.widget_index = 4;  // shift down if notif visible
    s.focus.item_index = 0;
    s.text_input_widget = s.focus.widget_index;
    s.text_buf = s.url;
    s.text_cap = sizeof(s.url);
    mount_and_render(emit_addapp_thunk);
}

// Map an input/button widget index → which text buffer to edit, or -1
// if the widget at that index isn't a text input on the current page.
static void rebind_text_buf_for_focus(void)
{
    s.text_input_widget = -1;
    s.text_buf = NULL; s.text_cap = 0;
    if (s.page == SHELL_PAGE_WIFI) {
        if (s.focus.widget_index == 2) { s.text_input_widget = 2; s.text_buf = s.ssid; s.text_cap = sizeof(s.ssid); }
        else if (s.focus.widget_index == 3) { s.text_input_widget = 3; s.text_buf = s.psk;  s.text_cap = sizeof(s.psk); }
    } else if (s.page == SHELL_PAGE_ADD_APP) {
        int url_idx = s.addapp_msg[0] ? 4 : 3;
        if (s.focus.widget_index == url_idx) {
            s.text_input_widget = url_idx; s.text_buf = s.url; s.text_cap = sizeof(s.url);
        }
    }
}

// --- Button action dispatcher ------------------------------------- //
//
// Called with a button's `href` when the user presses ENTER on it.

static void wifi_connect_async(void)
{
    if (s.ssid[0] == '\0') {
        strncpy(s.wifi_status, "SSID is empty", sizeof(s.wifi_status));
        s.wifi_status[sizeof(s.wifi_status) - 1] = '\0';
        return;
    }
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

static void addapp_install(void)
{
    if (s.url[0] == '\0') {
        strncpy(s.addapp_msg, "URL is empty", sizeof(s.addapp_msg));
        strncpy(s.addapp_msg_level, "warn", sizeof(s.addapp_msg_level));
        return;
    }
    if (!pageros_wifi_is_connected()) {
        strncpy(s.addapp_msg, "Wi-Fi not connected", sizeof(s.addapp_msg));
        strncpy(s.addapp_msg_level, "warn", sizeof(s.addapp_msg_level));
        return;
    }
    char app_id[PAGEROS_SIDELOAD_MAX_APP_ID];
    esp_err_t r = pageros_sideload_install_from_url(s.url, app_id, sizeof(app_id));
    if (r == ESP_OK) {
        // Truncate app_id to fit the notification line.
        snprintf(s.addapp_msg, sizeof(s.addapp_msg), "Installed %.48s", app_id);
        strncpy(s.addapp_msg_level, "info", sizeof(s.addapp_msg_level));
    } else {
        snprintf(s.addapp_msg, sizeof(s.addapp_msg), "Install failed: %s",
                 esp_err_to_name(r));
        strncpy(s.addapp_msg_level, "error", sizeof(s.addapp_msg_level));
    }
    s.addapp_msg_level[sizeof(s.addapp_msg_level) - 1] = '\0';
}

// Returns true if the href was recognised + acted on.
static bool handle_button_href(const char *href)
{
    if (!href) return false;
    if (strcmp(href, "shell:home") == 0)      { mount_home();     return true; }
    if (strcmp(href, "shell:settings") == 0)  { mount_settings(); return true; }
    if (strcmp(href, "shell:wifi") == 0)      { mount_wifi();     return true; }
    if (strcmp(href, "shell:add-app") == 0)   { mount_add_app();  return true; }
    if (strcmp(href, "shell:about") == 0) {
        // Inline About — overload settings emitter for now; a real
        // about screen lands in a later iteration.
        strncpy(s.addapp_msg, "PagerOS pre-alpha · CC0", sizeof(s.addapp_msg));
        strncpy(s.addapp_msg_level, "info", sizeof(s.addapp_msg_level));
        return true;
    }
    if (strcmp(href, "shell:wifi-connect") == 0) {
        wifi_connect_async();
        mount_and_render(emit_wifi_thunk);
        return true;
    }
    if (strcmp(href, "shell:install") == 0) {
        addapp_install();
        mount_and_render(emit_addapp_thunk);
        return true;
    }
    if (strncmp(href, "open:", 5) == 0) {
        ESP_LOGI("shell", "ENTER → open app %s (app launch TBD)", href + 5);
        return true;
    }
    return false;
}

// Shell input handler — owns navigation between widgets and items in
// the foreground Frame. Encoder = scroll within the focused widget;
// ENTER = activate; BACK = apprt_back.
// How many "focusable" widgets does the current page have, in render
// order? Used to bound encoder navigation across inputs + buttons.
//
// HOME / SETTINGS / ABOUT — focus stays on the list widget (item navs).
// WIFI — 4 stops: SSID input, password input, Connect button, Back button.
// ADD_APP — 3 stops: URL input, Install button, Back button (+ shifted if notif visible).

static void page_advance_focus(int step)
{
    int n_stops = 0;
    int stops[8];

    switch (s.page) {
    case SHELL_PAGE_HOME:
    case SHELL_PAGE_SETTINGS: {
        // Single list widget; move item_index.
        const pgr_cbor_value_t *fr = pageros_apprt_foreground_frame();
        int n = focused_list_len(fr, 1);
        if (n > 0) {
            s.focus.widget_index = 1;
            s.focus.item_index = (s.focus.item_index + step + n) % n;
        }
        rebind_text_buf_for_focus();
        return;
    }
    case SHELL_PAGE_WIFI:
        // body order: heading(0), status(1), ssid(2), pwd(3), Connect(4), Back(5)
        stops[n_stops++] = 2;
        stops[n_stops++] = 3;
        stops[n_stops++] = 4;
        stops[n_stops++] = 5;
        break;
    case SHELL_PAGE_ADD_APP: {
        // body order: heading(0), [notif?](1), hint, url, install, back
        int notif = s.addapp_msg[0] ? 1 : 0;
        stops[n_stops++] = 2 + notif;     // hint? actually input is after hint
        // Correction: order is heading, [notif], hint, input, install, back.
        // So indices: heading=0, notif=1?, hint=1or2, input=2or3, install=3or4, back=4or5.
        n_stops = 0;
        int base = 2 + notif;             // input
        stops[n_stops++] = base;
        stops[n_stops++] = base + 1;      // install
        stops[n_stops++] = base + 2;      // back
        break;
    }
    }
    if (n_stops == 0) return;
    // Find current index in the stops table; if not present, snap to 0.
    int cur = 0;
    for (int i = 0; i < n_stops; i++) if (stops[i] == s.focus.widget_index) { cur = i; break; }
    cur = (cur + step + n_stops) % n_stops;
    s.focus.widget_index = stops[cur];
    s.focus.item_index = 0;
    rebind_text_buf_for_focus();
}

static bool default_shell_handler(const pageros_router_event_t *ev, void *ctx)
{
    (void)ctx;
    pageros_power_kick();
    const pgr_cbor_value_t *fr = pageros_apprt_foreground_frame();
    bool dirty = false;

    // Verbose event log — helps diagnose phantom navigation. Drop or
    // demote to LOGD once the input chain is trusted.
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
    ESP_LOGI("shell", "EVT %s (%d) page=%d focus.w=%d focus.i=%d",
             kind, detail, (int)s.page, s.focus.widget_index, s.focus.item_index);

    switch (ev->kind) {
    case PAGEROS_ROUTER_EVT_ENCODER: {
        int step = (ev->as.enc == PAGEROS_ROUTER_ENC_CW) ? +1 : -1;
        page_advance_focus(step);
        dirty = true;
        break;
    }
    case PAGEROS_ROUTER_EVT_NAV:
        if (ev->as.nav == PAGEROS_ROUTER_NAV_ENTER) {
            char href_buf[96];
            const char *href = focused_href(fr, s.focus.widget_index,
                                            s.focus.item_index, href_buf, sizeof(href_buf));
            // Buttons live as standalone widgets in the body — their
            // href is on the widget itself, not inside a list. Fall
            // back to scanning the widget directly for an "href" field
            // when focused_href() (list-item lookup) misses.
            if (!href && fr && fr->kind == PGR_CBOR_KIND_MAP) {
                const pgr_cbor_value_t *body = NULL;
                for (size_t i = 0; i < fr->v.map.len; i++) {
                    const pgr_cbor_pair_t *p = &fr->v.map.items[i];
                    if (p->key.kind == PGR_CBOR_KIND_TEXT &&
                        p->key.v.bytes.len == 4 &&
                        memcmp(p->key.v.bytes.data, "body", 4) == 0) { body = &p->val; break; }
                }
                if (body && body->kind == PGR_CBOR_KIND_ARRAY &&
                    s.focus.widget_index >= 0 &&
                    s.focus.widget_index < (int)body->v.arr.len) {
                    const pgr_cbor_value_t *w = &body->v.arr.items[s.focus.widget_index];
                    if (w->kind == PGR_CBOR_KIND_MAP) {
                        for (size_t i = 0; i < w->v.map.len; i++) {
                            const pgr_cbor_pair_t *p = &w->v.map.items[i];
                            if (p->key.kind == PGR_CBOR_KIND_TEXT &&
                                p->key.v.bytes.len == 4 &&
                                memcmp(p->key.v.bytes.data, "href", 4) == 0 &&
                                p->val.kind == PGR_CBOR_KIND_TEXT) {
                                size_t l = p->val.v.bytes.len;
                                if (l >= sizeof(href_buf)) l = sizeof(href_buf) - 1;
                                memcpy(href_buf, p->val.v.bytes.data, l);
                                href_buf[l] = '\0';
                                href = href_buf;
                                break;
                            }
                        }
                    }
                }
            }
            if (href) {
                ESP_LOGI("shell", "ENTER → %s", href);
                // handle_button_href mounts a new page when applicable
                // and that mount already renders. Don't set dirty=true
                // afterwards — the fall-through re-render at the end of
                // this handler would queue a SECOND present in quick
                // succession and overflow the SPI transmit queue.
                handle_button_href(href);
            }
        } else if (ev->as.nav == PAGEROS_ROUTER_NAV_BACK_LONG) {
            // Long press: always go back to Home regardless of state.
            mount_home();
            dirty = true;
        } else if (ev->as.nav == PAGEROS_ROUTER_NAV_BACK) {
            // Short press: if a text input has focus, treat as
            // backspace; otherwise navigate one screen back.
            if (s.text_input_widget == s.focus.widget_index && s.text_buf) {
                size_t L = strlen(s.text_buf);
                if (L > 0) { s.text_buf[L - 1] = '\0'; dirty = true; }
            } else {
                switch (s.page) {
                case SHELL_PAGE_SETTINGS: mount_home();     dirty = true; break;
                case SHELL_PAGE_WIFI:
                case SHELL_PAGE_ADD_APP:  mount_settings(); dirty = true; break;
                case SHELL_PAGE_HOME:     break;
                }
            }
        }
        break;
    case PAGEROS_ROUTER_EVT_KEY: {
        if (!ev->as.key.pressed) return true;  // act on press, ignore release
        char c = keymap_lookup(ev->as.key.row, ev->as.key.col);
        ESP_LOGI("shell", "key r=%u c=%u ch='%c'(0x%02x)%s",
                 ev->as.key.row, ev->as.key.col,
                 (c >= 0x20 && c < 0x7f) ? c : '?', (unsigned)c,
                 c == 0 ? " UNMAPPED" : "");
        if (!s.text_buf) break;
        if (c == '\b' || c == 0x7f) {            // Backspace key on the matrix
            size_t L = strlen(s.text_buf);
            if (L > 0) { s.text_buf[L - 1] = '\0'; dirty = true; }
            break;
        }
        if (c == 0) break;                       // unmapped — bubble to logs
        size_t L = strlen(s.text_buf);
        if (L + 1 < s.text_cap) {
            s.text_buf[L] = c;
            s.text_buf[L + 1] = '\0';
            dirty = true;
        }
        break;
    }
    default:
        return false;
    }

    if (dirty) {
        // Re-emit the current page so input widgets pick up the new
        // buffer contents (focus + text), then push to the display.
        switch (s.page) {
        case SHELL_PAGE_HOME:     /* home content doesn't change with focus */ break;
        case SHELL_PAGE_SETTINGS: /* same */ break;
        case SHELL_PAGE_WIFI:     mount_and_render(emit_wifi_thunk); return true;
        case SHELL_PAGE_ADD_APP:  mount_and_render(emit_addapp_thunk); return true;
        }
        render_foreground();
    }
    return true;
}

static void on_gps_fix(const pageros_gps_fix_t *fix, void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI("gps",
             "fix: lat=%.6f lon=%.6f acc=%.1fm sats=%u utc_ms=%llu",
             fix->latitude_deg, fix->longitude_deg,
             (double)fix->accuracy_m, (unsigned)fix->satellites,
             (unsigned long long)fix->utc_epoch_ms);
}

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

    // Boot self-test — see SPEC.md §7.2 step 2 / TASKS.md FW-002.
    selftest_result_t st = selftest_run();
    selftest_log_result(&st);
    selftest_halt_if_hard_fail(&st);

    // NVS must be up before identity load (FW-014). If the partition is
    // truncated or full from a prior boot we erase + re-init so first-
    // boot identity gen still succeeds — destroying existing pageros_id
    // state is the lesser evil vs. wedging the device.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES
            || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs needs erase (%s); reformatting",
                 esp_err_to_name(nvs_err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    // Identity load (FW-014). First boot generates an Ed25519 keypair;
    // subsequent boots load it from NVS. The seed is the only thing
    // stored — the pubkey is derived each boot via FW-015 crypto.
    esp_err_t id_err = pageros_identity_init();
    if (id_err != ESP_OK) {
        ESP_LOGE(TAG, "identity init failed: %s — continuing without "
                      "stable identity", esp_err_to_name(id_err));
    }

    // Shared I2C bus + XL9555 port expander come up FIRST after NVS.
    // The XL9555 gates power to most peripherals on this board (LoRa,
    // GPS, NFC, SD, speaker amp) — without flipping its enable bits
    // the SPI/UART probes for those chips return chip-not-present.
    // See pageros_xl9555.h for the bit assignments.
    esp_err_t i2c_err = pageros_i2c_bus_init();
    if (i2c_err != ESP_OK) {
        ESP_LOGW(TAG, "shared I2C bus init failed: %s", esp_err_to_name(i2c_err));
    }
    esp_err_t xl_err = pageros_xl9555_init();
    if (xl_err == ESP_OK) {
        // Power on every gated peripheral, release the keyboard + GPS
        // chip resets, and arm the SD pull-ups so the card slot drives
        // valid lines. KB_EN and KB_RST are BOTH required for the
        // TCA8418 — KB_EN gates VCC, KB_RST is the active-low chip
        // reset; without VCC, releasing reset is a no-op and the chip
        // never ACKs on I2C.
        pageros_xl9555_set(PAGEROS_XL_LORA_EN,   true);
        pageros_xl9555_set(PAGEROS_XL_GPS_EN,    true);
        pageros_xl9555_set(PAGEROS_XL_GPS_RST,   true);   // active LOW, drive HIGH = run
        pageros_xl9555_set(PAGEROS_XL_NFC_EN,    true);
        pageros_xl9555_set(PAGEROS_XL_SD_EN,     true);
        pageros_xl9555_set(PAGEROS_XL_SD_PULLEN, true);
        pageros_xl9555_set(PAGEROS_XL_KB_RST,    true);   // active LOW, drive HIGH = run
        pageros_xl9555_set(PAGEROS_XL_KB_EN,     true);   // TCA8418 VCC enable
        pageros_xl9555_set(PAGEROS_XL_AMP_EN,    true);
        pageros_xl9555_set(PAGEROS_XL_DRV_EN,    true);   // DRV2605 haptic VCC
        pageros_xl9555_set(PAGEROS_XL_GPIO_EN,   true);   // generic peripheral gate
        // Give the powered chips ≥ TCA8418's 20 ms POR + a margin
        // before any I2C probe goes out.
        vTaskDelay(pdMS_TO_TICKS(80));
        ESP_LOGI(TAG, "XL9555 enables asserted (LoRa+GPS+NFC+SD+amp+kb+drv+gpio)");
    } else {
        ESP_LOGW(TAG, "XL9555 init failed: %s — peripherals stay gated off",
                 esp_err_to_name(xl_err));
    }

    // Display bring-up (FW-005) BEFORE storage. The display, SD card,
    // and LoRa all share SPI2_HOST; the display's CS=38 has no
    // external pull-up so it floats until the display driver
    // configures it as a driven-HIGH output. If storage init runs
    // first, the SD's SPI transactions reach the ST7796 (which has
    // no hardware reset pin: DISP_PIN_RST = -1, so it can't be
    // recovered) and the panel locks up showing black even though
    // our subsequent render reports success. Initialising display
    // first parks CS=38 high so SD traffic stays on the SD only.
    esp_err_t disp_err = pageros_display_init();
    if (disp_err == ESP_OK) {
        pageros_display_fill(pageros_display_rgb565(0x00, 0x00, 0x00));
        pageros_display_present();
    } else {
        ESP_LOGW(TAG, "display init failed: %s", esp_err_to_name(disp_err));
    }

    // Storage mount (FW-003). Creates the §7.4 directory tree on first
    // boot. Soft-fails — the shell renders a recovery screen when no
    // card is present (SPEC §7.2 step 4 + docs/user/troubleshooting),
    // and that's a shell-level concern, not the storage driver's.
    esp_err_t sd_err = pageros_storage_init();
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s — proceeding without SD",
                 esp_err_to_name(sd_err));
    }
    // SD init pumps significant traffic on the shared SPI bus; the
    // ST7796 has been observed to glitch its DISP_ON state during the
    // SD card's init sequence even with CS deasserted. Re-arm it now.
    if (disp_err == ESP_OK) (void)pageros_display_panel_reinit();

    // Logger (FW-004). Initialise after storage so the first LOG_INFO
    // line lands on /sd/logs/pageros.log; falls back to console-only
    // when the SD mount above failed. Any earlier code paths that need
    // a durable line can also call pageros_logger_init() again later —
    // it's idempotent and will pick up SD the moment it appears.
    esp_err_t log_err = pageros_logger_init();
    if (log_err != ESP_OK) {
        ESP_LOGW(TAG, "logger init failed: %s", esp_err_to_name(log_err));
    } else {
        LOG_INFO(TAG, "logger online, sd=%s",
                 pageros_logger_is_writing_to_sd() ? "yes" : "no");
    }

    // LoRa bring-up (FW-008). 868 MHz / SF7 / 125 kHz is a quiet
    // boot-time default — actual channel choice belongs to the
    // higher-level mesh stack (LORA-* tasks). Soft-fails so a missing
    // SX1262 (e.g. dev board with the LoRa power gate off) doesn't
    // block the rest of the system from booting.
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

    // Wi-Fi station bring-up (FW-009). We init the radio so the HTTPS
    // client and the shell's settings flow can both reach the API; the
    // actual `pageros_wifi_connect(ssid, pwd, ...)` call is owned by
    // the shell once it has credentials in NVS — there's no SSID source
    // here yet (FW-029 owns the Wi-Fi config UX).
    esp_err_t wifi_err = pageros_wifi_init();
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "wifi init failed: %s", esp_err_to_name(wifi_err));
    } else {
        // Auto-connect on boot if Settings → Wi-Fi has saved creds.
        char saved_ssid[33] = {0}, saved_psk[65] = {0};
        if (pageros_wifi_creds_load(saved_ssid, sizeof(saved_ssid),
                                    saved_psk, sizeof(saved_psk)) == ESP_OK
                && saved_ssid[0]) {
            ESP_LOGI(TAG, "Wi-Fi auto-connect to '%s'", saved_ssid);
            esp_err_t wc = pageros_wifi_connect(saved_ssid, saved_psk, 15000);
            if (wc != ESP_OK) {
                ESP_LOGW(TAG, "auto-connect failed: %s — Settings → Wi-Fi to fix",
                         esp_err_to_name(wc));
            }
        }
    }

    // FW-019 / FW-020 / FW-028 / FW-029 — render the Shell home Frame
    // into the display's framebuffer and present. This replaces the
    // boot-splash gradient with the actual home screen so the user sees
    // something legible at first boot.
    pageros_fonts_init(NULL);
    pageros_apprt_init(NULL);
    pageros_shell_init();
    if (disp_err == ESP_OK) {
        if (pageros_shell_mount_home() == ESP_OK) {
            pageros_widgets_palette_t pal;
            pageros_widgets_default_palette(&pal);
            pageros_widgets_ctx_t ctx = {
                .canvas  = {
                    .pixels = (uint16_t *)pageros_display_framebuffer(),
                    .w      = PAGEROS_DISPLAY_WIDTH,
                    .h      = PAGEROS_DISPLAY_HEIGHT,
                },
                .palette = pal,
                .focus   = { .widget_index = 0, .item_index = 0, .scroll = 0 },
                .title   = "PagerOS",
                .help    = "MENU = up/down  ENTER = open",
            };
            const pgr_cbor_value_t *fr = pageros_apprt_foreground_frame();
            if (fr) {
                const pgr_cbor_value_t *body = NULL;
                // Pull the "body" array out of the Frame map.
                if (fr->kind == PGR_CBOR_KIND_MAP) {
                    for (size_t i = 0; i < fr->v.map.len; i++) {
                        const pgr_cbor_pair_t *p = &fr->v.map.items[i];
                        if (p->key.kind == PGR_CBOR_KIND_TEXT &&
                            p->key.v.bytes.len == 4 &&
                            memcmp(p->key.v.bytes.data, "body", 4) == 0) {
                            body = &p->val;
                            break;
                        }
                    }
                }
                if (pageros_widgets_render_screen(&ctx, body) == ESP_OK) {
                    pageros_display_present();
                    ESP_LOGI(TAG, "rendered Shell home Frame to display");
                } else {
                    ESP_LOGW(TAG, "Shell render failed");
                }
            } else {
                ESP_LOGW(TAG, "Shell mounted but no foreground frame");
            }
        } else {
            ESP_LOGW(TAG, "Shell mount_home failed");
        }
    }

    // GPS bring-up (FW-011). Soft-fails: if the receiver isn't present
    // or the I2C expander mis-detects, log and continue so the rest of
    // the device still boots. Foreground apps that need location query
    // pageros_gps_get_last_fix(); subscribed apps can register their own
    // callback via pageros_gps_set_fix_callback().
    esp_err_t gps_err = pageros_gps_init();
    if (gps_err == ESP_OK) {
        pageros_gps_set_fix_callback(on_gps_fix, NULL);
    } else {
        ESP_LOGW(TAG, "GPS init skipped: %s", esp_err_to_name(gps_err));
    }

    // Input bring-up (FW-007 encoder + ENTER/BACK).
    esp_err_t input_err = pageros_input_init();
    if (input_err != ESP_OK) {
        ESP_LOGW(TAG, "input init skipped: %s", esp_err_to_name(input_err));
    }

    // Keyboard bring-up (FW-006). Keymap translation lives at the
    // shell/widget layer; the driver only emits raw (row,col) events.
    esp_err_t kbd_err = pageros_kbd_init();
    if (kbd_err != ESP_OK) {
        ESP_LOGW(TAG, "kbd init skipped: %s", esp_err_to_name(kbd_err));
    }

    // IMU stub (FW-012). Confirms the BHI260AP is present on the bus
    // so future fusion-firmware work has a real probe to start from;
    // no event dispatch in v1 per TASKS.md.
    esp_err_t imu_err = pageros_imu_init();
    if (imu_err != ESP_OK) {
        ESP_LOGW(TAG, "IMU init skipped: %s", esp_err_to_name(imu_err));
    }

    // Power management (FW-035). Defaults: dim at 30 s, screen off at
    // 60 s, light sleep at 5 min. Backlight ACTIVE = full.
    pageros_power_init(NULL);

    // Input router (FW-024): joins both driver queues into one dispatcher
    // and routes events to the focused widget, falling back to the app
    // shell on bubble. Until a real foreground app installs a focus
    // handler, every event flows to `default_shell_handler` directly.
    if (input_err == ESP_OK || kbd_err == ESP_OK) {
        esp_err_t r = pageros_router_init();
        if (r == ESP_OK) {
            pageros_router_set_shell_handler(default_shell_handler, NULL);
        } else {
            ESP_LOGW(TAG, "router init skipped: %s", esp_err_to_name(r));
        }
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
