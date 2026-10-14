// NET_EXT_KN_BOT_PATH body (shared/include/bot_path_net.h): fill on the authority, decode on
// the client. The route comes from bot_path_current() (module bots-nav, bot_path.h): point 0
// is the bot's feet (segment 0's `from`), every further point a segment's `to`, with the
// segment's kind / flags on the point it ends at.
#include <bot_path_net.h>
#include <bot_path.h>
#include <stdio.h>
#include <string.h>

static void put3(float out[3], const vec3 v) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}

uint16_t bot_path_packet_fill(Game *game, const GameBot *bot, NetBotPathPacket *out) {
    if (!bot || !out || !bot->player) return 0;
    memset(out, 0, sizeof(*out));
    out->bot_sid = bot->uid;
    out->target_sid = -1;
    out->state = (uint8_t) bot->state;
    snprintf(out->state_name, sizeof(out->state_name), "%s", bot_state_name(bot->state));
    if (bot->player->active) out->flags |= NET_BOT_PATH_ACTIVE;

    vec3 goal;
    if (bot_path_goal(bot, &goal)) {
        out->flags |= NET_BOT_PATH_HAS_GOAL;
        put3(out->goal, goal);
    }
    if (bot->path_stranded) out->flags |= NET_BOT_PATH_STRANDED;

    if (bot->target_uid >= 0) {
        out->target_sid = bot->target_uid;
        const Player *target = game ? game_find_player(game, bot->target_uid) : NULL;
        if (target) {
            out->flags |= NET_BOT_PATH_HAS_TARGET;
            put3(out->target, target->position);
            if (bot->sees_target) out->flags |= NET_BOT_PATH_SEES_TARGET;
        }
    }
    if (bot->have_seen || bot->target_uid >= 0) {
        out->flags |= NET_BOT_PATH_HAS_LAST_KNOWN;
        put3(out->last_known, bot->last_known);
    }

    BotPathSegment segments[BOT_PATH_SEGMENTS_MAX];
    const int count = bot_path_current(bot, segments, BOT_PATH_SEGMENTS_MAX);
    if (count > 0) {
        const int shown = count < NET_BOT_PATH_POINTS_MAX - 1 ? count : NET_BOT_PATH_POINTS_MAX - 1;
        if (shown < count) out->flags |= NET_BOT_PATH_TRUNCATED;
        NetBotPathPoint *p = &out->points[0];
        p->x = segments[0].from.x, p->y = segments[0].from.y, p->z = segments[0].from.z;
        p->kind = BOT_PATH_WALK;
        for (int i = 0; i < shown; i++) {
            p = &out->points[i + 1];
            p->x = segments[i].to.x, p->y = segments[i].to.y, p->z = segments[i].to.z;
            p->kind = segments[i].kind;
            p->flags = segments[i].flags;
        }
        out->n = (uint8_t) (shown + 1);
        if (segments[count - 1].flags & BOT_PATH_FLAG_PARTIAL) out->flags |= NET_BOT_PATH_PARTIAL;
    }
    return (uint16_t) NET_BOT_PATH_SIZE(out->n);
}

int bot_path_packet_decode(const void *body, const uint16_t length, NetBotPathPacket *out) {
    if (!body || !out || length < NET_BOT_PATH_SIZE(0)) return 0;
    memset(out, 0, sizeof(*out));
    memcpy(out, body, NET_BOT_PATH_SIZE(0));
    if (out->n > NET_BOT_PATH_POINTS_MAX || length < NET_BOT_PATH_SIZE(out->n)) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    memcpy(out->points, (const uint8_t *) body + NET_BOT_PATH_SIZE(0), (size_t) out->n * sizeof(NetBotPathPoint));
    out->state_name[NET_BOT_PATH_STATE_SIZE - 1] = 0;
    return 1;
}
