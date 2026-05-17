#pragma once
#include <shared.h>
#include <replxx.h>

#ifdef WIN32
#include <winsock2.h>
#else
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

    Player *player;
} ClientConnection;

typedef struct {
    int should_quit;

    size_t client_count;
    ClientConnection *clients;

    ServerConfig config;
    Game game;
} Server;

extern Replxx *g_replxx;
extern Server *g_server;

ReplxxActionResult cli_sigint(int, void*);
void cli_main();

void net_main();
void net_cleanup();

void on_client_connect(ClientConnection*);
void on_client_disconnect(ClientConnection*);
void on_client_event(ClientConnection*);
