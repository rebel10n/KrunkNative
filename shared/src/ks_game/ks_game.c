// KrunkScript <-> Game integration (shared/include/ks_game.h, doc/v7/krunkscript.md 2.2, 2.4,
// 3.0, 3.1.2, 3.1.7, 4.1, 4.2, 5). Both sides: the server host (server/src/ks_server_host.c)
// and the client host (client/src/ks) load the map's script through ks_game_load; the hook
// points below are called from shared/src/player.c and game.c.
#include "ks_game_internal.h"

#include <ks_import_js.h>
#include <triggers_v7.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <sys/timeb.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
// State

KsGame *ks_game_state(Game *game, const int create)
{
    if (!game) return NULL;
    if (!game->ks && create) {
        KsGame *kg = calloc(1, sizeof(*kg));
        if (!kg) return NULL;
        kg->game = game;
        kg->side = KS_SIDE_CLIENT;
        game->ks = kg;
    }
    return game->ks;
}

void ks_ref_reset_player(KsRef *ref)
{
    ref->allow = KS_PLAYER_ALLOW_ALL;
    ref->disabled = 0;
    ref->sync_count = 0;
}

KsRef *ks_ref_find(const KsGame *kg, const int kind, const int id)
{
    if (!kg) return NULL;
    for (size_t i = 0; i < kg->ref_count; i++) {
        KsRef *r = kg->refs[i];
        if (r->kind == kind && r->id == id) return r;
    }
    return NULL;
}

KsRef *ks_ref_get(KsGame *kg, const int kind, const int id)
{
    KsRef *r = ks_ref_find(kg, kind, id);
    if (r || !kg) return r;
    if (kg->ref_count == kg->ref_cap) {
        const size_t cap = kg->ref_cap ? kg->ref_cap * 2 : 32;
        KsRef **refs = realloc(kg->refs, cap * sizeof(*refs));
        if (!refs) return NULL;
        kg->refs = refs;
        kg->ref_cap = cap;
    }
    r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->kg = kg;
    r->kind = kind;
    r->id = id;
    ks_ref_reset_player(r);
    kg->refs[kg->ref_count++] = r;
    return r;
}

static void free_refs(KsGame *kg)
{
    for (size_t i = 0; i < kg->ref_count; i++) free(kg->refs[i]);
    free(kg->refs);
    kg->refs = NULL;
    kg->ref_count = kg->ref_cap = 0;
}

// Wall clock in ms. MinGW's C runtime has no C11 timespec_get/TIME_UTC: _ftime64 there
// (same as ks_stdlib.c).
double ks_wall_ms(void)
{
#ifdef _WIN32
    struct __timeb64 tb;
    _ftime64(&tb);
    return (double) tb.time * 1000.0 + (double) tb.millitm;
#else
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) return (double) time(NULL) * 1000.0;
    return floor((double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6);
#endif
}

double ks_game_now_ms(KsGame *kg)
{
    if (kg && kg->host && kg->host->now_ms) return kg->host->now_ms(kg->host_user);
    return ks_wall_ms();
}

// ---------------------------------------------------------------------------
// Map script text (1.1, 5.1)

char *ks_game_map_source(const Game *game, const KsSide side, size_t *len)
{
    if (len) *len = 0;
    if (!game || !game->map || !game->map->scripts) return NULL;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(game->map->scripts, side == KS_SIDE_SERVER ? "server" : "client");
    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) return NULL;
    size_t n = 0;
    char *src = ks_decode_script(item->valuestring, strlen(item->valuestring), &n);
    if (!src) return NULL;
    // all-whitespace scripts run nothing
    size_t i = 0;
    while (i < n && (src[i] == ' ' || src[i] == '\t' || src[i] == '\r' || src[i] == '\n')) i++;
    if (i == n) {
        free(src);
        return NULL;
    }
    if (len) *len = n;
    return src;
}

uint32_t ks_source_hash(const char *src, const size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char) src[i];
        h *= 16777619u;
    }
    return h ? h : 1;
}

// ---------------------------------------------------------------------------
// Lifecycle

static void kg_log(void *user, const int is_error, const char *line)
{
    KsGame *kg = user;
    if (kg && kg->host && kg->host->log) kg->host->log(kg->host_user, is_error, line);
}

static double kg_now(void *user)
{
    return ks_game_now_ms(user);
}

static void host_log(KsGame *kg, const KsHost *host, void *host_user, const int is_error, const char *line)
{
    (void) kg;
    if (host && host->log) host->log(host_user, is_error, line);
}

static const TriggerScriptHooks g_trigger_hooks; // .on_custom_trigger, set below

static void reset_runtime_state(KsGame *kg)
{
    kg->flags = 0;
    kg->fixed_delta = 0.0;
    kg->frozen = 0;
    kg->rate1_count = kg->rate10_count = 0;
    kg->rate_win1 = kg->rate_win10 = 0;
    for (size_t i = 0; i < kg->ref_count; i++) ks_ref_reset_player(kg->refs[i]);
}

void ks_game_unload(Game *game)
{
    KsGame *kg = game ? game->ks : NULL;
    if (!kg || (!kg->script && !kg->prog)) return;
    if (kg->script && kg->side == KS_SIDE_SERVER && !ks_script_failed(kg->script)) ks_script_call_hook(kg->script, "onServerClosed", NULL, 0);
    if (kg->side == KS_SIDE_SERVER && game->script_hooks == &g_trigger_hooks) triggers_set_script_hooks(game, NULL);
    ks_script_free(kg->script);
    ks_program_free(kg->prog);
    kg->script = NULL;
    kg->prog = NULL;
    kg->source_hash = 0;
    reset_runtime_state(kg);
    // wrappers are per script instance: the refs (and their per-player state) start over
    free_refs(kg);
}

void ks_game_free(Game *game)
{
    if (!game || !game->ks) return;
    ks_game_unload(game);
    free_refs(game->ks);
    free(game->ks);
    game->ks = NULL;
}

int ks_game_load(Game *game, const KsSide side, const char *src, const size_t len, const KsHost *host, void *host_user)
{
    if (!game) return -1;
    ks_game_unload(game);
    if (!src || !len) return 0;

    // Compiled-JS map scripts (SniperRoyale2, Verb_Arena: the v7 compiler's output, 1.16) run
    // as the KrunkScript the importer recovers (WP7); outside that shape they run nothing.
    KsDiag err = {0};
    char *imported = NULL;
    const uint32_t source_hash = ks_source_hash(src, len); // of the map's text (ks_server_round_init compares it)
    if (ks_is_compiled_js(src, len)) {
        imported = ks_import_js(src, len, side, NULL, &err);
        if (!imported) {
            char line[320];
            ks_diag_format(&err, line, sizeof(line));
            char msg[480];
            snprintf(msg, sizeof(msg), "%s (%s script, %s)", KS_COMPILED_JS_MESSAGE, side == KS_SIDE_SERVER ? "server" : "client", line);
            host_log(NULL, host, host_user, 1, msg);
            return -1;
        }
        host_log(NULL, host, host_user, 0, KS_IMPORT_JS_MESSAGE);
    }

    KsProgram *prog = imported ? ks_compile(imported, strlen(imported), side, &err, NULL, 0, NULL)
                               : ks_compile(src, len, side, &err, NULL, 0, NULL);
    free(imported);
    if (!prog) {
        char line[320];
        ks_diag_format(&err, line, sizeof(line));
        char msg[400];
        snprintf(msg, sizeof(msg), "%s script: %s", side == KS_SIDE_SERVER ? "Server" : "Client", line);
        host_log(NULL, host, host_user, 1, msg);
        return -1;
    }

    KsGame *kg = ks_game_state(game, 1);
    if (!kg) {
        ks_program_free(prog);
        return -1;
    }
    kg->side = side;
    kg->host = host;
    kg->host_user = host_user;
    kg->wrap = (KsHost) {.side = side, .log = kg_log, .now_ms = kg_now, .api = host ? host->api : NULL};
    kg->prog = prog;
    kg->source_hash = source_hash;
    reset_runtime_state(kg);
    kg->rate_t0 = ks_game_now_ms(kg);
    if (side == KS_SIDE_SERVER) triggers_set_script_hooks(game, &g_trigger_hooks);

    // ks_script_new runs the global initialisers (with the VM's default seed: ks.h has no way
    // to seed before them); the rest of the stream comes from the map script and the host.
    kg->script = ks_script_new(prog, &kg->wrap, kg);
    if (!kg->script || ks_script_failed(kg->script)) {
        // 1.14 [KN]: a failing initialiser disables the script, the map still loads
        ks_game_unload(game);
        return -1;
    }
    ks_script_seed(kg->script, ((uint64_t) kg->source_hash << 32) ^ 0x9E3779B97F4A7C15ull ^ (uint64_t) (uint32_t) game->current_map_index);
    return 0;
}

uint32_t ks_game_source_hash(const Game *game)
{
    return game && game->ks ? game->ks->source_hash : 0;
}

KsScript *ks_game_script(const Game *game)
{
    if (!game || !game->ks || !game->ks->script || ks_script_failed(game->ks->script)) return NULL;
    return game->ks->script;
}

KsSide ks_game_side(const Game *game)
{
    return game && game->ks && game->ks->script ? game->ks->side : KS_SIDE_CLIENT;
}

Game *ks_game_of(KsScript *s)
{
    KsGame *kg = ks_script_user(s);
    return kg ? kg->game : NULL;
}

void *ks_game_host_user(KsScript *s)
{
    KsGame *kg = ks_script_user(s);
    return kg ? kg->host_user : NULL;
}

void ks_game_seed(Game *game, const uint64_t seed)
{
    KsScript *s = ks_game_script(game);
    if (s) ks_script_seed(s, seed);
}

void ks_game_set_player_class(Game *game, const KsHostClass *cls)
{
    KsGame *kg = ks_game_state(game, cls != NULL);
    if (kg) kg->player_class = cls;
}

const KsHostClass *ks_game_player_class(const Game *game)
{
    return game && game->ks && game->ks->player_class ? game->ks->player_class : &ks_player_class;
}

void ks_game_set_account_fn(Game *game, const KsGameAccountFn fn)
{
    KsGame *kg = ks_game_state(game, fn != NULL);
    if (kg) kg->account = fn;
}

// ---------------------------------------------------------------------------
// Hooks

int ks_game_has_hook(const Game *game, const char *hook)
{
    KsScript *s = ks_game_script(game);
    return s && ks_hook_param_count(game->ks->side, hook) >= 0 && ks_script_has_hook(s, hook);
}

int ks_game_call(Game *game, const char *hook, const KsValue *args, const int argc)
{
    KsScript *s = ks_game_script(game);
    if (!s || !hook) return -1;
    if (ks_hook_param_count(game->ks->side, hook) < 0) return 0; // hooks of the other side never run (4.1)
    if (!ks_script_has_hook(s, hook)) return 0;
    return ks_script_call_hook(s, hook, args, argc);
}

void ks_game_start(Game *game)
{
    KsGame *kg = game ? game->ks : NULL;
    if (kg) kg->frozen = 0; // game.init resets gameTimer: a freeze ends with the round
    ks_game_call(game, "start", NULL, 0);
}

void ks_game_update(Game *game, const double delta_ms, const double delta_mlt)
{
    KsScript *s = ks_game_script(game);
    if (!s) return;
    game->ks->fixed_delta = delta_ms;
    const KsValue arg = ks_num(delta_ms * (delta_mlt > 0.0 ? delta_mlt : 1.0));
    ks_game_call(game, "update", &arg, 1);
}

static KsValue id_value(KsScript *s, const Player *player)
{
    char id[24];
    ks_player_id(player, id, sizeof(id));
    return ks_str(s, id, strlen(id));
}

void ks_player_id(const Player *player, char *out, const size_t n)
{
    if (!out || !n) return;
    if (!player) out[0] = 0;
    else snprintf(out, n, "%d", player->uid);
}

static int server_hook(const Game *game, const char *hook)
{
    return ks_game_side(game) == KS_SIDE_SERVER && ks_game_has_hook(game, hook);
}

int ks_game_on_player_update(Game *game, Player *player, Input *in)
{
    KsGame *kg = game ? game->ks : NULL;
    KsScript *s = ks_game_script(game);
    if (!kg || !s || !player || !in) return kg && (kg->flags & KS_FLAG_NO_DEFAULT_BEHAVIOUR);

    if (ks_game_has_hook(game, "onPlayerUpdate")) {
        // PLJ:864-910: delta = max(0.1, min(input[1], dltMx)) / deltaDiv; a fresh inputs copy
        double delta = (double) in->delta * 1000.0;
        const double max_delta = (double) game_constants.max_delta * 1000.0;
        if (!(delta <= max_delta)) delta = max_delta;
        if (!(delta >= 0.1)) delta = 0.1;
        KsValue inputs = ks_obj_new(s);
        ks_handle_pin(s, inputs);
        ks_obj_set(s, inputs, "mouseY", ks_num(in->x_dir));
        ks_obj_set(s, inputs, "mouseX", ks_num(in->y_dir));
        // config.movDirs[i] = -PI + (i + 1) * PI / 4; undefined when not moving
        ks_obj_set(s, inputs, "movDir", in->move_dir >= 0 && in->move_dir <= 7 ? ks_num(-M_PI + (in->move_dir + 1) * M_PI / 4.0) : ks_undef());
        ks_obj_set(s, inputs, "lMouse", ks_bool(in->shoot));
        ks_obj_set(s, inputs, "rMouse", ks_bool(in->scope));
        ks_obj_set(s, inputs, "jump", ks_bool(in->jump));
        ks_obj_set(s, inputs, "reload", ks_bool(in->reload));
        ks_obj_set(s, inputs, "crouch", ks_bool(in->crouch));
        ks_obj_set(s, inputs, "scroll", ks_num(in->scroll == 1 || in->scroll == -1 ? in->scroll : 0));
        ks_obj_set(s, inputs, "swap", ks_num(in->swap));
        ks_obj_set(s, inputs, "restK", ks_num(0));   // input[13]: no KN input field
        ks_obj_set(s, inputs, "inter", ks_bool(0));  // input[14]: KN interacts are NET_PACKET_INTERACT
        KsValue args[3] = {id_value(s, player), ks_num(delta), inputs};
        ks_handle_pin(s, args[0]);
        ks_game_call(game, "onPlayerUpdate", args, 3);
        ks_handle_unpin(s, args[0]);
        ks_handle_unpin(s, inputs);
    }

    const KsRef *ref = ks_ref_find(kg, KS_REF_PLAYER, player->uid);
    const unsigned d = ref ? ref->disabled : 0;
    if (d & KS_INPUT_MOUSEY) in->x_dir = 0.0f;
    if (d & KS_INPUT_MOUSEX) in->y_dir = 0.0f;
    if (d & KS_INPUT_MOVDIR) in->move_dir = -1;
    if (d & KS_INPUT_LMOUSE) in->shoot = 0;
    if (d & KS_INPUT_RMOUSE) in->scope = 0;
    if (d & KS_INPUT_JUMP) in->jump = 0;
    if (d & KS_INPUT_RELOAD) in->reload = 0;
    if (d & KS_INPUT_CROUCH) in->crouch = 0;
    if (d & KS_INPUT_SCROLL) in->scroll = 0;
    if (d & KS_INPUT_SWAP) in->swap = 0;
    return (kg->flags & KS_FLAG_NO_DEFAULT_BEHAVIOUR) != 0;
}

void ks_game_on_player_spawn(Game *game, Player *player)
{
    KsGame *kg = game ? game->ks : NULL;
    if (!kg || !player) return;
    // players.js reset (PLJ:458-473): default flags, disabledInputs, custSync
    KsRef *ref = ks_ref_find(kg, KS_REF_PLAYER, player->uid);
    if (ref) ks_ref_reset_player(ref);
    if (!game_is_authority(game) || !server_hook(game, "onPlayerSpawn")) return;
    KsScript *s = kg->script;
    KsValue id = id_value(s, player);
    ks_game_call(game, "onPlayerSpawn", &id, 1);
}

void ks_game_on_player_death(Game *game, Player *victim, Player *killer)
{
    if (!victim || !server_hook(game, "onPlayerDeath")) return;
    KsScript *s = game->ks->script;
    KsValue args[2] = {id_value(s, victim), killer ? id_value(s, killer) : ks_str(s, "", 0)};
    ks_handle_pin(s, args[0]);
    ks_game_call(game, "onPlayerDeath", args, 2);
    ks_handle_unpin(s, args[0]);
}

void ks_game_on_player_damage(Game *game, Player *victim, Player *doer, const double amount)
{
    if (!victim || !(amount > 0.0) || !server_hook(game, "onPlayerDamage")) return;
    KsScript *s = game->ks->script;
    KsValue args[3] = {id_value(s, victim), doer ? id_value(s, doer) : ks_str(s, "", 0), ks_num(amount)};
    ks_handle_pin(s, args[0]);
    ks_game_call(game, "onPlayerDamage", args, 3);
    ks_handle_unpin(s, args[0]);
}

void ks_game_on_player_leave_uid(Game *game, const int uid)
{
    if (server_hook(game, "onPlayerLeave")) {
        char id[24];
        snprintf(id, sizeof(id), "%d", uid);
        KsValue arg = ks_str(game->ks->script, id, strlen(id));
        ks_game_call(game, "onPlayerLeave", &arg, 1);
    }
    // the wrapper's script-added properties go with the player (6.4)
    KsScript *s = ks_game_script(game);
    if (s) ks_host_forget(s, ks_game_player_class(game), (uint32_t) uid);
}

void ks_game_on_player_leave(Game *game, Player *player)
{
    if (player) ks_game_on_player_leave_uid(game, player->uid);
}

void ks_game_on_chat_message(Game *game, Player *player, const char *text)
{
    if (!text || !server_hook(game, "onChatMessage")) return;
    KsScript *s = game->ks->script;
    KsValue args[2] = {ks_str(s, text, strlen(text)), player ? id_value(s, player) : ks_str(s, "", 0)};
    ks_handle_pin(s, args[0]);
    ks_game_call(game, "onChatMessage", args, 2);
    ks_handle_unpin(s, args[0]);
}

void ks_game_on_game_end(Game *game)
{
    if (server_hook(game, "onGameEnd")) ks_game_call(game, "onGameEnd", NULL, 0);
}

void ks_game_on_custom_trigger(Game *game, const char *player_id, const char *param, const int value)
{
    if (!server_hook(game, "onCustomTrigger")) return;
    KsScript *s = game->ks->script;
    if (!player_id || !player_id[0]) player_id = "NONE";
    if (!param) param = "";
    KsValue args[3] = {ks_str(s, player_id, strlen(player_id)), ks_str(s, param, strlen(param)), ks_num(value)};
    ks_handle_pin(s, args[0]);
    ks_handle_pin(s, args[1]);
    ks_game_call(game, "onCustomTrigger", args, 3);
    ks_handle_unpin(s, args[1]);
    ks_handle_unpin(s, args[0]);
}

// Trigger action 58 (map-logic §3.10): the trigger runtime calls the table on game->script_hooks.
static void trigger_custom(Game *game, const char *player_id, const char *param, const int value)
{
    ks_game_on_custom_trigger(game, player_id, param, value);
}

// Trigger condition 12 (v10.0.2 "Custom Action"): !!mainScript.shouldTrigger(playerId, sid, param);
// no script or no hook fails the condition. A runtime error fails it too.
static int trigger_should(Game *game, const char *player_id, const int trigger_sid, const char *param)
{
    if (!server_hook(game, "shouldTrigger")) return 0;
    KsScript *s = game->ks->script;
    KsValue args[3] = {ks_str(s, player_id, strlen(player_id)), ks_num(trigger_sid), ks_str(s, param, strlen(param))};
    ks_handle_pin(s, args[0]);
    ks_handle_pin(s, args[2]);
    KsValue result = ks_undef();
    const int rc = ks_script_call_hook_value(s, "shouldTrigger", args, 3, &result);
    ks_handle_unpin(s, args[2]);
    ks_handle_unpin(s, args[0]);
    if (rc != 0) return 0;
    switch (result.tag) { // !!value (JS ToBoolean)
        case KS_UNDEF: case KS_NULL: return 0;
        case KS_BOOL: return result.as.b != 0;
        case KS_NUM: return result.as.num != 0.0 && result.as.num == result.as.num;
        case KS_STR: { size_t len = 0; ks_str_utf8(result, &len); return len != 0; }
        default: return 1;
    }
}

static const TriggerScriptHooks g_trigger_hooks = {.on_custom_trigger = trigger_custom, .should_trigger = trigger_should};

// ---------------------------------------------------------------------------
// Flags, rate limits, per-player state

int ks_game_flag(const Game *game, const KsGameFlag flag)
{
    return game && game->ks && (game->ks->flags & (int) flag) != 0;
}

void ks_game_set_flag(Game *game, const KsGameFlag flag, const int on)
{
    KsGame *kg = ks_game_state(game, on);
    if (!kg) return;
    if (on) kg->flags |= (int) flag;
    else kg->flags &= ~(int) flag;
}

static int rate_bump(KsRateCount *list, int *count, const char *key)
{
    for (int i = 0; i < *count; i++)
        if (!strcmp(list[i].key, key)) return ++list[i].count;
    if (*count >= KS_RATE_KEYS) return 1 << 30; // too many distinct keys in one window: refuse
    KsRateCount *r = &list[(*count)++];
    snprintf(r->key, sizeof(r->key), "%s", key);
    r->count = 1;
    return 1;
}

int ks_game_rate(Game *game, const char *key, const int limit, const int per_10s)
{
    KsGame *kg = ks_game_state(game, 1);
    if (!kg || !key) return 0;
    const double now = ks_game_now_ms(kg);
    const long long w1 = (long long) floor((now - kg->rate_t0) / 1000.0);
    const long long w10 = (long long) floor((now - kg->rate_t0) / 10000.0);
    if (w1 != kg->rate_win1) {
        kg->rate_win1 = w1;
        kg->rate1_count = 0;
    }
    if (w10 != kg->rate_win10) {
        kg->rate_win10 = w10;
        kg->rate10_count = 0;
    }
    // f2123: increments first, fails when the count >= limit; f2124: fails when > limit
    if (per_10s) return rate_bump(kg->rate10, &kg->rate10_count, key) <= limit;
    return rate_bump(kg->rate1, &kg->rate1_count, key) < limit;
}

int ks_player_allows(const Player *player, const int what)
{
    if (!player || !player->game || !player->game->ks) return 1;
    const KsRef *ref = ks_ref_find(player->game->ks, KS_REF_PLAYER, player->uid);
    return !ref || (ref->allow & what) == what;
}

size_t ks_game_sync_payload(Game *game, const Player *player, uint8_t *out, const size_t cap)
{
    KsScript *s = ks_game_script(game);
    if (!s || !player) return 0;
    const KsRef *ref = ks_ref_find(game->ks, KS_REF_PLAYER, player->uid);
    if (!ref || !ref->sync_count) return 0;
    const KsValue self = ks_game_wrap_player(s, game, player);
    KsValue list = ks_arr_new(s);
    ks_handle_pin(s, list);
    for (int i = 0; i < ref->sync_count; i++) {
        const KsValue v = ks_obj_get(s, self, ref->sync[i]);
        if (v.tag != KS_NUM && v.tag != KS_STR && v.tag != KS_BOOL) continue; // 2.6 [KN]: primitives only
        ks_arr_push(s, list, ks_str(s, ref->sync[i], strlen(ref->sync[i])));
        ks_arr_push(s, list, v);
    }
    size_t n = ks_arr_len(list) ? ks_serialize(s, list, out, cap) : 0;
    if (n > cap) n = 0; // too large for one packet: skipped this time
    ks_handle_unpin(s, list);
    return n;
}

// ---------------------------------------------------------------------------
// Shared API entries: GAME.DEFAULT, TIME.fixedDelta / freeze / unfreeze

static KsGame *api_kg(void *user) { return user; }

static KsValue d_no_behaviour(KsScript *s, void *u, const KsValue *a, int n)
{
    (void) s; (void) a; (void) n;
    if (api_kg(u)) api_kg(u)->flags |= KS_FLAG_NO_DEFAULT_BEHAVIOUR;
    return ks_undef();
}

static KsValue d_no_prediction(KsScript *s, void *u, const KsValue *a, int n)
{
    (void) s; (void) a; (void) n;
    if (api_kg(u)) api_kg(u)->flags |= KS_FLAG_NO_PREDICTION;
    return ks_undef();
}

static KsValue d_no_sync(KsScript *s, void *u, const KsValue *a, int n)
{
    (void) s; (void) a; (void) n;
    if (api_kg(u)) api_kg(u)->flags |= KS_FLAG_NO_SERVER_SYNC;
    return ks_undef();
}

static KsValue t_fixed_delta(KsScript *s, void *u, const KsValue *a, int n)
{
    (void) s; (void) a; (void) n;
    return ks_num(api_kg(u) ? api_kg(u)->fixed_delta : 0.0);
}

// game.gameTimer != "skip" (a running countdown), not ending, not endTimer "inf"
static int round_clock_running(const Game *game)
{
    if (game->end_timer > 0.0f || game->round_endless) return 0;
    if (game->mode && game->mode->config.timed) return 0;
    return game->round_timer > 0.0f;
}

static KsValue t_freeze(KsScript *s, void *u, const KsValue *a, int n)
{
    (void) s; (void) a; (void) n;
    KsGame *kg = api_kg(u);
    if (kg && kg->side == KS_SIDE_SERVER && !kg->frozen && round_clock_running(kg->game)) kg->frozen = 1;
    return ks_undef();
}

static KsValue t_unfreeze(KsScript *s, void *u, const KsValue *a, int n)
{
    (void) s; (void) a; (void) n;
    KsGame *kg = api_kg(u);
    if (kg && kg->side == KS_SIDE_SERVER && kg->frozen && kg->game->end_timer <= 0.0f && !kg->game->round_endless) kg->frozen = 0;
    return ks_undef();
}

int ks_game_round_frozen(const Game *game)
{
    return game && game->ks && game->ks->frozen;
}

void ks_game_api_fill(KsApiImpl *api, const KsSide side)
{
    if (!api) return;
    api[KS_API_DEFAULT_DISABLEPLAYERBEHAVIOUR].fn = d_no_behaviour;
    api[KS_API_DEFAULT_DISABLEPREDICTION].fn = d_no_prediction;
    api[KS_API_DEFAULT_DISABLESERVERSYNC].fn = d_no_sync;
    api[KS_API_TIME_FIXEDDELTA].fn = t_fixed_delta;
    if (side == KS_SIDE_SERVER) {
        api[KS_API_TIME_FREEZE].fn = t_freeze;
        api[KS_API_TIME_UNFREEZE].fn = t_unfreeze;
    }
    ks_hostobj_api_fill(api, side);
}
