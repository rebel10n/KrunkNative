// Deposit and Deposit FFA (Krunker v7.1.4 modes/depo.js, depoffa.js; doc/v7/map-logic.md §4.1
// rows 14 / 29): kills drop the victim's KR Tags (+1 for the kill) as "KR Tag" spawnables
// (medals.js 3, picked up in shared/src/spawnables.c), carriers deposit them one at a time on
// the active objective.
#include "mode_internal.h"

// depo.js / depoffa.js onKill: count = victim.dogTags (+1 when killed by someone else). A kill,
// or a suicide / fall with lastGround and more than one tag, drops them: up to maxPerCluster in
// one spawnable at the drop point, more in clusters of maxPerCluster plus the rest, each moved
// randInt(-10, 10) on x and z.
void mode_kill_deposit(Game *game, Player *victim, Player *killer, const int weapon_id) {
    (void) weapon_id;
    const GameModeConfig *config = &game->mode->config;
    int count = victim->mode.dog_tags;
    const int killed = killer && killer != victim;
    if (killed) count += 1;
    victim->mode.dog_tags = 0; // the spawn resets dogTags in v7 (player.reset); the tags are on the floor now
    if (!count) return;

    int use_ground = victim->mode.has_last_ground && (!killer || (killer == victim && count > 1));
    if (!killed && !use_ground) return;
    if (victim->mode.has_last_ground && !victim->on_ground) use_ground = 1;
    const vec3 at = use_ground ? victim->mode.last_ground : victim->position;

    const int per = config->max_per_cluster > 0 ? config->max_per_cluster : 3;
    if (count <= per) {
        spawnable_drop(game, SPAWNABLE_KR_TAG, victim, count, at);
        return;
    }

    const int clusters = count / per, rest = count % per;
    for (int i = 0; i < clusters; i++) {
        const vec3 jitter = {at.x + (float) mode_rand_int(-10, 10), at.y, at.z + (float) mode_rand_int(-10, 10)};
        spawnable_drop(game, SPAWNABLE_KR_TAG, victim, per, jitter);
    }
    if (rest) {
        const vec3 jitter = {at.x + (float) mode_rand_int(-10, 10), at.y, at.z + (float) mode_rand_int(-10, 10)};
        spawnable_drop(game, SPAWNABLE_KR_TAG, victim, rest, jitter);
    }
}

// depo.js update (every 500 ms) / depoffa.js (every 750 ms): every active carrier on the active
// objective deposits one tag: deposits++, "Deposited", objScore (50 / 75), "krt"; Deposit also
// gives its team +1.
void mode_update_deposit(Game *game, const float delta) {
    GameMode *mode = game->mode;
    if (!mode->objective_count || !mode->wait_timer_count || !mode->wait_timers[0].cont_time) return;
    mode->objective_timer -= delta;
    if (mode->objective_timer > 0.0f) return;
    mode->objective_timer = mode->config.teams ? 0.5f : 0.75f;

    const ModeObjective *objective = mode_active_objective(game);
    if (!objective) return;
    const int points = mode->config.obj_score ? mode->config.obj_score : 10;

    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player || !player->active || player->mode.dog_tags <= 0 || !mode_player_collides(player, objective->position, objective->half)) continue;
        player->mode.dog_tags--;
        player->mode.deposits++;
        if (mode->config.teams) mode_team_score_add(game, player->team, 1);
        player_score(player, points, 0);
        mode_send_medal(game, player, "Deposited", points);
        mode_send_counter(game, NET_MODE_COUNTER_KR_TAGS, player, (float) player->mode.dog_tags, 1);
        game->leaders_dirty = 1;
    }
}
