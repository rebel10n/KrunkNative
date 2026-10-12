#pragma once
// WP-C (doc/v7/map-logic.md §3, §7 WP-C): the v7.1.4 trigger runtime API: event dispatch, the
// delayed-action queue, custom / global values, player looping sounds and the KrunkScript hooks
// (§3.10). The data model (TriggerData.actions, TriggerActionData) is in triggers.h and parsed by
// map.c; the tables (events 0-31, conditions 0-11, actions 0-71) are trigger_v7_data.h.
// Bodies: shared/src/triggers_v7.c (onTrigger, dispatch, queue, values), triggers_conditions.c,
// triggers_actions.c. Everything is authority-only (the server and the offline sandbox server);
// on a client mirror the calls are no-ops and the results arrive as packets.
#include <shared.h>
#include <trigger_v7_data.h>

struct GameAI_t;

// game.js objectTrigger(eventName, obj, player): one object's trigger for a v7 event index (0..31),
// when the object is active and listens to that event.
void trigger_object_event(Game*, Object *object, int event, Player *player);
// playerTrigger(eventName, player): every active trigger listening to an onPlayer* event (17, 18, 19, 28).
void trigger_player_event(Game*, int event, Player *player);
// multiObjectTrigger / findTriggers(eventName, player): every active trigger with that event
// (onGameStart 26, onNukeStart / onNukeEnd 21 / 22, onObjectiveSwitch 27).
void trigger_multi_event(Game*, int event, Player *player);
// game.onTrigger(player, ai) for an AI with Can Trigger (obj.isAI: no teamCheck, no self
// exclusion of targets): onAIDeath 15, onAIDamage 20, onAIHalfHealth 29, onAIKillPlayer 31. The
// caller gates on the AI's event (ai->data.trig.event); this fires unconditionally.
void trigger_ai_event(Game*, struct GameAI_t *ai, int event, Player *player);
#define TRIGGER_HAVE_AI_EVENT 1
// game.update #5-6: the timerObjs countdown and the triggerQueue of delayed actions (authority).
void trigger_queue_update(Game*, float delta);
// MapManager.resetAll trigger state + globalVals / triggerQueue / playerSounds at init (round start):
// storage, triggered, trigT, uses, object custVals, the rsc spawn chance, pickups (resetPickups).
void trigger_round_reset(Game*);
// The late-join part of getSyncData the trigger runtime owns: "gbl" (global values), "asnd"
// (player looping sounds), "pkups" (moved / emptied pickups). Called by game_send_object_state.
void trigger_send_join_state(Game*, int uid);
// A player left: its looping sounds stop (removePlayerSound(sid, null, true)) and queued actions
// lose their player.
void trigger_player_left(Game*, Player*);

// ---- KrunkScript interface (§3.10). NULL hooks are no-ops.
typedef struct TriggerScriptHooks {
    void (*on_custom_trigger)(Game *game, const char *player_id, const char *param, int value); // action 58
    void (*on_player_spawn)(Game *game, int player_sid);
    void (*on_player_damage)(Game *game, int victim_sid, int doer_sid, int amount);
    void (*on_player_death)(Game *game, int victim_sid, int killer_sid);
} TriggerScriptHooks;
// Stored in Game.script_hooks (NULL clears). Action 58 calls on_custom_trigger(game, the player's
// decimal uid or "NONE", tct ("" none), tc) after its tad delay, on the authority only. The
// player hooks are KrunkScript's own (ks_game_on_player_* in player.c), not called from here.
void triggers_set_script_hooks(Game *game, const TriggerScriptHooks *hooks);

// custVals (per player, <= TRIGGER_V7_CUSTOM_VALUE_LIMIT + 1 keys, never reset) and globalVals
// (per game, reset at init). The setters send "cv" / "gv" (packet block NET_PACKET_BLOCK_TRIGGERS)
// on the authority; a missing key reads 0. The setters refuse a new key on a full set.
double trigger_global_value_get(const Game *game, const char *key);
void trigger_global_value_set(Game *game, const char *key, double value);
double player_custom_value_get(const Player *player, const char *key);
void player_custom_value_set(Game *game, Player *player, const char *key, double value);
const TriggerValueSet *trigger_global_values(const Game *game); // NULL before the first value
// The per-round custVals of a map object (interface trigger values), NULL none.
const TriggerValueSet *trigger_object_values(const Game *game, const Object *object);
// Client mirrors (no packets): "gv" / the "gbl" join list (reset clears first) and "cv".
void trigger_mirror_global_value(Game *game, const char *key, double value, int reset);
void trigger_mirror_custom_value(Player *player, const char *key, double value);

// game.addPlayerSound / removePlayerSound (looping sounds that follow a player: Juggernaut,
// KrunkScript). Authority: kept for joiners ("asnd" in the sync data) and broadcast.
void game_add_player_sound(Game*, Player*, const char *name, float volume, float rate, int loop);
void game_remove_player_sound(Game*, Player*, const char *name, int force); // name NULL + force: all of them
