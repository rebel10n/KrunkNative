// Krunker v10.0.3 Defuse: the bomb flow (shared/include/defuse.h). Sources, all deob-10.0.3.js:
//   player.update's sites loop        :192122-192146 (plant: an attacker with the bomb in hand, M1 on the ground)
//   player.update's pickups loop      :192155-192171 (defuse: a defender at the inactive bomb pickup)
//   game.plantBomb                    :217725-217747
//   game.diffuseBomb                  :217748-217758
//   game.updateBomb                   :217779-217806 (called by game.update after the vote timers, :222165)
//   def.onKill                        :119839-119856
//   game.reset's activeBomb = null    :219926 (mode_round_start clears GameMode.bomb)
// KrunkNative keeps game.activeBomb in GameMode.bomb (modes.h ModeBomb); the bomb itself is the map's
// WEAPON_PICKUP holding weapon 17 (shared/src/interact.c keeps the pickups).
#include <defuse.h>
#include <modes.h>
#include "modes/mode_internal.h"
#include <ruleset.h>
#include <string.h>

int defuse_enabled(const Game *game) {
    if (!game || !game->mode || !game->mode->config.site || !game->map) return 0;
    for (size_t i = 0; i < game->map->object_count; i++) {
        if (game->map->objects[i]->bomb_site) return 1; // map.manager.sites.length
    }
    return 0;
}

float defuse_beep_interval(const float time_left) {
    if (time_left <= 2.0f) return 0.15f;
    if (time_left <= 5.0f) return 0.2f;
    if (time_left <= 10.0f) return 0.5f;
    if (time_left <= 20.0f) return 1.0f;
    return DEFUSE_BOMB_TICK;
}

static const Weapon *weapon_of(const Game *game, const int id) {
    const size_t count = game->weapon_count ? game->weapon_count : WEAPON_COUNT;
    const Weapon *const *all = game->weapons ? (const Weapon *const *) game->weapons : rules_weapons();
    return id >= 0 && (size_t) id < count ? all[id] : NULL;
}

// (!mode || !mode.noPickups), and per pickup `teamOptions[team - 1] != "Infected"`
static int pickups_open(const Game *game, const Player *player) {
    const GameModeConfig *m = &game->mode->config;
    if (m->no_pickups) return 0;
    const char *team = player->team >= 1 && player->team <= 2 ? m->team_options[player->team - 1] : NULL;
    return !(team && !strcmp(team, "Infected"));
}

int defuse_site_at(const Game *game, const Player *player) {
    if (!game || !game->map || !player) return -1;
    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *o = game->map->objects[i];
        if (o->bomb_site && game_player_in_object(player, o, 0.0f)) return (int) i;
    }
    return -1;
}

// a pickup holding the bomb that is not active: the planted bomb
static int planted_bomb(const Game *game, const int uid, GamePickup *k) {
    if (!game_pickup_state(game, uid, k) || k->active || k->weapon < 0) return 0;
    const Weapon *w = weapon_of(game, k->weapon);
    return w && w->bomb;
}

int defuse_bomb_pickup_at(const Game *game, const Player *player) {
    if (!game || !game->map || !player || !game->mode || !game->mode->config.site) return -1;
    for (size_t i = 0; i < game->map->object_count; i++) {
        GamePickup k;
        if (game->map->objects[i]->pickup && planted_bomb(game, (int) i, &k) && game_player_in_pickup(game, player, (int) i)) return (int) i;
    }
    return -1;
}

// One hold step: doingInter, interProg += delta / ms, makingProg; 1 when the bar is full (interProg = 1)
static int hold_step(Player *player, const float delta, const float ms) {
    player->doing_interact = 1;
    player->interact_progress += delta * 1000.0f / ms;
    player->making_progress = 1;
    if (player->interact_progress < 1.0f) return 0;
    player->interact_progress = 1.0f;
    return 1;
}

void defuse_player_collide(Player *player, const Input *input, const float delta, const int recon) {
    Game *game = player ? player->game : NULL;
    if (!game || !game->map || !game->mode || !game->mode->config.site || !input) return;
    const int hold = input->shoot && player->on_ground; // lMouse && this.onGround

    // sites: `!recon && this.collides(site)`, then `team == 2 && weapon.bomb` (the prompts: game_interact_scan)
    if (!recon && player->team == DEFUSE_ATTACKERS && player->weapon && player->weapon->bomb && hold) {
        for (size_t i = 0; i < game->map->object_count; i++) {
            const Object *o = game->map->objects[i];
            if (!o->bomb_site || !game_player_in_object(player, o, 0.0f)) continue;
            // the planter's own beep_0 every 400 ms (isYou, SOUND.play) is the client's: sound_events.c
            if (hold_step(player, delta, DEFUSE_PLANT_MS)) defuse_plant(game, player); // plantBomb: isServer only
            if (!player->weapon || !player->weapon->bomb) break; // planted: the bomb left the hand
        }
    }

    // pickups: `!active && team == 1 && weaponConfig[pickup].bomb && mode.site` (no recon check in v10)
    if (player->team != DEFUSE_DEFENDERS || !hold || !pickups_open(game, player)) return;
    for (size_t i = 0; i < game->map->object_count; i++) {
        GamePickup k;
        if (!game->map->objects[i]->pickup || !planted_bomb(game, (int) i, &k) || !game_player_in_pickup(game, player, (int) i)) continue;
        if (hold_step(player, delta, DEFUSE_DEFUSE_MS)) defuse_defuse(game, player); // diffuseBomb: the server's only
    }
}

void defuse_plant(Game *game, Player *player) {
    // KrunkNative: one bomb at a time (v10 has no such guard; with interact.c's bomb pickup rule the planted bomb
    // always leaves the planter's hand, so this only stops a bomb that found no pickup from planting every tick)
    if (!defuse_enabled(game) || !player || !game_is_authority(game) || game->mode->bomb.active) return;
    // gameStatsTracker.logGameEvent("objective", {action: "bomb_plant"}): no stats tracker natively
    mode_send_medal(game, player, "Bomb Planted", DEFUSE_PLANT_SCORE); // incMedal + ADD_MEDAL [19, 200, 0, 1]
    player_score(player, DEFUSE_PLANT_SCORE, 2);                      // players.score(player, 200, 2): an objective
    vec3 at = {0.0f, 0.0f, 0.0f};
    // dropWeapon(player, false, 17, false) || {}: the bomb's pickup, inactive, where the planter stands. Without a
    // pickup to go back to the bomb has no position (`activeBomb.x || 0`: its beeps play at 0 0 0)
    const int uid = game_player_drop_weapon_id(game, player, DEFUSE_BOMB_WEAPON, 0, &at);
    ModeBomb *bomb = &game->mode->bomb;
    bomb->active = 1;
    bomb->team = player->team;
    bomb->tick = DEFUSE_BOMB_TICK;
    bomb->timer = DEFUSE_BOMB_TIME;
    bomb->position = uid >= 0 ? at : (vec3) {0.0f, 0.0f, 0.0f};
    mode_send_message(game, "BOMB PLANTED");                                              // broadcast("gmsg", ..)
    game_play_sound(game, "bo_plant", -2, (vec3) {0.0f, 0.0f, 0.0f}, 1.0f, 1.0f); // broadcast("so", "bo_plant", 1, 1): 2D
    game->leaders_dirty = 1;
}

void defuse_defuse(Game *game, Player *player) {
    if (!defuse_enabled(game) || !player || !game_is_authority(game) || !game->mode->bomb.active) return;
    // gameStatsTracker.logGameEvent("objective", {action: "bomb_defuse"}): no stats tracker natively
    game->mode->bomb.active = 0; // activeBomb = null
    player->mode.diffuses++;     // def.condition ends the round, def.winCondition gives it to this team
    game_play_sound(game, "bo_diff", -2, (vec3) {0.0f, 0.0f, 0.0f}, 1.0f, 1.0f); // broadcast("so", "bo_diff", 1, 1)
}

void defuse_update(Game *game, const float delta) {
    if (!defuse_enabled(game) || !game_is_authority(game) || game->end_timer > 0.0f) return; // && !this.endTimer
    ModeBomb *bomb = &game->mode->bomb;
    if (!bomb->active) return;
    bomb->tick -= delta;
    bomb->timer -= delta;
    if (bomb->timer <= 0.0f) {
        const NetNukePacket blast = {.state = 1};
        game_send(game, NET_TARGET_ALL, NET_PACKET_NUKE, &blast, sizeof(blast)); // broadcast("n", 1): the flash
        const int team = bomb->team;
        bomb->active = 0; // (activeBomb = null after the kills: nothing below reads it)
        for (size_t i = 0; i < game->player_count; i++) {
            Player *player = game->players[i];
            // players.kill(player, null, null, false, true): no killer, the feed shows it, no kill rewards
            if (player && player->active && player->team != team) player_kill(player, NULL, NULL, 1);
        }
    } else if (bomb->tick <= 0.0f) {
        bomb->tick = defuse_beep_interval(bomb->timer);
        // broadcast("so", "beep_0", 1, 0, x || 0, y || 0, z || 0): 3D at the bomb
        game_play_sound(game, "beep_0", -1, bomb->position, 1.0f, 1.0f);
    }
}

void defuse_on_kill(Game *game, Player *victim, Player *killer) {
    if (!game || !victim || !victim->loadout || !game_is_authority(game)) return;
    int slot = -1;
    for (int i = 0; i < victim->loadout_size && slot < 0; i++) {
        if (victim->loadout[i] == DEFUSE_BOMB_WEAPON) slot = i; // loadout.indexOf(17)
    }
    if (slot < 0) return;
    if (killer && killer != victim) { // loadoutIndex = the bomb, players.dropWeapon(victim): active, where it died
        game_player_drop_weapon_id(game, victim, DEFUSE_BOMB_WEAPON, 1, NULL);
        return;
    }
    // no killer (a fall, a death zone, the victim's own weapon): the bomb's empty pickup holds it again at its origin.
    // The bomb stays in the dead carrier's loadout (and lastPicked), as v10's.
    if (DEFUSE_BOMB_WEAPON >= PLAYER_PICKED_MAX || !victim->last_picked[DEFUSE_BOMB_WEAPON]) return;
    const int uid = victim->last_picked[DEFUSE_BOMB_WEAPON] - 1;
    GamePickup k;
    if (!game_pickup_state(game, uid, &k) || !k.replace || k.weapon >= 0) return; // findEmptyPickup(lastPicked[17], true)
    const vec3 origin = k.origin;
    game_pickup_set(game, uid, DEFUSE_BOMB_WEAPON, 1, &origin, -1); // updatePickup(uid, 17, null, true, [orgX, orgY, orgZ])
    // broadcast("inat", -1, null, null, uid, 17, true, null, pos): KN sends the pickup as the server holds it
    // (active). v10's own clients read the null as an inactive pickup (an official-server quirk the official
    // link keeps, client/src/net/krunker_link_world.c on_interactions)
    game_pickup_send(game, NET_TARGET_ALL, uid);
}
