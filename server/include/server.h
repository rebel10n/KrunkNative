#pragma once
#include <shared.h>
#include <replxx.h>

typedef struct {
    const unsigned char major;
    const unsigned char minor;
    const unsigned char patch;
} ServerVersion;

typedef struct {
    int tick_rate;
    int should_quit;
    Game game;
} Server;

extern Replxx *g_replxx;
extern Server *g_server;
