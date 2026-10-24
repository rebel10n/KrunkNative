// Krunker v7.1.4 map triggers and interfaces (the trigger runtime itself, onTrigger / conditions /
// actions, is shared/src/triggers_v7.c and its siblings; check points, teleporters and
// changePosition are shared/src/world.c):
//   src/data/triggers.js     the event / action tables
//   src/data/game.js         updateTeleporters, updateDestructables, playSound
//   libs/MapManager.js       GameObject trigger fields, addHelpers (interfaces), resetAll
//   src/data/players.js      the destructible branch of shoot / melee
//   src/data/projectiles.js  the destructible branch of a projectile hit
// Map objects are addressed by their index in Map.objects (Krunker GameObject uid). Krunker's
// map.manager.destObjs list is the `destroyed` flag (an object is in destObjs exactly while it
// is destroyed), map.manager.interfaces are the objects with an interface id and collision.
// Everything that changes game state runs on the authority only (Krunker `io`).
#include <shared.h>
#include <prop_hunt.h> // Prop Hunt: the Seeker penalty
#include <ai.h>
#include <ks_game.h> // v10.0.3 onObjectDamage / onObjectDestroyed
#include "modes/mode_internal.h"
#include "triggers_internal.h"
#include <math.h>
#include <pcg_basic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const g_trigger_events[TRIGGER_EVENT_COUNT] = {
    "onShoot", "onMelee", "onDamage", "onEnterObject", "onDestroy", "onRespawn", "onTimer", "onProjectile",
    "onThrowingMelee", "onPunch", "onHoldMouse", "onCollision", "onStorageEquals", "onStorageGreater",
    "onStorageLesser", "onAIDeath", "onInteractKey", "onPlayerSpawn", "onPlayerKill", "onPlayerDie", "onAIDamage",
    "onNukeStart", "onNukeEnd", "onExitObject", "onCustomValEquals", "onCustomValLesser", "onGameStart",
    "onObjectiveSwitch", "onPlayerDamage", "onAIHalfHealth", "onCustomValGreater", "onAIKillPlayer",
};

#define N TRIGGER_VALUE_NUMBER
#define T TRIGGER_VALUE_TEXT
#define S TRIGGER_VALUE_SOUND
const TriggerActionInfo g_trigger_actions[TRIGGER_ACTION_COUNT] = {
    {"Give Player Score", N}, {"Kill Player", N}, {"Respawn Player", N}, {"Change Player Health", N},
    {"Destroy Interface", N}, {"Toggle Interface Gate", N}, {"Change Interface Health", N},
    {"Teleport Player To Interface", N}, {"Change Interface Deposit Box Amount", N}, {"Kill Opposing Team", N},
    {"Respawn Opposing Team", N}, {"Respawn Destructible Interface", N}, {"Toggle Destructible Interface", N},
    {"Set Player Score", N}, {"Broadcast Sound", S}, {"Clear Player Loadout", N}, {"Broadcast Message", T},
    {"Send Message", T}, {"Swap Player Team", N}, {"Set Player Team", N}, {"Round Time", N},
    {"Reset Interface", N}, {"End Round", N}, {"Refill Player Knife", N}, {"Destroy Self", N},
    {"Increase Interface Storage", N}, {"Decrease Interface Storage", N}, {"Set Interface Storage", N},
    {"Reset Interface Storage", N}, {"Broadcast Popup", T}, {"Send Popup", T}, {"Jump Scare", N},
    {"Reward KR", N}, {"Clear Player Checkpoints", N}, {"Send Sound", S}, {"Explosion", N}, {"Spawn AI", N},
    {"Set Player Value", T}, {"Inc/Dec Player Value", T}, {"Play Animation", T},
    {"Teleport Player to Path Node", N}, {"Teleport Destructible Interface to Path Node", N},
    {"Reset Destructible Interface Position", N}, {"Set Interface Trigger Value", T},
    {"Inc/Dec Interface Trigger Value", T}, {"Set Trigger Value (Self)", T}, {"Inc/Dec Trigger Value (Self)", T},
    {"Change Class", N}, {"Set Interface Deposit Box Amount", N}, {"Save Data", T}, {"Change Primary Weapon", N},
    {"Change Secondary Weapon", N}, {"Set Global Value", T}, {"Change Global Value", T}, {"Stop Sound", S},
    {"Show/Hide GUI Element (All)", N}, {"Show/Hide GUI Element (Player)", N}, {"Load Data", N},
    {"Custom Action", T}, {"Reward Junk", T}, {"Reward Skin", T}, {"Change Ambient Sound", N},
    {"Give Challenge Progress", T}, {"Play Animation (Self)", T}, {"Give Effect", N},
    {"Give Challenge Progress (All)", T}, {"Give Random Effect", N}, {"Play Video Ad", N}, {"Drop Powerup", N},
    {"Spawn Bot at Node", N}, {"Force AI Target", N}, {"Mode Specific", N},
    {"Spawn Entity", T}, {"Despawn Entity", T}, {"Set Entity Value", T}, // v10.0.2
};
#undef N
#undef T
#undef S

// config.emitterSounds (deob-10.0.3.js:21278: 74 entries, v7.1.4 73 + scare_1; index 16 changed from v3.1.1's jumpS_0 to scare_0)
const char *const g_trigger_emitter_sounds[TRIGGER_EMITTER_SOUND_COUNT] = {
    "nuke_0", "siren_0", "explosion", "fart_0", "taunt_0", "taunt_1", "taunt_2", "taunt_3", "buy_0", "buy_1",
    "case_0", "case_1", "cheer_0", "reward", "store", "tick_0", "scare_0", "cough_1", "cough_2", "taunt_4",
    "ambient_1", "ambient_2", "ambient_3", "ambient_4", "ambient_5", "ambient_6", "ambient_7", "ambient_8",
    "beep_0", "bo_plant", "bo_diff", "collect_1", "bounce_0", "headshot_0", "hit_0", "impact_0", "jump_0",
    "jump_1", "land_0", "punch_0", "punch_1", "slide_0", "spray", "step_0", "step_1", "step_2", "whizz_0",
    "whizz_1", "gclick_0", "pick_0", "pick_1", "build_0", "break_wood", "bhop_check", "bhop_reset", "gong_0",
    "dragon_0", "taunt_5", "spray_1", "stepwo_0", "stepwo_1", "stepwat_0", "stepwat_1", "stepsa_0", "stepsa_1",
    "stepsno_0", "stepsno_1", "stepsno_2", "bounce_1", "zap_0", "jug_0", "crit_0", "melee_equip",
    "scare_1", // v10.0.3 (deob-10.0.3.js:21278)
};
// Old maps keep their "es" picks: v3.1.1's 51 names and v7.1.4's 73 are this list's first 51 / 73
// with one rename, 16 jumpS_0 -> scare_0 (v3.1.1 shipped no jumpS_0.mp3, so a v3 map's 16 played
// nothing; v7.1.4 and v10.0.3 play scare_0 for it, as KrunkNative does). config.networkSounds,
// the list sent as an index, is shared/include/network_sounds.h.

// config.soundScapes {Default: 1, City: 2, ... Festive: 19}: Change Ambient Sound's tc indexes the keys
const int g_trigger_sound_scapes[TRIGGER_SOUND_SCAPE_COUNT] = {1, 2, 3, 4, 5, 6, 7, 8, 11, 15, 16, 17, 18, 19};

void trigger_sound_name(const int sound, const int asset, char *out, const size_t size) {
    if (!out || !size) return;
    if (asset) snprintf(out, size, "asset:%d", sound); // a user asset sound (raw id)
    else snprintf(out, size, "%s", g_trigger_emitter_sounds[sound >= 0 && sound < TRIGGER_EMITTER_SOUND_COUNT ? sound : 0]);
}

// JS truthiness / parseInt / parseFloat of a map field (NaN answers 0 with *ok 0)
static int js_truthy(const cJSON *v) {
    if (!v || cJSON_IsNull(v) || cJSON_IsFalse(v)) return 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0.0 && !isnan(v->valuedouble);
    if (cJSON_IsString(v)) return v->valuestring && v->valuestring[0];
    return 1;
}

static double js_parse(const cJSON *v, const int integer, int *ok) {
    *ok = 0;
    if (cJSON_IsNumber(v) && isfinite(v->valuedouble)) {
        *ok = 1;
        return integer ? trunc(v->valuedouble) : v->valuedouble;
    }
    if (!cJSON_IsString(v) || !v->valuestring) return 0.0;
    const char *s = v->valuestring;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    char *end;
    const double x = integer ? (double) strtol(s, &end, 10) : strtod(s, &end);
    *ok = end != s && isfinite(x);
    return *ok ? x : 0.0;
}

// parseInt(v) || fallback / parseFloat(v) || fallback
static double js_number_or(const cJSON *v, const int integer, const double fallback) {
    int ok;
    const double x = js_parse(v, integer, &ok);
    return ok && x != 0.0 ? x : fallback;
}

static float emitter_limit(const double v, const float min, const float max) { // UTILS.limitMM
    return (float) (v < min ? min : v > max ? max : v);
}

int trigger_emitter_parse(const cJSON *object, TriggerEmitter *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!cJSON_IsObject(object)) return 0;
    const cJSON *esa = cJSON_GetObjectItem(object, "esa");
    const int sound = (int) js_number_or(js_truthy(esa) ? esa : cJSON_GetObjectItem(object, "es"), 1, 0.0); // parseInt(esa || es) || 0
    out->asset = esa && !cJSON_IsNull(esa);                                                                 // esa != null
    trigger_sound_name(sound, out->asset, out->sound, sizeof(out->sound));
    out->volume = emitter_limit(js_number_or(cJSON_GetObjectItem(object, "ev"), 0, 1.0), TRIGGER_EMITTER_VOLUME_MIN, TRIGGER_EMITTER_VOLUME_MAX);
    out->rate = emitter_limit(js_number_or(cJSON_GetObjectItem(object, "er"), 0, 1.0), TRIGGER_EMITTER_RATE_MIN, TRIGGER_EMITTER_RATE_MAX);
    out->distance = emitter_limit(js_number_or(cJSON_GetObjectItem(object, "ed"), 1, 0.0), 0.0f, 5000.0f);
    out->distance_model = (int) emitter_limit(js_number_or(cJSON_GetObjectItem(object, "edm"), 1, 0.0), 0.0f, 2.0f);
    out->rolloff = emitter_limit(js_number_or(cJSON_GetObjectItem(object, "ero"), 0, 0.0), 0.0f, 1.0f);
    return 1;
}

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

int trigger_map_uid(const Game *game, const Object *object) {
    for (size_t i = 0; game->map && object && i < game->map->object_count; i++) {
        if (game->map->objects[i] == object) return (int) i;
    }
    return -1;
}

static int map_object_uid(const Game *game, const Object *object) {
    return trigger_map_uid(game, object);
}

static int teams_allow(const Game *game, const int team, const Player *player) {
    return !team || !game->mode || !game->mode->config.teams || (player && team == player->team);
}

int trigger_team_check(const Game *game, const int team, const Player *player) {
    return teams_allow(game, team, player);
}

static int rand_int(const int min, const int max) { // UTILS.randInt (inclusive)
    return max <= min ? min : min + (int) pcg32_boundedrand((uint32_t) (max - min + 1));
}

int trigger_rand_int(const int min, const int max) {
    return rand_int(min, max);
}

int trigger_wait_blocks(const Game *game) { // !(!waitTimers || waitTimers[0].contTime)
    return game->mode && game->mode->wait_timer_count > 0 && !game->mode->wait_timers[0].cont_time;
}

int game_object_event(const Object *object) {
    return trigger_event_of(object);
}

static int event_is(const Object *object, const int event) {
    return game_object_event(object) == event;
}

// map.manager.interfaces: addHelpers registers objects with an interface id and collision.
int trigger_is_interface(const Object *object) {
    return object->interface_id && object->collision_type != COLLISION_TYPE_NONE;
}

static void send_object_state(Game *game, const int uid, const int kind, const int value) {
    trigger_send_object_state(game, uid, kind, value);
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

// io_foundJunk (index.js:1563-1576): io_addChat(-1, null, "<span style='color:#fff'>You</span>
// found <span style='color:#cc51f8'>[" + parseFloat(junk).toFixed(2) + "]</span> Junk", 1). Called
// by the server when kn_brain accepted the credit (doc/v10/junk.md).
void game_found_junk(Game *game, const Player *player, const double amount) {
    if (!game_is_authority(game) || !player) return;
    char text[64];
    snprintf(text, sizeof(text), "You found [%.2f] Junk", amount);
    NetChatPacket packet;
    net_packet_chat(&packet, -1, NET_CHAT_FOUND_JUNK, NULL, text);
    game_send(game, player->uid, NET_PACKET_CHAT, &packet, sizeof(packet));
}

// ---------------------------------------------------------------- positions

// game_change_position (players.changePosition): shared/src/world.c (map-logic WP-B).

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
    // v7.1.4: onRespawn fires with destroyedBy, which may be null (v3.1.1 required destroyedBy)
    if (event_is(object, TRIGGER_EVENT_ON_RESPAWN)) game_on_trigger(game, destroyed_by(game, object), object, chain);
    object->destroyed_by = -1;
}

void game_set_gate(Game *game, Object *object, const int active) {
    object->active = (unsigned char) !!active;
    send_object_state(game, map_object_uid(game, object), NET_OBJECT_GATE, object->active); // "gte"
}

// The health branch of players.shoot / melee and projectiles.js, then the hit events (v7.1.4: the
// damage needs a positive amount and no blocking waitTimer; a projectile also needs its owner).
void game_object_hit(Game *game, Object *object, Player *dealer, const float damage, const int thrown_melee, const int kind) {
    if (!game_is_authority(game) || !object) return;
    // Prop Hunt: a Seeker's shot or swing on a prop object of the map costs it 10 health (:172554, :173031)
    if (dealer && (kind == GAME_HIT_SHOT || kind == GAME_HIT_MELEE)) game_prop_seeker_hit(game, dealer, object);
    const int can_damage = damage > 0.0f && (kind != GAME_HIT_PROJECTILE || dealer); // projectile: owner.isPlayer && I > 0
    if (can_damage && !trigger_wait_blocks(game) && object->health) {
        object->health -= damage;
        // v10.0.3 damageObject (deob-10.0.3.js:220473-220481): the server script's onObjectDamage(sid, interface || 0,
        // playerId, dmg) after the health drop, onObjectDestroyed(sid, interface || 0, playerId) after onDestroy
        const int sid = map_object_uid(game, object);
        ks_game_on_object_damage(game, sid, object->interface_id, dealer, damage);
        if (dealer) {
            const NetHitPacket hit = {.target_uid = -1}; // IO.send(player.id, "4"): hitmarker only
            game_send(game, dealer->uid, NET_PACKET_HIT, &hit, sizeof(hit));
        }
        if (object->health <= 0.0f) {
            game_destroy_object(game, object, dealer);
            if (event_is(object, TRIGGER_EVENT_ON_DESTROY)) game_on_trigger(game, dealer, object, NULL);
            ks_game_on_object_destroyed(game, sid, object->interface_id, dealer);
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
        case GAME_HIT_PROJECTILE: // only for a player's projectile (owner.isPlayer)
            fire = dealer && (fire || event == TRIGGER_EVENT_ON_SHOOT || event == TRIGGER_EVENT_ON_PROJECTILE ||
                              (thrown_melee && event == TRIGGER_EVENT_ON_THROWING_MELEE));
            break;
        default:
            break;
    }
    if (fire) game_on_trigger(game, dealer, object, NULL);
}

// ---------------------------------------------------------------- onTrigger

// game.onTrigger for a TriggerSource built by game_on_trigger / game_trigger_ai_death below: the
// v7 runtime (shared/src/triggers_v7.c) takes the source's uid / interface target from it.
static void trigger_fire(Game *game, Player *h, const TriggerSource *j, int *chain) {
    TriggerSource source = *j;
    if (source.object) {
        source.uid = map_object_uid(game, source.object);
        source.interface_target = source.trig->interface_target;
    } else if (!source.ai) { // a v3-shaped AI source without its GameAI (isAI, no interface)
        source.uid = -1;
        source.interface_target = source.trig->interface_target;
    }
    trigger_run(game, h, &source, chain ? *chain : -1);
}

void game_on_trigger(Game *game, Player *source, Object *object, int *chain) {
    if (!object || !object->trig) return;
    const TriggerSource j = {
        .object = object,
        .trig = object->trig,
        .team = (int) object->team,
        .interface_id = object->interface_id,
        .position = {object->position.x, object->position.y + object_box_size(object).y * 0.5f, object->position.z},
    };
    trigger_fire(game, source, &j, chain);
}

void game_trigger_ai_death(Game *game, Player *source, GameAI *ai) {
    if (!ai || ai->data.trig.event != TRIGGER_EVENT_ON_AI_DEATH) return; // this.onAIDeath
    trigger_ai_event(game, ai, TRIGGER_EVENT_ON_AI_DEATH, source);       // isAI: no teamCheck (triggers_v7.c)
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
    // (the timerObjs loop runs at its v7 update position: trigger_queue_update, triggers_v7.c)

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
        } else if (object->start_destroyed) { // alive although it starts destroyed (rsc, a respawn): syncAll(dest)
            packet.kind = NET_OBJECT_RESPAWN;
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
    trigger_send_join_state(game, uid); // gbl, asnd, pkups (shared/src/triggers_v7.c)
}
