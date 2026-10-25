#pragma once
// Krunker v10.0.3 live events: config.events (deob-10.0.3.js:22364, exported as config.events at :22553) and what
// the game does with them (doc/v10/events.md). 10.0.3's own list is empty (the bundle ships `[]`); its readers
// name the fields an event has:
//
//   id             the round's eventIndex (updateEventIndex :219733-219751), the `event_<id>` image and challenges
//   name           the popup's title, the challenge card / category names (:250849-250877, :260291)
//   start, end     dates: updateEventIndex and hasActiveEvents (:28747-28757) take now in [start, end]
//   reveal         ms: getActiveEvent (:28758-28775) also answers an event starting within `reveal` that has not ended
//   imgExt         img/events/event_<id>.<imgExt || "png"> (:250877, :260291)
//   longDesc       the popup's text (:260317); noChallenges: "playing the event" and no View Challenges button
//   rewards        {skins: the popup's 12 tiles (:260296-260307), weights: endGame's rarity weights (:221745)}
//   rewardChance   endGame's roll, `|| 100` (:221740); eligibleMaps: the map indexes it pays on (:221737)
//   limitedTimeMode  genMatchList puts the event's mode first in the vote list (:221830-221832, :222459-222463)
//
// KrunkNative's events are kn_brain's feed ("gameEvents", services/brain/community.c) in these names, with the
// kill pools' startsAt / endsAt (shared/include/kill_pools.h) for start / end (both spellings are read). One KN
// field: eventObjects (bool), the EVENT prefab's meshes show during this event too (v10 shows them for event 3
// only, :256309-256313; that rule stays).
#include <cJSON.h>
#include <shared.h> // Map, Object

#define LIVE_EVENTS_MAX 8
#define LIVE_EVENT_NAME_SIZE 64
#define LIVE_EVENT_DESC_SIZE 640
#define LIVE_EVENT_EXT_SIZE 8
#define LIVE_EVENT_SKINS_MAX 32
#define LIVE_EVENT_MAPS_MAX 32
#define LIVE_EVENT_RARITIES 7       // store.rarities (Common .. Unobtainable)
#define LIVE_EVENT_POPUP_SKINS 12   // eventPopup's tiles (deob-10.0.3.js:260298)
#define LIVE_EVENT_EGG_EVENT 3      // the event whose EVENT prefab meshes v10 shows (deob-10.0.3.js:256309)

typedef struct {
    int id;
    char name[LIVE_EVENT_NAME_SIZE];
    double start_ms, end_ms;        // new Date(start / end).getTime(); end 0 = open-ended (KrunkNative's feed)
    double reveal_ms;               // reveal, 0 = none
    char img_ext[LIVE_EVENT_EXT_SIZE]; // imgExt (letters and digits only), "" = "png"
    // longDesc as text: v10 writes it as markup; KrunkNative's feed never carries markup to the clients, so it is
    // a string, or a list of lines the popup shows as <li> items (eventPopInfo > li, main.css). Lines are '\n'-joined.
    char long_desc[LIVE_EVENT_DESC_SIZE];
    unsigned char long_desc_list;   // long_desc came as a list
    unsigned char no_challenges;    // noChallenges
    unsigned char limited_time_mode;
    unsigned char event_objects;    // KN eventObjects: the EVENT prefab shows (always for event 3)
    unsigned char has_reward_skins; // rewards.skins given (the popup's "Earn these skins" block, even empty)
    unsigned char has_weights;      // rewards.weights given (else store.lootRars)
    unsigned char has_eligible_maps;
    int reward_skin_count;          // rewards.skins (store indexes, kept as given: a bad one is an empty tile)
    int reward_skins[LIVE_EVENT_SKINS_MAX];
    double reward_weights[LIVE_EVENT_RARITIES];
    int reward_chance;              // rewardChance || 100 (a percentage)
    int eligible_map_count;
    int eligible_maps[LIVE_EVENT_MAPS_MAX]; // eligibleMaps: official map indexes
} LiveEvent;

typedef struct {
    int count;
    LiveEvent events[LIVE_EVENTS_MAX];
} LiveEvents;

// A store skin as endGame's event reward reads it (store.skins[i]: index, rarity, eventReward, seas)
typedef struct {
    int index;
    int rarity;
    int event_reward; // eventReward, -1 none
    int season;       // seas, -1 none
} LiveEventSkin;

typedef double (*LiveEventRandom)(void *user);   // Math.random(), [0, 1); NULL = pcg32
typedef int (*LiveEventOwns)(void *user, int skin); // account.hasSkin(index)

// The events of a JSON array (the feed's "gameEvents", the brain's welcome / push / GET /data/events): up to
// LIVE_EVENTS_MAX, in order. Returns the count.
int live_events_parse(LiveEvents *out, const cJSON *events);
// config.events.find(e => e.id == id): NULL none (id 0 never matches)
const LiveEvent *live_events_find(const LiveEvents *events, int id);
// updateEventIndex's loop (deob-10.0.3.js:219737-219745): the id of the first event with start <= now <= end, 0 none.
// The caller applies the public-round rule and forceEvent.
int live_events_index(const LiveEvents *events, double now_ms);
// UTILS.hasActiveEvents (deob-10.0.3.js:28747-28757): an event runs now
int live_events_any_running(const LiveEvents *events, double now_ms);
// UTILS.getActiveEvent(id) (deob-10.0.3.js:28758-28775): the event `id`, else the first event that reveals early
// (reveal set, not ended, starting within reveal); NULL none
const LiveEvent *live_events_active(const LiveEvents *events, int id, double now_ms);
// The start handler's toggleMeshGroup("event_0", ...) (deob-10.0.3.js:256309-256313): the EVENT prefab's meshes show
// in this round (event 3, or a KrunkNative eventObjects event) unless the player spectates
int live_events_show_objects(const LiveEvents *events, int event_index, int spectating);
// eligibleMaps (deob-10.0.3.js:221737): none given = every map
int live_event_map_eligible(const LiveEvent *event, int map_index);
// img/events/event_<id>.<imgExt || "png"> (no leading slash)
void live_event_image(const LiveEvent *event, int id, char *out, size_t n);
// game.getItemByWeights(items, weights) (deob-10.0.3.js:220754-220806): a rarity drawn over the rarities the items
// have (weights[r] || 0; weights NULL = store.lootRars), then one item of it at random; -1 none. rarity_of(i)
// gives store.skins[i].rarity (-1: not a skin, skipped).
int live_events_pick_by_weights(const int *items, int count, const double *weights, int (*rarity_of)(void *user, int skin),
                                void *user, LiveEventRandom random, void *random_user);
// endGame's event reward (deob-10.0.3.js:221732-221752) for one account: the event of this round's eventIndex, its
// candidates the store skins with eventReward == eventIndex of this season on an eligible map, then a
// randInt(1, 100) <= rewardChance roll, the ones the account lacks, a pick by rewards.weights. -1 none.
// KrunkNative difference: the event's rewards.skins (the popup's list) are candidates too (an operator's feed
// cannot tag store skins with eventReward), each once.
int live_events_reward(const LiveEvent *event, int event_index, int map_index, int season, const LiveEventSkin *store,
                       int store_count, LiveEventOwns owns, void *owns_user, LiveEventRandom random, void *random_user);
// the limitedTimeMode of the event `event_index` (genMatchList's event pick, deob-10.0.3.js:221830-221832)
int live_events_limited_time_mode(const LiveEvents *events, int event_index);
// A custom game has no EVENT objects (map.generate skips them, deob-10.0.3.js:189737): KrunkNative keeps them in the
// map (the same uids on both ends) and makes them inactive (no collision, no shot, no mesh). Nothing for another game.
// Returns the EVENT objects of the map.
int live_events_map_objects(Map *map, int custom);
// The egg hunt (game.foundEventItem, deob-10.0.3.js:220700-220728): the event whose EVENT objects are eggs, and the
// eggs a hunt has (config.easterEggs, :19419, :22520)
#define LIVE_EVENT_EGGS_TOTAL 187
