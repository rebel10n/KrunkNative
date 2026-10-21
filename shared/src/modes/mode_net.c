// The mode packets (shared.h NET_PACKET_MODE_STATE .. NET_PACKET_MEDAL and the WP-D block
// 140-149): what Krunker's server sends as gmsg / tm / ts / ufl / ufi / obj / uobj / uct / cct /
// rds / lkt / tx / krt / chrg / bh / wep / lv / ac / inat / lock / lm / crsp / dsp / end(tms) /
// am. All leave through game_send (GameHooks.send).
#include "mode_internal.h"
#include <math.h>
#include <string.h>

static void copy_text(char *out, const size_t size, const char *text) {
    snprintf(out, size, "%s", text ? text : "");
}

static void mode_fill_flag(const GameMode *mode, const int index, NetModeFlag *out) {
    const ModeFlag *flag = &mode->flags[index];
    const vec3 where = flag->carrier_uid >= 0 ? flag->ping : flag->position;
    out->position[0] = where.x;
    out->position[1] = where.y;
    out->position[2] = where.z;
    out->carrier_uid = flag->carrier_uid;
    out->team = flag->team;
    out->uid = flag->uid;
    out->active = flag->active ? 1 : 0;
}

static void mode_fill_state(Game *game, const Player *recipient, NetModeStatePacket *packet) {
    const GameMode *mode = game->mode;
    memset(packet, 0, sizeof(*packet));

    packet->mode_id = mode->config.id;
    packet->team_scores[0] = mode->team_scores[1];
    packet->team_scores[1] = mode->team_scores[2];
    copy_text(packet->flag_info[0], NET_MODE_TEXT_SIZE, mode->flag_info[1][0] ? mode->flag_info[1] : "AT BASE");
    copy_text(packet->flag_info[1], NET_MODE_TEXT_SIZE, mode->flag_info[2][0] ? mode->flag_info[2] : "AT BASE");
    packet->active_objective = mode->config.objective ? mode->active_objective : -1; // v7: the objective uid
    packet->objective_weapon = mode->config.obj_weapon ? mode->active_objective : -1;
    packet->move_lock = game->move_lock;
    packet->locked = recipient ? recipient->mode.locked : 0;
    packet->lives = mode->lives ? (recipient ? recipient->mode.lives : mode->lives) : -1;
    packet->flag_count = (uint8_t) mode->flag_count;
    for (int f = 0; f < mode->flag_count; f++) mode_fill_flag(mode, f, &packet->flags[f]);

    if (mode->waiting_sent) {
        copy_text(packet->message, sizeof(packet->message), NET_GAME_MESSAGE_WAITING);
    } else if (mode->message_sent && mode->wait_timer_count) {
        char time[16];
        const int total = (int) mode->wait_timers[0].time;
        snprintf(time, sizeof(time), "%02d:%02d", (total / 60) % 60, total % 60);
        snprintf(packet->message, sizeof(packet->message), "%s%s", mode->wait_timers[0].msg ? mode->wait_timers[0].msg : "", time);
    }
}

// init sync for one joiner (or everyone): teamScores 1..5, roundC, lockT, obj, doms, spwns
static void mode_send_sync_v7(Game *game, const int uid) {
    const GameMode *mode = game->mode;
    NetModeSyncV7Packet sync;
    memset(&sync, 0, sizeof(sync));
    for (int t = 0; t < MODE_TEAMS; t++) {
        sync.team_scores[t] = mode->team_scores[t];
        sync.team_round_wins[t] = mode->team_round_wins[t];
    }
    sync.rounds_left = mode->round_count;
    sync.has_round_wins = mode->has_round_wins;
    sync.lockout_ms = (int32_t) (mode->lockout_timer * 1000.0f);
    sync.objective_uid = mode->config.objective ? mode->active_objective : -1;
    sync.objective_ms = (int32_t) (mode->objective_time * 1000.0f);
    game_send(game, uid, NET_PACKET_MODE_SYNC_V7, &sync, sizeof(sync));

    if (mode->dom_zone_count) {
        int zones[MODE_MAX_DOM_ZONES];
        for (int z = 0; z < mode->dom_zone_count; z++) zones[z] = z;
        mode_send_dom_update(game, zones, mode->dom_zone_count, uid);
    }
    for (int i = 0; i < mode->objective_count; i++) {
        if (mode->objectives[i].on_zone) mode_send_occupancy(game, &mode->objectives[i]);
    }
    if (uid != NET_TARGET_ALL) spawnables_send_all(game, uid);
}

void mode_send_state(Game *game, const int uid) {
    if (!game->mode) return;
    NetModeStatePacket packet;

    if (uid != NET_TARGET_ALL) {
        mode_fill_state(game, game_find_player(game, uid), &packet);
        game_send(game, uid, NET_PACKET_MODE_STATE, &packet, sizeof(packet));
        mode_send_sync_v7(game, uid);
        return;
    }

    for (size_t i = 0; i < game->player_count; i++) {
        const Player *player = game->players[i];
        if (!player) continue;
        mode_fill_state(game, player, &packet);
        game_send(game, player->uid, NET_PACKET_MODE_STATE, &packet, sizeof(packet));
    }

    // spectators / menu clients: the recipient-free snapshot
    mode_fill_state(game, NULL, &packet);
    game_send(game, NET_TARGET_NON_PLAYERS, NET_PACKET_MODE_STATE, &packet, sizeof(packet));
    mode_send_sync_v7(game, NET_TARGET_ALL);
}

void mode_send_message(Game *game, const char *text) {
    NetGameMessagePacket packet;
    memset(&packet, 0, sizeof(packet));
    copy_text(packet.text, sizeof(packet.text), text);
    game_send(game, NET_TARGET_ALL, NET_PACKET_GAME_MESSAGE, &packet, sizeof(packet));
}

void mode_send_message_to(Game *game, const int uid, const char *text) {
    NetGameMessagePacket packet;
    memset(&packet, 0, sizeof(packet));
    copy_text(packet.text, sizeof(packet.text), text);
    game_send(game, uid, NET_PACKET_GAME_MESSAGE, &packet, sizeof(packet));
}

void mode_send_team(Game *game, const Player *player) {
    const NetTeamPacket packet = {
        .uid = player->uid,
        .team = player->team,
        .health = player->health,
        .max_health = player->max_health,
    };
    game_send(game, NET_TARGET_ALL, NET_PACKET_TEAM, &packet, sizeof(packet));
}

void mode_send_health(Game *game, const Player *player) {
    mode_send_team(game, player);
}

void mode_send_team_score(Game *game, const int team) {
    const NetTeamScorePacket packet = {.team = team, .score = game->mode->team_scores[team]};
    game_send(game, NET_TARGET_ALL, NET_PACKET_TEAM_SCORE, &packet, sizeof(packet));
}

void mode_send_flag(Game *game, const int index) {
    NetFlagPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.index = index;
    mode_fill_flag(game->mode, index, &packet.flag);
    const int team = game->mode->flags[index].team;
    copy_text(packet.info, sizeof(packet.info), team > 0 && team < MODE_TEAMS && game->mode->flag_info[team][0] ? game->mode->flag_info[team] : "AT BASE");
    game_send(game, NET_TARGET_ALL, NET_PACKET_FLAG, &packet, sizeof(packet));
}

void mode_send_objective(Game *game, const int kind, const int value) {
    const NetObjectivePacket packet = {.kind = kind, .value = value};
    game_send(game, NET_TARGET_ALL, NET_PACKET_OBJECTIVE, &packet, sizeof(packet));
}

void mode_send_lives(Game *game, const Player *player) {
    const NetLivesPacket packet = {.lives = player->mode.lives};
    game_send(game, player->uid, NET_PACKET_LIVES, &packet, sizeof(packet));
}

void mode_send_action(Game *game, const Player *player, const Player *other, const char *text) {
    NetActionPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.uid = player ? player->uid : -1;
    packet.other_uid = other ? other->uid : -1;
    copy_text(packet.text, sizeof(packet.text), text);
    game_send(game, NET_TARGET_ALL, NET_PACKET_ACTION, &packet, sizeof(packet));
}

void mode_send_loadout(Game *game, const Player *player) {
    NetLoadoutPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.uid = player->uid;
    packet.count = player_loadout_base(player) < NET_LOADOUT_MAX ? player_loadout_base(player) : NET_LOADOUT_MAX; // the base: extra weapons ride in LOADOUT_EXTRA
    for (int i = 0; i < packet.count; i++) {
        packet.weapons[i] = player->loadout[i];
        packet.ammo[i] = player->ammo ? (int32_t) player->ammo[i] : 0;
    }
    packet.loadout_index = player->loadout_index;
    packet.weapon_tier = player->mode.weapon_tier;
    game_send(game, NET_TARGET_ALL, NET_PACKET_LOADOUT, &packet, sizeof(packet));
}

void mode_send_lock(Game *game, const int uid, const int kind, const int locked) {
    const NetLockPacket packet = {.kind = (uint8_t) kind, .locked = (uint8_t) !!locked};
    game_send(game, uid, NET_PACKET_LOCK, &packet, sizeof(packet));
}

// "crsp type, sid, x, y, z, team, data" (playerSpecific: to the owner only) / "dsp sid"
void mode_send_spawnable(Game *game, const int index) {
    const ModeSpawnable *spawnable = &game->mode->spawnables[index];
    NetSpawnableV7Packet packet;
    memset(&packet, 0, sizeof(packet));
    packet.sid = spawnable->sid;
    packet.type = (uint8_t) spawnable->type;
    packet.active = spawnable->active;
    packet.team = spawnable->team;
    packet.position[0] = roundf(spawnable->position.x * 10.0f) / 10.0f; // x.round(1)
    packet.position[1] = roundf(spawnable->position.y * 10.0f) / 10.0f;
    packet.position[2] = roundf(spawnable->position.z * 10.0f) / 10.0f;
    packet.data_c = spawnable->data_c;
    packet.data_w = spawnable->data_w;
    packet.data_xd = spawnable->data_xd;
    packet.data_zd = spawnable->data_zd;
    game_send(game, spawnable->owner_uid >= 0 ? spawnable->owner_uid : NET_TARGET_ALL, NET_PACKET_SPAWNABLE_V7, &packet, sizeof(packet));
}

// "obj uid, objtvTime, time == null"
void mode_send_objective_v7(Game *game, const int uid, const int initial) {
    const NetObjectiveV7Packet packet = {
        .uid = uid,
        .time_ms = (int32_t) (game->mode->objective_time * 1000.0f),
        .initial = (uint8_t) !!initial,
    };
    game_send(game, NET_TARGET_ALL, NET_PACKET_OBJECTIVE_V7, &packet, sizeof(packet));
}

// "uobj uid, onZone"
void mode_send_occupancy(Game *game, const ModeObjective *objective) {
    const NetObjectiveOccupancyPacket packet = {.uid = objective->uid, .on_zone = objective->on_zone};
    game_send(game, NET_TARGET_ALL, NET_PACKET_OBJECTIVE_OCCUPANCY, &packet, sizeof(packet));
}

// "uct [uid, capturedBy, captureT, lastTeamOn, teamsOn, mlt]..." for the listed zones
void mode_send_dom_update(Game *game, const int *zones, const int count, const int uid) {
    NetDomUpdatePacket packet;
    memset(&packet, 0, sizeof(packet));
    for (int i = 0; i < count && packet.count < MODE_MAX_DOM_ZONES; i++) {
        const int z = zones[i];
        if (z < 0 || z >= game->mode->dom_zone_count) continue;
        const ModeDomZone *zone = &game->mode->dom_zones[z];
        NetDomZoneState *out = &packet.zones[packet.count++];
        out->uid = z;
        out->captured_by = zone->captured_by;
        out->capture_t = zone->capture_t * 1000.0f;
        out->last_team_on = zone->last_team_on;
        out->teams_on_count = (uint8_t) zone->teams_on_count;
        for (int t = 0; t < zone->teams_on_count && t < MODE_TEAMS; t++) out->teams_on[t] = (int8_t) zone->teams_on[t];
        out->mlt = zone->mlt;
    }
    if (packet.count) game_send(game, uid, NET_PACKET_DOM_UPDATE, &packet, sizeof(packet));
}

// "cct uid, team"
void mode_send_dom_capture(Game *game, const int zone, const int team) {
    const NetDomCapturePacket packet = {.uid = zone, .team = team};
    game_send(game, NET_TARGET_ALL, NET_PACKET_DOM_CAPTURE, &packet, sizeof(packet));
}

// "rds roundCount, false, winTeam, teamRoundWins"
void mode_send_rounds(Game *game, const int win_team) {
    NetRoundsPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.rounds_left = game->mode->round_count;
    packet.win_team = win_team;
    packet.has_round_wins = game->mode->has_round_wins;
    for (int t = 0; t < MODE_TEAMS; t++) packet.team_round_wins[t] = game->mode->team_round_wins[t];
    game_send(game, NET_TARGET_ALL, NET_PACKET_ROUNDS, &packet, sizeof(packet));
}

// "lkt lockoutTimer"
void mode_send_lockout(Game *game) {
    const NetLockoutPacket packet = {.ms = (int32_t) (game->mode->lockout_timer * 1000.0f)};
    game_send(game, NET_TARGET_ALL, NET_PACKET_LOCKOUT, &packet, sizeof(packet));
}

// "tx title, text" (the overtime popup)
void mode_send_text_popup(Game *game, const char *title, const char *text) {
    NetTextPopupPacket packet;
    memset(&packet, 0, sizeof(packet));
    copy_text(packet.title, sizeof(packet.title), title);
    copy_text(packet.text, sizeof(packet.text), text);
    game_send(game, NET_TARGET_ALL, NET_PACKET_TEXT_POPUP, &packet, sizeof(packet));
}

// "krt dogTags" (to the carrier) / "chrg sid, ms" / "bh sid, headMlt" (everyone)
void mode_send_counter(Game *game, const int kind, const Player *player, const float value, const int to_player) {
    const NetModeCounterPacket packet = {
        .kind = (uint8_t) kind,
        .uid = player->uid,
        .value = kind == NET_MODE_COUNTER_CHARGE ? value * 1000.0f : value,
    };
    game_send(game, to_player ? player->uid : NET_TARGET_ALL, NET_PACKET_MODE_COUNTER, &packet, sizeof(packet));
}

static int16_t clamp16(const int value) {
    return (int16_t) (value > 32767 ? 32767 : value < -32768 ? -32768 : value);
}

void mode_send_end(Game *game, const ModeWinner winner, Player **sorted, const size_t count) {
    NetModeEndPacket packet;
    memset(&packet, 0, sizeof(packet));

    packet.mode_id = game->mode->config.id;
    packet.winner_team = game->mode->config.teams && winner.team > 0 ? winner.team : -1;
    packet.team_scores[0] = game->mode->team_scores[1];
    packet.team_scores[1] = game->mode->team_scores[2];

    for (size_t i = 0; i < count && packet.count < NET_MODE_END_MAX; i++) {
        const Player *player = sorted[i];
        if (!player) continue;
        NetModeEndEntry *entry = &packet.entries[packet.count++];
        entry->uid = player->uid;
        entry->stats[NET_MODE_STAT_CAPS] = clamp16(player->mode.caps);
        entry->stats[NET_MODE_STAT_CONFIRMS] = clamp16(player->mode.confirms);
        entry->stats[NET_MODE_STAT_DENIES] = clamp16(player->mode.denies);
        entry->stats[NET_MODE_STAT_OBJECTIVE] = clamp16(player->mode.objective_score);
        entry->stats[NET_MODE_STAT_WEAPON_TIER] = clamp16(player->mode.weapon_tier);
        entry->stats[NET_MODE_STAT_SETBACKS] = clamp16(player->mode.setbacks);
        entry->stats[NET_MODE_STAT_MELEES] = clamp16(player->mode.melees);
        entry->stats[NET_MODE_STAT_CONVS] = clamp16(player->mode.convs);
        entry->stats[NET_MODE_STAT_LIVES] = clamp16(player->mode.lives);
        entry->stats[NET_MODE_STAT_ASSISTS] = clamp16((int) player->assists);
        // getTime(lastTimer, showMS) keeps tenths: store them + 1 so 0 still means "DNF"
        if (player->mode.has_timer) entry->stats[NET_MODE_STAT_TIME] = clamp16((int) floorf(player->mode.timer * 10.0f + 0.0001f) + 1);
        entry->stats[NET_MODE_STAT_DEPOSITS] = clamp16(player->mode.deposits);
        entry->stats[NET_MODE_STAT_PICKUPS] = clamp16(player->mode.pickups);
    }

    game_send(game, NET_TARGET_ALL, NET_PACKET_MODE_END, &packet, sizeof(packet));
}

int mode_leader_stat_value(const Player *player, const char *key) {
    if (!player || !key) return 0;
    const ModePlayerState *m = &player->mode;
    if (!strcmp(key, "score") || !strcmp(key, "points")) return player->score;     // endForm.points: score
    if (!strcmp(key, "kills")) return (int) player->kills;
    if (!strcmp(key, "deaths") || !strcmp(key, "downs")) return (int) player->deaths; // endForm.downs: deaths
    if (!strcmp(key, "caps")) return m->caps;
    if (!strcmp(key, "objective")) return m->objective_score;                     // endForm.objective: objectiveScore || 0
    if (!strcmp(key, "weaponTier")) return m->weapon_tier;
    if (!strcmp(key, "confirms")) return m->confirms;
    if (!strcmp(key, "denies")) return m->denies;
    if (!strcmp(key, "deposits")) return m->deposits;
    if (!strcmp(key, "pickups")) return m->pickups;
    if (!strcmp(key, "lives")) return m->lives;
    if (!strcmp(key, "zombies")) return m->ai_kills;                              // endForm.zombies: aiKills || 0
    if (!strcmp(key, "dmg")) return (int) lroundf(player->damage_dealt);         // endForm.dmg: round(dmgDealt || 0)
    return 0;
}

static int mode_leaders_by_score(const Player *a, const Player *b) { return b->score - a->score; }

void mode_sort_leaders(const Game *game, Player **players, const size_t count) {
    const GameModeConfig *config = game && game->mode ? &game->mode->config : NULL;
    const ModeSortFn sort = config && config->leader_sort ? config->leader_sort : mode_leaders_by_score;
    for (size_t i = 1; i < count; i++) {
        Player *player = players[i];
        size_t at = i;
        while (at > 0 && sort(players[at - 1], player) > 0) {
            players[at] = players[at - 1];
            at--;
        }
        players[at] = player;
    }
}

void mode_send_medal(Game *game, const Player *player, const char *text, const int score) {
    // WP-E part 2c: the mode medals' game.incMedal (Confirmed, Denied, Capture, Defender, ...).
    // The players are the game's own (non-const) ones.
    game_inc_medal(game, (Player *) player, text);
    NetMedalPacket packet;
    memset(&packet, 0, sizeof(packet));
    copy_text(packet.text, sizeof(packet.text), text);
    packet.score = score;
    game_send(game, player->uid, NET_PACKET_MEDAL, &packet, sizeof(packet));
}
