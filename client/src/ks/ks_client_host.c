// KrunkScript client host (doc/v7/krunkscript.md 2.2, 2.4, 3, 4.1, 6.6 WP4): the client's
// KsHost, the script lifecycle per INIT, the event hooks and the client-side GAME.PLAYERS,
// GAME.NETWORK and GAME.DEFAULT.disable3D entries. The namespaces live next to this file
// (ks_ui.c, ks_scene.c, ks_sound.c, ks_input.c, ks_misc.c); GAME.OVERLAY is WP5 (ks_overlay.h).
//
// Lifecycle (v7 index.js:13493-13539, game.js:3789): every INIT drops the previous instance
// (Game.mainScript = null), empties #customGUIHolder (overlay.resetAll), parses the map's
// client script again (globals re-initialised every round) and calls start() once the round is
// set up. KN additionally removes everything the old instance created (2.2 KN rules).
#include "ks_client_internal.h"
#include <ks_client.h>
#include <ks_overlay.h>
#include <console.h>
#include <gltf.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <game_config.h> // class_overrides: player.assetID
#include <net_ext.h> // NET_PLAYER_ASSET_TEXT
#include <player_class_model.h> // player.assetID: the class model's swap
#include <entities.h> // v10.0.3 entities: the client half of game.update
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

KsClientState g_ksc;

// ---------------------------------------------------------------------------
// helpers (ks_client_internal.h)

int ksc_is_str(const KsValue v) {
    size_t len = 0;
    return v.tag == KS_STR && ks_str_utf8(v, &len) && len > 0;
}

int ksc_parse_hex(const char *s, vec4 *out) {
    if (!s || s[0] != '#') return 0;
    const size_t len = strlen(s + 1);
    if (len != 3 && len != 6) return 0;
    unsigned int v[6];
    for (size_t i = 0; i < len; i++) {
        const char c = s[1 + i];
        if (c >= '0' && c <= '9') v[i] = (unsigned int) (c - '0');
        else if (c >= 'a' && c <= 'f') v[i] = (unsigned int) (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v[i] = (unsigned int) (c - 'A' + 10);
        else return 0;
    }
    if (out) {
        if (len == 3) *out = (vec4) {(float) (v[0] * 17) / 255.0f, (float) (v[1] * 17) / 255.0f, (float) (v[2] * 17) / 255.0f, 1.0f};
        else *out = (vec4) {(float) (v[0] * 16 + v[1]) / 255.0f, (float) (v[2] * 16 + v[3]) / 255.0f, (float) (v[4] * 16 + v[5]) / 255.0f, 1.0f};
    }
    return 1;
}

int ksc_is_hex(const KsValue v) {
    if (v.tag != KS_STR) return 0;
    return ksc_parse_hex(ks_str_utf8(v, NULL), NULL);
}

const char *ksc_str(KsScript *s, const KsValue v, char *buf, const size_t n) {
    if (!n) return "";
    buf[0] = 0;
    if (s) ks_to_string(s, v, buf, n);
    return buf;
}

int ksc_truthy(const KsValue v) {
    switch (v.tag) {
        case KS_UNDEF:
        case KS_NULL: return 0;
        case KS_BOOL: return v.as.b != 0;
        case KS_NUM: return v.as.num != 0 && v.as.num == v.as.num;
        case KS_STR: {
            size_t len = 0;
            ks_str_utf8(v, &len);
            return len > 0;
        }
        default: return 1;
    }
}

// F(x) = parseFloat(ToString(x)) || 0 (host setters, 1.12)
double ksc_float(const KsValue v) {
    double d = 0.0;
    if (v.tag == KS_NUM) d = v.as.num;
    else if (v.tag == KS_STR) d = ks_overlay_js_parse_float(ks_str_utf8(v, NULL));
    else if (v.tag == KS_ARR && ks_arr_len(v) == 1) return ksc_float(ks_arr_get(NULL, v, 0)); // [x] -> ToString = String(x)
    return d == d && d != 0.0 ? d : 0.0;
}

KsValue ksc_opt(KsScript *s, const KsValue obj, const char *name) {
    if (obj.tag != KS_OBJ && obj.tag != KS_HOST) return ks_undef();
    return ks_obj_get(s, obj, name);
}

KsValue ksc_xy(KsScript *s, const char *kx, const double x, const char *ky, const double y) {
    const KsValue o = ks_obj_new(s);
    ks_obj_set(s, o, kx, ks_num(x));
    ks_obj_set(s, o, ky, ks_num(y));
    return o;
}

KsScript *ksc_script(void) {
    return g_ksc.client ? ks_game_script(&g_ksc.client->game) : NULL;
}

Game *ksc_game(void) { return g_ksc.client ? &g_ksc.client->game : NULL; }

// GAME.log / runtime errors (2.8): "[KRUNKSCRIPT] <text>" in the developer console (cyan,
// errors dark red as v7's console styles) and on stdout / the browser console.
// KRUNKNATIVE_KS_LOG_STDOUT=1 echoes it on stdout natively too (the client script tests, client/tests/ks_scene_e2e.py).
static void ksc_console(const int is_error, const char *text) {
    ui_console_printf(is_error ? "#ff6b6b" : "#00e5ff", "[KRUNKSCRIPT] %s", text);
#ifdef __EMSCRIPTEN__
    fprintf(stderr, "[KRUNKSCRIPT] %s%s\n", is_error ? "error: " : "", text);
#else
    static int echo = -1;
    if (echo < 0) {
        const char *v = getenv("KRUNKNATIVE_KS_LOG_STDOUT");
        echo = v && v[0] && v[0] != '0';
    }
    if (echo) {
        printf("[KRUNKSCRIPT] %s%s\n", is_error ? "error: " : "", text);
        fflush(stdout);
    }
#endif
}

void ksc_log(const int is_error, const char *fmt, ...) {
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    ksc_console(is_error, text);
}

void ksc_warn_light(const char *what) {
    if (g_ksc.warned_lights) return;
    g_ksc.warned_lights = 1;
    ksc_log(0, "%s: the Krunker Box Office renderer has no %s yet; the object is a placeholder (methods work, no light)", what, what);
}

int ksc_call(const char *hook, const KsValue *args, const int argc) {
    if (!ksc_script()) return -1;
    return ks_game_call(ksc_game(), hook, args, argc);
}

int ksc_has_hook(const char *hook) { return ksc_script() && ks_game_has_hook(ksc_game(), hook); }

static void host_log(void *user, const int is_error, const char *line) {
    (void) user;
    ksc_console(is_error, line);
}

static double host_now(void *user) {
    (void) user;
    return ks_wall_ms(); // Date.now() (portable: MinGW has no timespec_get)
}

// ---------------------------------------------------------------------------
// GAME.PLAYERS (client half, API:3100-3141) and the client-only player members (3.2.1)

// getSelf: the local player (`null` before spawn / while spectating)
static KsValue p_get_self(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u; (void) a; (void) n;
    Client *client = g_ksc.client;
    if (!client || !client->me) return ks_null();
    return ks_game_wrap_player(s, &client->game, client->me);
}

static int g_use_lod = 1;      // game.useLOD (no LOD models in KN: recorded only)
static int g_disable_meshes;   // game.disableMeshes

static KsValue p_toggle_lod(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u;
    g_use_lod = ksc_truthy(ksc_arg(a, n, 0));
    return ks_undef();
}

static KsValue p_disable_meshes(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u; (void) a; (void) n;
    g_disable_meshes = 1;
    return ks_undef();
}

int ks_client_players_hidden(void) { return g_disable_meshes; }

// Script-hidden players (player.visible = false): uid list
#define KSC_HIDDEN_MAX 64
static int g_hidden_uid[KSC_HIDDEN_MAX];
static int g_hidden_count;

static int player_hidden(const int uid) {
    for (int i = 0; i < g_hidden_count; i++) {
        if (g_hidden_uid[i] == uid) return 1;
    }
    return 0;
}

static void player_set_hidden(const int uid, const int hidden) {
    for (int i = 0; i < g_hidden_count; i++) {
        if (g_hidden_uid[i] != uid) continue;
        if (!hidden) g_hidden_uid[i] = g_hidden_uid[--g_hidden_count];
        return;
    }
    if (hidden && g_hidden_count < KSC_HIDDEN_MAX) g_hidden_uid[g_hidden_count++] = uid;
}

static KsValue pc_get_visible(KsScript *s, void *native, uint32_t id) {
    (void) s; (void) id;
    const Player *p = ks_player_of(native);
    return ks_bool(p && p->mesh && !g_disable_meshes && !player_hidden(p->uid));
}

static void pc_set_visible(KsScript *s, void *native, uint32_t id, KsValue v) {
    (void) s; (void) id;
    const Player *p = ks_player_of(native);
    if (p && p->mesh) player_set_hidden(p->uid, !ksc_truthy(v));
}

// inView: the model is drawn and inside the camera frustum (index.js:14814 frustum test)
static KsValue pc_get_in_view(KsScript *s, void *native, uint32_t id) {
    (void) s; (void) id;
    const Player *p = ks_player_of(native);
    Client *client = g_ksc.client;
    if (!p || !client || !p->active || p->is_you || g_disable_meshes || player_hidden(p->uid)) return ks_bool(0);
    const float *v = client->camera.world_inverse_matrix, *pr = client->camera.projection_matrix;
    const float x = p->position.x, y = p->position.y + p->height * 0.5f, z = p->position.z;
    const float cx = v[0] * x + v[1] * y + v[2] * z + v[3];
    const float cy = v[4] * x + v[5] * y + v[6] * z + v[7];
    const float cz = v[8] * x + v[9] * y + v[10] * z + v[11];
    if (cz >= 0.0f) return ks_bool(0);
    const float clip_x = pr[0] * cx, clip_y = pr[5] * cy, w = -cz;
    return ks_bool(fabsf(clip_x) <= w * 1.1f && fabsf(clip_y) <= w * 1.1f);
}

static KsValue pc_get_asset(KsScript *s, void *native, const KsValue *a, int n) {
    (void) a; (void) n;
    const Player *p = ks_player_of(native);
    KsValue out = ks_undef();
    if (p) ks_scene_player_asset(s, p, &out);
    return out;
}

static KsValue pc_play_anim(KsScript *s, void *native, const KsValue *a, int n) {
    const Player *p = ks_player_of(native);
    const KsValue clip = ksc_arg(a, n, 0);
    if (!p || !ksc_is_str(clip)) return ks_undef();
    char buf[64];
    ks_scene_player_play_anim(p, ksc_str(s, clip, buf, sizeof(buf)), (int) ksc_num_or(ksc_arg(a, n, 1), 1));
    return ks_undef();
}

static KsValue pc_stop_anim(KsScript *s, void *native, const KsValue *a, int n) {
    (void) s; (void) a; (void) n;
    const Player *p = ks_player_of(native);
    if (p) ks_scene_player_stop_anim(p);
    return ks_undef();
}

// get assetID(): `config && config.aID || ""` (deob-10.0.3.js:207880-207882), the class's, never a swap
static KsValue pc_get_asset_id(KsScript *s, void *native, uint32_t id) {
    (void) id;
    const Player *p = ks_player_of(native);
    if (!p) return ks_undef();
    const Game *game = p->game;
    const ClassConfig *c = game && game->classes && p->class_index >= 0 && (size_t) p->class_index < game->class_count ? &game->classes[p->class_index] : NULL;
    const char *aid = class_overrides(c)->asset_id;
    return ks_str(s, aid, strlen(aid));
}

// set assetID(v): players.updateAssetID(player, v += "") (:207883-207887, :197964-197975) on this client: with
// config.mdl and a non-empty text it waits in assetTxt and an active player's model is generated again at once
// (regenMeshes), which takes it (player_class_model.h)
static void pc_set_asset_id(KsScript *s, void *native, uint32_t id, KsValue v) {
    (void) id;
    Player *p = ks_player_of(native);
    Client *client = g_ksc.client;
    if (!p) return;
    char text[NET_PLAYER_ASSET_TEXT];
    ks_to_string(s, v, text, sizeof(text));
    if (!player_class_model_set_asset(p, text) || !client || !p->active || !p->mesh) return;
    const int render_you = p->render_you;
    client_drop_player_mesh(client, p);
    client_add_player_mesh(client, p, render_you);
}

static const KsHostProp g_client_player_props[] = {
    {"assetID", pc_get_asset_id, pc_set_asset_id, NULL, 0},
    {"visible", pc_get_visible, pc_set_visible, NULL, 0},
    {"inView", pc_get_in_view, NULL, NULL, 0},
    {"getAsset", NULL, NULL, pc_get_asset, 0},
    {"playAnim", NULL, NULL, pc_play_anim, 0},
    {"stopAnim", NULL, NULL, pc_stop_anim, 0},
};

// The shared player class plus the client-only members; a shared member of the same name
// is replaced (the shared table may carry gated placeholders for them).
static void build_player_class(void) {
    if (g_ksc.player_props) return;
    const int extra = (int) (sizeof(g_client_player_props) / sizeof(g_client_player_props[0]));
    g_ksc.player_props = calloc((size_t) (ks_player_prop_count + extra), sizeof(KsHostProp));
    if (!g_ksc.player_props) return;
    int count = 0;
    for (int i = 0; i < ks_player_prop_count; i++) {
        int replaced = 0;
        for (int k = 0; k < extra; k++) replaced |= !strcmp(ks_player_props[i].name, g_client_player_props[k].name);
        if (!replaced) g_ksc.player_props[count++] = ks_player_props[i];
    }
    for (int k = 0; k < extra; k++) g_ksc.player_props[count++] = g_client_player_props[k];
    g_ksc.player_class = (KsHostClass) {"player", g_ksc.player_props, count};
}

// Hides (or shows again) every mesh of a player model: PLAYERS.disableMeshes (v7 builds the
// players without meshes) and player.visible = false (objInstances.visible).
static void mesh_set(Mesh *mesh, const int visible) {
    if (mesh) mesh->visible = visible;
}

static void color_cube_set(const ColorCube *cube, const int visible) {
    if (!cube) return;
    for (size_t i = 0; i < cube->mesh_count; i++) mesh_set(cube->meshes[i], visible);
}

static void player_model_set_visible(const Player *player, const int visible) {
    const PlayerMesh *pm = player->mesh;
    if (!pm) return;
    color_cube_set(pm->head, visible);
    color_cube_set(pm->body, visible);
    for (int i = 0; i < 2; i++) {
        color_cube_set(pm->legs[i], visible);
        if (pm->crouched_legs[i]) {
            mesh_set(pm->crouched_legs[i]->upper, visible);
            mesh_set(pm->crouched_legs[i]->joint, visible);
            color_cube_set(pm->crouched_legs[i]->lower, visible);
        }
    }
    for (size_t w = 0; pm->arms && w < player->loadout_size; w++) {
        const PlayerArms *arms = pm->arms[w];
        if (!arms) continue;
        const PlayerArmMesh *sides[2] = {arms->left, arms->right};
        for (int k = 0; k < 2; k++) {
            if (!sides[k]) continue;
            mesh_set(sides[k]->extender, visible);
            mesh_set(sides[k]->upper, visible);
            mesh_set(sides[k]->joint, visible);
            color_cube_set(sides[k]->lower, visible);
        }
        mesh_set(arms->weapon_left, visible);
        mesh_set(arms->weapon_right, visible);
        mesh_set(arms->attachment, visible);
        mesh_set(arms->charm_mount, visible);
        mesh_set(arms->charm, visible);
        mesh_set(arms->wrist, visible);
        if (arms->wrist_model) gltf_instance_set_visible(arms->wrist_model, visible);
    }
    for (int i = 0; i < PLAYER_COSMETIC_SLOTS; i++) {
        mesh_set(pm->cosmetics[i], visible);
        if (pm->cosmetic_models[i]) gltf_instance_set_visible(pm->cosmetic_models[i], visible);
    }
}

// ---------------------------------------------------------------------------
// GAME.NETWORK.send (client, API:1350-1392, 2.5)

static int net_send(void *user, const int type, const void *payload, const uint16_t len) {
    Client *client = user;
    client_net_send(client, (NetPacketType) type, payload, len);
    return 1;
}

static KsValue n_send(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    Client *client = g_ksc.client;
    if (!client) return ks_bool(0);
    // Script messages exist only on servers that announced NET_PROTOCOL_MINOR_V7 (and the
    // offline sandbox): elsewhere nothing is sent.
    if (!client->offline && g_ksc.server_minor < NET_PROTOCOL_MINOR_V7) return ks_bool(0);
    char id[KS_NET_ID_MAX * 2];
    ksc_str(s, ksc_arg(a, n, 0), id, sizeof(id));
    // v7 returns undefined here; KN answers true when queued, false when rejected (7 #11)
    return ks_bool(ks_net_send_msg(s, &g_ksc.net_seq, id, ksc_arg(a, n, 1), net_send, client) == 1);
}

// ---------------------------------------------------------------------------
// GAME.DEFAULT.disable3D (API:1328-1349): RENDER.disable()

static KsValue d_disable_3d(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u; (void) a; (void) n;
    if (ksc_game()) ks_game_set_flag(ksc_game(), KS_FLAG_DISABLE_3D, 1);
    return ks_undef();
}

int ks_client_has_main_script(void) { return ksc_script() != NULL; }
int ks_client_3d_disabled(void) { return ksc_script() && ks_game_flag(ksc_game(), KS_FLAG_DISABLE_3D); }
int ks_client_no_prediction(void) { return ksc_script() && ks_game_flag(ksc_game(), KS_FLAG_NO_PREDICTION); }

// ---------------------------------------------------------------------------
// GAME.OVERLAY (3.1.10): the WP5 canvas (ks_overlay.h) with KsValue -> KsOvArg arguments

static KsValue overlay_call(KsScript *s, const KsOvMethod method, const KsValue *a, const int n) {
    enum { MAX_ARGS = 12 };
    KsOvArg args[MAX_ARGS];
    char strs[MAX_ARGS][256];
    const int argc = n < MAX_ARGS ? n : MAX_ARGS;
    for (int i = 0; i < argc; i++) {
        KsOvArg *o = &args[i];
        memset(o, 0, sizeof(*o));
        switch (a[i].tag) {
            case KS_UNDEF: o->type = KS_OV_UNDEF; break;
            case KS_NULL: o->type = KS_OV_NULL; break;
            case KS_BOOL: o->type = KS_OV_BOOL; o->boolean = a[i].as.b; break;
            case KS_NUM: o->type = KS_OV_NUM; o->num = a[i].as.num; break;
            case KS_STR: o->type = KS_OV_STR; o->str = ksc_str(s, a[i], strs[i], sizeof(strs[i])); break;
            default: o->type = KS_OV_OBJ; break;
        }
    }
    const KsOvResult r = ks_overlay_call(method, args, argc);
    switch (r.type) {
        case KS_OV_RET_NUM: return ks_num(r.num);
        case KS_OV_RET_SIZE: return ksc_xy(s, "width", r.width, "height", r.height);
        case KS_OV_RET_ERROR: ks_runtime_error(s, "IndexSizeError"); return ks_undef(); // becomes Internal Error "OVERLAY.<name>"
        default: return ks_undef();
    }
}

#define OV(api, method) \
    static KsValue ov_##api(KsScript *s, void *u, const KsValue *a, int n) { (void) u; return overlay_call(s, KS_OV_##method, a, n); }
OV(GETSIZE, GET_SIZE)
OV(OFFSET, OFFSET)
OV(SCALE, SCALE)
OV(CLEAR, CLEAR)
OV(MEASURETEXT, MEASURE_TEXT)
OV(DRAWCIRCLE, DRAW_CIRCLE)
OV(DRAWRECT, DRAW_RECT)
OV(DRAWLINE, DRAW_LINE)
OV(DRAWTEXT, DRAW_TEXT)
OV(DRAWIMAGE, DRAW_IMAGE)
OV(BEGINPATH, BEGIN_PATH)
OV(CLOSEPATH, CLOSE_PATH)
OV(SAVE, SAVE)
OV(RESTORE, RESTORE)
OV(TRANSLATE, TRANSLATE)
OV(TRANSFORM, TRANSFORM)
OV(SETTRANSFORM, SET_TRANSFORM)
OV(MOVETO, MOVE_TO)
OV(LINETO, LINE_TO)
OV(FILL, FILL)
OV(FILLSTYLE, FILL_STYLE)
OV(STROKE, STROKE)
OV(STROKETEXT, STROKE_TEXT)
OV(STROKERECT, STROKE_RECT)
OV(STROKESTYLE, STROKE_STYLE)
OV(LINEWIDTH, LINE_WIDTH)
OV(LINEDASHOFFSET, LINE_DASH_OFFSET)
OV(LINEJOIN, LINE_JOIN)
OV(ELLIPSE, ELLIPSE)
OV(ARC, ARC)
OV(ARCTO, ARC_TO)
OV(BEZIERCURVETO, BEZIER_CURVE_TO)
OV(QUADRATICCURVETO, QUADRATIC_CURVE_TO)
OV(GLOBALALPHA, GLOBAL_ALPHA)

static void overlay_api(KsApiImpl *api) {
    api[KS_API_OVERLAY_GETSIZE].fn = ov_GETSIZE;
    api[KS_API_OVERLAY_OFFSET].fn = ov_OFFSET;
    api[KS_API_OVERLAY_SCALE].fn = ov_SCALE;
    api[KS_API_OVERLAY_CLEAR].fn = ov_CLEAR;
    api[KS_API_OVERLAY_MEASURETEXT].fn = ov_MEASURETEXT;
    api[KS_API_OVERLAY_DRAWCIRCLE].fn = ov_DRAWCIRCLE;
    api[KS_API_OVERLAY_DRAWRECT].fn = ov_DRAWRECT;
    api[KS_API_OVERLAY_DRAWLINE].fn = ov_DRAWLINE;
    api[KS_API_OVERLAY_DRAWTEXT].fn = ov_DRAWTEXT;
    api[KS_API_OVERLAY_DRAWIMAGE].fn = ov_DRAWIMAGE;
    api[KS_API_OVERLAY_BEGINPATH].fn = ov_BEGINPATH;
    api[KS_API_OVERLAY_CLOSEPATH].fn = ov_CLOSEPATH;
    api[KS_API_OVERLAY_SAVE].fn = ov_SAVE;
    api[KS_API_OVERLAY_RESTORE].fn = ov_RESTORE;
    api[KS_API_OVERLAY_TRANSLATE].fn = ov_TRANSLATE;
    api[KS_API_OVERLAY_TRANSFORM].fn = ov_TRANSFORM;
    api[KS_API_OVERLAY_SETTRANSFORM].fn = ov_SETTRANSFORM;
    api[KS_API_OVERLAY_MOVETO].fn = ov_MOVETO;
    api[KS_API_OVERLAY_LINETO].fn = ov_LINETO;
    api[KS_API_OVERLAY_FILL].fn = ov_FILL;
    api[KS_API_OVERLAY_FILLSTYLE].fn = ov_FILLSTYLE;
    api[KS_API_OVERLAY_STROKE].fn = ov_STROKE;
    api[KS_API_OVERLAY_STROKETEXT].fn = ov_STROKETEXT;
    api[KS_API_OVERLAY_STROKERECT].fn = ov_STROKERECT;
    api[KS_API_OVERLAY_STROKESTYLE].fn = ov_STROKESTYLE;
    api[KS_API_OVERLAY_LINEWIDTH].fn = ov_LINEWIDTH;
    api[KS_API_OVERLAY_LINEDASHOFFSET].fn = ov_LINEDASHOFFSET;
    api[KS_API_OVERLAY_LINEJOIN].fn = ov_LINEJOIN;
    api[KS_API_OVERLAY_ELLIPSE].fn = ov_ELLIPSE;
    api[KS_API_OVERLAY_ARC].fn = ov_ARC;
    api[KS_API_OVERLAY_ARCTO].fn = ov_ARCTO;
    api[KS_API_OVERLAY_BEZIERCURVETO].fn = ov_BEZIERCURVETO;
    api[KS_API_OVERLAY_QUADRATICCURVETO].fn = ov_QUADRATICCURVETO;
    api[KS_API_OVERLAY_GLOBALALPHA].fn = ov_GLOBALALPHA;
}

// ---------------------------------------------------------------------------
// The host table

static void build_api(void) {
    memset(g_ksc.api, 0, sizeof(g_ksc.api));
    ks_game_api_fill(g_ksc.api, KS_SIDE_CLIENT);
    g_ksc.api[KS_API_PLAYERS_GETSELF].fn = p_get_self;
    g_ksc.api[KS_API_PLAYERS_TOGGLELOD].fn = p_toggle_lod;
    g_ksc.api[KS_API_PLAYERS_DISABLEMESHES].fn = p_disable_meshes;
    g_ksc.api[KS_API_NETWORK_SEND].fn = n_send;
    g_ksc.api[KS_API_DEFAULT_DISABLE3D].fn = d_disable_3d;
    overlay_api(g_ksc.api);
    ks_ui_api(g_ksc.api);
    ks_scene_api(g_ksc.api);
    ks_sound_api(g_ksc.api);
    ks_input_api(g_ksc.api);
    ks_misc_api(g_ksc.api);
    g_ksc.host = (KsHost) {.side = KS_SIDE_CLIENT, .log = host_log, .now_ms = host_now, .api = g_ksc.api};
}

void ks_client_init(Client *client) {
    g_ksc.client = client;
    build_api();
    build_player_class();
}

// ---------------------------------------------------------------------------
// Lifecycle

static void render_hook(void *user, const double delta_ms) {
    (void) user;
    const KsValue arg = ks_num(delta_ms);
    ksc_call("render", &arg, 1);
}

// Everything the instance created goes; the next one starts clean.
static void unload_instance(Client *client) {
    ks_overlay_set_render_hook(NULL, NULL);
    if (client) ks_game_unload(&client->game);
    ks_ui_unload();
    ks_scene_unload();
    ks_sound_unload();
    ks_input_unload(client);
    ks_misc_unload();
    if (client) {
        for (size_t i = 0; i < client->game.player_count; i++) {
            const Player *p = client->game.players[i];
            if (p && (g_disable_meshes || player_hidden(p->uid))) player_model_set_visible(p, 1);
        }
    }
    g_disable_meshes = 0;
    g_use_lod = 1;
    g_hidden_count = 0;
    ks_net_reassembler_free(&g_ksc.reasm);
    memset(&g_ksc.reasm, 0, sizeof(g_ksc.reasm));
    g_ksc.map = NULL;
    g_ksc.loaded = 0;
}

void ks_client_unload(Client *client) {
    if (!g_ksc.loaded) return;
    unload_instance(client);
}

void ks_client_round(Client *client, const int server_minor) {
    g_ksc.server_minor = server_minor;
    if (!g_ksc.client) ks_client_init(client);
    unload_instance(client);
    ks_ui_round_reset();
    ks_input_round_reset(client);
    if (!client->game.ready || !client->game.map) return;

    size_t len = 0, shared_len = 0;
    char *src = ks_game_map_source(&client->game, KS_SIDE_CLIENT, &len);
    // v10.0.3 prepareGame (deob-10.0.3.js:220186-220193): scripts.shared on the client too, after the main script;
    // the entity manager steps the replicated entities with it (ks_entities.h, client/src/entities.c)
    char *shared = ks_game_map_source(&client->game, KS_SIDE_SHARED, &shared_len);
    if (!src && !shared) return;

    g_ksc.loaded = 1;
    g_ksc.map = client->game.map;
    // GAME.OVERLAY.getSize() is valid in global initialisers (Snake Demo's `obj bounds`).
    ks_overlay_client_init();
    ks_game_set_player_class(&client->game, g_ksc.player_class.props ? &g_ksc.player_class : NULL);
    const int rc = src ? ks_game_load(&client->game, KS_SIDE_CLIENT, src, len, &g_ksc.host, &g_ksc) : 0;
    free(src);
    if (shared) ks_game_load_shared(&client->game, KS_SIDE_CLIENT, shared, shared_len, &g_ksc.host, &g_ksc);
    free(shared);
    if (rc != 0 || !ksc_script()) return;
    // UTILS.rand* on the client: a random stream (6.4; Math.random in v7)
    ks_game_seed(&client->game, (uint64_t) host_now(NULL) * 2654435761u ^ (uint64_t) (uintptr_t) client);

    ks_overlay_set_render_hook(render_hook, NULL);
    ks_game_start(&client->game); // Game.init's end: mainScript.start()
}

// ---------------------------------------------------------------------------
// Per frame

void ks_client_frame(Client *client, const float world_delta_seconds, const float frame_delta_seconds) {
    g_ksc.frame++;
    // v10 game.update's entity half (advance, the client events into this script, updateVisuals), before update()
    entities_update(client, frame_delta_seconds);
    if (!g_ksc.loaded) return;
    // The game the script belongs to went away (left the server, a new map without INIT yet).
    if (!client->game.ready || client->game.map != g_ksc.map || (!client->offline && !client->net_connected)) {
        unload_instance(client);
        return;
    }
    (void) frame_delta_seconds;
    if (ksc_script()) {
        // update(gameTimeDiff * deltaMlt): the round-end slow motion scales the world delta,
        // TIME.fixedDelta() returns it before deltaMlt (2.4). KN has no map deltaMlt yet: 1.
        double delta_ms = (double) world_delta_seconds * 1000.0;
        if (delta_ms > 33.0) delta_ms = 33.0; // gameTimeDiff = min(frame dt, config.dltMx = 33 ms)
        g_ksc.last_update_delta = delta_ms;
        ks_game_update(&client->game, delta_ms, 1.0);
    }
    if (!g_ksc.loaded) return; // the script left the game (changeGame) inside update()
    ks_input_frame(client);
    ks_ui_frame();
    ks_scene_frame(client, (float) (frame_delta_seconds * 1000.0f));

    for (size_t i = 0; i < client->game.player_count; i++) {
        const Player *p = client->game.players[i];
        if (p && (g_disable_meshes || player_hidden(p->uid))) player_model_set_visible(p, 0);
    }
}

void ks_client_ui_late(Client *client) {
    (void) client;
    ks_ui_late();
}

void ks_client_camera(Client *client) {
    if (!g_ksc.loaded) return;
    ks_input_apply_camera(client);
}

// ---------------------------------------------------------------------------
// Events

void ks_client_player_spawned(Client *client, const Player *player) {
    (void) client;
    if (!player || !ksc_has_hook("onPlayerSpawn")) return;
    KsScript *s = ksc_script();
    char id[16];
    ks_player_id(player, id, sizeof(id));
    const KsValue arg = ks_str(s, id, strlen(id));
    ksc_call("onPlayerSpawn", &arg, 1);
}

void ks_client_player_killed(Client *client, const int victim_uid, const int killer_uid) {
    (void) client;
    if (!ksc_has_hook("onPlayerDeath")) return;
    KsScript *s = ksc_script();
    char victim[16], killer[16] = "";
    snprintf(victim, sizeof(victim), "%d", victim_uid);
    if (killer_uid >= 0) snprintf(killer, sizeof(killer), "%d", killer_uid);
    const KsValue args[2] = {ks_str(s, victim, strlen(victim)), ks_str(s, killer, strlen(killer))};
    ksc_call("onPlayerDeath", args, 2);
}

void ks_client_div_clicked(const char *id) { ks_ui_click(id); }

int ks_client_handle_packet(Client *client, const int type, const void *payload, const uint16_t length) {
    (void) client;
    if (type == NET_PACKET_SCRIPT_LOG) {
        // the server script's GAME.log / errors ("ksL", IDX:17769-17771)
        int is_error = 0;
        char text[KS_NET_LOG_TEXT_MAX + 1];
        if (ks_net_decode_log(payload, length, &is_error, text, sizeof(text))) ksc_console(is_error, text);
        return 1;
    }
    if (type == NET_PACKET_SCRIPT_MSG) {
        const uint8_t *body = NULL;
        size_t body_len = 0;
        if (ks_net_reassemble(&g_ksc.reasm, payload, length, &body, &body_len) != 1) return 1;
        KsScript *s = ksc_script();
        if (!s || !ksc_has_hook("onNetworkMessage")) return 1;
        char id[KS_NET_ID_MAX + 1];
        KsValue data = ks_undef();
        if (!ks_net_decode_msg(s, body, body_len, id, sizeof(id), &data)) return 1;
        const KsValue args[2] = {ks_str(s, id, strlen(id)), data};
        ksc_call("onNetworkMessage", args, 2);
        return 1;
    }
    if (type == NET_PACKET_SCRIPT_SYNC) {
        KsScript *s = ksc_script();
        Client *c = g_ksc.client;
        if (!s || !c || !c->me) return 1;
        ks_net_apply_sync(s, ks_game_wrap_player(s, &c->game, c->me), payload, length);
        return 1;
    }
    return 0;
}
