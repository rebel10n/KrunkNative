// KrunkScript Game integration internals shared by ks_game.c and ks_hostobj.c (WP3).
#ifndef KS_GAME_INTERNAL_H
#define KS_GAME_INTERNAL_H

#include <ks_game.h>

enum { KS_REF_PLAYER = 1, KS_REF_AI, KS_REF_OBJECT };

// The stable `native` of a host wrapper: one per (kind, engine id) for the life of the
// KsGame (the engine object itself is looked up by id on every access). Players also keep
// their per-player script state here.
typedef struct KsRef {
    struct KsGame *kg;
    int kind;
    int id;
    // players (3.2.1); reset by ks_game_on_player_spawn
    int allow;                 // KS_PLAYER_* bits, all set by default
    uint16_t disabled;         // disableDefault(input) bits (KS_INPUT_*)
    int sync_count;
    char sync[KS_SYNC_MAX_NAMES][KS_SYNC_NAME_MAX * 4 + 1]; // custSync names (<= 20 UTF-16 units)
} KsRef;

#define KS_PLAYER_ALLOW_ALL (KS_PLAYER_DEFAULT_MOVEMENT | KS_PLAYER_DEFAULT_VELOCITY | KS_PLAYER_DEFAULT_ROTATION | \
                             KS_PLAYER_CAN_SHOOT | KS_PLAYER_CAN_MELEE)

enum { // player.disableDefault names (PLJ:911-946)
    KS_INPUT_MOUSEY = 1 << 0, KS_INPUT_MOUSEX = 1 << 1, KS_INPUT_MOVDIR = 1 << 2, KS_INPUT_LMOUSE = 1 << 3,
    KS_INPUT_RMOUSE = 1 << 4, KS_INPUT_JUMP = 1 << 5, KS_INPUT_RELOAD = 1 << 6, KS_INPUT_CROUCH = 1 << 7,
    KS_INPUT_SCROLL = 1 << 8, KS_INPUT_SWAP = 1 << 9, KS_INPUT_RESTK = 1 << 10, KS_INPUT_INTER = 1 << 11,
};

#define KS_RATE_KEYS 64
typedef struct {
    char key[48];
    int count;
} KsRateCount;

struct KsGame {
    Game *game;
    KsSide side;
    KsProgram *prog;
    KsScript *script;
    const KsHost *host;        // the host's vtable (api table, log, clock)
    void *host_user;
    KsHost wrap;               // what the script sees: log / now forward to `host` with host_user
    uint32_t source_hash;
    int flags;                 // KsGameFlag
    double fixed_delta;        // TIME.fixedDelta (ms)
    const KsHostClass *player_class;
    KsGameAccountFn account;

    KsRef **refs;
    size_t ref_count, ref_cap;

    // f2123 / f2124 counters (fixed wall-clock windows from the script load)
    double rate_t0;
    long long rate_win1, rate_win10;
    KsRateCount rate1[KS_RATE_KEYS], rate10[KS_RATE_KEYS];
    int rate1_count, rate10_count;

    // TIME.freeze: game.oldGameTimer
    int frozen;
};

// F(x) = parseFloat(ToString(x)) || 0 (shared/src/ks/ks_value.c; declared here too for trees whose
// ks.h predates its prototype)
double ks_to_float(KsValue);

KsGame *ks_game_state(Game*, int create);
KsRef *ks_ref_get(KsGame*, int kind, int id);   // find or create (NULL on OOM)
KsRef *ks_ref_find(const KsGame*, int kind, int id);
double ks_game_now_ms(KsGame*);
void ks_ref_reset_player(KsRef*);


// ks_hostobj.c: the shared GAME.* entries filled by ks_game_api_fill
void ks_hostobj_api_fill(KsApiImpl *api, KsSide side);

#endif
