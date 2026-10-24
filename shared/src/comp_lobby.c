// Krunker v7.1.4 comp lobby (shared/include/comp_lobby.h): game.js lobbyPlayers, syncLobbyP,
// updLobbyP, canSpawn.
#include <comp_lobby.h>
#include <ruleset.h>
#include <stdlib.h>
#include <string.h>

static CompLobby *lobby(Game *game) {
    if (!game->comp_lobby) game->comp_lobby = calloc(1, sizeof(CompLobby));
    return game->comp_lobby;
}

static CompLobbyEntry *find(const Game *game, const int uid) {
    CompLobby *l = game ? game->comp_lobby : NULL;
    if (!l) return NULL;
    for (int i = 0; i < l->count; i++) {
        if (l->entries[i].uid == uid) return &l->entries[i];
    }
    return NULL;
}

const CompLobbyEntry *comp_lobby_get(const Game *game, const int uid) {
    return find(game, uid);
}

static void send_entry(Game *game, const int target, const NetCompLobbyOp op, const CompLobbyEntry *entry) {
    NetCompLobbyPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.op = (uint8_t) op;
    if (entry) packet.entry = *entry;
    game_send(game, target, NET_PACKET_COMP_LOBBY, &packet, sizeof(packet));
}

void comp_lobby_send_all(Game *game, const int uid) {
    if (!game || !game->config.comp) return;
    send_entry(game, uid, NET_COMP_LOBBY_CLEAR, NULL);
    const CompLobby *l = game->comp_lobby;
    for (int i = 0; l && i < l->count; i++) {
        if (l->entries[i].active) send_entry(game, uid, NET_COMP_LOBBY_SET, &l->entries[i]);
    }
}

void comp_lobby_join(Game *game, const int uid, const CompLobbyEntry *identity) {
    if (!game || !game->config.comp) return; // syncLobbyP: if (game.isComp)
    CompLobbyEntry *entry = find(game, uid);
    const int created = !entry;
    if (!entry) {
        CompLobby *l = lobby(game);
        if (!l || l->count >= COMP_LOBBY_MAX) return;
        entry = &l->entries[l->count++];
        memset(entry, 0, sizeof(*entry));
        entry->uid = uid;
        entry->team = 0;
        entry->class_index = -1;
        entry->flag = -1;
    }
    if (identity) { // i, n, v, cl, l, f from player.account
        entry->account_id = identity->account_id;
        entry->level = identity->level;
        entry->verified = identity->verified;
        entry->flag = identity->flag;
        memcpy(entry->name, identity->name, sizeof(entry->name));
        memcpy(entry->clan, identity->clan, sizeof(entry->clan));
        entry->name[sizeof(entry->name) - 1] = 0;
        entry->clan[sizeof(entry->clan) - 1] = 0;
    }
    const int was_active = entry->active;
    entry->active = 1;
    // a new entry or a reactivated one: the table to the joiner, the entry to everyone else
    if (created || !was_active) {
        comp_lobby_send_all(game, uid);
        send_entry(game, NET_TARGET_EXCEPT(uid), NET_COMP_LOBBY_SET, entry);
    } else {
        send_entry(game, NET_TARGET_ALL, NET_COMP_LOBBY_SET, entry);
    }
}

void comp_lobby_leave(Game *game, const int uid) {
    CompLobbyEntry *entry = find(game, uid);
    if (!entry || !game->config.comp) return;
    if (entry->active && entry->team) entry->rejected = 1; // rej: left with a team
    entry->active = 0;
    entry->class_index = -1;
    entry->team = 0;
    CompLobbyEntry removed;
    memset(&removed, 0, sizeof(removed));
    removed.uid = uid;
    send_entry(game, NET_TARGET_ALL, NET_COMP_LOBBY_REMOVE, &removed);
}

static void broadcast(Game *game, const CompLobbyEntry *entry) {
    send_entry(game, NET_TARGET_ALL, NET_COMP_LOBBY_SET, entry);
}

int comp_lobby_class_count(const Game *game, const int team, const int class_index) {
    const CompLobby *l = game ? game->comp_lobby : NULL;
    int count = 0;
    for (int i = 0; l && i < l->count; i++) {
        const CompLobbyEntry *e = &l->entries[i];
        if (e->active && e->team == team && e->class_index == class_index) count++;
    }
    return count;
}

int comp_lobby_class_limit(const Game *game, const int class_index) {
    if (!game || class_index < 0) return 0;
    // KrunkNative's classes (from KN_CLASS_BASE: the Nettspend) have no classLim: the team's size
    if (class_index >= KN_CLASS_BASE) return game->config.team_size + 1;
    return class_index < GAME_CONFIG_CLASS_LIMITS ? game->config.class_limits[class_index] : 0;
}

int comp_lobby_update(Game *game, const int uid, const char key, const int value) {
    CompLobbyEntry *entry = find(game, uid);
    if (!entry || !game->config.comp) return 0;
    const GameConfig *cfg = &game->config;
    if (key == 'c') {
        if (value == entry->class_index) return 0;
        // any class of the table: Krunker's CLASS_COUNT and KrunkNative's Nettspend (CLASS_ID_NETTSPEND, the last), open to comp too
        if (value < 0 || value >= (int) (game->class_count ? game->class_count : rules_class_count())) return 0;
        // limitClasses: classLim<value> slots per team; a class with no limit is locked. classLim0..15 are
        // Krunker's; a KrunkNative class past them gets the team's size (comp_lobby_class_limit)
        if (cfg->limit_classes) {
            const int limit = comp_lobby_class_limit(game, value);
            if (!limit) return 0;
            int taken = 0;
            const CompLobby *l = game->comp_lobby;
            for (int i = 0; i < l->count; i++) { // v7 counts every entry with this team / class (a included)
                const CompLobbyEntry *e = &l->entries[i];
                if (e->team == entry->team && e->class_index == value && ++taken >= limit) return 0;
            }
        }
        entry->class_index = (int8_t) value;
    } else if (key == 't') {
        if (value == entry->team || !cfg->select_team) return 0; // only selTeam lobbies pick teams
        if (value < 0 || value > 2) return 0;
        const int size = cfg->team_size + 1; // tmSize 0..3 = 1v1..4v4
        int taken = 0;
        const CompLobby *l = game->comp_lobby;
        for (int i = 0; value && i < l->count; i++) {
            const CompLobbyEntry *e = &l->entries[i];
            if (e->active && e->team == value && ++taken >= size) return 0;
        }
        entry->team = (int8_t) value;
    } else {
        return 0;
    }
    broadcast(game, entry);
    return 1;
}

void comp_lobby_set_team(Game *game, const int uid, const int team) {
    CompLobbyEntry *entry = find(game, uid);
    if (!entry || entry->team == team) return;
    entry->team = (int8_t) team;
    broadcast(game, entry);
}

const char *comp_lobby_spawn_block(const Game *game, const int uid) {
    if (!game || !game->config.comp) return NULL;
    const CompLobbyEntry *entry = find(game, uid);
    if (!entry) return "Missing Sync Data";                    // ranked.sync
    if (!entry->team) return "Please join a Team to Spawn";     // ranked.team.spawn
    if (entry->class_index < 0) return "Please select a Class to Spawn"; // ranked.class.spawn
    return NULL;
}

void comp_lobby_apply(Game *game, const NetCompLobbyPacket *packet) {
    if (!game || !packet) return;
    if (packet->op == NET_COMP_LOBBY_CLEAR) {
        if (game->comp_lobby) game->comp_lobby->count = 0;
        return;
    }
    CompLobbyEntry *entry = find(game, packet->entry.uid);
    if (packet->op == NET_COMP_LOBBY_REMOVE) { // io_syncPConObj(id, null): delete vO367[id]
        if (!entry) return;
        CompLobby *l = game->comp_lobby;
        *entry = l->entries[--l->count];
        return;
    }
    if (!entry) {
        CompLobby *l = lobby(game);
        if (!l || l->count >= COMP_LOBBY_MAX) return;
        entry = &l->entries[l->count++];
    }
    *entry = packet->entry;
    entry->name[sizeof(entry->name) - 1] = 0;
    entry->clan[sizeof(entry->clan) - 1] = 0;
}

void comp_lobby_free(Game *game) {
    if (!game) return;
    free(game->comp_lobby);
    game->comp_lobby = NULL;
}
