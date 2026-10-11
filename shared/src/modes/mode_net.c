// The mode packets (shared.h NET_PACKET_MODE_STATE .. NET_PACKET_MEDAL): what Krunker's
// server sends as gmsg / tm / ts / ufl / ufi / obj / wep / lv / ac / inat / lock / lm /
// crsp / dsp / end(tms) / am. All leave through game_send (GameHooks.send).
#include "mode_internal.h"
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
}

static void mode_fill_state(Game *game, const Player *recipient, NetModeStatePacket *packet) {
    const GameMode *mode = game->mode;
    memset(packet, 0, sizeof(*packet));

    packet->mode_id = mode->config.id;
    packet->team_scores[0] = mode->team_scores[1];
    packet->team_scores[1] = mode->team_scores[2];
    copy_text(packet->flag_info[0], NET_MODE_TEXT_SIZE, mode->flag_info[1][0] ? mode->flag_info[1] : "AT BASE");
    copy_text(packet->flag_info[1], NET_MODE_TEXT_SIZE, mode->flag_info[2][0] ? mode->flag_info[2] : "AT BASE");
    packet->active_objective = mode->config.objective ? mode->active_objective : -1;
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

void mode_send_state(Game *game, const int uid) {
    if (!game->mode) return;
    NetModeStatePacket packet;

    if (uid != NET_TARGET_ALL) {
        mode_fill_state(game, game_find_player(game, uid), &packet);
        game_send(game, uid, NET_PACKET_MODE_STATE, &packet, sizeof(packet));
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
    packet.count = player->loadout_size < NET_LOADOUT_MAX ? player->loadout_size : NET_LOADOUT_MAX;
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

void mode_send_spawnable(Game *game, const int index) {
    const ModeSpawnable *tag = &game->mode->spawnables[index];
    const NetSpawnablePacket packet = {
        .sid = tag->sid,
        .active = tag->active,
        .team = tag->team,
        .position = {tag->position.x, tag->position.y, tag->position.z},
    };
    game_send(game, NET_TARGET_ALL, NET_PACKET_SPAWNABLE, &packet, sizeof(packet));
}

static int16_t clamp16(const int value) {
    return (int16_t) (value > 32767 ? 32767 : value < -32768 ? -32768 : value);
}

void mode_send_end(Game *game, const ModeWinner winner, Player **sorted, const size_t count) {
    NetModeEndPacket packet;
    memset(&packet, 0, sizeof(packet));

    packet.mode_id = game->mode->config.id;
    packet.winner_team = game->mode->config.teams && winner.team ? winner.team : -1;
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
    }

    game_send(game, NET_TARGET_ALL, NET_PACKET_MODE_END, &packet, sizeof(packet));
}

void mode_send_medal(Game *game, const Player *player, const char *text, const int score) {
    NetMedalPacket packet;
    memset(&packet, 0, sizeof(packet));
    copy_text(packet.text, sizeof(packet.text), text);
    packet.score = score;
    game_send(game, player->uid, NET_PACKET_MEDAL, &packet, sizeof(packet));
}
