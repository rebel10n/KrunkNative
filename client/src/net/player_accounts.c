// Account data of players: client/include/player_accounts.h.
#include <player_accounts.h>
#include <stdio.h>
#include <string.h>

#define PA_MAX 128

static struct {
    NetPlayerInfoPacket infos[PA_MAX];
    int count;
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

// A bot player (server bot_send_info: NET_PLAYER_INFO_BOT). Humans and guests send no such bit.
int player_accounts_is_bot(const int uid) {
    const NetPlayerInfoPacket *info = player_accounts_get(uid);
    return info && (info->flags & NET_PLAYER_INFO_BOT) != 0;
}

void player_accounts_on_init(void) {
    g_pa.count = 0;
}
