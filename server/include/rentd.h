#pragma once
// kn_rentd: the rented servers' supervisor on a game host (doc/v10/rented-servers.md). It polls kn_brain's agent API
// (GET /mm/internal/rentals?host=, services/brain/rentals.c) and keeps one server_bin per rented slot running, with the
// slot's game, until the rental lapses. This header is its pure part (rentd_core.c): the brain's answer, the
// instance's server.cfg, the per-slot decision and the crash backoff. rentd.c is the process loop around it.
#include <stddef.h>

#define RENTD_MAX_SLOTS 32

typedef struct {
    int id, port, region, generation, run;
    char name[64];         // SVN_SID: the server's hostname, the 24/7 row's name
    char public_host[128]; // the address players connect to (empty: the agent's public_host)
    char map[96];          // the game's name (CONF.mn)
    char creator[48];      // its creator (CONF.cr): published:<creator>/<name>
    char key[96];          // a catalogue key (CONF.key), preferred over the name
} RentdSlot;

// What every instance's server.cfg shares (rentd.cfg).
typedef struct {
    char host_id[64];             // this host's id in brain.rentals.pool[].host
    char region[16];              // server.cfg region (the Box Office 2-letter code, VA)
    char listen_host[64];
    char public_host[128];        // default public_host of the instances
    char coordinator_url[256];    // server.cfg coordinator_url (the brain's internal WS)
    char coordinator_secret[160]; // brain.internal_secret: the internal WS and the agent API
    char join_secret[160];
    char auth_url[256];
    char brain_http_url[256];     // the agent API's base (https://coordinator.krunk.io); also server.cfg brain_http_url
    int max_players;
} RentdShared;

// The agent API's answer {now, slots: [...]} -> slots (at most max); -1 when it is not that JSON.
int rentd_parse_slots(const char *json, RentdSlot *out, int max);
// The instance's map argument: the catalogue key, else published:<creator>/<name>, else the name; 0 when the slot
// has no game or its text cannot sit in a quoted server.cfg value.
int rentd_map_arg(const RentdSlot *slot, char *out, size_t size);
// The instance's server.cfg text; -1 when it does not fit or the slot has no game.
int rentd_cfg_text(const RentdSlot *slot, const RentdShared *shared, const char *state_dir, char *out, size_t size);

typedef enum { RENTD_KEEP, RENTD_START, RENTD_STOP, RENTD_RESTART } RentdAction;
// want: the brain's slot (NULL: the slot left this host). alive: a child runs; gen: the generation it runs;
// now / not_before: the backoff after a crash.
RentdAction rentd_decide(const RentdSlot *want, int alive, int gen, double now, double not_before);
// Seconds to wait before the next start after `crashes` consecutive early exits: 1, 2, 4 .. 60.
double rentd_backoff(int crashes);
