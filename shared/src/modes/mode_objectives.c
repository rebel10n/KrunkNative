// Map objectives of the modes (Krunker v7.1.4 libs/MapManager.js addObjective / addFlag /
// addDominationZone, game.js flags + objectives, modes/point.js, king.js, tdf.js, car.js,
// players.js playerCollisions): Hardpoint / KOTH zones, CTF / Blitz / neutral flags and touch
// conversions (Infected, Hide & Seek). Authority only. v7.1.4 removed v3.1.1's generic objective
// scoring in game.update: each mode's update scores its own objective (map-logic.md §2.3, §4.1).
#include "mode_internal.h"
#ifdef GAME_CONFIG_V7
#include <game_config.h>
#endif
#include <math.h>
#include <string.h>

int player_collides(Player*, const Object*, float); // shared/src/player.c

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

// MapManager.addObjective uid: obi (1..10) - 1, else the first free-looking uid as v7 computes
// it: any existing uid above the candidate jumps it past that uid, an equal one bumps it, and
// with no jump at all the uid is the objective count.
static int objective_uid(const GameMode *mode, const int obi) {
    if (obi) return CLAMP(obi, 1, 10) - 1;
    int uid = 0, jumped = 0;
    for (int i = 0; i < mode->objective_count; i++) {
        if (mode->objectives[i].uid > uid) {
            uid = mode->objectives[i].uid + 1;
            jumped = 1;
        } else if (mode->objectives[i].uid == uid) {
            uid++;
        }
    }
    return jumped ? uid : mode->objective_count;
}

void mode_load_objects(Game *game) {
    GameMode *mode = game->mode;
    mode->objective_count = 0;
    mode->flag_count = 0;
    mode->dom_zone_count = 0;
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

        switch (map_raw_object_prefab(object)) {
            case PREFAB_OBJECTIVE:
                // addObjective(x, y, z, sx, sz, sy, {objId}): y + h / 2 centre, half extents
                if (mode->objective_count < MODE_MAX_OBJECTIVES) {
                    const int obi = raw_int(object, "obi", 0);
                    const int uid = objective_uid(mode, obi);
                    mode->objectives[mode->objective_count++] = (ModeObjective) {
                        .position = {position.x, position.y + scale.y * 0.5f, position.z},
                        .half = {scale.x * 0.5f, scale.y * 0.5f, scale.z * 0.5f},
                        .link = obi,
                        .uid = uid,
                    };
                }
                break;
            case PREFAB_DOMINATION_ZONE:
                // addDominationZone(x, y, z, sx, sz, sy): uid = its index
                if (mode->dom_zone_count < MODE_MAX_DOM_ZONES) {
                    ModeDomZone *zone = &mode->dom_zones[mode->dom_zone_count++];
                    memset(zone, 0, sizeof(*zone));
                    zone->position = (vec3) {position.x, position.y + scale.y * 0.5f, position.z};
                    zone->half = (vec3) {scale.x * 0.5f, scale.y * 0.5f, scale.z * 0.5f};
                    zone->mlt = 1.0f;
                }
                break;
            case PREFAB_FLAG: {
                // addFlag(x, y, z, parseInt(tm || 0)): tm -1 is neutral, else team tm + 1; the
                // crystal floats flagScale / 2 + flagOff above
                if (mode->flag_count >= MODE_MAX_FLAGS) break;
                const int tm = raw_int(object, "tm", 0);
                ModeFlag *flag = &mode->flags[mode->flag_count++];
                memset(flag, 0, sizeof(*flag));
                flag->origin = (vec3) {position.x, position.y + MODE_FLAG_SCALE * 0.5f + MODE_FLAG_OFF, position.z};
                flag->position = flag->origin;
                flag->ping = flag->origin;
                flag->team = tm == -1 ? -1 : tm + 1;
                flag->neutral = tm == -1;
                flag->carrier_uid = -1;
                break;
            }
            case PREFAB_SCORE_ZONE:
                mode->target_score += raw_int(object, "sp", 0) ? raw_int(object, "sp", 0) : 1; // parseInt(sp) || 1
                break;
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

Player *mode_find_player(const Game *game, const int uid) {
    for (size_t i = 0; i < game->player_count; i++) {
        if (game->players[i] && game->players[i]->uid == uid) return game->players[i];
    }
    return NULL;
}

const ModeObjective *mode_objective_by_uid(const Game *game, const int uid) {
    if (!game || !game->mode || uid < 0) return NULL;
    for (int i = 0; i < game->mode->objective_count; i++) {
        if (game->mode->objectives[i].uid == uid) return &game->mode->objectives[i];
    }
    return NULL;
}

ModeObjective *mode_active_objective(Game *game) {
    return (ModeObjective *) mode_objective_by_uid(game, game->mode->active_objective);
}

// The drop point of a kill: lastGround while the victim was airborne, else where it died
// (`lastGround && !onGround ? lastGround : player`). Returns 1 when lastGround was used.
int mode_kill_point(const Player *victim, vec3 *out) {
    const int ground = victim->mode.has_last_ground && !victim->on_ground;
    *out = ground ? victim->mode.last_ground : victim->position;
    return ground;
}

// The objective update guard of point / king / depo: objectives exist and the first waitTimer
// is the running rotation timer (contTime). v7.1.4 (as v3.1.1): a single objective never gets a
// rotation timer, so a one-objective map does not score in these modes.
static int objective_clock_running(const Game *game) {
    const GameMode *mode = game->mode;
    return mode->objective_count && mode->wait_timer_count && mode->wait_timers[0].cont_time;
}

// ------------------------------------------------------------------ Hardpoint

static int cfg_comp(const Game *game) {
#ifdef GAME_CONFIG_V7 // WP-E: game.isComp
    return !!game->config.comp;
#else
    (void) game;
    return 0;
#endif
}

static int cfg_team_size(const Game *game) {
#ifdef GAME_CONFIG_V7 // WP-E: config.tmSize select index (0..3 = 1v1..4v4)
    return game->config.team_size;
#else
    (void) game;
    return 3; // serverConfig tmSize default
#endif
}

// point.js update: every 1.5 s everyone on the active objective scores objScore (type 2). In
// competitive games the round ends as soon as the trailing team can no longer catch up within
// the remaining clock ("chi" modes.round.win / loss).
void mode_update_point(Game *game, const float delta) {
    GameMode *mode = game->mode;
    if (!objective_clock_running(game)) return;
    mode->objective_timer -= delta;
    if (mode->objective_timer > 0.0f) return;
    mode->objective_timer = MODE_OBJECTIVE_TICK;

    const int points = mode->config.obj_score ? mode->config.obj_score : 10;
    const ModeObjective *objective = mode_active_objective(game);
    for (size_t i = 0; objective && i < game->player_count; i++) {
        Player *player = game->players[i];
        if (player && player->active && mode_player_collides(player, objective->position, objective->half)) player_score(player, points, 2);
    }

    const int score1 = mode->team_scores[1], score2 = mode->team_scores[2];
    if (mode->not_winnable || score1 == score2 || !cfg_comp(game) || !(score1 || score2)) return;
    const int ticks = (int) floorf(fmaxf(game->round_timer, 0.0f) / MODE_OBJECTIVE_TICK);
    const int catch_up = ticks * points * (cfg_team_size(game) + 1);
    const int trailing = score1 > score2 ? 2 : 1;
    const int leading = score1 > score2 ? 1 : 2;
    if (mode->team_scores[trailing] + catch_up >= mode->team_scores[leading]) return;

    mode->not_winnable = 1;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (!player) continue;
        NetChatPacket chat;
        // "chi" -1, null, i18n key, 2, "#ff0000" (en.js modes.round.loss / win)
        net_packet_chat(&chat, -1, NET_CHAT_SERVER, NULL,
                        player->team == trailing ? "This round is no longer winnable" : "Your Team has secured this round");
        game_send(game, player->uid, NET_PACKET_CHAT, &chat, sizeof(chat));
    }
    mode->skip = 1; // game.endGame()
}

// ------------------------------------------------------------------ King of the Hill

// king.js collisionCheck: player.onZone = the active hill's uid while standing on it
void mode_collision_king(Game *game, Player *player) {
    const ModeObjective *objective = mode_active_objective(game);
    if (objective && mode_player_collides(player, objective->position, objective->half)) player->mode.on_zone = objective->uid;
}

// king.js update: hill occupancy every tick ("uobj" on change); every 1.5 s a lone occupant
// scores objScore 10 (type 2). v3.1.1 scored everyone on the hill 20.
void mode_update_king(Game *game, const float delta) {
    GameMode *mode = game->mode;
    if (!objective_clock_running(game)) return;

    int tick = 0;
    mode->objective_timer -= delta;
    if (mode->objective_timer <= 0.0f) {
        mode->objective_timer = MODE_OBJECTIVE_TICK;
        tick = 1;
    }

    ModeObjective *objective = mode_active_objective(game);
    if (!objective) return;

    const int before = objective->on_zone;
    Player *lone = NULL;
    int count = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player || !player->active || !mode_player_collides(player, objective->position, objective->half)) continue;
        if (!count) lone = player;
        count++;
    }
    objective->on_zone = count;
    if (count == 1 && tick && lone) player_score(lone, mode->config.obj_score, 2);
    if (before != objective->on_zone) mode_send_occupancy(game, objective);
}

// king.js onKill: a kill from the hill +50 "Defender" (and killMlt += hillMlt), a kill of
// someone on the hill +25 "Attacker".
void mode_kill_king(Game *game, Player *victim, Player *killer, const int weapon_id) {
    (void) weapon_id;
    if (!killer || killer == victim || game->mode->active_objective < 0) return;
    if (killer->mode.on_zone == game->mode->active_objective) {
        player_score(killer, 50, 0);
        mode_send_medal(game, killer, "Defender", 50);
        killer->mode.kill_mlt += game->mode->config.hill_mlt;
    } else if (victim->mode.on_zone == game->mode->active_objective) {
        player_score(killer, 25, 0);
        mode_send_medal(game, killer, "Attacker", 25);
    }
}

// ------------------------------------------------------------------ Team Defender / Carrier

// tdf.js / car.js update: every 1.5 s the flag carrier scores objScore, only 1 in small public
// lobbies (connectedClients < 4, no host, not comp). KN counts the non-bot players as the
// connected clients.
void mode_update_carrier(Game *game, const float delta) {
    GameMode *mode = game->mode;
    if (!mode->flag_count || (mode->wait_timer_count && !mode->wait_timers[0].cont_time)) return;
    mode->objective_timer -= delta;
    if (mode->objective_timer > 0.0f) return;
    mode->objective_timer = MODE_OBJECTIVE_TICK;

    int clients = 0;
    for (size_t i = 0; i < game->player_count; i++) clients += game->players[i] && !game->players[i]->is_bot;
#ifdef GAME_CONFIG_V7
    const int hosted = game_config_is_custom(game); // hasHost(): a hosted game or the sandbox
#else
    const int hosted = 0;
#endif
    const int low = clients < 4 && !hosted && !cfg_comp(game);
    const int points = low ? 1 : mode->config.obj_score ? mode->config.obj_score : 10;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (player && player->active && player->mode.flag) player_score(player, points, 0);
    }
}

// ------------------------------------------------------------------ flags (CTF, Blitz, oneflag)

static void flag_set_info(Game *game, const int team, const char *text) {
    if (team < 0 || team >= MODE_TEAMS) return;
    snprintf(game->mode->flag_info[team], MODE_TEXT_SIZE, "%s", text);
}

static int flag_at_base(const ModeFlag *flag) {
    return flag->position.x == flag->origin.x && flag->position.y == flag->origin.y && flag->position.z == flag->origin.z;
}

// updateCarrier(flag, player): carrier sid, game.flagTeam / flagCarrier, "ufl"
static void flag_set_carrier(Game *game, const int index, Player *player) {
    ModeFlag *flag = &game->mode->flags[index];
    Player *old = flag->carrier_uid >= 0 ? mode_find_player(game, flag->carrier_uid) : NULL;
    if (old && old->mode.flag == index + 1) old->mode.flag = 0;

    flag->carrier_uid = player ? player->uid : -1;
    if (player) {
        player->mode.flag = index + 1;
        flag->ping_timer = MODE_FLAG_UPDATE;
        flag->ping = (vec3) {player->position.x, player->position.y + MODE_FLAG_HOFF, player->position.z};
        game->mode->flag_team = player->team;
        game->mode->flag_carrier_uid = player->uid;
    } else {
        flag->ping = flag->position;
        game->mode->flag_team = 0;
        game->mode->flag_carrier_uid = -1;
    }
}

// resetFlag: back at its base, "AT BASE" (not oneflag), no idle timer.
void mode_reset_flag(Game *game, const int index) {
    if (index < 0 || index >= game->mode->flag_count) return;
    ModeFlag *flag = &game->mode->flags[index];

    flag->position = flag->origin;
    flag_set_carrier(game, index, NULL);
    if (!game->mode->config.oneflag) flag_set_info(game, flag->team, "AT BASE");
    flag->idle_timer = 0.0f;
    mode_send_flag(game, index);
}

// resetFlags (finishInit): every flag home; only the flags of this mode's kind are in play.
void mode_reset_flags(Game *game) {
    GameMode *mode = game->mode;
    mode->flag_team = 0;
    mode->flag_carrier_uid = -1;
    for (int f = 0; f < mode->flag_count; f++) {
        ModeFlag *flag = &mode->flags[f];
        flag->carrier_uid = -1;
        flag->position = flag->origin;
        flag->ping = flag->origin;
        flag->idle_timer = 0.0f;
        flag->ping_timer = 0.0f;
        flag->active = (!mode->config.oneflag || flag->neutral) && (mode->config.oneflag || !flag->neutral);
    }
}

// dropFlag: where the carrier was (y + flagOff), "AWAY" (not oneflag), home after flagIdle.
void mode_drop_flag(Game *game, Player *carrier) {
    if (!carrier || !carrier->mode.flag) return;
    const int index = carrier->mode.flag - 1;
    if (index < 0 || index >= game->mode->flag_count) return;

    ModeFlag *flag = &game->mode->flags[index];
    if (!game->mode->config.rush) mode_send_action(game, carrier, NULL, "dropped the Flag");
    flag_set_carrier(game, index, NULL);
    carrier->mode.flag = 0;
    if (!game->mode->config.oneflag) flag_set_info(game, flag->team, "AWAY");
    flag->position = (vec3) {carrier->position.x, carrier->position.y + MODE_FLAG_OFF, carrier->position.z};
    flag->ping = flag->position;
    flag->idle_timer = MODE_FLAG_IDLE;
    mode_send_flag(game, index);
}

// capFlag: caps++, +1 team score, +150 "Capture" (type 2), the enemy flag goes home; Blitz
// (rush) sends no "ac" and puts the capper back at a spawn point.
static void flag_capture(Game *game, Player *player) {
    const int index = player->mode.flag - 1;
    player->mode.caps++;
    player->mode.flag = 0;
    mode_team_score_add(game, player->team, 1);
    if (!game->mode->config.rush) mode_send_action(game, player, NULL, "captured the Flag");
    mode_send_medal(game, player, "Capture", 150);
    player_score(player, 150, 2);
    mode_reset_flag(game, index);

    if (game->mode->config.rush) {
        vec3 position;
        int direction;
        if (game_get_spawn_point(game, player, &position, &direction)) game_change_position(game, player, position, 1, 0, 0, 0);
    }
}

// pickupFlag: an enemy (or, oneflag, any) flag is taken (+20 "Pickup", type 2) when you carry
// none; your own flag away from base returns it (+50 "Return").
static void flag_touch(Game *game, Player *player, const int index) {
    ModeFlag *flag = &game->mode->flags[index];
    const GameModeConfig *config = &game->mode->config;
    if (flag->carrier_uid >= 0) return;

    if (player->team != flag->team || config->oneflag) {
        if (player->mode.flag) return;
        mode_send_medal(game, player, "Pickup", 20);
        player_score(player, 20, 2);
        flag_set_carrier(game, index, player);
        if (!config->oneflag) flag_set_info(game, flag->team, player->name);
        mode_send_flag(game, index);
        if (!config->rush) mode_send_action(game, player, NULL, "got the Flag");
        return;
    }

    if (flag_at_base(flag)) return;
    if (!config->rush) mode_send_action(game, player, NULL, "returned the Flag");
    mode_send_medal(game, player, "Return", 50);
    player_score(player, 50, 2);
    mode_reset_flag(game, index);
}

// game.updateFlags (server) + the player.js flag / flag-zone collisions.
void mode_tick_flags(Game *game, const float delta) {
    GameMode *mode = game->mode;
    if (!(mode->config.flags || mode->config.oneflag) || !mode->flag_count) return;

    const vec3 flag_half = {MODE_FLAG_SCALE, MODE_FLAG_SCALE * 0.5f, MODE_FLAG_SCALE};
    const vec3 zone_half = {MODE_FLAG_ZONE_SIZE * 0.5f, MODE_FLAG_ZONE_HEIGHT * 0.5f, MODE_FLAG_ZONE_SIZE * 0.5f};

    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player || !player->active || player->noclip) continue;

        for (int f = 0; f < mode->flag_count; f++) {
            const ModeFlag *flag = &mode->flags[f];
            if (!flag->active) continue;

            // the flag's trigger zone: carrying a flag into your own zone while your flag is home
            // (Blitz: home or carried from home) captures; oneflag modes never capture
            const vec3 zone_center = {flag->origin.x, flag->origin.y - MODE_FLAG_SCALE * 0.5f - MODE_FLAG_OFF + MODE_FLAG_ZONE_HEIGHT * 0.5f, flag->origin.z};
            if (player->mode.flag && !mode->config.oneflag && flag->team == player->team && (mode->config.rush || flag->carrier_uid < 0) &&
                flag_at_base(flag) && mode_player_collides(player, zone_center, zone_half)) {
                flag_capture(game, player);
                continue;
            }

            if (mode_player_collides(player, flag->position, flag_half)) flag_touch(game, player, f);
        }
    }

    for (int f = 0; f < mode->flag_count; f++) {
        ModeFlag *flag = &mode->flags[f];
        if (!flag->active) continue;

        // v7: a dropped flag goes home after flagIdle
        if (flag->carrier_uid < 0 && flag->idle_timer > 0.0f) {
            flag->idle_timer -= delta;
            if (flag->idle_timer <= 0.0f) mode_reset_flag(game, f);
        }

        // updateFlagPings: the carrier's position every flagUpdate ("ufp")
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
