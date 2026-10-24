#pragma once
// Krunker v10.0.3 event kill pools (doc/v10/kill-pools.md): during a live event, a public match pays its
// players items per kill. v10's game server holds `ongoingEvents` (set by its host from the events
// API; the bundle only reads it) and, at endGame, draws getKillReward() once per kill, at most 30 times,
// for every player with a (non hack-flagged) account (deob-10.0.3.js:221797-221807). The items go to the
// player in the END's otherData.items (:221896-221900: the Collect Rewards button and its overlay,
// :236769-236823) and into the account in updateAccounts (onUpdateAccounts {skins: [{id, quantity}]},
// :217260-217282).
//
// An event (the fields v10 reads): startsAt (the draw order), killPools [{weight, contents
// [{content_type, content_ref}]}], killPoolsWeight (0..100, the event's share of a draw), killPoolsMode
// ("dilute": the earlier events' shares shrink by (100 - weight)%; anything else adds), killPoolsIcon
// (the item's `r`: ./img/<icon>.webp on the Collect button). Only "skin" contents pay; "currency" and
// others draw nothing (v10 returns null for them).
//
// KrunkNative: kn_brain serves the ongoing events to the game servers (services/brain/community.c,
// the feed's "gameEvents"; "welcome" and "game-events" frames); the server draws at the END
// (server/src/accounts.c), sends NET_EXT_KN_END_ITEM per item and commits them with the stats.
#include <cJSON.h>
#include <stdint.h>

#define KILL_POOL_EVENTS_MAX 8
#define KILL_POOL_POOLS_MAX 8     // per event
#define KILL_POOL_CONTENTS_MAX 32 // per pool
#define KILL_POOL_ICON_SIZE 28
#define KILL_POOL_DRAWS_MAX 30    // Math.min(player.kills, 30) (deob-10.0.3.js:221801)

enum { KILL_POOL_CONTENT_OTHER = 0, KILL_POOL_CONTENT_SKIN = 1 };

typedef struct {
    int type;   // KILL_POOL_CONTENT_*: content_type "skin", or anything else (currency: no reward)
    int ref;    // parseInt(content_ref): a store skin index (< 0 or NaN: no reward)
} KillPoolContent;

typedef struct {
    double weight;    // pool.weight || 0
    int content_count;
    KillPoolContent contents[KILL_POOL_CONTENTS_MAX];
} KillPool;

typedef struct {
    int id;               // the event's id: the eventIndex of a public round it runs in (updateEventIndex)
    double starts_at_ms;  // startsAt.getTime()
    double ends_at_ms;    // endsAt.getTime() (KrunkNative's feed; v10's config.events end), 0 = open-ended
    double weight;        // killPoolsWeight || 0 (a percentage)
    int dilute;           // killPoolsMode === "dilute"
    char icon[KILL_POOL_ICON_SIZE]; // killPoolsIcon, "" none (the item has no `r`)
    int pool_count;       // killPools.length (pools past KILL_POOL_POOLS_MAX are dropped)
    KillPool pools[KILL_POOL_POOLS_MAX];
} KillPoolEvent;

typedef struct {
    int count;
    KillPoolEvent events[KILL_POOL_EVENTS_MAX];
} KillPoolEvents;

// getKillReward's answer: {t: "skin", i, r?}
typedef struct {
    int skin;
    char icon[KILL_POOL_ICON_SIZE]; // r: the event's killPoolsIcon, "" when it has none
} KillReward;

// A uniform double in [0, 1) (Math.random); NULL = pcg32
typedef double (*KillPoolRandom)(void *user);

// new Date(v).getTime() of a feed date: epoch ms, or an ISO 8601 UTC date ("2026-10-24T18:00:00Z", ".sss" and a
// space for the T allowed); unreadable or absent: 0 (also live_events.c's start / end)
double kill_pools_time(const cJSON *v);
// The events of a JSON array in v10's field names (startsAt: an ISO date or epoch ms). Returns the count.
int kill_pools_parse(KillPoolEvents *out, const cJSON *events);
// ongoingEvents.some(e => e.killPools.length > 0): whether endGame draws at all
int kill_pools_any(const KillPoolEvents *events);
// game.updateEventIndex's search (deob-10.0.3.js:219733-219751): the id of the first event (list order) with
// start <= now <= end, 0 none. The caller applies the public-round rule (no host, not custom / official / comp).
int kill_pools_event_index(const KillPoolEvents *events, double now_ms);
// getKillReward (deob-10.0.3.js:220807-220924): 1 and *out when this draw pays an item, 0 for none
int kill_pools_draw(const KillPoolEvents *events, KillPoolRandom random, void *user, KillReward *out);
// endGame's loop: min(kills, KILL_POOL_DRAWS_MAX) draws into out[] (at most max); returns the items drawn
int kill_pools_draw_kills(const KillPoolEvents *events, unsigned int kills, KillPoolRandom random, void *user, KillReward *out, int max);
