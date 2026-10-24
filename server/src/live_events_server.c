// Krunker v10.0.3 live events on the game server (server/include/server_live_events.h, doc/v10/events.md).
//
// The events are the brain's (services/brain/community.c "gameEvents": the ones running now, in the welcome and
// the "game-events" pushes; accounts.c hands them here). They decide:
//   - the round's eventIndex: updateEventIndex at game.init (deob-10.0.3.js:219733-219751), sent after the INIT as
//     NET_EXT_INIT_EVENT (net.c), and Game.event_ltm, the event's limitedTimeMode (genMatchList's first pick,
//     :221830-221832, shared/src/game_config.c);
//   - endGame's event reward (:221732-221752): one draw per account in a public round.
// gameserver.force_event is v10's game.forceEvent (:219747-219750): the id every round of this server plays,
// whatever the round (custom, hosted, the offline sandbox: KRUNKNATIVE_EVENT). v10 clears it after one round (its
// host sets it again); KrunkNative keeps it for every round.
#include <server_live_events.h>
#include <server_config.h>
#include <server_log.h>
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
