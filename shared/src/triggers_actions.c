// Krunker v7.1.4 trigger actions 0-71 (data/triggers.js:321-1610; doc/v7/map-logic.md §3.7) and
// the v10.0.2 entity actions 72-74 (doc/v10/map-format.md §5; v10 map dialect only):
// actions[ta].execute(game, serverIO, player h, trigger j, action a, chain), in table order.
//
// Targets are the map interfaces d (map.manager.interfaces: objects with an "in" and collision,
// uid order) with (j.isAI || d != j) && d.interface != j.interface && d.interface == j.interfaceT,
// iterated while triggerChains[chain] > 0. The value of a hasCust action is getActionValue
// (trigger_action_value): an undefined value skips the action (or that target).
// Backend-only actions (doc/v7/decisions.md "Backend-only triggers"): 32 Reward KR, 49 Save Data
// and 57 Load Data go to the account backends of GameHooks (reward_kr, storage_save,
// storage_load: kn_brain or the offline sandbox's file store, server/src/ks_storage.c; NULL =
// nothing); 62 / 65 Give Challenge Progress tally the map's key for the account's challenges
// (GameHooks.progress -> the stats commit); 59 Reward Junk only prints the finder's chat line
// (reward_junk: KN has no junk wallet); 60 Reward Skin and 67 video ads do nothing (no store
// backend in the game server).
// Select-index actions (47, 50, 51, 61, 64) index the runtime tables directly with the raw value,
// as v7 does (§3.7 "Select-index quirk").
#include <shared.h>
#include "triggers_internal.h"
#include "modes/mode_internal.h"
#include <modes_v7.h>
#include <spawnables.h>
#include <world.h>
#include <ks_storage_data.h> // WP6: Save Data keys / values
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ruleset.h>
#include <bots.h>
#include <junk.h>

#define CHAIN_OK(game, chain) trigger_chain_left(game, chain)

// The trigger text of an action (trigConstTx), NULL when it has none (an empty or dropped "tct").
static const char *action_text(const TriggerActionData *a) {
    return a->has_text && a->text[0] ? a->text : NULL;
}

// String.trim() of `text` into out
static void trigger_trim(const char *text, char *out, const size_t size) {
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') text++;
    size_t n = strlen(text);
    while (n && (text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\n' || text[n - 1] == '\r')) n--;
    if (n >= size) n = size - 1;
    memcpy(out, text, n);
    out[n] = 0;
}

// (text + "").split("|"): [0] and [1] trimmed into first / second ("" when missing). 0 without text.
static int trigger_split_text(const char *text, char *first, char *second, const size_t size) {
    first[0] = second[0] = 0;
    if (!text) return 0;
    char part[TRIGGER_TEXT_MAX + 1];
    const char *bar = strchr(text, '|');
    const size_t n = bar ? (size_t) (bar - text) : strlen(text);
    snprintf(part, sizeof(part), "%.*s", (int) n, text);
    trigger_trim(part, first, size);
    if (bar) {
        const char *end = strchr(bar + 1, '|');
        snprintf(part, sizeof(part), "%.*s", (int) (end ? (size_t) (end - bar - 1) : strlen(bar + 1)), bar + 1);
        trigger_trim(part, second, size);
    }
    return 1;
}

static int targets(const TriggerSource *j, const Object *d) {
    return trigger_is_interface(d) && (!j->object || d != j->object) && d->interface_id != j->interface_id &&
           d->interface_id == j->interface_target;
}

static void send_object_state(Game *game, const Object *object, const int kind, const int value) {
    trigger_send_object_state(game, trigger_map_uid(game, object), kind, value);
}

static void send_chat(Game *game, const int target, const char *text) {
    NetChatPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.uid = -1;
    packet.kind = NET_CHAT_SERVER; // "ch" -1, null, text, 2
    snprintf(packet.text, sizeof(packet.text), "%s", text);
    game_send(game, target, NET_PACKET_CHAT, &packet, sizeof(packet));
}

static void send_popup(Game *game, const int target, const char *text) {
    NetPopupPacket packet;
    memset(&packet, 0, sizeof(packet));
    snprintf(packet.text, sizeof(packet.text), "%s", text); // "am" [text, null, 0, 1]
    game_send(game, target, NET_PACKET_POPUP, &packet, sizeof(packet));
}

static int has_account(const Game *game, const Player *player);

// 59 Reward Junk (triggers.js:1392-1404): (!isSandbox && isPubType() && h.account) and v =
// parseFloat(trigConstTx || 0) with !!v && !(v >= 100) -> game.sendUserJunk("trigger", h, v): the
// account is credited (doc/v10/junk.md) and the finder's "You found [v] Junk" line follows the
// brain's acceptance (server, accounts.c). [KN] A negative v is not credited (v7 would subtract it;
// the wallet never goes below 0). Bots (never an account in v7) get nothing.
static void reward_junk(Game *game, const Player *h, const TriggerActionData *a) {
    if (!h || game_player_is_bot(game, h)) return;
    const char *text = action_text(a);
    double v = 0.0;
    if (text) {
        while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') text++;
        const char *digits = text + (*text == '+' || *text == '-');
        // parseFloat reads decimals only: "0x10" is 0, "inf" / "nan" NaN (strtod takes all three)
        if (!(digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))) v = strtod(text, NULL);
    }
    if (isnan(v) || v == 0.0 || v >= 100.0) return;
    if (game->is_sandbox || !game_is_pub_type(game) || !has_account(game, h)) return; // !isSandbox && isPubType() && account
    game_junk_credit(game, h, junk_hundredths(v), "trigger");
}

static int has_account(const Game *game, const Player *player) {
    AccountInfo account;
    return player && trigger_account(game, player, &account) && account.has_account;
}

// 49 Save Data (triggers.js:1178-1204): a player with an account, the "Data Name" lowercased and
// cut to 12 characters, the Save Type option (trigConst2 clamped to 0..5) and the value
// (getActionValue, default trigConst || 1); the name "time" saves the player's gameTime, else the
// round clock (gameTimerP, ms). Sent as storageUpdt's raw "key,value" (private scope).
// [KN] v7's sandbox skipped it; KN's sandbox has the offline file store, so it saves there. The
// key also loses , ' " (v7 put it into the comma-separated raw data as is, which split it).
static void save_data(Game *game, Player *h, const TriggerSource *j, const TriggerActionData *a) {
    const char *text = action_text(a);
    if (!game->hooks.storage_save || !text || !has_account(game, h)) return;
    char key[KS_STORAGE_TRIGGER_KEY_MAX + 1];
    size_t n = 0;
    for (const char *c = text; *c && n < KS_STORAGE_TRIGGER_KEY_MAX; c++) { // tct is ASCII (MMG:1992-2003)
        if (*c == ',' || *c == '\'' || *c == '"') continue;
        key[n++] = (char) (*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
    }
    key[n] = 0;
    if (!n) return;
    const int type = a->constant2 < 0 ? 0 : a->constant2 > 5 ? 5 : a->constant2; // UTILS.limitMM(.., 0, 5)
    double number;
    if (!strcmp(key, "time")) {
        const double t = h->game_time != 0.0f ? (double) h->game_time : (double) game->round_timer * 1000.0;
        if (!(t == t) || t == 0.0) return; // v7: 0 / not a number -> undefined: nothing saved
        number = t;
    } else {
        int value;
        if (!trigger_action_value(game, h, j, NULL, a, a->constant ? a->constant : 1, &value)) return;
        number = value;
    }
    char value[40];
    ks_storage_format_number(number, value);
    game->hooks.storage_save(game, h, key, type, value);
}

static const Weapon *weapon_at(const Game *game, const int id) {
    const size_t count = game->weapon_count ? game->weapon_count : WEAPON_COUNT;
    const Weapon *const *all = game->weapons ? (const Weapon *const *) game->weapons : rules_weapons();
    return id >= 0 && (size_t) id < count ? all[id] : NULL;
}

// players.changePosition(h, x, y, z, recon, stop, dir, true): the move, "l" for the owner and
// "chgp" for everyone.
static void change_position(Game *game, Player *h, const vec3 feet, const int recon, const int stop) {
    game_change_position(game, h, feet, recon, stop, 0, 0);
    trigger_send_change_position(game, h, recon, stop, 0, 0);
}

// h.checkPoint && objects[checkPoint] ? that object (its GameObject centre) : getSpawnPoint(team, h)
static void respawn_player(Game *game, Player *h) {
    vec3 at = h->position;
    const Map *map = game->map;
    if (h->checkpoint > 0 && map && (size_t) h->checkpoint <= map->object_count) {
        const Object *cp = map->objects[h->checkpoint - 1];
        at = (vec3) {cp->position.x, cp->position.y + cp->scale.y * 0.5f, cp->position.z};
    } else {
        int direction;
        game_get_spawn_point(game, h, &at, &direction);
    }
    change_position(game, h, at, 1, 0); // changePosition(h, x, y, z, true, false, undefined, true)
}

// destroyObj: active false; a gate closes ("gte"), anything else is destroyed ("do")
static void destroy_target(Game *game, Object *d, Player *h) {
    if (d->gate) {
        game_set_gate(game, d, 0);
    } else {
        game_destroy_object(game, d, h);
    }
}

static void fire_if(Game *game, Player *h, Object *d, const int fire, int chain) {
    if (fire) game_on_trigger(game, h, d, &chain);
}

// players.score(h, v, 0, true): Set Player Score (no "5"; the public score cap)
static void set_score(Game *game, Player *h, int value) {
    const int before = h->score;
    h->score = value;
    if (h->score > GAME_SCORE_CAP) {
        h->score = GAME_SCORE_CAP;
        value = 0;
    }
    mode_on_score(game, h, value, 0);
    game->leaders_dirty = 1;
    // challenges.update(game, "score", max(0, score - before)): a set counts only what it added (deob-10.0.4.js:175948)
    if (h->score > before) game_challenge(game, h, "score", h->score - before, -1, -1);
}

static void round_time(Game *game, const int value) {
    GameMode *mode = game->mode;
    if (!mode || mode->skip || game->end_timer > 0.0f || game->round_endless) return; // isNumber(gameTimer)
    float *clock = mode->config.timed ? &mode->round_clock : &game->round_timer;
    *clock += (float) value / 1000.0f; // gameTimer is in ms
    if (*clock <= 0.0f) mode->skip = 1;  // gameTimer = "skip"
}

// The name and flags of an action's triggerSound [name, vol, rate, emitFrom, isAsset].
static void sound_name(const TriggerActionData *a, char *name, const size_t size) {
    if (a->asset_sound) snprintf(name, size, "asset:%d", a->sound); // a user asset sound (raw id)
    else snprintf(name, size, "%s", g_trigger_emitter_sounds[a->sound >= 0 && a->sound < TRIGGER_EMITTER_SOUND_COUNT ? a->sound : 0]);
}

#define NET_SOUND_UID_2D (-2) // NetSoundPacket.uid: a 2D sound ("so"), not positional

static void play_sound(Game *game, const int target, const Player *h, const TriggerSource *j, const TriggerActionData *a) {
    NetSoundPacket packet;
    memset(&packet, 0, sizeof(packet));
    sound_name(a, packet.name, sizeof(packet.name));
    packet.volume = a->volume ? a->volume : 1.0f;
    packet.rate = a->rate ? a->rate : 1.0f;
    if (a->emit_from == 2) { // "so" name, vol, 1, 0, 0, 0, rate: global (2D)
        packet.uid = NET_SOUND_UID_2D;
    } else { // "s": at the player (ef && h, head height) or at the trigger
        const int at_player = a->emit_from && h;
        const vec3 at = at_player ? (vec3) {h->position.x, h->position.y + h->height, h->position.z} : j->position;
        packet.uid = at_player ? h->uid : -1;
        packet.position[0] = roundf(at.x);
        packet.position[1] = roundf(at.y);
        packet.position[2] = roundf(at.z);
    }
    game_send(game, target, NET_PACKET_SOUND, &packet, sizeof(packet));
}

// The class's loadout (setClass: primary, the picked secondary when the class has one, the knife),
// health and speed, for Change Class mid-life (updateItems(game, io, true, ..., true)).
static void apply_class(Game *game, Player *h, const int class_index) {
    const ClassConfig *classes = game->classes ? game->classes : rules_classes();
    const ClassConfig *c = &classes[class_index];
    h->class_index = class_index;
    int loadout[3];
    int count = 0;
    const int primary = c->loadout ? c->loadout[0] : 1;
    if (weapon_at(game, primary)) loadout[count++] = primary;
    const Weapon *secondary = weapon_at(game, h->secondary_index);
    if (c->secondary && !game->config.no_secondaries) {
        const int sec = secondary && secondary->secondary ? h->secondary_index : WEAPON_ID_PISTOL;
        if (weapon_at(game, sec) && sec != primary) loadout[count++] = sec;
    }
    if (weapon_at(game, WEAPON_ID_KNIFE) && primary != WEAPON_ID_KNIFE) loadout[count++] = WEAPON_ID_KNIFE;
    if (!count) return;
    game_player_changing(game, h, 0);
    mode_set_player_loadout(h, loadout, count, 0);
    const int mode_health = game->mode ? game->mode->config.health : 0;
    const float health_mlt = game->config.health_mlt > 0.0f ? game->config.health_mlt : 1.0f;
    h->max_health = mode_health > 0 ? mode_health : (int) ((float) c->health * health_mlt);
    h->health = (float) h->max_health;
    h->speed = c->speed ? c->speed : 1.0f;
    game_player_changing(game, h, 1);
    mode_send_loadout(game, h);
    const NetTeamPacket team = {.uid = h->uid, .team = h->team, .health = h->health, .max_health = h->max_health};
    game_send(game, NET_TARGET_ALL, NET_PACKET_TEAM, &team, sizeof(team)); // the health of the new class
}

// Change Primary / Secondary Weapon: replace the first loadout weapon of that kind, else insert.
static void change_weapon(Game *game, Player *h, const int id, const int secondary) {
    const Weapon *w = weapon_at(game, id);
    if (!w || !h->loadout) return;
    if (secondary ? !w->secondary : (w->secondary || w->melee)) return;
    int ids[NET_LOADOUT_MAX + 1];
    int count = 0, replaced = 0;
    for (int i = 0; i < player_loadout_base(h) && count < NET_LOADOUT_MAX; i++) {
        const Weapon *have = weapon_at(game, h->loadout[i]);
        ids[count] = h->loadout[i];
        const int same_kind = have && (secondary ? have->secondary : !have->secondary && !have->melee && !have->equipment);
        if (!replaced && same_kind) {
            if (h->loadout[i] == id) return;
            ids[count] = id;
            replaced = 1;
        }
        count++;
    }
    if (!replaced) {
        if (count >= NET_LOADOUT_MAX) return;
        const int at = secondary ? (count > 1 ? 1 : 0) : 0; // loadout.splice(at, 0, id)
        for (int i = count; i > at; i--) ids[i] = ids[i - 1];
        ids[at] = id;
        count++;
    }
    game_player_changing(game, h, 0);
    mode_set_player_loadout(h, ids, count, 1); // updateLoadout(game, loadoutIndex, true, loadout)
    game_player_changing(game, h, 1);
    mode_send_loadout(game, h); // broadcast("inat", sid, loadout, loadoutIndex)
}

// The interface trigger value actions 43 / 44: the target's custVals, then its onCustomVal* events
// against its Event Value.
static int set_interface_value(Game *game, Player *h, const TriggerSource *j, const TriggerActionData *a, const int add, const int chain) {
    const char *key = action_text(a);
    if (!key) return 0;
    Map *map = game->map;
    for (size_t l = 0; l < map->object_count && CHAIN_OK(game, chain); l++) {
        Object *d = map->objects[l];
        if (!targets(j, d)) continue;
        TriggerValueSet *set = trigger_object_values_mut(game, d, 1);
        TriggerValue *slot = set ? trigger_values_slot(set, key) : NULL;
        if (!slot) return 0; // custValLimit: the whole action stops
        if (add) trigger_value_add(slot, a->constant); // a string value appends (JS +=)
        else trigger_value_set_number(slot, a->constant);
        const double threshold = d->trig ? d->trig->constant_event : 0;
        fire_if(game, h, d, slot->value == threshold && trigger_has_event(d, TRIGGER_EVENT_ON_CUSTOM_VAL_EQUALS), chain);
        fire_if(game, h, d, slot->value > threshold && trigger_has_event(d, TRIGGER_EVENT_ON_CUSTOM_VAL_GREATER), chain);
        fire_if(game, h, d, slot->value < threshold && trigger_has_event(d, TRIGGER_EVENT_ON_CUSTOM_VAL_LESSER), chain);
    }
    return 1;
}

static void storage_action(Game *game, Player *h, const TriggerSource *j, const TriggerActionData *a, const int chain) {
    Map *map = game->map;
    for (size_t l = 0; l < map->object_count && CHAIN_OK(game, chain); l++) {
        Object *d = map->objects[l];
        if (!targets(j, d) || !d->trig) continue;
        const int equals = trigger_has_event(d, TRIGGER_EVENT_ON_STORAGE_EQUALS);
        const int greater = trigger_has_event(d, TRIGGER_EVENT_ON_STORAGE_GREATER);
        const int lesser = trigger_has_event(d, TRIGGER_EVENT_ON_STORAGE_LESSER);
        // Reset Interface Storage only looks for Equals / Greater targets (v3 quirk, kept)
        if (a->action == TRIGGER_ACTION_RESET_STORAGE ? !(equals || greater) : !(equals || greater || lesser)) continue;
        if (a->action == TRIGGER_ACTION_RESET_STORAGE) {
            d->trig->storage = 0;
        } else {
            int value;
            if (!trigger_action_value(game, h, j, d, a, a->constant, &value)) continue;
            if (a->action == TRIGGER_ACTION_INCREASE_STORAGE) d->trig->storage += value;
            else if (a->action == TRIGGER_ACTION_DECREASE_STORAGE) d->trig->storage -= value;
            else d->trig->storage = value;
        }
        const int storage = d->trig->storage, threshold = d->trig->constant_event;
        fire_if(game, h, d, storage == threshold && equals, chain);
        fire_if(game, h, d, storage > threshold && greater, chain);
        fire_if(game, h, d, storage < threshold && lesser, chain);
    }
}

void trigger_action_execute(Game *game, Player *h, const TriggerSource *j, const TriggerActionData *a, const int chain) {
    if (!game || !game->map || !j || !a) return;
    Map *map = game->map;
    int value;

    switch (a->action) {
        case TRIGGER_ACTION_GIVE_SCORE:
            if (h && trigger_action_value(game, h, j, NULL, a, a->constant ? a->constant : 1, &value)) player_score(h, value, 0);
            break;
        case TRIGGER_ACTION_KILL_PLAYER:
            if (h && !h->god_mode) player_kill(h, NULL, NULL, 0);
            break;
        case TRIGGER_ACTION_RESPAWN_PLAYER:
            if (h) respawn_player(game, h);
            break;
        case TRIGGER_ACTION_CHANGE_HEALTH:
            if (h && trigger_action_value(game, h, j, NULL, a, a->constant, &value) &&
                player_change_health(h, NULL, -(float) value, 0)) player_kill(h, NULL, NULL, 0);
            break;
        case TRIGGER_ACTION_DESTROY_INTERFACE:
            for (size_t l = 0; l < map->object_count && CHAIN_OK(game, chain); l++) {
                Object *d = map->objects[l];
                if (!targets(j, d) || !(d->health || d->gate || (d->can_destroy && !d->destroyed))) continue;
                destroy_target(game, d, h);
                fire_if(game, h, d, trigger_has_event(d, TRIGGER_EVENT_ON_DESTROY) || trigger_has_event(d, TRIGGER_EVENT_ON_DAMAGE), chain);
            }
            break;
        case TRIGGER_ACTION_TOGGLE_GATE:
            for (size_t l = 0; l < map->object_count; l++) { // (no chain check)
                Object *d = map->objects[l];
                if (targets(j, d) && d->gate) game_set_gate(game, d, !d->active);
            }
            break;
        case TRIGGER_ACTION_CHANGE_INTERFACE_HEALTH:
            for (size_t l = 0; l < map->object_count && CHAIN_OK(game, chain); l++) {
                Object *d = map->objects[l];
                if (!targets(j, d) || !d->health || !d->can_destroy || d->destroyed) continue; // v7: canDest && !destroyed
                if (!trigger_action_value(game, h, j, d, a, a->constant, &value)) continue;
                d->health += (float) value; // health -= -value
                if (d->health <= 0.0f) {
                    game_destroy_object(game, d, h);
                    fire_if(game, h, d, trigger_has_event(d, TRIGGER_EVENT_ON_DESTROY), chain);
                }
                fire_if(game, h, d, trigger_has_event(d, TRIGGER_EVENT_ON_DAMAGE) && a->constant < 0, chain); // (trigConst || 0) < 0
            }
            break;
        case TRIGGER_ACTION_TELEPORT_TO_INTERFACE:
            if (!h) break;
            for (size_t l = 0; l < map->object_count && CHAIN_OK(game, chain); l++) {
                Object *d = map->objects[l];
                if (targets(j, d)) change_position(game, h, d->position, 1, 1); // (d.x, d.y - d.height, d.z, true, true, undefined, true): the last one wins
            }
            break;
        case TRIGGER_ACTION_CHANGE_DEPOSIT:
        case TRIGGER_ACTION_SET_DEPOSIT:
            for (size_t l = 0; l < map->object_count && CHAIN_OK(game, chain); l++) {
                Object *d = map->objects[l];
                if (!targets(j, d) || !d->bank) continue;
                if (!trigger_action_value(game, h, j, d, a, a->constant, &value)) continue;
                d->deposited = a->action == TRIGGER_ACTION_SET_DEPOSIT ? value : d->deposited + value;
                send_object_state(game, d, NET_OBJECT_BANK, d->deposited); // "bnk"
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
            for (size_t l = 0; l < map->object_count && CHAIN_OK(game, chain); l++) {
                Object *d = map->objects[l];
                if (!targets(j, d) || !(d->start_health || d->can_destroy) || !d->destroyed) continue;
                int key = chain;
                game_respawn_object(game, d, &key); // "ro", onRespawn(destroyedBy) (no destroyedBy needed in v7)
            }
            break;
        case TRIGGER_ACTION_TOGGLE_DESTRUCTIBLE:
            for (size_t l = 0; l < map->object_count && CHAIN_OK(game, chain); l++) {
                Object *d = map->objects[l];
                if (!targets(j, d) || !(d->start_health || d->can_destroy)) continue;
                if (!d->destroyed) {
                    game_destroy_object(game, d, h);
                    fire_if(game, h, d, trigger_has_event(d, TRIGGER_EVENT_ON_DESTROY) || trigger_has_event(d, TRIGGER_EVENT_ON_DAMAGE), chain);
                } else {
                    int key = chain;
                    game_respawn_object(game, d, &key);
                }
            }
            break;
        case TRIGGER_ACTION_SET_SCORE:
            if (h && trigger_action_value(game, h, j, NULL, a, a->constant, &value)) set_score(game, h, value);
            break;
        case TRIGGER_ACTION_BROADCAST_SOUND:
            play_sound(game, NET_TARGET_ALL, h, j, a);
            break;
        case TRIGGER_ACTION_CLEAR_LOADOUT:
            if (h && h->loadout && !(h->loadout_size == 1 && h->loadout[0] == WEAPON_ID_KNIFE)) {
                const int knife = WEAPON_ID_KNIFE;
                mode_set_player_loadout(h, &knife, 1, 0); // h.updateLoadout(game, 0, true, [12])
                mode_send_loadout(game, h);               // "inat" sid, [12], 0
            }
            break;
        case TRIGGER_ACTION_BROADCAST_MESSAGE:
            if (action_text(a)) send_chat(game, NET_TARGET_ALL, a->text);
            break;
        case TRIGGER_ACTION_SEND_MESSAGE:
            if (h && action_text(a)) send_chat(game, h->uid, a->text);
            break;
        case TRIGGER_ACTION_SWAP_TEAM:
            if (h) mode_update_team(game, h, h->team == 2 ? 1 : 2);
            break;
        case TRIGGER_ACTION_SET_TEAM:
            if (h) mode_update_team(game, h, a->constant < 1 ? 1 : a->constant > TRIGGER_TEAM_LIMIT ? TRIGGER_TEAM_LIMIT : a->constant);
            break;
        case TRIGGER_ACTION_ROUND_TIME:
            round_time(game, a->constant);
            break;
        case TRIGGER_ACTION_RESET_INTERFACE:
            for (size_t l = 0; l < map->object_count && CHAIN_OK(game, chain); l++) {
                Object *d = map->objects[l];
                if (!targets(j, d)) continue;
                if (d->destroyed) {
                    int key = chain;
                    game_respawn_object(game, d, &key);
                }
                if (d->trig) {
                    d->trig->timer = (float) (d->trig->constant_event ? d->trig->constant_event : 1) * 1000.0f;
                    d->trig->triggered = 0;
                    d->trig->use_count = 0;
                }
                d->health = d->start_health;
                d->destroyed = 0;
                // v7 sets active without telling the clients; a closed gate it opens would stay closed
                // there (v7 bug, fixed: the gate state goes out)
                if (d->gate && !d->active) game_set_gate(game, d, 1);
                d->active = 1;
            }
            break;
        case TRIGGER_ACTION_END_ROUND:
            if (game->mode) game->mode->skip = 1; // gameTimer = "skip"
            break;
        case TRIGGER_ACTION_REFILL_KNIFE:
            if (!h) break;
            h->can_throw = 1; // refillPlayer(h): refillKnife + send("rfl")
            game_send(game, h->uid, NET_PACKET_REFILL, NULL, 0);
            break;
        case TRIGGER_ACTION_DESTROY_SELF: {
            Object *self = j->object;
            if (!self || !(self->health || self->gate || self->can_destroy) || !self->active) break; // (AIs: no map object)
            destroy_target(game, self, h);
            fire_if(game, h, self, !j->trig->self_destruct &&
                                       (trigger_has_event(self, TRIGGER_EVENT_ON_DESTROY) || trigger_has_event(self, TRIGGER_EVENT_ON_DAMAGE)), chain);
            break;
        }
        case TRIGGER_ACTION_INCREASE_STORAGE:
        case TRIGGER_ACTION_DECREASE_STORAGE:
        case TRIGGER_ACTION_SET_STORAGE:
        case TRIGGER_ACTION_RESET_STORAGE:
            storage_action(game, h, j, a, chain);
            break;
        case TRIGGER_ACTION_BROADCAST_POPUP:
            if (action_text(a)) send_popup(game, NET_TARGET_ALL, a->text);
            break;
        case TRIGGER_ACTION_SEND_POPUP:
            if (h && action_text(a)) send_popup(game, h->uid, a->text);
            break;
        case TRIGGER_ACTION_JUMP_SCARE:
            if (h) {
                const NetJumpScarePacket packet = {.index = 0}; // send("scr", 0)
                game_send(game, h->uid, NET_PACKET_JUMP_SCARE, &packet, sizeof(packet));
            }
            break;
        case TRIGGER_ACTION_CLEAR_CHECKPOINTS:
            if (!h) break;
#ifdef WORLD_HAVE_CLEAR_CHECKPOINTS
            world_clear_checkpoints(game, h); // checkPoint, List, Data cleared + "chp" true (map-B)
#else
            h->checkpoint = 0;
            h->checkpoint_list_count = 0;
#endif
            break;
        case TRIGGER_ACTION_SEND_SOUND:
            if (h) play_sound(game, h->uid, h, j, a);
            break;
        case TRIGGER_ACTION_EXPLOSION:
            if (trigger_action_value(game, h, j, NULL, a, a->constant ? a->constant : 100, &value)) {
                game_explosion(game, j->position, 30.0f, (float) value, h, -1); // explosion(j.x, j.y, j.z, 30, val, h, {})
            }
            break;
        case TRIGGER_ACTION_SPAWN_AI:
#ifdef MAP_AI_HAVE_TRIGGER_API
            map_ai_spawn_interface(game, j->interface_id, j->interface_target); // each pending AI of interfaceT
#endif
            break;
        case TRIGGER_ACTION_SET_PLAYER_VALUE:
        case TRIGGER_ACTION_CHANGE_PLAYER_VALUE: {
            const char *key = action_text(a); // tct reduced to [a-z0-9_]{0,10} (map.c)
            if (!h || !key) break;
            if (a->action == TRIGGER_ACTION_SET_PLAYER_VALUE) player_custom_value_set(game, h, key, a->constant); // send("cv", key, val)
            else player_custom_value_add(game, h, key, a->constant); // custVals[key] += val (a string appends), "cv"
            break;
        }
        case TRIGGER_ACTION_PLAY_ANIMATION: {
            const char *clip = action_text(a);
            if (!clip) break;
#ifdef MAP_AI_HAVE_TRIGGER_API
            map_ai_play_anim_interface(game, a->constant, clip); // AIs with interface == tc ("ana" sid, clip, 1)
#endif
            for (size_t l = 0; l < map->object_count && CHAIN_OK(game, chain); l++) {
                Object *d = map->objects[l]; // (d != j && d.interface != j.interface && d.interface == tc: the id, not tin)
                if (trigger_is_interface(d) && d != j->object && d->interface_id != j->interface_id && d->interface_id == a->constant) {
                    world_play_anim(game, d, clip, 1);
                }
            }
            break;
        }
        case TRIGGER_ACTION_TELEPORT_TO_NODE:
            if (h && trigger_action_value(game, h, j, NULL, a, a->constant, &value)) {
                const Object *node = world_path_node(map, value);
                if (node) change_position(game, h, node->position, 1, 1); // (node.x, node.y, node.z, true, true, undefined, true)
            }
            break;
        case TRIGGER_ACTION_INTERFACE_TO_NODE:
            for (size_t l = 0; l < map->object_count && CHAIN_OK(game, chain); l++) {
                Object *d = map->objects[l];
                if (!targets(j, d) || !trigger_action_value(game, h, j, d, a, a->constant, &value)) continue;
                if (!world_path_node(map, value)) return; // no such node: the action stops
                world_teleport_object_to_node(game, d, value); // teleportToNode: "mo" (map-B)
            }
            break;
        case TRIGGER_ACTION_RESET_INTERFACE_POSITION:
            for (size_t l = 0; l < map->object_count && CHAIN_OK(game, chain); l++) {
                Object *d = map->objects[l];
                if (targets(j, d)) world_reset_object_position(game, d); // teleportFromNode: "mf" (map-B)
            }
            break;
        case TRIGGER_ACTION_SET_INTERFACE_VALUE:
        case TRIGGER_ACTION_CHANGE_INTERFACE_VALUE:
            set_interface_value(game, h, j, a, a->action == TRIGGER_ACTION_CHANGE_INTERFACE_VALUE, chain);
            break;
        case TRIGGER_ACTION_SET_TRIGGER_VALUE:
        case TRIGGER_ACTION_CHANGE_TRIGGER_VALUE: {
            const char *key = action_text(a);
            if (!key || !j->object) break; // AIs have no custVals
            TriggerValueSet *set = trigger_object_values_mut(game, j->object, 1);
            TriggerValue *slot = set ? trigger_values_slot(set, key) : NULL;
            if (slot && a->action == TRIGGER_ACTION_SET_TRIGGER_VALUE) trigger_value_set_number(slot, a->constant); // no events
            else if (slot) trigger_value_add(slot, a->constant);
            break;
        }
        case TRIGGER_ACTION_CHANGE_CLASS: {
            const size_t class_count = game->class_count ? game->class_count : CLASS_COUNT;
            if (h && a->constant >= 0 && (size_t) a->constant < class_count && h->class_index != a->constant) apply_class(game, h, a->constant);
            break;
        }
        case TRIGGER_ACTION_CHANGE_PRIMARY:
        case TRIGGER_ACTION_CHANGE_SECONDARY:
            if (h) change_weapon(game, h, a->constant, a->action == TRIGGER_ACTION_CHANGE_SECONDARY);
            break;
        case TRIGGER_ACTION_SET_GLOBAL_VALUE:
        case TRIGGER_ACTION_CHANGE_GLOBAL_VALUE: {
            const char *key = action_text(a);
            if (!key) break;
            if (a->action == TRIGGER_ACTION_SET_GLOBAL_VALUE) trigger_global_value_set(game, key, a->constant); // "gv"
            else trigger_global_value_add(game, key, a->constant); // globalVals[key] += val (a string appends), "gv"
            break;
        }
        case TRIGGER_ACTION_STOP_SOUND: {
            NetStopSoundPacket packet;
            memset(&packet, 0, sizeof(packet));
            if (a->asset_sound) snprintf(packet.name, sizeof(packet.name), "%d", a->sound);
            else sound_name(a, packet.name, sizeof(packet.name));
            packet.asset = a->asset_sound;
            game_send(game, NET_TARGET_ALL, NET_PACKET_STOP_SOUND, &packet, sizeof(packet)); // broadcast("sso", name, isAsset)
            break;
        }
        case TRIGGER_ACTION_GUI_ALL:
        case TRIGGER_ACTION_GUI_PLAYER: {
            if (a->action == TRIGGER_ACTION_GUI_PLAYER && !h) break;
            const NetGuiTogglePacket packet = {.id = a->constant, .status = (uint8_t) (a->constant2 < 0 || a->constant2 > 255 ? 0 : a->constant2)};
            game_send(game, a->action == TRIGGER_ACTION_GUI_ALL ? NET_TARGET_ALL : h->uid, NET_PACKET_GUI_TOGGLE, &packet, sizeof(packet)); // "tgui" id, status
            break;
        }
        case TRIGGER_ACTION_CUSTOM: // mainScript.onCustomTrigger(h ? h.id : "NONE", tct || "", parseInt(tc) || 0)
            if (game->script_hooks && game->script_hooks->on_custom_trigger) {
                char id[16];
                if (h) snprintf(id, sizeof(id), "%d", h->uid);
                else snprintf(id, sizeof(id), "NONE");
                game->script_hooks->on_custom_trigger(game, id, action_text(a) ? a->text : "", a->constant);
            }
            break;
        case TRIGGER_ACTION_CHANGE_AMBIENT:
            if (a->constant >= 0 && a->constant < TRIGGER_SOUND_SCAPE_COUNT) {
                const NetAmbientPacket packet = {.ambient = g_trigger_sound_scapes[a->constant]};
                game_send(game, NET_TARGET_ALL, NET_PACKET_AMBIENT_CHANGE, &packet, sizeof(packet)); // broadcast("camb", soundScape)
            }
            break;
        case TRIGGER_ACTION_PLAY_ANIMATION_SELF:
            if (action_text(a) && j->object) world_play_anim(game, j->object, a->text, 1); // playAnim(j, clip, 1, true)
            break;
        case TRIGGER_ACTION_GIVE_EFFECT:
            if (h && a->constant >= 0 && a->constant < EFFECT_COUNT) player_give_effect(h, a->constant, 1); // giveEffect(h, idx)
            break;
        case TRIGGER_ACTION_RANDOM_EFFECT:
            if (h) player_give_effect(h, trigger_rand_int(0, EFFECT_COUNT - 1), 1); // giveEffect(h, 0, 1, true): a random one
            break;
        case TRIGGER_ACTION_DROP_POWERUP:
            // zombies only (and not in custom games without mimicPubs; KN has no such flag: zombies rounds
            // on any map drop it)
            if (game_mode_id(game) == MODE_V7_ZOM) spawnable_drop(game, trigger_rand_int(9, 15), h, 0, j->position);
            break;
        case TRIGGER_ACTION_SPAWN_BOT:
#ifdef MAP_AI_HAVE_TRIGGER_API
            if (h && trigger_action_value(game, h, j, NULL, a, a->constant, &value)) map_bot_spawn_at_node(game, value, 1); // "Uncle Joe"
#endif
            break;
        case TRIGGER_ACTION_FORCE_AI_TARGET:
#ifdef MAP_AI_HAVE_TRIGGER_API
            map_ai_force_target_point(game, j->position); // AI.forceTarget = {j.x, j.y, j.z} for 30 s
#endif
            break;
        case TRIGGER_ACTION_MODE_SPECIFIC:
            mode_v7_on_trigger(game, h, j->uid); // game.mode.onTrigger(game, j): the trigger's uid
            break;
        case TRIGGER_ACTION_REWARD_KR: // rewardKR(h, parseInt(trigConst || 0)) when game.krRewards (the hook checks it)
            if (h && game->hooks.reward_kr) game->hooks.reward_kr(game, h, a->constant);
            break;
        case TRIGGER_ACTION_SAVE_DATA:
            save_data(game, h, j, a);
            break;
        case TRIGGER_ACTION_LOAD_DATA: // getStoragePlayer(account, null) -> trigger_load_data_apply (triggers.js:1353-1375)
            if (game->hooks.storage_load && has_account(game, h)) game->hooks.storage_load(game, h);
            break;
        case TRIGGER_ACTION_CHALLENGE: // 62 Give Challenge Progress: (!isSandbox && isPubType && h.account)
            // challenges.update(game, trigConstTx, parseInt(trigConst || 0), null, h), then v10.0.3's
            // `player.runProg[key] = 1` (deob-10.0.3.js:183654-183658; Raid on Bastion reads bthrone)
            if (!game->is_sandbox && h && action_text(a) && has_account(game, h)) {
                game_challenge(game, h, a->text, (int) a->constant, -1, -1);
                if (game_is_pub_type(game)) mode_run_progress(h, a->text);
            }
            break;
        case TRIGGER_ACTION_CHALLENGE_ALL: // 65 Give Challenge Progress (All): every active player
            if (!game->is_sandbox && h && action_text(a) && has_account(game, h)) {
                for (size_t i = 0; i < game->player_count; i++) {
                    const Player *p = game->players[i];
                    if (p && p->active) game_challenge(game, p, a->text, (int) a->constant, -1, -1);
                }
            }
            break;
        // v10.0.2 entity actions (L163648-163700): game.entities does the work (the entities package,
        // TriggerScriptHooks); without it they do nothing, like v10 without game.entities.
        case TRIGGER_ACTION_SPAWN_ENTITY: { // tct "archetype[|tag]" (tag trimmed, <= 32) at the trigger's position
            const TriggerScriptHooks *hooks = game->script_hooks;
            char arch[TRIGGER_TEXT_MAX + 1], tag[TRIGGER_TEXT_MAX + 1];
            if (!hooks || !hooks->spawn_entity || !trigger_split_text(action_text(a), arch, tag, sizeof(arch)) || !arch[0]) break;
            tag[32] = 0; // .substr(0, 32)
            hooks->spawn_entity(game, arch, tag[0] ? tag : NULL, j->position);
            break;
        }
        case TRIGGER_ACTION_DESPAWN_ENTITY: { // tct: the tag (trimmed)
            const TriggerScriptHooks *hooks = game->script_hooks;
            char tag[TRIGGER_TEXT_MAX + 1];
            if (!hooks || !hooks->despawn_entities || !action_text(a)) break;
            trigger_trim(a->text, tag, sizeof(tag));
            hooks->despawn_entities(game, tag);
            break;
        }
        case TRIGGER_ACTION_SET_ENTITY_VALUE: { // tct "tag|key" (both trimmed, non-empty), value getActionValue(tc || 0)
            const TriggerScriptHooks *hooks = game->script_hooks;
            char tag[TRIGGER_TEXT_MAX + 1], key[TRIGGER_TEXT_MAX + 1];
            if (!hooks || !hooks->set_entity_value || !trigger_split_text(action_text(a), tag, key, sizeof(tag)) || !tag[0] || !key[0]) break;
            if (trigger_action_value(game, h, j, NULL, a, a->constant, &value)) hooks->set_entity_value(game, tag, key, value);
            break;
        }
        case TRIGGER_ACTION_REWARD_JUNK:
            reward_junk(game, h, a);
            break;
        case TRIGGER_ACTION_REWARD_SKIN:    // public games only
        case TRIGGER_ACTION_VIDEO_AD:       // "vad": no ads natively
        default:
            break;
    }
}
