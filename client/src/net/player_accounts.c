// Account data of players + the server's billboard config: client/include/player_accounts.h.
#include <player_accounts.h>
#include <stdio.h>
#include <string.h>

#define PA_MAX 128

static struct {
    NetPlayerInfoPacket infos[PA_MAX];
    int count;
    NetBillboardPacket billboard;
    int billboard_set;      // a BILLBOARD arrived (else the defaults)
    int billboard_pending;  // ... since the last INIT
} g_pa;

void player_accounts_on_info(const NetPlayerInfoPacket *info) {
    NetPlayerInfoPacket copy = *info;
    copy.clan[sizeof(copy.clan) - 1] = 0;
    for (int i = 0; i < g_pa.count; i++) {
        if (g_pa.infos[i].uid == info->uid) {
            g_pa.infos[i] = copy;
            return;
        }
    }
    if (g_pa.count < PA_MAX) g_pa.infos[g_pa.count++] = copy;
}

const NetPlayerInfoPacket *player_accounts_get(const int uid) {
    for (int i = 0; i < g_pa.count; i++) {
        if (g_pa.infos[i].uid == uid) return &g_pa.infos[i];
    }
    return NULL;
}

static void pa_default(char *dst, const size_t size, const char *value, const char *def) {
    snprintf(dst, size, "%s", value && value[0] ? value : def);
}

// game.updateBillboards: txt || "Your Ad Here", txtCol || "#e3e3e3", bgCol || "#000000"
void player_accounts_on_billboard(const NetBillboardPacket *billboard) {
    NetBillboardPacket b = *billboard;
    b.txt[sizeof(b.txt) - 1] = 0;
    b.txt_col[sizeof(b.txt_col) - 1] = 0;
    b.bg_col[sizeof(b.bg_col) - 1] = 0;
    pa_default(g_pa.billboard.txt, sizeof(g_pa.billboard.txt), b.txt, "Your Ad Here");
    pa_default(g_pa.billboard.txt_col, sizeof(g_pa.billboard.txt_col), b.txt_col, "#e3e3e3");
    pa_default(g_pa.billboard.bg_col, sizeof(g_pa.billboard.bg_col), b.bg_col, "#000000");
    g_pa.billboard_set = 1;
    g_pa.billboard_pending = 1;
}

const NetBillboardPacket *player_accounts_billboard(void) {
    if (!g_pa.billboard_set) {
        const NetBillboardPacket none = {{0}};
        player_accounts_on_billboard(&none);
        g_pa.billboard_set = 0;
        g_pa.billboard_pending = 0;
    }
    return &g_pa.billboard;
}

void player_accounts_on_init(void) {
    g_pa.count = 0;
    if (!g_pa.billboard_pending) g_pa.billboard_set = 0;
    g_pa.billboard_pending = 0;
}
