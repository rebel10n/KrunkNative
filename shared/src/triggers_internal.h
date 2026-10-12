#pragma once
// WP-C internals of the v7.1.4 trigger runtime, shared by shared/src/triggers.c (destructibles,
// hits, per-tick objects), triggers_v7.c (onTrigger, dispatch, queue, values), triggers_actions.c,
// triggers_conditions.c and interact.c. Not a public header.
#include <triggers_v7.h>
#include <account.h>
#include <ai.h>
#include <map_ai.h>

// The trigger object an event / action runs for (v7 `obj` / `j`): a map object or an AI (j.isAI).
typedef struct {
    Object *object;            // NULL for an AI
    struct GameAI_t *ai;       // NULL for a map object
    TriggerData *trig;
    int uid;                   // j.uid (map object index) / j.sid (AI)
    int team;                  // j.team (AIs: 0, no teamCheck)
    int interface_id;          // j.interface
    int interface_target;      // j.interfaceT
    vec3 position;             // j.x / j.y / j.z: an object's centre, an AI's feet
} TriggerSource;

// One triggerQueue entry {action, player, obj, chain, timer}: the action is copied (its condition
// was checked when queued and is not checked again, v7 quirk); players / AIs are looked up again.
typedef struct {
    TriggerActionData action;
    int player_uid;            // -1 none
    int object_uid;            // -1: an AI source
    int ai_sid;                // the AI source's sid (object_uid -1)
    int chain;
    float timer;               // ms
} TriggerQueued;

typedef struct {
    int uid;
    char name[NET_SOUND_NAME_SIZE];
    float volume, rate;
    unsigned char loop;
} TriggerPlayerSound;

#define TRIGGER_CHAIN_AI (-2)  // the chain key of AI sources (their v7 obj.uid is undefined: one shared key)

typedef struct TriggerRuntime_t {
    const Map *map;                  // the map the per-object arrays below were sized for
    size_t object_count;
    TriggerValueSet global_values;   // game.globalVals
    TriggerValueSet **object_values; // per uid: obj.custVals (reset each round), NULL none yet
    int *chains;                     // triggerChains per uid (object_count + 1: the AI key last)
    TriggerQueued *queue;            // game.triggerQueue
    size_t queue_count, queue_capacity;
    TriggerPlayerSound *sounds;      // game.playerSounds
    size_t sound_count, sound_capacity;
    GamePickup *pickups;             // per uid; pickup objects only (others zeroed)
    unsigned char *pickup_moved;     // per uid: away from its origin (a drop): join sync
} TriggerRuntime;

TriggerRuntime *trigger_runtime(Game*);           // allocated / re-sized to game->map on demand
TriggerValueSet *trigger_object_values_mut(Game*, const Object*, int create);

int trigger_map_uid(const Game*, const Object*);  // the object's index in Map.objects, -1 none
int trigger_team_check(const Game*, int team, const Player*); // game.teamCheck(obj, player)
int trigger_rand_int(int min, int max);           // UTILS.randInt (inclusive)
int trigger_wait_blocks(const Game*);             // game.waitTimers && !waitTimers[0].contTime
int trigger_is_interface(const Object*);          // map.manager.interfaces (in && !noCol)
// Event flags of an object (obj[eventN]): a TRIGGER's v7 event, -1 none.
static inline int trigger_event_of(const Object *o) { return o && o->trig ? o->trig->event : TRIGGER_EVENT_NONE; }
static inline int trigger_has_event(const Object *o, const int event) { return trigger_event_of(o) == event; }

void trigger_source_object(TriggerSource*, Object*);
void trigger_source_ai(TriggerSource*, struct GameAI_t*);
// game.onTrigger(player, obj, chain): chain -1 starts one keyed by the source.
void trigger_run(Game*, Player*, const TriggerSource*, int chain);
int trigger_chain_left(Game*, int chain);         // triggerChains[chain] > 0
// getActionValue(player, obj, action, "number", def): 1 and *out, 0 = undefined (skip the action).
int trigger_action_value(Game*, Player*, const TriggerSource *obj, const Object *target, const TriggerActionData*, int def, int *out);

// conditions.check(game, io, player, obj, action) (shared/src/triggers_conditions.c)
int trigger_condition_check(Game*, Player*, const TriggerSource*, const TriggerActionData*);
// actions[ta].execute(game, io, player, obj, action, chain) (shared/src/triggers_actions.c)
void trigger_action_execute(Game*, Player*, const TriggerSource*, const TriggerActionData*, int chain);

// Packets (shared/src/triggers_v7.c)
void trigger_send_object_state(Game*, int uid, int kind, int value);
void trigger_send_change_position(Game*, const Player*, int recon, int stop, int has_dir, int dir);

// Accounts (GameHooks.account_info; NULL = guest)
int trigger_account(const Game*, const Player*, AccountInfo *out);

// Pickups (shared/src/interact.c)
void interact_reset_pickups(Game*);
void interact_send_pickups(Game*, int uid);
