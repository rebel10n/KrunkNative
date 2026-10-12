// map-logic WP-B: the v7.1.4 battle-royale zone (libs/Zone.js, game.js update #24 / resetZone /
// syncData.zone; doc/v7/map-logic.md §1.8.2). The authority shrinks the circle, runs the stage
// waits ("Zone closing in ", a contTime waitTimer) and every 1000 ms broadcasts "zn" and takes 10 HP
// from every active player outside it (the Infected team is exempt). Clients tween to each "zn"
// over 900 ms and hide the zone once a size of 0 arrives.
#include <world.h>
#include "modes/mode_internal.h" // mode_push_wait_timer (game.waitTimers.push)
#include <pcg_basic.h>
#include <math.h>
#include <string.h>

#define ZONE_SHRINK_BASE 2.5f    // vLN253: circleShrinkSpeed = 2.5 * limitMM(zoneSpeed || 1, 1, 50)
#define ZONE_SYNC_MS 1000.0f     // game.zoneTimer
#define ZONE_DAMAGE 10.0f        // changeHealth(player, null, 10) per sync outside the circle
#define ZONE_TWEEN_MS 900.0f     // client TWEEN to each "zn"

static float zone_clampf(const float v, const float lo, const float hi) { return v < lo ? lo : v > hi ? hi : v; }

static double zone_random(void) { return ldexp((double) pcg32_random(), -32); } // Math.random()

// generateTargetCircle: a random point within half the current radius (zoneRnd), else the origin
static void zone_next_circle(WorldZone *zone, const int random) {
    zone->waiting = 0;
    float x = 0.0f, z = 0.0f;
    if (random) {
        const double angle = zone_random() * 2.0 * M_PI;
        const double dist = zone->scale.x / 2.0 * sqrt(zone_random());
        x = (float) (dist * cos(angle));
        z = (float) (dist * sin(angle));
    }
    zone->target_position = (vec3) {x, 0.0f, z};
    const float size = zone->stages ? zone->scale.x / 2.0f : 0.0f;
    zone->target_scale = (vec3) {size, 0.0f, size};
}

void zone_reset(Game *game) {
    WorldZone *zone = &game->world.zone;
    memset(zone, 0, sizeof(*zone));
    const Map *map = game->map;
    if (!map || !map->zone.enabled) return;

    const MapZoneConfig *cfg = &map->zone;
    zone->active = 1;
    zone->visible = 1;
    // Zone constructor: isNumber checks, then the config limits
    zone->wait_time = zone_clampf(cfg->wait, 5.0f, 500.0f) * 1000.0f;
    zone->stages_start = cfg->stages < 0 ? 0 : cfg->stages > 10 ? 10 : cfg->stages;
    zone->size = zone_clampf(cfg->size ? cfg->size : 50.0f, 0.0f, 10000.0f);
    zone->height = zone_clampf(cfg->height ? cfg->height : 1000.0f, 100.0f, 10000.0f);
    zone->shrink_speed = ZONE_SHRINK_BASE * zone_clampf(cfg->speed ? cfg->speed : 1.0f, 1.0f, 50.0f);

    // reset(): the full circle at the origin, then the first target
    zone->wait_timer = zone->wait_time;
    zone->stages = zone->stages_start;
    zone->position = (vec3) {0.0f, 0.0f, 0.0f};
    zone->scale = (vec3) {zone->size, zone->height, zone->size};
    zone_next_circle(zone, cfg->random);
    zone->broadcast_timer = 0.0f; // finishInit: zoneTimer = 0 (the first tick broadcasts)

    // the client starts from the reset circle (Zone.reset with a render: scale (size, height, size))
    zone->tween_from[0] = zone->tween_to[0] = 0.0f;
    zone->tween_from[1] = zone->tween_to[1] = 0.0f;
    zone->tween_from[2] = zone->tween_to[2] = zone->size;
    zone->tween_from[3] = zone->tween_to[3] = zone->size;
    zone->tween_time = ZONE_TWEEN_MS;
}

// Zone.update(dt) on the server: wait / next stage once the target is reached, else move the
// size and the centre toward their targets at shrinkSpeed. The size steps along the normalised
// 3-vector (x, y, z) - (tx, 0, tz), so the height dilutes the radial rate (a v7 quirk kept, Appendix D).
static void zone_step(Game *game, WorldZone *zone, const float dt) {
    if (zone->scale.x <= zone->target_scale.x && zone->scale.z <= zone->target_scale.z) {
        if (zone->stages) {
            zone->wait_timer -= dt * 1000.0f;
            if (!zone->waiting) {
                zone->waiting = 1;
                // {time: waitTimer, contTime: true, canDMG: true, msg: "Zone closing in "}
                const ModeWaitTimer timer = {.time = zone->wait_timer / 1000.0f, .msg = "Zone closing in ", .cont_time = 1};
                if (game->mode) mode_push_wait_timer(game, &timer, 0);
            }
            if (zone->wait_timer <= 0.0f) {
                zone->wait_timer = zone->wait_time;
                zone->stages--;
                zone_next_circle(zone, game->map && game->map->zone.random);
            }
        }
        return;
    }
    const double sx = zone->target_scale.x - zone->scale.x, sy = zone->target_scale.y - zone->scale.y, sz = zone->target_scale.z - zone->scale.z;
    double len = sqrt(sx * sx + sy * sy + sz * sz);
    if (len == 0) len = 1;
    const double step = (double) dt * zone->shrink_speed;
    zone->scale.x = (float) (zone->scale.x + sx / len * step);
    zone->scale.y = (float) (zone->scale.y + sy / len * step);
    zone->scale.z = (float) (zone->scale.z + sz / len * step);
    const double px = zone->target_position.x - zone->position.x, py = zone->target_position.y - zone->position.y,
                 pz = zone->target_position.z - zone->position.z;
    double plen = sqrt(px * px + py * py + pz * pz);
    if (plen == 0) plen = 1;
    zone->position.x = (float) (zone->position.x + px / plen * step);
    zone->position.y = (float) (zone->position.y + py / plen * step);
    zone->position.z = (float) (zone->position.z + pz / plen * step);
}

static NetZonePacket zone_packet(const WorldZone *zone) {
    return (NetZonePacket) {zone->position.x, zone->position.z, zone->scale.x, zone->scale.z}; // getData()
}

// Zone.isOutside: dist2D(player, centre) > circleSize.x
static int zone_outside(const WorldZone *zone, const vec3 p) {
    return hypotf(p.x - zone->position.x, p.z - zone->position.z) > zone->scale.x;
}

void zone_update(Game *game, const float delta) {
    WorldZone *zone = &game->world.zone;
    if (!zone->active || !game_is_authority(game)) return;
    // game.update: the zone runs while there is no waitTimer or the first one keeps the clock
    // running (contTime: the zone's own stage wait); warmup / start timers hold it
    if (game->mode && game->mode->wait_timer_count && !game->mode->wait_timers[0].cont_time) return;

    const float delta_ms = delta * 1000.0f * game->config.delta_mlt;
    zone->broadcast_timer -= delta_ms;
    zone_step(game, zone, delta_ms / 1000.0f);
    if (zone->broadcast_timer > 0.0f) return;
    zone->broadcast_timer = ZONE_SYNC_MS;

    const NetZonePacket packet = zone_packet(zone);
    game_send(game, NET_TARGET_ALL, NET_PACKET_ZONE, &packet, sizeof(packet));

    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player || !player->active) continue;
        const char *team = player->team > 0 ? mode_team_name(game, player->team) : NULL;
        if (team && !strcmp(team, "Infected")) continue; // teamOptions[team - 1] == "Infected"
        if (!zone_outside(zone, player->position)) continue;
        if (player_change_health(player, NULL, ZONE_DAMAGE, 0)) player_kill(player, NULL, NULL, 0);
    }
}

int zone_contains(const Game *game, const vec3 position) {
    const WorldZone *zone = &game->world.zone;
    return !zone->active || !zone_outside(zone, position);
}

void zone_apply_packet(Game *game, const NetZonePacket *packet) {
    WorldZone *zone = &game->world.zone;
    if (!zone->active) zone_reset(game); // a zone the client did not know yet (no map zone)
    if (!zone->active) {
        zone->active = 1;
        zone->height = 1000.0f;
    }
    float current[4];
    const float k = zone->tween_time >= ZONE_TWEEN_MS ? 1.0f : zone->tween_time / ZONE_TWEEN_MS;
    for (int i = 0; i < 4; i++) current[i] = zone->tween_from[i] + (zone->tween_to[i] - zone->tween_from[i]) * k;
    memcpy(zone->tween_from, current, sizeof(current)); // shrinkTween / moveTween .stop(), then from here
    zone->tween_to[0] = packet->x;
    zone->tween_to[1] = packet->z;
    zone->tween_to[2] = packet->size_x;
    zone->tween_to[3] = packet->size_z;
    zone->tween_time = 0.0f;
    zone->synced = 1;
    if (packet->size_x <= 0.0f || packet->size_z <= 0.0f) zone->visible = 0;
    // the client's own circle follows the server's (zone_contains for HUD / prediction use)
    zone->position = (vec3) {packet->x, 0.0f, packet->z};
    zone->scale.x = packet->size_x;
    zone->scale.z = packet->size_z;
}

void zone_animate(Game *game, const float delta) {
    WorldZone *zone = &game->world.zone;
    if (zone->active && zone->tween_time < ZONE_TWEEN_MS) zone->tween_time = fminf(ZONE_TWEEN_MS, zone->tween_time + delta * 1000.0f);
}

int zone_circle(const Game *game, float *x, float *z, float *radius, float *height) {
    const WorldZone *zone = &game->world.zone;
    if (!zone->active || !zone->visible) return 0;
    float cx, cz, r;
    const int authority = game_is_authority(game);
    if (authority || !zone->synced) {
        cx = zone->position.x;
        cz = zone->position.z;
        r = zone->scale.x;
        if (!authority) {
            cx = zone->tween_to[0];
            cz = zone->tween_to[1];
            r = zone->tween_to[2];
        }
    } else {
        const float k = zone->tween_time >= ZONE_TWEEN_MS ? 1.0f : zone->tween_time / ZONE_TWEEN_MS;
        cx = zone->tween_from[0] + (zone->tween_to[0] - zone->tween_from[0]) * k;
        cz = zone->tween_from[1] + (zone->tween_to[1] - zone->tween_from[1]) * k;
        r = zone->tween_from[2] + (zone->tween_to[2] - zone->tween_from[2]) * k;
    }
    if (x) *x = cx;
    if (z) *z = cz;
    if (radius) *radius = r;
    if (height) *height = zone->height;
    return 1;
}

void zone_send_state(Game *game, const int uid) {
    const WorldZone *zone = &game->world.zone;
    if (!zone->active || !game_is_authority(game)) return;
    const NetZonePacket packet = zone_packet(zone); // syncData.zone = map.zone.getData()
    game_send(game, uid, NET_PACKET_ZONE, &packet, sizeof(packet));
}
