// Server-authoritative projectiles: Krunker v3.1.1 src/data/projectile.js
// (flight, map/player collision, "pr"/"pre" packets) and game.explosion (splash
// damage with linear falloff, 0.2x self damage). Krunker speeds are units per
// millisecond; the simulation keeps those units and converts the tick delta.
#include <shared.h>
#include <math.h>
#include <string.h>

// weapons.js projectile blocks (the native Weapon struct does not carry them) and
// projectiles.stored[0], the thrown knife.
static const struct {
    const char *weapon;
    ProjectileConfig config;
} g_projectile_configs[] = {
    {"Rocket Launcher", {.speed = 0.72f, .gravity = 0.00008f, .range = 1500.0f, .damage = 50.0f, .explode = 35.0f, .explode_damage = 35.0f, .no_impact = 1}},
    {"Crossbow", {.speed = 0.68f, .gravity = 0.0f, .range = 1500.0f, .damage = 200.0f}},
    {"Blaster", {.speed = 1.0f, .gravity = 0.0f, .range = 500.0f, .damage = 35.0f, .no_impact = 1}},
};

static const ProjectileConfig g_thrown_melee = {.speed = 0.38f, .gravity = 0.0002f, .range = 1500.0f, .damage = 200.0f};

const ProjectileConfig *projectile_config_for(const Weapon *weapon, const int thrown_melee) {
    if (thrown_melee) return &g_thrown_melee;
    if (!weapon || !weapon->name) return NULL;

    for (size_t i = 0; i < sizeof(g_projectile_configs) / sizeof(g_projectile_configs[0]); i++) {
        if (!strcmp(g_projectile_configs[i].weapon, weapon->name)) return &g_projectile_configs[i].config;
    }

    return NULL;
}

static void projectile_send_end(Game *game, const GameProjectile *projectile, const int hit_object) {
    const NetProjectileEndPacket packet = {
        .id = projectile->id,
        .position = {projectile->position.x, projectile->position.y, projectile->position.z},
        .impact = (uint8_t) (hit_object && !projectile->config.no_impact),
    };
    game_send(game, NET_TARGET_ALL, NET_PACKET_PROJECTILE_END, &packet, sizeof(packet));
}

// projectiles.init: the first free slot, Krunker's rounding of the launch values.
void game_spawn_projectile(Game *game, Player *owner, const int weapon_index, const ProjectileConfig *config,
                           const vec3 origin, const float yaw, const float pitch, const int thrown_melee) {
    if (!game_is_authority(game) || !config) return;

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
    projectile->position = (vec3) {roundf(origin.x * 10.0f) / 10.0f, roundf(origin.y * 10.0f) / 10.0f, roundf(origin.z * 10.0f) / 10.0f};
    projectile->config = *config;
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
    };
    game_send(game, NET_TARGET_ALL, NET_PACKET_PROJECTILE, &packet, sizeof(packet));
}

// game.explosion(x, y, z, radius, dmg, owner): 1 - distance / radius of the damage,
// a fifth for the owner (who may hurt themselves).
void game_explosion(Game *game, const vec3 position, const float radius, const float damage, Player *owner, const int weapon_index) {
    if (!game_is_authority(game) || radius <= 0.0f) return;

    const NetExplosionPacket packet = {.position = {roundf(position.x), roundf(position.y), roundf(position.z)}, .radius = roundf(radius)};
    game_send(game, NET_TARGET_ALL, NET_PACKET_EXPLOSION, &packet, sizeof(packet));

    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player || !player->active) continue;

        const float dx = player->position.x - position.x;
        const float dy = player->position.y - position.y;
        const float dz = player->position.z - position.z;
        const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
        const float falloff = 1.0f - distance / radius;
        if (falloff <= 0.0f) continue;

        const float dealt = damage * falloff * (player == owner ? 0.2f : 1.0f);
        if (player_change_health(player, owner, dealt, 0)) {
            PlayerKillInfo info = {.weapon_id = weapon_index, .distance = distance};
            player_kill(player, owner, &info, 0);
        }
    }
}

typedef struct {
    Player *player;
    float t;
} ProjectileHit;

static int projectile_object_blocks(const Object *object) {
    // Same filter as the hitscan (player.c combat_object_shootable): Krunker noShoot zones/helpers.
    if (!object->active || object->no_shoot || object->collision_type == COLLISION_TYPE_NONE) return 0;
    return !(object->kill || object->score_zone || object->team_zone || object->teleporter || object->checkpoint ||
             object->pickup || object->flag || object->premium || object->verified || object->trigger ||
             object->ladder || object->ramp || object->prefab == PREFAB_TEAM_ZONE || object->prefab == PREFAB_SPAWN_POINT ||
             object->prefab == PREFAB_CAMERA_POSITION || object->prefab == PREFAB_SPECTATE_CAM);
}

// Nearest hit along this tick's step (lineInRect against step = velocity * dt).
static ProjectileHit projectile_cast(Game *game, const GameProjectile *projectile, const float dt_ms, int *hit_any) {
    const vec3 step = {projectile->velocity.x * dt_ms, projectile->velocity.y * dt_ms, projectile->velocity.z * dt_ms};
    ProjectileHit best = {NULL, 2.0f};
    *hit_any = 0;

    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *object = game->map->objects[i];
        if (!projectile_object_blocks(object)) continue;

        const float t = line_in_rect(projectile->position, step, object->position, object->scale);
        if (t > 0.0f && t <= 1.0f && t < best.t) {
            best = (ProjectileHit) {NULL, t};
            *hit_any = 1;
        }
    }

    const float step_length = projectile->speed * dt_ms;

    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player || !player->active || player->uid == projectile->owner_uid) continue;

        float t;
        if (projectile->config.hit_box > 0.0f) {
            const float dx = projectile->position.x - player->position.x;
            const float dy = projectile->position.y - (player->position.y + player->height * 0.5f);
            const float dz = projectile->position.z - player->position.z;
            const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
            t = distance <= projectile->config.hit_box ? distance / projectile->config.hit_box : -1.0f;
        } else {
            const float pad = 1.0f; // config.hitBoxPad
            const float half = player->scale + pad;
            const vec3 box = {half * 2.0f, player->height + pad, half * 2.0f};
            t = step_length > 0.0f ? line_in_rect(projectile->position, step, player->position, box) : -1.0f;
        }

        if (t > 0.0f && t <= 1.0f && t < best.t) {
            best = (ProjectileHit) {player, t};
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
            hit_object = !hit.player;

            if (hit.player && projectile->config.damage > 0.0f && player_change_health(hit.player, owner, projectile->config.damage, 0)) {
                PlayerKillInfo info = {.weapon_id = projectile->weapon_index, .distance = 0.0f};
                info.thrown = projectile->thrown_melee;
                player_kill(hit.player, owner, &info, 0);
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
