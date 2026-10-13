// Game actions the modes trigger (Krunker v7.1.4 game.js updateTeam, lockPlayer, lockMove,
// nextObjective / randObjective, nextWeapon, increaseWeapon / decreaseWeapon and the player.js
// updateLoadout they end in).
#include "mode_internal.h"
#include <triggers_v7.h>
#include <pcg_basic.h>
#include <string.h>
#include <math.h>
#include <ruleset.h>

float mode_random(void) {
    return (float) ldexp((double) pcg32_random(), -32);
}

int mode_rand_int(const int min, const int max) {
    if (max <= min) return min;
    return min + (int) pcg32_boundedrand((uint32_t) (max - min + 1));
}

int mode_active_count(const Game *game) {
    int count = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        if (game->players[i] && game->players[i]->active) count++;
    }
    return count;
}

// updateTeam: "tm" to everyone, leaders resync. Infected / Seekers / Bosses also get
// their team's class on the next spawn (setClass teamClass).
void mode_update_team(Game *game, Player *player, const int team) {
    player->team = team;
    mode_apply_outfit(game, player);
    mode_send_team(game, player);
    game->leaders_dirty = 1;
}

// pickBoss / pickStalker / pickSimon: aK3 = health, health = aK3, regen = 0, "h".
void mode_set_role_health(Game *game, Player *player, const int health) {
    player->max_health = health;
    player->health = (float) health;
    player->hp_chase = 1.0f;
    player->mode.no_regen = 1;
    mode_send_team(game, player); // carries health + max health (Krunker "h")
}

void mode_lock_player(Game *game, Player *player, const int locked) {
    player->mode.locked = (unsigned char) !!locked;
    if (locked) player->input_queue_size = 0;
    mode_send_lock(game, player->uid, NET_LOCK_PLAYER, locked);
}

void mode_lock_move(Game *game) {
    game->move_lock = 1;
    mode_send_lock(game, NET_TARGET_ALL, NET_LOCK_MOVE, 1);
}

void mode_unlock_move(Game *game) {
    game->move_lock = 0;
    mode_send_lock(game, NET_TARGET_ALL, NET_LOCK_MOVE, 0);
}

int mode_input_locked(const Player *player) {
    return player && player->mode.locked;
}

// waitTimers.push(timer) when `replace` is 0 and a list exists, else waitTimers = [timer].
void mode_push_wait_timer(Game *game, const ModeWaitTimer *timer, const int replace) {
    GameMode *mode = game->mode;

    if (replace || !mode->wait_timer_count) {
        mode->wait_timers[0] = *timer;
        mode->wait_timer_count = 1;
        mode->wait_second_sent = -1;
        return;
    }

    if (mode->wait_timer_count < MODE_MAX_WAIT_TIMERS) mode->wait_timers[mode->wait_timer_count++] = *timer;
}

float mode_cfg_objective_time(const Game *game) {
#ifdef GAME_CONFIG_V7 // WP-E: config.objtvTime (minutes, 0.1..10)
    if (game->config.objective_time > 0.0f) return game->config.objective_time;
#else
    (void) game;
#endif
    return 1.0f; // (config.objtvTime || 1)
}

// game.js getNextObjective(forceUid): the objective with uid forceUid (or activeObjective + 1),
// else uid 0.
static int mode_next_objective_uid(const Game *game, const int force_uid) {
    const int want = force_uid >= 0 ? force_uid : game->mode->active_objective + 1;
    for (int i = 0; i < game->mode->objective_count; i++) {
        if (game->mode->objectives[i].uid == want) return want;
    }
    return 0;
}

// nextObjective(time, push, forceUid): the next (or forced) objective for objtvTime minutes,
// "obj uid, ms, initial", and with more than one objective a rotation timer that keeps the
// round clock running; onObjectiveSwitch fires.
void mode_next_objective(Game *game, const float overflow, const int initial, const int push, const int force_uid) {
    GameMode *mode = game->mode;

    mode->active_objective = mode_next_objective_uid(game, force_uid);
    const float time = fmaxf(0.001f, mode_cfg_objective_time(game) * MODE_OBJECTIVE_ROTATE - overflow);
    mode->objective_time = time;
    for (int i = 0; i < mode->objective_count; i++) mode->objectives[i].on_zone = 0; // setObjective
    mode_send_objective_v7(game, mode->active_objective, initial);

    if (mode->objective_count > 1) {
        const ModeWaitTimer timer = {
            .time = time,
            .msg = "next objective ",
            .trigger = force_uid < 0 ? mode_trigger_next_objective : mode_trigger_rand_objective,
            .cont_time = 1,
        };
        mode_push_wait_timer(game, &timer, !(push && mode->wait_timer_count));
    }

    trigger_multi_event(game, 27, NULL); // multiObjectTrigger("onObjectiveSwitch")
}

// randObjective(time, push): nextObjective with a random objective's uid (0 without objectives)
void mode_rand_objective(Game *game, const float overflow, const int initial, const int push) {
    const GameMode *mode = game->mode;
    const int uid = mode->objective_count ? mode->objectives[mode_rand_int(0, mode->objective_count - 1)].uid : 0;
    mode_next_objective(game, overflow, initial, push, uid);
}

// weapons.onlyGoodWeapons: neither melee, equipment nor customOnly, and not weapon 15.
static int mode_random_weapon(const Game *game) {
    int ids[64];
    int count = 0;
    for (size_t i = 0; i < game->weapon_count && count < 64; i++) {
        const Weapon *weapon = game->weapons[i];
        if (!weapon || weapon->melee || weapon->equipment || i == 15) continue;
#ifdef PLAYER_EFFECTS_V7 // player-viewer WP-B weapons table: customOnly
        if (weapon->custom_only) continue;
#endif
        ids[count++] = (int) i;
    }
    return count ? ids[mode_rand_int(0, count - 1)] : 0;
}

// nextWeapon (Sharp Shooter): everyone switches to a random weapon every 20 s.
void mode_next_weapon(Game *game, const float overflow, const int initial) {
    GameMode *mode = game->mode;
    mode->active_objective = mode_random_weapon(game);

    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player) continue;
        mode_set_player_loadout(player, &mode->active_objective, 1, 0);
        if (player->active) mode_send_loadout(game, player);
    }

    mode_send_objective(game, NET_OBJECTIVE_WEAPON, mode->active_objective);

    const ModeWaitTimer timer = {
        .time = MODE_WEAPON_ROTATE - overflow,
        .msg = "next weapon in ",
        .trigger = mode_trigger_next_weapon,
        .cont_time = 1,
    };
    mode_push_wait_timer(game, &timer, !(initial && mode->wait_timer_count));
}

static int mode_order_index(const GameModeConfig *config, const int weapon_id) {
    for (int i = 0; i < config->weapon_order_size; i++) {
        if (config->weapon_order[i] == weapon_id) return i;
    }
    return -1;
}

// increaseWeapon(player, info): the weapon after the one that got the kill.
void mode_increase_weapon(Game *game, Player *player, const int weapon_id) {
    const GameModeConfig *config = &game->mode->config;
    const int current = weapon_id >= 0 ? weapon_id : (player->loadout_size ? player->loadout[0] : -1);
    const int index = mode_order_index(config, current);
    int next;

    if (index > -1) {
        if (index + 1 < config->weapon_order_size && current == config->weapon_order[index + 1]) return;
        next = index + 1 < config->weapon_order_size ? config->weapon_order[index + 1] : config->weapon_order[config->weapon_order_size - 1];
        player->mode.weapon_tier = index + 1;
    } else {
        next = config->weapon_order[0];
        player->mode.weapon_tier = 0;
    }

    mode_set_player_loadout(player, &next, 1, 0);
    mode_send_loadout(game, player);
}

// decreaseWeapon: one tier back (a setback unless already at the first weapon).
void mode_decrease_weapon(Game *game, Player *player) {
    const GameModeConfig *config = &game->mode->config;

    player->mode.weapon_tier--;
    if (player->mode.weapon_tier < 0) player->mode.weapon_tier = 0;
    else player->mode.setbacks++;

    const int tier = player->mode.weapon_tier < config->weapon_order_size ? player->mode.weapon_tier : config->weapon_order_size - 1;
    const int weapon = config->weapon_order[tier];
    mode_set_player_loadout(player, &weapon, 1, 0);
    mode_send_loadout(game, player);
}

// The loadout a mode forces on (re)spawn, before the knife: Gun Game keeps the tier's weapon
// (startingLoadout / keepLoadout), Sharp Shooter the objective weapon, the Stalker its knife,
// and the v7 startingLoadout modes (aon, md, chs, imp, zom) their list (players.js:1566).
// Same on the server and on the clients (they mirror tier, objective weapon and teams), so the
// predicted loadout matches. Returns the weapon count written to `out` (NET_LOADOUT_MAX room).
int mode_gun_loadout(const Game *game, const Player *player, int *out) {
    if (!game->mode) return 0;
    const GameModeConfig *config = &game->mode->config;

    if (config->weapon_order_size) {
        const int tier = CLAMP(player->mode.weapon_tier, 0, config->weapon_order_size - 1);
        out[0] = config->weapon_order[tier];
        return 1;
    }

    if (config->obj_weapon && game->mode->active_objective >= 0) {
        out[0] = game->mode->active_objective;
        return 1;
    }

    if (config->team_options[1] && !strcmp(config->team_options[1], "Stalker") && player->team == 2 && game->mode->started) {
        out[0] = WEAPON_ID_KNIFE;
        return 1;
    }

    if (config->starting_loadout_size) {
        const int count = config->starting_loadout_size < NET_LOADOUT_MAX ? config->starting_loadout_size : NET_LOADOUT_MAX;
        for (int i = 0; i < count; i++) out[i] = config->starting_loadout[i];
        return count;
    }

    return 0;
}

// player.updateLoadout(game, index, true, weapons): the weapons plus the knife (not with
// noMelee), fresh ammo (ammoLimit when the mode sets one) and no pending reloads; canThrow is
// !noKnives (player.js:376, 591-598).
void mode_set_player_loadout(Player *player, const int *weapons, const int count, const int keep_index) {
    Game *game = player->game;
    const size_t weapon_count = game->weapon_count ? game->weapon_count : WEAPON_COUNT;
    Weapon **all = game->weapons ? game->weapons : (Weapon **) rules_weapons();

    int loadout[NET_LOADOUT_MAX + 1];
    int size = 0;

    for (int i = 0; i < count && size < NET_LOADOUT_MAX; i++) {
        if (weapons[i] >= 0 && (size_t) weapons[i] < weapon_count && all[weapons[i]]) loadout[size++] = weapons[i];
    }

    int has_knife = 0;
    for (int i = 0; i < size; i++) has_knife |= loadout[i] == WEAPON_ID_KNIFE;
    if (!has_knife && !mode_no_melee(game) && (size_t) WEAPON_ID_KNIFE < weapon_count) loadout[size++] = WEAPON_ID_KNIFE;
    if (!size) return;

    int *new_loadout = calloc((size_t) size, sizeof(int));
    unsigned int *new_ammo = calloc((size_t) size, sizeof(unsigned int));
    float *new_reloads = calloc((size_t) size, sizeof(float));

    if (!new_loadout || !new_ammo || !new_reloads) {
        free(new_loadout);
        free(new_ammo);
        free(new_reloads);
        return;
    }

    for (int i = 0; i < size; i++) {
        new_loadout[i] = loadout[i];
        new_ammo[i] = (unsigned int) mode_ammo_limit(game, (int) all[loadout[i]]->ammo);
    }

    game_player_changing(game, player, 0); // the offline client drops the meshes of the old loadout
    free(player->loadout);
    free(player->ammo);
    free(player->reloads);
    player->loadout = new_loadout;
    player->ammo = new_ammo;
    player->reloads = new_reloads;
    player->loadout_size = size;
    player->loadout_base = size;
    player_loadout_append_extras(player); // KrunkNative Object Manager: the extra weapons stay after it
    player->can_throw = !mode_no_knives(game);
    player->upgraded_slots = 0; // v7.1.4 updateLoadout without upgrades[]: no krumMachine slots (WP-B)

    const int index = keep_index && player->loadout_index < size ? player->loadout_index : 0;
    player->loadout_index = -1; // force the swap below to re-equip
    player_swap_weapon(player, index, 1, 0, 0);
    game_player_changing(game, player, 1);
}

static int mode_team_is(const Game *game, const Player *player, const char *option) {
    const GameModeConfig *config = game && game->mode ? &game->mode->config : NULL;
    return config && player->team >= 1 && player->team <= 2 && config->team_options[player->team - 1] &&
           !strcmp(config->team_options[player->team - 1], option);
}

// players.js:699 modelOverrides[teamOptions[team - 1]]: Infected {headIndex 122, skinColor},
// Stalker {noCosmetics, every colour 0}. Server side the result rides in SPAWN; clients apply it
// again on "tm".
int mode_apply_outfit(const Game *game, Player *player) {
    PlayerCosmetics outfit = player->cosmetics;
    PlayerCosmeticsV7 outfit_v7 = player->cosmetics_v7;

    if (mode_team_is(game, player, "Infected")) {
        outfit.skin_color = MODE_INFECTED_SKIN;
        outfit_v7.head = MODE_INFECTED_HAT; // v7 headIndex: Zombie Head is a HEAD (type 15) item
    } else if (mode_team_is(game, player, "Stalker")) {
        outfit = PLAYER_COSMETICS_NONE;
        outfit.skin_color = MODE_STALKER_SKIN;
        outfit.hair_color = MODE_STALKER_SKIN;
        // noCosmetics clears every slot (players.js:697-712); the badge and card are not on the model
        const int badge = outfit_v7.badge, card = outfit_v7.player_card;
        outfit_v7 = PLAYER_COSMETICS_V7_NONE;
        outfit_v7.badge = badge;
        outfit_v7.player_card = card;
    } else {
        return 0;
    }

    if (!memcmp(&outfit, &player->cosmetics, sizeof(outfit)) && !memcmp(&outfit_v7, &player->cosmetics_v7, sizeof(outfit_v7))) return 0;
    game_player_changing(game, player, 0);
    player->cosmetics = outfit;
    player->cosmetics_v7 = outfit_v7;
    game_player_changing(game, player, 1);
    return 1;
}
