// PagerOS firmware entry point (post-pivot).
//
// One app, one purpose: the on-device AI agent terminal ("cyberdeck"
// mode). Boot brings up every hardware subsystem, the lock screen
// gates entry with the "press enter to jack in" prompt, and ENTER
// hands the user straight into the agent. Everything else — the old
// desktop, marketplace, widget Frames, sideload pipeline, builtin
// apps — has moved to firmware/legacy/ for reference and is no
// longer in the build.
//
// What's left in main:
//   - boot bring-up of every subsystem
//   - the lock screen (with optional 4-digit PIN)
//   - the agent terminal UI (chrome bars + transcript + input)
//   - a small shell handler that routes input between those two pages
//
// All "tools" the agent can call live in firmware/components/agent/.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "pageros_agent.h"
#include "pageros_audio.h"
#include "pageros_cyber_fx.h"
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
#include "pageros_storage.h"
#include "pageros_widgets.h"
#include "pageros_xl9555.h"
#include "selftest.h"

static const char *TAG = "pageros";

// --- Shell state -------------------------------------------------- //

typedef enum {
    SHELL_PAGE_LOCK = 0,
    SHELL_PAGE_AGENT,
} shell_page_t;

static struct {
    shell_page_t page;

    // Lock screen PIN.
    char  lock_pin_buf[5];
    char  lock_pin_stored[5];
    bool  lock_pin_required;
    int   lock_attempts;

    // Agent terminal.
    char  agent_input[256];
    char  agent_status[80];      // "READY" / "THINKING" / "TOOL X" / "ERR Y"
    int   agent_scroll_offset;   // 0 = follow tail; +N = lines pinned

    uint64_t boot_us;
} s = {
    .page = SHELL_PAGE_LOCK,
};

static pageros_widgets_palette_t g_palette;
static SemaphoreHandle_t g_render_mu;
static void render_screen(void);
static void render_screen_locked(void);

// --- Boot log (cyberpunk roll on splash) -------------------------- //

#define BOOT_LOG_MAX 14

typedef enum { BL_INFO = 0, BL_OK, BL_WARN, BL_ERROR } boot_log_lvl_t;

static struct {
    char           lines[BOOT_LOG_MAX][72];
    boot_log_lvl_t levels[BOOT_LOG_MAX];
    int            count;
} g_boot;

static void boot_log_add(boot_log_lvl_t lvl, const char *fmt, ...)
{
    if (g_boot.count >= BOOT_LOG_MAX) {
        // shift up
        for (int i = 1; i < BOOT_LOG_MAX; i++) {
            memcpy(g_boot.lines[i - 1], g_boot.lines[i], sizeof(g_boot.lines[0]));
            g_boot.levels[i - 1] = g_boot.levels[i];
        }
        g_boot.count = BOOT_LOG_MAX - 1;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_boot.lines[g_boot.count], sizeof(g_boot.lines[0]), fmt, ap);
    va_end(ap);
    g_boot.levels[g_boot.count] = lvl;
    g_boot.count++;
}

static void boot_splash_render(void)
{
    pageros_fonts_canvas_t canvas = {
        .pixels = (uint16_t *)pageros_display_framebuffer(),
        .w      = PAGEROS_DISPLAY_WIDTH,
        .h      = PAGEROS_DISPLAY_HEIGHT,
    };
    if (!canvas.pixels) return;
    pageros_widgets_fill_rect(&canvas, 0, 0, canvas.w, canvas.h, g_palette.bg);
    pageros_fonts_draw_text(&canvas, 8, 4, "[BOOT//PAGEROS]", -1,
                            g_palette.accent, canvas.w);
    pageros_fonts_draw_text(&canvas, canvas.w - 88, 4,
                            "v0.JACK-IN", -1, g_palette.dim, 90);
    for (int i = 0; i < g_boot.count; i++) {
        uint16_t c = g_palette.fg;
        switch (g_boot.levels[i]) {
        case BL_OK:    c = g_palette.info;   break;
        case BL_WARN:  c = g_palette.warn;   break;
        case BL_ERROR: c = g_palette.error;  break;
        default:       c = g_palette.fg;
        }
        pageros_fonts_draw_text(&canvas, 8, 20 + i * 12,
                                g_boot.lines[i], -1, c, canvas.w - 16);
    }
    pageros_display_present();
}

// --- Lock screen PIN persistence ---------------------------------- //

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

// --- Chrome state for top/bot bars -------------------------------- //

static const char *power_state_str(void)
{
    switch (pageros_power_state()) {
    case PAGEROS_POWER_ACTIVE:      return "ACT";
    case PAGEROS_POWER_DIM:         return "DIM";
    case PAGEROS_POWER_SCREEN_OFF:  return "OFF";
    case PAGEROS_POWER_LIGHT_SLEEP: return "LSL";
    case PAGEROS_POWER_DEEP_SLEEP:  return "DSL";
    }
    return "?";
}

static void collect_chrome_state(pageros_chrome_state_t *out, char *id_buf)
{
    memset(out, 0, sizeof(*out));
    int64_t up_us = esp_timer_get_time();
    int up_min   = (int)(up_us / 1000000 / 60);
    out->hh = (up_min / 60) % 24;
    out->mm = up_min % 60;

    char fp[PAGEROS_IDENTITY_FP_LEN] = {0};
    if (pageros_identity_fingerprint(fp) == ESP_OK && fp[0]) {
        // Show only the first 9 chars to fit in the topbar.
        strncpy(id_buf, fp, 9);
        id_buf[9] = '\0';
        out->identity_short = id_buf;
    }

    // No battery gauge wired yet — show unknown until a power-monitor
    // tool lands.
    out->battery_pct = -1;

    out->wifi_state  = pageros_wifi_is_connected() ? 2 : 0;
    out->lora_state  = pageros_lora_is_ready() ? 1 : 0;
    pageros_gps_fix_t fix = {0};
    out->gps_state   = (pageros_gps_get_last_fix(&fix) == ESP_OK
                        && fix.accuracy_m > 0.0f) ? 1 : 0;
    out->power_mode  = power_state_str();
}

// --- Lock screen render (preserved verbatim from legacy) ---------- //

static void render_lock(pageros_fonts_canvas_t *canvas)
{
    pageros_widgets_fill_rect(canvas, 0, 0, canvas->w, canvas->h, g_palette.bg);
    pageros_cyber_fx_paint_matrix(canvas, &g_palette, 0, 0,
                                  canvas->w, canvas->h);

    pageros_fonts_draw_text(canvas, 8, 4,
                            "[SYS//LOCK·SECURE-LINK-OK]", -1,
                            g_palette.info, canvas->w);
    pageros_fonts_draw_text(canvas, canvas->w - 88, 4,
                            "v0.JACK-IN", -1, g_palette.dim, 90);

    pageros_cyber_fx_draw_bracket_pair(canvas, 50, 24,
                                       canvas->w - 100, 64,
                                       g_palette.accent);

    int64_t up_us = esp_timer_get_time();
    int up_min   = (int)(up_us / 1000000 / 60);
    int hh       = (up_min / 60) % 24;
    int mm       = up_min % 60;
    char clock[8];
    snprintf(clock, sizeof(clock), "%02d:%02d", hh, mm);
    int clock_w = 16 * 3 * (int)strlen(clock);
    int cx = (canvas->w - clock_w) / 2;
    pageros_fonts_draw_text_scaled_16(canvas, cx + 2, 36, clock, -1,
                                      g_palette.accent, 3);
    pageros_fonts_draw_text_scaled_16(canvas, cx, 34, clock, -1,
                                      g_palette.info, 3);

    char fp[PAGEROS_IDENTITY_FP_LEN] = {0};
    if (pageros_identity_fingerprint(fp) == ESP_OK && fp[0]) {
        char buf[24];
        snprintf(buf, sizeof(buf), "▶ID:%s◀", fp);
        int bw = pageros_fonts_measure_text_16(buf, -1);
        pageros_fonts_draw_text_16(canvas, (canvas->w - bw) / 2,
                                   34 + 48 + 6, buf, -1,
                                   g_palette.dim, canvas->w);
    }

    int box_y = 34 + 48 + 8 + 28;
    if (s.lock_pin_required) {
        int total = 4 * 32 + 3 * 8;
        int x0 = (canvas->w - total) / 2;
        int filled = (int)strlen(s.lock_pin_buf);
        for (int i = 0; i < 4; i++) {
            int cx0 = x0 + i * (32 + 8);
            pageros_widgets_fill_rect(canvas, cx0, box_y, 32, 32, g_palette.bg);
            pageros_widgets_outline_rect(canvas, cx0, box_y, 32, 32,
                                         g_palette.info);
            if (i < filled) {
                pageros_widgets_outline_rect(canvas, cx0 + 2, box_y + 2,
                                             28, 28, g_palette.accent);
                pageros_widgets_fill_rect(canvas, cx0 + 10, box_y + 10,
                                          12, 12, g_palette.accent);
            }
        }
        const char *prompt = "// ENTER ACCESS PIN ::";
        int pw = pageros_fonts_measure_text(prompt, -1);
        pageros_fonts_draw_text(canvas, (canvas->w - pw) / 2,
                                box_y + 32 + 12, prompt, -1,
                                g_palette.accent, canvas->w);
    } else {
        const char *prompt = "▶ PRESS ENTER TO JACK IN ◀";
        int pw = pageros_fonts_measure_text_16(prompt, -1);
        pageros_fonts_draw_text_16(canvas, (canvas->w - pw) / 2,
                                   box_y + 8, prompt, -1,
                                   g_palette.accent, canvas->w);
    }

    pageros_widgets_chrome_scanlines(canvas, 0, 0, canvas->w, canvas->h, 2);

    char btag[64];
    snprintf(btag, sizeof(btag), "[NET:%s][RF:%s][PWR:%s]",
             pageros_wifi_is_connected() ? "ONLINE" : "OFFLINE",
             pageros_lora_is_ready() ? "ARMED" : "IDLE",
             power_state_str());
    pageros_fonts_draw_text(canvas, 8, canvas->h - 12, btag, -1,
                            g_palette.dim, canvas->w);
}

// --- Agent terminal render --------------------------------------- //
//
// Standard chrome (topbar + botbar), solid bg, transcript pane with
// role-tagged turns, a status line above the input. No matrix rain —
// readability over vibe inside the terminal itself. The jack-in
// aesthetic lives entirely on the lock screen.

static uint16_t agent_role_color(pageros_agent_role_t r)
{
    switch (r) {
    case PAGEROS_AGENT_ROLE_USER:      return g_palette.accent;
    case PAGEROS_AGENT_ROLE_ASSISTANT: return g_palette.fg;
    case PAGEROS_AGENT_ROLE_TOOL:      return g_palette.dim;
    case PAGEROS_AGENT_ROLE_SYSTEM:    return g_palette.dim;
    }
    return g_palette.fg;
}

static const char *agent_role_tag(pageros_agent_role_t r)
{
    switch (r) {
    case PAGEROS_AGENT_ROLE_USER:      return "USR";
    case PAGEROS_AGENT_ROLE_ASSISTANT: return "AGT";
    case PAGEROS_AGENT_ROLE_TOOL:      return "TOOL";
    case PAGEROS_AGENT_ROLE_SYSTEM:    return "SYS";
    }
    return "?";
}

// Char-wrap with the compact 8x8 glyph path.
static int agent_draw_wrapped(const pageros_fonts_canvas_t *canvas,
                              int x, int y_base, int max_w, int line_h,
                              int y_clip_top, int y_clip_bot,
                              const char *text, uint16_t color)
{
    if (!text || !*text) return 0;
    int per_line = max_w / 6;
    if (per_line < 12) per_line = 12;
    int line = 0;
    const char *p = text;
    while (*p) {
        char buf[160];
        int n = 0;
        while (*p && *p != '\n' && n < per_line && n < (int)sizeof(buf) - 1) {
            buf[n++] = *p++;
        }
        buf[n] = '\0';
        if (*p == '\n') p++;
        int y = y_base + line * line_h;
        if (y >= y_clip_top && y + 8 <= y_clip_bot) {
            pageros_fonts_draw_text(canvas, x, y, buf, n, color, max_w);
        }
        line++;
        // Don't break on empty line — a turn like "Here's what I have:\n…"
        // produces a legitimate empty row between the prefix and the body,
        // and we still want to draw the body afterwards.
    }
    return line;
}

static void render_agent(pageros_fonts_canvas_t *canvas)
{
    // Solid background.
    pageros_widgets_fill_rect(canvas, 0, 0, canvas->w, canvas->h, g_palette.bg);

    // Standard chrome bars.
    pageros_chrome_state_t cs;
    char id_buf[10];
    collect_chrome_state(&cs, id_buf);
    pageros_widgets_chrome_topbar(canvas, &g_palette, &cs);
    pageros_widgets_chrome_botbar(canvas, &g_palette, &cs);

    // Body region.
    int body_y = PAGEROS_CHROME_BAR_H;
    int body_h = canvas->h - 2 * PAGEROS_CHROME_BAR_H;

    // Status line at the top of the body, color-coded.
    const char *status = s.agent_status[0] ? s.agent_status
                          : (pageros_agent_is_ready() ? "READY" : "OFFLINE");
    uint16_t st_color = g_palette.info;
    if (strncmp(status, "ERR", 3) == 0)        st_color = g_palette.error;
    else if (strncmp(status, "TOOL", 4) == 0)  st_color = g_palette.warn;
    else if (strncmp(status, "THINK", 5) == 0) st_color = g_palette.accent;
    else if (strncmp(status, "OFFL", 4) == 0)  st_color = g_palette.error;

    char status_line[100];
    snprintf(status_line, sizeof(status_line),
             "[%.40s]  %d msgs",
             status, pageros_agent_transcript_count());
    pageros_fonts_draw_text(canvas, 8, body_y + 2, status_line, -1,
                            st_color, canvas->w);

    // Transcript region — compact 8x8 font.
    int t_x      = 8;
    int t_top    = body_y + 14;
    int t_bot    = body_y + body_h - 16;
    int line_h   = 10;
    int max_w    = canvas->w - 16;
    int tag_w    = 32;   // room for "USR>" in 8x8 plus a gap

    int n_turns = pageros_agent_transcript_count();
    int counts[64] = {0};
    int total_lines = 0;
    int per_line = (max_w - tag_w) / 6;
    if (per_line < 12) per_line = 12;
    if (n_turns > 64) n_turns = 64;
    for (int i = 0; i < n_turns; i++) {
        pageros_agent_turn_t t = {0};
        if (!pageros_agent_transcript_get(i, &t)) continue;
        if (t.role == PAGEROS_AGENT_ROLE_SYSTEM) continue;
        const char *txt = t.content ? t.content
                          : (t.tool_summary ? t.tool_summary : "");
        if (t.role == PAGEROS_AGENT_ROLE_ASSISTANT && t.tool_summary
                && (!t.content || !t.content[0])) {
            counts[i] = 1;
            total_lines += 1;
            continue;
        }
        int lines = 1;
        const char *p = txt; int n = 0;
        while (*p) {
            if (*p == '\n' || n >= per_line) { lines++; n = 0; if (*p == '\n') p++; }
            else { n++; p++; }
        }
        counts[i] = lines;
        total_lines += lines;
    }

    int budget = (t_bot - t_top) / line_h;
    if (budget < 3) budget = 3;

    // Clamp scroll. scroll_offset is in lines, measured from the tail.
    int max_scroll = total_lines - budget;
    if (max_scroll < 0) max_scroll = 0;
    if (s.agent_scroll_offset > max_scroll) s.agent_scroll_offset = max_scroll;
    if (s.agent_scroll_offset < 0)          s.agent_scroll_offset = 0;

    // Find the first turn to render. We render `budget + scroll_offset`
    // tail lines starting from `first`; the bottommost scroll_offset
    // lines fall below t_bot and get clipped — that's how scroll works.
    int first   = 0;
    int running = total_lines;
    int target  = budget + s.agent_scroll_offset;
    while (first < n_turns && running > target) {
        running -= counts[first];
        first++;
    }

    int y = t_top;
    for (int i = first; i < n_turns; i++) {
        pageros_agent_turn_t t = {0};
        if (!pageros_agent_transcript_get(i, &t)) continue;
        if (t.role == PAGEROS_AGENT_ROLE_SYSTEM) continue;
        uint16_t color = agent_role_color(t.role);
        const char *tag = agent_role_tag(t.role);
        if (y + 8 <= t_bot && y >= t_top) {
            pageros_fonts_draw_text(canvas, t_x, y, tag, -1, color, tag_w);
        }

        if (t.role == PAGEROS_AGENT_ROLE_ASSISTANT && t.tool_summary
                && (!t.content || !t.content[0])) {
            char dispatch[120];
            snprintf(dispatch, sizeof(dispatch), "-> %s", t.tool_summary);
            int used = agent_draw_wrapped(canvas, t_x + tag_w, y, max_w - tag_w,
                                          line_h, t_top, t_bot,
                                          dispatch, g_palette.warn);
            y += line_h * (used > 0 ? used : 1);
            continue;
        }

        const char *txt = t.content ? t.content
                          : (t.tool_summary ? t.tool_summary : "(empty)");
        int used = agent_draw_wrapped(canvas, t_x + tag_w, y, max_w - tag_w,
                                      line_h, t_top, t_bot, txt, color);
        y += line_h * (used > 0 ? used : 1);
    }

    // Scroll indicator on the right edge.
    if (s.agent_scroll_offset > 0) {
        pageros_fonts_draw_text(canvas, canvas->w - 28, t_top, "[+]", -1,
                                g_palette.warn, 24);
    }

    // Separator + input line.
    int input_y = body_y + body_h - 18;
    pageros_widgets_fill_rect(canvas, 0, input_y - 2, canvas->w, 1,
                              g_palette.dim);

    char prompt[280];
    if (!pageros_agent_is_ready()) {
        snprintf(prompt, sizeof(prompt),
                 "[OFFLINE] no API key / config");
        pageros_fonts_draw_text(canvas, 8, input_y + 4, prompt, -1,
                                g_palette.error, canvas->w);
    } else if (pageros_agent_is_busy()) {
        snprintf(prompt, sizeof(prompt),
                 "[%.20s] please wait...", status);
        pageros_fonts_draw_text(canvas, 8, input_y + 4, prompt, -1,
                                g_palette.warn, canvas->w);
    } else {
        const char *cursor = ((esp_timer_get_time() / 500000) & 1) ? "_" : " ";
        snprintf(prompt, sizeof(prompt), "> %s%s", s.agent_input, cursor);
        pageros_fonts_draw_text(canvas, 8, input_y + 4, prompt, -1,
                                g_palette.accent, canvas->w);
    }
}

// --- Render dispatch -------------------------------------------- //

static void render_screen_locked(void)
{
    pageros_fonts_canvas_t canvas = {
        .pixels = (uint16_t *)pageros_display_framebuffer(),
        .w      = PAGEROS_DISPLAY_WIDTH,
        .h      = PAGEROS_DISPLAY_HEIGHT,
    };
    if (!canvas.pixels) return;

    switch (s.page) {
    case SHELL_PAGE_LOCK:  render_lock(&canvas); break;
    case SHELL_PAGE_AGENT: render_agent(&canvas); break;
    }

    pageros_cyber_fx_paint_transition(&canvas, &g_palette);
    pageros_widgets_chrome_toast(&canvas, &g_palette);
    pageros_cyber_fx_paint_overlay(&canvas, &g_palette);
    pageros_display_present();
}

static void render_screen(void)
{
    static shell_page_t last_page = (shell_page_t)-1;
    if (s.page != last_page) {
        last_page = s.page;
        pageros_cyber_fx_on_page_enter("");
    }
    if (g_render_mu) {
        xSemaphoreTake(g_render_mu, portMAX_DELAY);
        render_screen_locked();
        xSemaphoreGive(g_render_mu);
    } else {
        render_screen_locked();
    }
}

// --- Page mounts ------------------------------------------------- //

static void mount_lock(void)
{
    ESP_LOGI("shell", "mount_lock");
    s.page = SHELL_PAGE_LOCK;
    s.lock_pin_buf[0] = '\0';
    render_screen();
}

// Hook exposed to the agent's lock_device tool so it can drop the
// device back to the lock screen from anywhere.
void agent_request_lock(void)
{
    mount_lock();
}

// Agent observer — runs from the agent inference task. Updates the
// status line + re-renders the agent page. render_screen() takes the
// mutex, so the cross-task call is safe.
static void agent_observer(const pageros_agent_event_t *evt, void *ctx)
{
    (void)ctx;
    switch (evt->kind) {
    case PAGEROS_AGENT_EVT_THINKING:
        strncpy(s.agent_status, "THINKING", sizeof(s.agent_status) - 1);
        break;
    case PAGEROS_AGENT_EVT_TOOL:
        snprintf(s.agent_status, sizeof(s.agent_status),
                 "TOOL %.50s", evt->text ? evt->text : "?");
        break;
    case PAGEROS_AGENT_EVT_MESSAGE:
        strncpy(s.agent_status, "READY", sizeof(s.agent_status) - 1);
        break;
    case PAGEROS_AGENT_EVT_ERROR:
        snprintf(s.agent_status, sizeof(s.agent_status),
                 "ERR %.60s", evt->text ? evt->text : "");
        break;
    case PAGEROS_AGENT_EVT_IDLE:
        if (strncmp(s.agent_status, "ERR", 3) != 0) {
            strncpy(s.agent_status, "READY", sizeof(s.agent_status) - 1);
        }
        break;
    }
    s.agent_status[sizeof(s.agent_status) - 1] = '\0';
    if (s.page == SHELL_PAGE_AGENT) render_screen();
}

static void mount_agent(void)
{
    ESP_LOGI("shell", "mount_agent");
    s.page = SHELL_PAGE_AGENT;
    s.agent_input[0] = '\0';
    if (s.agent_status[0] == '\0') {
        strncpy(s.agent_status,
                pageros_agent_is_ready() ? "READY" : "OFFLINE",
                sizeof(s.agent_status) - 1);
        s.agent_status[sizeof(s.agent_status) - 1] = '\0';
    }
    pageros_agent_set_observer(agent_observer, NULL);
    render_screen();
}

// --- TCA8418 keyboard decode ------------------------------------ //
//
// Verbatim from LilyGoLib's LilyGo_LoRa_Pager.cpp keymap. Handles
// caps + symbol layer toggles, ENTER (k=20), BACKSPACE (k=30).

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
    char ch = g_symbol_on ? kb_symbol_map[row][col] : kb_keymap[row][col];
    if (g_caps_on && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    return ch;
}

// --- Input handler ---------------------------------------------- //

static bool handle_lock_input(const pageros_router_event_t *ev)
{
    if (ev->kind == PAGEROS_ROUTER_EVT_NAV) {
        if (ev->as.nav == PAGEROS_ROUTER_NAV_ENTER) {
            if (s.lock_pin_required) {
                if (strlen(s.lock_pin_buf) != 4) {
                    pageros_toast("PIN TOO SHORT",
                                  PAGEROS_TOAST_ERROR, 1500);
                    pageros_audio_play_ui_click();
                    return true;
                }
                if (strcmp(s.lock_pin_buf, s.lock_pin_stored) != 0) {
                    s.lock_attempts++;
                    s.lock_pin_buf[0] = '\0';
                    pageros_toast("WRONG PIN", PAGEROS_TOAST_ERROR, 1500);
                    pageros_audio_play_ui_click();
                    render_screen();
                    return true;
                }
            }
            s.lock_pin_buf[0] = '\0';
            pageros_audio_play_ui_click();
            mount_agent();
            return true;
        }
        if (ev->as.nav == PAGEROS_ROUTER_NAV_BACK) {
            if (s.lock_pin_required) {
                size_t L = strlen(s.lock_pin_buf);
                if (L > 0) {
                    s.lock_pin_buf[L - 1] = '\0';
                    pageros_audio_play_ui_scroll();
                }
                render_screen();
            }
            return true;
        }
    }
    if (ev->kind == PAGEROS_ROUTER_EVT_KEY && ev->as.key.pressed) {
        char ch = kb_decode(ev->as.key.row, ev->as.key.col);
        if (ch >= '0' && ch <= '9' && s.lock_pin_required) {
            size_t L = strlen(s.lock_pin_buf);
            if (L < 4) {
                s.lock_pin_buf[L]     = ch;
                s.lock_pin_buf[L + 1] = '\0';
                pageros_audio_play_ui_scroll();
                render_screen();
            }
            return true;
        }
        if (ch == '\n') {
            pageros_router_event_t fake = {
                .kind   = PAGEROS_ROUTER_EVT_NAV,
                .as.nav = PAGEROS_ROUTER_NAV_ENTER,
            };
            return handle_lock_input(&fake);
        }
    }
    return false;
}

static bool handle_agent_input(const pageros_router_event_t *ev)
{
    if (ev->kind == PAGEROS_ROUTER_EVT_NAV) {
        if (ev->as.nav == PAGEROS_ROUTER_NAV_ENTER) {
            if (s.agent_input[0] == '\0') return true;
            if (!pageros_agent_is_ready()) {
                pageros_toast("AGENT OFFLINE",
                              PAGEROS_TOAST_ERROR, 2000);
                return true;
            }
            if (pageros_agent_is_busy()) return true;
            if (pageros_agent_submit(s.agent_input) == ESP_OK) {
                s.agent_input[0] = '\0';
                s.agent_scroll_offset = 0;  // snap to tail on new submit
                strncpy(s.agent_status, "THINKING",
                        sizeof(s.agent_status) - 1);
                s.agent_status[sizeof(s.agent_status) - 1] = '\0';
                pageros_audio_play_ui_click();
            } else {
                pageros_toast("SUBMIT FAILED",
                              PAGEROS_TOAST_ERROR, 1500);
            }
            render_screen();
            return true;
        }
        if (ev->as.nav == PAGEROS_ROUTER_NAV_BACK) {
            size_t L = strlen(s.agent_input);
            if (L > 0) {
                s.agent_input[L - 1] = '\0';
                pageros_audio_play_ui_scroll();
                render_screen();
            }
            return true;
        }
        if (ev->as.nav == PAGEROS_ROUTER_NAV_BACK_LONG
            || ev->as.nav == PAGEROS_ROUTER_NAV_ENTER_LONG) {
            pageros_audio_play_ui_click();
            mount_lock();
            return true;
        }
    }
    if (ev->kind == PAGEROS_ROUTER_EVT_ENCODER) {
        // Encoder = transcript scroll. CCW = older (scroll up),
        // CW = newer (scroll down). 3 lines per tick.
        int step = (ev->as.enc == PAGEROS_ROUTER_ENC_CCW) ? +3 : -3;
        s.agent_scroll_offset += step;
        if (s.agent_scroll_offset < 0) s.agent_scroll_offset = 0;
        pageros_audio_play_ui_scroll();
        render_screen();
        return true;
    }
    if (ev->kind == PAGEROS_ROUTER_EVT_KEY && ev->as.key.pressed) {
        char ch = kb_decode(ev->as.key.row, ev->as.key.col);
        if (ch == '\n') {
            pageros_router_event_t fake = {
                .kind   = PAGEROS_ROUTER_EVT_NAV,
                .as.nav = PAGEROS_ROUTER_NAV_ENTER,
            };
            return handle_agent_input(&fake);
        }
        if (ch == '\b') {
            size_t L = strlen(s.agent_input);
            if (L > 0) {
                s.agent_input[L - 1] = '\0';
                pageros_audio_play_ui_scroll();
                render_screen();
            }
            return true;
        }
        if (ch >= 0x20 && ch < 0x7f) {
            size_t L = strlen(s.agent_input);
            if (L + 1 < sizeof(s.agent_input)) {
                s.agent_input[L]     = ch;
                s.agent_input[L + 1] = '\0';
                pageros_audio_play_ui_scroll();
                render_screen();
            }
            return true;
        }
    }
    return false;
}

static bool default_shell_handler(const pageros_router_event_t *ev, void *ctx)
{
    (void)ctx;
    pageros_power_state_t prev_pwr = pageros_power_state();
    pageros_power_kick();
    if (prev_pwr >= PAGEROS_POWER_SCREEN_OFF && s.page != SHELL_PAGE_LOCK) {
        mount_lock();
        return true;
    }
    switch (s.page) {
    case SHELL_PAGE_LOCK:  return handle_lock_input(ev);
    case SHELL_PAGE_AGENT: return handle_agent_input(ev);
    }
    return false;
}

// --- Boot --------------------------------------------------------- //

void app_main(void)
{
    s.boot_us = esp_timer_get_time();

    // NVS first — many subsystems persist config here.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_err = nvs_flash_init();
    }
    ESP_LOGI(TAG, "NVS: %s", esp_err_to_name(nvs_err));

    pageros_logger_init();

    // Palette + display first so the boot splash has somewhere to draw.
    pageros_widgets_cyberpunk_palette(&g_palette);

    esp_err_t i2c_err  = pageros_i2c_bus_init();
    esp_err_t x_err    = pageros_xl9555_init();

    // CRITICAL: assert every peripheral power gate on the XL9555 expander
    // BEFORE bringing up the shared-SPI peripherals (display, SD, LoRa,
    // NFC). Without these, the SD card's power rail is half-floating and
    // its SPI lines glitch the bus — display ends up garbled and storage
    // hangs. Legacy main did this; I missed it in the rewrite.
    if (x_err == ESP_OK) {
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
    }

    esp_err_t disp_err = pageros_display_init();

    // Defensive clear — PSRAM can survive software resets, so the
    // framebuffer might still hold pixels from the previous firmware.
    // Push a single black frame before anything else draws so we start
    // from a clean slate.
    if (disp_err == ESP_OK) {
        pageros_display_fill(0x0000);
        pageros_display_present();
    }

    boot_log_add(BL_INFO, "[..] PAGEROS BOOT");
    boot_log_add(BL_OK,   "[OK] I2C=%s XL9555=%s DISP=%s",
                 esp_err_to_name(i2c_err),
                 esp_err_to_name(x_err),
                 esp_err_to_name(disp_err));
    pageros_fonts_init(NULL);
    boot_splash_render();

    // Identity
    pageros_identity_init();
    char fp[PAGEROS_IDENTITY_FP_LEN] = {0};
    if (pageros_identity_fingerprint(fp) == ESP_OK) {
        boot_log_add(BL_OK, "[OK] IDENTITY %s", fp);
    }
    boot_splash_render();

    // SD mount is DEFERRED — legacy did the same. Bringing up the SD
    // card eagerly at boot, while the display + LoRa are still
    // initialising on the shared SPI bus, garbles the display. Agent
    // config load + NFC tag persistence mount lazily when needed.
    boot_log_add(BL_INFO, "[..] SD MOUNT DEFERRED");
    boot_splash_render();

    // LoRa
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
    boot_log_add(lora_err == ESP_OK ? BL_OK : BL_WARN,
                 "[%s] LORA: %s",
                 lora_err == ESP_OK ? "OK" : "!!",
                 esp_err_to_name(lora_err));
    boot_splash_render();

    // Wi-Fi + auto-connect
    esp_err_t wifi_err = pageros_wifi_init();
    if (wifi_err == ESP_OK) {
        boot_log_add(BL_OK, "[OK] WI-FI INIT");
        char saved_ssid[33] = {0}, saved_psk[65] = {0};
        esp_err_t cl = pageros_wifi_creds_load(saved_ssid, sizeof(saved_ssid),
                                               saved_psk, sizeof(saved_psk));
        if (cl != ESP_OK || saved_ssid[0] == '\0') {
            strncpy(saved_ssid, "internet",    sizeof(saved_ssid) - 1);
            strncpy(saved_psk,  "Albert@0210", sizeof(saved_psk)  - 1);
            pageros_wifi_creds_save(saved_ssid, saved_psk);
        }
        boot_log_add(BL_INFO, "[..] WI-FI CONNECT %s", saved_ssid);
        boot_splash_render();
        esp_err_t wc = pageros_wifi_connect(saved_ssid, saved_psk, 15000);
        boot_log_add(wc == ESP_OK ? BL_OK : BL_WARN,
                     "[%s] WI-FI: %s",
                     wc == ESP_OK ? "OK" : "!!",
                     esp_err_to_name(wc));
    } else {
        boot_log_add(BL_WARN, "[!!] WI-FI INIT: %s", esp_err_to_name(wifi_err));
    }
    boot_splash_render();

    // Audio
    esp_err_t audio_err = pageros_audio_init();
    boot_log_add(audio_err == ESP_OK ? BL_OK : BL_WARN,
                 "[%s] AUDIO", audio_err == ESP_OK ? "OK" : "!!");
    boot_splash_render();

    // Lock screen PIN
    lock_load_pin();

    // Cyber FX engine + render mutex
    g_render_mu = xSemaphoreCreateMutex();
    pageros_cyber_fx_init();

    if (disp_err == ESP_OK) {
        (void)pageros_display_panel_reinit();
        boot_log_add(BL_OK, "[OK] DISPLAY READY");
        boot_splash_render();
        vTaskDelay(pdMS_TO_TICKS(400));
        mount_lock();
    }

    // GPS
    pageros_gps_init();
    // Input + keyboard + IMU + NFC + power
    esp_err_t input_err = pageros_input_init();
    esp_err_t kbd_err   = pageros_kbd_init();
    pageros_imu_init();
    pageros_nfc_init();
    pageros_nfc_start(NULL, NULL);
    pageros_power_init(NULL);

    if (input_err == ESP_OK || kbd_err == ESP_OK) {
        if (pageros_router_init() == ESP_OK) {
            pageros_router_set_shell_handler(default_shell_handler, NULL);
        }
    }

    // Cyber FX background task drives lock-screen matrix rain.
    pageros_cyber_fx_start(render_screen);

    // Agent harness — depends on Wi-Fi + storage being up.
    esp_err_t agent_err = pageros_agent_init();
    if (agent_err == ESP_OK) {
        ESP_LOGI(TAG, "agent ready");
    } else {
        ESP_LOGW(TAG, "agent init: %s", esp_err_to_name(agent_err));
    }

    while (1) vTaskDelay(pdMS_TO_TICKS(60000));
}
