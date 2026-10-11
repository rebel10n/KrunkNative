#pragma once
#include <shared.h>
#include <social.h> // social hub requests (SocialAssembly)
#include <voice.h> // voice chat relay (io_vc)
#include <chat_commands.h>
#include <replxx.h>
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
    uint8_t skins_owned[512];          // bitset of owned store.skins indices (4096), enforced on ENT
} ServerAccount;

typedef struct {
    int fd;
    struct sockaddr_in addr;
    NetPacketBuffer read_buffer;
    int id;
    int player_number;
    int class_index;
    int secondary_index;           // ENT secondary pick (weapon id, -1 default)
    PlayerCosmetics cosmetics;     // ENT skin colour / cosmetic items (none for older ENTs)
    int slid_cont;                 // ENT slidCont (gameserver.ts ent[15]: player.slidCont), 1 for older ENTs
    int spray_index;               // ENT sprayIndex (gameserver.ts ent[1]: player.sprayIndex), 0 for older ENTs
    PlayerSkins skins;             // ENT weapon skins + meleeIndex (ent[2], ent[9]), owned ones only once enforced
    char name[SERVER_PLAYER_NAME_MAX];
    char address[CHAT_CMD_ADDRESS_SIZE]; // dotted peer address (chat /ip, /banip)
    unsigned char op:1;            // CLI `op`: passes the DEV checks of chat commands while perms are on
    // On the server's own machine: a loopback TCP peer that is not net-bridge (or a bridged
    // browser whose forwarded address is loopback too). Such a player hosts the server: DEV.
    unsigned char local:1;
    unsigned char first_packet_seen:1; // NET_PACKET_FORWARDED counts only as the first packet

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

    Player *player;
    ServerAccount acct;            // backend account + match stats (server/src/accounts.c)
} ClientConnection;

typedef struct {
    volatile int should_quit;

    size_t client_count;
    ClientConnection *clients;
    int next_client_id;
    unsigned int used_player_numbers;
    float now;
    pthread_mutex_t lock;

    ServerConfig config;
    Game game;
} Server;

extern Replxx *g_replxx;
extern Server *g_server;

ReplxxActionResult cli_sigint(int, void*);
void cli_main();

void net_main();
void net_cleanup();
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
