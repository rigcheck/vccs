/*
 * VCCS background plugin - keeps the DualShock 4 light bar the way you set it in
 * the VCCS app, after you leave the app and inside games / the system UI.
 *
 * Built on the GoldHEN Plugin SDK (third_party/GoldHEN_Plugins_SDK, MIT).
 *
 * How it works
 *   1. Hooks (SDK Detour) on scePadSetLightBar and scePadResetLightBar. While
 *      "override" is active, whatever colour a game or the system asks for is
 *      replaced by the VCCS colour, and a "reset to system colour" request is
 *      turned into the VCCS colour.
 *   2. A worker thread (4x per second) reads /data/vccs/config.ini, decides if
 *      the override is active, caches the colour in one atomic word for the
 *      hooks, and also pushes the colour itself (covers processes that set the
 *      light bar before the plugin loaded or never set it again).
 *
 * Override is active only when ALL are true: config readable, keep=1,
 * system_default=0, and the VCCS app is NOT in the foreground (the app writes
 * an increasing counter to /data/vccs/alive twice a second; if the counter has
 * not changed for 3 s the app is considered gone).
 *
 * Safety
 *   - Hooks are only installed if the target's first instructions are safe to
 *     copy (no ret / relative branch / RIP-relative operand in the 14 bytes the
 *     SDK overwrites), checked with the SDK's disassembler. Otherwise the plugin
 *     falls back to polling only.
 *   - Create /data/vccs/no_hooks (any content) to disable hooks entirely.
 *   - Never calls scePadOpen; only uses pad handles the host process already
 *     opened (scePadGetHandle), so it cannot take a controller from a game.
 *   - /data/vccs/plugin.log records, per process (title id), what happened.
 *
 * NOT verified on hardware. Build with -DHOST_TEST (tests/plugin_test.c) to run
 * the logic on a PC against stubs and a fake Detour.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#ifdef HOST_TEST
#include "host_stubs.h"
#include "Detour.h"
#include "GoldHEN.h"
#include "Utilities.h"
#include "HDE64.h"
static uint64_t host_ms = 0;
static uint64_t plat_ms(void) { return host_ms; }
#else
#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include "Common.h"
#include "HDE64.h"
static uint64_t plat_ms(void) { return sceKernelGetProcessTime() / 1000; }
#endif

#ifndef CFG_DIR
#define CFG_DIR "/data/vccs"
#endif
#define CFG_FILE CFG_DIR "/config.ini"
#define ALIVE_FILE CFG_DIR "/alive"
#define LOG_FILE CFG_DIR "/plugin.log"
#define NO_HOOKS_FILE CFG_DIR "/no_hooks"
#define ALIVE_WINDOW_MS 3000
#define TICK_US 250000
#define MAXU 4
#define LOG_MAX_BYTES 16384
#define HOOK_MIN_BYTES 14 /* sizeof(Detour.JumpInstructions64) */

/* ------------------------------------------------------------------ log */
static char g_tag[96];
static bool g_tag_ready = false;

static void make_tag(void)
{
    if (g_tag_ready) return;
    struct proc_info pi;
    memset(&pi, 0, sizeof pi);
    if (sys_sdk_proc_info(&pi) == 0) {
        pi.titleid[sizeof pi.titleid - 1] = 0;
        pi.name[sizeof pi.name - 1] = 0;
        snprintf(g_tag, sizeof g_tag, "%s %s pid=%d", pi.titleid[0] ? pi.titleid : "-", pi.name[0] ? pi.name : "-", pi.pid);
    } else {
        snprintf(g_tag, sizeof g_tag, "pid=?");
    }
    g_tag_ready = true;
}

static void plog(const char *msg)
{
    make_tag();
    FILE *f = fopen(LOG_FILE, "a");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    if (ftell(f) > LOG_MAX_BYTES) { /* keep the file small */
        fclose(f);
        f = fopen(LOG_FILE, "w");
        if (!f) return;
    }
    fprintf(f, "[%lu ms %s] %s\n", (unsigned long)plat_ms(), g_tag, msg);
    fclose(f);
}

static char g_last_msg[96] = "";
static void note_status(const char *why, int users, int handles, int ok, int err)
{
    char m[96];
    snprintf(m, sizeof m, "%s users=%d handles=%d applied=%d err=0x%X", why, users, handles, ok, (unsigned)err);
    if (strcmp(m, g_last_msg) == 0) return; /* log changes only */
    snprintf(g_last_msg, sizeof g_last_msg, "%s", m);
    plog(m);
}

/* --------------------------------------------------------------- config */
typedef struct { int r, g, b, bright, off, sysdef, keep; } PCfg;

static int pclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* false if the file is missing or holds no recognised keys */
static bool pcfg_load(PCfg *c)
{
    FILE *f = fopen(CFG_FILE, "r");
    if (!f) return false;
    c->r = 0; c->g = 0; c->b = 0; c->bright = 100; c->off = 0; c->sysdef = 0; c->keep = 1;
    char line[96];
    int v, got = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "r=%d", &v) == 1) { c->r = pclamp(v, 0, 255); got++; }
        else if (sscanf(line, "g=%d", &v) == 1) { c->g = pclamp(v, 0, 255); got++; }
        else if (sscanf(line, "b=%d", &v) == 1) { c->b = pclamp(v, 0, 255); got++; }
        else if (sscanf(line, "brightness=%d", &v) == 1) { c->bright = pclamp(v, 0, 100); got++; }
        else if (sscanf(line, "off=%d", &v) == 1) { c->off = v != 0; got++; }
        else if (sscanf(line, "system_default=%d", &v) == 1) { c->sysdef = v != 0; got++; }
        else if (sscanf(line, "keep=%d", &v) == 1) { c->keep = v != 0; got++; }
    }
    fclose(f);
    return got > 0;
}

/* The app writes an increasing counter to ALIVE_FILE. "Foreground" = the counter
 * changed within the last ALIVE_WINDOW_MS (measured with this process's clock).
 * The first value ever seen is treated as stale (it may be left over from a
 * previous run). Missing file = app not running. */
static long g_alive_val = -1;
static bool g_alive_init = false;
static uint64_t g_alive_changed_ms = 0;

static bool app_in_foreground(void)
{
    FILE *f = fopen(ALIVE_FILE, "r");
    if (!f) return false;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1) v = -1;
    fclose(f);
    uint64_t now = plat_ms();
    if (!g_alive_init) {
        g_alive_init = true;
        g_alive_val = v;
        g_alive_changed_ms = now - ALIVE_WINDOW_MS - 1;
        return false;
    }
    if (v != g_alive_val) { g_alive_val = v; g_alive_changed_ms = now; }
    return (now - g_alive_changed_ms) <= ALIVE_WINDOW_MS;
}

/* ---------------------------------------------- override state (for hooks)
 * One 32-bit word so the hooks (running on game/system threads) never take a lock:
 * bit31 = override active, bits 23..0 = 0xRRGGBB. */
#define ST_ACTIVE 0x80000000u
static uint32_t g_state = 0;
static void st_store(uint32_t s) { __atomic_store_n(&g_state, s, __ATOMIC_RELAXED); }
static uint32_t st_load(void) { return __atomic_load_n(&g_state, __ATOMIC_RELAXED); }

static bool get_override(OrbisPadColor *o)
{
    uint32_t s = st_load();
    if (!(s & ST_ACTIVE)) return false;
    memset(o, 0, sizeof *o);
    o->r = (uint8_t)((s >> 16) & 0xFF);
    o->g = (uint8_t)((s >> 8) & 0xFF);
    o->b = (uint8_t)(s & 0xFF);
    return true;
}

/* ---------------------------------------------------------------- hooks */
HOOK_INIT(scePadSetLightBar);
HOOK_INIT(scePadResetLightBar);
static bool g_hook_set_ok = false, g_hook_reset_ok = false;

typedef int (*set_fn_t)(int, const OrbisPadColor *);
typedef int (*reset_fn_t)(int);

/* call the real scePadSetLightBar (through the Detour stub if hooked) */
static int real_set(int handle, const OrbisPadColor *c)
{
    if (g_hook_set_ok) return HOOK_CONTINUE(scePadSetLightBar, set_fn_t, handle, c);
    return scePadSetLightBar(handle, c);
}

static int scePadSetLightBar_hook(int handle, const OrbisPadColor *c)
{
    OrbisPadColor ov;
    if (get_override(&ov)) return HOOK_CONTINUE(scePadSetLightBar, set_fn_t, handle, &ov);
    return HOOK_CONTINUE(scePadSetLightBar, set_fn_t, handle, c);
}

static int scePadResetLightBar_hook(int handle)
{
    OrbisPadColor ov;
    if (get_override(&ov)) return real_set(handle, &ov); /* "back to system colour" -> VCCS colour */
    return HOOK_CONTINUE(scePadResetLightBar, reset_fn_t, handle);
}

/* The SDK Detour overwrites the first >=14 bytes of the target with a jump and copies
 * them to a stub. That is only safe if those bytes are position-independent and the
 * function is at least that long. Reject anything that is not clearly a plain prologue. */
static bool prologue_hookable(const void *fn)
{
    if (!fn) return false;
    const uint8_t *p = (const uint8_t *)fn;
    size_t total = 0;
    for (int guard = 0; total < HOOK_MIN_BYTES && guard < 16; guard++) {
        hde64s hs;
        unsigned len = hde64_disasm(p + total, &hs);
        if (len == 0 || (hs.flags & F_ERROR)) return false;
        if (hs.flags & F_RELATIVE) return false; /* call/jmp/jcc rel */
        if ((hs.flags & F_MODRM) && hs.modrm_mod == 0 && hs.modrm_rm == 5) return false; /* RIP-relative */
        if (hs.opcode != 0x0F) {
            switch (hs.opcode) {
            case 0xC2: case 0xC3: case 0xCA: case 0xCB: case 0xCC: /* ret / int3 */
            case 0xE8: case 0xE9: case 0xEB:
                return false;
            default: break;
            }
            if (hs.opcode == 0xFF && hs.modrm_reg >= 2 && hs.modrm_reg <= 5) return false; /* indirect call/jmp */
        }
        total += len;
    }
    return total >= HOOK_MIN_BYTES;
}
static bool (*g_prologue_check)(const void *) = prologue_hookable;

static bool install_hook(Detour *d, const void *fn, void *hook, const char *name)
{
    char m[96];
    if (!g_prologue_check(fn)) {
        snprintf(m, sizeof m, "hook %s skipped: unsafe prologue", name);
        plog(m);
        return false;
    }
    Detour_Construct(d, DetourMode_x64);
    void *stub = Detour_DetourFunction(d, (uint64_t)fn, hook);
    if (!stub || !d->StubPtr) {
        snprintf(m, sizeof m, "hook %s failed", name);
        plog(m);
        return false;
    }
    snprintf(m, sizeof m, "hook %s installed", name);
    plog(m);
    return true;
}

static void install_hooks(void)
{
    FILE *nf = fopen(NO_HOOKS_FILE, "r");
    if (nf) {
        fclose(nf);
        plog("hooks disabled by no_hooks file (polling only)");
        return;
    }
    g_hook_set_ok = install_hook(&Detour_scePadSetLightBar, (const void *)scePadSetLightBar, (void *)scePadSetLightBar_hook, "scePadSetLightBar");
    g_hook_reset_ok = install_hook(&Detour_scePadResetLightBar, (const void *)scePadResetLightBar, (void *)scePadResetLightBar_hook, "scePadResetLightBar");
}

/* ----------------------------------------------------------------- tick */
static int idle(const char *why)
{
    st_store(0);
    note_status(why, 0, 0, 0, 0);
    return 0;
}

/* One pass. Returns the number of controllers that accepted the colour. */
static int vccs_plugin_tick(void)
{
    if (app_in_foreground()) return idle("standby:app-foreground");

    PCfg c;
    if (!pcfg_load(&c)) return idle("standby:no-config");
    if (!c.keep) return idle("standby:keep-off");
    if (c.sysdef) return idle("standby:system-default");

    OrbisPadColor col;
    memset(&col, 0, sizeof col);
    col.r = c.off ? 0 : (uint8_t)(c.r * c.bright / 100);
    col.g = c.off ? 0 : (uint8_t)(c.g * c.bright / 100);
    col.b = c.off ? 0 : (uint8_t)(c.b * c.bright / 100);
    st_store(ST_ACTIVE | ((uint32_t)col.r << 16) | ((uint32_t)col.g << 8) | col.b); /* hooks start overriding */

    OrbisUserServiceLoginUserIdList l;
    memset(&l, 0xFF, sizeof l);
    int ur = sceUserServiceGetLoginUserIdList(&l);
    if (ur != 0) { note_status("active:user-list-failed", 0, 0, 0, ur); return 0; }

    int users = 0, handles = 0, n = 0, err = 0;
    for (int i = 0; i < MAXU; i++) {
        int uid = (int)l.userId[i];
        if (uid == -1) continue;
        users++;
        int h = scePadGetHandle(uid, 0, 0); /* only a handle this process already has */
        if (h < 0) continue;
        handles++;
        int r = real_set(h, &col);
        if (r == 0) n++; else err = r;
    }
    note_status("active", users, handles, n, err);
    return n;
}

#ifdef HOST_TEST
static void vccs_test_reset(void)
{
    g_alive_init = false; g_alive_val = -1; g_alive_changed_ms = 0;
    g_last_msg[0] = 0; g_tag_ready = false;
    st_store(0);
}
#else
static void *vccs_worker(void *arg)
{
    (void)arg;
    for (;;) {
        vccs_plugin_tick();
        sceKernelUsleep(TICK_US);
    }
    return NULL;
}

int module_start(size_t argc, const void *args)
{
    (void)argc; (void)args;
    plog("module_start (plugin loaded in this process)");
    install_hooks();
    ScePthread th;
    int tr = scePthreadCreate(&th, NULL, vccs_worker, NULL, "vccs_plugin");
    if (tr != 0) {
        char m[64];
        snprintf(m, sizeof m, "worker thread failed: 0x%X", (unsigned)tr);
        plog(m);
    }
    return 0; /* never block or fail module load */
}

int module_stop(size_t argc, const void *args)
{
    (void)argc; (void)args;
    return 0;
}
#endif
