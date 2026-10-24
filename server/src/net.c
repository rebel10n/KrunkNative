// The server core's connections (server.h): packet dispatch, spawns, map changes, chat
// commands and the per-tick broadcast. The bytes move through each connection's transport
// (ClientConnection.transport): TCP sockets (net_tcp.c), UDP peers (net_udp.c) or the
// in-process sandbox (sandbox.c); the TCP listener and the net thread live in net_tcp.c.
#include <prop_hunt.h> // Prop Hunt: "prp" / "prpR", the prop after a SPAWN
#include <stalker.h>   // Stalker: "stlkrt", the hidden stalker out of the state stream
#include <player_anim.h> // the "uai" of a spawn's onPlayerSpawn after its SPAWN
#include <server.h>
#include <server_log.h>
#include <server_accounts.h> // backend: JOIN, PLAYER_INFO, stats, billboards (doc/backend.md section 8)
#include <server_votekick.h> // the player list's vote kick / K.P.D call (NET_PACKET_EXT subtypes)
#include <server_trade.h>    // the live trade of Trade mode (NET_EXT_TRADE_*, int [5, sid]; doc/v10/trade.md)
#include <server_captcha.h>  // the in-game captcha's answer (NET_EXT_CAPTCHA_ANSWER, game_captcha.c)
#include <server_brain_events.h> // v10.0.3 warsClan after the INIT, map prize wheels ("int" 9, "cnfm" 2)
#include <brain.h>           // brain_enabled(): the brain owns matchmaking and the hub
#include <bot_banter.h>      // bot chat through kn_brain (GameHooks.bot_banter)
#include <ai.h>
#include <bots.h>            // Krunker v7.1.4 bot players (shared/src/bots.c)
#include <ks_server.h>       // KrunkScript server host (server/src/ks_server_host.c)
#include <ks_storage.h>      // WP6: trigger storage / KR reward hooks (server/src/ks_storage.c)
#include <ks_game.h>
#include <ks_entities.h>     // v10.0.3 entity replication: a joiner's entSeed / lo / ent / entAtt, the "l" tail
#include <game_config.h>    // WP-E: GAME_CONFIG / NETWORK_DATA, match votes, viewDis
#include <social_v7.h>      // WP-E part 2b: chat / voice channels, multi-spray, taunts
#include <comp_lobby.h>     // v7 comp lobby (lobbyPlayers): ranked matches and comp hosts
#include <net_ext.h>         // NET_PACKET_EXT (254): the v10+ envelope
#include <bot_path_net.h>    // KrunkNative: NET_EXT_KN_BOT_PATH for a spectator following a bot (net_broadcast_bot_paths)
#include <object_manager.h>  // KrunkNative Object Manager (spawn menu): ids 250-254
#include <flavor.h>          // Krunker Strike: the product flavor carried in INIT / HELLO (shared.h)
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pcg_basic.h>

#ifdef WIN32
#include <ws2tcpip.h> // inet_pton
#endif

// Client fds close through their transport (sockets, UDP peers, sandbox connections).
#define CLOSE_CLIENT_FD(client) ((client)->transport->close((client)->fd))

// Wall clock for ping RTTs: g_server->now only advances once per tick.
static double net_time() {
    return server_time();
}

static ClientConnection *net_client_by_id(const int id) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        if (g_server->clients[i].id == id) return &g_server->clients[i];
    }

    return NULL;
}

// ------------------------------------------------------------------ spectators
// Krunker v3.1.1 spectating (packetHandlers.ts io_ent with ent[6] = 1, ws.spectating):
// the connection has no Player, so it never spawns, scores or shows on the leaderboard,
// and it receives every player's state. Kept by connection id here; ClientConnection
// entries move when the array is compacted.
#define NET_MAX_SPECTATORS 64

static struct {
    int id;
    int target_uid; // "fpSp": the player it follows, -1 free cam (informational, like the v3.1.1 server)
    float bot_path_at; // KrunkNative: game time of the last NET_EXT_KN_BOT_PATH sent to it (net_broadcast_bot_paths)
    float sent_bar[NET_INTER_BAR_COUNT]; // the followed player's interact / charge bar as last sent (net_broadcast_inter_progress)
} g_spectators[NET_MAX_SPECTATORS];
static int g_spectator_count;

static int net_spectator_index(const int id) {
    for (int i = 0; i < g_spectator_count; i++) {
        if (g_spectators[i].id == id) return i;
    }

    return -1;
}

static int net_is_spectating(const ClientConnection *client) {
    return client && net_spectator_index(client->id) >= 0;
}

static void net_set_spectating(const ClientConnection *client, const int on) {
    const int index = net_spectator_index(client->id);

    if (on && index < 0 && g_spectator_count < NET_MAX_SPECTATORS) {
        g_spectators[g_spectator_count].id = client->id;
        g_spectators[g_spectator_count].target_uid = -1;
        g_spectators[g_spectator_count].bot_path_at = -1.0f;
        memset(g_spectators[g_spectator_count].sent_bar, 0, sizeof(g_spectators[g_spectator_count].sent_bar)); // a new client shows none
        g_spectator_count++;
    } else if (!on && index >= 0) {
        g_spectators[index] = g_spectators[--g_spectator_count];
    }
}

static void net_send_chat_to(ClientConnection *client, const int uid, const NetChatKind kind, const char *name, const char *text) {
    NetChatPacket packet;
    net_packet_chat(&packet, uid, kind, name, text);
    net_send_packet(client->fd, NET_PACKET_CHAT, &packet, sizeof(packet));
}

static void net_broadcast_chat_except(const ClientConnection *skip, const int uid, const NetChatKind kind, const char *name, const char *text) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        if (client != skip) net_send_chat_to(client, uid, kind, name, text);
    }
}

// A server line about player `uid` (config.serverMessages: "$" in `template` = `name`) to one connection, in
// the form it reads: an EXT peer gets NET_EXT_KN_SERVER_LINE (the name apart from the template; its client
// formats the line and shows the name as it shows that player everywhere else, Hide player names), an older
// peer the composed NET_CHAT_SERVER line (uid -1, no name: what every peer got before).
static void net_send_server_line_to(ClientConnection *to, const int uid, const char *name, const char *template) {
    if (to->peer_minor >= NET_PROTOCOL_MINOR_EXT) {
        NetServerLinePacket body;
        memset(&body, 0, sizeof(body));
        body.uid = uid;
        snprintf(body.name, sizeof(body.name), "%s", name ? name : "");
        snprintf(body.text, sizeof(body.text), "%s", template ? template : "");
        uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(body)];
        const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_KN_SERVER_LINE, &body, sizeof(body));
        if (n) net_send_packet(to->fd, NET_PACKET_EXT, buf, n);
        return;
    }
    char text[NET_CHAT_TEXT_SIZE];
    net_format_server_line(text, sizeof(text), template, name);
    net_send_chat_to(to, -1, NET_CHAT_SERVER, NULL, text);
}

static void net_shown_name(const ClientConnection *to, const ClientConnection *subject, const char *real, char *out, size_t size);

// A server line about the player `uid` (real name `real`, its connection `subject`, NULL once it has left) to
// everyone, or to everyone but the subject: each recipient gets the name as it may see it (a ranked warmup masks
// the others' names, net_shown_name) and the form its protocol reads (net_send_server_line_to).
static void net_broadcast_about(const ClientConnection *subject, const int uid, const char *real, const char *template, const int skip_subject) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        if (skip_subject && client == subject) continue;
        char name[PLAYER_NAME_SIZE];
        net_shown_name(client, subject, real, name, sizeof(name));
        net_send_server_line_to(client, uid, name, template);
    }
}

// config.serverMessages[type] about `subject` to everyone else (broadcastServerMessage in the v3.1.1 server).
// subject->id is its player's uid (GameHooks.send).
static void net_broadcast_server_message(const ClientConnection *subject, const char *template) {
    net_broadcast_about(subject, subject->id, subject->name, template, 1);
}

void net_broadcast_player_line(const int uid, const char *real, const char *template) {
    net_broadcast_about(net_client_by_id(uid), uid, real, template, 0);
}

void net_server_line_to(ClientConnection *to, const int uid, const char *name, const char *template) {
    net_send_server_line_to(to, uid, name, template);
}

// A server line naming several players (game_send_server_names) to one connection, in the form it reads. An EXT peer gets
// NET_EXT_KN_SERVER_NAMES: each name by its uid, as this recipient may see it (net_shown_name, the ranked warmup mask; a
// name with no uid or not a player goes as it is), and the client formats the template. An older peer gets the composed
// NET_CHAT_SERVER line with the same names.
static void net_send_server_names_to(ClientConnection *to, const int count, const ServerName *names, const char *template) {
    const int n = count < 0 ? 0 : count > NET_SERVER_NAMES_MAX ? NET_SERVER_NAMES_MAX : count;
    char shown[NET_SERVER_NAMES_MAX][PLAYER_NAME_SIZE];
    for (int i = 0; i < n; i++) {
        const int uid = names[i].uid;
        const ClientConnection *subject = uid >= 0 ? net_client_by_id(uid) : NULL;
        net_shown_name(to, subject, names[i].name, shown[i], sizeof(shown[i]));
    }
    if (to->peer_minor >= NET_PROTOCOL_MINOR_EXT) {
        NetServerNamesPacket body;
        memset(&body, 0, sizeof(body));
        body.count = (uint8_t) n;
        for (int i = 0; i < n; i++) {
            body.names[i].uid = names[i].uid >= 0 || names[i].uid == NET_SERVER_NAME_RAW ? names[i].uid : NET_SERVER_NAME_NONE;
            snprintf(body.names[i].name, sizeof(body.names[i].name), "%s", shown[i]);
        }
        snprintf(body.text, sizeof(body.text), "%s", template ? template : "");
        uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(body)];
        const uint16_t len = net_ext_pack(buf, sizeof(buf), NET_EXT_KN_SERVER_NAMES, &body, sizeof(body));
        if (len) net_send_packet(to->fd, NET_PACKET_EXT, buf, len);
        return;
    }
    const char *texts[NET_SERVER_NAMES_MAX];
    for (int i = 0; i < n; i++) texts[i] = shown[i];
    char text[NET_CHAT_TEXT_SIZE];
    net_format_server_names(text, sizeof(text), template, texts, n);
    net_send_chat_to(to, -1, NET_CHAT_SERVER, NULL, text);
}

// GameHooks.server_line: a bot's join / leave line (shared/src/bots.c), `target` as net_game_send takes it.
static void net_game_server_line(Game *game, const int target, const int uid, const char *name, const char *template) {
    (void) game;
    if (target <= NET_TARGET_ALL) {
        const int skip = target <= NET_TARGET_EXCEPT(0) ? NET_TARGET_EXCEPT_UID(target) : -1;
        for (size_t i = 0; i < g_server->client_count; i++) {
            ClientConnection *client = &g_server->clients[i];
            if (target == NET_TARGET_NON_PLAYERS && client->player) continue;
            if (skip >= 0 && client->id == skip) continue;
            net_send_server_line_to(client, uid, name, template);
        }
        return;
    }
    ClientConnection *client = net_client_by_id(target);
    if (client) net_send_server_line_to(client, uid, name, template);
}

// GameHooks.server_names: a line naming players (chat lists, a finish line), `target` as net_game_send takes it.
static void net_game_server_names(Game *game, const int target, const int count, const ServerName *names, const char *template) {
    (void) game;
    if (target <= NET_TARGET_ALL) {
        const int skip = target <= NET_TARGET_EXCEPT(0) ? NET_TARGET_EXCEPT_UID(target) : -1;
        for (size_t i = 0; i < g_server->client_count; i++) {
            ClientConnection *client = &g_server->clients[i];
            if (target == NET_TARGET_NON_PLAYERS && client->player) continue;
            if (skip >= 0 && client->id == skip) continue;
            net_send_server_names_to(client, count, names, template);
        }
        return;
    }
    ClientConnection *client = net_client_by_id(target);
    if (client) net_send_server_names_to(client, count, names, template);
}

// A bot's line naming other players (game_send_chat_names) to one connection, in the form it reads. An EXT peer gets
// NET_EXT_KN_CHAT_NAMES: the bot by its uid and name, and the names by uid as this recipient may see them (net_shown_name,
// as net_send_server_names_to does); the client formats the template. An older peer gets the bot's composed NET_PACKET_CHAT line.
static void net_send_chat_names_to(ClientConnection *to, const int speaker, const char *speaker_name, const int count,
                                   const ServerName *names, const char *template) {
    const int n = count < 0 ? 0 : count > NET_CHAT_NAMES_MAX ? NET_CHAT_NAMES_MAX : count;
    char shown[NET_SERVER_NAMES_MAX][PLAYER_NAME_SIZE];
    for (int i = 0; i < n; i++) {
        const int uid = names[i].uid;
        const ClientConnection *subject = uid >= 0 ? net_client_by_id(uid) : NULL;
        net_shown_name(to, subject, names[i].name, shown[i], sizeof(shown[i]));
    }
    if (to->peer_minor >= NET_PROTOCOL_MINOR_EXT) {
        NetChatNamesPacket body;
        memset(&body, 0, sizeof(body));
        body.sender.uid = speaker;
        snprintf(body.sender.name, sizeof(body.sender.name), "%s", speaker_name ? speaker_name : "");
        body.count = (uint8_t) n;
        for (int i = 0; i < n; i++) {
            body.names[i].uid = names[i].uid >= 0 || names[i].uid == NET_SERVER_NAME_RAW ? names[i].uid : NET_SERVER_NAME_NONE;
            snprintf(body.names[i].name, sizeof(body.names[i].name), "%s", shown[i]);
        }
        snprintf(body.text, sizeof(body.text), "%s", template ? template : "");
        uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(body)];
        const uint16_t len = net_ext_pack(buf, sizeof(buf), NET_EXT_KN_CHAT_NAMES, &body, sizeof(body));
        if (len) net_send_packet(to->fd, NET_PACKET_EXT, buf, len);
        return;
    }
    const char *texts[NET_SERVER_NAMES_MAX];
    for (int i = 0; i < n; i++) texts[i] = shown[i];
    char text[NET_CHAT_TEXT_SIZE];
    net_format_server_names(text, sizeof(text), template, texts, n);
    NetChatPacket chat;
    net_packet_chat(&chat, speaker, NET_CHAT_PLAYER, speaker_name, text);
    net_send_packet(to->fd, NET_PACKET_CHAT, &chat, sizeof(chat));
}

// GameHooks.chat_names: a bot's line naming other players, to every connection (a bot's chat goes to everyone, as game_send does).
static void net_game_chat_names(Game *game, const int speaker, const char *speaker_name, const int count, const ServerName *names,
                                const char *template) {
    (void) game;
    for (size_t i = 0; i < g_server->client_count; i++)
        net_send_chat_names_to(&g_server->clients[i], speaker, speaker_name, count, names, template);
}

void net_broadcast_chat(const char *from, const char *msg) {
    char text[NET_CHAT_TEXT_SIZE];
    if (!net_chat_sanitize(text, sizeof(text), msg, msg ? strlen(msg) : 0)) return;

    net_broadcast_chat_except(NULL, -1, NET_CHAT_SERVER, from, text);
}

// The brain's ranked notice ("ranked-notice"): a line for every player of this game server. An EXT peer gets
// NET_EXT_KN_RANKED_NOTICE (the queued player apart from the template, so its client shows the name as Hide player names
// does); an older peer the composed NET_CHAT_RANKED line with the name, as before.
void net_broadcast_ranked_notice(const char *name, const char *template) {
    NetRankedNoticePacket body;
    memset(&body, 0, sizeof(body));
    snprintf(body.name, sizeof(body.name), "%s", name ? name : "");
    snprintf(body.text, sizeof(body.text), "%s", template ? template : "");
    char composed[NET_CHAT_TEXT_SIZE];
    net_format_server_line(composed, sizeof(composed), body.text, body.name);
    char text[NET_CHAT_TEXT_SIZE];
    if (!net_chat_sanitize(text, sizeof(text), composed, strlen(composed))) return;

    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(body)];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_KN_RANKED_NOTICE, &body, sizeof(body));
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        if (client->peer_minor >= NET_PROTOCOL_MINOR_EXT) {
            if (n) net_send_packet(client->fd, NET_PACKET_EXT, buf, n);
        } else {
            net_send_chat_to(client, -1, NET_CHAT_RANKED, NULL, text);
        }
    }
}

static char g_game_announcement[NET_GAME_ANNOUNCEMENT_TEXT];
static int g_game_ranked_queue;                    // the card's ranked-queue action (announce ranked-queue)
static char g_game_ranked_label[NET_GAME_RANKED_LABEL];

const char *net_game_announcement(void) { return g_game_announcement; }

void net_send_game_announcement(ClientConnection *client) {
    if (!client || client->peer_minor < NET_PROTOCOL_MINOR_EXT) return; // EXT peers only
    NetGameAnnouncementPacket body;
    memset(&body, 0, sizeof(body));
    snprintf(body.message, sizeof(body.message), "%s", g_game_announcement);
    body.ranked_queue = g_game_ranked_queue != 0;
    snprintf(body.ranked_label, sizeof(body.ranked_label), "%s", g_game_ranked_label);
    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(body)];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_GAME_ANNOUNCEMENT, &body, (uint16_t) sizeof(body));
    if (n) net_send_packet(client->fd, NET_PACKET_EXT, buf, n);
}

void net_set_game_announcement(const char *msg) {
    // control characters become spaces; cut at a UTF-8 boundary to fit the packet
    size_t n = 0;
    for (const char *p = msg ? msg : ""; *p && n + 1 < sizeof(g_game_announcement); p++)
        g_game_announcement[n++] = (unsigned char) *p < 0x20 || *p == 0x7f ? ' ' : *p;
    while (n > 0 && msg[n] && ((unsigned char) msg[n] & 0xC0) == 0x80) n--; // never half a character
    g_game_announcement[n] = 0;
    for (size_t i = 0; i < g_server->client_count; i++) net_send_game_announcement(&g_server->clients[i]);
}

// announce ranked-queue <label> (NULL or "" clears it): the card's ranked-queue action, sent to everyone now
void net_set_game_ranked_queue(const char *label) {
    g_game_ranked_queue = label && *label;
    size_t n = 0;
    for (const char *p = label ? label : ""; *p && n + 1 < sizeof(g_game_ranked_label); p++)
        g_game_ranked_label[n++] = (unsigned char) *p < 0x20 || *p == 0x7f ? ' ' : *p;
    g_game_ranked_label[n] = 0;
    for (size_t i = 0; i < g_server->client_count; i++) net_send_game_announcement(&g_server->clients[i]);
}

// Game hooks (GameHooks.send): player uid == ClientConnection.id.
// WP-D: the v7 mode packets (NET_PACKET_BLOCK_MODES, 140-149) only go to peers that announced
// NET_PROTOCOL_MINOR_V7 in their HELLO; so do map-logic WP-H's zombies / airdrop packets. WP-E: the lobby packets (NET_PACKET_BLOCK_LOBBY, 180-189:
// match votes, network data, game config) too.
static int net_peer_takes(const ClientConnection *client, const int type) {
    if (!game_ai_packet_for_peer(type, client->peer_minor)) return 0; // v3.1.1 vs v7 AI packets (shared/src/ai.c)
    if (type >= NET_PACKET_BLOCK_LOBBY && type < NET_PACKET_BLOCK_LOBBY + 10) return client->peer_minor >= NET_PROTOCOL_MINOR_V7;
    // map-logic WP-H: zombies (150-159) and airdrop (177-179) packets, v7 peers only
    if ((type >= NET_PACKET_BLOCK_ZOMBIES && type < NET_PACKET_BLOCK_ZOMBIES + 10) ||
        (type >= NET_PACKET_BLOCK_AIRDROP && type < NET_PACKET_BLOCK_LOBBY)) return ks_server_peer_v7(client);
    // player-viewer WP-E: the HUD / death screen / social packets (NET_PACKET_BLOCK_SOCIAL, 240-249)
    if (type >= NET_PACKET_BLOCK_SOCIAL && type < NET_PACKET_BLOCK_SOCIAL + 10) return ks_server_peer_v7(client);
    // NET_PACKET_EXT (254, net_ext.h): peers that announced 1.4
    if (type == NET_PACKET_EXT) return client->peer_minor >= NET_PROTOCOL_MINOR_EXT;
    // KrunkNative Object Manager (NET_PACKET_BLOCK_SANDBOX, 250-253): v7 peers only
    if (type >= NET_PACKET_BLOCK_SANDBOX && type < NET_PACKET_BLOCK_END) return ks_server_peer_v7(client);
    // WP-E part 2b: v7 peers get every spray as NET_PACKET_SPRAY_V7 (multi-spray), older ones the v3 "sp"
    if (type == NET_PACKET_SPRAY) return !ks_server_peer_v7(client);
    return type < NET_PACKET_BLOCK_MODES || type >= NET_PACKET_BLOCK_MODES + 10 || ks_server_peer_v7(client);
}

// ------------------------------------------------------------------ live-world filter
// Krunker v7.1.4 sends the live world only to `player.active || player.spectating`: the "k"
// player sync (game.js updateState, which also skips player.locked), the "9" tracers
// (players.js), "pr" projectile launches (projectiles.js), "ex" explosions and "s" broadcast
// sounds (game.js explosion / playSound), "ana" object and AI clips (game.js playAnim, AI.js).
// A dead player (death screen and menu until its next spawn) and a connection that never
// spawned get none of them; everything else (kill feed "3", chat, "h" health, "pre"
// projectile ends, leaderboard, timer, objectives, spawns / leaves, "end") still goes to
// every connection. Every broadcast below and the STATE sync go through this one filter.
// Kept for everyone on purpose (KrunkNative differences): SPAWN goes out at once where v7 defers
// a player's "0" until the viewer is live (players.sync sentTo), and GRAPPLE (v7's "k" grapple
// slot) is sent as changes only, so a release missed on the death screen would leave a stale
// rope after the respawn. Both are a few bytes per event.
static int net_live_only_packet(const int type) {
    switch (type) {
        case NET_PACKET_STATE:       // "k"
        case NET_PACKET_SHOT:        // "9"
        case NET_PACKET_PROJECTILE:  // "pr" (resent on spawn: net_resync_live_world)
        case NET_PACKET_AI_SHOT:     // "pr" of an AI (the AI half of the same launch)
        case NET_PACKET_EXPLOSION:   // "ex"
        case NET_PACKET_SOUND:       // "s" of game.playSound (a trigger's sound sent to one player is not filtered, triggers.js "s" / "so")
        case NET_PACKET_OBJECT_ANIM: // "ana" isObj
        case NET_PACKET_AI_ANIM:     // "ana" of an AI
            return 1;
        default:
            return 0;
    }
}

// v7 `player.active || player.spectating`
static int net_client_in_live_world(const ClientConnection *client) {
    return net_is_spectating(client) || (client->player && client->player->active);
}

// Does the broadcast of `type` reach `client`? Live-world packets only reach a connection in the
// live world. A held-back PROJECTILE is remembered (missed_projectiles) and resent when the
// connection spawns: KrunkNative difference, v7 never resends a "pr", so a rocket fired during
// the death screen flew on invisible after the respawn.
static int net_recipient_takes(ClientConnection *client, const int type, const void *payload, const uint16_t length) {
    int slot = -1;
    if ((type == NET_PACKET_PROJECTILE && length == sizeof(NetProjectilePacket)) ||
        (type == NET_PACKET_PROJECTILE_END && length == sizeof(NetProjectileEndPacket))) {
        int32_t id; // first field of both
        memcpy(&id, payload, sizeof(id));
        if (id >= 0 && id < GAME_MAX_PROJECTILES) slot = id;
    }
    if (slot >= 0) client->missed_projectiles[slot / 64] &= ~(UINT64_C(1) << (slot % 64)); // a launch it sees, or the slot's end
    if (!net_live_only_packet(type) || net_client_in_live_world(client)) return 1;
    if (slot >= 0 && type == NET_PACKET_PROJECTILE) client->missed_projectiles[slot / 64] |= UINT64_C(1) << (slot % 64);
    return 0;
}

// On spawn: the projectiles still in flight that the connection missed while it was out of the
// live world, from their current position, velocity and remaining range (AI shots with their
// shooter). Transient events (tracers, explosions, sounds, clips) are not replayed, like v7;
// the player positions follow with the next STATE (the client snaps on spawn).
static void net_resync_live_world(ClientConnection *client) {
    uint64_t missed[sizeof(client->missed_projectiles) / sizeof(client->missed_projectiles[0])];
    memcpy(missed, client->missed_projectiles, sizeof(missed));
    memset(client->missed_projectiles, 0, sizeof(client->missed_projectiles));
    for (int slot = 0; slot < GAME_MAX_PROJECTILES; slot++) {
        const GameProjectile *p = &g_server->game.projectiles[slot];
        if (!(missed[slot / 64] & (UINT64_C(1) << (slot % 64))) || !p->active || !net_peer_takes(client, NET_PACKET_PROJECTILE)) continue;
        const int bomb = p->streak == STREAK_BOMBARDMENT + 1; // a Bombardment bomb (game_spawn_bomb): no owner on the wire
        const NetProjectilePacket packet = {
            .id = p->id,
            .owner_uid = bomb ? -1 : p->owner_uid,
            .weapon_index = p->weapon_index,
            .position = {p->position.x, p->position.y, p->position.z},
            .velocity = {p->velocity.x, p->velocity.y, p->velocity.z},
            .gravity = p->config.gravity,
            .range = p->range,
            .ai_kind = (uint8_t) p->ai_kind,
            .bomb = (uint8_t) bomb,
        };
        net_send_packet(client->fd, NET_PACKET_PROJECTILE, &packet, sizeof(packet));
        const GameAIShot *shot = g_server->game.ai ? &g_server->game.ai->shots[slot] : NULL;
        if (p->ai_kind && shot && shot->used && net_peer_takes(client, NET_PACKET_AI_SHOT)) {
            const NetAIShotPacket ai_shot = {.projectile = p->id, .sid = shot->sid};
            net_send_packet(client->fd, NET_PACKET_AI_SHOT, &ai_shot, sizeof(ai_shot));
        }
    }
}

// NetKillPacket.fists NET_KILL_NUKE: protocol 1.x.2 peers (and any later minor)
static int net_peer_takes_kill_nuke(const ClientConnection *client) {
    return client->peer_minor > NET_PROTOCOL_MINOR_EXT || client->peer_patch >= NET_PROTOCOL_PATCH_KILL_NUKE;
}

// A game packet to one connection: the sound packets in their V1 layout (24-byte names) to a peer before patch 7,
// which never gets a sound whose name does not fit them (net_sound_packet_to_v1).
static void net_send_game_packet(const ClientConnection *client, const int type, const void *payload, const uint16_t length) {
    if ((type == NET_PACKET_SOUND || type == NET_PACKET_STOP_SOUND || type == NET_PACKET_PLAYER_SOUND) &&
        !net_peer_has_sound_v2(client->peer_minor, client->peer_patch)) {
        uint8_t v1[NET_SOUND_PACKET_V1_MAX];
        const uint16_t n = net_sound_packet_to_v1(type, payload, length, v1);
        if (n) net_send_packet(client->fd, (NetPacketType) type, v1, n);
        return;
    }
    net_send_packet(client->fd, (NetPacketType) type, payload, length);
}

static void net_game_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;

    if (uid <= NET_TARGET_ALL) {
        const int skip = uid <= NET_TARGET_EXCEPT(0) ? NET_TARGET_EXCEPT_UID(uid) : -1;

        for (size_t i = 0; i < g_server->client_count; i++) {
            ClientConnection *client = &g_server->clients[i];
            if (uid == NET_TARGET_NON_PLAYERS && client->player) continue;
            if (skip >= 0 && client->id == skip) continue;
            if (!net_peer_takes(client, type)) continue;
            if (!net_recipient_takes(client, type, payload, length)) continue; // v7: the live world only to the living
            if (type == NET_PACKET_KILL && length == sizeof(NetKillPacket) && !net_peer_takes_kill_nuke(client)) {
                NetKillPacket kill; // a pre-patch-2 peer reads any non-zero fists as a fists kill
                memcpy(&kill, payload, sizeof(kill));
                kill.fists &= NET_KILL_FISTS;
                net_send_packet(client->fd, (NetPacketType) type, &kill, sizeof(kill));
                continue;
            }
            net_send_game_packet(client, type, payload, length);
        }
        return;
    }

    ClientConnection *client = net_client_by_id(uid);
    if (client && type == NET_PACKET_END && length == sizeof(NetEndPacket)) accounts_on_end_packet(uid, payload); // didWin for the commit
    if (client && net_peer_takes(client, type)) net_send_game_packet(client, type, payload, length);
}

static void net_change_map_internal(int map_index, int mode_index, int respawn_joined);

// GameHooks.next_round: the end screen ran out, start the next round on the voted match
// (WP-E game_next_match: endData.mts tally, no votes = a random pick) and respawn everyone who
// played. No picks: game_init picks a random playable rotation pair (game_pick_rotation_pair).
static void net_game_next_round(Game *game) {
    int map_index, mode_index;
    if (!game_next_match(game, &map_index, &mode_index)) map_index = mode_index = -1;
    net_change_map_internal(map_index, mode_index, 1);
}

static ClientConnection *net_client_by_id(int id);
static void net_spawn_client(ClientConnection *client);
static ObjectManagerPerms net_om_perms(const ClientConnection *client);

// GameHooks.reward: player.reward at endGame (accounts only, server/src/accounts.c).
static int net_game_reward(Game *game, const Player *player) {
    (void) game;
    return accounts_reward(player);
}

// GameHooks.can_use_skin: v10 players.spray's canUseSkin, the coordinator's inventory (accounts.c).
static int net_game_can_use_skin(const Game *game, const Player *player, const int skin) {
    (void) game;
    return accounts_can_use_skin(player, skin);
}

// GameHooks.progress (WP-E part 2c): v7 incStat / challenge events of account players (accounts.c).
static void net_game_progress(Game *game, const Player *player, const int stat, const char *key, const int value, const int weapon,
                              const int streak, const int item) {
    (void) game;
    accounts_progress(player, stat, key, value, weapon, streak, item);
}

// GameHooks.junk_credit (Junk wallet): v7 game.sendUserJunk of an account player (accounts.c).
static void net_game_junk_credit(Game *game, const Player *player, const int64_t hundredths, const char *reason) {
    (void) game;
    accounts_junk_credit(player, hundredths, reason);
}

// GameHooks.give_skin (season-system): v7 game.giveSkin of an account player (accounts.c).
static void net_game_give_skin(Game *game, const Player *player, const int skin, const char *reason) {
    (void) game;
    accounts_give_skin(player, skin, reason);
}

// NET_EXT_KN_STRIKE_KILL_FLAG (net_ext.h): the killer's Country Flag for the victim's Krunker Strike
// kill card (FRVR killCardInfo.flag). Only the victim's card shows it, so only the victim's Strike
// connection gets it, before the KILL that player_kill sends after this hook.
static void net_send_strike_kill_flag(const Player *victim, const Player *killer) {
    if (!victim || !killer || killer == victim) return;
    ClientConnection *client = net_client_by_id(victim->uid);
    if (!client || client->peer_flavor != FLAVOR_STRIKE || !net_peer_takes(client, NET_PACKET_EXT)) return;
    const ClientConnection *killer_client = net_client_by_id(killer->uid); // NULL for a bot: no flag
    const NetStrikeKillFlagPacket body = {
        .killer_uid = killer->uid,
        .flag = (int16_t) (killer_client ? killer_client->acct.flag : -1),
    };
    uint8_t buffer[NET_EXT_HEADER_SIZE + sizeof(body)];
    const uint16_t n = net_ext_pack(buffer, sizeof(buffer), NET_EXT_KN_STRIKE_KILL_FLAG, &body, sizeof(body));
    if (n) net_send_packet(client->fd, NET_PACKET_EXT, buffer, n);
}

// GameHooks.kill: v10 Turf Wars (accounts.c); the Strike kill card's flag rides before the KILL
static void net_game_kill(Game *game, const Player *victim, const Player *killer) {
    (void) game;
    accounts_on_kill(victim, killer);
    net_send_strike_kill_flag(victim, killer);
}

// GameHooks.respawn: a mode respawns a dead player itself (forceSpawn, Krunker "frs").
static void net_game_respawn(Game *game, const int uid) {
    (void) game;
    ClientConnection *client = net_client_by_id(uid);
    if (client && client->player && !client->player->active && g_server->game.end_timer <= 0.0f) net_spawn_client(client);
}

// GameHooks.alloc_uid: a bot player's uid comes from the connection id counter, so no
// connection ever gets the same one.
static int net_game_alloc_uid(Game *game) {
    (void) game;
    return g_server->next_client_id++;
}

static int net_alloc_player_number() {
    const unsigned short limit = g_server->config.max_players ? g_server->config.max_players : 8;

    for (int i = 1; i <= limit && i < 32; i++) {
        const unsigned int bit = 1u << i;
        if (g_server->used_player_numbers & bit) continue;

        g_server->used_player_numbers |= bit;
        return i;
    }

    return 0;
}

static void net_release_player_number(ClientConnection *client) {
    if (!client || client->player_number <= 0 || client->player_number >= 32) return;

    g_server->used_player_numbers &= ~(1u << client->player_number);
    client->player_number = 0;
}

static void net_assign_guest_name(ClientConnection *client) {
    if (!client) return;

    snprintf(client->name, sizeof(client->name), "Guest_%d", client->player_number);
}

// A live sign-out (accounts.c): the connection's guest name again (the next spawn's)
void net_client_guest_name(ClientConnection *client) {
    net_assign_guest_name(client);
}

static void net_free_player(Player *player) {
    if (!player) return;

    free(player->loadout);
    free(player->ammo);
    free(player->reloads);
    free(player->input_queue);
    free(player);
}

static void net_remove_game_player(Player *player) {
    if (!player) return;

    size_t index = SIZE_MAX;

    for (size_t i = 0; i < g_server->game.player_count; i++) {
        if (g_server->game.players[i] == player) {
            index = i;
            break;
        }
    }

    if (index == SIZE_MAX) {
        net_free_player(player);
        return;
    }

    if (index + 1 < g_server->game.player_count) {
        memmove(&g_server->game.players[index], &g_server->game.players[index + 1], (g_server->game.player_count - index - 1) * sizeof(Player*));
    }

    g_server->game.player_count--;

    if (!g_server->game.player_count) {
        free(g_server->game.players);
        g_server->game.players = NULL;
    } else {
        Player **new_players = realloc(g_server->game.players, g_server->game.player_count * sizeof(Player*));
        if (new_players) g_server->game.players = new_players;
    }

    net_free_player(player);
}

// What the server itself saw the player do this life: shots fired (hitscan rays and the stream
// count life_shots, projectiles and thrown knives shot_count), damage dealt and damage taken.
static int net_idle_shots(const Player *player) { return player->life_shots + (int) player->shot_count; }

static void net_idle_note_actions(ClientConnection *client, const Player *player) {
    client->idle_shots = net_idle_shots(player);
    client->idle_damage_dealt = player->damage_dealt;
    client->idle_health = player->health;
}

static void net_mark_client_spawned(ClientConnection *client) {
    if (!client || !client->player) return;

    client->spawned_at = g_server->now;
    client->last_significant_move_at = g_server->now;
    client->idle_position = client->player->position;
    client->has_idle_position = 1;
    client->has_idle_look = 0; // the new life's first input frame is the look reference
    net_idle_note_actions(client, client->player);
}

// The idle clock restarts: the player did something (net_check_idle_clients).
static void net_note_activity(ClientConnection *client) {
    client->last_significant_move_at = g_server->now;
}

// An accepted input frame is activity when anything in it is held or pressed (a move key, fire,
// aim, jump, crouch, reload, interact, reset, a weapon swap, the wheel, a slot pick), or when the
// view turned away from the look of the last activity. Only empty frames with the same look (the
// client keeps sending them while nobody touches it) are not: v7.1.4's own idle timer
// (controls.js) resets on every key event and every mouse move.
// 11d36be compared each frame with the one before it, so a held trigger, aim or crouch with the
// mouse still (identical frames) never counted, and neither did a slow turn of less than 1e-4
// rad per frame (low or aim sensitivity, fractional browser mouse deltas): players who stood still
// and shot were kicked mid-fight. The look is now compared with the look of the last activity,
// so a slow turn adds up.
static void net_note_input_activity(ClientConnection *client, const Input *input) {
    const int held = input->move_dir >= 0 || input->shoot || input->scope || input->jump || input->crouch || input->reload ||
                     input->interact_held || input->reset || input->swap || input->scroll || input->select_slot;
    const int turned = client->has_idle_look && (fabsf(input->x_dir - client->idle_look[0]) > 1e-4f ||
                                                 fabsf(input->y_dir - client->idle_look[1]) > 1e-4f);
    if (held || turned) net_note_activity(client);
    if (held || turned || !client->has_idle_look) {
        client->has_idle_look = 1;
        client->idle_look[0] = input->x_dir;
        client->idle_look[1] = input->y_dir;
    }
}

// One tick's frames for one client go out in a single send(): Krunker sends its per-tick
// sync as one ws message, and ~one send() per player per client per tick (with
// net_send_packet() spinning on EAGAIN) held g_server->lock for milliseconds at full lobbies.
#define NET_BATCH_SIZE 8192

typedef struct {
    size_t length;
    uint8_t data[NET_BATCH_SIZE];
} NetSendBatch;

static int net_batch_append(NetSendBatch *batch, const NetPacketType type, const void *payload, const uint16_t length) {
    if (batch->length + NET_PACKET_HEADER_SIZE + length > sizeof(batch->data)) return 0;
    uint8_t *frame = batch->data + batch->length;
    frame[0] = (uint8_t) type;
    frame[1] = (uint8_t) (length >> 8);
    frame[2] = (uint8_t) (length & 0xff);
    if (payload && length) memcpy(frame + NET_PACKET_HEADER_SIZE, payload, length);
    batch->length += NET_PACKET_HEADER_SIZE + length;
    return 1;
}

// Sends the batch through the connection's transport (TCP: one send(), UDP: one knet message
// per frame, sandbox: the in-memory queue).
static void net_batch_flush(const ClientConnection *client, NetSendBatch *batch) {
    if (batch->length) client->transport->send_frames(client->fd, batch->data, batch->length);
    batch->length = 0;
}

static const cJSON *net_current_map_json() {
    const Game *game = &g_server->game;
    const int index = game->current_map_index;
    return index >= 0 && (size_t) index < game->map_count ? game->maps[index] : NULL;
}

// MAP_INFO before INIT: map indices differ between server and client map lists
// (a custom server map, other rotations), so the client resolves the map by hash
// and asks for the JSON (MAP_REQUEST) when it does not have it.
static void net_send_map_info(ClientConnection *client) {
    const cJSON *map = net_current_map_json();
    size_t size = 0;
    const uint32_t hash = net_map_hash(map, NULL, &size);
    if (!hash) return;

    NetMapInfoPacket packet = {.map_index = g_server->game.current_map_index, .hash = hash, .size = (uint32_t) size};
    const cJSON *name = cJSON_GetObjectItem(map, "name");
    if (cJSON_IsString(name)) snprintf(packet.name, sizeof(packet.name), "%s", name->valuestring);

    net_send_packet(client->fd, NET_PACKET_MAP_INFO, &packet, sizeof(packet));
}

// Stream the map JSON the client asked for (any map of the rotation, by hash).
static void net_handle_map_request(ClientConnection *client, const NetPacket *packet) {
    if (packet->length != sizeof(NetMapRequestPacket)) return;

    NetMapRequestPacket request;
    memcpy(&request, packet->payload, sizeof(request));

    for (size_t i = 0; i < g_server->game.map_count; i++) {
        const cJSON *map = g_server->game.maps[i];
        size_t size = 0;
        if (net_map_hash(map, NULL, &size) != request.hash) continue;

        char *json = NULL;
        net_map_hash(map, &json, &size);
        if (!json) return;

        if (size > NET_MAP_MAX_SIZE) {
            server_log("Not sending map %zu to %s: %zu bytes is over the %u byte limit", i, client->name, size, NET_MAP_MAX_SIZE);
            cJSON_free(json);
            return;
        }

        server_log("Sending map %zu (%zu bytes) to %s", i, size, client->name);

        NetMapDataPacket chunk = {.hash = request.hash};
        for (size_t offset = 0; offset < size; offset += NET_MAP_CHUNK_SIZE) {
            const size_t length = size - offset < NET_MAP_CHUNK_SIZE ? size - offset : NET_MAP_CHUNK_SIZE;
            chunk.offset = (uint32_t) offset;
            chunk.length = (uint16_t) length;
            memcpy(chunk.data, json + offset, length);
            if (!net_send_packet(client->fd, NET_PACKET_MAP_DATA, &chunk, (uint16_t) (NET_MAP_DATA_HEADER_SIZE + length))) break;
        }

        cJSON_free(json);
        return;
    }
}

// WP-E part 2b (shared/include/social_v7.h): a v7 peer may send the v7 chat / voice / spray / taunt
// packets to this server.
static void net_send_social_caps(const ClientConnection *client) {
    if (client->peer_minor < NET_PROTOCOL_MINOR_V7) return;
    const uint8_t version = NET_SOCIAL_V7_VERSION;
    net_send_packet(client->fd, NET_PACKET_SOCIAL_V7, &version, sizeof(version));
}

// v7 syncLobbyP(player, id, 0) in a comp game: the connection's entry (account identity, a
// ranked roster team) and the whole table to it ("cd"). Called after every INIT (the client
// resets its session there) and when accounts settle the identity.
void net_comp_lobby_sync(ClientConnection *client) {
    Game *game = &g_server->game;
    if (!game->config.comp || !client) return;
    CompLobbyEntry identity;
    memset(&identity, 0, sizeof(identity));
    identity.flag = -1;
    identity.class_index = -1;
    if (client->acct.account_id && client->acct.join_state == SERVER_JOIN_READY) {
        identity.account_id = client->acct.account_id;
        identity.level = client->acct.level;
        identity.verified = (uint8_t) !!(client->acct.flags & NET_PLAYER_INFO_VERIFIED);
        snprintf(identity.name, sizeof(identity.name), "%s", client->acct.username);
        snprintf(identity.clan, sizeof(identity.clan), "%s", client->acct.clan);
    } else if (client->local) { // the offline sandbox's own player counts as an account (decisions.md)
        snprintf(identity.name, sizeof(identity.name), "%s", client->name);
    }
    comp_lobby_join(game, client->id, &identity);
    comp_lobby_send_all(game, client->id);
    const int team = accounts_assigned_team(client); // ranked: the roster team, never picked
    if (team) comp_lobby_set_team(game, client->id, team);
    // a comp host's account rosters (cfg.teams): the account's team when it is on one (getAccountTeamIndex + 1,
    // deob-10.0.3.js:216009-216014); config.lockedTeams then keeps the comp menu's Join / Leave Team away
    const int roster = team || !identity.name[0] ? 0 : game_config_roster_team(game, identity.name);
    if (roster) comp_lobby_set_team(game, client->id, roster);
}

// "gsc" to one connection (EXT peers): the current game state
static void net_send_game_state(ClientConnection *client) {
    if (!client || client->peer_minor < NET_PROTOCOL_MINOR_EXT) return;
    NetGameStatePacket state = {.state = g_server->game.game_state};
    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(state)];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_GAME_STATE, &state, sizeof(state));
    if (n) net_send_packet(client->fd, NET_PACKET_EXT, buf, n);
}

// NET_EXT_KN_RANKED_MATCH: the lobby's ranked flag, sent after the INIT (the client resets its game there).
// accounts_ranked_active is the roster lobby of a ranked match (accounts.c acc_ranked_setup).
static void net_send_ranked_match(ClientConnection *client) {
    if (client->peer_minor < NET_PROTOCOL_MINOR_EXT) return;
    const NetRankedMatchPacket packet = {.ranked = (uint8_t) !!accounts_ranked_active()};
    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(packet)];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_KN_RANKED_MATCH, &packet, sizeof(packet));
    if (n) net_send_packet(client->fd, NET_PACKET_EXT, buf, n);
}

// v10 init's [2] eventIndex (Game.event_index), which NetInitPacket has no room for: right after the INIT
static void net_send_event_index(ClientConnection *client) {
    if (client->peer_minor < NET_PROTOCOL_MINOR_EXT) return;
    const NetInitEventPacket packet = {.event_index = g_server->game.event_index};
    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(packet)];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_INIT_EVENT, &packet, sizeof(packet));
    if (n) net_send_packet(client->fd, NET_PACKET_EXT, buf, n);
}

static void net_send_init(ClientConnection *client) {
    accounts_send_billboard(client); // init "bill" (config gameserver.billboards), before the map loads
    net_send_map_info(client);
    // WP-E: init's config / customData / networkData, before the INIT that uses them. A peer whose
    // HELLO is still on its way gets them when it arrives (NET_PACKET_HELLO below).
    if (client->peer_minor >= NET_PROTOCOL_MINOR_V7) game_config_send(&g_server->game, client->id);

    const NetInitPacket packet = {
        .major = NET_PROTOCOL_MAJOR,
        .minor = NET_PROTOCOL_MINOR, // 1.1: accepts ENT V5 / V6, 1.2: V7 (client_enter_game sends them only then)
        .patch = NET_PROTOCOL_PATCH, // 1.3.1: ENT V8 (WP-C)
        // Krunker v10.0.3 in the low nibble (wire compatibility), this server's product flavor in
        // the high nibble (shared.h). A base server's byte stays 3; a client of a different flavor
        // refuses us from this (client_apply_init path).
        .ruleset = NET_WIRE_RULESET_BYTE(NET_INIT_RULESET, flavor_get()),
        .map_index = g_server->game.current_map_index,
        .mode_index = game_mode_id(&g_server->game), // Krunker "init" modeIndex: the mode id
        .tick_rate = g_server->config.tick_rate,
        .client_id = client->id,
    };

    net_send_packet(client->fd, NET_PACKET_INIT, &packet, sizeof(packet));
    net_send_game_state(client);  // v10 gameState: a joiner learns the phase now (its names follow it)
    net_send_ranked_match(client); // v10 INIT isRanked: after the INIT, which resets the client's game
    net_send_event_index(client);  // v10 INIT eventIndex, likewise
    net_send_social_caps(client); // WP-E part 2b: after the INIT (the client resets it there)
    net_comp_lobby_sync(client);  // comp lobby: the connection's entry + the table
    brain_events_send_init(client); // v10 init syncData.warsClan: the Turf Wars clan holding the map
    ks_entities_send_state(&g_server->game, client->id); // v10 init syncData entSeed / lo / ent / entAtt (after the INIT that resets them)
    game_ai_sync_non_players(&g_server->game); // a connection without a player: every AI's "aai" next AI.sync
}

// v10 getName under isUsingRankedNames: a ranked player's name is "Player_<n>" for everyone else
// until the match starts (game_names_masked: WARMUP or earlier). n is the roster slot (KN choice;
// v10 numbers the lobby's players in join order). `real` is the connection's name.
static ClientConnection *net_client_of_player(const Player *player) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        if (player && g_server->clients[i].player == player) return &g_server->clients[i];
    }
    return NULL;
}

static void net_shown_name(const ClientConnection *to, const ClientConnection *subject, const char *real, char *out, const size_t size) {
    const int id = subject ? accounts_ranked_lobby_id(subject) : 0;
    if (id > 0 && to != subject && game_names_masked(&g_server->game)) snprintf(out, size, "Player_%d", id);
    else snprintf(out, size, "%s", real ? real : "");
}

// SKINS of `player` to `client`: the worn skins, and their paint (V2) to a peer that announced patch 6. The paint is
// the owner's (its painted copies, accounts_skin_paints); a bot or a guest wears none.
static void net_send_skins(ClientConnection *client, const Player *player) {
    if (!net_peer_has_skin_paint(client->peer_minor, client->peer_patch)) {
        const NetSkinsPacket skins = {.uid = player->uid, .skins = player->skins};
        net_send_packet(client->fd, NET_PACKET_SKINS, &skins, NET_SKINS_PACKET_V1_SIZE);
        return;
    }
    NetSkinsPacketV2 skins = {.uid = player->uid, .skins = player->skins};
    accounts_skin_paints(net_client_of_player(player), &player->skins, &skins.paints);
    net_send_packet(client->fd, NET_PACKET_SKINS, &skins, NET_SKINS_PACKET_V2_SIZE);
}

static void net_send_spawn(ClientConnection *client, const Player *player, const int is_you) {
    // The weapon / melee skins first: the client builds the player's meshes on SPAWN.
    net_send_skins(client, player);

    NetSpawnPacket packet = {
        .uid = player->uid,
        .is_you = (uint8_t) is_you,
        .active = player->active,
        .class_index = player->class_index,
        .secondary_index = player->secondary_index,
        .cosmetics = player->cosmetics,
        .team = player->team,
        .health = player->health,
        .max_health = player->max_health,
        .position = {player->position.x, player->position.y, player->position.z},
        .direction = {player->direction.x, player->direction.y},
        .attach_index = player->attach_index, // WP-C: v7.1.4 sync attachIndex, headMlt
        // the player's own headMlt; Bighead FFA's mode.head_mlt rides in "bh" (NET_MODE_COUNTER_HEAD)
        .head_mlt = player->head_mlt > 0.0f ? player->head_mlt : 1.0f,
    };

    net_shown_name(client, net_client_of_player(player), player->name, packet.name, sizeof(packet.name));

    // WP-F: the v7.1.4 slots (players.js sync). badgeIndex only when the player shows its custom
    // badge, and not in anonMode (except to itself and dev viewers).
    packet.cosmetics_v7 = player->cosmetics_v7;
    const int anon_hidden = player->anon_mode && !is_you && !(client->acct.flags & NET_PLAYER_INFO_DEV);
    if (anon_hidden || !(player->show_badges & PLAYER_SHOW_BADGE_CUSTOM)) packet.cosmetics_v7.badge = -1;

    // WP-C: 1.3.0 and older peers only take the V2 SPAWN; 1.3.1 peers prefix-read the WP-F V4
    const uint16_t size = net_peer_has_wpc(client->peer_minor, client->peer_patch) ? NET_SPAWN_PACKET_V4_SIZE : NET_SPAWN_PACKET_V2_SIZE;
    net_send_packet(client->fd, NET_PACKET_SPAWN, &packet, size);

    // Prop Hunt: the prop it wears (and its rotation) after its SPAWN, which reset them (prop_hunt.h)
    const Game *game = &g_server->game;
    if (client->peer_minor >= NET_PROTOCOL_MINOR_EXT && game->mode && game->mode->config.prop && game_player_is_prop(game, player)) {
        uint8_t prop[NET_EXT_HEADER_SIZE + sizeof(NetPropRotationPacket) + sizeof(NetPropUpdatePacket)];
        uint16_t n = prop_pack_update(player, 0, prop, sizeof(prop));
        if (n) net_send_packet(client->fd, NET_PACKET_EXT, prop, n);
        n = player->prop_rot || player->prop_rot_saved_set ? prop_pack_rotation(player, 0, prop, sizeof(prop)) : 0;
        if (n) net_send_packet(client->fd, NET_PACKET_EXT, prop, n);
    }

    // v10 init syncData.plrPose (deob-10.0.3.js:220294-220302): a posed player's pose after its SPAWN, so a
    // joiner (net_send_existing_spawns) has it; again after a respawn, which the client takes as the same pose
    if (player->pose.has && client->peer_minor >= NET_PROTOCOL_MINOR_EXT) {
        uint8_t pose[NET_EXT_HEADER_SIZE + sizeof(NetPlayerPosePacket)];
        const uint16_t n = player_pose_pack(player, pose, sizeof(pose));
        if (n) net_send_packet(client->fd, NET_PACKET_EXT, pose, n);
    }

    // KrunkNative Object Manager: the SPAWN rebuilt the class loadout; the extra weapons follow it
    if (player->extra_weapon_count > 0 && ks_server_peer_v7(client)) {
        NetLoadoutExtraPacket extra = {.uid = player->uid};
        for (int i = 0; i < player->extra_weapon_count && i < PLAYER_EXTRA_WEAPONS_MAX; i++) extra.weapons[extra.count++] = (uint8_t) player->extra_weapons[i];
        net_send_packet(client->fd, NET_PACKET_LOADOUT_EXTRA, &extra, sizeof(extra));
    }
}

static void net_broadcast_spawn(const Player *player) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        net_send_spawn(client, player, client->player == player);
    }
}

// v10's "0" carries each player's customLoadout (getPlayerAddData deob-10.0.3.js:198810; players.add dresses the player
// with it, :195103-195108), so a joiner sees the loadout a keepLoadout mode gave it. Our clients rebuild such a loadout
// from the Gun Game tier they mirror from LOADOUT packets (mode_gun_loadout), which a joiner never got: its own LOADOUT
// (the base loadout, its ammo and the tier) follows the player's SPAWN.
static void net_send_existing_loadout(ClientConnection *client, const Player *player) {
    const GameMode *mode = g_server->game.mode;
    if (!player->active || !mode || !(mode->config.weapon_order_size > 0 || mode->config.keep_loadout) || !net_peer_takes(client, NET_PACKET_LOADOUT)) return;
    NetLoadoutPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.uid = player->uid;
    packet.count = player_loadout_base(player) < NET_LOADOUT_MAX ? player_loadout_base(player) : NET_LOADOUT_MAX;
    for (int i = 0; i < packet.count; i++) {
        packet.weapons[i] = player->loadout[i];
        packet.ammo[i] = player->ammo ? (int32_t) player->ammo[i] : -1;
    }
    packet.loadout_index = player->loadout_index;
    packet.weapon_tier = player->mode.weapon_tier;
    if (packet.count) net_send_packet(client->fd, NET_PACKET_LOADOUT, &packet, sizeof(packet));
}

// Every known player, dead ones too (active = 0): the scoreboard needs their names.
static void net_send_existing_spawns(ClientConnection *client) {
    for (size_t i = 0; i < g_server->game.player_count; i++) {
        Player *player = g_server->game.players[i];
        if (!player || player == client->player) continue;
        net_send_spawn(client, player, 0);
        net_send_existing_loadout(client, player); // customLoadout: the mode's loadout of this player (Gun Game's tier)
    }
}

// "start" with spectating = true plus what io_ent streams to a new spectator: every
// player (dead ones too, for names), the leaderboard and the round clock.
static void net_send_spectate_state(ClientConnection *client) {
    const NetSpectatePacket ack = {.spectating = 1};
    net_send_packet(client->fd, NET_PACKET_SPECTATE, &ack, sizeof(ack));
    net_send_existing_spawns(client);
    accounts_send_infos_to(client);
    game_bots_send_infos(&g_server->game, client->id); // bot players' account-style PLAYER_INFO
    game_sync_leaders(&g_server->game, client->id);
    game_send_round_state(&g_server->game, client->id);
    game_ai_sync_non_players(&g_server->game); // AI.sync sends spectators every AI ("aai" first)
    net_resync_live_world(client); // projectiles launched while it was in the menu
}

// KrunkNative: the life that ends leaves its eye and look for the Object Manager's menu spawns
static void net_keep_view(ClientConnection *client, const Player *player) {
    if (!client || !player) return;
    client->keep.has_view = 1;
    client->keep.view_eye = (vec3) {player->position.x, player->position.y + (player->height > 0.0f ? player->height : game_constants.player_height) -
                                                            game_constants.camera_height, player->position.z};
    client->keep.view_direction = player->direction;
}

// The cheats the client mirrors (its prediction and HUD lines), after its SPAWN: those on, kept from
// the menu or an earlier map (chat_commands.c player_keep_apply put them on the Player)
static void net_send_kept_cheats(ClientConnection *client) {
    const Player *p = client->player;
    if (!p) return;
    const struct {
        unsigned int bit;
        int on;
        NetCheatFlag flag;
    } k[] = {{PLAYER_KEEP_NOCLIP, p->noclip, NET_CHEAT_NOCLIP}, {PLAYER_KEEP_UNLIMITED_AMMO, p->unlimited_ammo, NET_CHEAT_UNLIMITED_AMMO},
             {PLAYER_KEEP_FORCE_HIDE, p->force_hide, NET_CHEAT_FORCE_HIDE}};
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if (!(client->keep.set & k[i].bit) || !k[i].on) continue;
        const NetCheatPacket packet = {.flag = (uint8_t) k[i].flag, .on = 1};
        net_send_packet(client->fd, NET_PACKET_CHEAT, &packet, sizeof(packet));
    }
}

// ENT with spectate = 1: a live player leaves the match first (LEAVE to the others),
// then the connection spectates until an ENT without the flag spawns it.
static void net_start_spectating(ClientConnection *client) {
    net_set_spectating(client, 1);

    if (client->player) {
        Player *player = client->player;
        net_keep_view(client, player); // KrunkNative: where the Object Manager places things from the menu now
        ks_server_on_leave(client); // KrunkScript onPlayerLeave: the player leaves the match
        player->active = 0;
        game_player_left(&g_server->game, player);

        const NetLeavePacket leave = {.uid = player->uid};
        for (size_t i = 0; i < g_server->client_count; i++) {
            if (&g_server->clients[i] != client) net_send_packet(g_server->clients[i].fd, NET_PACKET_LEAVE, &leave, sizeof(leave));
        }

        net_remove_game_player(player);
        client->player = NULL;
        client->joined = 0;
        client->spawned_at = 0.0f;
        client->last_significant_move_at = g_server->now; // markPlayerUnspawned
        client->has_idle_position = 0;
    }

    server_log("%s is spectating", client->name);
    net_send_spectate_state(client);
}

static void net_spawn_client(ClientConnection *client) {
    const int first_spawn = !client->player;

    if (!client->player) {
        Player *player = player_init(&g_server->game);
        if (!player) return;

        player->uid = client->id;
        player->team = accounts_assigned_team(client); // ranked roster team (players.setTeam isFromQueue), 0 = mode picks
        player->ping = client->ping_ms > 0 ? (float) client->ping_ms : 0.0f; // lag compensation (getStateConst)
        player->custom_values = client->custom_values; // WP-C: custVals survive the map change (v7 never resets them)
        client->player = player;
        game_players_add(&g_server->game, player);
    }

    snprintf(client->player->name, sizeof(client->player->name), "%s", client->name);
    client->player->class_index = client->class_index;
    // comp lobby: the player spawns on its lobby team and class (getLobbyDat "t" / "c")
    const CompLobbyEntry *comp = g_server->game.config.comp ? comp_lobby_get(&g_server->game, client->id) : NULL;
    if (comp && comp->team) client->player->team = comp->team;
    if (comp && comp->class_index >= 0) client->player->class_index = comp->class_index;
    // v10.0.3 updateSecIndex (deob-10.0.3.js:190934-190948): a secondary weapons' req refuses is the Pistol (2). Its level
    // tail (minRec against account.level, game_weapon_level_ok) is checked here, at every spawn with the account as it is
    // then (a guest: level 0); player_spawn checks the rest of req (dev, customOnly, noComp)
    const int picked = client->secondary_index;
    const Weapon *picked_weapon = g_server->game.weapons && picked >= 0 && (size_t) picked < g_server->game.weapon_count
                                      ? g_server->game.weapons[picked] : NULL;
    const int level = client->acct.account_id > 0 ? client->acct.level : 0;
    client->player->secondary_index = game_weapon_level_ok(&g_server->game, picked_weapon, level, net_client_is_dev(client)) ? picked : WEAPON_ID_PISTOL;
    client->player->cosmetics = client->cosmetics;
    client->player->slid_cont = client->slid_cont; // gameserver.ts: player.slidCont = ent[15]
    client->player->spray_index = client->spray_index; // applyEntData: player.sprayIndex = ent[1]
    client->player->skins = client->skins;             // applyEntData: player.skins = ent[2], meleeIndex = ent[9]
    client->player->lag_comp = client->lag_comp;       // applyLagCompFromEnt: player.lagComp = ent[11]
    client->player->attach_index = client->attach_index; // WP-C: player.attachIndex = ent[12] (sent in SPAWN)
    client->player->chall_mode = client->chall_mode;     // WP-C: player.challMode = ent[7]
    client->player->anon_mode = client->anon_mode;       // WP-C: player.anonMode = ent[19]
    // WP-F: ent[15] shoeIndex, [21] petIndex, [26] wristIndex, [30] charms, [31] backIndex, [32] headIndex,
    // [33] playerCardIndex; ent[22..25] showBadges; the badge is the account's (stats.bdg)
    client->player->cosmetics_v7 = client->cosmetics_v7;
    client->player->cosmetics_v7.badge = client->acct.account_id ? client->acct.badge : -1;
    client->player->show_badges = client->show_badges;
    // WP-B: v7 player.streakIndexs (ENT) and player.account for reqAccount streaks: a backend
    // account, or the host's own machine (the offline sandbox counts as an account, decisions.md)
    player_set_streak_indexes(client->player, client->streak_indexes_set ? client->streak_indexes : NULL);
    client->player->has_account = (unsigned char) (client->acct.account_id > 0 || client->local);
    // account.isDev for the spawn's weapon req (player_spawn: a dev's Grappler in any lobby, v10 updateSecIndex):
    // a DEV account, an op or the server's own machine, read at every spawn (a live account switch counts next)
    client->player->dev = (unsigned char) !!net_client_is_dev(client);
    // KrunkNative Object Manager: the connection's extra weapons, while this game allows them
    const ObjectManagerPerms om_perms = net_om_perms(client);
    // the Build Tool's cost waiver follows the Object Manager's spawn right (shared.h build_free)
    client->player->build_free = (object_manager_caps(&g_server->game, &om_perms) & OM_CAP_SPAWN) != 0;
    if (!object_manager_keep_extras(&g_server->game, &om_perms)) {
        const int had = client->player->extra_weapon_count > 0;
        client->om_extra_count = 0;
        player_set_extra_weapons(client->player, NULL, 0);
        if (had) object_manager_send_extras(&g_server->game, client->player, NET_TARGET_ALL);
    } else { // (the Tool Gun only while this player may spawn: a host who handed over the game loses it)
        client->om_extra_count = object_manager_filter_extras(&g_server->game, &om_perms, client->om_extras, client->om_extra_count);
        player_set_extra_weapons(client->player, client->om_extras, client->om_extra_count);
    }
    player_spawn(client->player);
    // KrunkNative: what commands set while this connection was not spawned in (god, noclip ... from the
    // menu, a menu `tp`), and cheats that outlive a map change's new Player
    player_keep_apply(client->player, &client->keep);
    if (client->keep.has_spawn) {
        client->keep.has_spawn = 0;
        client->player->position = client->keep.spawn_at;
        client->player->last_position = client->keep.spawn_at;
        client->player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    }
    accounts_player_card(client, client->player); // WP-E: class stat of the spawned class + pcStatIndex value
    client->joined = 1;
    net_mark_client_spawned(client);
    if (first_spawn) net_send_existing_spawns(client);
    net_broadcast_spawn(client->player);
    player_anim_spawn_sent(client->player); // a class-model swap of its onPlayerSpawn, after the SPAWN (player_anim.h)
    net_resync_live_world(client); // what the live-world filter held back while dead / in the menu
    net_send_kept_cheats(client); // the client's own copy: noclip, unlimited ammo, forcehide (after its SPAWN)

    if (first_spawn) {
        // io_ent first spawn: leaderboard + round clock for the joiner, then
        // broadcastServerMessage(0). The joiner's own HUD already seeds its
        // "joined the game" line (hud_ui.c hud_seed_chat), so it is skipped here.
        game_sync_leaders(&g_server->game, client->id);
        game_send_round_state(&g_server->game, client->id);

        if (!client->announced) {
            client->announced = 1;
            if (accounts_join_message()[0]) net_broadcast_server_message(client, accounts_join_message());
        }
    }

    accounts_on_spawn(client, first_spawn); // PLAYER_INFO (account level / clan), MOTD on the first spawn
    if (first_spawn) game_bots_send_infos(&g_server->game, client->id); // the bots' level / clan, like accounts_send_infos_to
}

// ENT from a client: (re)spawn unless the round is over, the player is alive, or
// it died less than config.deathDelay ago.
static void net_request_spawn(ClientConnection *client) {
    // io_ent in the vote window: spawn anyway, then the late "end" (didWin -1), so the
    // joiner gets the end screen instead of a closed menu over nothing.
    if (game_end_late_spawn(&g_server->game, client->player)) {
        net_spawn_client(client);
        game_send_end_late(&g_server->game, client->id);
        return;
    }
    if (!game_can_spawn(&g_server->game, client->player)) return; // shared with the offline client
    // comp lobby canSpawn: a team and a class picked in the comp menu
    const char *blocked = comp_lobby_spawn_block(&g_server->game, client->id);
    if (blocked) {
        accounts_notice(client, blocked);
        return;
    }

    net_spawn_client(client);
}

static void net_free_players() {
    for (size_t i = 0; i < g_server->client_count; i++) {
        if (g_server->clients[i].player) g_server->clients[i].custom_values = g_server->clients[i].player->custom_values; // WP-C
        g_server->clients[i].player = NULL;
        g_server->clients[i].keep.has_view = 0;  // KrunkNative: the old map's spots mean nothing on the next one
        g_server->clients[i].keep.has_spawn = 0;
        g_server->clients[i].spawned_at = 0.0f;
        g_server->clients[i].last_significant_move_at = g_server->now; // markPlayerUnspawned: a new idle clock
        g_server->clients[i].has_idle_position = 0;
    }

    for (size_t i = 0; i < g_server->game.player_count; i++) {
        Player *player = g_server->game.players[i];
        net_free_player(player);
    }

    free(g_server->game.players);
    g_server->game.players = NULL;
    g_server->game.player_count = 0;
}

// Re-init the game on another map/mode and announce it (INIT). With
// respawn_joined every client that played the round respawns (next round);
// otherwise only the players alive right now do (CYCLE / CLI `map`).
static void net_change_map_internal(const int map_index, const int mode_index, const int respawn_joined) {
    if (!g_server->game.map_count) return;

    uint8_t *respawn_clients = NULL;

    if (g_server->client_count) {
        respawn_clients = calloc(g_server->client_count, sizeof(uint8_t));

        if (respawn_clients) {
            for (size_t i = 0; i < g_server->client_count; i++) {
                const ClientConnection *client = &g_server->clients[i];
                respawn_clients[i] = client->player && (client->player->active || (respawn_joined && client->joined));
            }
        }
    }

    accounts_round_ending(); // stats of a round abandoned before its END
    net_free_players();

    for (size_t i = 0; i < g_server->client_count; i++) {
        g_server->clients[i].joined = 0;
        memset(g_server->clients[i].missed_projectiles, 0, sizeof(g_server->clients[i].missed_projectiles)); // game_init drops every projectile
    }

    game_init(&g_server->game, map_index, mode_index, 0);
    ks_server_round_init(); // KrunkScript: (re)load the map's server script, start()
    accounts_round_changed(); // lobby size / round length per mode, billboard rotation, match id

    server_log("Loaded map %d mode %d", g_server->game.current_map_index, g_server->game.current_mode_index);

    for (size_t i = 0; i < g_server->client_count; i++) {
        net_send_init(&g_server->clients[i]);
    }

    if (respawn_clients) {
        for (size_t i = 0; i < g_server->client_count; i++) {
            if (respawn_clients[i]) net_spawn_client(&g_server->clients[i]);
        }

        free(respawn_clients);
    }

    // INIT resets every client's session: spectators get their spectate state again.
    for (size_t i = 0; i < g_server->client_count; i++) {
        if (net_is_spectating(&g_server->clients[i])) net_send_spectate_state(&g_server->clients[i]);
    }
}

void net_change_map(const int map_index, const int mode_index) {
    net_change_map_internal(map_index, mode_index, 0);
}

static void net_cycle_map() {
    if (!g_server->game.map_count) return;
    net_change_map((g_server->game.current_map_index + 1) % (int) g_server->game.map_count, g_server->game.current_mode_index);
}

// ------------------------------------------------------------------ chat commands
// Krunker v3.1.1 "/" commands (shared/src/chat_commands.c) over the connections. Kicks
// and bans close the target after the current poll pass (net_main), so a command never
// compacts g_server->clients while net_poll_client() is walking it.

#define NET_PENDING_KICKS_MAX 32

static struct {
    int id;
    NetErrorPacket error;
} g_pending_kicks[NET_PENDING_KICKS_MAX];
static int g_pending_kick_count;
static ChatCommandHost g_chat_host;

// isDev: CLI / config op, a DEV account the brain verified (players.player_developer), or a
// player on the server's own machine (it hosts this server, v3.1.1's offline host). Loopback
// alone is not enough: net-bridge connects from 127.0.0.1 for every browser, so a bridged
// connection counts by the address it forwards (NET_PACKET_FORWARDED, net_handle_forwarded).
int net_client_is_dev(const ClientConnection *client) {
    // KRUNKNATIVE_GUEST_LOCAL (tests): the offline sandbox's own connection is a guest for the Object Manager, as a
    // player on a server is (client/tests/physgun_cycle_e2e.py runs the Physics Gun's non-dev pace this way)
    if (client->local && getenv("KRUNKNATIVE_GUEST_LOCAL")) return 0;
    return client->op || client->local || (client->acct.flags & NET_PLAYER_INFO_DEV);
}

// KrunkNative Object Manager: who is asking (shared/src/object_manager.c decides what they may do)
static ObjectManagerPerms net_om_perms(const ClientConnection *client) {
    return (ObjectManagerPerms) {
        .dev = (unsigned char) !!net_client_is_dev(client),
        .host = (unsigned char) !!accounts_is_custom_host(client),
        .ranked = (unsigned char) !!accounts_ranked_active(),
    };
}

// NET_PACKET_SPAWN_REQ: one Object Manager action, answered with NET_PACKET_SPAWN_RESULT. The
// connection keeps the extra weapons across map changes (the players are freed there).
static void net_keep_om_extras(ClientConnection *client) {
    if (!client || !client->player) return;
    client->om_extra_count = client->player->extra_weapon_count;
    memcpy(client->om_extras, client->player->extra_weapons, sizeof(client->om_extras));
}

// A request of a connection without a live player (the menu, dead, spectating): devs still spawn,
// from their view, and their extra weapons go into the connection's list for the next spawn
static void net_om_request_view(ClientConnection *client, const NetSpawnReqPacket *req, NetSpawnResultPacket *result) {
    const ObjectManagerPerms perms = net_om_perms(client);
    ObjectManagerView view;
    object_manager_menu_view(&g_server->game, client->player, &client->keep, client->id, &view);
    object_manager_request_view(&g_server->game, &view, req, &perms, client->om_extras, &client->om_extra_count, result);
    if (client->player) player_set_extra_weapons(client->player, client->om_extras, client->om_extra_count); // (a dead Player: the list only)
}

static void net_handle_object_manager(ClientConnection *client, const NetSpawnReqPacket *req) {
    const ObjectManagerPerms perms = net_om_perms(client);
    NetSpawnResultPacket result;
    if (client->player && client->player->active) {
        object_manager_request(&g_server->game, client->player, req, &perms, &result);
        net_keep_om_extras(client);
    } else {
        net_om_request_view(client, req, &result);
    }
    net_send_packet(client->fd, NET_PACKET_SPAWN_RESULT, &result, sizeof(result));
}

static PlayerKeep *net_chat_keep(void *user, const int id) {
    (void) user;
    ClientConnection *client = net_client_by_id(id);
    return client ? &client->keep : NULL;
}

static void net_chat_om_view(void *user, const int id, const NetSpawnReqPacket *req, NetSpawnResultPacket *out) {
    (void) user;
    ClientConnection *client = net_client_by_id(id);
    if (!client) {
        memset(out, 0, sizeof(*out));
        out->status = OM_STATUS_DEAD;
        return;
    }
    net_om_request_view(client, req, out);
}

// The drop key ("drpw", int action 2) on an Object Manager extra slot: v7's dropWeapon only takes
// picked-up weapons back to their pickup, so the extra one is tossed as OM_ACTION_DROP_HELD
// instead (a v7 peer gets the SPAWN_RESULT). 1 = handled.
static int net_drop_extra(ClientConnection *client, const int action) {
    if (action != 2 || !client->player || !ks_server_peer_v7(client) || !object_manager_holds_extra(client->player)) return 0;
    const NetSpawnReqPacket req = {.action = OM_ACTION_DROP_HELD, .pitch = client->player->direction.x, .yaw = client->player->direction.y};
    net_handle_object_manager(client, &req);
    return 1;
}

// `give` / `drop` changed a player's extra weapons (shared/src/chat_commands.c): the connection
// keeps them across respawns and map changes, as after a SPAWN_REQ
static void net_chat_om_extras(void *user, const int id) {
    (void) user;
    net_keep_om_extras(net_client_by_id(id));
}

// KrunkNative `class`: the connection keeps the class and secondary its player has now, for its next spawn, and
// every client gets the new look and loadout (SPAWN, as a respawn sends it).
static void net_chat_class_switched(void *user, const int id) {
    (void) user;
    ClientConnection *client = net_client_by_id(id);
    if (!client || !client->player) return;
    client->class_index = client->player->class_index;
    client->secondary_index = client->player->secondary_index;
    net_broadcast_spawn(client->player);
}

static int net_chat_peer_count(void *user) {
    (void) user;
    return (int) g_server->client_count;
}

static int net_chat_peer_at(void *user, const int index, ChatPeer *out) {
    (void) user;
    if (index < 0 || (size_t) index >= g_server->client_count) return 0;
    const ClientConnection *client = &g_server->clients[index];
    *out = (ChatPeer) {
        .id = client->id,
        .name = client->name,
        .address = client->address,
        .ping_ms = client->ping_ms,
        .player = client->player,
        .spectating = net_is_spectating(client),
        .dev = net_client_is_dev(client),
        .premium_host = accounts_is_custom_host(client),
        .ranked = accounts_ranked_active(), // the Object Manager's rules (give / drop)
        .account_id = client->acct.account_id, // backend account (server/src/accounts.c), 0 guest
    };
    return 1;
}

static void net_chat_disconnect(void *user, const int id, const char *message, const char *code) {
    (void) user;
    if (g_pending_kick_count >= NET_PENDING_KICKS_MAX) return;
    g_pending_kicks[g_pending_kick_count].id = id;
    NetErrorPacket *error = &g_pending_kicks[g_pending_kick_count].error;
    memset(error, 0, sizeof(*error));
    snprintf(error->message, sizeof(error->message), "%s", message ? message : "Disconnected");
    snprintf(error->code, sizeof(error->code), "%s", code ? code : "000000");
    g_pending_kick_count++;
}

// Close a connection after the current poll pass with NET_PACKET_ERROR (accounts.c).
void net_kick_client(const int id, const char *message, const char *code) {
    net_chat_disconnect(NULL, id, message, code);
}

// A live sign-out / account switch (accounts.c) took rights away (lost: 1 when DEV or the host went):
// the cheats they allowed go too, and a third-person camera they granted (silent when off)
unsigned int net_revoke_lost_cheats(ClientConnection *client, const int lost) {
    if (!client) return 0;
    const unsigned int off = chat_command_revoke_cheats(&g_chat_host, client->id, client->player, &client->keep, net_client_is_dev(client),
                                                        accounts_is_custom_host(client));
    const char *reason = NULL;
    if (lost && !chat_third_person_allowed(&g_chat_host, client->id, &reason)) {
        const NetCheatPacket packet = {.flag = NET_CHEAT_THIRD_PERSON, .on = 0};
        net_send_packet(client->fd, NET_PACKET_CHEAT, &packet, sizeof(packet));
    }
    return off;
}

static void net_chat_change_map(void *user, const int map_index, const int mode_index) {
    (void) user;
    net_change_map(map_index, mode_index);
}

static void net_chat_log(void *user, const char *text) {
    (void) user;
    server_log("[Server] %s", text);
}

static int net_chat_ban_account(void *user, const int account_id, const char *name) {
    (void) user;
    return accounts_ban_account(account_id, name);
}

static int net_chat_unban_account(void *user, const int account_id) {
    (void) user;
    return accounts_unban_account(account_id);
}

static int net_chat_list_account_bans(void *user, int *out, const int max) {
    (void) user;
    return accounts_list_account_bans(out, max);
}

static int net_chat_kpd_officer(void *user, const int requester, const char *name, const int on) {
    (void) user;
    return accounts_kpd_officer(requester, name, on);
}

static int net_chat_kpd_apps(void *user, const int requester) {
    (void) user;
    return accounts_kpd_apps(requester);
}

ChatCommandHost *net_chat_host(void) {
    g_chat_host.game = &g_server->game;
    g_chat_host.perms = g_server->config.perms;
    g_chat_host.cheats_open = g_server->config.cheats_open;
    g_chat_host.lobby = "native";
    g_chat_host.region = "local";
    g_chat_host.tick_rate = g_server->config.tick_rate ? (float) g_server->config.tick_rate : 30.0f;
    g_chat_host.peer_count = net_chat_peer_count;
    g_chat_host.peer_at = net_chat_peer_at;
    g_chat_host.disconnect = net_chat_disconnect;
    g_chat_host.change_map = net_chat_change_map;
    g_chat_host.log = net_chat_log;
    g_chat_host.ban_account = net_chat_ban_account;     // gameserver.bans_file "accounts"
    g_chat_host.unban_account = net_chat_unban_account;
    g_chat_host.list_account_bans = net_chat_list_account_bans;
    g_chat_host.om_extras_changed = net_chat_om_extras;
    g_chat_host.keep = net_chat_keep;                 // KrunkNative: cheats / tp from the menu
    g_chat_host.om_request_view = net_chat_om_view;   // KrunkNative: give / spawn from the menu
    g_chat_host.class_switched = net_chat_class_switched; // KrunkNative `class` (any game; devs only)
    g_chat_host.kpd_officer = net_chat_kpd_officer;   // KrunkNative: /kpd <account> on|off (kn_brain)
    g_chat_host.kpd_apps = net_chat_kpd_apps;         // KrunkNative: /kpd apps
    return &g_chat_host;
}

// ws.close(4002 / 4003) for the targets queued by the last commands. Caller holds the lock.
static void net_process_pending_kicks(void) {
    for (int i = 0; i < g_pending_kick_count; i++) {
        ClientConnection *client = net_client_by_id(g_pending_kicks[i].id);
        if (!client) continue;
        net_send_packet(client->fd, NET_PACKET_ERROR, &g_pending_kicks[i].error, sizeof(g_pending_kicks[i].error));
        server_log("%s %s (id %d)", g_pending_kicks[i].error.message, client->name, client->id);
        net_remove_client(client);
    }
    g_pending_kick_count = 0;
}

// The joiner's own SPAWN at join, dead (active = 0): the official server's join, where the player waits until its
// ENT (the spawn after the ENT is net_spawn_client). The waiting player is not in the game yet, so it is a temporary
// one, freed here; the client's spawn after its ENT is the first real player.
static void net_send_join_spawn(ClientConnection *client) {
    Player *waiting = player_init(&g_server->game);
    if (!waiting) return;
    waiting->uid = client->id;
    waiting->team = accounts_assigned_team(client);
    waiting->active = 0;
    snprintf(waiting->name, sizeof(waiting->name), "%s", client->name);
    net_send_spawn(client, waiting, 1);
    net_free_player(waiting);
}

void net_add_client(ClientConnection *client) {
    if (g_server->config.max_players && g_server->client_count >= g_server->config.max_players) {
        server_log("Rejecting client: server full (%zu/%u)", g_server->client_count, (unsigned int) g_server->config.max_players);
        CLOSE_CLIENT_FD(client);
        return;
    }

    snprintf(client->address, sizeof(client->address), "%s", inet_ntoa(client->addr.sin_addr));
    client->local = (ntohl(client->addr.sin_addr.s_addr) >> 24) == 127; // until a FORWARDED says otherwise
    client->first_packet_seen = 0;
    if (chat_command_ip_banned(net_chat_host(), client->address)) { // pnhBannedAddresses: close(4003, "banned")
        const NetErrorPacket error = {.message = "Banned", .code = "004003"};
        net_send_packet(client->fd, NET_PACKET_ERROR, &error, sizeof(error));
        server_log("Rejecting %s: IP banned", client->address);
        CLOSE_CLIENT_FD(client);
        return;
    }
    NetErrorPacket refuse = {{0}};
    if (accounts_refuse_connect(client->address, &refuse)) { // draining ("GAME UPDATING")
        net_send_packet(client->fd, NET_PACKET_ERROR, &refuse, sizeof(refuse));
        server_log("Rejecting %s: %s", client->address, refuse.message);
        CLOSE_CLIENT_FD(client);
        return;
    }

    ClientConnection *new_clients = realloc(g_server->clients, (g_server->client_count + 1) * sizeof(ClientConnection));

    if (!new_clients) {
        CLOSE_CLIENT_FD(client);
        return;
    }

    g_server->clients = new_clients;
    client->id = g_server->next_client_id++;
    client->player_number = net_alloc_player_number();

    if (!client->player_number) {
        CLOSE_CLIENT_FD(client);
        return;
    }

    net_assign_guest_name(client);
    client->connected_at = g_server->now;
    client->spawned_at = 0.0f;
    client->last_significant_move_at = g_server->now;
    client->has_idle_position = 0;
    client->joined = 0;
    client->announced = 0;
    client->voice_player = 0;
    client->class_index = 0;
    client->secondary_index = -1;
    client->cosmetics = PLAYER_COSMETICS_NONE;
    client->slid_cont = 1;
    client->spray_index = 0;
    client->skins = PLAYER_SKINS_NONE;
    client->lag_comp = 1.0f;
    client->attach_index = DEFAULT_ATTACHMENT_INDEX;
    client->chall_mode = 0;
    client->anon_mode = 0;
    client->cosmetics_v7 = PLAYER_COSMETICS_V7_NONE;
    client->show_badges = PLAYER_SHOW_BADGES_ALL;
    client->pc_stat_index = -1;
    client->chat_limiter = (NetChatLimiter) {0};
    client->ping_seq = 0;
    client->ping_ms = -1;
    client->next_ping_at = 0.0; // first probe right away (pi on connect)
    client->social = (SocialAssembly) {0};
    client->voice = (SocialAssembly) {0};
    client->voice_v7 = (SocialAssembly) {0};
    memset(&client->custom_values, 0, sizeof(client->custom_values)); // WP-C: no custVals yet
    client->om_extra_count = 0; // Object Manager: no extra weapons yet
    memset(&client->keep, 0, sizeof(client->keep)); // KrunkNative: no kept player state yet
    memset(client->missed_projectiles, 0, sizeof(client->missed_projectiles));
    client->join_spawn_due = 0;
    g_server->clients[g_server->client_count] = *client;

    ClientConnection *stored_client = &g_server->clients[g_server->client_count++];
    accounts_on_connect(stored_client);
    on_client_connect(stored_client);
    net_send_init(stored_client);
    stored_client->join_spawn_due = 1; // sent with its HELLO (the peer's version sets the SPAWN's size)
    game_send_round_state(&g_server->game, stored_client->id);
}

void net_remove_client(ClientConnection *client) {
    size_t index = SIZE_MAX;

    for (size_t i = 0; i < g_server->client_count; i++) {
        if (&g_server->clients[i] == client) {
            index = i;
            break;
        }
    }

    if (index == SIZE_MAX) return;

    accounts_on_disconnect(client); // partial stats commit, player-leave to the brain
    votekick_on_leave(client); // a vote kick's target left (void), or a voter (recounted)
    trade_on_leave(client);    // its trade requests / session end ("NAME left the trade")
    comp_lobby_leave(&g_server->game, client->id); // syncLobbyP(player, id, 1)
    on_client_disconnect(client);
    net_set_spectating(client, 0);
    ks_server_on_leave(client); // KrunkScript onPlayerLeave, before the player is removed
    ks_server_on_disconnect(client);
    if (client->player) {
        client->player->active = 0;
        game_player_left(&g_server->game, client->player);
        const NetLeavePacket leave = {.uid = client->player->uid};
        for (size_t i = 0; i < g_server->client_count; i++) {
            if (&g_server->clients[i] != client) net_send_packet(g_server->clients[i].fd, NET_PACKET_LEAVE, &leave, sizeof(leave));
        }
        net_remove_game_player(client->player);
        client->player = NULL;
    }
    if (client->announced) {
        client->announced = 0;
        if (accounts_leave_message()[0]) net_broadcast_server_message(client, accounts_leave_message()); // serverMessages[1]
    }
    net_release_player_number(client);
    social_assembly_free(&client->social);
    social_assembly_free(&client->voice);
    social_assembly_free(&client->voice_v7);
    CLOSE_CLIENT_FD(client);

    if (index + 1 < g_server->client_count) {
        memmove(&g_server->clients[index], &g_server->clients[index + 1], (g_server->client_count - index - 1) * sizeof(ClientConnection));
    }

    g_server->client_count--;

    if (!g_server->client_count) {
        free(g_server->clients);
        g_server->clients = NULL;
        return;
    }

    ClientConnection *new_clients = realloc(g_server->clients, g_server->client_count * sizeof(ClientConnection));
    if (new_clients) g_server->clients = new_clients;
}

_Static_assert(sizeof(NetChatV7Packet) <= NET_MAX_PAYLOAD, "NetChatV7Packet exceeds NET_MAX_PAYLOAD");

static int net_team_mode(void) {
    return g_server->game.mode && g_server->game.mode->config.teams;
}

static int net_teammates(const ClientConnection *a, const ClientConnection *b) {
    return a->player && b->player && a->player->team == b->player->team;
}

// KrunkNative (security, doc/v7/command-auth.md): on a gated server (brain tickets, online_mode,
// a password) a connection whose JOIN is not settled yet has no identity. Its chat, "/" commands
// and dev packets (tp, cycle, prmA, clearLog) are refused and logged instead of running as a
// guest's; join_timeout closes it. The log is throttled (one line a second) against floods.
static int net_client_admitted(const ClientConnection *client, const char *what) {
    if (client->acct.join_state == SERVER_JOIN_READY) return 1;
    static double last_log;
    const double now = net_time();
    if (now - last_log >= 1.0) {
        last_log = now;
        server_log("[gameserver] refused %s from %d (%s): join not verified", what, client->id, client->address);
    }
    return 0;
}

// One player chat line to `to`: v7 peers get NET_PACKET_CHAT_V7 ("ch", channel, ..., accountId),
// older peers the v3 NET_PACKET_CHAT.
static void net_send_player_chat(ClientConnection *to, const ClientConnection *from, const int channel, const char *text) {
    NetChatV7Packet packet = {.channel = (int8_t) channel, .account_id = from->acct.account_id};
    char name[sizeof(from->name)];
    net_shown_name(to, from, from->name, name, sizeof(name));
    net_packet_chat(&packet.chat, from->id, NET_CHAT_PLAYER, name, text);
    if (ks_server_peer_v7(to)) net_send_packet(to->fd, NET_PACKET_CHAT_V7, &packet, sizeof(packet));
    else net_send_packet(to->fd, NET_PACKET_CHAT, &packet.chat, sizeof(packet.chat));
}

// io_ct: burst limit (closing the socket like the v3.1.1 server, code 4008),
// 70 characters, blank lines dropped, "/" commands, config.chatInterval (a line sooner than 800 ms after the
// sender's last one is dropped, NET_CHAT_INTERVAL), then "ch" to everyone. v7.1.4 sendPlayerChat (server/index.ts:2065,
// the reference for v10's "ct" [channel, msg], deob-10.0.3.js:257277): the team channel only exists in team
// modes, and its lines go to the sender's team only (the sender included, teammates = the room members whose
// player has the sender's team: no spectator); v3 "ct" is the global channel.
// A connection without a player (a spectator, or one still in the menu before its first spawn) has no team: its
// team-channel line stays on the team channel and reaches the sender alone, never the other team. (The reference
// drops every chat line of such a socket; KrunkNative lets spectators talk on Global, so only the team line
// differs, and the sender sees it echoed instead of lost.)
static int net_handle_chat(ClientConnection *client, const char *payload, const size_t length, const int requested_channel) {
    if (!net_chat_allow(&client->chat_limiter, (float) net_time())) {
        server_log("Kicking %s: chat rate limit exceeded", client->name);
        net_remove_client(client);
        return 0;
    }
    if (!net_client_admitted(client, "chat")) return 1;

    char text[NET_CHAT_TEXT_SIZE];
    if (!net_chat_sanitize(text, sizeof(text), payload, length)) return 1;

    if (chat_command_handle(net_chat_host(), client->id, text)) { // "/" commands answer the sender only
        server_log("[cmd] %s: %s", client->name, text);
        return 1;
    }
    if (!net_chat_spaced(&client->chat_limiter, (float) net_time())) { // config.chatInterval (NET_CHAT_INTERVAL)
        server_log("[chat] %s: dropped, sooner than %.1f s after the last line", client->name, (double) NET_CHAT_INTERVAL);
        return 1;
    }

    const int channel = requested_channel == CHAT_CHANNEL_TEAM && net_team_mode() ? CHAT_CHANNEL_TEAM : CHAT_CHANNEL_GLOBAL;
    server_log("[chat%s] %s: %s", channel == CHAT_CHANNEL_TEAM ? (client->player ? " team" : " team, no team: sender only") : "",
               client->name, text);
    ks_server_on_chat(client, text); // KrunkScript onChatMessage (not for "/" commands)
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *to = &g_server->clients[i];
        if (channel == CHAT_CHANNEL_TEAM && to != client && !net_teammates(client, to)) continue;
        net_send_player_chat(to, client, channel, text);
    }
    return 1;
}

static void net_handle_pong(ClientConnection *client, const NetPacket *packet) {
    if (packet->length != sizeof(NetPingPacket) || !client->ping_seq) return;

    NetPingPacket pong;
    memcpy(&pong, packet->payload, sizeof(pong));
    if (pong.seq != client->ping_seq) return;

    const double now = net_time();
    const double readable_at = client->transport->pong_received_at(client->fd); // datagram arrival / socket readable
    const double received_at = readable_at > client->ping_sent_at && readable_at < now ? readable_at : now;
    client->ping_ms = net_ping_update(client->ping_ms, (float) (received_at - client->ping_sent_at));
    client->ping_seq = 0;
    if (client->player) client->player->ping = (float) client->ping_ms; // io_po: player.ping = smoothed
    client->next_ping_at = now + NET_PING_INTERVAL;

    const NetPingResultPacket result = {.ping_ms = client->ping_ms};
    net_send_packet(client->fd, NET_PACKET_PING_RESULT, &result, sizeof(result));
}

static int net_social_send(void *user, const NetPacketType type, const void *payload, const uint16_t length) {
    const ClientConnection *client = user;
    return net_send_packet(client->fd, type, payload, length);
}

// Social hub frames (shared/include/social.h): reassemble the request, answer it from
// the server's map list, stream the reply back in chunks.
static void net_handle_social(ClientConnection *client, const NetPacket *packet) {
    char *request = NULL;
    uint32_t id = 0;
    if (social_assembly_push(&client->social, packet->payload, packet->length, &request, &id) != 1) return;
    if (brain_enabled()) {
        // a node behind kn_brain: the hub is the brain's social socket and /api routes
        // (v3.1.1 gameserver.ts MM_NODE_MODE=node leaves them to the brain); drop it like
        // socialWs.ts drops what it does not handle
        free(request);
        return;
    }

    SocialMapInfo maps[64];
    const SocialHost host = {.maps = maps, .map_count = social_maps_from_game(&g_server->game, maps, 64)};
    char *reply = social_service_handle(&host, request);
    free(request);
    if (!reply) return;
    social_send_message(NET_PACKET_SOCIAL_REPLY, id, reply, strlen(reply), net_social_send, client);
    free(reply);
}

// One clip of `speaker` to every other connection. v7 peers get NET_PACKET_VOICE_V7 ("vc", audio,
// name, sid, channel, volume, isFinal, accountId) and play it when the channel is -1 or theirs;
// older peers get the v3 NET_PACKET_VOICE. KrunkNative fix: v7's server relays a team-channel clip
// to everyone and only the receivers' own channel filters it, so enemies on their team channel
// heard it too; here team-channel clips (channel 1 in a team mode) reach the speaker's team only.
static void net_relay_voice(const ClientConnection *speaker, const char *audio, const VoiceV7Meta *meta) {
    const int team_only = meta->channel == CHAT_CHANNEL_TEAM && net_team_mode();
    size_t v3_len = 0, v7_len = 0;
    char *v3 = NULL, *v7 = NULL;
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *to = &g_server->clients[i];
        if (to == speaker) continue; // "Skip echoing back to the sender"
        if (team_only && !net_teammates(speaker, to)) continue;
        const int peer_v7 = ks_server_peer_v7(to);
        if (peer_v7 && !v7) v7 = voice_relay_v7_message(meta, speaker->name, audio, &v7_len);
        if (!peer_v7 && !v3) v3 = voice_relay_message(speaker->name, audio, &v3_len);
        const char *msg = peer_v7 ? v7 : v3;
        if (!msg) continue;
        social_send_message(peer_v7 ? (NetPacketType) NET_PACKET_VOICE_V7 : NET_PACKET_VOICE, (uint32_t) speaker->id, msg,
                            peer_v7 ? v7_len : v3_len, net_social_send, to);
    }
    free(v3);
    free(v7);
}

// io_vc (v3.1.1): a clip from a connection that has a player (voice_player) goes to every other
// connection as ["vc", audio, alias, sid]; v7 peers see it on every channel at full volume.
static void net_handle_voice(ClientConnection *client, const NetPacket *packet) {
    char *audio = NULL;
    if (social_assembly_push(&client->voice, packet->payload, packet->length, &audio, NULL) != 1) return;
    // `if (!player || typeof audio !== "string" || !audio.length) return;` and the 256 KiB cap
    const size_t len = strlen(audio);
    if (client->voice_player && len && len <= VOICE_MAX_DATA) {
        const VoiceV7Meta meta = {.channel = CHAT_CHANNEL_ALL, .volume = 1.0f, .account_id = client->acct.account_id};
        net_relay_voice(client, audio, &meta);
    }
    free(audio);
}

// v7.1.4 "vc", dataUrl, channel, micVolume, micQuality (server/index.ts:4241-4263): dropped without
// a player, unless it is a data:audio/ URL of at most 750 000 characters; volume clamped to 0..1.
static void net_handle_voice_v7(ClientConnection *client, const NetPacket *packet) {
    char *msg = NULL;
    if (social_assembly_push(&client->voice_v7, packet->payload, packet->length, &msg, NULL) != 1) return;
    VoiceV7Send sent;
    const char *audio = NULL;
    if (client->voice_player && voice_parse_send_v7(msg, &sent, &audio)) {
        const VoiceV7Meta meta = {.channel = sent.channel, .volume = sent.volume, .is_final = 0, .account_id = client->acct.account_id};
        net_relay_voice(client, audio, &meta);
    }
    free(msg);
}

// packetHandlers.ts requireDev for the dev packets, through the chat commands' gate
// (chat_command_allowed: perms, cheats_open, ChatPeer.dev / premium_host); a denial answers
// ["notify", "DEV only"] (the mod's notify toast).
static int net_require_dev(ClientConnection *client, const ChatCommandPerm perm, const char *label) {
    return net_client_admitted(client, label) && chat_command_allowed(net_chat_host(), client->id, perm, label);
}

// net-bridge's first frame: the browser's real address. Only a loopback peer (the bridge on
// this machine) may claim one, and only before anything else; the claimed address then
// decides IP bans and whether the player is on the server's machine.
static int net_handle_forwarded(ClientConnection *client, const NetPacket *packet) {
    if (client->first_packet_seen || !client->local || !packet->length || packet->length > NET_FORWARDED_ADDRESS_SIZE) {
        server_log("[gameserver] ignored FORWARDED from %d (%s)", client->id, client->address);
        return 1;
    }
    char address[NET_FORWARDED_ADDRESS_SIZE + 1];
    memcpy(address, packet->payload, packet->length);
    address[packet->length] = 0;
    struct in_addr parsed;
    const int v4 = inet_pton(AF_INET, address, &parsed) == 1;
    client->local = v4 ? (ntohl(parsed.s_addr) >> 24) == 127 : !strcmp(address, "::1");
    snprintf(client->address, sizeof(client->address), "%s", address[0] ? address : "?");
    server_log("[gameserver] %d is bridged for %s%s", client->id, client->address, client->local ? " (this machine)" : "");
    if (chat_command_ip_banned(net_chat_host(), client->address)) {
        net_kick_client(client->id, "Banned", "004003");
    }
    return 1;
}

// io_prmA(index): premiumKeys / the N key (index 0 = noclip), gated like /noclip.
static void net_handle_premium_action(ClientConnection *client, const NetPacket *packet) {
    if (packet->length != 1 || !net_client_admitted(client, "prmA")) return;
    chat_command_premium_action(net_chat_host(), client->id, packet->payload[0]);
}

// NET_EXT_KN_SWAP_TEAM (Krunker Strike's Tap-to-Play "Switch Teams", FRVR SWAP_TEAM): the sender takes
// the team it asked for between lives (mode_swap_team: team modes with selTeam, not comp). Ranked's
// roster is fixed, as COMP_SET 't' says. A refused request changes nothing and gets no answer.
static void net_handle_swap_team(ClientConnection *client, const uint8_t *body, const uint16_t length) {
    NetSwapTeamPacket req;
    if (!client || !client->player || length < sizeof(req)) return;
    memcpy(&req, body, sizeof(req));
    if (accounts_ranked_active()) return;
    mode_swap_team(&g_server->game, client->player, req.team);
}

// io_tp(x, y, z): DEV only with perms; finite coordinates; the feet go EYE_OFFSET_Y below
// the given y; position/velocity/ground state reset; the sender gets the ack.
static void net_handle_teleport(ClientConnection *client, const NetPacket *packet) {
    if (packet->length != sizeof(NetTeleportPacket)) return;
    if (!net_require_dev(client, CHAT_PERM_DEV_CHEAT, "tp")) return;
    NetTeleportPacket tp;
    memcpy(&tp, packet->payload, sizeof(tp));
    if (!isfinite(tp.x) || !isfinite(tp.y) || !isfinite(tp.z)) {
        server_log("[gameserver] tp invalid coords: %g %g %g", tp.x, tp.y, tp.z);
        return;
    }
    Player *player = client->player;
    if (!player) return; // io_tp would throw on ctx.player null
    const NetTeleportPacket ack = {tp.x, tp.y - NET_TELEPORT_EYE_OFFSET, tp.z};
    net_player_teleport(player, ack.x, ack.y, ack.z);
    net_send_packet(client->fd, NET_PACKET_TELEPORT_ACK, &ack, sizeof(ack));
    server_log("[gameserver] tp %d -> %g %g %g", client->id, ack.x, ack.y, ack.z);
}

// Test only: KRUNKNATIVE_SERVER_POS_LOG=<file> appends one line per accepted input frame, the
// authoritative feet after it: "seq delta x y z on_ground" (compare with the client's input CSV).
static void net_pos_log_frame(const Player *player, const Input *input) {
    static FILE *log;
    static int opened;
    if (!opened) {
        opened = 1;
        const char *path = getenv("KRUNKNATIVE_SERVER_POS_LOG");
        if (path && path[0]) log = fopen(path, "w");
    }
    if (!log || !player) return;
    fprintf(log, "%d %.5f %.5f %.5f %.5f %d\n", input->seq, input->delta, player->position.x, player->position.y, player->position.z, player->on_ground);
    fflush(log);
}

// One INPUT frame (single or batched): v3.1.1 inputDecode.ts checks in player_accept_input
// (non-finite fields dropped, delta clamped, seq floor, 240-frame cap). Physics only ever runs
// from these inputs: the server never takes a position from the client, so a client-side
// noclip flag or position jump has no effect on the authoritative player.
static void net_handle_input_frame(ClientConnection *client, const NetInputPacket *net_input, const double arrival,
                                   const double received) {
    Input input;
    if (!client->player || !client->player->active) return;
    if (g_server->game.end_timer > 0.0f) return; // io_q: isVoteWindow drops the batch
    net_packet_to_input(&input, net_input);
    input.arrival = arrival;
    input.received = received; // the packet's arrival: exact lag compensation's input clock (LagCompClock)
    if (player_accept_input(client->player, &input)) net_note_input_activity(client, &input); // replays do not count
    net_pos_log_frame(client->player, &input);

    LagCompClock *clock = &client->player->lag_clock;
    if (clock->anomaly_new) {
        clock->anomaly_new = 0;
        server_log("[lagcomp] %s: input clock ran %s real time (summed deltas vs arrivals); arrival-anchored lag "
                   "compensation for the rest of this life", client->name,
                   clock->offset + clock->excess > clock->baseline ? "slower than" : "faster than");
    }
}

// When this client's packet reached the server, on the game.state_now clock that stamps the
// stateHistory (anchored lag compensation, shared/src/player.c): the wall time poll() saw the
// socket readable (TCP; the handler may have waited for the tick's lock) or now, mapped by the
// last tick's start. state_now advances by each tick's wall delta, so state_now - tick_wall
// is constant up to float rounding.
static double net_input_arrival(const ClientConnection *client) {
    const double now = net_time();
    const double readable_at = client->transport->input_received_at(client->fd);
    const double wall = readable_at > 0.0 && readable_at <= now && now - readable_at < 1.0 ? readable_at : now;
    return g_server->game.state_now + (wall - g_server->tick_wall);
}

// "q": every frame of the batch, in order, through the same checks as a single INPUT. The
// client sends a batch once per clientSendRate, so its earlier frames were generated earlier:
// each one arrives "back" by the deltas (seconds, clamped like procInputs) of the frames after it.
static void net_handle_input_batch(ClientConnection *client, const NetPacket *packet) {
    NetInputBatchPacket batch;
    if (!net_packet_read_input_batch(&batch, packet)) return;
    double arrival[NET_INPUT_BATCH_MAX];
    double at = net_input_arrival(client);
    for (int i = batch.count - 1; i >= 0; i--) {
        arrival[i] = at;
        const float delta = batch.frames[i].delta;
        if (isfinite(delta) && delta > 0.0f) at -= MIN(delta, game_constants.max_delta);
    }
    const double received = arrival[batch.count ? batch.count - 1 : 0];
    for (int i = 0; i < batch.count; i++) net_handle_input_frame(client, &batch.frames[i], arrival[i], received);
}

// ENT after its fields were stored (also replayed by accounts.c once a deferred join settled).
void net_resume_ent(ClientConnection *client, const int spectate) {
    // v7.1.4 "vc" relays whatever findSocketPlayer finds: every ENT adds a player, a spectator's
    // too (player.spectating), and a dead player keeps it. v3.1.1 left ctx.player null for
    // spectators, which this server copied: spectators could not be heard (v7's muteSpec setting
    // exists to silence them on the receiving side).
    client->voice_player = 1;
    if (spectate) {
        net_start_spectating(client);
    } else {
        net_request_spawn(client);
        // A spectator plays again once it actually spawned (not on the end screen).
        if (client->player && client->player->active) net_set_spectating(client, 0);
    }
}

// Packets only a player's own action sends (a click, a key, a chat line): each one restarts the
// idle clock, alive, dead or in the menu (the respawn click is the ENT). Not: what clients send
// on their own (HELLO, JOIN, PONG, map / social / script traffic) and input frames, which count
// only when something is held or the view turned (net_note_input_activity).
static int net_packet_is_activity(const uint8_t type) {
    switch (type) {
        case NET_PACKET_ENT:
        case NET_PACKET_STREAK_SLOTS:
        case NET_PACKET_PC_STAT:
        case NET_PACKET_CHAT_SEND:
        case NET_PACKET_CHAT_V7:
        case NET_PACKET_MATCH_VOTE:
        case NET_PACKET_COMP_SET:
        case NET_PACKET_SPECTATE_TARGET:
        case NET_PACKET_SPRAY_V7:
        case NET_PACKET_SPAWN_REQ:
        case NET_PACKET_TAUNT:
        case NET_PACKET_USE_STREAK:
        case NET_PACKET_INTERACT:
        case NET_PACKET_CONFIRM_INTERACT:
        case NET_PACKET_PREMIUM_ACTION:
        case NET_PACKET_TELEPORT:
        case NET_PACKET_CYCLE:
        case NET_PACKET_VOICE_SEND:
        case NET_PACKET_VOICE_V7:
            return 1;
        default:
            return 0;
    }
}

// v10 ping (NET_EXT_MARK_POSITION, "mpos"): tmpPlayer.markPosition (:170189) sends the camera ray's
// first hit when the game is ranked or comp (:221125) and the player is alive. Every other connection
// gets it as addPingIcon(key, position) (:160954) with key = the pinging connection's id. Nothing is
// echoed to the sender (v10's client draws no local marker either).
static void net_handle_mark_position(ClientConnection *client, const uint8_t *body, const uint16_t body_length) {
    const Game *game = &g_server->game;
    if (!(game->is_ranked || game->config.comp) || body_length < sizeof(NetMarkPositionSendPacket)) return;
    if (!client->player || !client->player->active) return;
    NetMarkPositionSendPacket hit;
    memcpy(&hit, body, sizeof(hit));
    if (!isfinite(hit.x) || !isfinite(hit.y) || !isfinite(hit.z)) return;
    NetMarkPositionPacket ping;
    memset(&ping, 0, sizeof(ping));
    snprintf(ping.key, sizeof(ping.key), "%d", client->id);
    ping.x = hit.x;
    ping.y = hit.y;
    ping.z = hit.z;
    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(ping)];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_MARK_POSITION, &ping, sizeof(ping));
    if (!n) return;
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *other = &g_server->clients[i];
        if (other == client || !net_peer_takes(other, NET_PACKET_EXT)) continue;
        net_send_packet(other->fd, NET_PACKET_EXT, buf, n);
    }
}

// Returns 0 when the client was removed.
int net_handle_packet(ClientConnection *client, const NetPacket *packet) {
    if (packet->type != NET_PACKET_FORWARDED) client->first_packet_seen = 1;
    if (net_packet_is_activity(packet->type)) net_note_activity(client);
    switch (packet->type) {
        case NET_PACKET_HELLO:; // ";": a declaration may not follow a label before C23 (GCC 10 on bullseye)
            NetHelloPacket hello;
            if (net_packet_read_hello(&hello, packet)) { // the peer's protocol minor.patch (v7 AI / WP-C gates)
                client->peer_minor = hello.major == NET_PROTOCOL_MAJOR ? hello.minor : 0;
                client->peer_patch = hello.major == NET_PROTOCOL_MAJOR ? hello.patch : 0;
                // its ruleset mask (shared.h NET_HELLO_RULESETS) is not read: there is one Krunker
                // version, and no HELLO is refused for it (net-bridge sends none).
                // Krunker Strike: the high nibble carries the peer's product flavor. A peer that
                // announces a non-base flavor different from ours is refused (never silently mixed);
                // a base / absent nibble is accepted (net-bridge, the server-list probe, a base
                // client — a non-base client refuses us from our own INIT instead).
                const GameFlavor peer_flavor = (GameFlavor) NET_WIRE_FLAVOR(hello.rulesets);
                client->peer_flavor = (unsigned char) peer_flavor; // the Strike kill card's flag goes to Strike peers only
                if (peer_flavor != FLAVOR_BASE && peer_flavor != flavor_get()) {
                    char msg[sizeof(((NetErrorPacket *) 0)->message)];
                    snprintf(msg, sizeof(msg), "This is a %s server", flavor_display_name(flavor_get()));
                    server_log("Refusing client %d: peer flavor %u, server is %s", client->id,
                               (unsigned int) peer_flavor, flavor_key(flavor_get()));
                    net_kick_client(client->id, msg, NET_ERROR_CODE_FLAVOR);
                    break;
                }
            }
            ks_server_on_hello(client, packet); // the peer's protocol minor (KrunkScript packet gate)
            if (client->peer_minor >= NET_PROTOCOL_MINOR_V7 && g_server->game.ready) { // WP-E: its INIT went out before the HELLO
                game_config_send(&g_server->game, client->id);
                comp_lobby_send_all(&g_server->game, client->id);
                net_send_social_caps(client);
                net_send_ranked_match(client); // the ranked flag too: an EXT peer's INIT went out before its HELLO
                if (g_server->game.end_timer > 0.0f) game_send_match_votes(&g_server->game, client->id);
            }
            if (client->peer_minor >= NET_PROTOCOL_MINOR_EXT && g_server->game.ready) {
                net_send_game_state(client); // its INIT went out first
                ks_entities_send_state(&g_server->game, client->id); // the entities its INIT's EXTs could not carry yet
                brain_events_send_init(client); // the map's Turf Wars clan (init syncData.warsClan)
            }
            // the game announcement: a gated join gets it when it settles (accounts.c acc_set_ready);
            // an ungated one is settled from its connect, before this HELLO named its protocol
            if (client->acct.join_state == SERVER_JOIN_READY) net_send_game_announcement(client);
            if (client->join_spawn_due) { // the joiner's own dead SPAWN, now that its version is known
                client->join_spawn_due = 0;
                net_send_join_spawn(client);
            }
            break;
        case NET_PACKET_MATCH_VOTE: { // WP-E "maVote" "mode,map" (end screen; players and spectators)
            NetMatchVotePacket vote;
                net_send_event_index(client); // init [2] eventIndex likewise (the offline sandbox's INIT beats its HELLO)
            if (packet->length != sizeof(vote) || client->peer_minor < NET_PROTOCOL_MINOR_V7) break;
            memcpy(&vote, packet->payload, sizeof(vote));
            game_vote_match(&g_server->game, client->id, vote.mode_id, vote.map_index);
            break;
        }
        case NET_PACKET_COMP_SET: { // v7 updSyncDat: "cd" key, value -> game.updLobbyP (comp lobbies)
            NetCompSetPacket set;
            if (packet->length != sizeof(set) || client->peer_minor < NET_PROTOCOL_MINOR_V7) break;
            memcpy(&set, packet->payload, sizeof(set));
            if (set.key == 't' && accounts_ranked_active()) { // the roster's teams are fixed
                accounts_notice(client, "Can't switch while in-game"); // ranked.switch
                break;
            }
            comp_lobby_update(&g_server->game, client->id, (char) set.key, set.value);
            break;
        }
        case NET_PACKET_SCRIPT_MSG: // KrunkScript GAME.NETWORK.send from the client script
            ks_server_on_script_msg(client, packet);
            break;
        case NET_PACKET_FORWARDED: // net-bridge: the browser's address, before its HELLO
            // net_handle_forwarded refuses (and logs) a late or non-loopback claim itself
            net_handle_forwarded(client, packet);
            client->first_packet_seen = 1;
            break;
        case NET_PACKET_PREMIUM_ACTION: // "prmA"
            net_handle_premium_action(client, packet);
            break;
        case NET_PACKET_ENT: {
            NetEntPacket ent;
            if (!net_packet_read_ent(&ent, packet)) break;

            client->class_index = ent.class_index;
            client->secondary_index = ent.secondary_index;
            client->cosmetics = ent.cosmetics;
            client->slid_cont = ent.slid_cont;
            client->spray_index = ent.spray_index;
            client->skins = ent.skins;
            // gameserver.ts applyLagCompFromEnt: a finite ent[11] clamped to 0.1..1 (never 0: a client
            // must not turn off the rewind of its targets); otherwise the previous value stays
            if (isfinite(ent.lag_comp)) client->lag_comp = fminf(1.0f, fmaxf(0.1f, ent.lag_comp));
            if (client->player) client->player->lag_comp = client->lag_comp;
            // WP-C (ENT V8): attachIndex, challengeMode, anonMode (server/index.ts parseEnterData), applied at the spawn
            client->attach_index = ent.attach_index >= -1 && ent.attach_index < 256 ? ent.attach_index : DEFAULT_ATTACHMENT_INDEX;
            client->chall_mode = ent.chall_mode;
            client->anon_mode = ent.anon_mode;
            client->cosmetics_v7 = ent.cosmetics_v7; // WP-F (ENT V9)
            client->show_badges = ent.show_badges;

            int spectate = ent.spectate;
            if (!spectate && !accounts_clan_war_allowed(client)) { // v10 mode.clanWar: the teams are the two clans
                const char *label = accounts_clan_war_label();
                char text[NET_CHAT_TEXT_SIZE];
                snprintf(text, sizeof(text), "%s: only members of these clans can play. Spectating.", label ? label : "Clan War");
                accounts_notice(client, text);
                spectate = 1;
            }
            if (spectate && accounts_ranked_active()) {
                accounts_notice(client, "Spectating Disabled in Ranked"); // ranked.spectate
                spectate = 0;
            }
            // brain-linked: the first ENT waits for the JOIN / account lookup (server/src/accounts.c)
            if (accounts_gate_ent(client, spectate)) net_resume_ent(client, spectate);
            break;
        }
        case NET_PACKET_JOIN:
            accounts_handle_join(client, packet);
            break;
        case NET_PACKET_SPECTATE_TARGET: {
            const int index = net_spectator_index(client->id);
            if (packet->length != sizeof(NetSpectateTargetPacket) || index < 0) break;

            NetSpectateTargetPacket target;
            memcpy(&target, packet->payload, sizeof(target));
            g_spectators[index].target_uid = target.uid;
            break;
        }
        case NET_PACKET_CHAT_SEND:
            if (packet->length && packet->length <= sizeof(NetChatSendPacket))
                return net_handle_chat(client, (const char *) packet->payload, packet->length, CHAT_CHANNEL_GLOBAL);
            break;
        case NET_PACKET_CHAT_V7: // WP-E: "ct", channel, msg (getChatTeam: 1 is the team channel, anything else global)
            if (packet->length > 1 && packet->length <= sizeof(NetChatV7SendPacket))
                return net_handle_chat(client, (const char *) packet->payload + 1, packet->length - 1u, packet->payload[0] == 1 ? CHAT_CHANNEL_TEAM : CHAT_CHANNEL_GLOBAL);
            break;
        case NET_PACKET_SPRAY_V7: { // WP-E: "spry"[, wheel index] (handleSpray)
            NetSprayRequestPacket spray = {.index = -1};
            if (!client->player || packet->length != sizeof(spray)) break;
            memcpy(&spray, packet->payload, sizeof(spray));
            game_player_spray_request(&g_server->game, client->player, spray.index); // canUseSkin: net_game_can_use_skin
            break;
        }
        case NET_PACKET_SPAWN_REQ: { // KrunkNative Object Manager (F1 spawn menu), v7 peers
            if (!ks_server_peer_v7(client)) break;
            if (packet->length >= sizeof(NetSpawnEditPacket)) { // OM_ACTION_EDIT: the request + a pose [+ a field patch]
                NetSpawnEditPacket edit;
                memcpy(&edit, packet->payload, sizeof(edit));
                if (edit.req.action != OM_ACTION_EDIT) break;
                const ObjectManagerPerms perms = net_om_perms(client);
                NetSpawnResultPacket result;
                if (packet->length == sizeof(NetSpawnEditPacket)) object_manager_edit(&g_server->game, client->player, &edit, &perms, &result);
                else object_manager_edit_fields(&g_server->game, client->player, &edit, (const char *) packet->payload + sizeof(edit),
                                                packet->length - sizeof(edit), &perms, &result); // OM_EDIT_FIELDS (the Properties tool)
                net_send_packet(client->fd, NET_PACKET_SPAWN_RESULT, &result, sizeof(result));
                break;
            }
            if (packet->length != sizeof(NetSpawnReqPacket)) break;
            NetSpawnReqPacket req;
            memcpy(&req, packet->payload, sizeof(req));
            net_handle_object_manager(client, &req);
            break;
        }
        case NET_PACKET_TAUNT: // WP-E: "prpt", i (handleModeTaunt -> players.taunt)
            if (client->player && packet->length == sizeof(NetTauntPacket)) game_player_taunt(&g_server->game, client->player, packet->payload[0]);
            break;
        case NET_PACKET_STREAK_SLOTS: { // WP-B: v7 ENT streakIndexes (streakIndex1..3), before the ENT
            if (packet->length != sizeof(NetStreakSlotsPacket)) break;
            const NetStreakSlotsPacket *slots = (const NetStreakSlotsPacket *) packet->payload;
            for (int i = 0; i < STREAK_SLOTS; i++) client->streak_indexes[i] = slots->indexes[i];
            client->streak_indexes_set = 1;
            if (client->player) player_set_streak_indexes(client->player, client->streak_indexes);
            break;
        }
        case NET_PACKET_PC_STAT: // WP-E: v7 ENT pcStatIndex (the killer's player card stat on the death screen), before the ENT
            if (packet->length != sizeof(NetPcStatPacket)) break;
            client->pc_stat_index = (int8_t) packet->payload[0];
            if (client->pc_stat_index < -1 || client->pc_stat_index >= PLAYER_CARD_STAT_COUNT) client->pc_stat_index = -1;
            if (client->player) accounts_player_card(client, client->player);
            break;
        case NET_PACKET_USE_STREAK: // WP-B: "k<slot>" -> players.checkStreak (v7.1.4 sandbox semantics)
            if (client->player && packet->length == sizeof(NetUseStreakPacket)) player_check_streak(client->player, packet->payload[0]);
            break;
        case NET_PACKET_INTERACT: { // io_int: v7 "int" type, uid / "intSec" (WP-C), or the v3 "int" action
            if (!client->player || !packet->length) break;
            if (packet->length == sizeof(NetInteractV7Packet)) {
                NetInteractV7Packet interact;
                memcpy(&interact, packet->payload, sizeof(interact));
                if (interact.action == 1 && interact.type == GAME_INTERACT_TRADE && !interact.secondary)
                    trade_handle_interact(client, interact.uid); // "int" [5, sid]: the trade prompt (deob-10.0.3.js:260125)
                else if (interact.action == 1 && interact.type == GAME_INTERACT_SPINNER && !interact.secondary)
                    brain_events_spinner_interact(client, interact.uid); // "int" [9, uid]: a map prize wheel (deob-10.0.3.js:197177)
                else if (interact.action == 1) game_player_interact_v7(&g_server->game, client->player, interact.type, interact.uid, interact.secondary);
                else if (!net_drop_extra(client, interact.action)) game_player_interact(&g_server->game, client->player, interact.action, interact.secondary);
                break;
            }
            NetInteractPacket interact = {0};
            if (packet->length > sizeof(interact)) break;
            memcpy(&interact, packet->payload, packet->length);
            if (!net_drop_extra(client, interact.action)) game_player_interact(&g_server->game, client->player, interact.action, interact.secondary);
            break;
        }
        case NET_PACKET_CONFIRM_INTERACT: { // "cnfm" (WP-C): confirmInteract
            NetConfirmPacket confirm = {0};
            if (!client->player || !packet->length || packet->length > sizeof(confirm)) break;
            memcpy(&confirm, packet->payload, packet->length);
            if (confirm.kind == 2) brain_events_spinner_confirm(client); // ["cnfm", 2]: the pending prize wheel spin (brain_events.c)
            else game_player_confirm_interact(&g_server->game, client->player, confirm.kind);
            break;
        }
        case NET_PACKET_PONG:
            net_handle_pong(client, packet);
            break;
        case NET_PACKET_MAP_REQUEST:
            net_handle_map_request(client, packet);
            break;
        case NET_PACKET_INPUT: {
            NetInputPacket net_input;
            if (net_packet_read_input(&net_input, packet)) {
                const double at = net_input_arrival(client);
                net_handle_input_frame(client, &net_input, at, at);
            }
            break;
        }
        case NET_PACKET_INPUT_BATCH: // "q"
            net_handle_input_batch(client, packet);
            break;
        case NET_PACKET_CYCLE: // io_cycle -> cmd_cycle: requireDev, like /cycle
            if (!packet->length && net_require_dev(client, CHAT_PERM_DEV, "cycle")) net_cycle_map();
            break;
        case NET_PACKET_TELEPORT: // "tp" (the mod's Ctrl TP)
            net_handle_teleport(client, packet);
            break;
        case NET_PACKET_CLEAR_LOG: // "clearLog" (the mod's F1): io_clearLog
            // KrunkNative (security, doc/v7/command-auth.md): v3.1.1 has no permission check, so any
            // guest could clear the operator's console (terminal escapes into stdout / the journal).
            // DEV only, like the mod's other dev keys.
            if (!packet->length && net_require_dev(client, CHAT_PERM_DEV, "clearLog")) server_log_clear();
            break;
        case NET_PACKET_SOCIAL_REQUEST:
            net_handle_social(client, packet);
            break;
        case NET_PACKET_VOICE_SEND:
            net_handle_voice(client, packet);
            break;
        case NET_PACKET_VOICE_V7:
            net_handle_voice_v7(client, packet);
            break;
        case NET_PACKET_EXT: {
            // v10+ envelope (net_ext.h). chrsp, dlnkW and joinedRank land with their features;
            // unknown subtypes are ignored.
            uint16_t subtype, body_length;
            const uint8_t *body;
            if (client->peer_minor < NET_PROTOCOL_MINOR_EXT || !net_ext_unpack(packet->payload, packet->length, &subtype, &body, &body_length)) break;
            if (subtype == NET_EXT_JOINED_RANK) accounts_handle_joined_rank(client);                      // "joinedRank" (accounts.c)
            else if (subtype == NET_EXT_KN_ACCOUNT_AUTH) accounts_handle_live_auth(client, body, body_length); // live sign-in (accounts.c)
            else if (subtype == NET_EXT_KN_GUEST_ID) accounts_handle_guest_id(client, body, body_length);   // a guest's unclaimed rewards (accounts.c)
            else if (subtype == NET_EXT_VOTE_KICK) votekick_handle(client, body, body_length);           // "v2k" (votekick.c)
            else if (subtype == NET_EXT_ADMIN_ACTION) votekick_handle_admin(client, body, body_length); // ADMIN_ACTIONS (votekick.c)
            else if (subtype == NET_EXT_KPD_CALL) votekick_handle_kpd(client, body, body_length);        // "kpd" (votekick.c)
            else if (subtype == NET_EXT_KPD_RATE) votekick_handle_kpd_rate(client, body, body_length);   // "rkpd" (votekick.c)
            else if (subtype == NET_EXT_KPD_APPLY) votekick_handle_kpd_apply(client, body, body_length); // "kpdA" (votekick.c)
            else if (subtype == NET_EXT_KPD_LIST) votekick_handle_kpd_list(client, body, body_length);   // "kpd" tab 4 / 5 (votekick.c)
            else if (subtype == NET_EXT_MARK_POSITION) net_handle_mark_position(client, body, body_length); // "mpos": a ranked / comp ping
            else if (subtype == NET_EXT_PROP_PICK && body_length >= sizeof(NetPropPickPacket) && client->player) { // "prp" (prop_hunt.h)
                NetPropPickPacket pick;
                memcpy(&pick, body, sizeof(pick));
                game_prop_pick(&g_server->game, client->player, pick.prop_id);
            } else if (subtype == NET_EXT_PROP_ROTATE && body_length >= sizeof(NetPropRotatePacket) && client->player) // "prpR" [reset]
                game_prop_rotate(&g_server->game, client->player, body[0] != 0);
            else if (subtype == NET_EXT_KN_SWAP_TEAM) net_handle_swap_team(client, body, body_length); // Tap-to-Play Switch Teams
            else if (trade_handle_ext(client, subtype, body, body_length)) net_note_activity(client); // the live trade (trade.c)
            else if (subtype == NET_EXT_CAPTCHA_ANSWER) game_captcha_on_answer(client, body, body_length); // "cptR" (game_captcha.c)
            else if (subtype == NET_EXT_CHARGE_CONSENT_RESPONSE) ks_server_charge_response(client, body, body_length); // "chrsp" (ks_server_host.c)
            else if (subtype == NET_EXT_STALKER_TAUNT && body_length >= sizeof(NetStalkerTauntPacket) && client->player) // "stlkrt" i (stalker.h)
                game_player_stalker_taunt(&g_server->game, client->player, body[0]);
            break;
        }
        default:
            on_client_event(client);
            break;
    }

    return 1;
}

int net_poll_client(ClientConnection *client) {
    return client->transport->poll(client);
}

void net_poll_clients(void) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        if (!net_poll_client(&g_server->clients[i])) i--;
    }
    net_process_pending_kicks(); // chat /kick, /ban, /banip (and the CLI running them)
}

// v10 setGameState's ranked branch (game.deob.js:191765-191816): on every phase change of a ranked
// match the roster's names are pushed again (the masked ones before IN_PROGRESS, the real ones
// after), each player seeing its own name. NET_EXT_PLAYER_NAME (0x0203) is the rename packet.
static void net_game_state_changed(Game *game, const GameState state) {
    (void) state;
    if (!game->is_ranked) return;
    for (size_t r = 0; r < g_server->client_count; r++) {
        ClientConnection *to = &g_server->clients[r];
        if (to->peer_minor < NET_PROTOCOL_MINOR_EXT) continue;
        for (size_t i = 0; i < game->player_count; i++) {
            const Player *player = game->players[i];
            if (!player) continue;
            NetPlayerNamePacket rename;
            memset(&rename, 0, sizeof(rename));
            rename.uid = player->uid;
            net_shown_name(to, net_client_of_player(player), player->name, rename.name, sizeof(rename.name));
            uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(rename)];
            const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_PLAYER_NAME, &rename, sizeof(rename));
            if (n) net_send_packet(to->fd, NET_PACKET_EXT, buf, n);
        }
    }
}

void net_install_game_hooks(void) {
    // Match events leave the shared game through these hooks (see GameHooks).
    g_server->game.hooks.user = g_server;
    g_server->game.hooks.state_changed = net_game_state_changed; // v10 setGameState: ranked names
    votekick_init(&g_server->game); // hooks.vote_kick_expired, fresh ban / cooldown lists
    g_server->game.hooks.send = net_game_send;
    g_server->game.hooks.server_line = net_game_server_line; // a bot's join / leave (bots.c): the form each peer reads
    g_server->game.hooks.server_names = net_game_server_names; // a line naming players (chat lists, finish lines): the same
    g_server->game.hooks.chat_names = net_game_chat_names; // a bot's line naming another player (its kill and round comments)
    g_server->game.hooks.next_round = net_game_next_round;
    g_server->game.hooks.respawn = net_game_respawn;
    g_server->game.hooks.reward = net_game_reward; // endGame player.reward (accounts.c)
    g_server->game.hooks.progress = net_game_progress; // WP-E part 2c: stats / challenge tallies (accounts.c)
    g_server->game.hooks.give_skin = net_game_give_skin; // season-system: found skins (accounts.c)
    g_server->game.hooks.junk_credit = net_game_junk_credit; // Junk wallet: Reward Junk and Junk Packages (accounts.c)
    g_server->game.hooks.kill = net_game_kill;           // v10 Turf Wars clan kills (accounts.c)
    g_server->game.hooks.alloc_uid = net_game_alloc_uid; // bot players (shared/src/bots.c)
    g_server->game.hooks.bot_banter = bot_banter_request;  // bot chat through kn_brain's AI (server/src/bot_banter.c)
    g_server->game.hooks.can_use_skin = net_game_can_use_skin; // players.spray canUseSkin (accounts.c)
    ks_storage_install_game_hooks(&g_server->game); // WP6: Save / Load Data, Reward KR (ks_storage.c)
}

// kickIdleSocket: ["error", "Kicked for inactivity", "000005"], then close.
static void net_kick_idle(ClientConnection *client, const char *phase) {
    NetErrorPacket error = {.message = "Kicked for inactivity", .code = "000005"};
    net_send_packet(client->fd, NET_PACKET_ERROR, &error, sizeof(error));
    server_log("Kicking %s for inactivity %s", client->name, phase);
    net_remove_client(client);
}

// Who the idle kick skips. Krunker v7.1.4 has no server idle kick; its only one is the
// client's menu kick (index.js gameLoop), which skips the game host (isGameHost), developer
// accounts and noKick modes. The server kick (a v3.1.1 gameserver.ts feature KrunkNative keeps)
// follows the same exemptions: DEV peers (the CLI op, a DEV account, a player on the server's
// own machine: whoever hosts a local server), the custom lobby's host account, and modes with
// noKick (LMS / Prop Hunt / Infected...: the dead wait for the round there).
static int net_idle_exempt(const ClientConnection *client) {
    const GameModeConfig *mode = g_server->game.mode ? &g_server->game.mode->config : NULL;
    return net_client_is_dev(client) || accounts_is_custom_host(client) || (mode && mode->no_kick);
}

// The server idle kick, after gameserver.ts checkIdleTimeouts (v3.1.1) but with v7.1.4's idea of
// activity. v3.1.1 only counted a 1-unit move, so a player shooting or aiming from one spot, or
// strafing in place, was kicked; v7.1.4's idle timer (controls.js / index.js CONTROLS.idleTimer)
// restarts on every key event and mouse move. Here the clock (last_significant_move_at)
// restarts on the connect, every (re)spawn, a map change (markPlayerUnspawned), the death, a
// 1-unit move, every input frame with anything held or pressed or a turned view
// (net_note_input_activity), every shot the server saw the player fire, damage dealt or taken,
// and every packet only a player's own action sends (ENT = the respawn click, chat, votes,
// sprays...: net_packet_is_activity).
//   alive:                      kicked after idle_kick_seconds without any of that
//   dead / before the spawn:    kicked after idle_menu_kick_seconds (0 = never). v7 kicks from
//     the menu itself, on the client (menu_ui.c, config.kickTimer 120 s, public lobbies); this is
//     the backstop for clients that do not (older builds, bridged browsers that lost focus).
// Never kicked: spectators, the end screen (players.clear() leaves no player to check), the
// exemptions above.
void net_check_idle_clients(const float now) {
    // mode.maxNoMov (Trade 180 s, deob-10.0.3.js:119709): the mode's own no-movement kick in game, where
    // the server kicks at all (idle_kick_seconds > 0); the v10 client never reads it (its kick is the menu's)
    const GameModeConfig *idle_mode = g_server->game.mode ? &g_server->game.mode->config : NULL;
    const float timeout = g_server->config.idle_timeout > 0.0f && idle_mode && idle_mode->max_no_mov > 0.0f
                              ? idle_mode->max_no_mov : g_server->config.idle_timeout;
    const float menu_timeout = g_server->config.idle_menu_timeout;
    if (timeout <= 0.0f && menu_timeout <= 0.0f) return;

    const float threshold = g_server->config.idle_move_threshold;
    const float threshold_sq = threshold * threshold;

    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        Player *player = client->player;

        if (g_server->game.end_timer > 0.0f || net_idle_exempt(client)) {
            client->last_significant_move_at = now; // a fresh clock if the exemption ends (host change, mode change)
            continue;
        }

        if (!player || !player->active) {
            if (net_is_spectating(client)) continue; // spectators are never idle-kicked (index.js: !window.Bf5)
            if (client->has_idle_position) { // died since the last check: the menu's clock starts (showMenu idleTimer = 0)
                client->has_idle_position = 0;
                client->last_significant_move_at = now;
            }

            if (menu_timeout > 0.0f && now - client->last_significant_move_at >= menu_timeout) {
                net_kick_idle(client, client->spawned_at > 0.0f ? "in game (dead)" : "before spawn");
                i--;
            }
            continue;
        }

        if (!client->has_idle_position) {
            net_mark_client_spawned(client);
            continue;
        }

        // the server's own record: a shot fired, damage dealt or taken (a projectile or a hit that
        // lands after the input that caused it, a player standing still under fire)
        if (net_idle_shots(player) != client->idle_shots || player->damage_dealt != client->idle_damage_dealt ||
            player->health < client->idle_health) {
            client->last_significant_move_at = now;
        }
        net_idle_note_actions(client, player);

        const float dx = player->position.x - client->idle_position.x;
        const float dy = player->position.y - client->idle_position.y;
        const float dz = player->position.z - client->idle_position.z;
        const float dist_sq = dx * dx + dy * dy + dz * dz;

        if (dist_sq >= threshold_sq) {
            client->idle_position = player->position;
            client->last_significant_move_at = now;
        }

        if (timeout > 0.0f && now - client->last_significant_move_at >= timeout) {
            net_kick_idle(client, "in game");
            i--;
        }
    }
}

// pi probes: one outstanding ping per client, NET_PING_INTERVAL after each pong
// (a lost probe is retried after twice the interval).
static void net_send_pings() {
    const double now = net_time();

    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        const int stale = client->ping_seq && now - client->ping_sent_at >= NET_PING_INTERVAL * 2.0f;
        if ((client->ping_seq && !stale) || now < client->next_ping_at) continue;

        client->ping_seq = pcg32_random() | 1u;
        client->ping_sent_at = now;

        const NetPingPacket packet = {.seq = client->ping_seq};
        net_send_packet(client->fd, NET_PACKET_PING, &packet, sizeof(packet));
    }
}

// gameserver.ts tick: "l" to every playing connection at NET_RECON_SEND_HZ with the last
// input procInputs drained (none before the first one of a life), and at once after a
// server-driven change of its movement (teleport, changePosition recon, a noclip flip).
// Intentional difference: those snaps ack player->input_seq instead of v3.1.1's
// MAX_SAFE_INTEGER. The state is exactly the one after that input, so the client replays its
// in-flight inputs on top of the new position like the server will, instead of dropping them
// and rubber-banding back once the next 1 Hz "l" arrives.
static void net_send_reconciliation(void) {
    const double now = net_time();
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        Player *player = client->player;
        if (ks_game_flag(&g_server->game, KS_FLAG_NO_SERVER_SYNC)) break; // KrunkScript DEFAULT.disableServerSync
        if (!net_recon_due(player, now, &client->next_recon_at)) continue;

        NetReconPacket packet;
        net_packet_from_recon(&packet, player, player->input_seq);
        // v10 SYNC_PLAYER's movement-scheme sendVals (deob-10.0.3.js:222398): SRM's / Vehicle's state for the
        // same ack, right before the RECON so the client replays from it (EXT peers; others keep their own)
        NetPlayerMoveValsPacket vals = {.ack_seq = packet.ack_seq, .scheme = (int32_t) player_movement_scheme(player)};
        vals.count = player_movement_send_vals(player, vals.vals);
        if (vals.count && client->peer_minor >= NET_PROTOCOL_MINOR_EXT) {
            uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(vals)];
            const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_PLAYER_MOVE_VALS, &vals, (uint16_t) sizeof(vals));
            if (n) net_send_packet(client->fd, NET_PACKET_EXT, buf, n);
        }
        // v10 SYNC_PLAYER's controlled entity [eid, tick, state] (deob-10.0.3.js:222404-222405), before the RECON: the
        // client sets it (applyControlledSync) and replays its inputs on it
        if (client->peer_minor >= NET_PROTOCOL_MINOR_EXT) ks_entities_send_controlled(&g_server->game, player, client->id);
        // WP-C: the v7 movement locks (and the grapple) only to 1.3.1+ peers (1.3.0 / net-bridge read exactly 58 bytes)
        const uint16_t size = net_peer_has_wpc(client->peer_minor, client->peer_patch) ? NET_RECON_PACKET_V3_SIZE : NET_RECON_PACKET_V1_SIZE;
        net_send_packet(client->fd, NET_PACKET_RECON, &packet, size);
    }
}

// Per tick (main thread): pings, reconciliation ("l"), then the player sync (Krunker "k",
// NET_STATE_SEND_HZ; none on the end screen).
static void net_broadcast_states_tick(void);

// Per tick: the sync below, then the UDP peers' queued frames leave at once (the net
// thread would only flush them after its next poll wake-up).
void net_broadcast_states() {
    net_broadcast_states_tick();
    server_transport_flush_all();
}

// KrunkNative spectate overlay (shared/include/net_ext.h NET_EXT_KN_BOT_PATH): a spectator who
// follows a bot (NET_PACKET_SPECTATE_TARGET) gets that bot's route, goal and target memory every
// NET_BOT_PATH_INTERVAL_S, nobody else gets anything (client/src/nav_overlay.c draws it with
// show_navmeshes on, hud_spectate.c shows the state line).
static void net_broadcast_bot_paths(void) {
    const GameBots *bots = g_server->game.bots;
    if (!bots || !g_spectator_count) return;
    for (int i = 0; i < g_spectator_count; i++) {
        const int target_uid = g_spectators[i].target_uid;
        if (target_uid < 0) continue;
        const float since = g_server->now - g_spectators[i].bot_path_at; // < 0: the clock restarted, send again
        if (g_spectators[i].bot_path_at >= 0.0f && since >= 0.0f && since < NET_BOT_PATH_INTERVAL_S) continue;
        const GameBot *bot = NULL;
        for (int b = 0; b < bots->count && !bot; b++) {
            if (bots->list[b].uid == target_uid && bots->list[b].player) bot = &bots->list[b];
        }
        if (!bot) continue;
        ClientConnection *client = NULL;
        for (size_t c = 0; c < g_server->client_count && !client; c++) {
            if (g_server->clients[c].id == g_spectators[i].id) client = &g_server->clients[c];
        }
        if (!client || !net_peer_takes(client, NET_PACKET_EXT)) continue;
        NetBotPathPacket packet;
        const uint16_t body = bot_path_packet_fill(&g_server->game, bot, &packet);
        if (!body) continue;
        uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(packet)];
        const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_KN_BOT_PATH, &packet, body);
        if (n) net_send_packet(client->fd, NET_PACKET_EXT, buf, n);
        g_spectators[i].bot_path_at = g_server->now;
    }
}

// interMProg of a player (player procInputs, deob-10.0.3.js:191508-191541): the charge over the full charge
// (weapon.chrgTime || throwCharge), 0 without one; the same value the player's own HUD draws (build_ui.c)
static float net_charge_progress(const Player *p) {
    if (!p->weapon || g_server->game.config.no_charge_weapons) return 0.0f; // config.chrgWeps
    const float full = p->weapon->charge_time > 0.0f ? p->weapon->charge_time : p->throw_charge > 0.0f ? p->throw_charge : 0.9f; // config.throwCharge
    return p->charge_time > 0.0f ? p->charge_time / full : 0.0f;
}

// v10's server loop (deob-10.0.3.js:222274-222292): an active player's interProg / interMProg, (x || 0).round(3), goes
// as "p" INTER_PROG / "mp" INTER_M_PROG to every connection whose specStream is that player when it changed: here
// NET_EXT_PLAYER_INTER_PROG to each spectator following it (NET_PACKET_SPECTATE_TARGET). Deliberate difference: the
// last value is kept per spectator, not per player, so one that switches to another player gets that one's bars at once.
static void net_broadcast_inter_progress(void) {
    for (int i = 0; i < g_spectator_count; i++) {
        if (g_spectators[i].target_uid < 0) continue;
        const Player *p = game_find_player(&g_server->game, g_spectators[i].target_uid);
        if (!p || !p->active) continue;
        const float value[NET_INTER_BAR_COUNT] = {roundf(p->interact_progress * 1000.0f) / 1000.0f, roundf(net_charge_progress(p) * 1000.0f) / 1000.0f};
        ClientConnection *client = NULL;
        for (size_t c = 0; c < g_server->client_count && !client; c++) {
            if (g_server->clients[c].id == g_spectators[i].id) client = &g_server->clients[c];
        }
        if (!client || !net_peer_takes(client, NET_PACKET_EXT)) continue;
        for (int bar = 0; bar < NET_INTER_BAR_COUNT; bar++) {
            if (value[bar] == g_spectators[i].sent_bar[bar]) continue;
            g_spectators[i].sent_bar[bar] = value[bar];
            const NetInterProgPacket packet = {.bar = (uint8_t) bar, .prog = value[bar]};
            uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(packet)];
            const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_PLAYER_INTER_PROG, &packet, sizeof(packet));
            if (n) net_send_packet(client->fd, NET_PACKET_EXT, buf, n);
        }
    }
}

static void net_broadcast_states_tick(void) {
    net_send_pings();
    if (g_server->game.end_timer > 0.0f) return;
    net_send_reconciliation();
    game_sync_grapples(&g_server->game, NET_TARGET_ALL); // the "k" sync's grapple slot
    net_broadcast_bot_paths(); // KrunkNative: the spectated bot's route (throttled per spectator)
    net_broadcast_inter_progress(); // the followed player's interact / charge bars (v10 "p" / "mp")

    static double next_state_at; // gameserver.ts K_PACKET_INTERVAL_MS
    const double state_now = net_time();
    if (state_now < next_state_at) return;
    // WP-E: the round's networkData send rate (config.serverSendFrequency 10, mode.sendR)
    const NetNetworkDataPacket *rates = game_network_data(&g_server->game);
    next_state_at = state_now + 1.0 / (rates->send_hz ? rates->send_hz : NET_STATE_SEND_HZ);

    NetStatePacket states[256];
    const Player *state_players[256];
    size_t state_count = 0;
    for (size_t j = 0; j < g_server->game.player_count && state_count < sizeof(states) / sizeof(states[0]); j++) {
        Player *player = g_server->game.players[j];
        // syncState leaves out a hidden player (a still stalker, forceHide) for every viewer, spectators
        // too (`!target.isHidden`, deob-10.0.3.js:222346; shared/src/stalker.c)
        if (!player || !player->active || player->is_hidden) continue;
        state_players[state_count] = player;
        net_packet_from_player(&states[state_count++], player);
    }
    // v10's "k" opens with players.hideAll (deob-10.0.3.js:258582): NET_EXT_PLAYER_SYNC heads each EXT peer's sync, an
    // empty one included (updateState sends "k" with an empty list, :222350), so a player it leaves out (viewDis,
    // isHidden) is hidden on that client and snaps when it comes back (client_players_hide_all)
    uint8_t marker[NET_EXT_HEADER_SIZE];
    const uint16_t marker_len = net_ext_pack(marker, sizeof(marker), NET_EXT_PLAYER_SYNC, NULL, 0);

    static NetSendBatch batch; // main thread only
    for (size_t i = 0; i < g_server->client_count; i++) {
        const ClientConnection *client = &g_server->clients[i];
        const int spectating = net_is_spectating(client);
        // v7.1.4 game.js updateState: "k" only to `(player.active || player.spectating) &&
        // !player.locked`. A dead player (death screen and menu until its next SPAWN), one that
        // never spawned (net_client_in_live_world) and a locked one (lockPlayer, the blindfolded
        // seeker) get no player positions: the kill cam looked at the killer once at death
        // (io_killPlayer camLookAt, killer.interpolate = false) and the killer holds still there.
        if (!net_client_in_live_world(client) || (!spectating && client->player->mode.locked)) continue;
        const Player *viewer = spectating ? NULL : client->player; // spectators see everyone
        // a peer before 1.4.5 checks the 57-byte STATE exactly: it gets V1 (no step / ground / grapple / ping)
        const uint16_t size = net_peer_has_state_v2(client->peer_minor, client->peer_patch) ? NET_STATE_PACKET_V2_SIZE : NET_STATE_PACKET_V1_SIZE;
        batch.length = 0;
        if (marker_len && net_peer_takes(client, NET_PACKET_EXT)) net_batch_append(&batch, NET_PACKET_EXT, marker, marker_len);
        else if (!state_count) continue; // a peer without the marker gets nothing from an empty sync, as before
        for (size_t j = 0; j < state_count; j++) {
            if (!game_network_relevant(&g_server->game, viewer, state_players[j])) continue; // WP-E mode.viewDis
            if (net_batch_append(&batch, NET_PACKET_STATE, &states[j], size)) continue;
            net_batch_flush(client, &batch); // full (> ~140 players): send and start the next batch
            net_batch_append(&batch, NET_PACKET_STATE, &states[j], size);
        }
        net_batch_flush(client, &batch);
    }
}
