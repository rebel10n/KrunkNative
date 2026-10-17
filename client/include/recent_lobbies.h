#pragma once
// The lobbies this client was in lately, most recent first: Find New Game, Quick Match and
// quick join exclude them so a new seek never lands back in the lobby it leaves, nor bounces
// between the last two. Only when nothing else exists do they come back (the older ones before the
// one the player is in). Used for our matchmaker (servers_ui.c: excludeGame + excludeRecent, the
// finder's exclude) and for krunker.io's (krunker_net.c / krunker_lobbies.c: excludeGame, the
// official client's own parameter). Ids are opaque (game ids, or "host:port" for finder servers).
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define RECENT_LOBBIES_MAX 4      // the one the player is in + 3 before it
#define RECENT_LOBBY_ID_MAX 160

typedef struct {
    char ids[RECENT_LOBBIES_MAX][RECENT_LOBBY_ID_MAX];
    int count;
} RecentLobbies;

// 1 = the most recent (the lobby the player is in, or just left), 2.. older ones; 0 = not recent.
static inline int recent_lobbies_rank(const RecentLobbies *r, const char *id) {
    if (!r || !id || !id[0]) return 0;
    for (int i = 0; i < r->count; i++) {
        if (!strcmp(r->ids[i], id)) return i + 1;
    }
    return 0;
}

// The lobby joined now goes first (moved there when it already is in the list); the oldest drops out.
static inline void recent_lobbies_push(RecentLobbies *r, const char *id) {
    if (!r || !id || !id[0] || strlen(id) >= RECENT_LOBBY_ID_MAX) return;
    int at = recent_lobbies_rank(r, id) - 1;
    if (at < 0) at = r->count < RECENT_LOBBIES_MAX ? r->count++ : RECENT_LOBBIES_MAX - 1;
    for (int i = at; i > 0; i--) memcpy(r->ids[i], r->ids[i - 1], RECENT_LOBBY_ID_MAX);
    snprintf(r->ids[0], RECENT_LOBBY_ID_MAX, "%s", id);
}

// Every id but `skip` (NULL: none), most recent first, comma-separated ("" when none fit or none are there).
static inline void recent_lobbies_join(const RecentLobbies *r, const char *skip, char *out, const size_t n) {
    if (!out || !n) return;
    out[0] = 0;
    size_t len = 0;
    for (int i = 0; r && i < r->count; i++) {
        if (skip && !strcmp(r->ids[i], skip)) continue;
        const size_t id_len = strlen(r->ids[i]);
        if (len + (len ? 1 : 0) + id_len + 1 > n) break;
        if (len) out[len++] = ',';
        memcpy(out + len, r->ids[i], id_len + 1);
        len += id_len;
    }
}
