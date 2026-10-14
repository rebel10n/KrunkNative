// map-logic WP-B (doc/v7/map-logic.md §2.1-§2.3, §7 WP-B): the v7.1.4 world runtime of
// shared/include/world.h: check points (game.setCheckPoint), teleporters (checkTeleport with
// telCheckPoint), players.changePosition, the Parkour resets (resetToStart / resetToLast), score
// zones with players.logTime, the account-gated zones, NoPVP / PVP zones, the tris pass, the
// terrain ground snap, collidingObjs (onEnterObject / onExitObject), node moves (teleportToNode /
// teleportFromNode, "mo" / "mf"), object animations ("ana") and the world part of the join sync.
// Runs on the authority (server / offline sandbox server) and, for prediction, on the client.
#include <world.h>
#include <object_manager.h>
#include <account.h>
#include <triggers_v7.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef KRUNKNATIVE_CLIENT
#include <client.h>
#endif

#define WORLD_RESET_TIMER 250.0f // resetTimer (ms) after a Parkour reset

void player_reset_step(Player*, int recon); // shared/src/player.c (manager.resetStep)

// config.clipNames: the clip index "ana" sends for a non-raw animation
static const char *const k_clip_names[] = {"Idle", "Move", "MoveFast", "Jump", "Melee", "Shoot", "Crouch"};

static int world_object_index(const Game *game, const Object *object) {
    for (size_t i = 0; game->map && i < game->map->object_count; i++) {
        if (game->map->objects[i] == object) return (int) i;
    }
    return -1;
}

static Object *world_object_at(const Game *game, const int index) {
    return game->map && index >= 0 && (size_t) index < game->map->object_count ? game->map->objects[index] : NULL;
}

// player.team: undefined while forcePVP (the player.js getter)
static int world_player_team(const Player *player) { return player && !player->force_pvp ? player->team : 0; }

// game.teamCheck(obj, player): !obj.team || !mode || !mode.teams || (player && obj.team == player.team)
static int world_team_check(const Game *game, const int team, const Player *player) {
    return !team || !game->mode || !game->mode->config.teams || (player && team == world_player_team(player));
}

// The v7 modes that log speed-run times. logTime / ownTime / allowReset are mode keys (bhop.js:
// logTime, ownTime, allowReset; race.js: logTime); the mode table has no fields for them yet, so
// they are read from the mode id (Parkour 4 and Race 7 keep their v3.1.1 ids in v7).
static int world_mode_log_time(const Game *game) {
    const int id = game_mode_id(game);
    return id == MODE_ID_BHOP || id == MODE_ID_RACE;
}
static int world_mode_own_time(const Game *game) { return game_mode_id(game) == MODE_ID_BHOP; }
static int world_mode_allow_reset(const Game *game) { return game_mode_id(game) == MODE_ID_BHOP; }

// game.doLogTime = mode.logTime && config.logTim (logTim default true)
static int world_do_log_time(const Game *game) {
#ifdef GAME_CONFIG_HAS_LOG_TIME
    if (!game->config.log_time) return 0;
#endif
    return world_mode_log_time(game);
}

static int world_last_checkpoint_time(const Game *game) { // config.lstChkT (default false)
#ifdef GAME_CONFIG_HAS_LOG_TIME
    return game->config.last_checkpoint_time;
#else
    (void) game;
    return 0;
#endif
}

// game.canOwnTime = !gameTimer: the round has no clock
static int world_can_own_time(const Game *game) {
    return game->mode && game->mode->config.game_time == 0.0f;
}

static void world_send_log_time(Game *game, const Player *player, const float time_ms, const int kind) {
    const NetLogTimePacket packet = {
        .time_ms = (int32_t) time_ms,
        .time_log = player->time_log,
        .kind = (uint8_t) kind,
        .show_ms = (uint8_t) (game->mode && game->mode->config.show_ms),
    };
    game_send(game, player->uid, NET_PACKET_LOG_TIME, &packet, sizeof(packet));
}

static void world_send_checkpoint(Game *game, const Player *player, const int reset) {
    const NetCheckpointPacket packet = {.reset = (uint8_t) !!reset};
    game_send(game, player->uid, NET_PACKET_CHECKPOINT, &packet, sizeof(packet)); // "chp" / "chp" true
}

// ---------------------------------------------------------------- positions (players.changePosition)

void game_change_position(Game *game, Player *player, const vec3 feet, const int recon, const int stop_momentum,
                          const int has_direction, const int direction) {
    player->position = feet;      // x / y / z and oldX / oldY / oldZ
    player->last_position = feet;
    if (has_direction) player->direction.y = -((float) direction + 1.0f) * (float) M_PI / 2.0f;
    if (stop_momentum) {
        player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
        player->step_val = 0.0f;
    }
    player_end_grapple(player); // this.endGrapple(player), unconditionally (v10 :173795)
    if (recon && game_is_authority(game)) { // doRecon: the player's own client snaps to it
        player->recon_snap = 1; // and the next tick's "l" carries the full state (net_send_reconciliation)
        const NetPositionPacket packet = {
            .x = feet.x, .y = feet.y, .z = feet.z,
            .stop_momentum = (uint8_t) !!stop_momentum,
            .has_direction = (uint8_t) !!has_direction,
            .direction = direction,
        };
        game_send(game, player->uid, NET_PACKET_POSITION, &packet, sizeof(packet));
    }
}

// ---------------------------------------------------------------- check points

// game.setCheckPoint: a new check point (teamCheck, singleUse once per checkPointList) stores
// checkPointData = [timeLog, gameTime]; the authority sends "chp".
void game_set_checkpoint(Game *game, Player *player, Object *object) {
    const int uid = world_object_index(game, object);
    if (uid < 0 || player->checkpoint == uid + 1) return; // !checkPoint || checkPoint != obj.uid
    if (!world_team_check(game, (int) object->team, player)) return;
    if (object->single_use) {
        for (int i = 0; i < player->checkpoint_list_count; i++) {
            if (player->checkpoint_list[i] == uid) return;
        }
        if (player->checkpoint_list_count < PLAYER_CHECKPOINT_LIST_MAX) player->checkpoint_list[player->checkpoint_list_count++] = uid;
    }
    player->checkpoint = uid + 1;
    player->checkpoint_time_log = player->time_log;
    player->checkpoint_game_time = player->game_time;
    player->has_checkpoint_data = 1;
    if (game_is_authority(game)) world_send_checkpoint(game, player, 0);
}

void world_clear_checkpoints(Game *game, Player *player) {
    if (!player) return;
    player->checkpoint = 0;
    player->checkpoint_list_count = 0;
    player->has_checkpoint_data = 0;
    player->checkpoint_time_log = 0;
    player->checkpoint_game_time = 0.0f;
    if (game && game_is_authority(game)) world_send_checkpoint(game, player, 1);
}

// The respawn of a player that reached a check point: its bottom centre, facing its direction.
int game_spawn_at_checkpoint(Game *game, Player *player) {
    if (!game->map || player->checkpoint <= 0 || (size_t) player->checkpoint > game->map->object_count) return 0;
    const Object *object = game->map->objects[player->checkpoint - 1];
    if (!object->checkpoint) return 0;
    player->position = object->position;
    player->last_position = object->position;
    player->direction.x = 0.0f;
    player->direction.y = -(float) M_PI / 2.0f * (1.0f + (float) object->direction);
    return 1;
}

// ---------------------------------------------------------------- teleporters

// game.checkTeleport (client prediction and authority): a sender (node != 1) without cooldown
// moves the player to the first receiver of its channel (node > 0, teamCheck); a node-2 receiver
// gets 2000 ms of cooldown. v7 telCheckPoint ("tcp"): to the player's check point instead,
// y - height + 0.01 (its bottom + 0.01), forceDir taking the check point's direction.
void game_check_teleport(Game *game, Player *player, Object *object) {
    if (object->node == 1 || object->tel_cooldown > 0.0f || !world_team_check(game, (int) object->team, player)) return;
    if (object->tel_checkpoint) {
        const Object *point = player->checkpoint > 0 ? world_object_at(game, player->checkpoint - 1) : NULL;
        if (point) {
            const vec3 feet = {point->position.x, point->position.y + 0.01f, point->position.z};
            game_change_position(game, player, feet, 0, object->stop_momentum, object->force_dir, point->direction);
        }
        return;
    }
    for (size_t i = 0; i < game->map->object_count; i++) {
        Object *to = game->map->objects[i];
        if (!to->teleporter || to->channel != object->channel || to->node <= 0 || to == object) continue;
        if (!world_team_check(game, (int) to->team, player)) continue;
        if (to->node == 2) to->tel_cooldown = 2000.0f;
        game_change_position(game, player, to->position, 0, to->stop_momentum, to->force_dir, to->direction);
        break;
    }
}

// ---------------------------------------------------------------- score zones / logTime

static void world_send_finish_chat(Game *game, const Player *player, const float time_ms) {
    // "ch" 0, null, "<name> finished in <time>", 2 (UTILS.getTime(t, true, t >= 1h, 1000, t >= 1d, 2))
    const int total = (int) time_ms;
    const int hours = total / 3600000, minutes = total / 60000 % 60, seconds = total / 1000 % 60, ms = total % 1000;
    char time[32];
    if (hours) snprintf(time, sizeof(time), "%02d:%02d:%02d.%02d", hours, minutes, seconds, ms / 10);
    else snprintf(time, sizeof(time), "%02d:%02d.%02d", minutes, seconds, ms / 10);
    NetChatPacket packet = {.uid = -1, .kind = NET_CHAT_SERVER};
    snprintf(packet.text, sizeof(packet.text), "%s finished in %s", player->name, time);
    game_send(game, NET_TARGET_ALL, NET_PACKET_CHAT, &packet, sizeof(packet));
}

static size_t world_score_zone_count(const Game *game) {
    size_t count = 0;
    for (size_t i = 0; game->map && i < game->map->object_count; i++) count += game->map->objects[i]->score_zone;
    return count;
}

void world_log_time(Game *game, Player *player) {
    if (!game_is_authority(game) || !world_do_log_time(game)) return;
    player->time_log++;
    // the round clock (game.gameTimer, ms), or the player's own clock in ownTime modes
    float time_ms = game->round_timer * 1000.0f;
    if (game->mode && game->mode->config.timed) time_ms = game->mode->round_clock * 1000.0f;
    if (world_mode_own_time(game) && world_can_own_time(game)) time_ms = player->game_time;
    world_send_log_time(game, player, time_ms, NET_LOG_TIME_ZONE); // "gt" time, timeLog
    if ((size_t) player->time_log == world_score_zone_count(game)) {
        world_send_finish_chat(game, player, time_ms);
        world_send_log_time(game, player, time_ms, NET_LOG_TIME_FINISH); // "t" the final time
    }
}

// player.js:1453-1458: scoreObjs[sid] once (per round; resetToStart clears it): score(scoreP), logTime
void world_player_score_zone(Player *player, const Object *object) {
    Game *game = player->game;
    const int index = world_object_index(game, object);
    if (index < 0) return;
    for (int i = 0; i < player->score_obj_count; i++) {
        if (player->score_objs[i] == index) return;
    }
    if (player->score_obj_count < PLAYER_SCORE_OBJS_MAX) player->score_objs[player->score_obj_count++] = index;
    player_score(player, (int) object->score_points, 0); // manager.score (authority only)
    world_log_time(game, player);
}

// players.tickPlayer: the ownTime clock runs once the player moved (coverDis >= 1) until the last
// score zone; "t" whenever the shown value (tenths) changes.
static void world_tick_own_time(Game *game, Player *player, const float delta_ms) {
    if (!world_do_log_time(game) || !world_mode_own_time(game) || player->covered_distance < 1.0f || !world_can_own_time(game)) return;
    player->game_time += delta_ms;
    if ((size_t) player->time_log >= world_score_zone_count(game)) return;
    const int tenths = (int) (player->game_time / 100.0f);
    if (tenths == player->last_timer_sent) return;
    player->last_timer_sent = tenths;
    world_send_log_time(game, player, player->game_time, NET_LOG_TIME_CLOCK);
}

// ---------------------------------------------------------------- Parkour resets (input [13])

static void world_reset_to_start(Player *player, const int recon) {
    Game *game = player->game;
    if (!world_mode_allow_reset(game) || !player->has_init_pos) return;
    (void) recon;
    player->time_log = 0;
    player->game_time = 0.0f;
    player->last_timer_sent = -1;
    player->reset_timer = WORLD_RESET_TIMER;
    player->position = player->init_pos;
    player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    player->covered_distance = 0.0f;
    player->score_obj_count = 0;
    if (game_is_authority(game)) {
        world_send_log_time(game, player, 0.0f, NET_LOG_TIME_CLOCK); // "t" "00:00.0"
        if (player->score) { // score(player, 0, 0, true): the score becomes 0
            player->score = 0;
            const NetScorePacket packet = {.delta = 0, .score = 0};
            game_send(game, player->uid, NET_PACKET_SCORE, &packet, sizeof(packet));
            game->leaders_dirty = 1;
        }
        world_clear_checkpoints(game, player); // checkPoint = null, list cleared, "chp" true
    }
}

static void world_reset_to_last(Player *player) {
    Game *game = player->game;
    if (!world_mode_allow_reset(game)) return;
    const Object *point = player->checkpoint > 0 ? world_object_at(game, player->checkpoint - 1) : NULL;
    if (!point) return;
    player->reset_timer = WORLD_RESET_TIMER;
    player->position = (vec3) {point->position.x, point->position.y + 0.01f, point->position.z}; // y - height + 0.01
    player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    if (point->force_dir) player->direction.y = -((float) point->direction + 1.0f) * (float) M_PI / 2.0f;
    if (world_last_checkpoint_time(game)) {
        player->time_log = player->has_checkpoint_data ? player->checkpoint_time_log : 0;
        player->game_time = player->has_checkpoint_data ? player->checkpoint_game_time : 0.0f;
        player->last_timer_sent = (int) (player->game_time / 100.0f);
        if (game_is_authority(game)) world_send_log_time(game, player, player->game_time, NET_LOG_TIME_CLOCK);
    }
}

void world_player_reset_input(Player *player, const int reset, const int recon, const float delta_ms) {
    // player.js:1830: resetTimer counts down in the non-recon part of procInputs
    if (!recon && player->reset_timer > 0.0f) {
        player->reset_timer -= delta_ms;
        if (player->reset_timer <= 0.0f) player->reset_timer = 0.0f;
    }
    // v9782 != 2 || !active || resetTimer || recon ? (v9782 && active && !resetTimer && !recon -> resetToStart) : resetToLast
    if (!reset || !player->active || player->reset_timer > 0.0f || recon) return;
    if (reset == 2) world_reset_to_last(player);
    else world_reset_to_start(player, recon);
}

// ---------------------------------------------------------------- zones

int world_zone_passable(const Player *player, const Object *object) {
    if (!player || !object) return 0;
    const Game *game = player->game;
    if (object->team_zone) return world_team_check(game, (int) object->team, player);
    if (!object->premium && !object->verified && !object->partner_zone && !object->kpd_zone && !object->kr_zone) return 0;
    AccountInfo account;
    game_account_info(game, player, &account); // NULL hook: a guest (everything false)
    if (object->premium) return account.premium;
    if (object->verified) return account.verified;
    if (object->partner_zone) return account.partner;
    if (object->kpd_zone) return account.can_flag;
    return account.kr >= (long long) object->kr_amount; // manager.enoughKR(player, amount)
}

// collidesE(obj, scale + hbS, collPad): the padded box test of the collision loop
static int world_collides_padded(const Player *player, const Object *object) {
    const float pad = game_constants.collision_padding;
    const vec3 size = object_box_size(object);
    const float w = size.x * 0.5f + pad + player->scale, l = size.z * 0.5f + pad + player->scale;
    const float top = object->position.y + size.y;
    return player->position.x > object->position.x - w && player->position.x < object->position.x + w &&
           player->position.z > object->position.z - l && player->position.z < object->position.z + l &&
           player->position.y + player->height >= object->position.y && player->position.y <= top;
}

void world_player_pvp_zones(Player *player) {
    player->no_pvp = 0;
    player->force_pvp = 0;
    const Game *game = player->game;
    if (player->noclip || !game->map) return;
    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *object = game->map->objects[i];
        if (!(object->no_pvp || object->force_pvp) || !object->active || !world_collides_padded(player, object)) continue;
        if (object->no_pvp) player->no_pvp = 1;
        else player->force_pvp = 1;
    }
}

int world_pvp_allows(const Player *attacker, const Player *victim) {
    if (!victim || victim->no_pvp || (attacker && attacker->no_pvp)) return 0;
    if (victim->force_pvp) return attacker && attacker->force_pvp;
    return !attacker || !attacker->force_pvp;
}

int world_pvp_ignores_team(const Player *attacker, const Player *victim) {
    return attacker && victim && attacker->force_pvp && victim->force_pvp;
}

// ---------------------------------------------------------------- tris / terrain

void world_player_tris(Player *player) {
    const Game *game = player->game;
    const Map *map = game->map;
    if (player->noclip || !map || !map->tris || !map->tri_count) return;
    for (size_t i = 0; i < map->tri_count; i++) {
        vec3 normal;
        float depth;
        if (!tri_capsule_collide(&map->tris[i * 9], player->position, player->scale, player->height, &normal, &depth)) continue;
        if (normal.y > 0.7f) player->on_ground = 1; // manager.setOnGround
        const float along = normal.x * player->velocity.x + normal.y * player->velocity.y + normal.z * player->velocity.z;
        player->velocity.x -= normal.x * along; // velocity.addScaledVector(normal, -normal.dot(velocity))
        player->velocity.y -= normal.y * along;
        player->velocity.z -= normal.z * along;
        player->position.x += normal.x * depth;
        player->position.y += normal.y * depth;
        player->position.z += normal.z * depth;
    }
}

int terrain_raycast(const Game *game, const vec3 origin, const vec3 direction, const float max_distance, float *distance) {
    const Terrain *terrain = game ? map_terrain(game->map) : NULL;
    if (!terrain) return 0;
    const double len = sqrt((double) direction.x * direction.x + (double) direction.y * direction.y + (double) direction.z * direction.z);
    if (len <= 0.0 || max_distance <= 0.0f) return 0;
    const double k = max_distance / len;
    double hit[3];
    // raycast(x, -z, y, dx, -dz, dy): terrain space; the hit is (x, -z, height)
    if (!terrain_raycast_local(terrain, origin.x, -origin.z, origin.y, direction.x * k, -direction.z * k, direction.y * k, hit, NULL)) return 0;
    const double dx = hit[0] - origin.x, dy = hit[2] - origin.y, dz = -hit[1] - origin.z;
    if (distance) *distance = (float) sqrt(dx * dx + dy * dy + dz * dz);
    return 1;
}

float world_terrain_fraction(const Game *game, const vec3 origin, const vec3 segment) {
    const float length = sqrtf(segment.x * segment.x + segment.y * segment.y + segment.z * segment.z);
    float distance;
    if (length <= 0.0f || !terrain_raycast(game, origin, segment, length, &distance)) return -1.0f;
    return distance / length;
}

int world_terrain_height(const Game *game, const float x, const float z, float *height) {
    const Terrain *terrain = game ? map_terrain(game->map) : NULL;
    double hit[3];
    if (!terrain || !terrain_raycast_local(terrain, x, -z, 10000.0, 0.0, 0.0, -20000.0, hit, NULL)) return 0;
    if (height) *height = (float) hit[2];
    return 1;
}

// player.js:1779-1797: below (or on) the terrain snaps to it, onTerrain, onGround, no fall.
int world_player_terrain(Player *player, const int recon) {
    float height;
    if (player->noclip || !world_terrain_height(player->game, player->position.x, player->position.z, &height)) return 0;
    if (player->position.y <= height) {
        player_reset_step(player, recon);
        player->on_terrain = 1;
    }
    if (!player->on_terrain) return 0;
    player->on_ground = 1; // manager.setOnGround
    player->position.y = height;
    player->velocity.y = 0.0f;
    player->jump_timer = 0.0f; // jumpCooldown = 0
    return 1;
}

// ---------------------------------------------------------------- collidingObjs

void world_player_colls_begin(Player *player) { player->colliding_seen = 0; }

void world_player_colls_touch(Player *player, const int object_index) {
    for (int i = 0; i < player->colliding_obj_count; i++) {
        if (player->colliding_objs[i] == object_index) player->colliding_seen |= (uint64_t) 1 << i;
    }
}

int world_player_colls_enter(Player *player, const int object_index) {
    for (int i = 0; i < player->colliding_obj_count; i++) {
        if (player->colliding_objs[i] == object_index) {
            player->colliding_seen |= (uint64_t) 1 << i;
            return 0;
        }
    }
    if (player->colliding_obj_count >= PLAYER_COLLIDING_OBJS_MAX) return 0;
    player->colliding_seen |= (uint64_t) 1 << player->colliding_obj_count;
    player->colliding_objs[player->colliding_obj_count++] = object_index;
    return 1;
}

void world_player_colls_end(Player *player) {
    Game *game = player->game;
    int exited[PLAYER_COLLIDING_OBJS_MAX], exit_count = 0, kept = 0;
    // v7 fills vA541 only with game.saveColls, which generateTriggerTypes sets whenever an object
    // listens to onEnterObject / onExitObject, i.e. whenever collidingObjs can hold anything: the
    // touched set is used unconditionally (objects swapped in at runtime need no saveColls refresh)
    for (int i = 0; i < player->colliding_obj_count; i++) {
        if (player->colliding_seen >> i & 1) player->colliding_objs[kept++] = player->colliding_objs[i];
        else exited[exit_count++] = player->colliding_objs[i];
    }
    player->colliding_obj_count = kept;
    player->colliding_seen = 0;
    for (int i = 0; i < exit_count; i++) {
        Object *object = world_object_at(game, exited[i]);
        if (object && game_is_authority(game)) trigger_object_event(game, object, TRIGGER_V7_EVENT_ON_EXIT_OBJECT, player); // objectTrigger("onExitObject")
    }
}

// ---------------------------------------------------------------- node moves / animation

static void world_move_object(Object *object, const vec3 feet) {
#ifdef KRUNKNATIVE_CLIENT
    Mesh *mesh = object->mesh;
    if (mesh) {
        mesh->transform.position.x += feet.x - object->position.x;
        mesh->transform.position.y += feet.y - object->position.y;
        mesh->transform.position.z += feet.z - object->position.z;
        mesh_update_transform_matrix(mesh);
    }
#endif
    object->position = feet; // game.moveObj (bottom centre; a ramp's line stays, as in v7)
}

static WorldMovedObject *world_moved_find(Game *game, const int index) {
    for (size_t i = 0; i < game->world.moved_count; i++) {
        if (game->world.moved[i].object == index) return &game->world.moved[i];
    }
    return NULL;
}

static void world_send_move(Game *game, const int uid, const int index, const int kind, const int node) {
    const NetObjectStatePacket packet = {.uid = index, .kind = (uint8_t) kind, .value = node};
    game_send(game, uid, NET_PACKET_OBJECT_STATE, &packet, sizeof(packet));
}

// game.teleportToNode(objIndex, nodeIndex): the object's feet on pathNodes[nid], movedObjs, "mo"
int world_teleport_object_to_node(Game *game, Object *object, const int node_id) {
    if (!game || !game->map || !object) return 0;
    const Object *node = world_path_node(game->map, node_id);
    const int index = world_object_index(game, object);
    if (!node || index < 0) return 0;
    WorldMovedObject *moved = world_moved_find(game, index);
    if (!moved) {
        if (game->world.moved_count == game->world.moved_cap) {
            const size_t cap = game->world.moved_cap ? game->world.moved_cap * 2 : 8;
            WorldMovedObject *list = realloc(game->world.moved, cap * sizeof(*list));
            if (!list) return 0;
            game->world.moved = list;
            game->world.moved_cap = cap;
        }
        moved = &game->world.moved[game->world.moved_count++];
        moved->object = index;
        moved->origin = object->position; // orgX / orgY / orgZ
    }
    moved->node = node_id;
    world_move_object(object, node->position);
    if (game_is_authority(game)) world_send_move(game, NET_TARGET_ALL, index, NET_OBJECT_MOVE_TO_NODE, node_id);
    return 1;
}

// game.teleportFromNode(objIndex): back to orgX / orgY / orgZ when it was moved, "mf"
void world_reset_object_position(Game *game, Object *object) {
    if (!game || !object) return;
    const int index = world_object_index(game, object);
    WorldMovedObject *moved = index >= 0 ? world_moved_find(game, index) : NULL;
    if (!moved) return;
    world_move_object(object, moved->origin);
    *moved = game->world.moved[--game->world.moved_count]; // movedObjs.splice (order is not observable but in the join list)
    if (game_is_authority(game)) world_send_move(game, NET_TARGET_ALL, index, NET_OBJECT_MOVE_FROM_NODE, 0);
}

// game.playAnim(obj, clip, force, raw): the authority sends "ana" uid, clip (index of
// config.clipNames, or the name when raw), raw, true to every active / spectating player. Only
// objects with an animated asset model (hasAnim: aid with g / q / f) play clips.
void world_play_anim(Game *game, Object *object, const char *clip, const int raw) {
    if (!game || !object || !clip || !clip[0] || !game_is_authority(game)) return;
    const int index = world_object_index(game, object);
    if (index < 0) return;
    int clip_index = -1;
    for (int i = 0; i < (int) (sizeof(k_clip_names) / sizeof(k_clip_names[0])); i++) {
        if (!strcmp(k_clip_names[i], clip)) clip_index = i;
    }
    if (!raw && clip_index < 0) return;
    NetObjectAnimPacket packet = {.uid = index, .is_object = 1, .raw = (uint8_t) !!raw};
    snprintf(packet.clip, sizeof(packet.clip), "%s", clip);
    game_send(game, NET_TARGET_ALL, NET_PACKET_OBJECT_ANIM, &packet, sizeof(packet));
}

// ---------------------------------------------------------------- round / join / tick

void world_player_spawned(Game *game, Player *player) {
    (void) game;
    player->no_pvp = 0;
    player->force_pvp = 0;
    player->reset_timer = 0.0f;
    player->colliding_obj_count = 0; // player reset: collidingObjs.length = 0
    player->colliding_seen = 0;
    if (!player->has_init_pos) { // setInitPos (the first spawn)
        player->init_pos = player->position;
        player->has_init_pos = 1;
    }
}

void world_round_start(Game *game) {
    // MapManager.resetAll: movedObjs.length = 0. KrunkNative keeps the map between rounds on the
    // same map (map_reset instead of a new generate), so the moved objects go home first.
    for (size_t i = game->world.moved_count; i-- > 0;) {
        Object *object = world_object_at(game, game->world.moved[i].object);
        if (object) world_move_object(object, game->world.moved[i].origin);
    }
    game->world.moved_count = 0;
    world_remove_builds(game); // finishInit: removeBuilds
    object_manager_round_start(game); // KrunkNative: the Object Manager's records of those objects go too
    zone_reset(game);          // finishInit: resetZone

    // generateTriggerTypes: saveColls when a trigger listens to onEnterObject / onExitObject
    game->world.save_colls = 0;
    for (size_t i = 0; game->map && i < game->map->object_count; i++) {
        const TriggerData *trig = game->map->objects[i]->trig; // event_index: te 0..31 as v7 reads it
        if (trig && (trig->event_index == TRIGGER_EVENT_ON_ENTER || trig->event_index == TRIGGER_V7_EVENT_ON_EXIT_OBJECT)) game->world.save_colls = 1;
    }
}

void world_send_state(Game *game, const int uid) {
    if (!game->ready || !game->map || !game_is_authority(game)) return;
    zone_send_state(game, uid); // syncData.zone
    world_send_builds(game, uid); // syncData.dyno
    for (size_t i = 0; i < game->world.moved_count; i++) // syncData.mov [uid, node, ...]
        world_send_move(game, uid, game->world.moved[i].object, NET_OBJECT_MOVE_TO_NODE, game->world.moved[i].node);
}

void world_tick(Game *game, const float delta) {
    if (!game->ready) return;
    const int authority = game_is_authority(game);
    if (!authority) zone_animate(game, delta);
    const float delta_ms = delta * 1000.0f * game->config.delta_mlt;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player || !player->active) continue;
        if (authority && !game->end_timer) world_tick_own_time(game, player, delta_ms); // players.tickPlayer
    }
}
