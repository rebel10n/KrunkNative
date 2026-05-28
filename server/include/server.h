#pragma once
#include <shared.h>
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
} ServerConfig;

#define SERVER_PLAYER_NAME_MAX 32

typedef struct {
    int fd;
    struct sockaddr_in addr;
    NetPacketBuffer read_buffer;
    int id;
    int player_number;
    int class_index;
    char name[SERVER_PLAYER_NAME_MAX];

    float connected_at;
    float spawned_at;
    float last_significant_move_at;
    vec3 idle_position;
    unsigned char has_idle_position:1;
    unsigned char joined:1;

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

void on_client_connect(ClientConnection*);
void on_client_disconnect(ClientConnection*);
void on_client_event(ClientConnection*);
