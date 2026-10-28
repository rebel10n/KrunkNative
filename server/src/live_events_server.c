// Krunker v10.0.3 live events on the game server (server/include/server_live_events.h, doc/v10/events.md).
//
// The events are the brain's (services/brain/community.c "gameEvents": the ones running now, in the welcome and
// the "game-events" pushes; accounts.c hands them here). They decide:
//   - the round's eventIndex: updateEventIndex at game.init (deob-10.0.3.js:219733-219751), sent after the INIT as
//     NET_EXT_INIT_EVENT (net.c), and Game.event_ltm, the event's limitedTimeMode (genMatchList's first pick,
//     :221830-221832, shared/src/game_config.c);
//   - endGame's event reward (:221732-221752): one draw per account in a public round;
//   - the egg hunt of event 3 (game.foundEventItem, :220700-220728): a shot, swing or damaging projectile on an EVENT
//     object (an egg) gives the finder a hit marker and, once a session per egg, asks the database whether the
//     account found it before; the answer's lines go to everyone ("event.easter.found.new" with the count of 187,
//     "event.easter.completed") or the finder ("event.easter.found.old") and moves the "eggs" challenges (+1, +10).
//     KrunkNative: kn_brain's "egg_found" op (services/brain/db_eggs.c, table event_eggs) stands in for proc 68.
// gameserver.force_event is v10's game.forceEvent (:219747-219750): the id every round of this server plays,
// whatever the round (custom, hosted, the offline sandbox: KRUNKNATIVE_EVENT). v10 clears it after one round (its
// host sets it again); KrunkNative keeps it for every round.
#include <server_live_events.h>
#include <server_config.h>
#include <server_log.h>
#include <server_accounts.h> // accounts_progress: the "eggs" challenge key
#include <brain.h>
#include <map_catalog.h>     // the egg key's official map index
#include <map_game_uids.h>   // the egg key's GameObject uid
#include <net_ext.h>
#include <pcg_basic.h>
#include <season.h> // SEASON_CURRENT: endGame's seas == currentSeason
#include <shared.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static LiveEvents g_events;

// store.skins as the event reward reads them (assets/skins.json: rarity, eventReward, seas), loaded the first time
// an event is set (not at the round's end: the file is 1.5 MB)
static LiveEventSkin *g_store;
static int g_store_count;
static int g_store_loaded;

static int ev_int(const cJSON *o, const char *key, const int fallback) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valueint : fallback;
}

static void ev_store_load(void) {
    if (g_store_loaded) return;
    g_store_loaded = 1;
    char *path = concat(assets_path(), "skins.json");
    unsigned char *buffer = NULL;
    size_t length = 0;
    if (!path || read_file(path, &buffer, &length)) {
        server_log_error("[events] cannot read %s: no event reward skins", path ? path : "skins.json");
        free(path);
        return;
    }
    free(path);
    cJSON *json = cJSON_ParseWithLength((const char *) buffer, length);
    free(buffer);
    const int n = cJSON_IsArray(json) ? cJSON_GetArraySize(json) : 0;
    g_store = n > 0 ? calloc((size_t) n, sizeof(*g_store)) : NULL;
    if (g_store) {
        const cJSON *item;
        cJSON_ArrayForEach(item, json) {
            LiveEventSkin *s = &g_store[g_store_count];
            s->index = g_store_count++;
            s->rarity = cJSON_IsObject(item) ? ev_int(item, "rarity", -1) : -1;
            s->event_reward = cJSON_IsObject(item) ? ev_int(item, "eventReward", -1) : -1;
            s->season = cJSON_IsObject(item) ? ev_int(item, "seas", -1) : -1;
        }
    }
    cJSON_Delete(json);
}

void server_events_set(const cJSON *events) {
    const int had = g_events.count;
    live_events_parse(&g_events, cJSON_IsArray(events) ? events : NULL);
    if (g_events.count) ev_store_load();
    if (!had && !g_events.count) return;
    char ids[96] = "";
    for (int i = 0; i < g_events.count; i++) {
        const size_t used = strlen(ids);
        snprintf(ids + used, sizeof(ids) - used, "%s%d%s", i ? "," : "", g_events.events[i].id,
                 g_events.events[i].limited_time_mode ? "(ltm)" : "");
    }
    server_log("[events] live events: %s", g_events.count ? ids : "none");
}

const LiveEvents *server_events(void) {
    return &g_events;
}

int server_events_round_index(const Game *game, const int custom) {
    int index = 0;
    // !hasHost() && !isCustom && !isOfficial && !isComp: KrunkNative's hosted lobbies and comp games are custom
    if (game && !custom && !game->config.comp && !game->is_official) index = live_events_index(&g_events, (double) time(NULL) * 1000.0);
    if (g_gs_config.force_event > 0) index = g_gs_config.force_event; // forceEvent
    return index;
}

typedef struct {
    const ClientConnection *client;
} EvOwner;

// account.hasSkin(index): the coordinator's inventory (skins_owned), nothing owned before it is known
static int ev_owns(void *user, const int skin) {
    const ClientConnection *c = ((const EvOwner *) user)->client;
    if (!c->acct.skins_known || skin < 0 || skin >= (int) sizeof(c->acct.skins_owned) * 8) return 0;
    return (c->acct.skins_owned[skin >> 3] >> (skin & 7)) & 1;
}

int server_events_reward(const ClientConnection *client, const int map_index) {
    const Game *game = &g_server->game;
    const LiveEvent *event = live_events_find(&g_events, game->event_index); // config.events.find(id == eventIndex)
    if (!client || !event || client->acct.account_id <= 0 || client->acct.hack) return -1;
    if (!g_store_loaded) ev_store_load();
    EvOwner owner = {client};
    const int skin = live_events_reward(event, game->event_index, map_index, SEASON_CURRENT, g_store, g_store_count, ev_owns, &owner,
                                        NULL, NULL);
    if (skin >= 0) server_log("[events] event %d reward: skin %d for account %d", event->id, skin, client->acct.account_id);
    return skin;
}

// ------------------------------------------------------------------ the egg hunt (event 3)

// The round's GameObject uids (the egg key is mapIndex-uid, :220705), its map's official index
static MapGameUids g_uids;
static int g_uids_map = -2;

// A map object with a skin reward ("rew", an AI_SPAWNER's v10.0.4 reward list)
static int ev_map_has_rewards(const cJSON *map) {
    const cJSON *object;
    cJSON_ArrayForEach(object, cJSON_GetObjectItemCaseSensitive(map, "objects")) {
        if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(object, "rew"))) return 1;
    }
    return 0;
}

void server_events_round(Game *game, const int custom) {
    // v10.0.4 AI skin rewards read the store's rarities: load them now, not when the first rewarding AI dies (1.5 MB)
    if (!g_store_loaded && game && game->current_map_index >= 0 && (size_t) game->current_map_index < game->map_count &&
        ev_map_has_rewards(game->maps[game->current_map_index]))
        ev_store_load();
    const int eggs = live_events_map_objects(game ? game->map : NULL, custom); // a custom game's are inert
    if (eggs) server_log("[events] %d EVENT object(s)%s", eggs, custom ? " (custom game: inert)" : "");
    map_game_uids_free(&g_uids);
    g_uids_map = -2;
    if (!eggs || !game || game->current_map_index < 0 || (size_t) game->current_map_index >= game->map_count) return;
    map_game_uids_build(&g_uids, game->maps[game->current_map_index], custom);
    g_uids_map = map_catalog_map_index_of_json(game->maps[game->current_map_index]);
    if (g_uids_map < 0) g_uids_map = game->current_map_index;
}

// player.tempEggs (:190247, :220706-220709): the eggs this connection found in this session
#define EGG_SESSIONS 64
#define EGG_KEYS 256
#define EGG_KEY_SIZE 24
static struct {
    int client_id, account_id;
    int count;
    char keys[EGG_KEYS][EGG_KEY_SIZE];
} g_temp[EGG_SESSIONS];

static int egg_seen(const int client_id, const int account_id, const char *key) {
    int slot = -1, free_slot = -1;
    for (int i = 0; i < EGG_SESSIONS; i++) {
        if (g_temp[i].client_id == client_id && g_temp[i].account_id == account_id && g_temp[i].account_id) slot = i;
        else if (!g_temp[i].account_id && free_slot < 0) free_slot = i;
    }
    if (slot < 0) {
        slot = free_slot >= 0 ? free_slot : (int) (pcg32_boundedrand(EGG_SESSIONS)); // a full table forgets one
        memset(&g_temp[slot], 0, sizeof(g_temp[slot]));
        g_temp[slot].client_id = client_id;
        g_temp[slot].account_id = account_id;
    }
    for (int i = 0; i < g_temp[slot].count; i++)
        if (!strcmp(g_temp[slot].keys[i], key)) return 1;
    if (g_temp[slot].count < EGG_KEYS) snprintf(g_temp[slot].keys[g_temp[slot].count++], EGG_KEY_SIZE, "%s", key);
    return 0;
}

static ClientConnection *ev_client(const int id) {
    for (size_t i = 0; g_server && i < g_server->client_count; i++)
        if (g_server->clients[i].id == id) return &g_server->clients[i];
    return NULL;
}

// "chi" [0, null, [key, ...args], 2, color] (ADD_CHATI18N): NET_EXT_KN_OFFICIAL_CHAT_I18N to `only` (or every EXT peer)
static void egg_line(const ClientConnection *only, const char *key, const int argc, const char *const *args) {
    static const char *const colors[] = {"#ffd45e", "#61fff7", "#ff63d8"}; // the egg lines' colours (:117031)
    uint8_t body[NET_EXT_BODY_MAX];
    NetOfficialChatI18nHeader h;
    memset(&h, 0, sizeof(h));
    h.kind = NET_CHAT_SERVER;
    h.argc = (uint8_t) argc;
    h.channel = 0;
    snprintf(h.color, sizeof(h.color), "%s", colors[pcg32_boundedrand(3)]);
    size_t len = sizeof(h);
    memcpy(body, &h, sizeof(h));
    const char *strings[2 + 4] = {"", key};
    for (int i = 0; i < argc && i < 4; i++) strings[2 + i] = args[i];
    for (int i = 0; i < 2 + argc && i < 6; i++) {
        const size_t n = strlen(strings[i]) + 1;
        if (len + n > sizeof(body)) return;
        memcpy(body + len, strings[i], n);
        len += n;
    }
    uint8_t packet[NET_EXT_HEADER_SIZE + NET_EXT_BODY_MAX];
    const uint16_t plen = net_ext_pack(packet, sizeof(packet), NET_EXT_KN_OFFICIAL_CHAT_I18N, body, (uint16_t) len);
    for (size_t i = 0; plen && g_server && i < g_server->client_count; i++) {
        const ClientConnection *c = &g_server->clients[i];
        if ((only && c != only) || c->peer_minor < NET_PROTOCOL_MINOR_EXT) continue;
        net_send_packet(c->fd, NET_PACKET_EXT, packet, plen);
    }
}

#define EGG_PENDING 32
static struct {
    uint32_t rid;
    int client_id, account_id;
} g_egg_pending[EGG_PENDING];

void server_events_forget_client(const int client_id) {
    for (int i = 0; i < EGG_SESSIONS; i++)
        if (g_temp[i].client_id == client_id) memset(&g_temp[i], 0, sizeof(g_temp[i]));
    for (int i = 0; i < EGG_PENDING; i++)
        if (g_egg_pending[i].client_id == client_id) memset(&g_egg_pending[i], 0, sizeof(g_egg_pending[i]));
}

static void ev_found_item(Game *game, Player *player, Object *object) {
    if (!game || !player || !object || game->event_index != LIVE_EVENT_EGG_EVENT) return; // eventIndex == 3
    if (!game_is_pub_type(game) && !player->dev) return;                                // isPubType() || account.isDev
    const NetHitPacket hit = {.target_uid = -1}; // !rare (never set in 10.0.3): send(DO_DAMAGE), a hit marker
    game_send(game, player->uid, NET_PACKET_HIT, &hit, sizeof(hit));
    const ClientConnection *client = ev_client(player->uid);
    if (!client || client->acct.account_id <= 0 || client->acct.hack) return; // !account
    int index = -1;
    for (size_t i = 0; game->map && i < game->map->object_count; i++)
        if (game->map->objects[i] == object) index = (int) i;
    char key[EGG_KEY_SIZE];
    snprintf(key, sizeof(key), "%d-%d", g_uids_map, map_game_uid_of_index(&g_uids, index)); // mapIndex + "-" + obj.uid
    if (egg_seen(client->id, client->acct.account_id, key)) return; // tempEggs
    if (!brain_linked()) return;
    int slot = -1;
    for (int i = 0; i < EGG_PENDING && slot < 0; i++)
        if (!g_egg_pending[i].rid) slot = i;
    if (slot < 0) return;
    const uint32_t rid = brain_next_rid();
    cJSON *frame = cJSON_CreateObject();
    cJSON_AddStringToObject(frame, "type", "acct");
    cJSON_AddNumberToObject(frame, "rid", rid);
    cJSON_AddStringToObject(frame, "op", "egg_found");
    cJSON *args = cJSON_AddObjectToObject(frame, "args");
    cJSON_AddNumberToObject(args, "id", client->acct.account_id);
    cJSON_AddStringToObject(args, "key", key);
    cJSON_AddNumberToObject(args, "total", LIVE_EVENT_EGGS_TOTAL);
    if (brain_post(frame, 0) != 0) return;
    g_egg_pending[slot].rid = rid;
    g_egg_pending[slot].client_id = client->id;
    g_egg_pending[slot].account_id = client->acct.account_id;
    server_log("[events] %s found egg %s", client->name, key);
}

int server_events_on_brain_frame(const cJSON *frame) {
    const cJSON *rid = cJSON_GetObjectItem(frame, "rid");
    if (!cJSON_IsNumber(rid)) return 0;
    int slot = -1;
    for (int i = 0; i < EGG_PENDING && slot < 0; i++)
        if (g_egg_pending[i].rid && g_egg_pending[i].rid == (uint32_t) rid->valuedouble) slot = i;
    if (slot < 0) return 0;
    g_egg_pending[slot].rid = 0;
    ClientConnection *client = ev_client(g_egg_pending[slot].client_id);
    if (!client || client->acct.account_id != g_egg_pending[slot].account_id) return 1; // gone
    const cJSON *err = cJSON_GetObjectItem(frame, "err");
    const cJSON *data = cJSON_GetObjectItem(frame, "data");
    const char *re = cJSON_GetStringValue(cJSON_GetObjectItem(data, "re"));
    const cJSON *cnt = cJSON_GetObjectItem(data, "cnt");
    const char *name = client->name;
    if ((cJSON_IsString(err) && err->valuestring[0]) || !cJSON_IsObject(data)) { // no row: "found.old" to the finder
        egg_line(client, "event.easter.found.old", 0, NULL);
        return 1;
    }
    if (re && !strcmp(re, "success")) { // the hunt's last egg
        const char *args[] = {name};
        egg_line(NULL, "event.easter.completed", 1, args);
        if (client->player) accounts_progress(client->player, 0, "eggs", 10, -1, -1, -1);
        server_log("[events] %s completed the egg hunt", name);
        return 1;
    }
    if (!cJSON_IsNumber(cnt) || cnt->valuedouble <= 0) { // found before
        egg_line(client, "event.easter.found.old", 0, NULL);
        return 1;
    }
    char count[16], total[16];
    snprintf(count, sizeof(count), "%d", cnt->valueint);
    snprintf(total, sizeof(total), "%d", LIVE_EVENT_EGGS_TOTAL);
    const char *args[] = {name, count, total};
    egg_line(NULL, "event.easter.found.new", 3, args);
    if (client->player) accounts_progress(client->player, 0, "eggs", 1, -1, -1, -1);
    server_log("[events] %s found %s of %s eggs", name, count, total);
    return 1;
}

int server_store_skin_rarity(const int skin) {
    if (!g_store_loaded) ev_store_load();
    return skin >= 0 && skin < g_store_count ? g_store[skin].rarity : -1;
}

static int ev_skin_rarity(const Game *game, const int skin) {
    (void) game;
    return server_store_skin_rarity(skin);
}

static int ev_has_skin(const Game *game, const Player *player, const int skin) {
    (void) game;
    return accounts_has_skin(player, skin);
}

void server_events_install_game_hooks(Game *game) {
    if (!game) return;
    game->hooks.event_item = ev_found_item;
    game->hooks.skin_rarity = ev_skin_rarity; // v10.0.4 AI rewards: getItemByChance's store.skins[i].rarity (shared/src/ai.c)
    game->hooks.has_skin = ev_has_skin;       // rewardUnique: account.hasSkin
}
