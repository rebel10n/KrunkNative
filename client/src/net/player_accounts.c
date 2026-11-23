// Account data of players: client/include/player_accounts.h.
#include <player_accounts.h>
#include <stdio.h>
#include <string.h>

#define PA_MAX 128

static struct {
    NetPlayerInfoPacket infos[PA_MAX];
    int32_t ranked_rank[PA_MAX]; // NET_EXT_KN_PLAYER_RANK_FLAG of infos[i].uid: rankedRank, 0 none
    int32_t flag[PA_MAX];        // its flagIndex, -1 none
    int count;
} g_pa;

void player_accounts_on_info(const NetPlayerInfoPacket *info) {
    NetPlayerInfoPacket copy = *info;
    copy.clan[sizeof(copy.clan) - 1] = 0;
    int i = 0;
    while (i < g_pa.count && g_pa.infos[i].uid != info->uid) i++;
    if (i == g_pa.count) {
        if (g_pa.count >= PA_MAX) return;
        g_pa.count++;
    }
    g_pa.infos[i] = copy;
    // a PLAYER_INFO starts the player over: its NET_EXT_KN_PLAYER_RANK_FLAG follows it (none from a bot or an older server)
    g_pa.ranked_rank[i] = 0;
    g_pa.flag[i] = -1;
}

void player_accounts_on_rank_flag(const NetPlayerRankFlagPacket *packet) {
    for (int i = 0; packet && i < g_pa.count; i++) {
        if (g_pa.infos[i].uid != packet->uid) continue;
        g_pa.ranked_rank[i] = packet->ranked_rank > 0 ? packet->ranked_rank : 0;
        g_pa.flag[i] = packet->flag >= 0 ? packet->flag : -1;
        return;
    }
}

const NetPlayerInfoPacket *player_accounts_get(const int uid) {
    for (int i = 0; i < g_pa.count; i++) {
        if (g_pa.infos[i].uid == uid) return &g_pa.infos[i];
    }
    return NULL;
}

int player_accounts_ranked_rank(const int uid) {
    for (int i = 0; i < g_pa.count; i++) {
        if (g_pa.infos[i].uid == uid) return g_pa.ranked_rank[i];
    }
    return 0;
}

int player_accounts_flag(const int uid) {
    for (int i = 0; i < g_pa.count; i++) {
        if (g_pa.infos[i].uid == uid) return g_pa.flag[i];
    }
    return -1;
}

// A bot player (server bot_send_info: NET_PLAYER_INFO_BOT). Humans and guests send no such bit.
int player_accounts_is_bot(const int uid) {
    const NetPlayerInfoPacket *info = player_accounts_get(uid);
    return info && (info->flags & NET_PLAYER_INFO_BOT) != 0;
}

void player_accounts_on_init(void) {
    g_pa.count = 0;
}
