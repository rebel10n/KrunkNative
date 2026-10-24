// KrunkScript Game integration internals shared by ks_game.c and ks_hostobj.c (WP3).
#ifndef KS_GAME_INTERNAL_H
#define KS_GAME_INTERNAL_H

#include <ks_game.h>

enum { KS_REF_PLAYER = 1, KS_REF_AI, KS_REF_OBJECT, KS_REF_ENTITY };

// ---------------------------------------------------------------------------
// Entities (v10.0.2 entity manager, L184033-185900; doc/v10/map-format.md section 4): the
// authority's simulation of the map's archetypes. ks_entities.c owns the lifecycle.

#define KS_ENT_STEP_MS 33.333333333333336 // v10 _63176: one simulation tick (ms)
#define KS_ENT_CATCHUP 64                 // a stalled entity is caught up by at most this many ticks
#define KS_ENT_EVENTS 8                   // ctx.emit per step
#define KS_ENT_NAME_MAX 32                // ctx.emit name (sliced)
#define KS_ENT_ERRORS 10                  // consecutive bad results before the entity freezes
#define KS_ENT_FIRST_EID 10000            // script spawns number from here (v10 nextEid)

typedef struct {
    char name[KS_ENT_NAME_MAX + 1];
    double a, b;
} KsEntEvent;

typedef struct KsEntity {
    int eid;
    const MapArchetype *arch;           // the map's archetype (outlives the round)
    char tag[MAP_ENTITY_TAG_MAX + 1];
    double hp;
    int active, frozen;
    int tick;                           // last simulated tick (v10 entity.tick)
    int step_tick;                      // the tick of the step in progress (ctx.tick)
    int errors;                         // v10 errorStreak
    double x, y, z, yaw, pitch, roll;   // pose: the state's x y z yaw pitch roll
    cJSON *state;                       // validated state (num / bool / str values)
    cJSON *params;                      // spawn params (num / bool / str values)
    uint32_t rng;                       // ctx.rand stream of the step in progress
    KsEntEvent events[KS_ENT_EVENTS];   // ctx.emit calls of the step in progress
    int event_count;
} KsEntity;

// Server hooks queued by the manager and drained into the main script after each advance (v10
// queueServerHook / drainServerHooks): onEntitySpawn, onEntityEvent, onEntityDamage, onEntityDestroyed.
enum { KS_ENT_HOOK_SPAWN = 1, KS_ENT_HOOK_EVENT, KS_ENT_HOOK_DAMAGE, KS_ENT_HOOK_DESTROYED };
typedef struct {
    int kind;
    int eid;
    char text[KS_ENT_NAME_MAX + 1];     // archetype name (spawn) or event name
    char doer[24];                      // damage / destroyed: the doer's player id ("" none)
    double a, b;                        // event numbers, or the damage amount in a
} KsEntHook;

typedef struct {
    KsEntity **items;                   // creation order (v10 entities Map); active entities only
    size_t count, cap;
    KsEntHook *queue;
    size_t queue_count, queue_cap, queue_head;
    int next_eid;
    double clock_ms;                    // the round's clock (v10 startTime based), ms
    long tick;                          // floor(clock / KS_ENT_STEP_MS)
    uint32_t seed;                      // v10 matchSeed (ctx.rand streams)
} KsEntities;

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

    // scripts.shared (KS_SIDE_SHARED): its own program and instance over the same host, run by the
    // entity manager (onEntityInit, simulateEntity); the manager state (ks_entities.c)
    KsProgram *shared_prog;
    KsScript *shared_script;
    uint32_t shared_hash;
    KsEntities ents;
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

// ks_pose.c: player.pose (v10.0.3 deob-10.0.3.js:208220-208231): the host property's getter / setter
// (the setter only on the server's script), and the sanitizePose conversion both ways
KsValue ks_player_pose_get(KsScript *s, void *native, uint32_t id);
void ks_player_pose_set(KsScript *s, void *native, uint32_t id, KsValue v);

// ks_entities.c: the entity manager. ks_entities_round_start (game start: despawns the last round's
// entities, spawns the map's ENTITY instances on the authority), ks_entities_update (the authority's
// clock steps, then the queued hooks), ks_entities_reset / free (no hooks), and the GAME.ENTITIES entries.
void ks_entities_round_start(KsGame*);
void ks_entities_update(KsGame*, double delta_ms);
void ks_entities_reset(KsGame*);
void ks_entities_free(KsGame*);
void ks_entities_api_fill(KsApiImpl *api, KsSide side);
// Logs an error line through the host (the script console of the side).
void ks_game_log_error(KsGame*, const char *line);
// The authority's KrunkScript game (server, offline sandbox): the entity manager runs only here.
int ks_game_is_authority_side(const KsGame*);

#endif
