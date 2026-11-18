/*
 * DualShock 4 Light Bar Manager - native PS4 homebrew (OpenOrbis toolchain)
 *
 * Light bar control goes through libScePad (scePadSetLightBar /
 * scePadResetLightBar). libScePad owns the USB / Bluetooth transport, so the
 * same call works for both connection types. Raw HID output reports are NOT
 * reachable from a userland app, so they are not used here.
 *
 * Brightness: the DS4 light bar has no brightness register in this API; it
 * only takes an RGB triple. Brightness here = RGB scaled by a percentage.
 * OFF = RGB (0,0,0), re-sent periodically so it is not left on a stale value.
 *
 * Build with -DHOST_TEST (see tests/) to run the logic against stub APIs.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#ifdef HOST_TEST
#include "host_stubs.h"
#else
#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/SystemService.h>
#endif

#define W 1920
#define H 1080
#define MAXC 4
#define HEARTBEAT_FRAMES 15 /* re-send colour ~4x/sec so system overrides are undone */

/* ------------------------------------------------------------------ video */
static uint32_t *g_fb; /* current back buffer, 0xAARRGGBB */

#ifndef HOST_TEST
/* numeric values of the PS4 video-out API (names differ between SDK headers) */
#define VCCS_VIDEO_BUS_MAIN 0            /* bus type: main */
#define VCCS_VIDEO_PIXEL_BGRA8_SRGB 0x80000000u /* A8R8G8B8 sRGB: 0xAARRGGBB in memory */
static int g_video = -1;
static void *g_buf[2];
static int g_cur = 0;
static OrbisVideoOutBufferAttribute g_attr;

static int plat_video_init(void)
{
    g_video = sceVideoOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, VCCS_VIDEO_BUS_MAIN, 0, NULL);
    if (g_video < 0) return g_video;

    size_t one = ((size_t)W * H * 4 + 0x1FFFFF) & ~(size_t)0x1FFFFF;
    size_t total = one * 2;
    off_t phys = 0;
    int r = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total, 0x200000, 3 /* WB_ONION */, &phys);
    if (r < 0) return r;
    void *addr = NULL;
    /* prot 0x33 = CPU r/w + GPU r/w */
    r = sceKernelMapDirectMemory(&addr, total, 0x33, 0, phys, 0x200000);
    if (r < 0) return r;
    g_buf[0] = addr;
    g_buf[1] = (uint8_t *)addr + one;

    sceVideoOutSetBufferAttribute(&g_attr, VCCS_VIDEO_PIXEL_BGRA8_SRGB,
                                  ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9, W, H, W);
    r = sceVideoOutRegisterBuffers(g_video, 0, g_buf, 2, &g_attr);
    if (r < 0) return r;
    sceVideoOutSetFlipRate(g_video, 0);
    g_fb = (uint32_t *)g_buf[g_cur];
    return 0;
}

static void plat_present(void)
{
    sceVideoOutSubmitFlip(g_video, g_cur, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0);
    while (sceVideoOutIsFlipPending(g_video) > 0) sceKernelUsleep(1000);
    g_cur ^= 1;
    g_fb = (uint32_t *)g_buf[g_cur];
}
#else
static uint32_t g_host_fb[W * H];
static int plat_video_init(void) { g_fb = g_host_fb; return 0; }
static void plat_present(void) {}
#endif

/* --------------------------------------------------------------- drawing */
#define C_BG 0xFF0E141B
#define C_PANEL 0xFF17212C
#define C_SEL 0xFF22344B
#define C_ACCENT 0xFF2D7DFF
#define C_TEXT 0xFFE8EEF5
#define C_DIM 0xFF7F8EA1
#define C_OK 0xFF39D26B
#define C_BAD 0xFFE0475B

static void rect(int x, int y, int w, int h, uint32_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++) {
        uint32_t *p = g_fb + (size_t)(y + j) * W + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

static void frame(int x, int y, int w, int h, int t, uint32_t c)
{
    rect(x, y, w, t, c);
    rect(x, y + h - t, w, t, c);
    rect(x, y, t, h, c);
    rect(x + w - t, y, t, h, c);
}

static uint32_t mix(uint32_t a, uint32_t b, int t) /* t = weight of a, 0..255 */
{
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    return 0xFF000000u | (uint32_t)(((ar * t + br * (255 - t)) / 255) << 16) |
           (uint32_t)(((ag * t + bg * (255 - t)) / 255) << 8) | (uint32_t)((ab * t + bb * (255 - t)) / 255);
}

/* 8x8 font, bit0 = leftmost pixel (public-domain font8x8 layout) */
static const char GL[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ%:-./<>()+";
static const uint8_t FD[][8] = {
    {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00}, {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00},
    {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00}, {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00},
    {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00}, {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00},
    {0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00}, {0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00},
    {0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00}, {0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00},
    {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00}, {0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00},
    {0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00}, {0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00},
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00}, {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00},
    {0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00}, {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00},
    {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, {0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00},
    {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00}, {0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00},
    {0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00}, {0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00},
    {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00}, {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00},
    {0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00}, {0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00},
    {0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00}, {0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00}, {0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00}, {0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00},
    {0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00}, {0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00},
    {0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00}, /* % */
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00}, /* : */
    {0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00}, /* - */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00}, /* . */
    {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00}, /* / */
    {0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00}, /* < */
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00}, /* > */
    {0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00}, /* ( */
    {0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00}, /* ) */
    {0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00}, /* + */
};

static int text_w(const char *s, int sc) { return (int)strlen(s) * 8 * sc; }

static void text(int x, int y, const char *s, int sc, uint32_t c)
{
    for (; *s; s++, x += 8 * sc) {
        char ch = *s;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        if (ch == ' ') continue;
        const char *p = strchr(GL, ch);
        if (!p) continue;
        const uint8_t *g = FD[p - GL];
        for (int r = 0; r < 8; r++)
            for (int b = 0; b < 8; b++)
                if ((g[r] >> b) & 1) rect(x + b * sc, y + r * sc, sc, sc, c);
    }
}

/* ------------------------------------------------------------- app state */
typedef struct {
    int userId;
    int handle;
    bool connected;
    int last_err;
} Ctl;

static Ctl g_ctl[MAXC];
static int g_nctl = 0;
static int g_target = 0; /* 0..g_nctl-1 = one controller, g_nctl = ALL */

static int g_col[3] = {0, 96, 255};
static int g_bright = 100; /* 0..100 */
static bool g_off = false;
static bool g_sysdef = false; /* user asked for system default colour */
static int g_preset = -1;
static bool g_keep = true; /* ask the VCCS background plugin to keep these settings after the app is left */
static int g_row = 0;
static bool g_dirty = true;
static int g_hb = 0;
static int g_frame = 0;
static char g_status[96] = "";

enum { ROW_R, ROW_G, ROW_B, ROW_BRIGHT, ROW_BSTEP, ROW_PRESET, ROW_TARGET, ROW_OFF, ROW_RESET, ROW_KEEP, ROWS };
static const char *ROWN[ROWS] = {"RED", "GREEN", "BLUE", "BRIGHTNESS", "BRIGHT STEP", "PRESET", "TARGET", "LIGHT", "SYSTEM COLOR", "KEEP AFTER EXIT"};

static const int LEVELS[4] = {100, 50, 10, 0};
static const char *LEVELN[4] = {"MAX", "MED", "MIN", "ZERO"};

static const struct { const char *n; uint8_t r, g, b; } PRE[] = {
    {"RED", 255, 0, 0},   {"ORANGE", 255, 90, 0}, {"YELLOW", 255, 190, 0}, {"GREEN", 0, 255, 0},
    {"CYAN", 0, 255, 255}, {"BLUE", 0, 0, 255},    {"PURPLE", 140, 0, 255}, {"PINK", 255, 0, 110},
    {"WHITE", 255, 255, 255}, {"PS BLUE", 0, 40, 255},
};
#define NPRE ((int)(sizeof(PRE) / sizeof(PRE[0])))

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void effective(uint8_t o[3])
{
    for (int i = 0; i < 3; i++) o[i] = g_off ? 0 : (uint8_t)(g_col[i] * g_bright / 100);
}

static bool targeted(int i) { return g_target == g_nctl || g_target == i; }

/* ---------------------------------------------------------- persistence
 * Settings are written to CFG_FILE a short time after the last change and are
 * loaded + applied on launch. The file is a plain key=value list that the
 * resident plugin (plugin/vccs_plugin.c) reads to keep the colour after the app
 * is left. While the app is in the foreground it also touches ALIVE_FILE about
 * twice a second so the plugin knows to stand down.
 * Controller selection (g_target) is intentionally not saved. */
#ifndef CFG_DIR
#define CFG_DIR "/data/vccs"
#endif
#define CFG_FILE CFG_DIR "/config.ini"
#define CFG_TMP CFG_DIR "/config.ini.tmp"
#define ALIVE_FILE CFG_DIR "/alive"
#define CFG_SETTLE_FRAMES 30

#ifdef HOST_TEST
#include <sys/stat.h>
static void plat_mkdir(const char *p) { mkdir(p, 0777); }
#else
static void plat_mkdir(const char *p) { sceKernelMkdir(p, 0777); }
#endif

typedef struct { int c[3], bright, off, sys, preset, keep; } Snap;
static Snap g_seen, g_saved;
static int g_stable = 0;
static char g_cfg_msg[32] = "";

static Snap snap(void)
{
    Snap s;
    memset(&s, 0, sizeof s);
    for (int i = 0; i < 3; i++) s.c[i] = g_col[i];
    s.bright = g_bright; s.off = g_off; s.sys = g_sysdef; s.preset = g_preset; s.keep = g_keep;
    return s;
}

static int clampi(int v, int lo, int hi);

static bool cfg_save(void)
{
    plat_mkdir(CFG_DIR);
    FILE *f = fopen(CFG_TMP, "w");
    if (!f) return false;
    fprintf(f, "# VCCS settings\nr=%d\ng=%d\nb=%d\nbrightness=%d\noff=%d\nsystem_default=%d\npreset=%d\nkeep=%d\n",
            g_col[0], g_col[1], g_col[2], g_bright, g_off, g_sysdef, g_preset, g_keep);
    if (fclose(f) != 0) return false;
    return rename(CFG_TMP, CFG_FILE) == 0;
}

static bool cfg_load(void)
{
    FILE *f = fopen(CFG_FILE, "r");
    if (!f) return false;
    char line[96];
    int v, got = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "r=%d", &v) == 1) { g_col[0] = clampi(v, 0, 255); got++; }
        else if (sscanf(line, "g=%d", &v) == 1) { g_col[1] = clampi(v, 0, 255); got++; }
        else if (sscanf(line, "b=%d", &v) == 1) { g_col[2] = clampi(v, 0, 255); got++; }
        else if (sscanf(line, "brightness=%d", &v) == 1) { g_bright = clampi(v, 0, 100); got++; }
        else if (sscanf(line, "off=%d", &v) == 1) { g_off = v != 0; got++; }
        else if (sscanf(line, "system_default=%d", &v) == 1) { g_sysdef = v != 0; got++; }
        else if (sscanf(line, "preset=%d", &v) == 1) { g_preset = (v >= 0 && v < 10) ? v : -1; got++; }
        else if (sscanf(line, "keep=%d", &v) == 1) { g_keep = v != 0; got++; }
    }
    fclose(f);
    if (got == 0) return false;
    g_dirty = true;
    return true;
}

/* call once per frame: save once the settings have stopped changing */
static void cfg_tick(void)
{
    Snap s = snap();
    if (memcmp(&s, &g_seen, sizeof s) != 0) { g_seen = s; g_stable = 0; return; }
    if (memcmp(&s, &g_saved, sizeof s) == 0) return;
    if (++g_stable < CFG_SETTLE_FRAMES) return;
    if (cfg_save()) { g_saved = s; snprintf(g_cfg_msg, sizeof g_cfg_msg, "SETTINGS SAVED"); }
    else { g_stable = 0; snprintf(g_cfg_msg, sizeof g_cfg_msg, "SAVE FAILED"); }
}

/* foreground heartbeat: plugin stands down while this file is fresh */
static void alive_tick(void)
{
    if (g_frame % 30) return;
    plat_mkdir(CFG_DIR);
    FILE *f = fopen(ALIVE_FILE, "w");
    if (f) { fprintf(f, "%d\n", g_frame); fclose(f); }
}

/* ------------------------------------------------------ controller layer */
static void add_user(int uid)
{
    for (int i = 0; i < g_nctl; i++)
        if (g_ctl[i].userId == uid) return;
    if (g_nctl >= MAXC) return;
    int h = scePadOpen(uid, 0, 0, NULL);
    if (h < 0) return; /* controller not assigned to that user yet; retry on next refresh */
    g_ctl[g_nctl].userId = uid;
    g_ctl[g_nctl].handle = h;
    g_ctl[g_nctl].connected = false;
    g_ctl[g_nctl].last_err = 0;
    g_nctl++;
}

static void refresh_users(void)
{
    OrbisUserServiceLoginUserIdList l;
    memset(&l, 0xFF, sizeof(l));
    if (sceUserServiceGetLoginUserIdList(&l) == 0) {
        for (int i = 0; i < MAXC; i++)
            if ((int)l.userId[i] != -1) add_user((int)l.userId[i]);
    }
    if (g_nctl == 0) {
        OrbisUserServiceUserId init = -1;
        if (sceUserServiceGetInitialUser(&init) == 0 && init != -1) add_user((int)init);
    }
}

static uint32_t read_pads(void)
{
    uint32_t b = 0;
    for (int i = 0; i < g_nctl; i++) {
        OrbisPadData d;
        memset(&d, 0, sizeof(d));
        if (scePadReadState(g_ctl[i].handle, &d) == 0) {
            g_ctl[i].connected = d.connected;
            if (d.connected) b |= d.buttons;
        } else {
            g_ctl[i].connected = false;
        }
    }
    return b;
}

static void push_light(void)
{
    uint8_t e[3];
    effective(e);
    int sent = 0, errs = 0;
    for (int i = 0; i < g_nctl; i++) {
        if (!targeted(i) || !g_ctl[i].connected) continue;
        int r;
        if (g_sysdef) {
            r = scePadResetLightBar(g_ctl[i].handle);
        } else {
            OrbisPadColor c;
            memset(&c, 0, sizeof c); c.r = e[0]; c.g = e[1]; c.b = e[2];
            r = scePadSetLightBar(g_ctl[i].handle, &c);
        }
        g_ctl[i].last_err = r < 0 ? r : 0;
        if (r < 0) errs++; else sent++;
    }
    if (g_nctl == 0) snprintf(g_status, sizeof g_status, "NO USER / CONTROLLER FOUND");
    else if (sent == 0 && errs == 0) snprintf(g_status, sizeof g_status, "NO CONNECTED CONTROLLER IN TARGET");
    else if (errs) snprintf(g_status, sizeof g_status, "LIGHT BAR CALL FAILED ON %d CONTROLLER(S)", errs);
    else if (g_sysdef) snprintf(g_status, sizeof g_status, "SYSTEM DEFAULT COLOR RESTORED");
    else if (g_off || (e[0] | e[1] | e[2]) == 0) snprintf(g_status, sizeof g_status, "LIGHT OFF (0 0 0) - RESENT EVERY %d FRAMES", HEARTBEAT_FRAMES);
    else snprintf(g_status, sizeof g_status, "OUTPUT %d %d %d", e[0], e[1], e[2]);
}

static void release_untargeted(void)
{
    for (int i = 0; i < g_nctl; i++)
        if (!targeted(i) && g_ctl[i].connected) scePadResetLightBar(g_ctl[i].handle);
}

/* ----------------------------------------------------------------- input */
static void touch(void) { g_dirty = true; g_sysdef = false; g_off = false; }

static void adjust(int dir, bool fast)
{
    switch (g_row) {
    case ROW_R: case ROW_G: case ROW_B:
        g_col[g_row] = clampi(g_col[g_row] + dir * (fast ? 8 : 1), 0, 255);
        g_preset = -1; touch(); break;
    case ROW_BRIGHT:
        g_bright = clampi(g_bright + dir * (fast ? 5 : 1), 0, 100);
        touch(); break;
    case ROW_BSTEP: {
        int idx = 0, best = 1000;
        for (int i = 0; i < 4; i++) {
            int d = g_bright > LEVELS[i] ? g_bright - LEVELS[i] : LEVELS[i] - g_bright;
            if (d < best) { best = d; idx = i; }
        }
        idx = clampi(idx + dir, 0, 3);
        g_bright = LEVELS[idx];
        touch(); break;
    }
    case ROW_PRESET:
        g_preset = g_preset < 0 ? (dir > 0 ? 0 : NPRE - 1) : (g_preset + dir + NPRE) % NPRE;
        g_col[0] = PRE[g_preset].r; g_col[1] = PRE[g_preset].g; g_col[2] = PRE[g_preset].b;
        touch(); break;
    case ROW_TARGET:
        if (g_nctl > 0) {
            g_target = (g_target + dir + g_nctl + 1) % (g_nctl + 1);
            release_untargeted();
            g_dirty = true;
        }
        break;
    case ROW_OFF:
        g_off = !g_off; g_sysdef = false; g_dirty = true; break;
    case ROW_KEEP:
        g_keep = !g_keep; break;
    default: break;
    }
}

static void toggle_off(void) { g_off = !g_off; g_sysdef = false; g_dirty = true; }
static void sys_default(void) { g_sysdef = true; g_dirty = true; }

static uint32_t g_held = 0;
static int g_rep = 0;

static void handle_input(uint32_t b)
{
    uint32_t pressed = b & ~g_held;
    g_held = b;

    if (pressed & ORBIS_PAD_BUTTON_UP) g_row = (g_row + ROWS - 1) % ROWS;
    if (pressed & ORBIS_PAD_BUTTON_DOWN) g_row = (g_row + 1) % ROWS;

    uint32_t lr = b & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT);
    if (lr == ORBIS_PAD_BUTTON_LEFT || lr == ORBIS_PAD_BUTTON_RIGHT) {
        int dir = lr == ORBIS_PAD_BUTTON_RIGHT ? 1 : -1;
        if (pressed & lr) { g_rep = 0; adjust(dir, (b & ORBIS_PAD_BUTTON_R1) != 0); }
        else {
            g_rep++;
            if (g_rep > 20 && g_rep % 2 == 0) adjust(dir, g_rep > 80 || (b & ORBIS_PAD_BUTTON_R1));
        }
    } else {
        g_rep = 0;
    }

    if (pressed & ORBIS_PAD_BUTTON_CROSS) {
        if (g_row == ROW_OFF) toggle_off();
        else if (g_row == ROW_RESET) sys_default();
        else if (g_row == ROW_KEEP) g_keep = !g_keep;
        else if (g_row == ROW_PRESET && g_preset >= 0) touch();
    }
    if (pressed & ORBIS_PAD_BUTTON_SQUARE) toggle_off();
    if (pressed & ORBIS_PAD_BUTTON_TRIANGLE) sys_default();
}

/* -------------------------------------------------------------------- UI */
static void slider(int x, int y, int w, int val, int maxv, uint32_t col, bool sel)
{
    rect(x, y + 14, w, 24, 0xFF0A0F14);
    int fw = val * w / maxv;
    rect(x, y + 14, fw, 24, col);
    int kx = x + fw - 6;
    rect(kx < x ? x : kx, y + 6, 12, 40, sel ? C_TEXT : C_DIM);
}

static void draw_ui(void)
{
    char buf[96];
    uint8_t e[3];
    effective(e);

    rect(0, 0, W, H, C_BG);
    rect(0, 0, W, 110, C_PANEL);
    rect(0, 108, W, 4, C_ACCENT);
    text(80, 38, "VCCS - VIBE CODED CONTROLLER SETTINGS", 4, C_TEXT);

    for (int i = 0; i < ROWS; i++) {
        int y = 150 + i * 78;
        bool sel = i == g_row;
        if (sel) { rect(60, y - 8, 1160, 68, C_SEL); rect(60, y - 8, 8, 68, C_ACCENT); }
        text(90, y + 12, ROWN[i], 3, sel ? C_TEXT : C_DIM);
        int sx = 520, sw = 560;
        switch (i) {
        case ROW_R: case ROW_G: case ROW_B: {
            uint32_t cc = i == ROW_R ? 0xFFFF3B3B : (i == ROW_G ? 0xFF3BE05A : 0xFF3B7BFF);
            slider(sx, y, sw, g_col[i], 255, cc, sel);
            snprintf(buf, sizeof buf, "%d", g_col[i]);
            text(1110, y + 12, buf, 3, C_TEXT);
            break;
        }
        case ROW_BRIGHT:
            slider(sx, y, sw, g_bright, 100, 0xFFE8EEF5, sel);
            snprintf(buf, sizeof buf, "%d%%", g_bright);
            text(1110, y + 12, buf, 3, C_TEXT);
            break;
        case ROW_BSTEP:
            for (int k = 0; k < 4; k++) {
                int bx = sx + k * 140;
                bool on = g_bright == LEVELS[k];
                rect(bx, y + 4, 128, 46, on ? C_ACCENT : 0xFF0A0F14);
                text(bx + (128 - text_w(LEVELN[k], 3)) / 2, y + 16, LEVELN[k], 3, on ? C_TEXT : C_DIM);
            }
            break;
        case ROW_PRESET:
            if (g_preset >= 0) {
                rect(sx, y + 4, 46, 46, 0xFF000000 | (PRE[g_preset].r << 16) | (PRE[g_preset].g << 8) | PRE[g_preset].b);
                snprintf(buf, sizeof buf, "< %s >", PRE[g_preset].n);
            } else snprintf(buf, sizeof buf, "< CUSTOM >");
            text(sx + 64, y + 14, buf, 3, C_TEXT);
            break;
        case ROW_TARGET:
            if (g_nctl == 0) snprintf(buf, sizeof buf, "NONE");
            else if (g_target == g_nctl) snprintf(buf, sizeof buf, "< ALL CONTROLLERS >");
            else snprintf(buf, sizeof buf, "< CONTROLLER %d >", g_target + 1);
            text(sx, y + 14, buf, 3, C_TEXT);
            break;
        case ROW_OFF:
            rect(sx, y + 4, 150, 46, g_off ? C_BAD : C_OK);
            text(sx + 20, y + 16, g_off ? "OFF" : "ON", 3, C_BG);
            break;
        case ROW_RESET:
            text(sx, y + 14, g_sysdef ? "ACTIVE" : "TRIANGLE TO RESTORE", 3, g_sysdef ? C_OK : C_DIM);
            break;
        case ROW_KEEP:
            rect(sx, y + 4, 150, 46, g_keep ? C_OK : C_BAD);
            text(sx + 20, y + 16, g_keep ? "ON" : "OFF", 3, C_BG);
            text(sx + 180, y + 20, "NEEDS VCCS PLUGIN", 2, C_DIM);
            break;
        }
    }

    /* right panel */
    rect(1260, 150, 600, 790, C_PANEL);
    text(1290, 175, "PREVIEW", 3, C_DIM);
    uint32_t pc = g_sysdef ? 0xFF2A3644 : (0xFF000000u | (uint32_t)(e[0] << 16) | (uint32_t)(e[1] << 8) | e[2]);
    if (!g_sysdef)
        for (int k = 6; k >= 1; k--) frame(1320 - k * 4, 225 - k * 4, 480 + k * 8, 250 + k * 8, 4, mix(pc, C_PANEL, 18 * (7 - k)));
    rect(1320, 225, 480, 250, pc);
    if (g_sysdef) text(1320 + (480 - text_w("SYSTEM DEFAULT", 4)) / 2, 330, "SYSTEM DEFAULT", 4, C_TEXT);
    else if ((e[0] | e[1] | e[2]) == 0) text(1320 + (480 - text_w("LIGHT OFF", 4)) / 2, 330, "LIGHT OFF", 4, C_DIM);
    frame(1320, 225, 480, 250, 3, C_TEXT);

    rect(1320, 520, 70, 40, 0xFF000000u | (uint32_t)(g_col[0] << 16) | (uint32_t)(g_col[1] << 8) | (uint32_t)g_col[2]);
    snprintf(buf, sizeof buf, "COLOR %d %d %d", g_col[0], g_col[1], g_col[2]);
    text(1410, 528, buf, 2, C_TEXT);

    text(1290, 600, "CONTROLLERS", 3, C_DIM);
    if (g_nctl == 0) text(1290, 650, "NONE FOUND", 3, C_BAD);
    for (int i = 0; i < g_nctl; i++) {
        int y = 650 + i * 56;
        rect(1290, y + 2, 26, 26, g_ctl[i].connected ? C_OK : C_BAD);
        snprintf(buf, sizeof buf, "%s%d %s", targeted(i) ? ">" : " ", i + 1, g_ctl[i].connected ? "CONNECTED" : "NOT CONNECTED");
        text(1330, y, buf, 3, C_TEXT);
        if (g_ctl[i].last_err) {
            snprintf(buf, sizeof buf, "ERR %08X", (unsigned)g_ctl[i].last_err);
            text(1330, y + 28, buf, 2, C_BAD);
        }
    }

    rect(0, 960, W, 120, C_PANEL);
    text(80, 975, g_status, 2, C_TEXT);
    if (g_cfg_msg[0]) text(W - 80 - text_w(g_cfg_msg, 2), 975, g_cfg_msg, 2, strstr(g_cfg_msg, "FAIL") ? C_BAD : C_OK);
    text(80, 1030, "UP/DOWN SELECT  LEFT/RIGHT ADJUST (HOLD R1 FAST)  SQUARE ON/OFF  TRIANGLE SYSTEM COLOR", 2, C_DIM);
}

/* ------------------------------------------------------------------ main */
static void app_init(void)
{
#ifndef HOST_TEST
    sceSystemServiceHideSplashScreen();
    sceUserServiceInitialize(NULL);
#endif
    scePadInit();
    plat_video_init();
    if (cfg_load()) snprintf(g_cfg_msg, sizeof g_cfg_msg, "SETTINGS LOADED");
    g_seen = g_saved = snap();
    refresh_users();
}

static void app_frame(void)
{
    if ((g_frame % 120) == 0) refresh_users();
    uint32_t b = read_pads();
    handle_input(b);
    if (g_dirty || ++g_hb >= HEARTBEAT_FRAMES) {
        push_light();
        g_dirty = false;
        g_hb = 0;
    }
    cfg_tick();
    alive_tick();
    draw_ui();
    plat_present();
    g_frame++;
}

#ifndef HOST_TEST
int main(void)
{
    app_init();
    for (;;) app_frame();
    return 0;
}
#endif
