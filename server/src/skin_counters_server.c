// v10.0.4 counter skins on the server (server/include/server_skin_counters.h).
#include <server_skin_counters.h>
#include <server_log.h>
#include <net_ext.h>
#include <ruleset.h> // rules_weapons: a weapon's melee flag without a game table
#include <shared.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// store.skins' counter skins (attach type 1): few (four in v10.0.4), so a short list
#define SC_TABLE_MAX 64
static struct {
    int skin, stat, weapon; // weapon: the weapon id the skin is for (skin.weapon - 1), -1 any (a melee skin)
} g_table[SC_TABLE_MAX];
static int g_table_count, g_table_loaded;

static void sc_table_load(void) {
    if (g_table_loaded) return;
    g_table_loaded = 1;
    char *path = concat(assets_path(), "skins.json");
    unsigned char *buffer = NULL;
    size_t length = 0;
    if (!path || read_file(path, &buffer, &length)) {
        server_log_error("[counters] cannot read %s: no counter skins", path ? path : "skins.json");
        free(path);
        return;
    }
    free(path);
    cJSON *json = cJSON_ParseWithLength((const char *) buffer, length);
    free(buffer);
    int index = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, json) {
        const cJSON *attach = cJSON_IsObject(item) ? cJSON_GetObjectItemCaseSensitive(item, "attach") : NULL;
        const cJSON *type = cJSON_IsObject(attach) ? cJSON_GetObjectItemCaseSensitive(attach, "type") : NULL;
        if (cJSON_IsNumber(type) && type->valueint == 1 && g_table_count < SC_TABLE_MAX) {
            // iíìîííî (deob-10.0.4.js:106491-106493): attach.stat when it is one of the eight, else "kills"
            const cJSON *stat = cJSON_GetObjectItemCaseSensitive(attach, "stat");
            const int s = cJSON_IsString(stat) ? skin_stat_index(stat->valuestring) : -1;
            const cJSON *weapon = cJSON_GetObjectItemCaseSensitive(item, "weapon");
            g_table[g_table_count].skin = index;
            g_table[g_table_count].stat = s >= 0 ? s : skin_stat_index("kills");
            g_table[g_table_count].weapon = cJSON_IsNumber(weapon) && weapon->valueint > 0 ? weapon->valueint - 1 : -1;
            g_table_count++;
        }
        index++;
    }
    cJSON_Delete(json);
}

static int sc_table_find(const int skin) {
    sc_table_load();
    for (int i = 0; i < g_table_count; i++)
        if (g_table[i].skin == skin) return i;
    return -1;
}

int skin_counters_stat_of(const int skin) {
    const int t = sc_table_find(skin);
    return t >= 0 ? g_table[t].stat : -1;
}

// m (the object, or its JSON string) of an owned entry: its `stat` count, -1 when it has none
static int64_t sc_entry_stat(const cJSON *entry, const char *stat) {
    const cJSON *m = cJSON_IsObject(entry) ? cJSON_GetObjectItemCaseSensitive(entry, "m") : NULL;
    cJSON *parsed = cJSON_IsString(m) && m->valuestring ? cJSON_Parse(m->valuestring) : NULL;
    if (parsed) m = parsed;
    const cJSON *v = cJSON_IsObject(m) ? cJSON_GetObjectItemCaseSensitive(m, stat) : NULL;
    const int64_t n = cJSON_IsNumber(v) && v->valuedouble >= 0 ? (int64_t) v->valuedouble : -1;
    cJSON_Delete(parsed);
    return n;
}

static ServerSkinCounter *sc_find(ClientConnection *client, const int skin) {
    for (uint16_t i = 0; client && i < client->acct.skin_counter_count; i++)
        if (client->acct.skin_counters[i].skin == skin) return &client->acct.skin_counters[i];
    return NULL;
}

// iîîíïiï (deob-10.0.4.js:24344-24353) of an owned entry: m.seed, else its instance id; 0 for a stack
static int32_t sc_entry_seed(const cJSON *entry) {
    const cJSON *m = cJSON_IsObject(entry) ? cJSON_GetObjectItemCaseSensitive(entry, "m") : NULL;
    cJSON *parsed = cJSON_IsString(m) && m->valuestring ? cJSON_Parse(m->valuestring) : NULL;
    if (parsed) m = parsed;
    const cJSON *seed = cJSON_IsObject(m) ? cJSON_GetObjectItemCaseSensitive(m, "seed") : NULL;
    const cJSON *id = cJSON_IsObject(entry) ? cJSON_GetObjectItemCaseSensitive(entry, "id") : NULL;
    const int32_t s = skin_copy_seed(cJSON_IsNumber(id) ? (int64_t) id->valuedouble : 0, cJSON_IsNumber(seed), cJSON_IsNumber(seed) ? seed->valuedouble : 0.0);
    cJSON_Delete(parsed);
    return s;
}

static void sc_note_seed(ClientConnection *client, const int skin, const int32_t seed) {
    if (!seed) return;
    for (uint16_t i = 0; i < client->acct.skin_seed_count; i++)
        if (client->acct.skin_seeds[i].skin == skin) return; // the first copy of the index keeps it
    if (client->acct.skin_seed_count >= SERVER_SKIN_PAINT_MAX) return;
    client->acct.skin_seeds[client->acct.skin_seed_count].skin = skin;
    client->acct.skin_seeds[client->acct.skin_seed_count].seed = seed;
    client->acct.skin_seed_count++;
}

void skin_counters_load(ClientConnection *client, const cJSON *skins) {
    if (!client) return;
    client->acct.skin_counter_count = 0;
    client->acct.skin_seed_count = 0;
    static const char *const stats[SKIN_STAT_COUNT] = SKIN_STATS;
    const cJSON *e;
    cJSON_ArrayForEach(e, skins) {
        const cJSON *ind = cJSON_IsObject(e) ? cJSON_GetObjectItemCaseSensitive(e, "ind") : e;
        const cJSON *cnt = cJSON_IsObject(e) ? cJSON_GetObjectItemCaseSensitive(e, "cnt") : NULL;
        if (!cJSON_IsNumber(ind) || (cnt && cJSON_IsNumber(cnt) && cnt->valueint <= 0)) continue;
        sc_note_seed(client, ind->valueint, sc_entry_seed(e));
        const int t = sc_table_find(ind->valueint);
        if (t < 0) continue;
        ServerSkinCounter *c = sc_find(client, ind->valueint);
        const int64_t n = sc_entry_stat(e, stats[g_table[t].stat]);
        if (c) { // a later entry of the same index (a copy): the first entry with a count keeps it
            if (c->base <= 0 && n > 0) c->base = n;
            continue;
        }
        if (client->acct.skin_counter_count >= SERVER_SKIN_COUNTER_MAX) continue;
        client->acct.skin_counters[client->acct.skin_counter_count++] = (ServerSkinCounter) {ind->valueint, g_table[t].stat, n > 0 ? n : 0, 0};
    }
}

// The weapon id worn skin slot `k` (PlayerSkins order) is on: loadout[0] / loadout[1], the melee weapon for 2; -1 none.
static int sc_slot_weapon(const Game *game, const Player *player, const int k) {
    if (!player->loadout || player->loadout_size <= 0) return -1;
    const size_t count = game->weapon_count ? game->weapon_count : rules_weapon_count();
    const Weapon *const *weapons = game->weapons ? (const Weapon *const *) game->weapons : (const Weapon *const *) rules_weapons();
    if (k == 2) {
        for (int i = 0; i < player->loadout_size; i++) {
            const int w = player->loadout[i];
            if (w >= 0 && (size_t) w < count && weapons[w] && weapons[w]->melee) return w;
        }
        return -1;
    }
    return k < player->loadout_size ? player->loadout[k] : -1;
}

void skin_counters_bases(const ClientConnection *owner, const PlayerSkins *skins, int32_t base[3], int32_t seed[3]) {
    base[0] = base[1] = base[2] = -1;
    if (seed) seed[0] = seed[1] = seed[2] = 0;
    if (!owner || !skins) return;
    const int32_t worn[3] = {skins->primary, skins->secondary, skins->melee};
    for (int k = 0; k < 3; k++) {
        if (worn[k] < 0) continue;
        for (uint16_t i = 0; i < owner->acct.skin_counter_count; i++) {
            const ServerSkinCounter *c = &owner->acct.skin_counters[i];
            if (c->skin == worn[k]) base[k] = (int32_t) (c->base > 0x7fffffff ? 0x7fffffff : c->base);
        }
        for (uint16_t i = 0; seed && i < owner->acct.skin_seed_count; i++)
            if (owner->acct.skin_seeds[i].skin == worn[k]) seed[k] = owner->acct.skin_seeds[i].seed;
    }
}

void skin_counters_send(ClientConnection *to, const ClientConnection *owner, const Player *player) {
    if (!to || !player || to->peer_minor < NET_PROTOCOL_MINOR_EXT) return;
    NetSkinCopiesPacket packet = {.uid = player->uid};
    skin_counters_bases(owner, &player->skins, packet.base, packet.seed);
    int any = 0;
    for (int k = 0; k < 3; k++) any |= packet.base[k] >= 0 || packet.seed[k] > 0;
    if (!any) return;
    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(packet)];
    const uint16_t len = net_ext_pack(buf, sizeof(buf), NET_EXT_KN_SKIN_COPIES, &packet, sizeof(packet));
    if (len) net_send_packet(to->fd, NET_PACKET_EXT, buf, len);
}

cJSON *skin_counters_take(ClientConnection *client, const int save) {
    if (!client) return NULL;
    static const char *const stats[SKIN_STAT_COUNT] = SKIN_STATS;
    cJSON *rows = NULL;
    for (uint16_t i = 0; i < client->acct.skin_counter_count; i++) {
        ServerSkinCounter *c = &client->acct.skin_counters[i];
        if (c->pending <= 0) continue;
        if (save) {
            if (!rows) rows = cJSON_CreateArray();
            cJSON *row = cJSON_CreateObject();
            cJSON_AddNumberToObject(row, "skin", c->skin);
            cJSON_AddStringToObject(row, "stat", stats[c->stat]);
            cJSON_AddNumberToObject(row, "count", (double) c->pending);
            cJSON_AddItemToArray(rows, row);
            c->base += c->pending; // the account's copy: m[stat] += count (:191579)
        }
        c->pending = 0;
    }
    return rows;
}

static ClientConnection *sc_client_of(const Player *player) {
    for (size_t i = 0; player && i < g_server->client_count; i++)
        if (g_server->clients[i].player == player) return &g_server->clients[i];
    return NULL;
}

// countSkinStat, the server branch (deob-10.0.4.js:175891-175893): the counter skin worn on that weapon counting that stat
// adds `amount`; weapon -1 (trackStat's loop, :184308-184311) is every worn counter of the stat, each sent to the owner
// as SKIN_STAT [weapon, stat, amount] (the client's countSkinStat, :221919-221923).
static void sc_count(Game *game, Player *player, const int weapon_id, const char *stat, const int amount) {
    ClientConnection *client = sc_client_of(player);
    const int s = skin_stat_index(stat);
    if (!client || client->acct.account_id <= 0 || !client->acct.skin_counter_count || s < 0) return;
    const int32_t worn[3] = {player->skins.primary, player->skins.secondary, player->skins.melee};
    for (int k = 0; k < 3; k++) {
        if (worn[k] < 0) continue;
        const int w = sc_slot_weapon(game, player, k);
        if (w < 0 || (weapon_id >= 0 && w != weapon_id)) continue;
        const int t = sc_table_find(worn[k]);
        if (t < 0 || (k < 2 && g_table[t].weapon >= 0 && g_table[t].weapon != w)) continue; // not that weapon's skin
        ServerSkinCounter *c = sc_find(client, worn[k]);
        if (!c || c->stat != s) continue;
        c->pending += amount;
        if (weapon_id < 0 && client->peer_minor >= NET_PROTOCOL_MINOR_EXT) {
            const NetSkinStatPacket packet = {.weapon = w, .amount = amount, .stat = (uint8_t) s};
            uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(packet)];
            const uint16_t len = net_ext_pack(buf, sizeof(buf), NET_EXT_KN_SKIN_STAT, &packet, sizeof(packet));
            if (len) net_send_packet(client->fd, NET_PACKET_EXT, buf, len);
        }
    }
}

void skin_counters_install_game_hooks(Game *game) {
    if (game) game->hooks.skin_stat = sc_count;
}
