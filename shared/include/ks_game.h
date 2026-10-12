// KrunkScript <-> Game integration (doc/v7/krunkscript.md 2.2, 2.4, 4.1, 5, 6.6 WP3).
//
// One script instance per (Game, side): Game.ks (NULL = no script). The server's Game runs
// the map's scripts.server (dedicated server and the offline sandbox: server/src/
// ks_server_host.c), the client's Game runs scripts.client (client/src/ks, WP4). Shared code
// only: no client or server headers, no GL. The hook points below are called from
// shared/src/player.c and game.c (both sides) and from the hosts (connection events).
//
// Every KsApiFn gets `user` = the KsGame (ks_game_of / ks_game_host_user recover the Game
// and the host's own pointer), so the shared both-side API entries work on both sides.
#ifndef KS_GAME_H
#define KS_GAME_H

#include <shared.h>
#include <ks.h>
#include <ks_api.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KsGame KsGame;

// ---------------------------------------------------------------------------
// Lifecycle (2.2)

// Decoded map JSON scripts.client / scripts.server of game->map (base64 or raw, 1.1);
// malloc'd, NUL-terminated (caller frees), NULL when that side has no script.
char *ks_game_map_source(const Game*, KsSide, size_t *len);

// Compiles and loads `src` for `side` (replacing any loaded script: ks_game_unload first),
// seeds UTILS.rand* and runs the global initialisers. A NULL / empty src just unloads.
// Returns 0 when loaded or when there is nothing to load, -1 on a compile or load error
// (logged through host->log; the map keeps running without a script, 1.14). host and
// host_user must outlive the script.
int ks_game_load(Game*, KsSide, const char *src, size_t len, const KsHost *host, void *host_user);
// Server: onServerClosed() first. Resets the GAME.DEFAULT flags, the per-player script state,
// the rate limiters and the trigger hooks; frees the instance. No-op without a script.
void ks_game_unload(Game*);
// Unload plus the per-Game state (player class, refs): call when the Game is torn down.
void ks_game_free(Game*);
// Source identity of the loaded script (FNV-1a of the text, 0 none): the server keeps its
// globals across rounds of the same map script (2.2) and reloads on a change.
uint32_t ks_game_source_hash(const Game*);
uint32_t ks_source_hash(const char *src, size_t len);

KsScript *ks_game_script(const Game*);   // NULL when none or failed
KsSide ks_game_side(const Game*);        // side of the loaded script (KS_SIDE_CLIENT when none)
Game *ks_game_of(KsScript*);             // the Game of an instance loaded by ks_game_load
void *ks_game_host_user(KsScript*);      // host_user given to ks_game_load
// Seed for UTILS.rand* (ks_script_seed); ks_game_load seeds from the map + a host value.
void ks_game_seed(Game*, uint64_t seed);

// Hook calls (one host entry each; runtime errors are contained and logged). Missing hooks
// and missing scripts are no-ops. ks_game_call returns ks_script_call_hook's result (0 ok,
// -1 runtime error / no script).
int ks_game_call(Game*, const char *hook, const KsValue *args, int argc);
int ks_game_has_hook(const Game*, const char *hook);
void ks_game_start(Game*);                                   // start()
// update(delta_ms * delta_mlt); TIME.fixedDelta() returns delta_ms (2.4). delta_mlt <= 0 = 1.
void ks_game_update(Game*, double delta_ms, double delta_mlt);

// ---------------------------------------------------------------------------
// GAME.DEFAULT flags (3.1.2): one-way until the script unloads.
typedef enum {
    KS_FLAG_NO_DEFAULT_BEHAVIOUR = 1, // disablePlayerBehaviour: procInputs skips the default after onPlayerUpdate
    KS_FLAG_NO_PREDICTION = 2,        // disablePrediction: the client neither predicts nor replays
    KS_FLAG_NO_SERVER_SYNC = 4,       // disableServerSync: the server stops the "l" (RECON) packet
    KS_FLAG_DISABLE_3D = 8,           // disable3D (client)
} KsGameFlag;
int ks_game_flag(const Game*, KsGameFlag);
// GAME.TIME.freeze() (3.1.7): the round clock stands still (game.gameTimer = 0 in v7) until
// unfreeze() or the next round.
int ks_game_round_frozen(const Game*);
void ks_game_set_flag(Game*, KsGameFlag, int on);

// ---------------------------------------------------------------------------
// Shared GAME.* entries (6.5): fills `api` (KS_API_COUNT entries, zeroed by the caller) with
// what shared code implements for `side`: PLAYERS.list / findByID, OBJECTS.list /
// getPathNodes, DEFAULT.disablePlayerBehaviour / disablePrediction / disableServerSync,
// TIME.fixedDelta, the RAYCAST / ANIM / TRIGGERS / CONFIG / PAYMENTS.charge / URLS stubs, the
// PLATFORM answers of a server and, on KS_SIDE_SERVER, TIME.freeze / unfreeze and AI.*.
// Hosts add their own entries on top (UTILS / math / TIME.now / log fall back to ks_stdlib).
void ks_game_api_fill(KsApiImpl *api, KsSide side);

// Fixed-window rate limiter of the v7 API context (3.0, API:7-51): per script instance,
// wall-clock windows. Returns 1 when the call may proceed. `per_10s` = 0: f2123(key, n)
// (1-s window, increments first and fails when the count >= limit, so `limit` 10 allows 9);
// 1: f2124(key, n) (10-s window, fails when the count > limit).
int ks_game_rate(Game*, const char *key, int limit, int per_10s);

// ---------------------------------------------------------------------------
// Host objects (3.2, ks_hostobj.c). Wrappers keep native = Game*, native_id = the engine id
// and look the object up on every access: a freed player / AI reads as undefined, nothing
// dangles across rounds. Player ids are decimal uid strings.
extern const KsHostProp ks_player_props[];
extern const int ks_player_prop_count;
extern const KsHostClass ks_player_class;
extern const KsHostClass ks_ai_class;
extern const KsHostClass ks_map_object_class;
// Client extension point: a class made of ks_player_props + client-only members (visible,
// inView, getAsset, playAnim, stopAnim). Set before ks_game_load; NULL = ks_player_class.
void ks_game_set_player_class(Game*, const KsHostClass*);
const KsHostClass *ks_game_player_class(const Game*);
KsValue ks_game_wrap_player(KsScript*, Game*, const Player*);   // null for NULL
KsValue ks_game_wrap_ai(KsScript*, Game*, int ai_sid);
KsValue ks_game_wrap_map_object(KsScript*, Game*, int object_index);
Player *ks_game_player_by_uid(Game*, uint32_t uid);
// The engine object behind the `native` of a ks_player_props / client player-class member
// (getters, setters and methods all receive it): NULL once the player is gone.
Player *ks_player_of(void *native);
Game *ks_native_game(void *native);
// Server host: accountID / accountName of a player (3.2.1, "" for guests and on the client).
typedef int (*KsGameAccountFn)(void *host_user, const Player*, char *id, size_t id_n, char *name, size_t name_n);
void ks_game_set_account_fn(Game*, KsGameAccountFn);
Player *ks_game_player_by_id(Game*, KsValue id);
Player *ks_game_find_player_str(Game*, const char *id);       // "<uid>" -> Player, NULL none                  // loose `player.id == id` (as strings)
// Player id string of a player ("" for NULL), as hooks pass it.
void ks_player_id(const Player*, char *out, size_t n);

// ---------------------------------------------------------------------------
// Per-player script state (3.2.1): defaultMovement / defaultVelocity / defaultRotation,
// disableShooting / disableMelee, disableDefault(input) masks and registerSyncValues names.
// Reset by player_spawn (players.js reset, PLJ:458-473).
enum {
    KS_PLAYER_DEFAULT_MOVEMENT = 1 << 0,  // player.defaultMovement (default true)
    KS_PLAYER_DEFAULT_VELOCITY = 1 << 1,  // player.defaultVelocity (default true)
    KS_PLAYER_DEFAULT_ROTATION = 1 << 2,  // player.defaultRotation (default true)
    KS_PLAYER_CAN_SHOOT = 1 << 3,         // !player.disableShooting
    KS_PLAYER_CAN_MELEE = 1 << 4,         // !player.disableMelee
};
// 1 when the default behaviour `what` (one KS_PLAYER_*) runs for this player (always 1
// without a script). Movement code consults these at the v7 sites (PLJ:969, 1127, 1217,
// 1239, 1264; PLS:2236, 2657).
int ks_player_allows(const Player*, int what);
#define KS_SYNC_MAX_NAMES 20  // custSync: names per player
#define KS_SYNC_NAME_MAX 20   // characters per name

// ---------------------------------------------------------------------------
// Hook points (4.1). Each is a no-op without a script or without that hook.

// procInputs (PLJ:894-948), both sides: onPlayerUpdate(id, delta, inputs), then the player's
// disableDefault masks are applied to *input. Returns 1 when GAME.DEFAULT.
// disablePlayerBehaviour is set: the caller skips the default movement / rotation / shooting.
int ks_game_on_player_update(Game*, Player*, Input *input);
// player_spawn (both sides): resets the per-player script state; on the authority's server
// script also fires onPlayerSpawn(id).
void ks_game_on_player_spawn(Game*, Player*);
// players.kill (authority): onPlayerDeath(id, killerID); killer may be the victim itself.
void ks_game_on_player_death(Game*, Player *victim, Player *killer);
// players.changeHealth (authority): damage > 0 that leaves the victim alive.
void ks_game_on_player_damage(Game*, Player *victim, Player *doer, double amount);
// Connection events (server host): onPlayerLeave(id) before removal; onChatMessage(msg, id)
// after sanitising, before broadcast (not for "/" commands); onGameEnd() after mode.onEnd.
void ks_game_on_player_leave(Game*, Player*);
void ks_game_on_player_leave_uid(Game*, int uid); // a connection without a player right now
void ks_game_on_chat_message(Game*, Player*, const char *text);
void ks_game_on_game_end(Game*);
// Trigger action 58 (map-logic §3.10): onCustomTrigger(playerID or "NONE", param, value).
void ks_game_on_custom_trigger(Game*, const char *player_id, const char *param, int value);

// Custom sync values of a player (2.6): the [name, value...] array of its registered names
// whose current wrapper value is num / str / bool, serialised for NET_PACKET_SCRIPT_SYNC.
// Returns the payload length, 0 when the player has no names (nothing to send).
size_t ks_game_sync_payload(Game*, const Player*, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
