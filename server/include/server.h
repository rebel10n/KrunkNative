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
} ServerConfig;

typedef struct {
    int fd;
    struct sockaddr_in addr;
    NetPacketBuffer read_buffer;
    int id;

    Player *player;
} ClientConnection;

typedef struct {
    int should_quit;

    size_t client_count;
    ClientConnection *clients;
    int next_client_id;
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

void on_client_connect(ClientConnection*);
void on_client_disconnect(ClientConnection*);
void on_client_event(ClientConnection*);
