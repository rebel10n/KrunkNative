// Krunker v7.1.4 status effects, perks, killstreaks and the hit-zone maths shared by hitscan,
// melee and projectiles (WP-B, doc/v7/player-viewer.md 1.2.5, 1.6).
// Sources: v7 src/data/effects.js, streaks.js, players.js giveEffect / removeEffect /
// clearEffect / givePerk / giveWeapon / removeWeapon / useStreak / checkStreak / updateStreaks
// / addStreak, game.js setJuggernaut, player.js hitboxPadW / hitboxPadH.
// Everything here is server-authoritative; the client mirror only applies the EFFECT / PERKS /
// STREAK / JUGGERNAUT packets (client/src/net/combat_events.c) so its prediction matches.
#include <shared.h>
#include "modes/mode_internal.h"
#include <triggers_v7.h> // WP-C: onNukeStart
#include <zombies.h> // map-logic WP-H: airdrops
#include <game_config.h> // WP-E: game_config_is_custom
#include <math.h>
#include <stdio.h>
#include <string.h>

// effects.js (timer in s; `mid` is the "am" medal icon).
const EffectConfig g_effects[EFFECT_COUNT] = {
    {"Boosted", "speed", 0.0f, 1.0f, 5, 0.1f, 0.0f, 41},
    {"Sleight of Hand", "reload", 0.0f, 1.0f, 5, 0.1f, 0.0f, 42},
    {"Rapid Fire", "fRate", 0.0f, 1.0f, 5, 0.05f, 0.0f, 43},
    {"Siphon", "siphon", 0.0f, 0.0f, 5, 1.0f, 10.0f, 44},
    {"Rooted", "rooted", 25.0f, 1.0f, 2, 0.25f, 0.0f, 45},
    {"Dizzy", "drunk", 35.0f, 0.0f, 4, 0.25f, 0.0f, 46},
    {"Poisoned", "enraged", 40.0f, 0.0f, 1, 1.0f, 10.0f, 47},
    {"Jump Boost", "jump", 0.0f, 1.0f, 5, 0.1f, 0.0f, 51},
    {"Zapped", "zap", 5.0f, 0.0f, 10, 0.5f, 2.0f, 52},
    {"Strength", "dmg", 0.0f, 1.0f, 10, 0.1f, 0.0f, 53},
};

// streaks.js
const StreakConfig g_streaks[STREAK_COUNT] = {
    [STREAK_NUKE] = {"Nuke", 2, 25, .req_account = 1},
    [STREAK_AIRDROP] = {"Airdrop", 1, 12, .no_customs = 1},
    [STREAK_REFILL] = {"Refill", 0, 5, .dev_only = 1},
    [STREAK_BOMBARDMENT] = {"Bombardment", 2, 20, .dev_only = 1},
    [STREAK_SLIMER] = {"Slimer", 0, 7},
    [STREAK_JUGGERNAUT] = {"Juggernaut", 2, 30},
    [STREAK_WAR_MACHINE] = {"War Machine", 1, 15},
};

const int g_default_streak_indexes[STREAK_SLOTS] = {-1, -1, STREAK_NUKE};

// v7 config.js
#define CONFIG_HEAD_SCALE 2.0f   // config.headScale
#define CONFIG_LEG_HEIGHT 4.2f   // config.legHeight
#define CONFIG_CROUCH_DST 3.0f   // config.crouchDst
#define CONFIG_HIT_BOX_PAD 0.6f  // serverConfig hitBoxPad def
#define CONFIG_HIT_BOX_PAD_C 0.1f // ... defC (competitive lobbies: KN's Ranked modes)
#define JUGGERNAUT_HEALTH 2000
#define PERK_HEALTH_VALUE 250

// The game's effect table: custom-game prks overrides (Game.effects), else effects.js.
static const EffectConfig *effect_config(const Player *player, const int effect) {
    const Game *game = player ? player->game : NULL;
    return game && game->effects ? &game->effects[effect] : &g_effects[effect];
}

static float *attribute_slot(PlayerAttributes *a, const int effect) {
    switch (effect) {
        case EFFECT_BOOSTED: return &a->speed;
        case EFFECT_SLEIGHT_OF_HAND: return &a->reload;
        case EFFECT_RAPID_FIRE: return &a->fire_rate;
        case EFFECT_SIPHON: return &a->siphon;
        case EFFECT_ROOTED: return &a->rooted;
        case EFFECT_DIZZY: return &a->drunk;
        case EFFECT_POISONED: return &a->enraged;
        case EFFECT_JUMP_BOOST: return &a->jump;
        case EFFECT_ZAPPED: return &a->zap;
        case EFFECT_STRENGTH: return &a->damage;
        default: return NULL;
    }
}

// effects.js execute(): the attribute for the current stack count.
static void effect_execute(Player *player, const int effect) {
    const EffectConfig *e = effect_config(player, effect);
    float *slot = attribute_slot(&player->attributes, effect);
    if (!slot) return;
    const float stacks = (float) player->effects[effect];

    switch (effect) {
        case EFFECT_BOOSTED:
        case EFFECT_JUMP_BOOST:
        case EFFECT_STRENGTH:
            *slot = 1.0f + stacks * e->mlt;
            break;
        case EFFECT_SLEIGHT_OF_HAND:
        case EFFECT_RAPID_FIRE:
        case EFFECT_ROOTED:
            *slot = fabsf(1.0f - stacks * e->mlt);
            break;
        case EFFECT_DIZZY:
            *slot = stacks * e->mlt;
            break;
        default: // siphon / poison / zap: HP per tick
            *slot = stacks * e->mlt * e->hp;
            break;
    }
}

static void effect_send(Player *player, const int effect) {
    Game *game = player->game;
    if (!game || !game_is_authority(game)) return;
    const NetEffectPacket packet = {.effect = (uint8_t) effect, .count = player->effects[effect]};
    game_send(game, player->uid, NET_PACKET_EFFECT, &packet, sizeof(packet));
}

void effects_reset(Player *player) {
    memset(player->effects, 0, sizeof(player->effects));
    memset(player->effect_timer_count, 0, sizeof(player->effect_timer_count));
    for (int i = 0; i < EFFECT_COUNT; i++) {
        float *slot = attribute_slot(&player->attributes, i);
        if (slot) *slot = effect_config(player, i)->def;
    }
    player->zapped_by = -1;
    player->regen_tick_count = 0;
    player->perk_count = 0;
    player->charge_time = 0.0f;
    player->full_charge = 0;
    player->over_charged = 0;
}

static void effect_push_timer(Player *player, const int effect, const float seconds) {
    unsigned char *count = &player->effect_timer_count[effect];
    if (*count < EFFECT_STACK_MAX) player->effect_timers[effect][(*count)++] = seconds;
}

// players.giveEffect(player, index, count): stacks capped at `max`; overflow refreshes the
// oldest timers instead (v7 resets timers [0, overflow) to the full time).
void player_give_effect(Player *player, const int effect, int count) {
    if (!player || effect < 0 || effect >= EFFECT_COUNT) return;
    const EffectConfig *e = effect_config(player, effect);
    if (count <= 0) count = 1;

    int stacks = player->effects[effect] + count;
    if (stacks > e->max) {
        const int over = stacks - e->max;
        player->effects[effect] = (unsigned char) e->max;
        if (e->timer > 0.0f) {
            for (int i = 0; i < over; i++) {
                if (i < player->effect_timer_count[effect]) player->effect_timers[effect][i] = e->timer;
                else effect_push_timer(player, effect, e->timer);
            }
        }
        // v7 returns here without execute() / "pk": the stack count did not change. Intentional
        // difference: execute anyway, so a first grant above `max` (count > max) still applies.
        effect_execute(player, effect);
        effect_send(player, effect);
        return;
    }

    player->effects[effect] = (unsigned char) stacks;
    if (e->timer > 0.0f) {
        for (int i = 0; i < count; i++) effect_push_timer(player, effect, e->timer);
    }
    effect_execute(player, effect);
    effect_send(player, effect);

    Game *game = player->game;
    if (game && game_is_authority(game)) { // send("am", [mid, "x" + stacks])
        NetMedalPacket medal;
        memset(&medal, 0, sizeof(medal));
        snprintf(medal.text, sizeof(medal.text), "%s x%d", e->name, stacks);
        game_send(game, player->uid, NET_PACKET_MEDAL, &medal, sizeof(medal));
    }
}

// io_effect (client): the server's stack count; 0 clears (clearEffect).
void player_set_effect_stacks(Player *player, const int effect, const int count) {
    if (!player || effect < 0 || effect >= EFFECT_COUNT) return;
    if (count <= 0) {
        player->effects[effect] = 0;
        float *slot = attribute_slot(&player->attributes, effect);
        if (slot) *slot = effect_config(player, effect)->def;
        return;
    }
    player->effects[effect] = (unsigned char) (count < 255 ? count : 255);
    effect_execute(player, effect);
}

// removeEffect(player, index, one) / clearEffect
void player_remove_effect(Player *player, const int effect, const int one) {
    if (!player || effect < 0 || effect >= EFFECT_COUNT) return;
    if (one && player->effects[effect]) {
        player->effects[effect]--;
        if (player->effects[effect]) {
            effect_execute(player, effect);
            effect_send(player, effect);
            return;
        }
    }
    player->effects[effect] = 0;
    float *slot = attribute_slot(&player->attributes, effect);
    if (slot) *slot = effect_config(player, effect)->def;
    player->effect_timer_count[effect] = 0;
    if (effect == EFFECT_ZAPPED) player->zapped_by = -1;
    effect_send(player, effect);
}

// tickPlayer: every timed stack counts down; an expired one removes one stack.
void player_tick_effects(Player *player, const float delta) {
    for (int e = 0; e < EFFECT_COUNT; e++) {
        for (int i = (int) player->effect_timer_count[e] - 1; i >= 0; i--) {
            player->effect_timers[e][i] -= delta;
            if (player->effect_timers[e][i] > 0.0f) continue;
            for (int k = i; k + 1 < player->effect_timer_count[e]; k++) player->effect_timers[e][k] = player->effect_timers[e][k + 1];
            player->effect_timer_count[e]--;
            player_remove_effect(player, e, 1);
            if (!player->effects[e]) break; // clearEffect dropped the remaining timers
        }
    }
}

// tickPlayer's 500 ms ticker (tickerC already advanced): Poisoned every 8th tick, Zapped every
// 2nd (credited to zappedBy with the Zapper's weapon id, a "zap_0" sound to the victim).
void player_effects_regen_tick(Player *player) {
    Game *game = player->game;
    if (!game_is_authority(game) || !player->active) return;

    if (player->attributes.enraged > 0.0f && player->regen_tick_count % 8 == 0) {
        if (player_change_health(player, NULL, player->attributes.enraged, 0)) {
            player_kill(player, NULL, NULL, 0);
            return;
        }
    }
    if (player->attributes.zap > 0.0f && player->regen_tick_count % 2 == 0) {
        game_play_sound(game, "zap_0", player->uid, player->position, 0.5f, 1.0f);
        Player *by = player->zapped_by >= 0 ? game_find_player(game, player->zapped_by) : NULL;
        if (player_change_health(player, by, player->attributes.zap, 0)) {
            PlayerKillInfo info = {.weapon_id = WEAPON_ID_ZAPPER};
            player_kill(player, by, &info, 0);
        }
    }
}

static float attr_or_one(const float v) {
    return v > 0.0f ? v : 1.0f; // a zeroed Player (before its first spawn) reads as no effect
}

float player_attr_speed(const Player *player) { return attr_or_one(player->attributes.speed); }
float player_attr_rooted(const Player *player) { return attr_or_one(player->attributes.rooted); }
float player_attr_jump(const Player *player) { return attr_or_one(player->attributes.jump); }
float player_attr_damage(const Player *player) { return attr_or_one(player->attributes.damage); }

float player_attr_reload(const Player *player) {
    return attr_or_one(player->attributes.reload) * (player_has_perk(player, PERK_FAST_RELOAD) ? 0.5f : 1.0f);
}

float player_attr_fire_rate(const Player *player) {
    return attr_or_one(player->attributes.fire_rate) * (player_has_perk(player, PERK_RAPID_FIRE) ? 0.66f : 1.0f);
}

int player_has_perk(const Player *player, const int perk) {
    for (int i = 0; i < player->perk_count; i++) {
        if (player->perks[i] == perk) return 1;
    }
    return 0;
}

// broadcast("h", health, sid, ..., true, true). The new maxHealth reaches the clients through
// the PERKS / JUGGERNAUT packet sent with it (NetHealthPacket has no maxHealth).
static void send_health_all(Player *player) {
    const NetHealthPacket packet = {.uid = player->uid, .health = ceilf(player->health)};
    game_send(player->game, NET_TARGET_ALL, NET_PACKET_HEALTH, &packet, sizeof(packet));
}

static void send_perks(Player *player) {
    NetPerksPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.uid = player->uid;
    packet.max_health = player->max_health;
    packet.count = (uint8_t) player->perk_count;
    for (int i = 0; i < player->perk_count && i < PLAYER_PERK_MAX; i++) packet.perks[i] = (int8_t) player->perks[i];
    game_send(player->game, NET_TARGET_ALL, NET_PACKET_PERKS, &packet, sizeof(packet));
}

// players.givePerk: once per perk; perk 0 sets 250 health.
void player_give_perk(Player *player, const int perk) {
    if (!player || player_has_perk(player, perk) || player->perk_count >= PLAYER_PERK_MAX) return;
    player->perks[player->perk_count++] = perk;
    if (!game_is_authority(player->game)) return;
    if (perk == PERK_HEALTH) {
        player->max_health = PERK_HEALTH_VALUE;
        player->health = PERK_HEALTH_VALUE;
    }
    send_perks(player);
    if (perk == PERK_HEALTH) send_health_all(player);
}

static const Weapon *weapon_at(const Game *game, const int id) {
    const size_t count = game->weapon_count ? game->weapon_count : WEAPON_COUNT;
    const Weapon *const *all = game->weapons ? (const Weapon *const *) game->weapons : g_weapons;
    return id >= 0 && (size_t) id < count ? all[id] : NULL;
}

int player_has_equipment(const Player *player) {
    for (int i = 0; player->loadout && i < player->loadout_size; i++) {
        const Weapon *w = weapon_at(player->game, player->loadout[i]);
        if (w && w->equipment) return 1;
    }
    return 0;
}

static int weapon_kind(const Weapon *w) { // 0 primary, 1 secondary, 2 melee, 3 equipment
    return w->equipment ? 3 : w->melee ? 2 : w->secondary ? 1 : 0;
}

const Weapon *player_slot_weapon(const Player *player, const int slot) {
    if (!player->loadout || slot < 0 || slot >= player->loadout_size) return NULL;
    const int id = player->loadout[slot];
    if (slot < 8 && (player->upgraded_slots >> slot & 1)) {
        const Weapon *upgraded = weapon_upgraded(id);
        if (upgraded) return upgraded;
    }
    return weapon_at(player->game, id);
}

void player_send_upgrades(Player *player) {
    NetUpgradesPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.uid = player->uid;
    packet.slots = player->upgraded_slots;
    for (int i = 0; i < player->loadout_size && i < NET_LOADOUT_MAX; i++) packet.ammo[i] = (int32_t) player->ammo[i];
    game_send(player->game, NET_TARGET_ALL, NET_PACKET_UPGRADES, &packet, sizeof(packet));
}

// The krumMachine's upgrade (v7 player.js / players.js 3396: upgrades[loadoutIndex] = 1, the
// upgraded weapon's full ammo). The machine itself (price, timer, "krm") is map-logic's.
void player_upgrade_weapon(Player *player, const int slot) {
    if (!player || !player->loadout || slot < 0 || slot >= player->loadout_size || slot >= 8) return;
    player->upgraded_slots |= (unsigned char) (1u << slot);
    const Weapon *weapon = player_slot_weapon(player, slot);
    if (weapon) player->ammo[slot] = weapon->ammo;
    if (slot == player->loadout_index) player->weapon = weapon;
    if (game_is_authority(player->game)) player_send_upgrades(player);
}

void player_set_upgrades(Player *player, const unsigned char slots) {
    player->upgraded_slots = slots;
    if (player->loadout && player->loadout_index >= 0 && player->loadout_index < player->loadout_size) {
        const Weapon *weapon = player_slot_weapon(player, player->loadout_index);
        if (weapon) player->weapon = weapon;
    }
}

// Rebuild the loadout keeping each kept weapon's ammo and upgrade (updateLoadout with the old
// ammos / upgrades), then put `index` in hand and tell everyone ("inat").
static void apply_loadout(Player *player, const int *ids, const int count, int index) {
    // + the Object Manager's extra weapons (KrunkNative): they keep their ammo too
    int old_ids[NET_LOADOUT_MAX + 1 + PLAYER_EXTRA_WEAPONS_MAX];
    unsigned int old_ammo[NET_LOADOUT_MAX + 1 + PLAYER_EXTRA_WEAPONS_MAX];
    const unsigned char old_upgrades = player->upgraded_slots;
    const int old_max = NET_LOADOUT_MAX + 1 + PLAYER_EXTRA_WEAPONS_MAX;
    const int old_count = player->loadout_size < old_max ? player->loadout_size : old_max;
    for (int i = 0; i < old_count; i++) {
        old_ids[i] = player->loadout[i];
        old_ammo[i] = player->ammo ? player->ammo[i] : 0;
    }

    player->upgraded_slots = 0;
    mode_set_player_loadout(player, ids, count, 0);
    unsigned char upgrades = 0;
    for (int i = 0; i < player->loadout_size; i++) {
        for (int k = 0; k < old_count; k++) {
            if (old_ids[k] == player->loadout[i]) {
                player->ammo[i] = old_ammo[k];
                if (k < 8 && i < 8 && (old_upgrades >> k & 1)) upgrades |= (unsigned char) (1u << i);
                old_ids[k] = -1;
                break;
            }
        }
    }
    player->upgraded_slots = upgrades;
    if (index < 0) index = 0;
    if (index >= player->loadout_size) index = player->loadout_size - 1;
    if (index != player->loadout_index) player_swap_weapon(player, index, 1, 0, 0);
    else player->weapon = player_slot_weapon(player, index);
    if (game_is_authority(player->game)) {
        mode_send_loadout(player->game, player);
        if (upgrades || old_upgrades) player_send_upgrades(player);
    }
}

// players.giveWeapon(player, id) without forceEquip: replaces the weapon of the same kind, or
// inserts it (melee last, a secondary after the primary, others first).
// v7 bug fixed: the insert path sets loadoutIndex = undefined (the client then holds nothing);
// here the new weapon goes in hand.
void player_give_weapon(Player *player, const int weapon_id) {
    if (!player || !player->loadout || !game_is_authority(player->game)) return;
    const Weapon *weapon = weapon_at(player->game, weapon_id);
    if (!weapon) return;
    const int kind = weapon_kind(weapon);

    int ids[NET_LOADOUT_MAX + 1];
    int count = 0;
    int has_primary = 0;
    int replaced = -1;
    for (int i = 0; i < player_loadout_base(player) && count < NET_LOADOUT_MAX; i++) {
        const Weapon *w = weapon_at(player->game, player->loadout[i]);
        ids[count] = player->loadout[i];
        if (w && replaced < 0 && weapon_kind(w) == kind) {
            ids[count] = weapon_id;
            replaced = count;
        }
        if (w && weapon_kind(w) == 0) has_primary = 1;
        count++;
    }

    int index = player->loadout_index;
    if (replaced < 0) {
        if (count >= NET_LOADOUT_MAX) return;
        const int at = kind == 2 ? count : kind == 1 ? (has_primary ? 1 : 0) : 0;
        for (int i = count; i > at; i--) ids[i] = ids[i - 1];
        ids[at] = weapon_id;
        count++;
        index = at;
    }
    apply_loadout(player, ids, count, index);
}

// players.removeWeapon(player, id)
void player_remove_weapon(Player *player, const int weapon_id) {
    if (!player || !player->loadout || !game_is_authority(player->game)) return;
    int ids[NET_LOADOUT_MAX + 1];
    int count = 0;
    int removed = -1;
    for (int i = 0; i < player_loadout_base(player) && count < NET_LOADOUT_MAX; i++) {
        if (removed < 0 && player->loadout[i] == weapon_id) {
            removed = i;
            continue;
        }
        ids[count++] = player->loadout[i];
    }
    if (removed < 0) return;
    int index = player->loadout_index;
    if (removed < index) index--;
    apply_loadout(player, ids, count, index);
}

// game.setJuggernaut: 2000 HP and the Minigun, or back to startHealth without it.
void player_set_juggernaut(Player *player, const int on) {
    if (!player) return;
    player->is_juggernaut = (unsigned char) !!on;
    Game *game = player->game;
    if (!game_is_authority(game)) return;

    const int max_health = on ? JUGGERNAUT_HEALTH : (player->active ? player->start_health : player->max_health);
    const NetJuggernautPacket packet = {.uid = player->uid, .on = (uint8_t) !!on, .max_health = max_health};
    game_send(game, NET_TARGET_ALL, NET_PACKET_JUGGERNAUT, &packet, sizeof(packet)); // "jg" (+ the jug_0 loop client-side)

    if (on) {
        player->max_health = JUGGERNAUT_HEALTH;
        player->health = JUGGERNAUT_HEALTH;
        send_health_all(player);
        player_give_weapon(player, WEAPON_ID_MINIGUN);
    } else if (player->active) {
        player->max_health = player->start_health;
        player->health = (float) player->start_health;
        send_health_all(player);
        player_remove_weapon(player, WEAPON_ID_MINIGUN);
    }
}

// index.js f1795: a pick only counts in its own slot.
void player_set_streak_indexes(Player *player, const int indexes[STREAK_SLOTS]) {
    for (int slot = 0; slot < STREAK_SLOTS; slot++) {
        const int s = indexes ? indexes[slot] : g_default_streak_indexes[slot];
        player->streak_indexes[slot] = s >= 0 && s < STREAK_COUNT && g_streaks[s].slot == slot ? s : -1;
    }
}

static int streak_disabled(const Game *game, const int streak) {
    // mode.disabledStreaks (aon / md / chs / imp: [4, 5, 6]) and the noStreaks switches
    return (game->mode && game->mode->config.no_streaks) || game->config.no_streaks || mode_streak_disabled(game, streak);
}

static void streak_send(Player *player, const int streak) {
    const NetStreakPacket packet = {.streak = (uint8_t) streak, .count = player->streaks_owned[streak]};
    game_send(player->game, player->uid, NET_PACKET_STREAK, &packet, sizeof(packet));
}

// players.addStreak: reqAccount streaks need an account (the offline sandbox counts as one,
// decisions.md), noCustoms ones (the Airdrop) are off in custom games and the sandbox.
static void streak_add(Player *player, const int streak) {
    if (g_streaks[streak].req_account && !player->has_account) return;
    if (g_streaks[streak].no_customs && game_config_is_custom(player->game)) return; // isCustom (WP-E, WP-H)
    if (player->streaks_owned[streak] < 255) player->streaks_owned[streak]++;
    streak_send(player, streak);
}

// players.updateStreaks (after realKillStreak++): earn the picked streaks reached exactly, then
// reset at the highest picked one. v7's server keeps the last picked slot's kills
// (maxConfiguredStreak); slot order makes that the highest for every stock streak.
void player_update_streaks(Player *player) {
    Game *game = player->game;
    unsigned int max_kills = 0;
    for (int slot = 0; slot < STREAK_SLOTS; slot++) {
        const int s = player->streak_indexes[slot];
        if (s < 0 || s >= STREAK_COUNT) continue;
        if ((unsigned int) g_streaks[s].kills > max_kills) max_kills = (unsigned int) g_streaks[s].kills;
        if (g_streaks[s].dev_only || streak_disabled(game, s)) continue;
        if (player->real_kill_streak == (unsigned int) g_streaks[s].kills) streak_add(player, s);
    }
    if (player->real_kill_streak >= max_kills) player->real_kill_streak = 0;
}

// streaks.js Nuke.start: 10 s, or 1 ms less than the round when under 10 s remain (untimed modes).
int game_start_nuke(Game *game, Player *player) {
    if (game->nuke_timer > 0.0f) return 0;
    const int timed = game->mode && game->mode->config.timed;
    game->nuke_timer = !timed && game->round_timer > 0.0f && game->round_timer < 10.0f ? game->round_timer - 0.001f : 10.0f;
    game->nuke_player = player;
    const NetNukePacket packet = {.state = 0};
    game_send(game, NET_TARGET_ALL, NET_PACKET_NUKE, &packet, sizeof(packet)); // "n", 0
    trigger_multi_event(game, TRIGGER_EVENT_ON_NUKE_START, player); // findTriggers("onNukeStart", player) (WP-C)
    return 1;
}

// streaks.js activate(); Refill and Bombardment are not ported (the dev-only two never activate
// for non-dev accounts). The Airdrop is map-logic WP-H's (shared/src/airdrop.c).
static int streak_activate(Player *player, const int streak) {
    Game *game = player->game;
    switch (streak) {
        case STREAK_NUKE: return game_start_nuke(game, player);
        case STREAK_AIRDROP: return airdrop_streak_activate(game, player);
        case STREAK_SLIMER:
            if (!player->active || player_has_equipment(player)) return 0;
            player_give_weapon(player, WEAPON_ID_SLIMER);
            return 1;
        case STREAK_JUGGERNAUT:
            if (!player->active || player_has_equipment(player)) return 0;
            player_set_juggernaut(player, 1);
            return 1;
        case STREAK_WAR_MACHINE:
            if (!player->active || player_has_equipment(player)) return 0;
            player_give_weapon(player, WEAPON_ID_WAR_MACHINE);
            return 1;
        default:
            return 0;
    }
}

// players.useStreak (not on the end screen)
int player_use_streak(Player *player, const int streak) {
    Game *game = player->game;
    if (!game_is_authority(game) || game->end_timer > 0.0f) return 0;
    if (streak < 0 || streak >= STREAK_COUNT || !player->streaks_owned[streak]) return 0;
    if (!streak_activate(player, streak)) return 0;
    // WP-E part 2c: streaks.js incStat (n / ad / sl / jg / wm) and challenges.update("stk", {stk})
    static const char *const stats[STREAK_COUNT] = {[STREAK_NUKE] = "n", [STREAK_AIRDROP] = "ad", [STREAK_SLIMER] = "sl",
                                                    [STREAK_JUGGERNAUT] = "jg", [STREAK_WAR_MACHINE] = "wm"};
    if (stats[streak]) game_inc_stat(game, player, stats[streak]);
    game_challenge(game, player, "stk", 1, -1, streak);
    player->streaks_owned[streak]--;
    streak_send(player, streak);
    return 1;
}

// players.checkStreak(player, slot): the slot's pick, else any owned streak of that slot.
// v7 bug fixed: the reconstructed server (index.ts handleUseStreak) passes the slot number of
// "k<slot>" to useStreak as a streak index; the sandbox calls checkStreak, which is used here.
void player_check_streak(Player *player, const int slot) {
    if (!player || !player->active || slot < 0 || slot >= STREAK_SLOTS) return;
    const int picked = player->streak_indexes[slot];
    if (picked >= 0 && picked < STREAK_COUNT && player->streaks_owned[picked]) {
        player_use_streak(player, picked);
        return;
    }
    for (int s = 0; s < STREAK_COUNT; s++) {
        if (player->streaks_owned[s] && g_streaks[s].slot == slot) {
            player_use_streak(player, s);
            return;
        }
    }
}

// player.hitboxPadW / hitboxPadH: game.config.hitBoxPad + class hbS / hbH. A comp lobby (ranked
// included) gets the option's defC 0.1. v3's mode.hitBoxPad (Search & Destroy 0.5) is not read by
// v7 and is ignored here too.
static float game_hitbox_pad(const Game *game) {
    if (game->config.hitbox_pad_set) return game->config.hitbox_pad;
    return game->config.comp ? CONFIG_HIT_BOX_PAD_C : CONFIG_HIT_BOX_PAD;
}

static const ClassConfig *player_class(const Player *player) {
    const Game *game = player->game;
    const size_t count = game->class_count ? game->class_count : CLASS_COUNT;
    const ClassConfig *classes = game->classes ? game->classes : g_classes;
    return player->class_index >= 0 && (size_t) player->class_index < count ? &classes[player->class_index] : NULL;
}

float player_hitbox_pad_w(const Player *player) {
    const ClassConfig *c = player_class(player);
    return game_hitbox_pad(player->game) + (c ? c->hitbox_pad_w : 0.0f);
}

float player_hitbox_pad_h(const Player *player) {
    const ClassConfig *c = player_class(player);
    return game_hitbox_pad(player->game) + (c ? c->hitbox_pad_h : 0.0f);
}

int player_hit_is_head(const Player *target, const float base_y, const float adjusted_height, const float hit_y) {
    const float head_mlt = (target->head_mlt > 0.0f ? target->head_mlt : 1.0f) * mode_head_mlt(target); // + Bighead FFA (WP-D)
    return base_y + adjusted_height - hit_y < CONFIG_HEAD_SCALE * head_mlt;
}

int player_hit_is_leg(const Player *target, const float base_y, const float hit_y) {
    return base_y + CONFIG_LEG_HEIGHT - CONFIG_CROUCH_DST * target->crouch_val > hit_y;
}

// players.js shoot: head x hsMlt (unless noHeadShot), legs x lsMlt only when not a headshot,
// headshotOnly zeroes body hits. v3.1.1 / old KN applied 1.5x heads even with noHeadShot and
// a fixed 0.5 legs on headshots too (doc/v7/player-viewer.md 5.2).
float player_hit_damage(const Game *game, const Weapon *weapon, float damage, const int head, const int leg) {
    if (head) {
        if (!weapon->no_headshot) damage *= weapon->headshot_mlt ? weapon->headshot_mlt : 1.5f;
    } else if (game->config.headshots_only) {
        damage = 0.0f;
    }
    if (leg && !head) damage *= weapon->leg_mlt ? weapon->leg_mlt : 0.5f;
    return damage;
}
