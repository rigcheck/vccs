/* Host-side stand-ins for the OpenOrbis Pad / UserService APIs. Test use only. */
#ifndef HOST_STUBS_H
#define HOST_STUBS_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#pragma GCC diagnostic ignored "-Wunused-function"

typedef struct { uint8_t r, g, b, a; } OrbisPadColor;
typedef struct { uint32_t buttons; bool connected; uint8_t connectedCount; } OrbisPadData;
typedef int OrbisUserServiceUserId;
typedef struct { OrbisUserServiceUserId userId[4]; } OrbisUserServiceLoginUserIdList;

#define ORBIS_PAD_BUTTON_UP 0x10
#define ORBIS_PAD_BUTTON_RIGHT 0x20
#define ORBIS_PAD_BUTTON_DOWN 0x40
#define ORBIS_PAD_BUTTON_LEFT 0x80
#define ORBIS_PAD_BUTTON_R1 0x800
#define ORBIS_PAD_BUTTON_TRIANGLE 0x1000
#define ORBIS_PAD_BUTTON_CIRCLE 0x2000
#define ORBIS_PAD_BUTTON_CROSS 0x4000
#define ORBIS_PAD_BUTTON_SQUARE 0x8000

/* test controls */
static uint32_t host_btn = 0;
static bool host_conn[8] = {0, true, true, 0, 0, 0, 0, 0};
static uint8_t host_light[8][3];
static int host_set_calls[8], host_reset_calls[8];
static int host_set_fail = 0; /* when nonzero scePadSetLightBar returns this */

static void host_system_override(int h) { host_light[h][0] = 0; host_light[h][1] = 0; host_light[h][2] = 64; }

static int sceKernelMkdir(const char *p, int m) { (void)p; (void)m; return 0; }
static int scePadInit(void) { return 0; }
__attribute__((unused)) static int host_open_calls = 0;
__attribute__((unused)) static bool host_pad_open[8] = {0, 1, 1, 0, 0, 0, 0, 0};
static int scePadOpen(int uid, int type, int idx, void *p) { (void)type; (void)idx; (void)p; host_open_calls++; return uid; }
__attribute__((unused)) static int scePadGetHandle(int uid, int type, int idx)
{
    (void)type; (void)idx;
    return (uid >= 0 && uid < 8 && host_pad_open[uid]) ? uid : -1;
}
static int scePadReadState(int h, OrbisPadData *d)
{
    d->connected = host_conn[h];
    d->buttons = host_conn[h] ? host_btn : 0;
    return 0;
}
static int scePadSetLightBar(int h, const OrbisPadColor *c)
{
    if (host_set_fail) return host_set_fail;
    host_light[h][0] = c->r; host_light[h][1] = c->g; host_light[h][2] = c->b;
    host_set_calls[h]++;
    return 0;
}
static int scePadResetLightBar(int h) { host_system_override(h); host_reset_calls[h]++; return 0; }
static int sceUserServiceGetLoginUserIdList(OrbisUserServiceLoginUserIdList *l)
{
    l->userId[0] = 1; l->userId[1] = 2; l->userId[2] = -1; l->userId[3] = -1;
    return 0;
}
static int sceUserServiceGetInitialUser(OrbisUserServiceUserId *u) { *u = 1; return 0; }
#endif
