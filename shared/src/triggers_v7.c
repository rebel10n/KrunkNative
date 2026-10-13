// Krunker v7.1.4 trigger runtime (doc/v7/map-logic.md §3.8; WP-C): game.onTrigger with chains,
// chance, singleUse / singleUseP, action costs and methods, conditions and the triggerQueue of
// delayed actions (v7 data/game.js:4000-4136), the dispatchers playerTrigger / objectTrigger /
// multiObjectTrigger / findTriggers (:4137-4160, :1457), getActionValue (:4163-4218), the
// timerObjs loop (:4890-4907), globalVals, player / object custVals, playerSounds (:3955-4000)
// and the trigger part of MapManager.resetAll (libs/MapManager.js:3760-3800).
// Authority only (v7 game.isServer; the offline sandbox server is one). The conditions are in
// shared/src/triggers_conditions.c, the 72 actions in shared/src/triggers_actions.c.
#include <shared.h>
#include "triggers_internal.h"
#include <ks_storage_data.h> // WP6: Load Data values (parseFloat)
#include <math.h>
#include <pcg_basic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- value sets

const TriggerValue *trigger_values_find(const TriggerValueSet *set, const char *key) {
    if (!set || !key) return NULL;
    for (int i = 0; i < set->count; i++) {
        if (!strcmp(set->values[i].key, key)) return &set->values[i];
    }
    return NULL;
}

TriggerValue *trigger_values_slot(TriggerValueSet *set, const char *key) {
    if (!set || !key) return NULL;
    TriggerValue *found = (TriggerValue *) trigger_values_find(set, key);
    if (found) return found;
    // custVals[key] == null: `if (Object.keys(custVals).length > config.custValLimit) return;`
    if (set->count > TRIGGER_V7_CUSTOM_VALUE_LIMIT || set->count >= TRIGGER_VALUE_SLOTS) return NULL;
    TriggerValue *slot = &set->values[set->count++];
    snprintf(slot->key, sizeof(slot->key), "%s", key);
    slot->value = 0.0;
    return slot;
}

// ---------------------------------------------------------------- runtime state

static void runtime_free_objects(TriggerRuntime *rt) {
    for (size_t i = 0; rt->object_values && i < rt->object_count; i++) free(rt->object_values[i]);
    free(rt->object_values);
    free(rt->chains);
    free(rt->pickups);
    free(rt->pickup_moved);
    rt->object_values = NULL;
    rt->chains = NULL;
    rt->pickups = NULL;
    rt->pickup_moved = NULL;
    rt->object_count = 0;
    rt->map = NULL;
}

// The per-uid arrays grown to `count` objects, the new tail zeroed; chains[object_count] (the AI
// chain, chain_counter) moves to the new end. 0: out of memory (the caller rebuilds them).
static int runtime_grow(TriggerRuntime *rt, const size_t count) {
    const size_t old = rt->object_count;
    TriggerValueSet **values = realloc(rt->object_values, (count + 1) * sizeof(TriggerValueSet *));
    if (!values) return 0;
    rt->object_values = values;
    int *chains = realloc(rt->chains, (count + 1) * sizeof(int));
    if (!chains) return 0;
    rt->chains = chains;
    GamePickup *pickups = realloc(rt->pickups, (count + 1) * sizeof(GamePickup));
    if (!pickups) return 0;
    rt->pickups = pickups;
    unsigned char *moved = realloc(rt->pickup_moved, count + 1);
    if (!moved) return 0;
    rt->pickup_moved = moved;
    const int ai_chain = rt->chains[old];
    memset(rt->object_values + old, 0, (count + 1 - old) * sizeof(TriggerValueSet *));
    memset(rt->chains + old, 0, (count + 1 - old) * sizeof(int));
    memset(rt->pickups + old, 0, (count + 1 - old) * sizeof(GamePickup));
    memset(rt->pickup_moved + old, 0, count + 1 - old);
    rt->chains[count] = ai_chain;
    rt->object_count = count;
    return 1;
}

TriggerRuntime *trigger_runtime(Game *game) {
    if (!game) return NULL;
    if (!game->trigger_rt) {
        game->trigger_rt = calloc(1, sizeof(TriggerRuntime));
        if (!game->trigger_rt) return NULL;
    }
    TriggerRuntime *rt = game->trigger_rt;
    const size_t count = game->map ? game->map->object_count : 0;
    // KrunkNative: objects appended mid-round (builds, the Object Manager's) keep every existing
    // object's state (custVals, chains, pickups); v7 keys these by uid and never loses them.
    if (rt->map == game->map && rt->object_values && count > rt->object_count && runtime_grow(rt, count)) return rt;
    if (rt->map != game->map || rt->object_count != count) {
        runtime_free_objects(rt);
        rt->map = game->map;
        rt->object_count = count;
        rt->object_values = calloc(count + 1, sizeof(TriggerValueSet *));
        rt->chains = calloc(count + 1, sizeof(int));
        rt->pickups = calloc(count + 1, sizeof(GamePickup));
        rt->pickup_moved = calloc(count + 1, 1);
        if (!rt->object_values || !rt->chains || !rt->pickups || !rt->pickup_moved) {
            runtime_free_objects(rt);
            return NULL;
        }
        rt->queue_count = 0; // the queue's sources belonged to the old map
    }
    return rt;
}

TriggerValueSet *trigger_object_values_mut(Game *game, const Object *object, const int create) {
    TriggerRuntime *rt = trigger_runtime(game);
    const int uid = trigger_map_uid(game, object);
    if (!rt || uid < 0 || (size_t) uid >= rt->object_count) return NULL;
    if (!rt->object_values[uid] && create) rt->object_values[uid] = calloc(1, sizeof(TriggerValueSet));
    return rt->object_values[uid];
}

const TriggerValueSet *trigger_object_values(const Game *game, const Object *object) {
    const TriggerRuntime *rt = game ? game->trigger_rt : NULL;
    if (!rt || rt->map != game->map) return NULL;
    const int uid = trigger_map_uid(game, object);
    return uid >= 0 && (size_t) uid < rt->object_count ? rt->object_values[uid] : NULL;
}

static int *chain_counter(TriggerRuntime *rt, const int chain) {
    if (!rt || !rt->chains) return NULL;
    if (chain == TRIGGER_CHAIN_AI) return &rt->chains[rt->object_count];
    return chain >= 0 && (size_t) chain < rt->object_count ? &rt->chains[chain] : NULL;
}

int trigger_chain_left(Game *game, const int chain) {
    const int *counter = chain_counter(trigger_runtime(game), chain);
    return !counter || *counter > 0; // triggerChains[undefined] <= 0 is false
}

// ---------------------------------------------------------------- packets

void trigger_send_object_state(Game *game, const int uid, const int kind, const int value) {
    const NetObjectStatePacket packet = {.uid = uid, .kind = (uint8_t) kind, .value = value};
    game_send(game, NET_TARGET_ALL, NET_PACKET_OBJECT_STATE, &packet, sizeof(packet));
}

// changePosition's 8th argument: broadcast "chgp" id, x, y, z, recon, stopMo, dir to everyone
void trigger_send_change_position(Game *game, const Player *player, const int recon, const int stop, const int has_dir, const int dir) {
    if (!game_is_authority(game) || !player) return;
    const NetChangePositionPacket packet = {
        .uid = player->uid,
        .x = player->position.x, .y = player->position.y, .z = player->position.z,
        .recon = (uint8_t) !!recon, .stop_momentum = (uint8_t) !!stop, .has_direction = (uint8_t) !!has_dir,
        .direction = dir,
    };
    game_send(game, NET_TARGET_ALL, NET_PACKET_CHANGE_POSITION, &packet, sizeof(packet));
}

static void send_value(Game *game, const int target, const int type, const char *key, const double value, const int flags) {
    NetValuePacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.flags = (uint8_t) flags;
    if (key) snprintf(packet.key, sizeof(packet.key), "%s", key);
    packet.value = value;
    game_send(game, target, type, &packet, sizeof(packet));
}

// The account facts conditions 6-11 and methods 4-8 read: GameHooks.account_info (kn_brain), else
// only player.account itself (Player.has_account: a backend account or the host's own machine,
// which covers the offline sandbox, doc/v7/decisions.md "Offline account"). 0 = a guest.
int trigger_account(const Game *game, const Player *player, AccountInfo *out) {
    memset(out, 0, sizeof(*out));
    if (!player) return 0;
    if (game_account_info(game, player, out)) return out->has_account;
    out->has_account = player->has_account;
    return out->has_account;
}

// ---------------------------------------------------------------- values

double trigger_global_value_get(const Game *game, const char *key) {
    const TriggerValue *v = game && game->trigger_rt ? trigger_values_find(&game->trigger_rt->global_values, key) : NULL;
    return v ? v->value : 0.0;
}

void trigger_global_value_set(Game *game, const char *key, const double value) {
    if (!game || !key || !key[0] || !game_is_authority(game)) return;
    TriggerRuntime *rt = trigger_runtime(game);
    TriggerValue *slot = rt ? trigger_values_slot(&rt->global_values, key) : NULL;
    if (!slot) return;
    slot->value = value;
    send_value(game, NET_TARGET_ALL, NET_PACKET_GLOBAL_VALUE, key, value, 0); // broadcast("gv", key, val)
}

const TriggerValueSet *trigger_global_values(const Game *game) {
    return game && game->trigger_rt ? &game->trigger_rt->global_values : NULL;
}

void trigger_mirror_global_value(Game *game, const char *key, const double value, const int reset) {
    TriggerRuntime *rt = trigger_runtime(game);
    if (!rt) return;
    if (reset) memset(&rt->global_values, 0, sizeof(rt->global_values));
    TriggerValue *slot = key && key[0] ? trigger_values_slot(&rt->global_values, key) : NULL;
    if (slot) slot->value = value; // io_updateGlobalValue: Game.globalVals[key] = val
}

void trigger_mirror_custom_value(Player *player, const char *key, const double value) {
    TriggerValue *slot = player && key && key[0] ? trigger_values_slot(&player->custom_values, key) : NULL;
    if (slot) slot->value = value; // io_customVal: tmpPlayer.custVals[key] = val
}

double player_custom_value_get(const Player *player, const char *key) {
    const TriggerValue *v = player ? trigger_values_find(&player->custom_values, key) : NULL;
    return v ? v->value : 0.0;
}

void trigger_load_data_apply(Game *game, Player *player, const char *const *keys, const char *const *values, const int count) {
    for (int i = 0; game && player && i < count; i++) {
        if (!keys[i] || !keys[i][0]) continue;
        if (!trigger_values_find(&player->custom_values, keys[i]) && player->custom_values.count > TRIGGER_V7_CUSTOM_VALUE_LIMIT) return;
        const double value = ks_storage_parse_number(values[i]);
        if (value == value) player_custom_value_set(game, player, keys[i], value);
    }
}

void player_custom_value_set(Game *game, Player *player, const char *key, const double value) {
    if (!game || !player || !key || !key[0]) return;
    TriggerValue *slot = trigger_values_slot(&player->custom_values, key);
    if (!slot) return;
    slot->value = value;
    if (game_is_authority(game)) send_value(game, player->uid, NET_PACKET_CUSTOM_VALUE, key, value, 0); // send("cv", key, val)
}

// config.playerVals ["health", "maxHealth", "score", "classIndex"]: player[key], NULL = not one
static int player_builtin_value(const Player *player, const char *key, double *out) {
    if (!strcmp(key, "health")) *out = player->health;
    else if (!strcmp(key, "maxHealth")) *out = player->max_health;
    else if (!strcmp(key, "score")) *out = player->score;
    else if (!strcmp(key, "classIndex")) *out = player->class_index;
    else return 0;
    return 1;
}

// getActionValue(player, target || obj, action, "number", def). v7: type 0 -> def; types 1 / 2 / 3
// read obj.custVals / the player value / globalVals[tcc] and return parseInt(v) || undefined (so a
// stored 0 skips the action, Appendix D); no key, a missing value or another type -> undefined.
int trigger_action_value(Game *game, Player *player, const TriggerSource *obj, const Object *target, const TriggerActionData *a,
                         const int def, int *out) {
    if (!a->value_type) {
        *out = def;
        return 1;
    }
    if (!a->value_key[0]) return 0;
    double value;
    const TriggerValue *v = NULL;
    switch (a->value_type) {
        case 1: { // the trigger value of `obj` (the target when there is one, else the trigger)
            const Object *o = target ? target : obj ? obj->object : NULL;
            if (!o) return 0; // AIs have no custVals
            v = trigger_values_find(trigger_object_values(game, o), a->value_key);
            if (!v) return 0;
            value = v->value;
            break;
        }
        case 2:
            if (!player) return 0;
            if (!player_builtin_value(player, a->value_key, &value)) {
                v = trigger_values_find(&player->custom_values, a->value_key);
                if (!v) return 0;
                value = v->value;
            }
            break;
        case 3:
            v = game->trigger_rt ? trigger_values_find(&game->trigger_rt->global_values, a->value_key) : NULL;
            if (!v) return 0;
            value = v->value;
            break;
        default:
            return 0;
    }
    if (!(value == value) || fabs(value) >= 2147483647.0) return 0;
    const int number = (int) value; // parseInt truncates toward zero
    if (!number) return 0;           // || undefined
    *out = number;
    return 1;
}

// ---------------------------------------------------------------- sources

void trigger_source_object(TriggerSource *j, Object *object) {
    memset(j, 0, sizeof(*j));
    j->object = object;
    j->trig = object ? object->trig : NULL;
    j->uid = -1;
    if (!object) return;
    j->team = (int) object->team;
    j->interface_id = object->interface_id;
    j->interface_target = object->trig ? object->trig->interface_target : 0;
    // GameObject y of a TRIGGER is its centre (addTrigger: y + h / 2 * hbh)
    j->position = (vec3) {object->position.x, object->position.y + object->scale.y * 0.5f, object->position.z};
}

// MAP_AI_HAVE_TRIGGER_API (map-F): GameAIData.interface_id ("in") exists.
#ifdef MAP_AI_HAVE_TRIGGER_API
#define AI_INTERFACE_ID(ai) ((ai)->data.interface_id)
#else
#define AI_INTERFACE_ID(ai) 0
#endif

void trigger_source_ai(TriggerSource *j, GameAI *ai) {
    memset(j, 0, sizeof(*j));
    j->ai = ai;
    j->uid = ai ? ai->sid : -1;
    if (!ai) return;
    j->trig = &ai->data.trig;
    j->interface_id = AI_INTERFACE_ID(ai);
    j->interface_target = ai->data.trig.interface_target;
    j->position = ai->position;
}

static int source_is_ai(const TriggerSource *j) {
    return !j->object;
}

// The action list of a trigger: tas, or (no list: hand-built / v3-shaped data) the mirror fields
// as tas[0], the wrap of map.js:538-557.
static const TriggerActionData *source_actions(const TriggerData *t, TriggerActionData *mirror, int *count) {
    if (t->actions) {
        *count = t->action_count;
        return t->actions;
    }
    if (t->action < 0) {
        *count = 0;
        return NULL;
    }
    memset(mirror, 0, sizeof(*mirror));
    mirror->action = t->action;
    mirror->constant = t->constant;
    if (t->text[0]) {
        mirror->has_text = 1;
        snprintf(mirror->text, sizeof(mirror->text), "%s", t->text);
    }
    mirror->asset_sound = t->asset_sound;
    mirror->sound = t->sound;
    mirror->volume = t->volume ? t->volume : 1.0f;
    mirror->rate = t->rate ? t->rate : 1.0f;
    mirror->emit_from = t->emit_from;
    *count = 1;
    return mirror;
}

// ---------------------------------------------------------------- the queue

static void queue_push(Game *game, Player *player, const TriggerSource *j, const TriggerActionData *a, const int chain) {
    TriggerRuntime *rt = trigger_runtime(game);
    if (!rt) return;
    if (rt->queue_count == rt->queue_capacity) {
        const size_t capacity = rt->queue_capacity ? rt->queue_capacity * 2 : 16;
        TriggerQueued *queue = realloc(rt->queue, capacity * sizeof(TriggerQueued));
        if (!queue) return;
        rt->queue = queue;
        rt->queue_capacity = capacity;
    }
    TriggerQueued *q = &rt->queue[rt->queue_count++];
    q->action = *a;
    q->player_uid = player ? player->uid : -1;
    q->object_uid = j->object ? j->uid : -1;
    q->ai_sid = j->ai ? j->ai->sid : -1;
    q->chain = chain;
    q->timer = a->delay * 1000.0f; // (trigDelay || 0) * 1000
}

static int action_known(const TriggerActionData *a) { // game.triggers.actions[trigAct || 0]
    return a->action >= 0 && a->action < TRIGGER_ACTION_COUNT;
}

static void run_actions(Game *game, Player *player, const TriggerSource *j, const TriggerActionData *actions, const int count,
                        const int check_conditions, const int chain) {
    const TriggerActionData *last = NULL;
    for (int i = 0; i < count; i++) {
        const TriggerActionData *a = &actions[i];
        last = a;
        if (check_conditions && !trigger_condition_check(game, player, j, a)) continue;
        if (!action_known(a)) continue;
        if (a->delay > 0.0f) queue_push(game, player, j, a, chain);
        else trigger_action_execute(game, player, j, a, chain);
    }
    if (j->trig->self_destruct && last) { // actions[24].execute(..., lastAction, chain)
        TriggerActionData destroy = *last;
        destroy.action = TRIGGER_ACTION_DESTROY_SELF;
        trigger_action_execute(game, player, j, &destroy, chain);
    }
}

static void send_medal(Game *game, const Player *player, const char *text, const int score) {
    NetMedalPacket medal;
    memset(&medal, 0, sizeof(medal));
    snprintf(medal.text, sizeof(medal.text), "%s", text);
    medal.score = score;
    game_send(game, player->uid, NET_PACKET_MEDAL, &medal, sizeof(medal)); // "am" [text, sub, 0, 1]
}

// game.onTrigger(player, obj, chain)
void trigger_run(Game *game, Player *player, const TriggerSource *j, int chain) {
    if (!game || !j || !j->trig || !game_is_authority(game) || !game->map) return;
    TriggerData *t = j->trig;
    if (!source_is_ai(j) && !trigger_team_check(game, j->team, player)) return; // obj.isAI || teamCheck(obj, player)

    TriggerActionData mirror;
    int count = 0;
    const TriggerActionData *actions = source_actions(t, &mirror, &count);
    if (!actions || count <= 0) return;

    TriggerRuntime *rt = trigger_runtime(game);
    if (!rt) return;
    if (chain != -1) {
        const int *counter = chain_counter(rt, chain);
        if (counter && *counter <= 0) return;
    }
    if (t->single_use && t->triggered) return;
    if (t->chance && !(trigger_rand_int(0, 100) <= t->chance)) return;
    if (t->single_use_player && player) { // uses: once per player id (before any cost check)
        for (int i = 0; i < t->use_count; i++) {
            if (t->uses[i] == player->uid) return;
        }
        if (t->use_count >= TRIGGER_V7_USES_MAX) return; // KN: a full list refuses newcomers
        t->uses[t->use_count++] = player->uid;
    }
    t->triggered = 1;
    if (chain == -1) {
        chain = source_is_ai(j) ? TRIGGER_CHAIN_AI : j->uid;
        int *counter = chain_counter(rt, chain);
        if (counter) *counter = TRIGGER_LIMIT;
    }
    int *counter = chain_counter(rt, chain);
    if (counter) (*counter)--;

    const int cost = t->action_cost;
    const int method = t->action_method;
    if (!cost && !(method > 3)) {
        run_actions(game, player, j, actions, count, 1, chain);
    } else if (method == 0) {
        // KR cost: a backend account (not a hacker) and a KR wallet (GameHooks.spend_kr, v7 DB call
        // 138). Without one (offline, no kn_brain wallet) the trigger is unaffordable
        // (doc/v7/decisions.md "Backend-only triggers"). The actions then run without their
        // conditions (v7 quirk, kept).
        AccountInfo account;
        if (!player || !trigger_account(game, player, &account) || !account.has_account || account.hacker) return;
        if (!game->hooks.spend_kr || !game->hooks.spend_kr(game, player, cost)) return;
        send_medal(game, player, "Spent", -cost); // send("am", ["Spent", "-N KR", 0, 1])
        run_actions(game, player, j, actions, count, 0, chain);
    } else {
        if (!player) return; // v7 reads player.score here (a TypeError without one): nothing runs
        AccountInfo account;
        trigger_account(game, player, &account);
        const int ok = (method < 3 && player->score >= cost) || (method == 3 && (int) player->kills >= cost) ||
                       (method == 4 && account.has_account) || (method == 5 && account.verified) ||
                       (method == 6 && account.premium) || (method == 7 && account.partner) ||
                       (method == 8 && account.level >= cost);
        if (!ok) return;
        if (cost) {
            send_medal(game, player, method > 1 ? "Unlocked" : "Purchased", method > 1 ? 0 : -cost);
            if (method < 2) player_score(player, -cost, 0);
        }
        run_actions(game, player, j, actions, count, 1, chain);
    }
}

// The timerObjs loop and the triggerQueue (game.update #5 / #6, while no waitTimer blocks).
void trigger_queue_update(Game *game, const float delta) {
    if (!game || !game->ready || !game->map || !game_is_authority(game) || trigger_wait_blocks(game)) return;
    const float delta_ms = delta * 1000.0f;
    Map *map = game->map;
    for (size_t i = 0; i < map->object_count; i++) { // timerObjs: onTimer objects with collision (addHelpers)
        Object *object = map->objects[i];
        if (!trigger_has_event(object, TRIGGER_EVENT_ON_TIMER) || object->collision_type == COLLISION_TYPE_NONE) continue;
        TriggerData *t = object->trig;
        if (t->timer <= 0.0f || !object->active) continue;
        t->timer -= delta_ms;
        if (t->timer <= 0.0f) {
            t->timer = (float) (t->constant_event ? t->constant_event : 1) * 1000.0f; // (trigConstE || 1) * 1000
            game_on_trigger(game, NULL, object, NULL);
        }
    }

    TriggerRuntime *rt = game->trigger_rt;
    if (!rt) return;
    for (size_t n = rt->queue_count; n-- > 0;) {
        if (n >= rt->queue_count) continue; // a round reset emptied the queue meanwhile
        rt->queue[n].timer -= delta_ms;
        if (rt->queue[n].timer > 0.0f) continue;
        const TriggerQueued q = rt->queue[n];
        memmove(&rt->queue[n], &rt->queue[n + 1], (rt->queue_count - n - 1) * sizeof(TriggerQueued));
        rt->queue_count--;

        TriggerSource j;
        if (q.object_uid >= 0) {
            if ((size_t) q.object_uid >= map->object_count) continue;
            trigger_source_object(&j, map->objects[q.object_uid]);
            j.uid = q.object_uid;
        } else {
            GameAI *ai = q.ai_sid >= 0 ? game_ai_find(game, q.ai_sid) : NULL;
            if (!ai) continue; // the AI is gone
            trigger_source_ai(&j, ai);
        }
        if (!j.trig) continue;
        Player *player = q.player_uid >= 0 ? game_find_player(game, q.player_uid) : NULL;
        trigger_action_execute(game, player, &j, &q.action, q.chain); // the condition is not checked again
    }
}

// ---------------------------------------------------------------- dispatch

void trigger_object_event(Game *game, Object *object, const int event, Player *player) {
    if (!game || !object || !object->active || !trigger_has_event(object, event)) return;
    game_on_trigger(game, player, object, NULL);
}

// playerObjs: addHelpers lists the !noCol objects whose event name contains "onPlayer"
void trigger_player_event(Game *game, const int event, Player *player) {
    if (!game || !game->map || !game_is_authority(game)) return;
    for (size_t i = 0; i < game->map->object_count; i++) {
        Object *object = game->map->objects[i];
        if (object->collision_type == COLLISION_TYPE_NONE) continue;
        if (object->active && trigger_has_event(object, event)) game_on_trigger(game, player, object, NULL);
    }
}

void trigger_multi_event(Game *game, const int event, Player *player) {
    if (!game || !game->map || !game_is_authority(game)) return;
    for (size_t i = 0; i < game->map->object_count; i++) {
        Object *object = game->map->objects[i];
        if (object->active && trigger_has_event(object, event)) game_on_trigger(game, player, object, NULL);
    }
}

void trigger_ai_event(Game *game, GameAI *ai, const int event, Player *player) {
    (void) event; // the caller gates on the AI's own event (this[eventN])
    if (!game || !ai || !game_is_authority(game)) return;
    TriggerSource j;
    trigger_source_ai(&j, ai);
    trigger_run(game, player, &j, -1);
}

void triggers_set_script_hooks(Game *game, const TriggerScriptHooks *hooks) {
    if (game) game->script_hooks = hooks;
}

// ---------------------------------------------------------------- player sounds

void game_add_player_sound(Game *game, Player *player, const char *name, const float volume, const float rate, const int loop) {
    if (!game || !player || !name || !name[0] || !game_is_authority(game)) return;
    TriggerRuntime *rt = trigger_runtime(game);
    if (!rt) return;
    if (rt->sound_count == rt->sound_capacity) {
        const size_t capacity = rt->sound_capacity ? rt->sound_capacity * 2 : 8;
        TriggerPlayerSound *sounds = realloc(rt->sounds, capacity * sizeof(TriggerPlayerSound));
        if (!sounds) return;
        rt->sounds = sounds;
        rt->sound_capacity = capacity;
    }
    TriggerPlayerSound *s = &rt->sounds[rt->sound_count++];
    memset(s, 0, sizeof(*s));
    s->uid = player->uid;
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->volume = volume;
    s->rate = rate;
    s->loop = (unsigned char) !!loop;
    NetPlayerSoundPacket packet = {.uid = player->uid, .add = 1, .loop = s->loop, .volume = volume, .rate = rate};
    snprintf(packet.name, sizeof(packet.name), "%s", name);
    game_send(game, NET_TARGET_ALL, NET_PACKET_PLAYER_SOUND, &packet, sizeof(packet)); // broadcast("asnd", sid, name, data)
}

// removePlayerSound(sid, name, force): the newest match goes (all of them with force), "rsnd" each.
void game_remove_player_sound(Game *game, Player *player, const char *name, const int force) {
    TriggerRuntime *rt = game ? game->trigger_rt : NULL;
    if (!rt || !player || !game_is_authority(game)) return;
    for (size_t n = rt->sound_count; n-- > 0;) {
        TriggerPlayerSound *s = &rt->sounds[n];
        if (s->uid != player->uid || (!force && (!name || strcmp(s->name, name)))) continue;
        NetPlayerSoundPacket packet = {.uid = player->uid, .add = 0, .force = (uint8_t) !!force};
        snprintf(packet.name, sizeof(packet.name), "%s", s->name);
        memmove(s, s + 1, (rt->sound_count - n - 1) * sizeof(TriggerPlayerSound));
        rt->sound_count--;
        game_send(game, NET_TARGET_ALL, NET_PACKET_PLAYER_SOUND, &packet, sizeof(packet));
        if (!force) return;
    }
}

void trigger_player_left(Game *game, Player *player) {
    if (!game || !player) return;
    game_remove_player_sound(game, player, NULL, 1);
    TriggerRuntime *rt = game->trigger_rt;
    for (size_t i = 0; rt && i < rt->queue_count; i++) {
        if (rt->queue[i].player_uid == player->uid) rt->queue[i].player_uid = -1;
    }
}

// ---------------------------------------------------------------- round reset / join

void trigger_round_reset(Game *game) {
    if (!game) return;
    TriggerRuntime *rt = trigger_runtime(game);
    if (!rt) return;
    rt->queue_count = 0;                        // triggerQueue.length = 0
    rt->sound_count = 0;                        // playerSounds.length = 0
    memset(&rt->global_values, 0, sizeof(rt->global_values)); // globalVals = {}
    for (size_t i = 0; i <= rt->object_count; i++) rt->chains[i] = 0;
    Map *map = game->map;
    for (size_t i = 0; map && i < map->object_count; i++) {
        Object *object = map->objects[i];
        free(rt->object_values[i]); // custVals = {}
        rt->object_values[i] = NULL;
        // resetAll: an rsc spawn chance lets a start-destroyed object exist this round (server only;
        // KN has no lowPop public lobbies, so the roll always runs)
        if (game_is_authority(game) && object->spawn_chance > 0.0f && object->start_destroyed && object->can_destroy &&
            (float) pcg32_boundedrand(10001) / 100.0f <= object->spawn_chance) {
            object->destroyed = 0;
            object->active = 1;
        }
        TriggerData *t = object->trig;
        if (!t) continue;
        t->storage = 0;
        t->triggered = 0;
        t->timer = (float) (t->constant_event ? t->constant_event : 1) * 1000.0f; // trigT = (trigConstE || 1) * 1000
        t->use_count = 0;
    }
    interact_reset_pickups(game); // resetPickups
}

void trigger_send_join_state(Game *game, const int uid) {
    if (!game || !game_is_authority(game)) return;
    // "gbl": the client's globalVals replaced by the server's
    send_value(game, uid, NET_PACKET_GLOBAL_VALUE, "", 0.0, NET_VALUE_RESET);
    TriggerRuntime *rt = game->trigger_rt;
    for (int i = 0; rt && i < rt->global_values.count; i++) {
        send_value(game, uid, NET_PACKET_GLOBAL_VALUE, rt->global_values.values[i].key, rt->global_values.values[i].value, 0);
    }
    // "asnd": the playing player sounds (a force-remove with uid -1 clears the client's first)
    NetPlayerSoundPacket clear = {.uid = -1, .add = 0, .force = 1};
    game_send(game, uid, NET_PACKET_PLAYER_SOUND, &clear, sizeof(clear));
    for (size_t i = 0; rt && i < rt->sound_count; i++) {
        const TriggerPlayerSound *s = &rt->sounds[i];
        NetPlayerSoundPacket packet = {.uid = s->uid, .add = 1, .loop = s->loop, .volume = s->volume, .rate = s->rate};
        snprintf(packet.name, sizeof(packet.name), "%s", s->name);
        game_send(game, uid, NET_PACKET_PLAYER_SOUND, &packet, sizeof(packet));
    }
    interact_send_pickups(game, uid); // "pkups"
}
