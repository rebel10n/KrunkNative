// Domination (Krunker v7.1.4 game.js resetDomZones / hasAllZones / updateDomZones :1663-1823,
// modes/dom.js; doc/v7/map-logic.md §4.1a). DOMINATION_ZONE map objects (uid = their index) are
// captured by standing on them alone as a team; owned zones score their team every 1.5 s.
// The server half runs on the authority, the client half (mode_dom_client_tick) interpolates
// captureT between "uct" packets exactly like v7's client runs the same function.
#include "mode_internal.h"
#include <string.h>

// resetDomZones (finishInit)
void mode_reset_dom_zones(Game *game) {
    GameMode *mode = game->mode;
    for (int z = 0; z < mode->dom_zone_count; z++) {
        ModeDomZone *zone = &mode->dom_zones[z];
        zone->captured_by = 0;
        zone->capture_t = 0.0f;
        zone->last_team_on = 0;
        zone->teams_on_count = 0;
        zone->on_point = 0;
        zone->mlt = 1.0f;
    }
}

// hasAllZones: the team that owns every zone, 0 none (also 0 without zones)
int mode_has_all_zones(const Game *game) {
    const GameMode *mode = game->mode;
    int team = 0;
    for (int z = 0; z < mode->dom_zone_count; z++) {
        const int owner = mode->dom_zones[z].captured_by;
        if (!owner) return 0;
        if (team && team != owner) return 0;
        team = owner;
    }
    return team;
}

// dom.js collisionCheck: player.onZone = the uid of a zone the player stands in (the last one)
void mode_collision_dom(Game *game, Player *player) {
    const GameMode *mode = game->mode;
    for (int z = 0; z < mode->dom_zone_count; z++) {
        if (mode_player_collides(player, mode->dom_zones[z].position, mode->dom_zones[z].half)) player->mode.on_zone = z;
    }
}

// dom.js onKill: killing from a zone your team owns +25 "Defender"; killing someone standing in a
// zone their team owns +25 "Attacker".
void mode_kill_dom(Game *game, Player *victim, Player *killer, const int weapon_id) {
    (void) weapon_id;
    const GameMode *mode = game->mode;
    if (!killer || killer == victim) return;
    const int kz = killer->mode.on_zone, vz = victim->mode.on_zone;
    if (kz >= 0 && kz < mode->dom_zone_count && mode->dom_zones[kz].captured_by == killer->team) {
        player_score(killer, 25, 0);
        mode_send_medal(game, killer, "Defender", 25);
    }
    if (vz >= 0 && vz < mode->dom_zone_count && mode->dom_zones[vz].captured_by == victim->team) {
        player_score(killer, 25, 0);
        mode_send_medal(game, killer, "Attacker", 25);
    }
}

// The capture step shared by both halves: one team alone on a zone it does not own.
// Returns 1 when the zone's state changed in a way "uct" reports.
static int dom_step(Game *game, ModeDomZone *zone, const float delta, const int server, int *captured) {
    const GameModeConfig *config = &game->mode->config;
    int changed = 0;
    *captured = 0;

    if (zone->teams_on_count != 1) {
        if (!zone->teams_on_count) {
            zone->capture_t = 0.0f;
            zone->last_team_on = 0;
        }
        return 0; // contested: captureT frozen
    }

    const int team = zone->teams_on[0];
    if (zone->captured_by && zone->captured_by == team) {
        zone->capture_t = 0.0f;
        zone->last_team_on = 0;
        return 0;
    }

    if (zone->last_team_on != team) {
        zone->capture_t = 0.0f;
        changed = 1;
    }

    if (server) {
        const int all = mode_has_all_zones(game);
        const float old_mlt = zone->mlt;
        zone->last_team_on = team;
        zone->mlt = 1.0f + (float) (zone->on_point - 1) * config->per_player_mlt;
        if (all && all != team) zone->mlt *= config->lockout_mlt;
        if (old_mlt != zone->mlt) changed = 1;
    }

    zone->capture_t += delta * zone->mlt;

    if (server && zone->capture_t >= config->capture_time) {
        changed = 1;
        *captured = 1;
        zone->capture_t = 0.0f;
        zone->captured_by = team;
    }
    return changed;
}

// updateDomZones (server): owned zones score +10 every 1.5 s, occupancy, capture progress,
// "cct" + rewards on a capture, "uct" for every zone that changed.
void mode_tick_dom(Game *game, const float delta) {
    GameMode *mode = game->mode;
    if (!mode->config.dom || !mode->dom_zone_count) return;
    if (mode->wait_timer_count && !mode->wait_timers[0].cont_time) return;

    int tick = 0;
    mode->objective_timer -= delta;
    if (mode->objective_timer <= 0.0f) {
        mode->objective_timer = MODE_OBJECTIVE_TICK;
        tick = 1;
    }

    int changed_zones[MODE_MAX_DOM_ZONES];
    int changed_count = 0;

    for (int z = 0; z < mode->dom_zone_count; z++) {
        ModeDomZone *zone = &mode->dom_zones[z];
        const int old_teams = zone->teams_on_count;

        if (zone->captured_by && tick) {
            mode_team_score_add(game, zone->captured_by, 10);
            game->leaders_dirty = 1;
        }

        Player *on_point[64];
        int on_point_count = 0;
        zone->teams_on_count = 0;
        for (size_t i = 0; i < game->player_count; i++) {
            Player *player = game->players[i];
            if (!player || !player->active || !mode_player_collides(player, zone->position, zone->half)) continue;
            int known = 0;
            for (int t = 0; t < zone->teams_on_count; t++) known |= zone->teams_on[t] == player->team;
            if (!known) {
                if (zone->teams_on_count < MODE_TEAMS) zone->teams_on[zone->teams_on_count++] = player->team;
                if (zone->teams_on_count > 1) break; // a second team: contested
            }
            if (on_point_count < 64) on_point[on_point_count++] = player;
        }
        zone->on_point = on_point_count;

        int captured;
        int changed = old_teams != zone->teams_on_count;
        changed |= dom_step(game, zone, delta, 1, &captured);

        if (captured) {
            game->leaders_dirty = 1;
            mode_send_dom_capture(game, z, zone->captured_by);
            const int all = mode_has_all_zones(game);
            if (all) mode_team_score_add(game, all, 250);
            for (int p = 0; p < on_point_count; p++) {
                Player *player = on_point[p];
                if (player->team != zone->captured_by) continue;
                player->mode.caps++;
                player_score(player, 200, 0);
                mode_send_medal(game, player, "Secured", 200);
                if (on_point_count > 2) {
                    player_score(player, 25, 0);
                    mode_send_medal(game, player, "Team Work", 25);
                }
            }
        }

        if (changed) changed_zones[changed_count++] = z;
    }

    if (changed_count) mode_send_dom_update(game, changed_zones, changed_count, NET_TARGET_ALL);
}

// The client half of updateDomZones: captureT grows between "uct" packets; and the players' onZone.
void mode_dom_client_tick(Game *game, const float delta) {
    if (!game || !game->mode || game_is_authority(game)) return;
    // player.js update runs on the client too: onZone = null, then mode.collisionCheck (the
    // game-overlay's capture bar / CONTESTED box read the local player's onZone)
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player) continue;
        player->mode.on_zone = -1;
        if (player->active && game->mode->config.collision_check) game->mode->config.collision_check(game, player);
    }
    if (!game->mode->config.dom) return;
    for (int z = 0; z < game->mode->dom_zone_count; z++) {
        int captured;
        dom_step(game, &game->mode->dom_zones[z], delta, 0, &captured);
    }
}
