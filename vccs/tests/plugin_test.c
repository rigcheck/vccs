#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include "Detour.h"
#include "GoldHEN.h"

/* Fakes for the SDK functions (the real ones are compiled from the SDK sources for the PS4).
 * The fake Detour makes the "stub" point at the original function, like a real trampoline would. */
static int g_fake_detour_fail = 0;
void Detour_Construct(Detour *d, DetourMode m) { memset(d, 0, sizeof *d); d->Mode = m; }
void *Detour_DetourFunction(Detour *d, uint64_t fn, void *hook)
{
    if (g_fake_detour_fail) return NULL;
    d->FunctionPtr = (void *)fn;
    d->HookPtr = hook;
    d->StubPtr = (void *)fn;
    return d->StubPtr;
}
void Detour_RestoreFunction(Detour *d) { (void)d; }
void Detour_Destroy(Detour *d) { (void)d; }
int sys_sdk_proc_info(struct proc_info *i)
{
    memset(i, 0, sizeof *i);
    i->pid = 77;
    strcpy(i->titleid, "CUSA00001");
    strcpy(i->name, "eboot.bin");
    return 0;
}

#include "../plugin/vccs_plugin.c"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok:   %s\n", msg); } while (0)

static void write_file(const char *path, const char *txt) { FILE *f = fopen(path, "w"); if (f) { fputs(txt, f); fclose(f); } }
static void reset_lights(void)
{
    memset(host_light, 0xEE, sizeof host_light);
    memset(host_set_calls, 0, sizeof host_set_calls);
    memset(host_reset_calls, 0, sizeof host_reset_calls);
    host_set_fail = 0;
}
static int total_sets(void) { int s = 0; for (int i = 0; i < 8; i++) s += host_set_calls[i]; return s; }
static bool light_is(int h, int r, int g, int b) { return host_light[h][0] == r && host_light[h][1] == g && host_light[h][2] == b; }
static bool fake_ok(const void *p) { (void)p; return true; }
static bool fake_bad(const void *p) { (void)p; return false; }
static bool active_now(void) { return (st_load() & ST_ACTIVE) != 0; }

static const char *CFG_ON = "r=10\ng=20\nb=30\nbrightness=100\noff=0\nsystem_default=0\nkeep=1\n";

int main(void)
{
    mkdir(CFG_DIR, 0777);
    remove(CFG_FILE); remove(ALIVE_FILE); remove(LOG_FILE); remove(NO_HOOKS_FILE);
    host_ms = 1000000;
    host_pad_open[1] = host_pad_open[2] = true;
    g_prologue_check = fake_ok;

    /* ---------------- polling logic ---------------- */
    reset_lights();
    CHECK(vccs_plugin_tick() == 0 && total_sets() == 0 && !active_now(), "no config file -> does nothing");

    write_file(CFG_FILE, "garbage\nnot=keys\n");
    CHECK(vccs_plugin_tick() == 0 && total_sets() == 0, "unrecognised config -> does nothing");

    write_file(CFG_FILE, "r=10\ng=20\nb=30\nbrightness=50\noff=0\nsystem_default=0\nkeep=1\n");
    reset_lights();
    CHECK(vccs_plugin_tick() == 2 && active_now(), "applies to both controllers when app is not running");
    CHECK(light_is(1, 5, 10, 15) && light_is(2, 5, 10, 15), "brightness scaling applied (50%)");

    /* heartbeat: counter based */
    write_file(ALIVE_FILE, "100\n"); host_ms += 250;
    reset_lights();
    CHECK(vccs_plugin_tick() == 2, "first heartbeat value seen is treated as stale (left over from an old run)");
    write_file(ALIVE_FILE, "130\n"); host_ms += 250;
    reset_lights();
    CHECK(vccs_plugin_tick() == 0 && total_sets() == 0 && !active_now(), "stands down when the heartbeat counter changes");
    host_ms += 2000;
    CHECK(vccs_plugin_tick() == 0, "still standing down inside the 3 s window");
    host_ms += 2000;
    reset_lights();
    CHECK(vccs_plugin_tick() == 2 && active_now(), "takes over once the heartbeat stops changing (app left)");

    remove(ALIVE_FILE); vccs_test_reset();

    write_file(CFG_FILE, "r=10\ng=20\nb=30\nbrightness=100\noff=0\nsystem_default=0\nkeep=0\n");
    reset_lights();
    CHECK(vccs_plugin_tick() == 0 && total_sets() == 0 && !active_now(), "keep=0 -> does nothing");
    vccs_plugin_tick(); vccs_plugin_tick(); /* repeated idle ticks must not spam the log */

    write_file(CFG_FILE, "r=10\ng=20\nb=30\nbrightness=100\noff=1\nsystem_default=0\nkeep=1\n");
    reset_lights();
    CHECK(vccs_plugin_tick() == 2 && light_is(1, 0, 0, 0) && light_is(2, 0, 0, 0), "off=1 -> light bar black");

    write_file(CFG_FILE, "r=10\ng=20\nb=30\nbrightness=100\noff=0\nsystem_default=1\nkeep=1\n");
    reset_lights();
    CHECK(vccs_plugin_tick() == 0 && total_sets() == 0 && host_reset_calls[1] == 0 && !active_now(), "system_default=1 -> leaves light bar to the system");

    write_file(CFG_FILE, "r=999\ng=-5\nb=30\nbrightness=300\nkeep=1\n");
    reset_lights();
    vccs_plugin_tick();
    CHECK(light_is(1, 255, 0, 30), "out-of-range values are clamped");

    write_file(CFG_FILE, "r=1\ng=2\nb=3\nbrightness=100\nkeep=1\n");
    host_pad_open[1] = false;
    reset_lights();
    int open_before = host_open_calls;
    CHECK(vccs_plugin_tick() == 1 && host_set_calls[1] == 0 && host_set_calls[2] == 1, "only touches controllers the host process already opened");
    host_pad_open[2] = false;
    CHECK(vccs_plugin_tick() == 0, "no open pads -> does nothing");
    CHECK(host_open_calls == open_before, "never calls scePadOpen");

    host_pad_open[1] = host_pad_open[2] = true;
    host_set_fail = -1;
    CHECK(vccs_plugin_tick() == 0, "light bar API error is survived");
    host_set_fail = 0;

    /* ---------------- log ---------------- */
    {
        FILE *f = fopen(LOG_FILE, "r");
        char txt[8192] = {0};
        size_t got = 0;
        if (f) { got = fread(txt, 1, sizeof txt - 1, f); fclose(f); }
        (void)got;
        CHECK(f && strstr(txt, "standby:app-foreground") && strstr(txt, "standby:keep-off") &&
              strstr(txt, "standby:system-default") && strstr(txt, "standby:no-config"), "log records why the plugin was idle");
        CHECK(strstr(txt, "active users=2 handles=2 applied=2 err=0x0") != NULL, "log records a successful apply");
        CHECK(strstr(txt, "err=0xFFFFFFFF") != NULL, "log records the light bar error code");
        int count = 0;
        for (char *q = txt; (q = strstr(q, "standby:keep-off")) != NULL; q++) count++;
        CHECK(count == 1, "log only records state changes (no spam)");
        CHECK(strstr(txt, "CUSA00001 eboot.bin pid=77") != NULL, "log lines are tagged with title id / process name");
    }

    /* ---------------- prologue safety check ---------------- */
    {
        static const uint8_t good[32] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48, 0x83, 0xEC, 0x10, 0x90, 0x90};
        static const uint8_t short_fn[32] = {0x55, 0x48, 0x89, 0xE5, 0x5D, 0xC3, 0x90, 0x90};
        static const uint8_t ret_first[32] = {0xC3, 0x90, 0x90, 0x90};
        static const uint8_t call_rel[32] = {0x55, 0x48, 0x89, 0xE5, 0xE8, 0x10, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        static const uint8_t rip_rel[32] = {0x48, 0x8D, 0x05, 0x10, 0x00, 0x00, 0x00, 0x55, 0x48, 0x89, 0xE5, 0x53, 0x50, 0x90, 0x90, 0x90};
        static const uint8_t jmp_thunk[32] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        static const uint8_t call_reg[32] = {0x55, 0x48, 0x89, 0xE5, 0xFF, 0xD0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        CHECK(prologue_hookable(good), "plain push/mov/sub prologue is hookable");
        CHECK(!prologue_hookable(short_fn), "function shorter than 14 bytes (ret inside) is refused");
        CHECK(!prologue_hookable(ret_first), "function starting with ret is refused");
        CHECK(!prologue_hookable(call_rel), "relative call in the first bytes is refused");
        CHECK(!prologue_hookable(rip_rel), "RIP-relative operand in the first bytes is refused");
        CHECK(!prologue_hookable(jmp_thunk), "jmp [rip+x] thunk is refused");
        CHECK(!prologue_hookable(call_reg), "indirect call in the first bytes is refused");
        CHECK(!prologue_hookable(NULL), "NULL target is refused");
    }

    /* ---------------- hooks ---------------- */
    write_file(CFG_FILE, CFG_ON);
    remove(ALIVE_FILE); vccs_test_reset();
    g_prologue_check = fake_ok; g_fake_detour_fail = 0;
    g_hook_set_ok = g_hook_reset_ok = false;
    install_hooks();
    CHECK(g_hook_set_ok && g_hook_reset_ok, "both hooks install");
    CHECK(Detour_scePadSetLightBar.HookPtr == (void *)scePadSetLightBar_hook &&
          Detour_scePadResetLightBar.HookPtr == (void *)scePadResetLightBar_hook, "hooks point at the plugin's functions");

    OrbisPadColor game;
    memset(&game, 0, sizeof game);
    game.r = 200; game.g = 100; game.b = 50;

    vccs_plugin_tick();
    reset_lights();
    scePadSetLightBar_hook(1, &game);
    CHECK(light_is(1, 10, 20, 30), "game's SetLightBar is replaced by the VCCS colour");
    reset_lights();
    scePadResetLightBar_hook(1);
    CHECK(light_is(1, 10, 20, 30) && host_reset_calls[1] == 0, "system 'reset to default colour' becomes the VCCS colour");

    write_file(CFG_FILE, "r=10\ng=20\nb=30\nbrightness=100\nkeep=0\n");
    vccs_plugin_tick();
    reset_lights();
    scePadSetLightBar_hook(1, &game);
    CHECK(light_is(1, 200, 100, 50), "keep=0 -> game's colour passes through untouched");
    scePadResetLightBar_hook(1);
    CHECK(host_reset_calls[1] == 1, "keep=0 -> reset passes through to the real function");

    write_file(CFG_FILE, CFG_ON);
    vccs_test_reset();
    write_file(ALIVE_FILE, "1\n"); vccs_plugin_tick();
    write_file(ALIVE_FILE, "2\n"); host_ms += 250; vccs_plugin_tick();
    reset_lights();
    scePadSetLightBar_hook(1, &game);
    CHECK(light_is(1, 200, 100, 50), "app in foreground -> hooks pass through (app controls the light)");
    remove(ALIVE_FILE); vccs_test_reset();

    /* install failures fall back safely */
    g_hook_set_ok = g_hook_reset_ok = false;
    g_fake_detour_fail = 1;
    install_hooks();
    CHECK(!g_hook_set_ok && !g_hook_reset_ok, "failed detour -> hooks marked not installed");
    vccs_plugin_tick();
    reset_lights();
    scePadResetLightBar_hook(1);
    CHECK(light_is(1, 10, 20, 30), "reset hook still sets the VCCS colour when the set-hook is missing");
    g_fake_detour_fail = 0;

    g_prologue_check = fake_bad;
    install_hooks();
    CHECK(!g_hook_set_ok && !g_hook_reset_ok, "unsafe prologue -> hooks not installed");
    g_prologue_check = fake_ok;

    write_file(NO_HOOKS_FILE, "1\n");
    install_hooks();
    CHECK(!g_hook_set_ok && !g_hook_reset_ok, "no_hooks file disables hooks");
    remove(NO_HOOKS_FILE);

    {
        FILE *f = fopen(LOG_FILE, "r");
        char txt[8192] = {0};
        size_t got = 0;
        if (f) { got = fread(txt, 1, sizeof txt - 1, f); fclose(f); }
        (void)got;
        CHECK(strstr(txt, "hook scePadSetLightBar installed") && strstr(txt, "hook scePadResetLightBar installed"), "log records hook installation");
        CHECK(strstr(txt, "hook scePadSetLightBar failed") != NULL, "log records a failed hook");
        CHECK(strstr(txt, "skipped: unsafe prologue") != NULL, "log records an unsafe-prologue skip");
        CHECK(strstr(txt, "hooks disabled by no_hooks") != NULL, "log records the no_hooks switch");
    }

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails ? 1 : 0;
}
