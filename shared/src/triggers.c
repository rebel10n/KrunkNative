// Krunker v3.1.1 map triggers, interfaces, check points and teleporters:
//   src/data/triggers.js     events / actions (the 32 action bodies below, in order)
//   src/data/game.js         onTrigger / triggerChains, setCheckPoint, checkTeleport,
//                            updateTeleporters, updateDestructables, the timerObjs loop, playSound
//   src/data/objects.js      GameObject trigger fields, addHelpers (interfaces, timerObjs), resetAll
//   src/data/players.js      changePosition, the destructible branch of shoot / melee
//   src/data/projectile.js   the destructible branch of a projectile hit
// Map objects are addressed by their index in Map.objects (Krunker GameObject uid). Krunker's
// map.manager.destObjs list is the `destroyed` flag (an object is in destObjs exactly while it
// is destroyed), map.manager.interfaces are the objects with an interface id and collision.
// Everything that changes game state runs on the authority only (Krunker `io`); teleporters
// and check points also run on clients for prediction like players.js does.
#include <shared.h>
#include <ai.h>
#include "modes/mode_internal.h"
#include <math.h>
#include <pcg_basic.h>
#include <stdio.h>
#include <string.h>

const char *const g_trigger_events[TRIGGER_EVENT_COUNT] = {
    "onShoot", "onMelee", "onDamage", "onEnter", "onDestroy", "onRespawn", "onTimer", "onProjectile",
    "onThrowingMelee", "onPunch", "onInteract", "onCollision", "onStorageEquals", "onStorageGreater",
    "onStorageLesser", "onAIDeath",
};

const TriggerActionInfo g_trigger_actions[TRIGGER_ACTION_COUNT] = {
    {"Give Player Score", TRIGGER_VALUE_NUMBER},
    {"Kill Player", TRIGGER_VALUE_NUMBER},
    {"Respawn Player", TRIGGER_VALUE_NUMBER},
    {"Change Player Health", TRIGGER_VALUE_NUMBER},
    {"Destroy Interface", TRIGGER_VALUE_NUMBER},
    {"Toggle Interface Gate", TRIGGER_VALUE_NUMBER},
    {"Change Interface Health", TRIGGER_VALUE_NUMBER},
    {"Teleport To Interface", TRIGGER_VALUE_NUMBER},
    {"Change Interface Deposit Box Amount", TRIGGER_VALUE_NUMBER},
    {"Kill Opposing Team", TRIGGER_VALUE_NUMBER},
    {"Respawn Opposing Team", TRIGGER_VALUE_NUMBER},
    {"Respawn Destructible Interface", TRIGGER_VALUE_NUMBER},
    {"Toggle Destructible Interface", TRIGGER_VALUE_NUMBER},
    {"Set Player Score", TRIGGER_VALUE_NUMBER},
    {"Broadcast Sound", TRIGGER_VALUE_SOUND},
    {"Clear Player Loadout", TRIGGER_VALUE_NUMBER},
    {"Broadcast Message", TRIGGER_VALUE_TEXT},
    {"Send Message", TRIGGER_VALUE_TEXT},
    {"Swap Player Team", TRIGGER_VALUE_NUMBER},
    {"Set Player Team", TRIGGER_VALUE_NUMBER},
    {"Round Time", TRIGGER_VALUE_NUMBER},
    {"Reset Interface", TRIGGER_VALUE_NUMBER},
    {"End Round", TRIGGER_VALUE_NUMBER},
    {"Refill Player Knife", TRIGGER_VALUE_NUMBER},
    {"Destroy Self", TRIGGER_VALUE_NUMBER},
    {"Increase Interface Storage", TRIGGER_VALUE_NUMBER},
    {"Decrease Interface Storage", TRIGGER_VALUE_NUMBER},
    {"Set Interface Storage", TRIGGER_VALUE_NUMBER},
    {"Reset Interface Storage", TRIGGER_VALUE_NUMBER},
    {"Broadcast Popup", TRIGGER_VALUE_TEXT},
    {"Send Popup", TRIGGER_VALUE_TEXT},
    {"Jump Scare", TRIGGER_VALUE_NUMBER},
};

const char *const g_trigger_emitter_sounds[TRIGGER_EMITTER_SOUND_COUNT] = {
    "nuke_0", "siren_0", "explosion", "fart_0", "taunt_0", "taunt_1", "taunt_2", "taunt_3", "buy_0", "buy_1",
    "case_0", "case_1", "cheer_0", "reward", "store", "tick_0", "jumpS_0", "cough_1", "cough_2", "taunt_4",
    "ambient_1", "ambient_2", "ambient_3", "ambient_4", "ambient_5", "ambient_6", "ambient_7", "ambient_8",
    "beep_0", "bo_plant", "bo_diff", "collect_1", "bounce_0", "headshot_0", "hit_0", "impact_0", "jump_0",
    "jump_1", "land_0", "punch_0", "punch_1", "slide_0", "spray", "step_0", "step_1", "step_2", "whizz_0",
    "whizz_1", "gclick_0", "pick_0", "pick_1",
};

const char *const g_trigger_network_sounds[TRIGGER_NETWORK_SOUND_COUNT] = {
    "spray", "taunt_0", "taunt_1", "taunt_2", "taunt_3", "weapon_1", "weapon_1_0", "weapon_2", "weapon_3",
    "weapon_4", "weapon_5", "weapon_6", "weapon_7", "weapon_8", "weapon_9", "weapon_10", "weapon_11",
    "weapon_12", "weapon_13", "weapon_14", "weapon_15", "weapon_16", "weapon_17", "weapon_18", "weapon_6_0",
    "weapon_1_2", "weapon_3_0", "weapon_3_2", "taunt_4", "weapon_2_0", "weapon_1_4", "weapon_1_3", "weapon_17",
    "weapon_8_0", "weapon_10_0", "collect_1", "melee_12", "headshot_0", "weapon_4_0", "melee_14", "beep_0",
    "bo_plant", "bo_diff", "weapon_1_5", "weapon_19", "melee_19", "jump_0", "jump_1", "step_0", "step_1", "step_2",
};

// /[^\w\:\;\(\)\?\,\.\|\[\] \-\+*\$\@\=\^!{}]+/ must not match, and at most 70 characters.
int trigger_text_valid(const char *text) {
    if (!text) return 0;
    size_t length = 0;
    for (const unsigned char *c = (const unsigned char *) text; *c; c++, length++) {
        const int word = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_';
        if (!word && !strchr(":;()?,.|[] -+*$@=^!{}", *c)) return 0;
    }
    return length <= TRIGGER_TEXT_MAX;
}

// ---------------------------------------------------------------- helpers

// The object a trigger action / event runs for: a map TRIGGER or a BOT (j.isAI).
typedef struct {
    Object *object;      // NULL for an AI
    TriggerData *trig;
    int team;
    int interface_id;    // j.interface (AIs: 0)
    vec3 position;       // j.x / j.y (centre of an object, feet of an AI) / j.z
} TriggerSource;

static int map_object_uid(const Game *game, const Object *object) {
    for (size_t i = 0; game->map && i < game->map->object_count; i++) {
        if (game->map->objects[i] == object) return (int) i;
    }
    return -1;
}

static int has_wait_timers(const Game *game) {
    return game->mode && game->mode->wait_timer_count > 0; // game.waitTimers != null
}

static int teams_allow(const Game *game, const int team, const Player *player) {
    return !team || !game->mode || !game->mode->config.teams || (player && team == player->team);
}

static int rand_int(const int min, const int max) { // UTILS.randInt (inclusive)
    return max <= min ? min : min + (int) pcg32_boundedrand((uint32_t) (max - min + 1));
}

int game_object_event(const Object *object) {
    return object && object->trig ? object->trig->event : TRIGGER_EVENT_NONE;
}

static int event_is(const Object *object, const int event) {
    return game_object_event(object) == event;
}

static int is_storage_event(const Object *object) {
    const int event = game_object_event(object);
    return event == TRIGGER_EVENT_ON_STORAGE_EQUALS || event == TRIGGER_EVENT_ON_STORAGE_GREATER || event == TRIGGER_EVENT_ON_STORAGE_LESSER;
}

// map.manager.interfaces: addHelpers registers objects with an interface id and collision.
static int is_interface(const Object *object) {
    return object->interface_id && object->collision_type != COLLISION_TYPE_NONE;
}

// (j.isAI || d.sid != j.sid) && d.interface != j.interface && d.interface == j.interfaceT
static int targets(const TriggerSource *j, const Object *d) {
    return is_interface(d) && d != j->object && d->interface_id != j->interface_id && d->interface_id == j->trig->interface_target;
}

static void send_object_state(Game *game, const int uid, const int kind, const int value) {
    const NetObjectStatePacket packet = {.uid = uid, .kind = (uint8_t) kind, .value = value};
    game_send(game, NET_TARGET_ALL, NET_PACKET_OBJECT_STATE, &packet, sizeof(packet));
}

static void send_text(Game *game, const int uid, const char *text, const int popup) {
    if (popup) { // "am" [text, null]: a medal popup without score
        NetPopupPacket packet = {0};
        snprintf(packet.text, sizeof(packet.text), "%s", text);
        game_send(game, uid, NET_PACKET_POPUP, &packet, sizeof(packet));
        return;
    }
    NetChatPacket packet = {.uid = -1, .kind = NET_CHAT_SERVER}; // "ch" null, text, 2
    snprintf(packet.text, sizeof(packet.text), "%s", text);
    game_send(game, uid, NET_PACKET_CHAT, &packet, sizeof(packet));
}

void game_play_sound(Game *game, const char *name, const int uid, const vec3 position, const float volume, const float rate) {
    if (!game_is_authority(game) || !name || !name[0] || !volume) return; // playSound: if (a1)
    NetSoundPacket packet = {
        .uid = uid,
        .position = {roundf(position.x), roundf(position.y), roundf(position.z)},
        .volume = volume,
        .rate = rate,
    };
    snprintf(packet.name, sizeof(packet.name), "%s", name);
    game_send(game, NET_TARGET_ALL, NET_PACKET_SOUND, &packet, sizeof(packet)); // active or spectating, the source too (a6)
}

// ---------------------------------------------------------------- positions

// game_change_position (players.changePosition): shared/src/world.c (map-logic WP-B).

// getSpawnPoint(player.team, player) (only the point: changePosition keeps the view).
static vec3 spawn_point(Game *game, Player *player) {
    vec3 point = player->position;
    int direction;
    game_get_spawn_point(game, player, &point, &direction);
    return point;
}

static void respawn_player(Game *game, Player *player) {
    game_change_position(game, player, spawn_point(game, player), 1, 0, 0, 0); // changePosition(h, x, y, z, true)
}

// ---------------------------------------------------------------- destructibles / gates / banks

void game_destroy_object(Game *game, Object *object, Player *by) {
    object->active = 0;
    object->health = 0.0f;
    object->destroyed = 1;
    object->destroyed_by = by ? by->uid : -1;
    send_object_state(game, map_object_uid(game, object), NET_OBJECT_DESTROY, 0); // destObjs.push(uid); "do"
}

static Player *destroyed_by(Game *game, const Object *object) {
    return object->destroyed_by >= 0 ? game_find_player(game, object->destroyed_by) : NULL;
}

void game_respawn_object(Game *game, Object *object, int *chain) {
    object->destroyed = 0; // destObjs.splice
    object->active = 1;
    object->respawn_time = object->respawn_random ? 0.0f : object->respawn_time_start;
    object->health = object->start_health;
    send_object_state(game, map_object_uid(game, object), NET_OBJECT_RESPAWN, 0); // "ro"
    Player *by = destroyed_by(game, object);
    if (event_is(object, TRIGGER_EVENT_ON_RESPAWN) && object->destroyed_by >= 0) game_on_trigger(game, by, object, chain);
    object->destroyed_by = -1;
}

void game_set_gate(Game *game, Object *object, const int active) {
    object->active = (unsigned char) !!active;
    send_object_state(game, map_object_uid(game, object), NET_OBJECT_GATE, object->active); // "gte"
}

// The health branch of players.shoot / melee and projectile.js, then the hit events.
void game_object_hit(Game *game, Object *object, Player *dealer, const float damage, const int thrown_melee, const int kind) {
    if (!game_is_authority(game) || !object) return;
    const int can_damage = kind == GAME_HIT_PROJECTILE ? dealer && damage : 1; // projectile: m.owner && I
    if (can_damage && !has_wait_timers(game) && object->health) {
        object->health -= damage;
        if (dealer) {
            const NetHitPacket hit = {.target_uid = -1}; // IO.send(player.id, "4"): hitmarker only
            game_send(game, dealer->uid, NET_PACKET_HIT, &hit, sizeof(hit));
        }
        if (object->health <= 0.0f) {
            game_destroy_object(game, object, dealer);
            if (event_is(object, TRIGGER_EVENT_ON_DESTROY)) game_on_trigger(game, dealer, object, NULL);
        }
    }

    const int event = game_object_event(object);
    int fire = event == TRIGGER_EVENT_ON_DAMAGE;
    switch (kind) {
        case GAME_HIT_SHOT:
            fire |= event == TRIGGER_EVENT_ON_SHOOT;
            break;
        case GAME_HIT_MELEE:
            fire |= event == TRIGGER_EVENT_ON_MELEE || (dealer && !dealer->can_throw && event == TRIGGER_EVENT_ON_PUNCH);
            break;
        case GAME_HIT_PROJECTILE:
            fire |= event == TRIGGER_EVENT_ON_SHOOT || event == TRIGGER_EVENT_ON_PROJECTILE ||
                    (dealer && thrown_melee && event == TRIGGER_EVENT_ON_THROWING_MELEE);
            break;
        default:
            break;
    }
    if (fire) game_on_trigger(game, dealer, object, NULL);
}

// ---------------------------------------------------------------- actions

static void fire_storage_events(Game *game, Player *h, Object *c, int *chain) {
    const int threshold = c->trig->constant_event;
    if (c->trig->storage == threshold && event_is(c, TRIGGER_EVENT_ON_STORAGE_EQUALS)) game_on_trigger(game, h, c, chain);
    if (c->trig->storage > threshold && event_is(c, TRIGGER_EVENT_ON_STORAGE_GREATER)) game_on_trigger(game, h, c, chain);
    if (c->trig->storage < threshold && event_is(c, TRIGGER_EVENT_ON_STORAGE_LESSER)) game_on_trigger(game, h, c, chain);
}

static void action_storage(Game *game, const int action, Player *h, const TriggerSource *j, int *chain) {
    const int value = j->trig->constant;
    for (size_t l = 0; l < game->map->object_count && !(*chain <= 0); l++) {
        Object *c = game->map->objects[l];
        if (!targets(j, c)) continue;
        // Reset Interface Storage only looks for onStorageEquals / onStorageGreater (triggers.js)
        if (action == TRIGGER_ACTION_RESET_STORAGE ? !(event_is(c, TRIGGER_EVENT_ON_STORAGE_EQUALS) || event_is(c, TRIGGER_EVENT_ON_STORAGE_GREATER))
                                                   : !is_storage_event(c)) continue;
        switch (action) {
            case TRIGGER_ACTION_INCREASE_STORAGE: c->trig->storage += value; break;
            case TRIGGER_ACTION_DECREASE_STORAGE: c->trig->storage -= value; break;
            case TRIGGER_ACTION_SET_STORAGE: c->trig->storage = value; break;
            default: c->trig->storage = 0; break;
        }
        fire_storage_events(game, h, c, chain);
    }
}

static void action_broadcast_sound(Game *game, Player *h, const TriggerSource *j) {
    const TriggerData *t = j->trig;
    char name[NET_SOUND_NAME_SIZE];
    if (t->asset_sound) snprintf(name, sizeof(name), "asset:%d", t->sound); // a user asset sound (raw)
    else if (t->network_sound) snprintf(name, sizeof(name), "%s", t->sound >= 0 && t->sound < TRIGGER_NETWORK_SOUND_COUNT ? g_trigger_network_sounds[t->sound] : "");
    else snprintf(name, sizeof(name), "%s", g_trigger_emitter_sounds[t->sound >= 0 && t->sound < TRIGGER_EMITTER_SOUND_COUNT ? t->sound : 0]);

    const int from_player = t->emit_from && h; // k = triggerSound[3] && h
    const vec3 at = from_player ? (vec3) {h->position.x, h->position.y + h->height, h->position.z} : j->position;
    game_play_sound(game, name, from_player ? h->uid : -1, at, t->volume, t->rate);
}

static void action_clear_loadout(Game *game, Player *h) {
    if (h->loadout_size == 1 && h->loadout[0] == WEAPON_ID_KNIFE) return;
    const int knife = WEAPON_ID_KNIFE;
    mode_set_player_loadout(h, &knife, 1, 0); // h.updateLoadout(f, 0, true, 12)
    mode_send_loadout(game, h);               // "inat" sid, [12], 0
}

static void action_set_score(Game *game, Player *h, int value) {
    h->score = value; // players.score(h, Q, false, true): no "5"
    if (h->score > GAME_SCORE_CAP) {
        h->score = GAME_SCORE_CAP;
        value = 0;
    }
    mode_on_score(game, h, value, 0);
    game->leaders_dirty = 1;
}

static void action_round_time(Game *game, const int value) {
    GameMode *mode = game->mode;
    if (!mode || mode->skip || game->end_timer > 0.0f || game->round_endless) return; // isNumber(gameTimer)
    float *clock = mode->config.timed ? &mode->round_clock : &game->round_timer;
    *clock += (float) value / 1000.0f; // gameTimer is in ms
    if (*clock <= 0.0f) mode->skip = 1; // gameTimer = "skip"
}

static void execute_action(Game *game, const int action, Player *h, const TriggerSource *j, int *chain) {
    const TriggerData *t = j->trig;
    Map *map = game->map;

    switch (action) {
        case TRIGGER_ACTION_GIVE_SCORE:
            if (h) player_score(h, t->constant ? t->constant : 1, 0);
            break;
        case TRIGGER_ACTION_KILL_PLAYER:
            if (h && !h->god_mode) player_kill(h, NULL, NULL, 0);
            break;
        case TRIGGER_ACTION_RESPAWN_PLAYER:
            if (h) respawn_player(game, h);
            break;
        case TRIGGER_ACTION_CHANGE_HEALTH:
            if (h && player_change_health(h, NULL, -(float) t->constant, 0)) player_kill(h, NULL, NULL, 0);
            break;
        case TRIGGER_ACTION_DESTROY_INTERFACE:
            for (size_t l = 0; l < map->object_count && !(*chain <= 0); l++) {
                Object *d = map->objects[l];
                if (!targets(j, d) || !(d->health || d->gate || (d->can_destroy && !d->destroyed))) continue;
                if (d->gate) {
                    game_set_gate(game, d, 0);
                } else {
                    game_destroy_object(game, d, h);
                }
                if (event_is(d, TRIGGER_EVENT_ON_DESTROY) || event_is(d, TRIGGER_EVENT_ON_DAMAGE)) game_on_trigger(game, h, d, chain);
            }
            break;
        case TRIGGER_ACTION_TOGGLE_GATE:
            for (size_t l = 0; l < map->object_count; l++) {
                Object *d = map->objects[l];
                if (targets(j, d) && d->gate) game_set_gate(game, d, !d->active);
            }
            break;
        case TRIGGER_ACTION_CHANGE_INTERFACE_HEALTH:
            for (size_t l = 0; l < map->object_count && !(*chain <= 0); l++) {
                Object *d = map->objects[l];
                if (!targets(j, d) || !d->health) continue;
                d->health += (float) t->constant; // d.health -= -(constant)
                if (d->health <= 0.0f) {
                    game_destroy_object(game, d, h);
                    if (event_is(d, TRIGGER_EVENT_ON_DESTROY)) game_on_trigger(game, h, d, chain);
                }
                if (event_is(d, TRIGGER_EVENT_ON_DAMAGE) && t->constant < 0) game_on_trigger(game, h, d, chain);
            }
            break;
        case TRIGGER_ACTION_TELEPORT_TO_INTERFACE:
            if (!h) break;
            for (size_t l = 0; l < map->object_count && !(*chain <= 0); l++) {
                Object *d = map->objects[l];
                if (targets(j, d)) game_change_position(game, h, d->position, 1, 1, 0, 0); // (d.x, d.y - d.height, d.z, true, true)
            }
            break;
        case TRIGGER_ACTION_CHANGE_DEPOSIT:
            for (size_t l = 0; l < map->object_count && !(*chain <= 0); l++) {
                Object *d = map->objects[l];
                if (!targets(j, d) || !d->bank) continue;
                d->deposited += t->constant;
                send_object_state(game, (int) l, NET_OBJECT_BANK, d->deposited); // "bnk"
            }
            break;
        case TRIGGER_ACTION_KILL_OPPOSING_TEAM:
            if (!h) break;
            for (size_t i = 0; i < game->player_count; i++) {
                Player *c = game->players[i];
                if (c && c->active && c != h && !c->god_mode && (!c->team || h->team != c->team)) player_kill(c, NULL, NULL, 1);
            }
            break;
        case TRIGGER_ACTION_RESPAWN_OPPOSING_TEAM:
            if (!h) break;
            for (size_t i = 0; i < game->player_count; i++) {
                Player *c = game->players[i];
                if (c && c->active && c != h && (!c->team || h->team != c->team)) respawn_player(game, c);
            }
            break;
        case TRIGGER_ACTION_RESPAWN_DESTRUCTIBLE:
            for (size_t l = 0; l < map->object_count && !(*chain <= 0); l++) {
                Object *c = map->objects[l];
                if (targets(j, c) && (c->start_health || c->can_destroy) && c->destroyed) game_respawn_object(game, c, chain);
            }
            break;
        case TRIGGER_ACTION_TOGGLE_DESTRUCTIBLE:
            for (size_t l = 0; l < map->object_count && !(*chain <= 0); l++) {
                Object *c = map->objects[l];
                if (!targets(j, c) || !(c->start_health || c->can_destroy)) continue;
                if (!c->destroyed) {
                    game_destroy_object(game, c, h);
                    if (event_is(c, TRIGGER_EVENT_ON_DESTROY) || event_is(c, TRIGGER_EVENT_ON_DAMAGE)) game_on_trigger(game, h, c, chain);
                } else {
                    game_respawn_object(game, c, chain);
                }
            }
            break;
        case TRIGGER_ACTION_SET_SCORE:
            if (h) action_set_score(game, h, t->constant);
            break;
        case TRIGGER_ACTION_BROADCAST_SOUND:
            action_broadcast_sound(game, h, j);
            break;
        case TRIGGER_ACTION_CLEAR_LOADOUT:
            if (h) action_clear_loadout(game, h);
            break;
        case TRIGGER_ACTION_BROADCAST_MESSAGE:
            if (t->text[0]) send_text(game, NET_TARGET_ALL, t->text, 0);
            break;
        case TRIGGER_ACTION_SEND_MESSAGE:
            if (h && t->text[0]) send_text(game, h->uid, t->text, 0);
            break;
        case TRIGGER_ACTION_SWAP_TEAM:
            if (h) mode_update_team(game, h, h->team == 2 ? 1 : 2);
            break;
        case TRIGGER_ACTION_SET_TEAM:
            if (h) mode_update_team(game, h, t->constant > 2 ? 2 : t->constant < 1 ? 1 : t->constant);
            break;
        case TRIGGER_ACTION_ROUND_TIME:
            action_round_time(game, t->constant);
            break;
        case TRIGGER_ACTION_RESET_INTERFACE:
            for (size_t l = 0; l < map->object_count && !(*chain <= 0); l++) {
                Object *c = map->objects[l];
                if (!targets(j, c) || !event_is(c, TRIGGER_EVENT_ON_TIMER)) continue;
                if (c->destroyed) game_respawn_object(game, c, chain);
                c->trig->timer = (float) c->trig->constant_event * 1000.0f;
                c->health = c->start_health;
                c->destroyed = 0;
                c->active = 1;
            }
            break;
        case TRIGGER_ACTION_END_ROUND:
            if (game->mode) game->mode->skip = 1; // gameTimer = "skip"
            break;
        case TRIGGER_ACTION_REFILL_KNIFE:
            if (!h) break;
            h->can_throw = 1; // h.refillKnife(); g.send(h.id, "rfl")
            game_send(game, h->uid, NET_PACKET_REFILL, NULL, 0);
            break;
        case TRIGGER_ACTION_DESTROY_SELF: {
            Object *self = j->object;
            if (!self || !(self->health || self->gate || self->can_destroy) || !self->active) break;
            if (self->gate) {
                game_set_gate(game, self, 0);
            } else {
                game_destroy_object(game, self, h);
            }
            if (!t->self_destruct && (event_is(self, TRIGGER_EVENT_ON_DESTROY) || event_is(self, TRIGGER_EVENT_ON_DAMAGE))) game_on_trigger(game, h, self, chain);
            break;
        }
        case TRIGGER_ACTION_INCREASE_STORAGE:
        case TRIGGER_ACTION_DECREASE_STORAGE:
        case TRIGGER_ACTION_SET_STORAGE:
        case TRIGGER_ACTION_RESET_STORAGE:
            action_storage(game, action, h, j, chain);
            break;
        case TRIGGER_ACTION_BROADCAST_POPUP:
            if (t->text[0]) send_text(game, NET_TARGET_ALL, t->text, 1);
            break;
        case TRIGGER_ACTION_SEND_POPUP:
            if (h && t->text[0]) send_text(game, h->uid, t->text, 1);
            break;
        case TRIGGER_ACTION_JUMP_SCARE:
            if (h) {
                const NetJumpScarePacket packet = {.index = 0}; // g.send(h.id, "scr", 0)
                game_send(game, h->uid, NET_PACKET_JUMP_SCARE, &packet, sizeof(packet));
            }
            break;
        default:
            break;
    }
}

// game.onTrigger(a0 = player, a1 = trigger, a2 = chain key): the per-chain triggerChains
// counter is one int shared by the nested calls of a chain (they all pass its key).
static void trigger_fire(Game *game, Player *h, const TriggerSource *j, int *chain) {
    TriggerData *t = j->trig;
    if (!game_is_authority(game) || !game->map || t->action < 0 || t->action >= TRIGGER_ACTION_COUNT) return;
    if (!teams_allow(game, j->team, h)) return; // (!a1.team || !mode || !mode.teams || (a0 && a1.team == a0.team))
    if (t->single_use && t->triggered) return;
    if (t->chance && !(rand_int(0, 100) <= t->chance)) return;
    t->triggered = 1;

    int fresh = TRIGGER_LIMIT;
    if (!chain) chain = &fresh; // a2 = a1.uid; triggerChains[a2] = triggerLimit
    if (*chain <= 0) return;
    (*chain)--;

    execute_action(game, t->action, h, j, chain);
    if (t->self_destruct) execute_action(game, TRIGGER_ACTION_DESTROY_SELF, h, j, chain); // actions[24]
}

void game_on_trigger(Game *game, Player *source, Object *object, int *chain) {
    if (!object || !object->trig) return;
    const TriggerSource j = {
        .object = object,
        .trig = object->trig,
        .team = (int) object->team,
        .interface_id = object->interface_id,
        .position = {object->position.x, object->position.y + object->scale.y * 0.5f, object->position.z},
    };
    trigger_fire(game, source, &j, chain);
}

void game_trigger_ai_death(Game *game, Player *source, GameAI *ai) {
    if (!ai || ai->data.trig.event != TRIGGER_EVENT_ON_AI_DEATH) return; // this.onAIDeath
    const TriggerSource j = {.trig = &ai->data.trig, .position = ai->position}; // isAI, no team / interface
    trigger_fire(game, source, &j, NULL);
}

// ---------------------------------------------------------------- check points / teleporters

// game_set_checkpoint / game_check_teleport / game_spawn_at_checkpoint: shared/src/world.c
// (map-logic WP-B: checkPointData, telCheckPoint, "chp" with the reset payload).

// ---------------------------------------------------------------- per tick

void game_tick_map_objects(Game *game, const float delta_ms) {
    if (!game->ready || !game->map) return;
    Map *map = game->map;

    for (size_t i = 0; i < map->object_count; i++) { // updateTeleporters
        Object *object = map->objects[i];
        if (object->teleporter && object->tel_cooldown > 0.0f) object->tel_cooldown = fmaxf(0.0f, object->tel_cooldown - delta_ms);
    }

    if (!game_is_authority(game) || game->end_timer > 0.0f) return;

    for (size_t i = 0; i < map->object_count; i++) { // the timerObjs loop
        Object *object = map->objects[i];
        if (!event_is(object, TRIGGER_EVENT_ON_TIMER) || object->collision_type == COLLISION_TYPE_NONE) continue; // addHelpers: !noCol
        TriggerData *t = object->trig;
        if (t->timer <= 0.0f || !object->active) continue;
        t->timer -= delta_ms;
        if (t->timer <= 0.0f) {
            t->timer = (float) t->constant_event * 1000.0f;
            game_on_trigger(game, NULL, object, NULL);
        }
    }

    for (size_t n = map->object_count; n-- > 0;) { // updateDestructables over destObjs (the destroyed objects)
        Object *object = map->objects[n];
        if (!object->destroyed || !(object->respawn_random || object->respawn_time_start)) continue; // canRespawn
        if (!object->respawn_time && object->respawn_random) object->respawn_time = (float) rand_int(1, 5000);
        object->respawn_time -= delta_ms;
        if (object->respawn_time <= 0.0f) game_respawn_object(game, object, NULL);
    }
}

// init "dest" / "gates" / "banks": what differs from the map's start state.
void game_send_object_state(Game *game, const int uid) {
    if (!game->ready || !game->map || !game_is_authority(game)) return;
    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *object = game->map->objects[i];
        NetObjectStatePacket packet = {.uid = (int32_t) i};
        if (object->destroyed) {
            packet.kind = NET_OBJECT_DESTROY;
        } else if (object->gate && object->active == object->start_closed) {
            packet.kind = NET_OBJECT_GATE;
            packet.value = object->active;
        } else if (object->bank && object->deposited) {
            packet.kind = NET_OBJECT_BANK;
            packet.value = object->deposited;
        } else {
            continue;
        }
        game_send(game, uid, NET_PACKET_OBJECT_STATE, &packet, sizeof(packet));
    }
}
