// Server-authoritative projectiles: Krunker v3.1.1 src/data/projectile.js, v7.1.4 projectiles.js
// (charge-scaled shots, effects on hit, head / leg multipliers, v7 hitbox padding)
// (flight, map/player collision, "pr"/"pre" packets) and game.explosion (splash
// damage with linear falloff, 0.2x self damage). Krunker speeds are units per
// millisecond; the simulation keeps those units and converts the tick delta.
#include <shared.h>
#include <world.h> // map-logic WP-B: PvP filter, terrain
#include <ai.h>
#include <prefabs.h> // prefab_info: geometry of an object hit
#include <math.h>
#include <string.h>

// v7.1.4 projectiles.stored[0], the thrown knife: dmg 200 -> 180, scaled by the throw charge.
static const ProjectileConfig g_thrown_melee = {.speed = 0.38f, .gravity = 0.0002f, .range = 1500.0f, .damage = 180.0f};

// The weapon's own pj_* block (weapons.js); NULL when it fires no projectile (pj_disable: the
// hitscan guns only fly with the bullet-drop game setting, player.c reads projectile_config then).
const ProjectileConfig *projectile_config_for(const Weapon *weapon, const int thrown_melee) {
    if (thrown_melee) return &g_thrown_melee;
    if (!weapon || weapon->projectile_config.speed <= 0.0f || weapon->projectile_disable) return NULL;
    return &weapon->projectile_config;
}

static void projectile_send_end(Game *game, const GameProjectile *projectile, const int hit_object) {
    const NetProjectileEndPacket packet = {
        .id = projectile->id,
        .position = {projectile->position.x, projectile->position.y, projectile->position.z},
        // "pre": others only see an impact when the projectile has an owner (AI shots never)
        .impact = (uint8_t) (hit_object && !projectile->config.no_impact && projectile->owner_uid >= 0),
    };
    game_send(game, NET_TARGET_ALL, NET_PACKET_PROJECTILE_END, &packet, sizeof(packet));
}

// projectiles.init: the first free slot, Krunker's rounding of the launch values.
static void projectile_spawn(Game *game, Player *owner, const int weapon_index, const ProjectileConfig *base,
                             const vec3 origin, const float yaw, const float pitch, const int thrown_melee, const int ai_kind,
                             const ProjectileShot *shot) {
    if (!game_is_authority(game) || !base) return;

    // Projectile.init: spd * v, dmg = ceil(dmg * d) * dmgMlt (v7.1.4 charge)
    ProjectileConfig scaled = *base;
    if (shot) {
        if (shot->speed_mlt > 0.0f) scaled.speed *= shot->speed_mlt;
        scaled.damage = ceilf(scaled.damage * (shot->damage_d > 0.0f ? shot->damage_d : 1.0f));
        if (shot->damage_mlt > 0.0f) scaled.damage *= shot->damage_mlt;
    }
    const ProjectileConfig *config = &scaled;

    GameProjectile *projectile = NULL;
    for (int i = 0; i < GAME_MAX_PROJECTILES; i++) {
        if (!game->projectiles[i].active) {
            projectile = &game->projectiles[i];
            projectile->id = i;
            break;
        }
    }
    if (!projectile) return;

    const float yaw_r = roundf(yaw * 1000.0f) / 1000.0f;
    const float pitch_r = roundf(pitch * 1000.0f) / 1000.0f;

    projectile->owner_uid = owner ? owner->uid : -1;
    projectile->weapon_index = weapon_index;
    projectile->active = 1;
    projectile->skip_move = 1;
    projectile->thrown_melee = (unsigned char) !!thrown_melee;
    projectile->ai_kind = ai_kind;
    // projectile.js init: stateConst = owner && !dontPred ? ping * 2 * lagComp : 0 (the owner's
    // getStateConst; anchored lag compensation measures it from this shot's input, player.c)
    projectile->state_const = owner && !config->dont_pred ? player_get_state_const(owner) : 0.0f;
    projectile->position = (vec3) {roundf(origin.x * 10.0f) / 10.0f, roundf(origin.y * 10.0f) / 10.0f, roundf(origin.z * 10.0f) / 10.0f};
    projectile->config = *config;
    projectile->effect = shot ? shot->effect : -1;
    projectile->effect_count = shot && shot->effect_count > 0 ? shot->effect_count : 1;
    projectile->speed = config->speed;
    projectile->range = config->range;
    projectile->yaw_dir = yaw_r - (float) M_PI;
    projectile->pitch_dir = pitch_r;
    projectile->velocity = (vec3) {
        config->speed * sinf(projectile->yaw_dir) * cosf(pitch_r),
        config->speed * sinf(pitch_r),
        config->speed * cosf(projectile->yaw_dir) * cosf(pitch_r),
    };

    const NetProjectilePacket packet = {
        .id = projectile->id,
        .owner_uid = projectile->owner_uid,
        .weapon_index = weapon_index,
        .position = {projectile->position.x, projectile->position.y, projectile->position.z},
        .velocity = {projectile->velocity.x, projectile->velocity.y, projectile->velocity.z},
        .gravity = config->gravity,
        .range = config->range,
        .ai_kind = (uint8_t) ai_kind,
    };
    game_send(game, NET_TARGET_ALL, NET_PACKET_PROJECTILE, &packet, sizeof(packet));
}

void game_spawn_projectile(Game *game, Player *owner, const int weapon_index, const ProjectileConfig *config,
                           const vec3 origin, const float yaw, const float pitch, const int thrown_melee) {
    projectile_spawn(game, owner, weapon_index, config, origin, yaw, pitch, thrown_melee, 0, NULL);
}

void game_spawn_projectile_ex(Game *game, Player *owner, const int weapon_index, const ProjectileConfig *config, const vec3 origin,
                              const float yaw, const float pitch, const int thrown_melee, const ProjectileShot *shot) {
    projectile_spawn(game, owner, weapon_index, config, origin, yaw, pitch, thrown_melee, 0, shot);
}

// ai.js shots: projectiles.init(x, y, z, yaw, pitch, stored[kind], null, {}, true, "orb_0")
void game_spawn_ai_projectile(Game *game, const int stored_kind, const vec3 origin, const float yaw, const float pitch) {
    projectile_spawn(game, NULL, -1, game_ai_projectile_config(stored_kind), origin, yaw, pitch, 0, stored_kind, NULL);
}

// game.explosion(x, y, z, radius, dmg, owner): 1 - distance / radius of the damage,
// a fifth for the owner (who may hurt themselves).
void game_explosion(Game *game, const vec3 position, float radius, float damage, Player *owner, const int weapon_index) {
    if (!game_is_authority(game) || radius <= 0.0f) return;
    radius *= mode_explosion_size_mlt(game);            // v7 mode.explSMlt (Impulsed 2), before the "ex"
    if (owner) damage *= player_attr_damage(owner);     // v7 source.attributes.dmg (Strength), players and AIs

    const NetExplosionPacket packet = {.position = {roundf(position.x), roundf(position.y), roundf(position.z)}, .radius = roundf(radius)};
    game_send(game, NET_TARGET_ALL, NET_PACKET_EXPLOSION, &packet, sizeof(packet));

    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player || !player->active) continue;
        if (!world_pvp_allows(owner, player)) continue; // WP-B: v7 explosion noPVP / forcePVP filter

        const float dx = player->position.x - position.x;
        const float dy = player->position.y - position.y;
        const float dz = player->position.z - position.z;
        const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
        const float falloff = 1.0f - distance / radius;
        if (falloff <= 0.0f) continue;

        const float dealt = damage * falloff * (player == owner ? 0.2f : 1.0f) * mode_explosion_damage_mlt(game, owner, player); // explMlt
        if (player_change_health(player, owner, dealt, 0)) {
            PlayerKillInfo info = {.weapon_id = weapon_index, .distance = distance};
            player_kill(player, owner, &info, 0);
        }
    }

    // game.explosion over AI.ais: only with an owner, changeHealth(-dmg * falloff, owner)
    for (int i = 0; owner && game->ai && i < game->ai->count; i++) {
        GameAI *ai = &game->ai->ais[i];
        if (!ai->active) continue;
        const float dx = ai->position.x - position.x, dy = ai->position.y - position.y, dz = ai->position.z - position.z;
        const float falloff = 1.0f - sqrtf(dx * dx + dy * dy + dz * dz) / radius;
        if (falloff > 0.0f) game_ai_damage(game, ai, owner, damage * falloff, 0);
    }
}

typedef struct {
    Player *player;
    GameAI *ai;
    float t;
    Object *object;     // u[0].obj of a map object hit
    PlayerStoredState state; // the player's rewound box (head / leg zones)
} ProjectileHit;

static int projectile_object_blocks(const Object *object) {
    // Same filter as the hitscan (player.c combat_object_shootable): Krunker noShoot zones/helpers.
    if (!object->active || object->no_shoot || object->collision_type == COLLISION_TYPE_NONE) return 0;
    return !(object->kill || object->score_zone || object->team_zone || object->teleporter || object->checkpoint ||
             object->pickup || object->flag || object->premium || object->verified ||
             object->ladder || object->ramp || object->prefab == PREFAB_TEAM_ZONE || object->prefab == PREFAB_SPAWN_POINT ||
             object->prefab == PREFAB_CAMERA_POSITION || object->prefab == PREFAB_SPECTATE_CAM);
}

// v7 bug fixed (thrown knives frozen in the air): projectiles.js sends "pre" with impact 1 for
// every map-object hit, so the client sticks the knife (linger 5 s) and puts impact dust wherever
// the projectile stopped. noVis volumes still block it (v7: roof blockers and clip walls stop shots
// too), but nobody sees them: on lostworld_s6, sandstorm_v3 or oasis_v3, 4 to 19 throws in a hundred
// from the spawns end on one far above any visible surface, and the knife hung there in mid-air.
// The same for a destructible the hit just destroyed. Those ends carry no impact: the knife goes away
// as on a player hit. (v7's own spray trace, players.spray, skips noVis objects.)
static int projectile_object_visible(const Object *object) {
    const PrefabInfo *info = prefab_info((int) object->prefab);
    return object->active && !object->no_vis && !(info && info->geometry == PREFAB_GEO_NONE);
}

// Nearest hit along this tick's step (lineInRect against step = velocity * dt).
static ProjectileHit projectile_cast(Game *game, const GameProjectile *projectile, const float dt_ms, int *hit_any) {
    const vec3 step = {projectile->velocity.x * dt_ms, projectile->velocity.y * dt_ms, projectile->velocity.z * dt_ms};
    ProjectileHit best = {NULL, NULL, 2.0f};
    *hit_any = 0;

    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *object = game->map->objects[i];
        if (!projectile_object_blocks(object)) continue;
        if (game_ai_projectile_skips_objects(game, projectile)) break; // projOb.noObjC (WP-F, shared/src/ai.c)

        const float t = line_in_rect(projectile->position, step, object->position, object->scale);
        if (t > 0.0f && t <= 1.0f && t < best.t) {
            best = (ProjectileHit) {NULL, NULL, t, (Object *) object};
            *hit_any = 1;
        }
    }

    // WP-B: projectiles.js:215 the terrain stops the step too
    const float terrain_t = world_terrain_fraction(game, projectile->position, step);
    if (terrain_t > 0.0f && terrain_t <= 1.0f && terrain_t < best.t) {
        best = (ProjectileHit) {NULL, NULL, terrain_t};
        *hit_any = 1;
    }

    const float step_length = projectile->speed * dt_ms;

    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player || !player->active || player->uid == projectile->owner_uid) continue;
        if (!world_pvp_allows(game_find_player(game, projectile->owner_uid), player)) continue; // WP-B: noPVP / forcePVP

        // p = fetchState(q, m.stateConst): every tick of the flight tests the rewound box
        PlayerStoredState state;
        if (!players_fetch_state(player, projectile->state_const, &state)) continue;

        float t;
        if (projectile->config.hit_box > 0.0f) {
            const float dx = projectile->position.x - state.position.x;
            const float dy = projectile->position.y - (state.position.y + state.adjusted_height * 0.5f);
            const float dz = projectile->position.z - state.position.z;
            const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
            t = distance <= projectile->config.hit_box ? distance / projectile->config.hit_box : -1.0f;
        } else {
            // v7.1.4 projectiles.js: scale + hitboxPadW sideways, ah + hitboxPadH on top
            // (game hitBoxPad 0.6 / comp 0.1 + class hbS / hbH; v3.1.1 padded by 1).
            const float half = player->scale + player_hitbox_pad_w(player);
            const vec3 box = {half * 2.0f, state.adjusted_height + player_hitbox_pad_h(player), half * 2.0f};
            t = step_length > 0.0f ? line_in_rect(projectile->position, step, state.position, box) : -1.0f;
        }

        if (t > 0.0f && t <= 1.0f && t < best.t) {
            best = (ProjectileHit) {player, NULL, t, NULL, state};
            *hit_any = 1;
        }
    }

    // projectiles.js: AI boxes for a player's projectile or another AI's shot (team-checked, never the shooter)
    for (int i = 0; game->ai && i < game->ai->count; i++) {
        GameAI *ai = &game->ai->ais[i];
        if (step_length <= 0.0f || !game_ai_projectile_can_hit(game, projectile, ai)) continue;
        const float t = game_ai_line_hit(ai, projectile->position, step);
        if (t > 0.0f && t <= 1.0f && t < best.t) {
            best = (ProjectileHit) {NULL, ai, t};
            *hit_any = 1;
        }
    }

    return best;
}

static void projectile_tick(Game *game, GameProjectile *projectile, const float dt_ms) {
    if (projectile->skip_move) {
        projectile->skip_move = 0;
    } else {
        if (projectile->config.gravity) {
            projectile->velocity.y -= projectile->config.gravity * dt_ms;
            projectile->speed = sqrtf(projectile->velocity.x * projectile->velocity.x + projectile->velocity.y * projectile->velocity.y +
                                      projectile->velocity.z * projectile->velocity.z);
            projectile->pitch_dir = asinf(CLAMP(projectile->velocity.y / projectile->speed, -1.0f, 1.0f));
        }

        projectile->position.x += projectile->velocity.x * dt_ms;
        projectile->position.y += projectile->velocity.y * dt_ms;
        projectile->position.z += projectile->velocity.z * dt_ms;

        projectile->range -= projectile->speed * dt_ms;
        if (projectile->range <= 0.0f) projectile->active = 0;
    }

    int hit_object = 0;

    if (projectile->active) {
        int hit_any = 0;
        const ProjectileHit hit = projectile_cast(game, projectile, dt_ms, &hit_any);

        if (hit_any) {
            Player *owner = game_find_player(game, projectile->owner_uid);

            // v7.1.4: smite, then attributes.dmg (Strength); the bot dmgMlt is player_change_health's
            float damage = owner && owner->smite ? 10000.0f : projectile->config.damage;
            if (owner) damage *= player_attr_damage(owner);

            // u[0].ai: changeHealth(-dmg, owner, thrown knife), killScore, "4"
            if (hit.ai) game_ai_projectile_hit_ai(game, projectile, hit.ai, owner, damage); // WP-F: player or AI shooter rules
            if (hit.player && !owner) game_ai_projectile_hit_player(game, projectile, hit.player); // an AI shot's effectInd (WP-F)
            // u[0].obj: destructible health and onShoot / onDamage / onProjectile / onThrowingMelee triggers
            if (hit.object) game_object_hit(game, hit.object, owner, damage, projectile->thrown_melee, GAME_HIT_PROJECTILE);
            // an impact on the terrain or on a map object still there to see (after this hit's damage)
            hit_object = !hit.player && !hit.ai && (!hit.object || projectile_object_visible(hit.object));

            if (hit.player && damage > 0.0f) {
                // v7.1.4: a player's direct hit takes the weapon's hsMlt / lsMlt (1 / 1 on every stock
                // projectile weapon). v7 bugs fixed: it tests the head band against the target's live
                // y and plain headScale; here the rewound box and headScale * headMlt, like hitscan.
                int head = 0, leg = 0;
                const Weapon *weapon = owner && projectile->weapon_index >= 0 && (size_t) projectile->weapon_index < game->weapon_count
                    ? game->weapons[projectile->weapon_index] : NULL;
                if (weapon) {
                    const float hit_y = projectile->position.y + projectile->velocity.y * dt_ms * hit.t;
                    head = player_hit_is_head(hit.player, hit.state.position.y, hit.state.adjusted_height, hit_y);
                    leg = player_hit_is_leg(hit.player, hit.state.position.y, hit_y);
                    damage = player_hit_damage(game, weapon, damage, head, leg);
                }
                // effectInd (Zapper): Zapped stacks, credited to the owner, unless a teammate
                if (projectile->effect >= 0 && (!owner || !owner->team || owner->team != hit.player->team ||
                                                (game->mode && game->mode->config.dmg_team) || game->config.friendly_fire)) {
                    player_give_effect(hit.player, projectile->effect, projectile->effect_count);
                    if (projectile->effect == EFFECT_ZAPPED && owner) hit.player->zapped_by = owner->uid;
                }
                if (player_change_health(hit.player, owner, damage, head)) {
                    PlayerKillInfo info = {.weapon_id = projectile->weapon_index, .distance = 0.0f};
                    info.thrown = projectile->thrown_melee;
                    info.headshot = (unsigned char) head;
                    info.legshot = (unsigned char) (leg && !head); // WP-E part 2c: the "ls" stat
                    player_kill(hit.player, owner, &info, 0);
                }
            }

            const float t = hit.t - 0.02f;
            projectile->position.x += t * projectile->velocity.x * dt_ms;
            projectile->position.y += t * projectile->velocity.y * dt_ms;
            projectile->position.z += t * projectile->velocity.z * dt_ms;
            projectile->active = 0;
        }
    }

    if (projectile->active) return;

    if (projectile->config.explode > 0.0f) {
        game_explosion(game, projectile->position, projectile->config.explode, projectile->config.explode_damage,
                       game_find_player(game, projectile->owner_uid), projectile->weapon_index);
    }

    projectile_send_end(game, projectile, hit_object);
}

void game_tick_projectiles(Game *game, const float delta) {
    if (!game_is_authority(game) || !game->map) return;

    const float dt_ms = delta * 1000.0f;

    for (int i = 0; i < GAME_MAX_PROJECTILES; i++) {
        GameProjectile *projectile = &game->projectiles[i];
        if (projectile->active) projectile_tick(game, projectile, dt_ms);
    }
}
