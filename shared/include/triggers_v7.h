#pragma once
// WP-C (doc/v7/map-logic.md §3, §7 WP-C): the v7.1.4 trigger runtime API: event dispatch, the
// delayed-action queue, custom / global values and the KrunkScript hooks (§3.10). The data
// model (TriggerData.actions, TriggerActionData) is in triggers.h and parsed by map.c; the
// tables (events 0-31, conditions 0-11, actions 0-71) are trigger_v7_data.h.
// Declared by WP-A; the bodies in shared/src/triggers_v7.c are stubs until WP-C replaces them.
#include <shared.h>
#include <trigger_v7_data.h>

// game.js objectTrigger(eventName, obj, player): one object's trigger for a v7 event index (0..31).
void trigger_object_event(Game*, Object *object, int event, Player *player);
// playerTrigger(eventName, player): every trigger listening to an onPlayer* event (17, 18, 19, 28).
void trigger_player_event(Game*, int event, Player *player);
// multiObjectTrigger(eventName, player): every trigger with that event (onGameStart 26,
// onNukeStart / onNukeEnd 21 / 22, onObjectiveSwitch 27, the custom value events 24 / 25 / 30).
void trigger_multi_event(Game*, int event, Player *player);
// game.update #5-6: the timerObjs countdown and the triggerQueue of delayed actions (authority).
void trigger_queue_update(Game*, float delta);
// MapManager.resetAll trigger state + globalVals / triggerQueue at init (round start).
void trigger_round_reset(Game*);

// ---- KrunkScript interface (§3.10). NULL hooks are no-ops.
typedef struct TriggerScriptHooks {
    void (*on_custom_trigger)(Game *game, const char *player_id, const char *param, int value); // action 58
    void (*on_player_spawn)(Game *game, int player_sid);
    void (*on_player_damage)(Game *game, int victim_sid, int doer_sid, int amount);
    void (*on_player_death)(Game *game, int victim_sid, int killer_sid);
} TriggerScriptHooks;
// Stored in Game.script_hooks (NULL clears). Action 58 calls on_custom_trigger(game, the player's
// decimal uid or "NONE", tct ("" none), tc) after its tad delay, on the authority only.
void triggers_set_script_hooks(Game *game, const TriggerScriptHooks *hooks);

// custVals (per player, <= TRIGGER_V7_CUSTOM_VALUE_LIMIT keys, never reset) and globalVals
// (per game, reset at init). The setters send "cv" / "gv" (packet block NET_PACKET_BLOCK_TRIGGERS).
double trigger_global_value_get(const Game *game, const char *key);
void trigger_global_value_set(Game *game, const char *key, double value);
double player_custom_value_get(const Player *player, const char *key);
void player_custom_value_set(Game *game, Player *player, const char *key, double value);
