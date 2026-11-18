#include "main.c"
#include <assert.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok:   %s\n", msg); } while (0)

static void run(int n) { for (int i = 0; i < n; i++) app_frame(); }
static void tap(uint32_t b) { host_btn = b; run(1); host_btn = 0; run(1); }
static bool light_is(int h, int r, int g, int b) { return host_light[h][0] == r && host_light[h][1] == g && host_light[h][2] == b; }

static void dump_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint32_t p = g_host_fb[i];
        fputc((p >> 16) & 255, f); fputc((p >> 8) & 255, f); fputc(p & 255, f);
    }
    fclose(f);
}

int main(void)
{
    plat_mkdir(CFG_DIR);
    remove(CFG_FILE);
    app_init();
    run(3);
    CHECK(g_nctl == 2, "detects 2 controllers (2 logged-in users)");
    CHECK(g_ctl[0].connected && g_ctl[1].connected, "both report connected");
    CHECK(g_target == 0 && g_nctl == 2, "default target is controller 1");

    /* initial colour 0,96,255 at 100% -> sent to target only */
    CHECK(light_is(1, 0, 96, 255), "initial colour reaches controller 1");
    CHECK(host_set_calls[2] == 0, "controller 2 untouched while not targeted");

    /* adjust red up 3 steps (row 0) */
    tap(ORBIS_PAD_BUTTON_RIGHT); tap(ORBIS_PAD_BUTTON_RIGHT); tap(ORBIS_PAD_BUTTON_RIGHT);
    CHECK(light_is(1, 3, 96, 255), "red slider +3 applied in real time");

    /* brightness step: go to BSTEP row (down x4), right -> MED */
    for (int i = 0; i < 4; i++) tap(ORBIS_PAD_BUTTON_DOWN);
    CHECK(g_row == ROW_BSTEP, "navigated to brightness step row");
    tap(ORBIS_PAD_BUTTON_RIGHT);
    CHECK(g_bright == 50 && light_is(1, 1, 48, 127), "MED brightness = 50% scaling");
    tap(ORBIS_PAD_BUTTON_RIGHT);
    CHECK(g_bright == 10, "MIN brightness = 10%");
    tap(ORBIS_PAD_BUTTON_RIGHT);
    CHECK(g_bright == 0 && light_is(1, 0, 0, 0), "ZERO brightness -> 0,0,0");
    tap(ORBIS_PAD_BUTTON_LEFT); tap(ORBIS_PAD_BUTTON_LEFT); tap(ORBIS_PAD_BUTTON_LEFT);
    CHECK(g_bright == 100, "back to MAX");

    /* preset */
    tap(ORBIS_PAD_BUTTON_DOWN); /* ROW_PRESET */
    tap(ORBIS_PAD_BUTTON_RIGHT);
    CHECK(light_is(1, 255, 0, 0), "preset RED applied");

    /* OFF + persistence against simulated system override */
    tap(ORBIS_PAD_BUTTON_SQUARE);
    CHECK(g_off && light_is(1, 0, 0, 0), "SQUARE turns light off (0,0,0)");
    host_system_override(1); /* system forces its own colour */
    CHECK(!light_is(1, 0, 0, 0), "(sim) system override changed the colour");
    run(HEARTBEAT_FRAMES + 2);
    CHECK(light_is(1, 0, 0, 0), "heartbeat re-applies OFF after override");

    /* turning on again */
    tap(ORBIS_PAD_BUTTON_SQUARE);
    CHECK(!g_off && light_is(1, 255, 0, 0), "SQUARE again restores colour");

    /* target ALL */
    for (int i = 0; i < 1; i++) tap(ORBIS_PAD_BUTTON_DOWN); /* ROW_TARGET */
    tap(ORBIS_PAD_BUTTON_RIGHT); tap(ORBIS_PAD_BUTTON_RIGHT);
    CHECK(g_target == g_nctl, "target cycles to ALL");
    run(2);
    CHECK(light_is(1, 255, 0, 0) && light_is(2, 255, 0, 0), "ALL targets both controllers");

    /* disconnect handling */
    host_conn[2] = false;
    run(2);
    CHECK(!g_ctl[1].connected, "disconnect detected");
    int before = host_set_calls[2];
    run(40);
    CHECK(host_set_calls[2] == before, "no writes to disconnected controller");
    host_conn[2] = true;
    host_system_override(2);
    run(HEARTBEAT_FRAMES + 2);
    CHECK(light_is(2, 255, 0, 0), "reconnected controller gets colour back");

    /* error path */
    host_set_fail = -1;
    run(HEARTBEAT_FRAMES + 2);
    CHECK(g_ctl[0].last_err == -1 && strstr(g_status, "FAILED"), "API error surfaced, no crash");
    host_set_fail = 0;

    /* system default */
    tap(ORBIS_PAD_BUTTON_TRIANGLE);
    CHECK(g_sysdef && host_reset_calls[1] > 0, "TRIANGLE calls scePadResetLightBar");

    /* persistence: change state, let it settle, confirm file, wipe state, reload */
    g_sysdef = false; g_off = true; g_col[0] = 12; g_col[1] = 34; g_col[2] = 56; g_bright = 50; g_preset = 3;
    run(CFG_SETTLE_FRAMES + 5);
    {
        FILE *f = fopen(CFG_FILE, "r");
        char txt[512] = {0};
        if (f) { fread(txt, 1, sizeof txt - 1, f); fclose(f); }
        CHECK(f && strstr(txt, "r=12") && strstr(txt, "brightness=50") && strstr(txt, "off=1"), "settings file written after settle");
    }
    CHECK(strstr(g_cfg_msg, "SAVED") != NULL, "UI reports settings saved");
    g_col[0] = g_col[1] = g_col[2] = 0; g_bright = 100; g_off = false; g_preset = -1;
    CHECK(cfg_load() && g_col[0] == 12 && g_col[1] == 34 && g_col[2] == 56 && g_bright == 50 && g_off && g_preset == 3, "settings reload restores all values");
    g_dirty = true; run(2);
    CHECK(light_is(1, 0, 0, 0), "loaded OFF state is pushed to controller");
    remove(CFG_FILE);
    CHECK(!cfg_load(), "missing config handled (returns false, no crash)");

    {
        FILE *a = fopen(ALIVE_FILE, "r");
        CHECK(a != NULL, "foreground heartbeat file is written");
        if (a) fclose(a);
    }
    g_row = ROW_KEEP;
    tap(ORBIS_PAD_BUTTON_CROSS);
    CHECK(!g_keep, "KEEP AFTER EXIT toggles off with Cross");
    run(CFG_SETTLE_FRAMES + 5);
    {
        FILE *f = fopen(CFG_FILE, "r");
        char txt[512] = {0};
        if (f) { fread(txt, 1, sizeof txt - 1, f); fclose(f); }
        CHECK(f && strstr(txt, "keep=0"), "keep=0 is saved for the plugin");
    }
    g_keep = true; g_dirty = true;

    dump_ppm("build/ui_preview.ppm");
    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails ? 1 : 0;
}
