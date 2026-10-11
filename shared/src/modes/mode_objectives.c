// Map objectives of the modes (Krunker v3.1.1 src/data/objects.js addObjective /
// addFlag / addScoreZone, 106.js flag + spawnable logic, players.js playerCollisions):
// Hardpoint / KOTH zones, CTF flags, Kill Confirmed tags and touch conversions
// (Infected, Hide & Seek). Authority only.
#include "mode_internal.h"
#include <math.h>
#include <string.h>

int player_collides(Player*, const Object*, float); // shared/src/player.c

#define PREFAB_KEY_SCORE_ZONE "SCORE_ZONE"
#define PREFAB_KEY_OBJECTIVE "OBJECTIVE"
#define PREFAB_KEY_FLAG "FLAG"

static int raw_prefab(const cJSON *object) {
    const cJSON *i = cJSON_GetObjectItem(object, "i");
    if (!i) i = cJSON_GetObjectItem(object, "id");

    if (cJSON_IsNumber(i)) return (int) cJSON_GetNumberValue(i);
    if (!cJSON_IsString(i)) return 0;

    const char *name = cJSON_GetStringValue(i);
    if (!strcmp(name, PREFAB_KEY_OBJECTIVE)) return PREFAB_OBJECTIVE;
    if (!strcmp(name, PREFAB_KEY_FLAG)) return PREFAB_FLAG;
    if (!strcmp(name, PREFAB_KEY_SCORE_ZONE)) return PREFAB_SCORE_ZONE;
    return -1;
}

static float raw_number(const cJSON *array, const int index) {
    const cJSON *item = cJSON_GetArrayItem(array, index);
    if (cJSON_IsNumber(item)) return (float) cJSON_GetNumberValue(item);
    if (cJSON_IsString(item)) return (float) atof(cJSON_GetStringValue(item));
    return 0.0f;
}

// parseInt(OBJECT.key || 0) || 0 (numbers and numeric strings alike)
static int raw_int(const cJSON *object, const char *key, const int fallback) {
    const cJSON *item = cJSON_GetObjectItem(object, key);
    if (cJSON_IsNumber(item)) return (int) cJSON_GetNumberValue(item);
    if (cJSON_IsString(item)) return atoi(cJSON_GetStringValue(item));
    return fallback;
}

void mode_load_objects(Game *game) {
    GameMode *mode = game->mode;
    mode->objective_count = 0;
    mode->flag_count = 0;
    mode->target_score = 0;

    const cJSON *objects = game->map ? cJSON_GetObjectItem(game->map->raw_data, "objects") : NULL;
    if (!cJSON_IsArray(objects)) return;

    const cJSON *object;
    cJSON_ArrayForEach(object, objects) {
        const cJSON *p = cJSON_GetObjectItem(object, "p");
        const cJSON *s = cJSON_GetObjectItem(object, "s");
        if (!cJSON_IsArray(p) || cJSON_GetArraySize(p) < 3 || cJSON_GetArraySize(s) < 3) continue; // map_init skips these too

        const vec3 position = {raw_number(p, 0), raw_number(p, 1), raw_number(p, 2)};
        const vec3 scale = {raw_number(s, 0), raw_number(s, 1), raw_number(s, 2)};

        switch (raw_prefab(object)) {
            case PREFAB_OBJECTIVE:
                // addObjective(x, y, z, sx, sz, sy, obi): y + h / 2 centre, half extents
                if (mode->objective_count < MODE_MAX_OBJECTIVES) {
                    mode->objectives[mode->objective_count++] = (ModeObjective) {
                        .position = {position.x, position.y + scale.y * 0.5f, position.z},
                        .half = {scale.x * 0.5f, scale.y * 0.5f, scale.z * 0.5f},
                        .link = raw_int(object, "obi", 0),
                    };
                }
                break;
            case PREFAB_FLAG:
                // addFlag(x, y, z, tm + 1): the crystal floats flagScale / 2 + flagOff above
                if (mode->flag_count < MODE_MAX_FLAGS) {
                    ModeFlag *flag = &mode->flags[mode->flag_count++];
                    flag->origin = (vec3) {position.x, position.y + MODE_FLAG_SCALE * 0.5f + MODE_FLAG_OFF, position.z};
                    flag->position = flag->origin;
                    flag->ping = flag->origin;
                    flag->team = raw_int(object, "tm", 0) + 1;
                    flag->carrier_uid = -1;
                    flag->ping_timer = 0.0f;
                }
                break;
            case PREFAB_SCORE_ZONE: {
                const int points = raw_int(object, "sp", 0);
                mode->target_score += points ? points : 1;
                break;
            }
            default:
                break;
        }
    }
}

// Player.collides(other) for a box: x/z overlap with the player's scale, y from the feet
// to the top of the player against the centre +- half height.
int mode_player_collides(const Player *player, const vec3 center, const vec3 half) {
    return player->position.x - player->scale < center.x + half.x &&
           player->position.x + player->scale > center.x - half.x &&
           player->position.z - player->scale < center.z + half.z &&
           player->position.z + player->scale > center.z - half.z &&
           player->position.y <= center.y + half.y &&
           player->position.y + player->height >= center.y - half.y;
}

int mode_in_kill_zone(const Game *game, const Player *player) {
    if (!game->map) return 0;
    if (player->position.y <= game->map->death_y) return 1;

    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *object = game->map->objects[i];
        if (object->kill && object->active && player_collides((Player *) player, object, 0.0f)) return 1;
    }

    return 0;
}

// ------------------------------------------------------------------ Hardpoint / KOTH

// game.update: every 1.5 s while the objective timer runs (waitTimers[0].contTime),
// everyone standing in the active objective scores objScore (10 by default).
void mode_tick_objectives(Game *game, const float delta) {
    GameMode *mode = game->mode;
    if (!mode->config.objective || !mode->objective_count) return;
    if (!mode->wait_timer_count || !mode->wait_timers[0].cont_time) return;

    mode->objective_timer -= delta;
    if (mode->objective_timer > 0.0f) return;
    mode->objective_timer = MODE_OBJECTIVE_TICK;

    if (mode->active_objective < 0 || mode->active_objective >= mode->objective_count) return;
    const ModeObjective *objective = &mode->objectives[mode->active_objective];
    const int points = mode->config.obj_score ? mode->config.obj_score : 10;

    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (player && player->active && mode_player_collides(player, objective->position, objective->half)) {
            player_score(player, points, 0);
        }
    }
}

// ------------------------------------------------------------------ CTF

static void flag_set_info(Game *game, const int team, const char *text) {
    if (team < 0 || team >= MODE_TEAMS) return;
    snprintf(game->mode->flag_info[team], MODE_TEXT_SIZE, "%s", text);
}

static Player *mode_find_player(const Game *game, const int uid) {
    for (size_t i = 0; i < game->player_count; i++) {
        if (game->players[i] && game->players[i]->uid == uid) return game->players[i];
    }
    return NULL;
}

static int flag_at_base(const ModeFlag *flag) {
    return flag->position.x == flag->origin.x && flag->position.y == flag->origin.y && flag->position.z == flag->origin.z;
}

// updateObjPos + updateCarrier(flag) + "ufl": the flag lies at `position`, nobody carries it.
static void flag_place(Game *game, const int index, const vec3 position) {
    ModeFlag *flag = &game->mode->flags[index];
    Player *carrier = flag->carrier_uid >= 0 ? mode_find_player(game, flag->carrier_uid) : NULL;
    if (carrier && carrier->mode.flag == index + 1) carrier->mode.flag = 0;

    flag->position = position;
    flag->ping = position;
    flag->carrier_uid = -1;
}

// resetFlag: back at its base, "AT BASE".
void mode_reset_flag(Game *game, const int index) {
    if (index < 0 || index >= game->mode->flag_count) return;
    ModeFlag *flag = &game->mode->flags[index];

    flag_place(game, index, flag->origin);
    flag_set_info(game, flag->team, "AT BASE");
    mode_send_flag(game, index);
}

// dropFlag: where the carrier was (y + flagOff), "AWAY".
void mode_drop_flag(Game *game, Player *carrier) {
    if (!carrier || !carrier->mode.flag) return;
    const int index = carrier->mode.flag - 1;
    if (index < 0 || index >= game->mode->flag_count) return;

    const ModeFlag *flag = &game->mode->flags[index];
    mode_send_action(game, carrier, NULL, "dropped the Flag");
    flag_place(game, index, (vec3) {carrier->position.x, carrier->position.y + MODE_FLAG_OFF, carrier->position.z});
    flag_set_info(game, flag->team, "AWAY");
    mode_send_flag(game, index);
}

// capFlag: +1 team score, +150, the enemy flag goes home.
static void flag_capture(Game *game, Player *player) {
    const int index = player->mode.flag - 1;
    player->mode.caps++;
    player->mode.flag = 0;
    mode_team_score_add(game, player->team, 1);
    mode_send_action(game, player, NULL, "captured the Flag");
    mode_send_medal(game, player, "Capture", 150);
    player_score(player, 150, 0);
    mode_reset_flag(game, index);
}

// pickupFlag: your own flag away from base returns it (+50); the enemy flag is taken (+20).
static void flag_touch(Game *game, Player *player, const int index) {
    ModeFlag *flag = &game->mode->flags[index];
    if (flag->carrier_uid >= 0) return;

    if (player->team == flag->team) {
        if (flag_at_base(flag)) return;
        mode_send_action(game, player, NULL, "returned the Flag");
        mode_send_medal(game, player, "Return", 50);
        player_score(player, 50, 0);
        mode_reset_flag(game, index);
        return;
    }

    if (player->mode.flag) return;

    mode_send_medal(game, player, "Pickup", 20);
    player_score(player, 20, 0);
    flag->carrier_uid = player->uid;
    flag->ping_timer = MODE_FLAG_UPDATE;
    flag->ping = (vec3) {player->position.x, player->position.y + MODE_FLAG_HOFF, player->position.z};
    player->mode.flag = index + 1;
    flag_set_info(game, flag->team, player->name);
    mode_send_flag(game, index);
    mode_send_action(game, player, NULL, "got the Flag");
}

void mode_tick_flags(Game *game, const float delta) {
    GameMode *mode = game->mode;
    if (!mode->config.flags || !mode->flag_count) return;

    const vec3 flag_half = {MODE_FLAG_SCALE, MODE_FLAG_SCALE * 0.5f, MODE_FLAG_SCALE};
    const vec3 zone_half = {MODE_FLAG_ZONE_SIZE * 0.5f, MODE_FLAG_ZONE_HEIGHT * 0.5f, MODE_FLAG_ZONE_SIZE * 0.5f};

    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player || !player->active) continue;

        for (int f = 0; f < mode->flag_count; f++) {
            const ModeFlag *flag = &mode->flags[f];

            // the flag's trigger zone: carrying the enemy flag into your own zone while
            // your flag is home captures
            const vec3 zone_center = {flag->origin.x, flag->origin.y - MODE_FLAG_SCALE * 0.5f - MODE_FLAG_OFF + MODE_FLAG_ZONE_HEIGHT * 0.5f, flag->origin.z};
            if (player->mode.flag && flag->team == player->team && flag->carrier_uid < 0 && flag_at_base(flag) &&
                mode_player_collides(player, zone_center, zone_half)) {
                flag_capture(game, player);
                continue;
            }

            if (mode_player_collides(player, flag->position, flag_half)) flag_touch(game, player, f);
        }
    }

    // updateFlagPings: the carrier's position every flagUpdate ("ufp")
    for (int f = 0; f < mode->flag_count; f++) {
        ModeFlag *flag = &mode->flags[f];
        if (flag->carrier_uid < 0) {
            flag->ping = flag->position;
            continue;
        }

        flag->ping_timer -= delta;
        if (flag->ping_timer > 0.0f) continue;

        const Player *carrier = mode_find_player(game, flag->carrier_uid);
        if (!carrier) continue;

        flag->ping = (vec3) {carrier->position.x, carrier->position.y + MODE_FLAG_HOFF, carrier->position.z};
        flag->ping_timer = MODE_FLAG_UPDATE;
        mode_send_flag(game, f);
    }
}

// ------------------------------------------------------------------ Kill Confirmed tags

// dropSpawnable(0, victim): a tag of the victim's team at (x, y + yOff, z) for 8 s.
void mode_drop_spawnable(Game *game, const Player *victim) {
    if (!victim) return;
    GameMode *mode = game->mode;

    int slot = -1;
    for (int i = 0; i < MODE_MAX_SPAWNABLES; i++) {
        if (!mode->spawnables[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) { // full: the oldest goes
        slot = 0;
        for (int i = 1; i < MODE_MAX_SPAWNABLES; i++) {
            if (mode->spawnables[i].timer < mode->spawnables[slot].timer) slot = i;
        }
    }

    mode->spawnables[slot] = (ModeSpawnable) {
        .sid = mode->next_spawnable_sid++,
        .position = {victim->position.x, victim->position.y + MODE_TAG_YOFF, victim->position.z},
        .team = victim->team,
        .timer = MODE_TAG_TIMER,
        .active = 1,
    };

    mode_send_spawnable(game, slot);
}

// spawnables[0].onPickup: your team's tag denies (+10), the enemy's confirms (+1 team, +25).
static void spawnable_pickup(Game *game, Player *player, const int slot) {
    ModeSpawnable *tag = &game->mode->spawnables[slot];
    tag->active = 0;

    if (player->team == tag->team) {
        player->mode.denies++;
        mode_send_medal(game, player, "Denied", 10);
        player_score(player, 10, 0);
    } else {
        mode_team_score_add(game, player->team, 1);
        player->mode.confirms++;
        mode_send_medal(game, player, "Confirmed", 25);
        player_score(player, 25, 0);
    }

    mode_send_spawnable(game, slot);
}

void mode_tick_spawnables(Game *game, const float delta) {
    GameMode *mode = game->mode;
    const vec3 half = {MODE_TAG_SCALE, MODE_TAG_SCALE * 0.5f, MODE_TAG_SCALE};

    for (int s = 0; s < MODE_MAX_SPAWNABLES; s++) {
        ModeSpawnable *tag = &mode->spawnables[s];
        if (!tag->active) continue;

        for (size_t i = 0; i < game->player_count && tag->active; i++) {
            Player *player = game->players[i];
            if (player && player->active && mode_player_collides(player, tag->position, half)) spawnable_pickup(game, player, s);
        }

        if (!tag->active) continue;

        tag->timer -= delta;
        if (tag->timer <= 0.0f) {
            tag->active = 0;
            mode_send_spawnable(game, s);
        }
    }
}

// ------------------------------------------------------------------ conversions

// players.playerCollisions: a convTeam player touching someone of the other team (head
// spheres within 2.4 * half the player height) converts them: +100, convs, "tm",
// killConv kills them, and the round clock gains 10 s.
void mode_tick_conversions(Game *game) {
    GameMode *mode = game->mode;
    if (!mode->config.conv_team || mode->wait_timer_count) return;

    const float radius = game_constants.player_height * 0.5f;

    for (size_t i = 0; i < game->player_count; i++) {
        Player *converter = game->players[i];
        if (!converter || !converter->active || converter->team != mode->config.conv_team) continue;

        for (size_t j = 0; j < game->player_count; j++) {
            Player *other = game->players[j];
            if (!other || !other->active || other->team == converter->team || other->god_mode) continue;

            const float dx = converter->position.x - other->position.x;
            const float dy = converter->position.y - other->position.y;
            const float dz = converter->position.z - other->position.z;
            if (sqrtf(dx * dx + dy * dy + dz * dz) > radius * 2.4f) continue;

            mode_send_action(game, converter, other, mode->config.conv_word ? mode->config.conv_word : "");
            player_score(converter, 100, 0);
            converter->mode.convs++;
            mode_update_team(game, other, converter->team);
            if (mode->config.kill_on_convert) player_kill(other, NULL, NULL, 1);
            if (game->round_timer > 0.0f) game->round_timer += MODE_CONVERT_TIME_BONUS;
        }
    }
}
