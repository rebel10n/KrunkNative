// Zombies (mode 35, Krunker v7.1.4 modes/zom.js; doc/v7/map-logic.md §4.1b). WP-D ships the mode
// shell: the table entry (modes.c), its condition / winCondition, the onKill score loss and the
// onPreEnd wave score. map-logic WP-H stage 2 owns this file and replaces the hooks below with
// the waves, AI templates (map_ai_spawn), zombie_spawn / skull_spawn / portal_spawn / frog_spawn
// SERVER_NODEs, mystery box / perk / krum machines, power-ups and easter eggs.
#include "mode_internal.h"

// zom.js gameStart: zWave 0 and the wave data (WP-H)
void mode_zombies_game_start(Game *game) {
    (void) game;
}

// zom.js update: the wave loop (WP-H)
void mode_zombies_update(Game *game, const float delta) {
    (void) game;
    (void) delta;
}

// zom.js onKill: a player died, so everyone in the game (and the victim) with a positive score
// loses 10% of it.
void mode_zombies_on_kill(Game *game, Player *victim, Player *killer, const int weapon_id) {
    (void) killer;
    (void) weapon_id;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player || !(player->active || player == victim) || player->score <= 0) continue;
        const int loss = player->score / 10; // Math.floor(score * 0.1)
        if (loss > 0) player_score(player, -loss, 0);
    }
}

// zom.js onPreEnd: everyone's score becomes (zWave || 1) * 100 (the wave is WP-H's)
void mode_zombies_on_pre_end(Game *game) {
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (player) player->score = 1 * 100;
    }
}

// zom.js onAIDeath: power-up drops, the 5th-wave Ammo Cache (WP-H)
void mode_zombies_on_ai_death(Game *game, struct GameAI_t *ai, Player *killer) {
    (void) game;
    (void) ai;
    (void) killer;
}

// zom.js onTrigger (action 71): the Zombie Portal at portal_spawn (WP-H)
void mode_zombies_on_trigger(Game *game, Player *player, const int value) {
    (void) game;
    (void) player;
    (void) value;
}

// the Zombies spawnables' pickups (WP-H): power-ups, perk bottle, portal, skull, free upgrade
void mode_zombies_spawnable(Game *game, Player *player, struct GameAI_t *ai, const int type) {
    (void) game;
    (void) player;
    (void) ai;
    (void) type;
}
