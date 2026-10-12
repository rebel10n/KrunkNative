// Game actions the modes trigger (Krunker.Unpacked-v3.1.1/106.js updateTeam, lockPlayer,
// lockMove, nextObjective, nextWeapon, increaseWeapon / decreaseWeapon and the players.js
// updateLoadout they end in).
#include "mode_internal.h"
#include <pcg_basic.h>
#include <string.h>
#include <math.h>

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

// nextObjective(overflow, initial): the next hardpoint, "obj", and with more than one
// objective a 60 s "next objective " timer that keeps the round clock running.
void mode_next_objective(Game *game, const float overflow, const int initial) {
    GameMode *mode = game->mode;

    mode->active_objective++;
    if (mode->active_objective >= mode->objective_count) mode->active_objective = 0;
    mode_send_objective(game, NET_OBJECTIVE_POINT, mode->objective_count ? mode->active_objective : -1);

    if (mode->objective_count > 1) {
        const ModeWaitTimer timer = {
            .time = MODE_OBJECTIVE_ROTATE - overflow,
            .msg = "next objective ",
            .trigger = mode_trigger_next_objective,
            .cont_time = 1,
        };
        mode_push_wait_timer(game, &timer, !(initial && mode->wait_timer_count));
    }
}

// weapons.onlyWeapons: neither melee nor equipment.
static int mode_random_weapon(const Game *game) {
    int ids[64];
    int count = 0;
    for (size_t i = 0; i < game->weapon_count && count < 64; i++) {
        const Weapon *weapon = game->weapons[i];
        if (weapon && !weapon->melee && !weapon->equipment) ids[count++] = (int) i;
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

// The loadout a mode forces on (re)spawn, before the knife: Gun Game keeps the tier's
// weapon (startingLoadout / keepLoadout), Sharp Shooter the objective weapon, the
// Stalker its knife. Same on the server and on the clients (they mirror tier,
// objective weapon and teams), so the predicted loadout matches.
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

    return 0;
}

// players.updateLoadout(game, index, true, ...weapons): the weapons plus the knife,
// fresh ammo (weapon ammo, not ammoLimit) and no pending reloads.
void mode_set_player_loadout(Player *player, const int *weapons, const int count, const int keep_index) {
    Game *game = player->game;
    const size_t weapon_count = game->weapon_count ? game->weapon_count : WEAPON_COUNT;
    Weapon **all = game->weapons ? game->weapons : (Weapon **) g_weapons;

    int loadout[NET_LOADOUT_MAX + 1];
    int size = 0;

    for (int i = 0; i < count && size < NET_LOADOUT_MAX; i++) {
        if (weapons[i] >= 0 && (size_t) weapons[i] < weapon_count && all[weapons[i]]) loadout[size++] = weapons[i];
    }

    int has_knife = 0;
    for (int i = 0; i < size; i++) has_knife |= loadout[i] == WEAPON_ID_KNIFE;
    if (!has_knife && (size_t) WEAPON_ID_KNIFE < weapon_count) loadout[size++] = WEAPON_ID_KNIFE;
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
        new_ammo[i] = all[loadout[i]]->ammo;
    }

    game_player_changing(game, player, 0); // the offline client drops the meshes of the old loadout
    free(player->loadout);
    free(player->ammo);
    free(player->reloads);
    player->loadout = new_loadout;
    player->ammo = new_ammo;
    player->reloads = new_reloads;
    player->loadout_size = size;
    player->can_throw = 1;
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

// players.js jLrR6: Infected players get modeSpecific.infected.skin and hat 122; the
// Stalker is black (modeSpecific.stalker skin/body) without hat, back, waist, face or
// melee skin. Server side the result rides in SPAWN; clients apply it again on "tm".
int mode_apply_outfit(const Game *game, Player *player) {
    PlayerCosmetics outfit = player->cosmetics;

    if (mode_team_is(game, player, "Infected")) {
        outfit.skin_color = MODE_INFECTED_SKIN;
        outfit.hat = MODE_INFECTED_HAT;
    } else if (mode_team_is(game, player, "Stalker")) {
        outfit = PLAYER_COSMETICS_NONE;
        outfit.skin_color = MODE_STALKER_SKIN;
        outfit.hair_color = MODE_STALKER_SKIN;
    } else {
        return 0;
    }

    if (!memcmp(&outfit, &player->cosmetics, sizeof(outfit))) return 0;
    game_player_changing(game, player, 0);
    player->cosmetics = outfit;
    game_player_changing(game, player, 1);
    return 1;
}
