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
} ServerConfig;

#define SERVER_PLAYER_NAME_MAX 32

typedef struct {
    int fd;
    struct sockaddr_in addr;
    NetPacketBuffer read_buffer;
    int id;
    int player_number;
    int class_index;
    int secondary_index;           // ENT secondary pick (weapon id, -1 default)
    PlayerCosmetics cosmetics;     // ENT skin colour / cosmetic items (none for older ENTs)
    char name[SERVER_PLAYER_NAME_MAX];
    char address[CHAT_CMD_ADDRESS_SIZE]; // dotted peer address (chat /ip, /banip)
    unsigned char op:1;            // CLI `op`: passes the DEV checks of chat commands while perms are on

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
    SocialAssembly social;         // social hub request being reassembled (shared/include/social.h)
    SocialAssembly voice;          // voice clip being reassembled (shared/include/voice.h, io_vc)

    Player *player;
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
