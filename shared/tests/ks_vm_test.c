// KrunkScript runtime conformance test (WP2, doc/v7/krunkscript.md 6.7):
//
//   ks_vm_test [DIR]       runs the C unit tests (number formatting/parsing tables, UTF-16
//                          strings, serialisation, GC) and every DIR/run/*.krnk program
//                          (default DIR = KS_TEST_DIR), then the corpus runtime smoke test
//                          (DIR/corpus/*.krnk: start + update/render hooks, no runtime
//                          error expected unless listed in DIR/run/corpus.allow).
//                          Then a fuzz pass: random hook calls with random argument values
//                          on every corpus script, random bytes into ks_deserialize.
//   ks_vm_test FILE.krnk   runs one program and prints its output.
//
// A run program FILE.krnk is compared with FILE.out: one line per GAME.log call, runtime
// errors prefixed with "!" ("!RUNTIME ERROR: 3:5 Index out of range."), compile errors
// "!COMPILE <line>:<col> <message>". Directives in the source (comment lines):
//   #! side server             compile/run as the server script (default client)
//   #! call <hook> [args...]   call a hook; args are JSON-like literals: 16, -1.5, "id",
//                              true, false, null, undefined. Default: `#! call start`.
//   #! callback [args...]      invoke the last callback the script handed to the mock
//                              STORAGE.load (a new host entry)
//   #! gc                      force a full collection
//   #! seed N                  ks_script_seed
//   #! self N [NAME]           PLAYERS.getSelf returns mock player N (renamed NAME)
//
// Mock host (client and server): PLAYERS.list/findByID return two "Player" host objects
// (id, username, health rw coerced, position sub-object, respawn() method, isYou),
// PLAYERS.getSelf -> null (see `#! self`), CAMERA.position.x rw PROP, TIME.now = 1700000000000,
// TIME.fixedDelta = 16, STORAGE.load(acct, map, cb) stores cb, AI.spawn raises a host error,
// SCENE.setSkyDome / NETWORK.send log "> <path> <args>" (objects one level deep).
//
// Then the compiled-JS importer (WP7, ks_import_js.h): every DIR/js/*.js is imported, run
// like a corpus script (smoke + fuzz) and, with DIR/js/NAME.run (directives as above,
// appended to the imported source), compared with DIR/js/NAME.out.
#include "ks.h"
#include "ks_api.h"
#include "ks_import_js.h"
#include "ks_net.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dirent.h>
#endif

#ifndef KS_TEST_DIR
#define KS_TEST_DIR "shared/tests/ks"
#endif

static int g_fail, g_checks;
#define CHECK(c)                                                              \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(c)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

// Internal runtime helpers (ks_rt.h) used by the unit tests.
size_t ks_num_format(double x, char *out);
double ks_parse_float(const char *s, size_t len);
double ks_parse_int(const char *s, size_t len);
double ks_string_to_number(const char *s, size_t len);
double ks_to_float(KsValue v); // also declared in ks.h

static char *slurp(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc((size_t)(n < 0 ? 0 : n) + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t r = fread(buf, 1, (size_t)(n < 0 ? 0 : n), fp);
    fclose(fp);
    buf[r] = 0;
    if (len) *len = r;
    return buf;
}

// ---------------------------------------------------------------------------
// Output capture

typedef struct Capture {
    char *p;
    size_t len, cap;
} Capture;

static void cap_put(Capture *c, const char *s, size_t n)
{
    if (c->len + n + 1 > c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 1024;
        while (cap < c->len + n + 1) cap *= 2;
        c->p = realloc(c->p, cap);
        if (!c->p) abort();
        c->cap = cap;
    }
    memcpy(c->p + c->len, s, n);
    c->len += n;
    c->p[c->len] = 0;
}

// ---------------------------------------------------------------------------
// Mock host

typedef struct MockPlayer {
    char id[8];
    char name[16];
    double health;
    double pos[3];
    double vel[3];
    int is_you;
} MockPlayer;

typedef struct Mock {
    Capture out;
    MockPlayer players[2];
    double cam_x;
    KsValue callback;
    int has_callback;
    int self;   // PLAYERS.getSelf: players[self - 1], 0 = null
} Mock;

static KsValue vec_get(KsScript *s, void *native, uint32_t id, int axis)
{
    (void)s; (void)id;
    return ks_num(((MockPlayer *)native)->pos[axis]);
}
static void vec_set(KsScript *s, void *native, uint32_t id, int axis, KsValue v)
{
    (void)s; (void)id;
    ((MockPlayer *)native)->pos[axis] = ks_to_float(v);
}
static KsValue vx_get(KsScript *s, void *n, uint32_t id) { return vec_get(s, n, id, 0); }
static KsValue vy_get(KsScript *s, void *n, uint32_t id) { return vec_get(s, n, id, 1); }
static KsValue vz_get(KsScript *s, void *n, uint32_t id) { return vec_get(s, n, id, 2); }
static void vx_set(KsScript *s, void *n, uint32_t id, KsValue v) { vec_set(s, n, id, 0, v); }
static void vy_set(KsScript *s, void *n, uint32_t id, KsValue v) { vec_set(s, n, id, 1, v); }
static void vz_set(KsScript *s, void *n, uint32_t id, KsValue v) { vec_set(s, n, id, 2, v); }
static const KsHostProp g_vec_props[] = {
    {"x", vx_get, vx_set, NULL, 0},
    {"y", vy_get, vy_set, NULL, 0},
    {"z", vz_get, vz_set, NULL, 0},
};
static const KsHostClass g_vec_class = {"Vec3", g_vec_props, 3};

// Generic engine object for client APIs the corpus touches (scene objects, sizes,
// projected points, bones): numeric fields x y z width height, position/rotation/scale
// sub-objects, and no-op methods. Stubs live in a fixed pool owned by the test.
typedef struct Stub {
    double f[5];      // x y z width height
    double vec[3][3]; // position rotation scale
} Stub;
static Stub g_stubs[4096];
static uint32_t g_stub_count;

static KsValue stub_new(KsScript *s, double w, double h);
static KsValue sv_get(void *n, int k) { return ks_num(((double *)n)[k]); }
static KsValue svx(KsScript *s, void *n, uint32_t id) { (void)s; (void)id; return sv_get(n, 0); }
static KsValue svy(KsScript *s, void *n, uint32_t id) { (void)s; (void)id; return sv_get(n, 1); }
static KsValue svz(KsScript *s, void *n, uint32_t id) { (void)s; (void)id; return sv_get(n, 2); }
static void svx_set(KsScript *s, void *n, uint32_t id, KsValue v) { (void)s; (void)id; ((double *)n)[0] = ks_to_float(v); }
static void svy_set(KsScript *s, void *n, uint32_t id, KsValue v) { (void)s; (void)id; ((double *)n)[1] = ks_to_float(v); }
static void svz_set(KsScript *s, void *n, uint32_t id, KsValue v) { (void)s; (void)id; ((double *)n)[2] = ks_to_float(v); }
static const KsHostProp g_stubvec_props[] = {
    {"x", svx, svx_set, NULL, 0}, {"y", svy, svy_set, NULL, 0}, {"z", svz, svz_set, NULL, 0},
};
static const KsHostClass g_stubvec_class = {"StubVec", g_stubvec_props, 3};
#define STUB_FIELD(name, k)                                                                         \
    static KsValue name(KsScript *s, void *n, uint32_t id) { (void)s; (void)id; return ks_num(((Stub *)n)->f[k]); } \
    static void name##_set(KsScript *s, void *n, uint32_t id, KsValue v) { (void)s; (void)id; ((Stub *)n)->f[k] = ks_to_float(v); }
STUB_FIELD(st_x, 0)
STUB_FIELD(st_y, 1)
STUB_FIELD(st_z, 2)
STUB_FIELD(st_w, 3)
STUB_FIELD(st_h, 4)
#undef STUB_FIELD
static KsValue st_vec(KsScript *s, void *n, uint32_t id, int k)
{
    return ks_host_object(s, &g_stubvec_class, ((Stub *)n)->vec[k], id * 4 + (uint32_t)k);
}
static KsValue st_pos(KsScript *s, void *n, uint32_t id) { return st_vec(s, n, id, 0); }
static KsValue st_rot(KsScript *s, void *n, uint32_t id) { return st_vec(s, n, id, 1); }
static KsValue st_scale(KsScript *s, void *n, uint32_t id) { return st_vec(s, n, id, 2); }
static KsValue st_true(KsScript *s, void *n, uint32_t id) { (void)s; (void)n; (void)id; return ks_bool(1); }
static KsValue st_noop(KsScript *s, void *n, const KsValue *a, int c) { (void)s; (void)n; (void)a; (void)c; return ks_undef(); }
static KsValue st_child(KsScript *s, void *n, const KsValue *a, int c) { (void)n; (void)a; (void)c; return stub_new(s, 0, 0); }
static const KsHostProp g_stub_props[] = {
    {"x", st_x, st_x_set, NULL, 0}, {"y", st_y, st_y_set, NULL, 0}, {"z", st_z, st_z_set, NULL, 0},
    {"width", st_w, st_w_set, NULL, 0}, {"height", st_h, st_h_set, NULL, 0},
    {"onScreen", st_true, NULL, NULL, 0}, {"visible", st_true, NULL, NULL, 0},
    {"position", st_pos, NULL, NULL, 1}, {"rotation", st_rot, NULL, NULL, 1}, {"scale", st_scale, NULL, NULL, 1},
    {"move", NULL, NULL, st_noop, 0}, {"rotate", NULL, NULL, st_noop, 0}, {"lookAt", NULL, NULL, st_noop, 0},
    {"attachTo", NULL, NULL, st_noop, 0}, {"detach", NULL, NULL, st_noop, 0}, {"playAnim", NULL, NULL, st_noop, 0},
    {"stopAnim", NULL, NULL, st_noop, 0}, {"delete", NULL, NULL, st_noop, 0}, {"getBone", NULL, NULL, st_child, 0},
};
static const KsHostClass g_stub_class = {"Stub", g_stub_props, (int)(sizeof g_stub_props / sizeof g_stub_props[0])};

static KsValue stub_new(KsScript *s, double w, double h)
{
    uint32_t i = g_stub_count++ % (uint32_t)(sizeof g_stubs / sizeof g_stubs[0]);
    memset(&g_stubs[i], 0, sizeof g_stubs[i]);
    g_stubs[i].f[3] = w;
    g_stubs[i].f[4] = h;
    for (int k = 0; k < 3; k++) g_stubs[i].vec[2][k] = 1;
    ks_host_forget(s, &g_stub_class, i + 1); // pool slot reused: drop the old wrapper
    return ks_host_object(s, &g_stub_class, &g_stubs[i], i + 1);
}
static KsValue h_stub(KsScript *s, void *u, const KsValue *a, int n) { (void)u; (void)a; (void)n; return stub_new(s, 0, 0); }
static KsValue h_size(KsScript *s, void *u, const KsValue *a, int n) { (void)u; (void)a; (void)n; return stub_new(s, 1280, 720); }
static KsValue h_first_arg(KsScript *s, void *u, const KsValue *a, int n) { (void)s; (void)u; return n > 0 ? a[0] : ks_undef(); }

static KsValue p_id(KsScript *s, void *n, uint32_t id) { (void)id; const char *v = ((MockPlayer *)n)->id; return ks_str(s, v, strlen(v)); }
static KsValue p_name(KsScript *s, void *n, uint32_t id) { (void)id; const char *v = ((MockPlayer *)n)->name; return ks_str(s, v, strlen(v)); }
static KsValue p_health(KsScript *s, void *n, uint32_t id) { (void)s; (void)id; return ks_num(((MockPlayer *)n)->health); }
static void p_set_health(KsScript *s, void *n, uint32_t id, KsValue v)
{
    (void)s; (void)id;
    ((MockPlayer *)n)->health = ks_to_float(v);
}
static KsValue p_isyou(KsScript *s, void *n, uint32_t id) { (void)s; (void)id; return ks_bool(((MockPlayer *)n)->is_you); }
static KsValue p_position(KsScript *s, void *n, uint32_t id) { return ks_host_object(s, &g_vec_class, n, id); }
static KsValue p_velocity(KsScript *s, void *n, uint32_t id)
{
    return ks_host_object(s, &g_stubvec_class, ((MockPlayer *)n)->vel, 1000 + id);
}
static KsValue p_respawn(KsScript *s, void *native, const KsValue *args, int argc)
{
    (void)s; (void)args; (void)argc;
    ((MockPlayer *)native)->health = 100;
    return ks_bool(1);
}
static KsValue p_fail(KsScript *s, void *native, const KsValue *args, int argc)
{
    (void)native; (void)args; (void)argc;
    ks_runtime_error(s, "mock failure");
    return ks_undef();
}
static KsValue p_asset(KsScript *s, void *n, const KsValue *a, int c) { (void)n; (void)a; (void)c; return stub_new(s, 0, 0); }
static const KsHostProp g_player_props[] = {
    {"id", p_id, NULL, NULL, 0},
    {"username", p_name, NULL, NULL, 0},
    {"health", p_health, p_set_health, NULL, 0},
    {"position", p_position, NULL, NULL, 1},
    {"isYou", p_isyou, NULL, NULL, 0},
    {"velocity", p_velocity, NULL, NULL, 1},
    {"respawn", NULL, NULL, p_respawn, 0},
    {"explode", NULL, NULL, p_fail, 0},
    {"disableDefault", NULL, NULL, st_noop, 0},
    {"getAsset", NULL, NULL, p_asset, 0},
};
static const KsHostClass g_player_class = {"Player", g_player_props, (int)(sizeof g_player_props / sizeof g_player_props[0])};

static KsValue player_value(KsScript *s, Mock *m, int i)
{
    return ks_host_object(s, &g_player_class, &m->players[i], (uint32_t)i + 1);
}

static KsValue h_players_list(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void)args; (void)argc;
    Mock *m = user;
    KsValue arr = ks_arr_new(s);
    for (int i = 0; i < 2; i++) ks_arr_push(s, arr, player_value(s, m, i));
    return arr;
}
static KsValue h_players_find(KsScript *s, void *user, const KsValue *args, int argc)
{
    Mock *m = user;
    char id[64] = "";
    if (argc > 0) ks_to_string(s, args[0], id, sizeof id);
    for (int i = 0; i < 2; i++)
        if (strcmp(m->players[i].id, id) == 0) return player_value(s, m, i);
    return ks_null();
}
static KsValue h_null(KsScript *s, void *u, const KsValue *a, int n) { (void)s; (void)u; (void)a; (void)n; return ks_null(); }
static KsValue h_self(KsScript *s, void *u, const KsValue *a, int n)
{
    (void)a; (void)n;
    Mock *m = u;
    return m->self > 0 && m->self <= 2 ? player_value(s, m, m->self - 1) : ks_null();
}
static void trace_value(KsScript *s, Mock *m, KsValue v)
{
    char t[128];
    if (v.tag == KS_OBJ) {
        cap_put(&m->out, "{", 1);
        KsValue k, val;
        for (size_t i = 0; ks_obj_entry(s, v, i, &k, &val); i++) {
            if (i) cap_put(&m->out, ",", 1);
            ks_to_string(s, k, t, sizeof t);
            cap_put(&m->out, t, strlen(t));
            cap_put(&m->out, ":", 1);
            ks_to_string(s, val, t, sizeof t);
            cap_put(&m->out, t, strlen(t));
        }
        cap_put(&m->out, "}", 1);
        return;
    }
    ks_to_string(s, v, t, sizeof t);
    cap_put(&m->out, t, strlen(t));
}
static KsValue h_trace(KsScript *s, Mock *m, const char *path, const KsValue *a, int n)
{
    cap_put(&m->out, "> ", 2);
    cap_put(&m->out, path, strlen(path));
    for (int i = 0; i < n; i++) {
        cap_put(&m->out, " ", 1);
        trace_value(s, m, a[i]);
    }
    cap_put(&m->out, "\n", 1);
    return ks_undef();
}
static KsValue h_skydome(KsScript *s, void *u, const KsValue *a, int n) { return h_trace(s, u, "SCENE.setSkyDome", a, n); }
static KsValue h_net_send(KsScript *s, void *u, const KsValue *a, int n) { return h_trace(s, u, "NETWORK.send", a, n); }
static KsValue h_cam_x(KsScript *s, void *user, const KsValue *a, int n) { (void)s; (void)a; (void)n; return ks_num(((Mock *)user)->cam_x); }
static KsValue h_cam_x_set(KsScript *s, void *user, const KsValue *a, int n)
{
    (void)s;
    ((Mock *)user)->cam_x = n > 0 ? ks_to_float(a[0]) : 0;
    return ks_undef();
}
static KsValue h_fixed_delta(KsScript *s, void *u, const KsValue *a, int n) { (void)s; (void)u; (void)a; (void)n; return ks_num(16); }
static KsValue h_storage_load(KsScript *s, void *user, const KsValue *args, int argc)
{
    Mock *m = user;
    if (m->has_callback) ks_handle_unpin(s, m->callback);
    m->has_callback = 0;
    if (argc > 2 && args[2].tag == KS_FN) {
        m->callback = args[2];
        m->has_callback = 1;
        ks_handle_pin(s, m->callback);
    }
    return ks_undef();
}
static KsValue h_fail(KsScript *s, void *u, const KsValue *a, int n)
{
    (void)u; (void)a; (void)n;
    ks_runtime_error(s, "mock failure");
    return ks_undef();
}

static double mock_now(void *user) { (void)user; return 1700000000000.0; }

static void mock_log(void *user, int is_error, const char *line)
{
    Mock *m = user;
    if (is_error) cap_put(&m->out, "!", 1);
    cap_put(&m->out, line, strlen(line));
    cap_put(&m->out, "\n", 1);
}

static KsApiImpl g_api[KS_API_COUNT];

static void mock_init_api(void)
{
    g_api[KS_API_PLAYERS_LIST].fn = h_players_list;
    g_api[KS_API_PLAYERS_FINDBYID].fn = h_players_find;
    g_api[KS_API_PLAYERS_GETSELF].fn = h_self;
    g_api[KS_API_SCENE_SETSKYDOME].fn = h_skydome;
    g_api[KS_API_NETWORK_SEND].fn = h_net_send;
    g_api[KS_API_CAMERA_POSITION_X].fn = h_cam_x;
    g_api[KS_API_CAMERA_POSITION_X].set = h_cam_x_set;
    g_api[KS_API_TIME_FIXEDDELTA].fn = h_fixed_delta;
    g_api[KS_API_AI_SPAWN].fn = h_fail;
    int load = ks_api_find("STORAGE.load");
    if (load >= 0) g_api[load].fn = h_storage_load;
    // Client APIs the corpus relies on (sizes, scene objects, projected points, DIV ids)
    static const char *const sized[] = {"OVERLAY.getSize", "UI.getSize"};
    for (size_t i = 0; i < sizeof sized / sizeof sized[0]; i++) {
        int id = ks_api_find(sized[i]);
        if (id >= 0) g_api[id].fn = h_size;
    }
    static const char *const stubs[] = {"SCENE.addCube", "SCENE.addPlane", "SCENE.addSphere", "SCENE.addSprite",
        "SCENE.addCustom", "SCENE.addAsset", "SCENE.addSign", "SCENE.addLightCone", "SCENE.addLightBar",
        "SCENE.addPointLight", "SCENE.addSpotLight", "SCENE.addDirLight", "SCENE.addRectLight",
        "SCENE.posToScreen", "INPUTS.mousePos", "INPUTS.mouseMovement", "INPUTS.mousePosOverlay", "CAMERA.getObj"};
    for (size_t i = 0; i < sizeof stubs / sizeof stubs[0]; i++) {
        int id = ks_api_find(stubs[i]);
        if (id >= 0) g_api[id].fn = h_stub;
    }
    int div = ks_api_find("UI.addDIV");
    if (div >= 0) g_api[div].fn = h_first_arg;
}

static void mock_reset(Mock *m)
{
    memset(m, 0, sizeof *m);
    snprintf(m->players[0].id, sizeof m->players[0].id, "7");
    snprintf(m->players[0].name, sizeof m->players[0].name, "Alice");
    m->players[0].health = 100;
    m->players[0].is_you = 1;
    m->players[0].pos[0] = 1.5;
    snprintf(m->players[1].id, sizeof m->players[1].id, "9");
    snprintf(m->players[1].name, sizeof m->players[1].name, "Bob");
    m->players[1].health = 50;
    m->players[1].pos[2] = -3;
}

// ---------------------------------------------------------------------------
// Directive parsing

// Parses JSON-like literal args from `p`; returns count.
static int parse_args(KsScript *s, const char *p, KsValue *out, int max)
{
    int n = 0;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n' || *p == '\r') break;
        if (*p == '"') {
            const char *q = ++p;
            while (*q && *q != '"') q++;
            out[n++] = ks_str(s, p, (size_t)(q - p));
            p = *q ? q + 1 : q;
        } else if (!strncmp(p, "true", 4)) { out[n++] = ks_bool(1); p += 4; }
        else if (!strncmp(p, "false", 5)) { out[n++] = ks_bool(0); p += 5; }
        else if (!strncmp(p, "null", 4)) { out[n++] = ks_null(); p += 4; }
        else if (!strncmp(p, "undefined", 9)) { out[n++] = ks_undef(); p += 9; }
        else if (!strncmp(p, "{}", 2)) { out[n++] = ks_obj_new(s); p += 2; }
        else {
            char *end;
            double d = strtod(p, &end);
            if (end == p) break;
            out[n++] = ks_num(d);
            p = end;
        }
    }
    return n;
}

static const char *find_directive(const char *src, const char *name, const char *from)
{
    size_t nl = strlen(name);
    for (const char *p = from ? from : src; (p = strstr(p, "#! ")) != NULL; p += 3)
        if ((p == src || p[-1] == '\n') && !strncmp(p + 3, name, nl)) return p;
    return NULL;
}

// Runs one program; returns the captured output (caller frees).
static char *run_program(const char *src, size_t len)
{
    Mock *m = calloc(1, sizeof *m);
    if (!m) abort();
    mock_reset(m);
    KsSide side = find_directive(src, "side server", NULL) ? KS_SIDE_SERVER : KS_SIDE_CLIENT;
    KsDiag err = {0};
    KsProgram *prog = ks_compile(src, len, side, &err, NULL, 0, NULL);
    if (!prog) {
        char line[400];
        snprintf(line, sizeof line, "!COMPILE %d:%d %s\n", err.line, err.col, err.message);
        cap_put(&m->out, line, strlen(line));
        char *r = m->out.p;
        free(m);
        return r;
    }
    KsHost host = {side, mock_log, mock_now, g_api};
    const char *seed = find_directive(src, "seed ", NULL);
    KsScript *s = ks_script_new(prog, &host, m);
    if (seed) ks_script_seed(s, strtoull(seed + 8, NULL, 10));
    int any_call = 0;
    // directives run in source order
    for (const char *p = src; (p = strstr(p, "#! ")) != NULL; p += 3) {
        if (p != src && p[-1] != '\n') continue;
        KsValue args[16];
        if (!strncmp(p + 3, "call ", 5)) {
            any_call = 1;
            char hook[64];
            int k = 0;
            const char *q = p + 8;
            while (*q && *q != ' ' && *q != '\n' && *q != '\r' && k < 63) hook[k++] = *q++;
            hook[k] = 0;
            int argc = parse_args(s, q, args, 16);
            if (!ks_script_failed(s)) ks_script_call_hook(s, hook, args, argc);
        } else if (!strncmp(p + 3, "callback", 8)) {
            int argc = parse_args(s, p + 11, args, 16);
            if (m->has_callback) {
                int ok;
                ks_call_value(s, m->callback, args, argc, &ok);
            }
        } else if (!strncmp(p + 3, "self ", 5)) {
            char *end;
            long i = strtol(p + 8, &end, 10);
            if (i >= 0 && i < 2) {
                m->self = (int)i + 1;
                while (*end == ' ') end++;
                size_t k = 0;
                while (end[k] && end[k] != '\n' && end[k] != '\r' && k + 1 < sizeof m->players[i].name) k++;
                if (k) {
                    memcpy(m->players[i].name, end, k);
                    m->players[i].name[k] = 0;
                }
            }
        } else if (!strncmp(p + 3, "gc", 2)) {
            extern void ks_gc_collect(KsScript *);
            ks_gc_collect(s);
        }
    }
    if (!any_call && !ks_script_failed(s)) ks_script_call_hook(s, "start", NULL, 0);
    ks_script_free(s);
    ks_program_free(prog);
    char *r = m->out.p ? m->out.p : calloc(1, 1);
    free(m);
    return r;
}

static int run_file(const char *path, int verbose)
{
    size_t len;
    char *src = slurp(path, &len);
    if (!src) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    char *got = run_program(src, len);
    free(src);
    char out_path[1024];
    snprintf(out_path, sizeof out_path, "%.*s.out", (int)(strlen(path) - 5), path);
    char *want = slurp(out_path, NULL);
    int ok = want && strcmp(got, want) == 0;
    if (verbose || !ok) {
        if (!ok) fprintf(stderr, "FAIL %s\n", path);
        if (!want) fprintf(stderr, "  (missing %s)\n", out_path);
        if (!ok || verbose) {
            fprintf(stderr, "--- got ---\n%s", got);
            if (want && !ok) fprintf(stderr, "--- want ---\n%s", want);
        }
    }
    free(got);
    free(want);
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Unit tests

static void test_number_table(const char *dir)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/run/number_table.txt", dir);
    char *t = slurp(path, NULL);
    CHECK(t != NULL);
    if (!t) return;
    int rows = 0, bad = 0;
    for (char *line = strtok(t, "\n"); line; line = strtok(NULL, "\n")) {
        unsigned long long bits = strtoull(line, NULL, 16);
        double v;
        memcpy(&v, &bits, 8);
        char out[32];
        ks_num_format(v, out);
        rows++;
        if (strcmp(out, line + 17) != 0) {
            if (bad++ < 5) fprintf(stderr, "number_table: %s got %s\n", line, out);
        }
    }
    free(t);
    CHECK(rows > 1000);
    CHECK(bad == 0);
}

static void fmt(double d, char *out) { ks_num_format(d, out); }

static void test_parse(void)
{
    char o[32];
    struct { const char *in; const char *pf, *pi, *num; } cases[] = {
        {"", "NaN", "NaN", "0"},
        {"  12abc", "12", "12", "NaN"},
        {"0x10", "0", "16", "16"},
        {"-0x10", "0", "-16", "NaN"},
        {" \n\t 3.5e3x", "3500", "3", "NaN"},
        {".5", "0.5", "NaN", "0.5"},
        {".", "NaN", "NaN", "NaN"},
        {"1.", "1", "1", "1"},
        {"1e", "1", "1", "NaN"},
        {"-Infinity", "-Infinity", "NaN", "-Infinity"},
        {"Infinityx", "Infinity", "NaN", "NaN"},
        {"\xc2\xa0 7", "7", "7", "7"},
        {"0b101", "0", "0", "5"},
        {"0o17", "0", "0", "15"},
        {"  -12  ", "-12", "-12", "-12"},
        {"\xef\xbb\xbf" "42", "42", "42", "42"},
        {"1e1000", "Infinity", "1", "Infinity"},
        {"+-1", "NaN", "NaN", "NaN"},
        {"  0X1F  ", "0", "31", "31"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        size_t n = strlen(cases[i].in);
        fmt(ks_parse_float(cases[i].in, n), o);
        if (strcmp(o, cases[i].pf)) fprintf(stderr, "parseFloat(%s) = %s\n", cases[i].in, o);
        CHECK(strcmp(o, cases[i].pf) == 0);
        fmt(ks_parse_int(cases[i].in, n), o);
        if (strcmp(o, cases[i].pi)) fprintf(stderr, "parseInt(%s) = %s\n", cases[i].in, o);
        CHECK(strcmp(o, cases[i].pi) == 0);
        fmt(ks_string_to_number(cases[i].in, n), o);
        if (strcmp(o, cases[i].num)) fprintf(stderr, "Number(%s) = %s\n", cases[i].in, o);
        CHECK(strcmp(o, cases[i].num) == 0);
    }
}

static void null_log(void *u, int e, const char *l) { (void)u; (void)e; (void)l; }

static KsScript *empty_script(KsProgram **prog)
{
    static const char src[] = "num unused = 0;";
    *prog = ks_compile(src, sizeof src - 1, KS_SIDE_CLIENT, NULL, NULL, 0, NULL);
    static KsHost host = {KS_SIDE_CLIENT, null_log, NULL, NULL};
    return *prog ? ks_script_new(*prog, &host, NULL) : NULL;
}

static void test_serial(void)
{
    KsProgram *prog;
    KsScript *s = empty_script(&prog);
    CHECK(s != NULL);
    if (!s) return;
    KsValue o = ks_obj_new(s);
    ks_obj_set(s, o, "n", ks_num(1.5));
    ks_obj_set(s, o, "s", ks_str(s, "h\xc3\xa9\"", 4));
    KsValue a = ks_arr_new(s);
    ks_arr_push(s, a, ks_bool(1));
    ks_arr_push(s, a, ks_null());
    ks_arr_push(s, a, ks_undef());
    ks_obj_set(s, o, "a", a);
    ks_obj_set(s, o, "u", ks_undef());
    // {"V_n":1.5,"V_s":"hé\"","V_a":[true,null,null]}  ("u" omitted)
    CHECK(ks_json_length_prefixed(s, o) == strlen("{\"V_n\":1.5,\"V_s\":\"h\\\"\",\"V_a\":[true,null,null]}") + 1);
    CHECK(ks_json_length_prefixed(s, ks_num(12.5)) == 4);
    CHECK(ks_json_length_prefixed(s, ks_null()) == 4);
    CHECK(ks_json_length_prefixed(s, ks_str(s, "\xf0\x9f\x98\x80", 4)) == 2);
    uint8_t buf[256];
    size_t n = ks_serialize(s, o, buf, sizeof buf);
    CHECK(n > 0 && n < sizeof buf);
    CHECK(ks_serialize(s, o, NULL, 0) == n); // size query
    int ok = 0;
    KsValue back = ks_deserialize(s, buf, n, &ok);
    CHECK(ok);
    CHECK(back.tag == KS_OBJ);
    CHECK(ks_to_number(ks_obj_get(s, back, "n")) == 1.5);
    size_t sl;
    const char *sv = ks_str_utf8(ks_obj_get(s, back, "s"), &sl);
    CHECK(sv && sl == 4 && memcmp(sv, "h\xc3\xa9\"", 4) == 0);
    KsValue ba = ks_obj_get(s, back, "a");
    CHECK(ks_arr_len(ba) == 3);
    CHECK(ks_arr_get(s, ba, 0).tag == KS_BOOL && ks_arr_get(s, ba, 2).tag == KS_UNDEF);
    CHECK(ks_obj_get(s, back, "u").tag == KS_UNDEF);
    // truncated / garbage input
    ks_deserialize(s, buf, n - 1, &ok);
    CHECK(!ok);
    uint8_t junk[] = {9, 1, 2};
    ks_deserialize(s, junk, sizeof junk, &ok);
    CHECK(!ok);
    // cycles: JSON.stringify throws -> SIZE_MAX, serialize fails
    ks_obj_set(s, o, "self", o);
    CHECK(ks_json_length_prefixed(s, o) == (size_t)-1);
    CHECK(ks_serialize(s, o, buf, sizeof buf) == 0);
    // ToString
    char t[64];
    ks_to_string(s, a, t, sizeof t);
    CHECK(strcmp(t, "true,,") == 0);
    ks_to_string(s, ks_num(-0.0), t, sizeof t);
    CHECK(strcmp(t, "0") == 0);
    ks_script_free(s);
    ks_program_free(prog);
}

// The official "sd" packets (shared/src/ks/ks_net_official.c, deob-10.0.3.js GAME.NETWORK :209434-209469): a msgpack
// [id, data] whose KrunkScript keys are "V_" + name, to and from our SCRIPT_MSG body.
static void test_official_net(void)
{
    KsProgram *prog;
    KsScript *s = empty_script(&prog);
    CHECK(s != NULL);
    if (!s) return;
    // S2C: {V_a: 1, V_b: [true, "x", nil], V_c: {V_d: 2.5}, 7: "seven"} -> data.a, data.b, data.c.d, data["7"]
    const KrunkerValue list[] = {KRUNKER_V_BOOL(1), KRUNKER_V_STR("x"), KRUNKER_V_NIL};
    const KrunkerValue inner[] = {KRUNKER_V_STR("V_d"), KRUNKER_V_NUM(2.5)};
    const KrunkerValue kv[] = {KRUNKER_V_STR("V_a"), KRUNKER_V_NUM(1), KRUNKER_V_STR("V_b"), KRUNKER_V_ARRAY(list, 3),
                               KRUNKER_V_STR("V_c"), KRUNKER_V_MAP(inner, 1), KRUNKER_V_NUM(7), KRUNKER_V_STR("seven")};
    const KrunkerValue id = KRUNKER_V_STR("hit"), data = KRUNKER_V_MAP(kv, 4);
    uint8_t body[512];
    const int len = ks_net_body_from_official(&id, &data, body, sizeof body);
    CHECK(len > 0);
    char got_id[KS_NET_ID_MAX + 1];
    KsValue v = ks_undef();
    CHECK(len > 0 && ks_net_decode_msg(s, body, (size_t)len, got_id, sizeof got_id, &v));
    CHECK(!strcmp(got_id, "hit") && v.tag == KS_OBJ);
    CHECK(ks_to_number(ks_obj_get(s, v, "a")) == 1);
    KsValue b = ks_obj_get(s, v, "b");
    CHECK(ks_arr_len(b) == 3 && ks_arr_get(s, b, 0).tag == KS_BOOL && ks_arr_get(s, b, 2).tag == KS_NULL);
    CHECK(ks_to_number(ks_obj_get(s, ks_obj_get(s, v, "c"), "d")) == 2.5);
    size_t sl;
    const char *sv = ks_str_utf8(ks_obj_get(s, v, "7"), &sl);
    CHECK(sv && sl == 5 && !memcmp(sv, "seven", 5));
    CHECK(ks_obj_get(s, v, "V_a").tag == KS_UNDEF); // the prefix is gone
    // a number id is its String(); an id over KS_NET_ID_MAX is refused
    const KrunkerValue num_id = KRUNKER_V_NUM(42), nil = KRUNKER_V_NIL;
    CHECK(ks_net_body_from_official(&num_id, &nil, body, sizeof body) == 4 && body[0] == 2 && body[1] == '4' && body[2] == '2' && body[3] == 1);
    char long_id[80];
    memset(long_id, 'i', sizeof long_id - 1);
    long_id[sizeof long_id - 1] = 0;
    const KrunkerValue big_id = KRUNKER_V_STR(long_id);
    CHECK(ks_net_body_from_official(&big_id, &nil, body, sizeof body) == -1);

    // C2S: our {n: 1.5, arr: [true, null, undefined], o: {k: "v"}} -> {V_n: 1.5, V_arr: [true, nil, nil], V_o: {V_k: "v"}}
    KsValue o = ks_obj_new(s);
    ks_obj_set(s, o, "n", ks_num(1.5));
    KsValue a = ks_arr_new(s);
    ks_arr_push(s, a, ks_bool(1));
    ks_arr_push(s, a, ks_null());
    ks_arr_push(s, a, ks_undef());
    ks_obj_set(s, o, "arr", a);
    KsValue k = ks_obj_new(s);
    ks_obj_set(s, k, "k", ks_str(s, "v", 1));
    ks_obj_set(s, o, "o", k);
    const int out_len = ks_net_encode_msg(s, "msg", o, body, sizeof body);
    CHECK(out_len > 0);
    KsNetOfficial off;
    char out_id[KS_NET_ID_MAX + 1];
    CHECK(out_len > 0 && ks_net_body_to_official(body, (size_t)out_len, out_id, sizeof out_id, &off));
    CHECK(!strcmp(out_id, "msg") && off.data.type == KRUNKER_MAP && off.data.len == 3);
    if (off.data.type == KRUNKER_MAP && off.data.len == 3) {
        const KrunkerValue *items = off.data.u.items;
        CHECK(!strcmp(items[0].u.str, "V_n") && items[1].type == KRUNKER_NUM && items[1].u.num == 1.5);
        CHECK(!strcmp(items[2].u.str, "V_arr") && items[3].type == KRUNKER_ARRAY && items[3].len == 3);
        CHECK(items[3].u.items[0].type == KRUNKER_BOOL && items[3].u.items[1].type == KRUNKER_NIL && items[3].u.items[2].type == KRUNKER_NIL);
        CHECK(!strcmp(items[4].u.str, "V_o") && items[5].type == KRUNKER_MAP && !strcmp(items[5].u.items[0].u.str, "V_k") &&
              items[5].u.items[1].type == KRUNKER_STR && !strcmp(items[5].u.items[1].u.str, "v"));
    }
    ks_net_official_free(&off);
    // a truncated body decodes to nothing
    CHECK(!ks_net_body_to_official(body, (size_t)out_len - 1, out_id, sizeof out_id, &off));
    ks_script_free(s);
    ks_program_free(prog);
}

// Heap accounting: cyclic garbage is reclaimed, pinned values survive.
size_t ks_test_heap_bytes(KsScript *s);
void ks_gc_collect(KsScript *s);

static void test_gc(void)
{
    KsProgram *prog;
    KsScript *s = empty_script(&prog);
    CHECK(s != NULL);
    if (!s) return;
    ks_gc_collect(s);
    size_t base = ks_test_heap_bytes(s);
    KsValue keep = ks_obj_new(s);
    ks_handle_pin(s, keep);
    for (int i = 0; i < 20000; i++) {
        KsValue a = ks_obj_new(s), b = ks_obj_new(s);
        ks_obj_set(s, a, "peer", b);
        ks_obj_set(s, b, "peer", a);
        ks_obj_set(s, a, "self", a);
        if (i % 1000 == 0) ks_obj_set(s, keep, "last", a);
    }
    CHECK(ks_test_heap_bytes(s) > base + 20000 * 64);
    ks_gc_collect(s);
    CHECK(ks_test_heap_bytes(s) < base + 4096);
    KsValue last = ks_obj_get(s, keep, "last");
    CHECK(last.tag == KS_OBJ);
    CHECK(ks_obj_get(s, ks_obj_get(s, last, "peer"), "peer").as.ref == last.as.ref);
    ks_handle_unpin(s, keep);
    ks_gc_collect(s);
    CHECK(ks_test_heap_bytes(s) < base + 256);
    ks_script_free(s);
    ks_program_free(prog);
}

// ---------------------------------------------------------------------------
// Corpus runtime smoke test

static int is_allowed(const char *allow, const char *name, const char *line)
{
    if (!allow) return 0;
    char key[512];
    snprintf(key, sizeof key, "%s %s\n", name, line);
    return strstr(allow, key) != NULL;
}

static int run_corpus_source(const char *name, const char *src, size_t len, const char *allow)
{
    KsSide side = strstr(name, "_server") ? KS_SIDE_SERVER : KS_SIDE_CLIENT;
    KsDiag err = {0};
    KsProgram *prog = ks_compile(src, len, side, &err, NULL, 0, NULL);
    if (!prog) {
        fprintf(stderr, "FAIL corpus %s: compile %d:%d %s\n", name, err.line, err.col, err.message);
        return 1;
    }
    Mock *m = calloc(1, sizeof *m);
    if (!m) abort();
    mock_reset(m);
    KsHost host = {side, mock_log, mock_now, g_api};
    KsScript *s = ks_script_new(prog, &host, m);
    KsValue args[3];
    if (!ks_script_failed(s)) {
        ks_script_call_hook(s, "start", NULL, 0);
        for (int f = 0; f < 3; f++) {
            args[0] = ks_num(16);
            ks_script_call_hook(s, "update", args, 1);
            ks_script_call_hook(s, "render", args, 1);
        }
        args[0] = ks_str(s, "7", 1);
        args[1] = ks_num(16);
        args[2] = ks_obj_new(s);
        ks_script_call_hook(s, "onPlayerUpdate", args, 3);
    }
    int bad = 0;
    for (char *line = m->out.p ? strtok(m->out.p, "\n") : NULL; line; line = strtok(NULL, "\n")) {
        if (line[0] == '!' && !is_allowed(allow, name, line)) {
            fprintf(stderr, "FAIL corpus %s: %s\n", name, line);
            bad = 1;
        }
    }
    ks_script_free(s);
    ks_program_free(prog);
    free(m->out.p);
    free(m);
    return bad;
}

// ---------------------------------------------------------------------------
// Fuzz: every hook of every corpus script with random argument values (any type), and
// random bytes into ks_deserialize. Runtime errors are fine; crashes/ASan reports are not.

static uint32_t g_rng = 0x12345678u;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static KsValue random_value(KsScript *s, Mock *m, int depth)
{
    switch (rnd() % (depth > 2 ? 7 : 10)) {
    case 0: return ks_undef();
    case 1: return ks_null();
    case 2: return ks_bool((int)(rnd() & 1));
    case 3: return ks_num((double)(int32_t)rnd() / (double)(1 + rnd() % 1000));
    case 4: {
        static const double special[] = {0, -0.0, 1.0 / 0.0, -1.0 / 0.0, 0.0 / 0.0, 1e21, 5e-324, 2147483648.0};
        return ks_num(special[rnd() % 8]);
    }
    case 5: {
        static const char *const strs[] = {"", "7", "9", "startBtn", "key", "\xc3\xa9\xf0\x9f\x98\x80", "0x10", " 12abc"};
        const char *t = strs[rnd() % 8];
        return ks_str(s, t, strlen(t));
    }
    case 6: return player_value(s, m, (int)(rnd() % 2));
    case 7: {
        KsValue a = ks_arr_new(s);
        for (uint32_t i = rnd() % 4; i > 0; i--) ks_arr_push(s, a, random_value(s, m, depth + 1));
        return a;
    }
    default: {
        static const char *const keys[] = {"x", "y", "id", "movDir", "data", "width", "jump"};
        KsValue o = ks_obj_new(s);
        for (uint32_t i = rnd() % 4; i > 0; i--) ks_obj_set(s, o, keys[rnd() % 7], random_value(s, m, depth + 1));
        return o;
    }
    }
}

static const char *const g_hooks[] = {
    "start", "update", "render", "onPlayerSpawn", "onPlayerDeath", "onPlayerUpdate", "onKeyPress", "onKeyUp",
    "onKeyHeld", "onControllerPress", "onControllerUp", "onControllerHeld", "onMouseClick", "onMouseUp",
    "onMouseScroll", "onDIVClicked", "onNetworkMessage", "onPlayerDamage", "onCustomTrigger", "onChatMessage",
    "onPlayerLeave", "onGameEnd", "onAdFinished", "onServerClosed",
};

static int fuzz_corpus_source(const char *name, const char *src, size_t len)
{
    KsSide side = strstr(name, "_server") ? KS_SIDE_SERVER : KS_SIDE_CLIENT;
    KsProgram *prog = ks_compile(src, len, side, NULL, NULL, 0, NULL);
    if (!prog) return 1;
    Mock *m = calloc(1, sizeof *m);
    if (!m) abort();
    mock_reset(m);
    KsHost host = {side, mock_log, mock_now, g_api};
    KsScript *s = ks_script_new(prog, &host, m);
    for (int round = 0; round < 40 && !ks_script_failed(s); round++) {
        for (size_t h = 0; h < sizeof g_hooks / sizeof g_hooks[0]; h++) {
            if (!ks_script_has_hook(s, g_hooks[h])) continue;
            KsValue args[4];
            int argc = (int)(rnd() % 5);
            for (int i = 0; i < argc; i++) args[i] = random_value(s, m, 0);
            ks_script_call_hook(s, g_hooks[h], args, argc);
        }
        m->out.len = 0; // keep the capture small
    }
    ks_script_free(s);
    ks_program_free(prog);
    free(m->out.p);
    free(m);
    return 0;
}

static int run_corpus_file(const char *dir, const char *name, const char *allow)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/corpus/%s", dir, name);
    size_t len;
    char *src = slurp(path, &len);
    if (!src) return 1;
    int bad = run_corpus_source(name, src, len, allow);
    bad += fuzz_corpus_source(name, src, len);
    free(src);
    return bad;
}

// ---------------------------------------------------------------------------
// Compiled-JS map scripts through the importer (WP7): DIR/js/*.js

static int run_imported_js(const char *dir, const char *name)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/js/%s", dir, name);
    size_t len;
    char *js = slurp(path, &len);
    if (!js) return 1;
    KsSide side = strstr(name, "_server") ? KS_SIDE_SERVER : KS_SIDE_CLIENT;
    KsDiag err = {0};
    size_t klen = 0;
    char *src = ks_import_js(js, len, side, &klen, &err);
    free(js);
    if (!src) {
        fprintf(stderr, "FAIL js %s: import %d:%d %s\n", name, err.line, err.col, err.message);
        return 1;
    }
    int bad = run_corpus_source(name, src, klen, NULL);
    bad += fuzz_corpus_source(name, src, klen);

    // NAME.run: directives run against the imported script, output compared with NAME.out
    char base[1024];
    snprintf(base, sizeof base, "%s/js/%.*s", dir, (int)(strlen(name) - 3), name);
    snprintf(path, sizeof path, "%s.run", base);
    size_t rlen;
    char *run = slurp(path, &rlen);
    if (run) {
        char *prog = malloc(klen + rlen + 2);
        if (!prog) abort();
        memcpy(prog, src, klen);
        prog[klen] = '\n';
        memcpy(prog + klen + 1, run, rlen + 1);
        char *got = run_program(prog, klen + 1 + rlen);
        snprintf(path, sizeof path, "%s.out", base);
        char *want = slurp(path, NULL);
        if (!want || strcmp(got, want) != 0) {
            fprintf(stderr, "FAIL js %s\n--- got ---\n%s--- want ---\n%s", name, got, want ? want : "(missing .out)\n");
            bad++;
        }
        free(want);
        free(got);
        free(prog);
        free(run);
    }
    free(src);
    return bad;
}

static void fuzz_deserialize(void)
{
    KsProgram *prog;
    KsScript *s = empty_script(&prog);
    if (!s) return;
    uint8_t buf[64];
    int accepted = 0;
    for (int i = 0; i < 20000; i++) {
        size_t n = 1 + rnd() % sizeof buf;
        for (size_t k = 0; k < n; k++) buf[k] = (uint8_t)(rnd() % 3 == 0 ? rnd() % 8 : rnd());
        int ok;
        ks_deserialize(s, buf, n, &ok);
        accepted += ok;
    }
    CHECK(accepted >= 0);
    ks_script_free(s);
    ks_program_free(prog);
}

// ---------------------------------------------------------------------------

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int list_dir(const char *dir, const char *suffix, char ***out)
{
    int n = 0;
#ifndef _WIN32
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int cap = 0;
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name), sl = strlen(suffix);
        if (l <= sl || strcmp(e->d_name + l - sl, suffix)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            *out = realloc(*out, (size_t)cap * sizeof(char *));
            if (!*out) abort();
        }
        size_t nl = strlen(e->d_name) + 1;
        (*out)[n] = malloc(nl);
        if (!(*out)[n]) abort();
        memcpy((*out)[n++], e->d_name, nl);
    }
    closedir(d);
    if (n) qsort(*out, (size_t)n, sizeof(char *), cmp_str);
#else
    (void)dir; (void)suffix; (void)out;
#endif
    return n;
}

int main(int argc, char **argv)
{
    mock_init_api();
    if (argc > 1 && strlen(argv[1]) > 5 && !strcmp(argv[1] + strlen(argv[1]) - 5, ".krnk")) {
        int rc = 0;
        for (int i = 1; i < argc; i++) rc |= run_file(argv[i], 1);
        return rc;
    }
    const char *dir = argc > 1 ? argv[1] : KS_TEST_DIR;

    test_number_table(dir);
    test_parse();
    test_serial();
    test_official_net();
    test_gc();

    char run_dir[1024];
    snprintf(run_dir, sizeof run_dir, "%s/run", dir);
    char **files = NULL;
    int n = list_dir(run_dir, ".krnk", &files);
    int run_fail = 0;
    for (int i = 0; i < n; i++) {
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", run_dir, files[i]);
        run_fail += run_file(path, 0);
        free(files[i]);
    }
    free(files);
    CHECK(n > 0);

    char corpus_dir[1024], allow_path[1024];
    snprintf(corpus_dir, sizeof corpus_dir, "%s/corpus", dir);
    snprintf(allow_path, sizeof allow_path, "%s/run/corpus.allow", dir);
    char *allow = slurp(allow_path, NULL);
    files = NULL;
    int nc = list_dir(corpus_dir, ".krnk", &files);
    int corpus_fail = 0;
    for (int i = 0; i < nc; i++) {
        corpus_fail += run_corpus_file(dir, files[i], allow);
        free(files[i]);
    }
    fuzz_deserialize();
    free(files);
    free(allow);

    char js_dir[1024];
    snprintf(js_dir, sizeof js_dir, "%s/js", dir);
    files = NULL;
    int nj = list_dir(js_dir, ".js", &files);
    int js_fail = 0;
    for (int i = 0; i < nj; i++) {
        js_fail += run_imported_js(dir, files[i]);
        free(files[i]);
    }
    free(files);
    CHECK(nj >= 2);
    corpus_fail += js_fail;

    printf("ks_vm_test: %d unit checks (%d failed), %d programs (%d failed), %d corpus scripts + %d imported JS "
           "scripts (%d failed)\n", g_checks, g_fail, n, run_fail, nc, nj, corpus_fail);
    return g_fail || run_fail || corpus_fail ? 1 : 0;
}
