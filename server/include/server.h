#pragma once
// The server core (kn_server_core): connection table, packet dispatch, broadcast, the game
// tick step and the accounts gate. server_bin links it with its network frontend (TCP / UDP
// sockets, rcon, brain link, finder, CLI: server_cli.h); the client links it as the offline
// sandbox server (server/src/sandbox.c, server_sandbox.h) over an in-memory transport. No GL,
// sockets or threads of its own: the frontend owns the I/O and the threads.
#include <shared.h>
#include <social.h> // social hub requests (SocialAssembly)
#include <voice.h> // voice chat relay (io_vc)
#include <chat_commands.h>
#include <player_card.h> // WP-E: PLAYER_CARD_STAT_COUNT (ServerAccount)
#include <progression.h> // WP-E part 2c: PROGRESSION_STAT_COUNT (ServerAccount)
#include <pthread.h>

#ifdef WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#endif

typedef struct {
    const unsigned char major;
    const unsigned char minor;
    const unsigned char patch;
} ServerVersion;

typedef struct {
    unsigned short tick_rate;
    unsigned short listen_port;
    unsigned short max_players;
    float idle_timeout;
    float idle_move_threshold;
    int perms;                     // --perms / PERMS=1: DEV-only chat commands need an op'd or loopback client
    int cheats_open;               // --open-cheats / gameserver.cheats "all": cheats for everyone while perms are off
} ServerConfig;

#define SERVER_PLAYER_NAME_MAX 32

// Backend identity + match bookkeeping of one connection (server/src/accounts.c,
// doc/backend.md section 8). account_id 0 = guest. Standalone servers never leave
// SERVER_JOIN_READY.
typedef enum {
    SERVER_JOIN_READY = 0,     // identity settled (guest or account): ENT spawns
    SERVER_JOIN_AWAIT,         // brain-linked: waiting for NET_PACKET_JOIN (join_timeout)
    SERVER_JOIN_AUTH,          // JOIN received, waiting for the brain's auth-reply
} ServerJoinState;

#define SERVER_CLASS_STATS_MAX 32 // account.stats c0..c31 (v7.1.4 has 16 classes)
#define SERVER_TALLY_MAX 96       // challenge tallies of one round (key x weapon x streak)

// WP-E part 2c: one challenges.update tally (shared/include/progression.h)
typedef struct {
    char key[8];
    int16_t weapon;            // -1 none
    int16_t streak;            // -1 none
    int value;                 // ADD keys: the sum; MAX / SET keys: the highest / last value
} ServerChallengeTally;

typedef struct {
    int account_id;            // 0 guest
    char username[32];
    int level;                 // 0 guest
    char clan[8];
    unsigned char flags;       // NET_PLAYER_INFO_*
    unsigned char hack:1;      // account.hack: no rewards / stats (game.js)
    unsigned char premium:1;
    int funds;

    unsigned char join_state;  // ServerJoinState
    unsigned char spawn_deferred:1;    // an ENT arrived before the identity was settled
    unsigned char spawn_spectate:1;
    float join_deadline;       // g_server->now
    uint32_t auth_rid;
    char ticket_jti[40];       // seek-game ticket id (player-join releases its reservation)

    float match_since;         // g_server->now of the first spawn in this round, 0 = not playing
    int round_result;          // END result of this round: 1 won, 0 lost, -1 draw / none
    unsigned char committed:1; // this round's stats were already sent

    // dedicated server (doc/backend.md 8.7-8.9)
    unsigned char central_pending:1;   // online_mode: waiting for central hasJoined
    unsigned char central_verified:1;  // online_mode: identity confirmed by central auth
    unsigned char skins_known:1;       // skins_owned holds the coordinator's inventory
    unsigned char password_ok:1;       // a JOIN carried the right server password
    uint8_t skins_owned[1024];         // bitset of owned store.skins indices (8192: v7.1.4 has 6448), enforced on ENT
    int badge;                         // badgeIndex (v7 account stats.bdg, badges.js), -1 none
    // player-viewer WP-E part 2a: the death screen's account values (players.js:4860), read from the
    // brain account at auth: stats["c" + classIndex] and every config.playerCardStats value
    unsigned char has_stats:1;
    int class_stats[SERVER_CLASS_STATS_MAX];
    char pc_stats[PLAYER_CARD_STAT_COUNT][16];
    // player-viewer WP-E part 2c: this round's game.incStat counts (progression.h stat keys) and
    // challenges.update tallies per (key, weapon, streak), committed with the stats and then cleared
    uint32_t stat_counts[PROGRESSION_STAT_COUNT];
    ServerChallengeTally tallies[SERVER_TALLY_MAX];
    int tally_count;
} ServerAccount;

struct ServerTransport;

typedef struct {
    int fd;
    struct sockaddr_in addr;
    const struct ServerTransport *transport; // the frontend that accepted it (set before net_add_client)
    NetPacketBuffer read_buffer;
    int id;
    int player_number;
    int class_index;
    int secondary_index;           // ENT secondary pick (weapon id, -1 default)
    PlayerCosmetics cosmetics;     // ENT skin colour / cosmetic items (none for older ENTs)
    int slid_cont;                 // ENT slidCont (gameserver.ts ent[15]: player.slidCont), 1 for older ENTs
    int spray_index;               // ENT sprayIndex (gameserver.ts ent[1]: player.sprayIndex), 0 for older ENTs
    PlayerSkins skins;             // ENT weapon skins + meleeIndex (ent[2], ent[9]), owned ones only once enforced
    float lag_comp;                // ENT lagComp (ent[11] slider) clamped to 0.1..1, 1 for older ENTs
    int attach_index;              // WP-C ENT V8 attachIndex (ent[12]), DEFAULT_ATTACHMENT_INDEX for older ENTs
    unsigned char chall_mode:1;    // WP-C ENT V8 challengeMode (ent[7])
    unsigned char anon_mode:1;     // WP-C ENT V8 anonMode (ent[19])
    PlayerCosmeticsV7 cosmetics_v7; // WP-F ENT V9: the v7.1.4 cosmetic slots (none for older ENTs; badge unused)
    unsigned char show_badges;     // WP-F ENT V9 showBadges (PLAYER_SHOW_BADGE_*), all for older ENTs
    int streak_indexes[3];         // WP-B: STREAK_SLOTS (v7 ENT streakIndexes), applied at spawn
    unsigned char streak_indexes_set:1; // 0: the default pick (Nuke only)
    int pc_stat_index;             // WP-E: NET_PACKET_PC_STAT (v7 ENT pcStatIndex), -1 none
    char name[SERVER_PLAYER_NAME_MAX];
    char address[CHAT_CMD_ADDRESS_SIZE]; // dotted peer address (chat /ip, /banip)
    unsigned char op:1;            // CLI `op`: passes the DEV checks of chat commands while perms are on
    // On the server's own machine: a loopback TCP peer that is not net-bridge (or a bridged
    // browser whose forwarded address is loopback too). Such a player hosts the server: DEV.
    unsigned char local:1;
    unsigned char first_packet_seen:1; // NET_PACKET_FORWARDED counts only as the first packet
    unsigned char peer_minor;      // HELLO minor of a NET_PROTOCOL_MAJOR peer (0 before / other majors): per-peer packet gates
    unsigned char peer_patch;      // WP-C: its HELLO patch (0 for a 2-byte HELLO): net_peer_has_wpc(peer_minor, peer_patch)

    float connected_at;
    float spawned_at;
    float last_significant_move_at;
    vec3 idle_position;
    unsigned char has_idle_position:1;
    unsigned char joined:1;        // spawned in the current round (respawned by the next round)
    unsigned char announced:1;     // "<name> joined the game" was broadcast for this connection
    unsigned char voice_player:1;  // v3.1.1 ctx.player for io_vc: set by every non-spectate ENT, cleared
                                   // by a spectate ENT, kept across deaths, round ends and map changes

    NetChatLimiter chat_limiter;   // io_ct burst limit
    uint32_t ping_seq;             // seq of the outstanding PING, 0 none
    double ping_sent_at;           // net_time() of that PING
    double next_ping_at;           // net_time() of the next PING
    int ping_ms;                   // smoothed RTT, -1 unknown
    double next_recon_at;          // net_time() of the next periodic RECON ("l", NET_RECON_SEND_HZ)
    SocialAssembly social;         // social hub request being reassembled (shared/include/social.h)
    SocialAssembly voice;          // voice clip being reassembled (shared/include/voice.h, io_vc)
    SocialAssembly voice_v7;       // WP-E: a NET_PACKET_VOICE_V7 clip being reassembled (v7 "vc" with channel)

    Player *player;
    ServerAccount acct;            // backend account + match stats (server/src/accounts.c)
    // WP-C: v7 player.custVals are never reset, but KN frees the players at every map change: the
    // connection keeps them across rounds (net_free_players / net_spawn_client)
    TriggerValueSet custom_values;
} ClientConnection;

typedef struct {
    volatile int should_quit;

    size_t client_count;
    ClientConnection *clients;
    int next_client_id;
    unsigned int used_player_numbers;
    float now;
    double tick_wall;              // CLOCK_MONOTONIC seconds when the running/last game_tick started
    pthread_mutex_t lock;

    ServerConfig config;
    Game game;
} Server;

extern Server *g_server;

// A connection's byte transport: TCP (server/src/net_tcp.c), UDP (net_udp.c) or the
// in-process sandbox (sandbox.c). Every hook runs with g_server->lock held unless noted.
typedef struct ServerTransport {
    const char *name;
    // Read what arrived for the connection and hand each whole packet to net_handle_packet().
    // Returns 0 once the connection was removed (net_handle_packet returned 0, or the peer
    // is gone and the transport called net_remove_client), else 1.
    int (*poll)(ClientConnection *client);
    // Send one tick's whole frames ([type, length hi, length lo, payload]...) to fd.
    void (*send_frames)(int fd, const uint8_t *frames, size_t length);
    void (*close)(int fd);
    // server_time() when the data carrying the latest PONG / INPUT of fd arrived, 0 unknown.
    double (*pong_received_at)(int fd);
    double (*input_received_at)(int fd);
    // Send whatever is queued for every peer now (end of tick), NULL when sends are immediate.
    void (*flush)(void);
} ServerTransport;

// Transports whose flush runs at the end of every tick (net_broadcast_states).
void server_transport_register(const ServerTransport *transport);
void server_transport_unregister(const ServerTransport *transport);
void server_transport_flush_all(void);

// The server's wall clock (seconds): CLOCK_MONOTONIC, or the sandbox's own clock
// (server_set_clock) so an in-process server follows the client's frame time.
double server_time(void);
void server_set_clock(double (*clock)(void)); // NULL = CLOCK_MONOTONIC

// Krunker's game hooks (GameHooks.send / next_round / respawn / reward / alloc_uid) on
// g_server->game. Caller holds g_server->lock.
void net_install_game_hooks(void);
// Every connection's pending packets through net_handle_packet, then the kicks the chat
// commands queued. Caller holds g_server->lock (server_bin's net thread, the sandbox step).
void net_poll_clients(void);
int net_handle_packet(ClientConnection*, const NetPacket*); // 0 = the client was removed
void net_add_client(ClientConnection*); // takes a copy; caller holds g_server->lock

// One server tick (main.c's loop body, the sandbox step): g_server->now / tick_wall, bot
// slots, game_tick, accounts_tick, idle kicks, the per-tick broadcast. Caller holds the lock.
void server_core_tick(float now, float delta, double tick_wall);

void net_broadcast_states();
void net_check_idle_clients(float);
void net_remove_client(ClientConnection*); // disconnect + free; caller holds g_server->lock

// Switch every client to another map/mode (the current players respawn on it).
// Caller holds g_server->lock.
#define SERVER_HAVE_NET_CHANGE_MAP 1
void net_change_map(int map_index, int mode_index);

// Server chat line to every client (CLI `say`): a NET_CHAT_SERVER message.
// Caller holds g_server->lock.
#define SERVER_HAVE_NET_CHAT 1
void net_broadcast_chat(const char *from, const char *msg);

// Krunker v3.1.1 chat "/" commands (shared/src/chat_commands.c) wired to the connections;
// the CLI runs them as CHAT_CMD_CONSOLE. Caller holds g_server->lock.
#define SERVER_HAVE_CHAT_COMMANDS 1
ChatCommandHost *net_chat_host(void);

void on_client_connect(ClientConnection*);
void on_client_disconnect(ClientConnection*);
void on_client_event(ClientConnection*);
