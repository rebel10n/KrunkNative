#pragma once
// Krunker Strike director client (doc/v10/strike-director.md): a Strike-flavor client seeks
// through the director (services/strike/director.c) when one is configured, instead of the
// brain's /mm/seek-game. Falls back to the finder's Strike pool (servers_ui.c) when the director
// cannot place it.
//
// The URL (no trailing slash), first match wins: KRUNKNATIVE_STRIKE_DIRECTOR (native), ?director=
// (web), the `knStrikeDirector` storage key. Only a Strike client (flavor.h) ever uses it.

typedef struct StrikeAllocation {
    int status;          // HTTP status (0 = transport error)
    int ok;              // a usable game server
    char host[128];
    int port;
    char game_id[64];
    char ticket[352];    // NetJoinPacket.client_key
    char region[32];
    char code[16];       // private lobby code ("" = public)
    char error[64];      // the director's error name ("NoAvailableServers", "GameVersionTooOld", ...)
} StrikeAllocation;

typedef void (*StrikeAllocFn)(void *user, const StrikeAllocation *a);

const char *strike_director_url(void);  // "" = none
int strike_director_enabled(void);      // a Strike client with a director URL

// POST /v1/director/allocate {region?, mode?, roomId?} (priv 0), or /allocate/private
// {customMatchId: code} / {roomId} / {} (priv 1). The body carries gameVersion (the protocol).
// 0 = not sent (no director); fn runs on the main thread (backend_poll).
int strike_director_allocate(int priv, const char *region, const char *mode, const char *room, const char *code,
                             StrikeAllocFn fn, void *user);

// Pure: an allocate answer into *out.
void strike_director_parse(int status, const char *body, StrikeAllocation *out);
