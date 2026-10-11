// Networking helpers shared by the native TCP client (net.c) and the
// WebSocket client used by the emscripten build (net_web.c): packet logging
// (enabled with KRUNKNATIVE_PACKET_LOG=1) and the thread-safe inbound queue
// drained by client_tick_net().
#ifdef WIN32
#include <windows.h>
#endif

#include <client.h>
#include <ai.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int client_net_packet_logging_enabled() {
    static int checked = 0;
    static int enabled = 0;

    if (!checked) {
        const char *value = getenv("KRUNKNATIVE_PACKET_LOG");
        enabled = value && value[0] && strcmp(value, "0") != 0;
        checked = 1;
    }

    return enabled;
}

static void client_net_log_bad_payload(const char *name, const NetPacketType type, const void *payload, const uint16_t length) {
    const uint8_t *bytes = payload;

    printf("{\"type\":\"%s\",\"id\":%u,\"length\":%u,\"payload\":\"", name, (unsigned int) type, (unsigned int) length);

    for (uint16_t i = 0; i < length; i++) {
        printf("%02X", bytes ? bytes[i] : 0);
    }

    printf("\"}");
}

#ifdef WIN32
static void client_net_enable_ansi_output() {
    static int checked = 0;
    if (checked) return;

    checked = 1;

    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == INVALID_HANDLE_VALUE) return;

    DWORD mode = 0;
    if (!GetConsoleMode(output, &mode)) return;

    SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}
#endif

void client_net_log_packet(const char *direction, const NetPacketType type, const void *payload, const uint16_t length) {
    if (!client_net_packet_logging_enabled()) return;

#ifdef WIN32
    client_net_enable_ansi_output();
#endif

    const int incoming = direction && direction[0] == '<';
    const char *style = incoming ? "\x1b[48;2;255;106;25m\x1b[30m" : "\x1b[48;2;119;255;119m\x1b[30m";

    printf("%s %s \x1b[0m ", style, incoming ? "<=" : "=>");

    if (length && !payload) {
        client_net_log_bad_payload("null", type, payload, length);
        printf("\n");
        fflush(stdout);
        return;
    }

    switch (type) {
        case NET_PACKET_HELLO: {
            if (length != sizeof(NetHelloPacket)) {
                client_net_log_bad_payload("hello", type, payload, length);
                break;
            }

            NetHelloPacket packet;
            memcpy(&packet, payload, sizeof(packet));

            printf("{\"type\":\"hello\",\"id\":%u,\"major\":%u,\"minor\":%u,\"patch\":%u}",
                   (unsigned int) type,
                   (unsigned int) packet.major,
                   (unsigned int) packet.minor,
                   (unsigned int) packet.patch);
            break;
        }
        case NET_PACKET_INIT: {
            if (length != sizeof(NetInitPacket)) {
                client_net_log_bad_payload("init", type, payload, length);
                break;
            }

            NetInitPacket packet;
            memcpy(&packet, payload, sizeof(packet));

            printf("{\"type\":\"init\",\"id\":%u,\"major\":%u,\"minor\":%u,\"patch\":%u,\"map_index\":%d,\"mode_index\":%d,\"tick_rate\":%u,\"client_id\":%d}",
                   (unsigned int) type,
                   (unsigned int) packet.major,
                   (unsigned int) packet.minor,
                   (unsigned int) packet.patch,
                   packet.map_index,
                   packet.mode_index,
                   (unsigned int) packet.tick_rate,
                   packet.client_id);
            break;
        }
        case NET_PACKET_ENT: {
            if (length != NET_ENT_PACKET_V2_SIZE && length != NET_ENT_PACKET_MIN_SIZE && (length < NET_ENT_PACKET_V3_SIZE || length > sizeof(NetEntPacket))) {
                client_net_log_bad_payload("ent", type, payload, length);
                break;
            }

            NetEntPacket packet = {.secondary_index = -1, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1};
            memcpy(&packet, payload, length);

            printf("{\"type\":\"ent\",\"id\":%u,\"class_index\":%d,\"secondary_index\":%d,\"spectate\":%u,\"slid_cont\":%u}",
                   (unsigned int) type,
                   packet.class_index,
                   packet.secondary_index,
                   (unsigned int) (length > NET_ENT_PACKET_V3_SIZE ? packet.spectate : 0),
                   (unsigned int) (length > offsetof(NetEntPacket, slid_cont) ? packet.slid_cont : 1));
            break;
        }
        case NET_PACKET_SPAWN: {
            if (length != sizeof(NetSpawnPacket) && length != NET_SPAWN_PACKET_V1_SIZE) {
                client_net_log_bad_payload("spawn", type, payload, length);
                break;
            }

            NetSpawnPacket packet = {.cosmetics = PLAYER_COSMETICS_NONE};
            memcpy(&packet, payload, length);
            packet.name[sizeof(packet.name) - 1] = '\0';

            printf("{\"type\":\"spawn\",\"id\":%u,\"uid\":%d,\"is_you\":%u,\"active\":%u,\"class_index\":%d,\"team\":%d,\"health\":%g,\"max_health\":%d,\"name\":\"%s\",\"position\":[%g,%g,%g],\"direction\":[%g,%g],\"skin\":%d,\"hat\":%d,\"back\":%d,\"waist\":%d,\"face\":%d,\"dye\":%d}",
                   (unsigned int) type,
                   packet.uid,
                   (unsigned int) packet.is_you,
                   (unsigned int) packet.active,
                   packet.class_index,
                   packet.team,
                   packet.health,
                   packet.max_health,
                   packet.name,
                   packet.position[0],
                   packet.position[1],
                   packet.position[2],
                   packet.direction[0],
                   packet.direction[1],
                   packet.cosmetics.skin_color, packet.cosmetics.hat, packet.cosmetics.back,
                   packet.cosmetics.waist, packet.cosmetics.face, packet.cosmetics.dye);
            break;
        }
        case NET_PACKET_INPUT: {
            if (length != sizeof(NetInputPacket) && length != NET_INPUT_PACKET_V2_SIZE && length != NET_INPUT_PACKET_V1_SIZE) {
                client_net_log_bad_payload("input", type, payload, length);
                break;
            }

            NetInputPacket packet = {0};
            memcpy(&packet, payload, length);

            printf("{\"type\":\"input\",\"id\":%u,\"seq\":%d,\"move_dir\":%d,\"delta\":%g,\"x_dir\":%g,\"y_dir\":%g,\"flags\":%u,\"swap\":%u,\"slowdown\":%u,\"scroll\":%d}",
                   (unsigned int) type,
                   packet.seq,
                   packet.move_dir,
                   packet.delta,
                   packet.x_dir,
                   packet.y_dir,
                   (unsigned int) packet.flags,
                   (unsigned int) packet.swap,
                   (unsigned int) packet.slowdown,
                   (int) packet.scroll);
            break;
        }
        case NET_PACKET_STATE: {
            if (length != sizeof(NetStatePacket)) {
                client_net_log_bad_payload("state", type, payload, length);
                break;
            }

            NetStatePacket packet;
            memcpy(&packet, payload, sizeof(packet));

            printf("{\"type\":\"state\",\"id\":%u,\"uid\":%d,\"ack_seq\":%d,\"active\":%u,\"loadout_index\":%d,\"position\":[%g,%g,%g],\"velocity\":[%g,%g,%g],\"direction\":[%g,%g],\"health\":%g,\"aim_val\":%g,\"crouch_val\":%g}",
                   (unsigned int) type,
                   packet.uid,
                   packet.ack_seq,
                   (unsigned int) packet.active,
                   packet.loadout_index,
                   packet.position[0],
                   packet.position[1],
                   packet.position[2],
                   packet.velocity[0],
                   packet.velocity[1],
                   packet.velocity[2],
                   packet.direction[0],
                   packet.direction[1],
                   packet.health,
                   packet.aim_val,
                   packet.crouch_val);
            break;
        }
        case NET_PACKET_CYCLE:
            if (length) {
                client_net_log_bad_payload("cycle", type, payload, length);
                break;
            }

            printf("{\"type\":\"cycle\",\"id\":%u}", (unsigned int) type);
            break;
#define LOG_READ(T, name) \
            T packet; \
            if (length != sizeof(T)) { client_net_log_bad_payload(name, type, payload, length); break; } \
            memcpy(&packet, payload, sizeof(packet))
        case NET_PACKET_CHAT_SEND: {
            char text[NET_CHAT_TEXT_SIZE];
            const size_t n = length < sizeof(text) - 1 ? length : sizeof(text) - 1;
            memcpy(text, payload, n);
            text[n] = 0;
            printf("{\"type\":\"chat_send\",\"id\":%u,\"text\":\"%s\"}", (unsigned int) type, text);
            break;
        }
        case NET_PACKET_CHAT: {
            LOG_READ(NetChatPacket, "chat");
            packet.name[sizeof(packet.name) - 1] = 0;
            packet.text[sizeof(packet.text) - 1] = 0;
            printf("{\"type\":\"chat\",\"id\":%u,\"uid\":%d,\"kind\":%u,\"name\":\"%s\",\"text\":\"%s\"}",
                   (unsigned int) type, packet.uid, (unsigned int) packet.kind, packet.name, packet.text);
            break;
        }
        case NET_PACKET_KILL: {
            LOG_READ(NetKillPacket, "kill");
            printf("{\"type\":\"kill\",\"id\":%u,\"victim\":%d,\"killer\":%d,\"weapon\":%d,\"headshot\":%u,\"wallbang\":%u,\"deaths\":%d,\"kills\":%d,\"score\":%d}",
                   (unsigned int) type, packet.victim_uid, packet.killer_uid, packet.weapon_index, (unsigned int) packet.headshot,
                   (unsigned int) packet.wallbang, packet.victim_deaths, packet.killer_kills, packet.score);
            break;
        }
        case NET_PACKET_HIT: {
            LOG_READ(NetHitPacket, "hit");
            printf("{\"type\":\"hit\",\"id\":%u,\"target\":%d,\"damage\":%d,\"crit\":%u,\"kill\":%u,\"ai\":%u}",
                   (unsigned int) type, packet.target_uid, packet.damage, (unsigned int) packet.crit, (unsigned int) packet.kill, (unsigned int) packet.ai);
            break;
        }
        case NET_PACKET_HEALTH: {
            LOG_READ(NetHealthPacket, "health");
            printf("{\"type\":\"health\",\"id\":%u,\"uid\":%d,\"health\":%g,\"source\":%u}",
                   (unsigned int) type, packet.uid, packet.health, (unsigned int) packet.has_source);
            break;
        }
        case NET_PACKET_SCORE: {
            LOG_READ(NetScorePacket, "score");
            printf("{\"type\":\"score\",\"id\":%u,\"delta\":%d,\"score\":%d}", (unsigned int) type, packet.delta, packet.score);
            break;
        }
        case NET_PACKET_LEADERBOARD: {
            LOG_READ(NetLeaderboardPacket, "leaderboard");
            printf("{\"type\":\"leaderboard\",\"id\":%u,\"rows\":[", (unsigned int) type);
            for (int i = 0; i < packet.count && i < GAME_LEADERBOARD_MAX; i++) {
                printf("%s[%d,%d,%d,%d]", i ? "," : "", packet.entries[i].uid, packet.entries[i].score, packet.entries[i].kills, packet.entries[i].deaths);
            }
            printf("]}");
            break;
        }
        case NET_PACKET_TIMER: {
            LOG_READ(NetTimerPacket, "timer");
            printf("{\"type\":\"timer\",\"id\":%u,\"seconds\":%d,\"end\":%u}", (unsigned int) type, packet.seconds, (unsigned int) packet.end);
            break;
        }
        case NET_PACKET_END: {
            LOG_READ(NetEndPacket, "end");
            printf("{\"type\":\"end\",\"id\":%u,\"result\":%d,\"next_round\":%d,\"winner\":%d}", (unsigned int) type, packet.result, packet.next_round, packet.winner_uid);
            break;
        }
        case NET_PACKET_PING:
        case NET_PACKET_PONG: {
            LOG_READ(NetPingPacket, "ping");
            printf("{\"type\":\"%s\",\"id\":%u,\"seq\":%u}", type == NET_PACKET_PING ? "ping" : "pong", (unsigned int) type, (unsigned int) packet.seq);
            break;
        }
        case NET_PACKET_PING_RESULT: {
            LOG_READ(NetPingResultPacket, "ping_result");
            printf("{\"type\":\"ping_result\",\"id\":%u,\"ping\":%d}", (unsigned int) type, packet.ping_ms);
            break;
        }
        case NET_PACKET_LEAVE: {
            LOG_READ(NetLeavePacket, "leave");
            printf("{\"type\":\"leave\",\"id\":%u,\"uid\":%d}", (unsigned int) type, packet.uid);
            break;
        }
        case NET_PACKET_SHOT: {
            LOG_READ(NetShotPacket, "shot");
            printf("{\"type\":\"shot\",\"id\":%u,\"uid\":%d,\"weapon\":%d,\"hit\":[%g,%g,%g],\"normal\":%u}",
                   (unsigned int) type, packet.uid, packet.weapon_index, packet.hit[0], packet.hit[1], packet.hit[2], (unsigned int) packet.has_normal);
            break;
        }
        case NET_PACKET_PROJECTILE: {
            LOG_READ(NetProjectilePacket, "projectile");
            printf("{\"type\":\"projectile\",\"id\":%u,\"slot\":%d,\"owner\":%d,\"weapon\":%d,\"ai_kind\":%u,\"position\":[%g,%g,%g]}",
                   (unsigned int) type, packet.id, packet.owner_uid, packet.weapon_index, (unsigned int) packet.ai_kind, packet.position[0], packet.position[1], packet.position[2]);
            break;
        }
        case NET_PACKET_PROJECTILE_END: {
            LOG_READ(NetProjectileEndPacket, "projectile_end");
            printf("{\"type\":\"projectile_end\",\"id\":%u,\"slot\":%d,\"position\":[%g,%g,%g],\"impact\":%u}",
                   (unsigned int) type, packet.id, packet.position[0], packet.position[1], packet.position[2], (unsigned int) packet.impact);
            break;
        }
        case NET_PACKET_EXPLOSION: {
            LOG_READ(NetExplosionPacket, "explosion");
            printf("{\"type\":\"explosion\",\"id\":%u,\"position\":[%g,%g,%g],\"radius\":%g}",
                   (unsigned int) type, packet.position[0], packet.position[1], packet.position[2], packet.radius);
            break;
        }
        case NET_PACKET_AI_ADD: { // "aai": AI.getData
            LOG_READ(NetAIAddPacket, "ai_add");
            packet.name[sizeof(packet.name) - 1] = 0;
            packet.src[sizeof(packet.src) - 1] = 0;
            printf("{\"type\":\"ai_add\",\"id\":%u,\"sid\":%d,\"index\":%d,\"name\":\"%s\",\"position\":[%g,%g,%g]",
                   (unsigned int) type, packet.sid, packet.index, packet.name, packet.position[0], packet.position[1], packet.position[2]);
            if (packet.has_data) {
                printf(",\"data\":{\"health\":%g,\"mSize\":%g,\"asset\":%d,\"idl\":%d,\"src\":\"%s\"}", packet.max_health, packet.m_size,
                       packet.asset, packet.idle_sound, packet.src);
            }
            printf("}");
            break;
        }
        case NET_PACKET_AI_SYNC: { // "ai": [sid, x, y, z, xD, yD, health, tFrame] per AI
            NetAISyncPacket packet;
            memset(&packet, 0, sizeof(packet));
            const size_t header = offsetof(NetAISyncPacket, entries);
            if (length < header || length > sizeof(packet)) { client_net_log_bad_payload("ai_sync", type, payload, length); break; }
            memcpy(&packet, payload, length);
            if (packet.count > NET_AI_SYNC_MAX || length != header + packet.count * sizeof(NetAISyncEntry)) {
                client_net_log_bad_payload("ai_sync", type, payload, length);
                break;
            }
            printf("{\"type\":\"ai_sync\",\"id\":%u,\"first\":%u,\"ais\":[", (unsigned int) type, (unsigned int) packet.first);
            for (int i = 0; i < packet.count; i++) {
                const NetAISyncEntry *e = &packet.entries[i];
                printf("%s[%d,%g,%g,%g,%g,%g,%d,%u]", i ? "," : "", e->sid, e->position[0], e->position[1], e->position[2], e->x_dir, e->y_dir,
                       e->health, (unsigned int) e->frame);
            }
            printf("]}");
            break;
        }
        // map triggers (shared/src/triggers.c)
        case NET_PACKET_OBJECT_STATE: { // "do" / "ro" / "gte" / "bnk"
            static const char *const kinds[] = {"destroy", "respawn", "gate", "bank"};
            LOG_READ(NetObjectStatePacket, "object_state");
            printf("{\"type\":\"object_state\",\"id\":%u,\"uid\":%d,\"kind\":\"%s\",\"value\":%d}", (unsigned int) type, packet.uid,
                   packet.kind < 4 ? kinds[packet.kind] : "?", packet.value);
            break;
        }
        case NET_PACKET_CHECKPOINT: // "chp"
            if (length) { client_net_log_bad_payload("checkpoint", type, payload, length); break; }
            printf("{\"type\":\"checkpoint\",\"id\":%u}", (unsigned int) type);
            break;
        case NET_PACKET_SOUND: { // "s"
            LOG_READ(NetSoundPacket, "sound");
            packet.name[sizeof(packet.name) - 1] = 0;
            printf("{\"type\":\"sound\",\"id\":%u,\"name\":\"%s\",\"uid\":%d,\"position\":[%g,%g,%g],\"volume\":%g,\"rate\":%g}",
                   (unsigned int) type, packet.name, packet.uid, packet.position[0], packet.position[1], packet.position[2], packet.volume, packet.rate);
            break;
        }
        case NET_PACKET_JUMP_SCARE: { // "scr"
            LOG_READ(NetJumpScarePacket, "jump_scare");
            printf("{\"type\":\"jump_scare\",\"id\":%u,\"index\":%d}", (unsigned int) type, packet.index);
            break;
        }
        case NET_PACKET_POSITION: { // changePosition with doRecon
            LOG_READ(NetPositionPacket, "position");
            printf("{\"type\":\"position\",\"id\":%u,\"position\":[%g,%g,%g],\"stop_momentum\":%u,\"direction\":%d}", (unsigned int) type,
                   packet.x, packet.y, packet.z, (unsigned int) packet.stop_momentum, packet.has_direction ? packet.direction : -1);
            break;
        }
        case NET_PACKET_POPUP: { // "am" [text, null]
            LOG_READ(NetPopupPacket, "popup");
            packet.text[sizeof(packet.text) - 1] = 0;
            printf("{\"type\":\"popup\",\"id\":%u,\"text\":\"%s\"}", (unsigned int) type, packet.text);
            break;
        }
#undef LOG_READ
        default:
            client_net_log_bad_payload("unknown", type, payload, length);
            break;
    }

    printf("\n");
    fflush(stdout);
}

void client_net_queue_packet(Client *client, const NetPacket *packet) {
    pthread_mutex_lock(&client->net_lock);

    // The server explains a kick right before it closes the socket; the transport
    // reports the close before the main thread drains this queue, so keep the text
    // here for on_client_socket_disconnect().
    if (packet->type == NET_PACKET_ERROR && packet->length == sizeof(NetErrorPacket)) {
        NetErrorPacket error;
        memcpy(&error, packet->payload, sizeof(error));
        error.message[sizeof(error.message) - 1] = 0;
        error.code[sizeof(error.code) - 1] = 0;
        snprintf(client->net_disconnect_reason, sizeof(client->net_disconnect_reason), "%s", error.message);
        snprintf(client->net_disconnect_code, sizeof(client->net_disconnect_code), "%s", error.code[0] ? error.code : "000000");
    }

    NetPacket *new_packets = realloc(client->net_packets, (client->net_packet_count + 1) * sizeof(NetPacket));

    if (new_packets) {
        client->net_packets = new_packets;
        client->net_packets[client->net_packet_count++] = *packet;
    }

    pthread_mutex_unlock(&client->net_lock);
}
