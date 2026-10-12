// Editor bus link: the map editor's end of the editor bus (transport and wire format:
// shared/include/editor_bus.h). It connects editor processes (each one a bus "instance"
// with its projects) and tools such as kn_editor_mcp, which query, edit, render and save
// the projects of the live editors through it.
//
// Roles. The first editor started with the bus on (`--mcp-bus` / `--editor-mcp`, or File >
// Projects > Editor Bus) hosts it: it holds <dir>/editor-bus.lock, listens on the socket and
// writes a fresh token. Later editors join as role "editor" peers and announce their
// projects; tools join as role "mcp"/"tool". When the host goes away the other editors race
// for the lock: the winner hosts (new token), the rest and the tools reconnect.
//
// Instances and projects. The host is instance 1; peers get ids from 2 in join order.
// Projects are "<instance>.<local id>" ("1.2"); requests may also name a project by path,
// file basename ("burg", "burg.json") or map name, case-insensitive; no project = the
// host's shown project.
//
// Permissions. A tool must send permission_request {scopes} before anything else; the host
// editor shows a dialog (Allow once / Always allow this client / Deny) and answers with
// permission_result {granted, decision, reason} once the user decides (requests sent in the
// meantime wait). "Always" grants are stored per client_id in the editor settings
// (kro_editor "mcpGrants"; File > Projects > MCP Access... lists and revokes them).
// --mcp-auto-accept[=read,edit,project] (env KRUNKNATIVE_MCP_AUTO_ACCEPT=1|scopes) accepts
// without asking (decision "auto", logged), --mcp-auto-deny (KRUNKNATIVE_MCP_AUTO_DENY=1)
// denies; headless editors without either deny (nobody could answer the dialog). Editor
// peers have every scope. Scopes: read (list, info, query, get, render, export, history,
// log, subscribe, copy), edit (add, update, remove, batch, select, undo, redo, save,
// paste), project (open, close, focus, new). A request outside the grant fails with
// error code "permission_denied".
//
// Host ops ({"t":"req","id","op","args"}):
//   list                         {instances:[{id,pid,host,projects:[{id,name,path,map,dirty,active,objects,undo,redo,revision}]}]}
//   open {path, instance?, activate?}  new {name?, instance?}  close {project, force?}  focus {project}
//   subscribe {events:["selection","change","projects","clipboard"]}
//   copy {project, uuids?}       -> bus clipboard (also every editor's Ctrl+V clipboard) {count}
//   paste {project, at?|offset?} -> {uuids, undo_steps:1}
//   call {project, tool, args}   -> the tool's result, run by the editor that has the project
// Call tools (on one project; each edit is one undo step):
//   info, query {prefab?,texture?,uuid?,near?:[x,y,z,r],limit?,offset?,fields?}, get {uuid},
//   export {full?} -> {map,revision,undo,redo}, history {limit?}, log {tail?},
//   add {objects,colors?,label?}, update {uuid,set?:{raw keys},props?:{editor props}},
//   remove {uuids}, batch {label,add,replace:[{uuid,data}],remove,map_set,map_unset,colors?},
//   select {uuids}, undo/redo {steps?}, save {path?,as?,format?:"compact"|"pretty"},
//   render {mode:"view"|"multiview", width?, height?, camera?:{pos,yaw,pitch,fov(deg),ortho?},
//           no_fog?, hide_helpers?, focus?:[uuids], split?}
//   copy_objects {uuids?} / paste_objects {objects, at?|offset?, label?} (the clipboard ops' halves)
// Events ({"t":"ev","ev",...}): projects {instances}, change {project,revision,objects,dirty},
// selection {project,uuid}, clipboard {count}.
#include "editor_internal.h"
#include "editor_model.h"
#include "editor_script_lang.h"
#include <app_mode.h>
#include <editor_bus.h>
#include <headless.h>
#include <math.h>
#include <png_write.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#define getpid _getpid
#endif

#define BUS_MAX_PEERS 32
#define BUS_MAX_FWD 64
#define BUS_LOG_LINES 256
#define BUS_EV_SELECTION 1
#define BUS_EV_CHANGE 2
#define BUS_EV_PROJECTS 4
#define BUS_EV_CLIPBOARD 8
#define BUS_VERSION "1"

enum { BUS_OFF, BUS_HOST, BUS_CLIENT };
enum { ROLE_NONE, ROLE_EDITOR, ROLE_TOOL };
enum { PERM_NONE, PERM_PENDING, PERM_DECIDED };

typedef struct {
    EdBusConn conn;
    int id;
    int role;
    char client_id[128], name[128], version[64];
    int pid;
    unsigned granted;
    int perm_state;
    unsigned perm_wanted;
    cJSON *perm_id;        // the permission_request's id
    cJSON *queued;         // requests waiting for the permission decision
    unsigned events;       // subscribed BUS_EV_*
    cJSON *projects;       // editors: their announced project list
    char decision[16];
    char reason[160];
} Peer;

typedef struct {
    int used;
    int id;                // id sent to the owner
    int to_peer;
    int from_peer;         // origin peer id
    cJSON *orig_id;
    int kind;              // FWD_*
} Fwd;
enum { FWD_RELAY, FWD_COPY };

static struct {
    // command line / policy
    int want;              // --mcp-bus / --editor-mcp / menu
    char dir[512];         // --bus-dir
    int auto_accept;       // --mcp-auto-accept[=scopes]
    unsigned auto_scopes;
    int auto_deny;
    // state
    int mode;
    EdBusPaths paths;
    double retry_at;
    int self_id;
    // host
    int lock_fd, listen_fd;
    char token[EDBUS_TOKEN_LEN + 1];
    Peer *peers[BUS_MAX_PEERS];
    int peer_count, next_peer;
    Fwd fwd[BUS_MAX_FWD];
    int next_fwd;
    cJSON *clip;           // the bus clipboard (absolute serialized objects)
    // client
    EdBusConn up;
    int projects_dirty;
    // log
    char *log[BUS_LOG_LINES];
    int log_head, log_count;
} B = {.lock_fd = -1, .listen_fd = -1};

static double now_s(void) { return glfwGetTime(); }

// ---------------------------------------------------------------------------
// console ring (the "log" tool) and messages

void ed_log_line(const char *line) {
    if (!line) return;
    if (B.log_count == BUS_LOG_LINES) {
        free(B.log[B.log_head]);
        B.log_head = (B.log_head + 1) % BUS_LOG_LINES;
        B.log_count--;
    }
    B.log[(B.log_head + B.log_count) % BUS_LOG_LINES] = strdup(line);
    B.log_count++;
}

static void bus_log(const char *fmt, ...) {
    char text[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[bus] %s\n", text);
    char line[720];
    snprintf(line, sizeof(line), "[bus] %s", text);
    ed_log_line(line);
}

// ---------------------------------------------------------------------------
// command line

int editor_bus_parse_arg(const int argc, char **argv, int *i) {
    const char *a = argv[*i];
    if (!strcmp(a, "--mcp-bus") || !strcmp(a, "--editor-mcp") || !strcmp(a, "--editor-bus")) {
        B.want = 1;
        return 1;
    }
    if (!strcmp(a, "--bus-dir") && *i + 1 < argc) {
        snprintf(B.dir, sizeof(B.dir), "%s", argv[++*i]);
        return 1;
    }
    if (!strncmp(a, "--bus-dir=", 10)) {
        snprintf(B.dir, sizeof(B.dir), "%s", a + 10);
        return 1;
    }
    if (!strcmp(a, "--mcp-auto-accept")) {
        B.auto_accept = 1;
        B.auto_scopes = EDBUS_SCOPE_ALL;
        return 1;
    }
    if (!strncmp(a, "--mcp-auto-accept=", 18)) {
        B.auto_accept = 1;
        B.auto_scopes = edbus_scope_parse(a + 18);
        return 1;
    }
    if (!strcmp(a, "--mcp-auto-deny")) {
        B.auto_deny = 1;
        return 1;
    }
    return 0;
}

void editor_usage(FILE *out) {
    fprintf(out, "Editor (--editor):\n"
                 "  --editor [query] [map.json ...]   open each map as a project (tabs; Ctrl+Tab switches)\n"
                 "  --mcp-bus | --editor-mcp          start or join the editor bus (tools like kn_editor_mcp attach)\n"
                 "  --bus-dir <dir>                   bus directory (default $KRUNKNATIVE_EDITOR_BUS_DIR,\n"
                 "                                    $XDG_RUNTIME_DIR/krunknative, /tmp/krunknative-<uid>)\n"
                 "  --mcp-auto-accept[=read,edit,project]  grant bus clients without the dialog (tests; logged)\n"
                 "  --mcp-auto-deny                   refuse every bus client (tests)\n"
                 "  env KRUNKNATIVE_MCP_AUTO_ACCEPT=1|<scopes>, KRUNKNATIVE_MCP_AUTO_DENY=1,\n"
                 "      KRUNKNATIVE_EDITOR_BUS_TCP=1 (localhost TCP instead of the unix socket)\n"
                 "  Headless editors deny bus clients unless --mcp-auto-accept is given.\n");
}

static void read_env_policy(void) {
    const char *acc = getenv("KRUNKNATIVE_MCP_AUTO_ACCEPT");
    if (acc && *acc && strcmp(acc, "0") != 0 && !B.auto_accept) {
        B.auto_accept = 1;
        B.auto_scopes = edbus_scope_parse(acc);
        if (!B.auto_scopes) B.auto_scopes = EDBUS_SCOPE_ALL;
    }
    const char *deny = getenv("KRUNKNATIVE_MCP_AUTO_DENY");
    if (deny && *deny && strcmp(deny, "0") != 0) B.auto_deny = 1;
}

// ---------------------------------------------------------------------------
// projects as the bus sees them

static void project_ref(const EditorProject *p, char *out, const size_t n) { snprintf(out, n, "%d.%d", B.self_id ? B.self_id : 1, p->id); }

static cJSON *project_json(EditorProject *p) {
    char id[32];
    project_ref(p, id, sizeof(id));
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "id", id);
    cJSON_AddNumberToObject(j, "local", p->id);
    cJSON_AddStringToObject(j, "name", ed_project_title(p));
    cJSON_AddStringToObject(j, "path", p->path);
    cJSON_AddStringToObject(j, "map", ed_project_map_name(p));
    cJSON_AddBoolToObject(j, "dirty", p->dirty);
    cJSON_AddBoolToObject(j, "active", p == g_ed.shown);
    cJSON_AddNumberToObject(j, "objects", (double) p->count);
    EditorProject *prev;
    ed_project_enter(p, &prev);
    cJSON_AddNumberToObject(j, "undo", (double) ed_history_steps(0));
    cJSON_AddNumberToObject(j, "redo", (double) ed_history_steps(1));
    ed_project_leave(prev);
    cJSON_AddNumberToObject(j, "revision", p->revision);
    return j;
}

static cJSON *local_projects(void) {
    cJSON *a = cJSON_CreateArray();
    for (size_t i = 0; i < g_ed.project_count; i++) cJSON_AddItemToArray(a, project_json(g_ed.projects[i]));
    return a;
}

static Peer *peer_by_id(const int id) {
    for (int i = 0; i < B.peer_count; i++) if (B.peers[i]->id == id) return B.peers[i];
    return NULL;
}

static cJSON *instances_json(void) {
    cJSON *list = cJSON_CreateArray();
    cJSON *self = cJSON_CreateObject();
    cJSON_AddNumberToObject(self, "id", B.self_id);
    cJSON_AddNumberToObject(self, "pid", (double) getpid());
    cJSON_AddBoolToObject(self, "host", 1);
    cJSON_AddItemToObject(self, "projects", local_projects());
    cJSON_AddItemToArray(list, self);
    for (int i = 0; i < B.peer_count; i++) {
        const Peer *pe = B.peers[i];
        if (pe->role != ROLE_EDITOR) continue;
        cJSON *inst = cJSON_CreateObject();
        cJSON_AddNumberToObject(inst, "id", pe->id);
        cJSON_AddNumberToObject(inst, "pid", pe->pid);
        cJSON_AddBoolToObject(inst, "host", 0);
        cJSON_AddItemToObject(inst, "projects", pe->projects ? cJSON_Duplicate(pe->projects, 1) : cJSON_CreateArray());
        cJSON_AddItemToArray(list, inst);
    }
    return list;
}

static int ci_equal(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        const char x = (char) (*a >= 'A' && *a <= 'Z' ? *a + 32 : *a), y = (char) (*b >= 'A' && *b <= 'Z' ? *b + 32 : *b);
        if (x != y) return 0;
    }
    return !*a && !*b;
}

static void strip_ref(const char *ref, char *base, const size_t n) {
    const char *b = ref;
    for (const char *c = ref; *c; c++) if (*c == '/' || *c == '\\') b = c + 1;
    snprintf(base, n, "%s", b);
    const size_t l = strlen(base);
    if (l > 5 && !strcmp(base + l - 5, ".json")) base[l - 5] = 0;
}

// host: which instance has the project `ref`; *local = its local id. 0 = not found.
static int resolve(const char *ref, int *local) {
    *local = 0;
    if (!ref || !*ref) {
        if (!g_ed.shown) return 0;
        *local = g_ed.shown->id;
        return B.self_id;
    }
    int inst = 0, loc = 0;
    char tail;
    if (sscanf(ref, "%d.%d%c", &inst, &loc, &tail) == 2) {
        if (inst == B.self_id) {
            if (!ed_project_find(loc)) return 0;
            *local = loc;
            return inst;
        }
        const Peer *pe = peer_by_id(inst);
        if (!pe || pe->role != ROLE_EDITOR) return 0;
        const cJSON *pj;
        cJSON_ArrayForEach(pj, pe->projects) {
            if ((int) ed_json_num(pj, "local", 0) == loc) {
                *local = loc;
                return inst;
            }
        }
        return 0;
    }
    EditorProject *p = ed_project_resolve(ref);
    if (p) {
        *local = p->id;
        return B.self_id;
    }
    char abs[1024] = "", base[256];
    if (!ed_abs_path(ref, abs, sizeof(abs))) abs[0] = 0;
    strip_ref(ref, base, sizeof(base));
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < B.peer_count; i++) {
            const Peer *pe = B.peers[i];
            if (pe->role != ROLE_EDITOR) continue;
            const cJSON *pj;
            cJSON_ArrayForEach(pj, pe->projects) {
                const char *path = ed_json_str(pj, "path", ""), *name = ed_json_str(pj, "name", ""), *map = ed_json_str(pj, "map", "");
                const int hit = pass == 0 ? path[0] && (!strcmp(path, ref) || (abs[0] && !strcmp(path, abs)))
                              : pass == 1 ? name[0] && ci_equal(name, base)
                                          : map[0] && ci_equal(map, ref);
                if (hit) {
                    *local = (int) ed_json_num(pj, "local", 0);
                    return pe->id;
                }
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// object helpers for the tools

static cJSON *vec_arr(const vec3 v) {
    cJSON *a = cJSON_CreateArray();
    cJSON_AddItemToArray(a, cJSON_CreateNumber(ed_round(v.x, 3)));
    cJSON_AddItemToArray(a, cJSON_CreateNumber(ed_round(v.y, 3)));
    cJSON_AddItemToArray(a, cJSON_CreateNumber(ed_round(v.z, 3)));
    return a;
}

static int object_index(const EditorObject *o) {
    for (size_t i = 0; i < g_ed.proj->count; i++) if (g_ed.proj->objects[i] == o) return (int) i;
    return -1;
}

static cJSON *object_json(const EditorObject *o, const int full) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "uuid", o->uuid);
    cJSON_AddNumberToObject(j, "id", o->uuid);
    cJSON_AddNumberToObject(j, "index", object_index(o));
    cJSON_AddNumberToObject(j, "prefab", o->prefab);
    cJSON_AddStringToObject(j, "prefab_name", o->prefab >= 0 && o->prefab < EDITOR_PREFAB_COUNT ? editor_prefabs[o->prefab].name : "?");
    cJSON_AddItemToObject(j, "p", vec_arr(ed_obj_vec(o, "p", (vec3) {0})));
    cJSON_AddItemToObject(j, "s", vec_arr(ed_obj_vec(o, "s", (vec3) {1, 1, 1})));
    cJSON_AddItemToObject(j, "r", vec_arr(ed_obj_vec(o, "r", (vec3) {0})));
    if (full) cJSON_AddItemToObject(j, "data", ed_obj_serialize(o));
    return j;
}

static int prefab_matches(const EditorObject *o, const cJSON *want) {
    if (!want) return 1;
    if (cJSON_IsNumber(want)) return o->prefab == (int) cJSON_GetNumberValue(want);
    if (!cJSON_IsString(want)) return 1;
    const EditorPrefab *p = &editor_prefabs[o->prefab];
    char spaced[64];
    snprintf(spaced, sizeof(spaced), "%s", want->valuestring);
    for (char *c = spaced; *c; c++) if (*c == ' ') *c = '_';
    return ci_equal(p->name, want->valuestring) || ci_equal(p->label, want->valuestring) || ci_equal(p->name, spaced);
}

static uint32_t json_uuid(const cJSON *v) { return cJSON_IsNumber(v) ? (uint32_t) cJSON_GetNumberValue(v) : cJSON_IsString(v) ? (uint32_t) strtoul(v->valuestring, NULL, 10) : 0; }

// inline palette colours (ci/ei -> c/e) of map-format objects
static void inline_colors(cJSON *d, const cJSON *colors) {
    static const char *keys[2] = {"c", "e"};
    for (int k = 0; k < 2; k++) {
        char ikey[4];
        snprintf(ikey, sizeof(ikey), "%si", keys[k]);
        const cJSON *idx = cJSON_GetObjectItemCaseSensitive(d, ikey);
        if (!idx) continue;
        const cJSON *col = cJSON_IsNumber(idx) ? cJSON_GetArrayItem(colors, (int) cJSON_GetNumberValue(idx)) : NULL;
        if (cJSON_IsString(col)) {
            cJSON_DeleteItemFromObjectCaseSensitive(d, keys[k]);
            cJSON_AddStringToObject(d, keys[k], col->valuestring);
        }
        cJSON_DeleteItemFromObjectCaseSensitive(d, ikey);
    }
}

static EditorObject *add_object(const cJSON *data, const cJSON *colors) {
    cJSON *d = cJSON_Duplicate(data, 1);
    cJSON_DeleteItemFromObjectCaseSensitive(d, "objUUID");
    inline_colors(d, colors);
    EditorObject *o = ed_obj_new(d, 0);
    cJSON_Delete(d);
    if (o) ed_obj_add(o, 1, 1);
    return o;
}

static void replace_object(EditorObject *o, const cJSON *data, const cJSON *colors) {
    cJSON *d = cJSON_Duplicate(data, 1);
    cJSON_DeleteItemFromObjectCaseSensitive(d, "objUUID");
    inline_colors(d, colors);
    EditorObject *norm = ed_obj_new(d, 0); // the constructor's normalisation (prefab key, defaults)
    cJSON_Delete(d);
    if (!norm) return;
    cJSON *before = cJSON_Duplicate(o->data, 1);
    EditorProject *p = g_ed.proj;
    p->obj_count[ed_obj_count_class(o)]--;
    cJSON_Delete(o->data);
    o->data = norm->data;
    o->prefab = norm->prefab;
    norm->data = NULL;
    ed_obj_free(norm);
    p->obj_count[ed_obj_count_class(o)]++;
    ed_obj_rebuild(o);
    ed_history_add('u', o, before);
    cJSON_Delete(o->history);
    o->history = cJSON_Duplicate(o->data, 1);
    g_ed.dirty_counts = 1;
    if (o == p->selected) g_ed.dirty_panel = 1;
    ed_mark_changed();
}

static int map_key_known(const char *k) {
    return ed_model_map_key_known(k); // a mapConfig default (editor_model.c)
}

// terrain* / sizeMlt -> regenTerrain, zone* -> regenZone (which also regens the ocean),
// oc* -> regenOcean (the ocean preview plane, editor_terrain.c)
static void map_changed(const int terrain, const int zone, const int ocean) {
    editor_apply_lighting();
    if (terrain) ed_terrain_regen();
    if (zone) ed_zone_regen();
    else if (ocean) ed_ocean_regen();
    ed_panel_refresh_all();
    ed_mark_changed();
}

// ---------------------------------------------------------------------------
// render tool

static int cmp_float(const void *a, const void *b) {
    const float x = *(const float *) a, y = *(const float *) b;
    return x < y ? -1 : x > y;
}

// world bounds of the focus objects (exact), else of the map without its outliers: far-away
// helpers (death planes, sky boxes, distant props) would shrink the map to a few pixels, so
// the box spans the 2nd..98th percentile of the objects' extents
static void bounds_of(EditorProject *p, const cJSON *focus, vec3 *mn, vec3 *mx) {
    const int focused = cJSON_GetArraySize(focus) > 0;
    const size_t n = p->count;
    float *lo[3], *hi[3];
    for (int k = 0; k < 3; k++) {
        lo[k] = malloc((n ? n : 1) * sizeof(float));
        hi[k] = malloc((n ? n : 1) * sizeof(float));
    }
    size_t m = 0;
    for (size_t i = 0; i < n && lo[2] && hi[2] && lo[0] && hi[0] && lo[1] && hi[1]; i++) {
        const EditorObject *o = p->objects[i];
        if (focused) {
            int hit = 0;
            const cJSON *u;
            cJSON_ArrayForEach(u, focus) hit |= json_uuid(u) == o->uuid;
            if (!hit) continue;
        }
        float w[16];
        ed_obj_world_box(o, w);
        vec3 a = {0}, b = {0};
        for (int c = 0; c < 8; c++) {
            const float x = c & 1 ? 0.5f : -0.5f, y = c & 2 ? 0.5f : -0.5f, z = c & 4 ? 0.5f : -0.5f;
            const vec3 q = {w[0] * x + w[1] * y + w[2] * z + w[3], w[4] * x + w[5] * y + w[6] * z + w[7], w[8] * x + w[9] * y + w[10] * z + w[11]};
            if (!c) a = b = q;
            a.x = fminf(a.x, q.x), a.y = fminf(a.y, q.y), a.z = fminf(a.z, q.z);
            b.x = fmaxf(b.x, q.x), b.y = fmaxf(b.y, q.y), b.z = fmaxf(b.z, q.z);
        }
        lo[0][m] = a.x, lo[1][m] = a.y, lo[2][m] = a.z;
        hi[0][m] = b.x, hi[1][m] = b.y, hi[2][m] = b.z;
        m++;
    }
    if (!m) {
        *mn = (vec3) {-50, -50, -50};
        *mx = (vec3) {50, 50, 50};
    } else {
        float rmin[3], rmax[3];
        const size_t cut = focused || m < 20 ? 0 : m / 50;
        for (int k = 0; k < 3; k++) {
            qsort(lo[k], m, sizeof(float), cmp_float);
            qsort(hi[k], m, sizeof(float), cmp_float);
            rmin[k] = lo[k][cut];
            rmax[k] = hi[k][m - 1 - cut];
            if (rmax[k] < rmin[k]) rmax[k] = rmin[k];
        }
        *mn = (vec3) {rmin[0], rmin[1], rmin[2]};
        *mx = (vec3) {rmax[0], rmax[1], rmax[2]};
    }
    for (int k = 0; k < 3; k++) {
        free(lo[k]);
        free(hi[k]);
    }
}

static char *png_b64(const unsigned char *rgba, const int w, const int h) {
    size_t n = 0;
    unsigned char *png = png_encode_rgb(rgba, w, h, &n);
    if (!png) return NULL;
    char *b = edbus_base64(png, n);
    free(png);
    return b;
}

static cJSON *tool_render(EditorProject *p, const cJSON *args, const char **err) {
    const char *mode = ed_json_str(args, "mode", cJSON_GetObjectItemCaseSensitive(args, "camera") ? "view" : "multiview");
    int w = (int) ed_json_num(args, "width", 1024), h = (int) ed_json_num(args, "height", 768);
    if (w < 16 || h < 16 || w > 4096 || h > 4096) {
        *err = "width/height must be 16..4096";
        return NULL;
    }
    const int multiview = !strcmp(mode, "multiview");
    const int no_fog = cJSON_GetObjectItemCaseSensitive(args, "no_fog") ? ed_json_truthy(cJSON_GetObjectItemCaseSensitive(args, "no_fog")) : multiview;
    const int flags = (no_fog ? ED_RENDER_NO_FOG : 0) | (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(args, "hide_helpers")) ? ED_RENDER_NO_HELPERS : 0);
    cJSON *res = cJSON_CreateObject();
    if (!multiview) {
        Camera cam = p->camera;
        float yaw = p->yaw, pitch = p->pitch, ortho = 0;
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(args, "camera");
        if (c) {
            const cJSON *pos = cJSON_GetObjectItemCaseSensitive(c, "pos");
            if (cJSON_GetArraySize(pos) >= 3)
                cam.position = (vec3) {(float) cJSON_GetArrayItem(pos, 0)->valuedouble, (float) cJSON_GetArrayItem(pos, 1)->valuedouble, (float) cJSON_GetArrayItem(pos, 2)->valuedouble};
            yaw = (float) ed_json_num(c, "yaw", yaw);
            pitch = (float) ed_json_num(c, "pitch", pitch);
            cam.fov = (float) (ed_json_num(c, "fov", 70) * M_PI / 180.0);
            ortho = (float) ed_json_num(c, "ortho", 0);
        }
        cam.far = fmaxf(cam.far, 10000.0f);
        unsigned char *px = ed_render_project(p, &cam, yaw, pitch, ortho, w, h, flags | (ortho > 0 ? ED_RENDER_ORTHO : 0));
        char *b = px ? png_b64(px, w, h) : NULL;
        free(px);
        if (!b) {
            cJSON_Delete(res);
            *err = "render failed";
            return NULL;
        }
        cJSON_AddStringToObject(res, "png_base64", b);
        free(b);
        cJSON_AddNumberToObject(res, "width", w);
        cJSON_AddNumberToObject(res, "height", h);
        cJSON *views = cJSON_AddArrayToObject(res, "views");
        cJSON_AddItemToArray(views, cJSON_CreateString("view"));
        return res;
    }
    // multiview: iso (perspective), front / top / right (orthographic) of the map or the focus
    vec3 mn, mx;
    bounds_of(p, cJSON_GetObjectItemCaseSensitive(args, "focus"), &mn, &mx);
    const vec3 c = {(mn.x + mx.x) / 2, (mn.y + mx.y) / 2, (mn.z + mx.z) / 2};
    const vec3 e = {fmaxf((mx.x - mn.x) / 2, 1), fmaxf((mx.y - mn.y) / 2, 1), fmaxf((mx.z - mn.z) / 2, 1)};
    const float radius = sqrtf(e.x * e.x + e.y * e.y + e.z * e.z);
    const int tw = w / 2, th = h / 2;
    const float aspect = (float) tw / (float) th;
    static const char *const names[4] = {"iso", "front", "top", "right"};
    const int split = ed_json_truthy(cJSON_GetObjectItemCaseSensitive(args, "split"));
    unsigned char *grid = split ? NULL : calloc((size_t) w * h * 4, 1);
    cJSON *images = split ? cJSON_AddArrayToObject(res, "images") : NULL;
    for (int v = 0; v < 4; v++) {
        Camera cam = p->camera;
        cam.zoom = 1;
        float yaw = 0, pitch = 0, ortho = 0;
        if (v == 0) {
            const float fov = 50.0f * (float) M_PI / 180.0f;
            const float dist = radius / sinf(fov / 2) * 1.05f;
            const float k = 1.0f / sqrtf(1 + 0.64f + 1);
            cam.fov = fov;
            cam.position = (vec3) {c.x + dist * k, c.y + dist * 0.8f * k, c.z + dist * k};
            yaw = (float) M_PI / 4;
            pitch = -atan2f(0.8f, sqrtf(2.0f));
            cam.near = fmaxf(1.0f, dist - radius * 1.5f);
            cam.far = dist + radius * 2 + 10;
        } else {
            const float pad = 100.0f;
            if (v == 1) { // front: along -Z
                cam.position = (vec3) {c.x, c.y, mx.z + pad};
                ortho = fmaxf(e.y, e.x / aspect) * 1.05f;
                cam.far = (mx.z - mn.z) + pad * 2;
            } else if (v == 2) { // top: down -Y, screen up = -Z
                cam.position = (vec3) {c.x, mx.y + pad, c.z};
                pitch = -(float) M_PI / 2;
                ortho = fmaxf(e.z, e.x / aspect) * 1.05f;
                cam.far = (mx.y - mn.y) + pad * 2;
            } else { // right: along -X
                cam.position = (vec3) {mx.x + pad, c.y, c.z};
                yaw = (float) M_PI / 2;
                ortho = fmaxf(e.y, e.z / aspect) * 1.05f;
                cam.far = (mx.x - mn.x) + pad * 2;
            }
            cam.near = 1.0f;
        }
        unsigned char *px = ed_render_project(p, &cam, yaw, pitch, ortho, tw, th, flags | (ortho > 0 ? ED_RENDER_ORTHO : 0));
        if (!px) continue;
        if (split) {
            char *b = png_b64(px, tw, th);
            cJSON *img = cJSON_CreateObject();
            cJSON_AddStringToObject(img, "view", names[v]);
            cJSON_AddStringToObject(img, "png_base64", b ? b : "");
            cJSON_AddNumberToObject(img, "width", tw);
            cJSON_AddNumberToObject(img, "height", th);
            cJSON_AddItemToArray(images, img);
            free(b);
        } else if (grid) {
            const int ox = (v & 1) * tw, oy = (v >> 1) * th;
            for (int y = 0; y < th; y++) memcpy(grid + ((size_t) (oy + y) * w + ox) * 4, px + (size_t) y * tw * 4, (size_t) tw * 4);
        }
        free(px);
    }
    if (grid) {
        char *b = png_b64(grid, w, h);
        free(grid);
        if (!b) {
            cJSON_Delete(res);
            *err = "render failed";
            return NULL;
        }
        cJSON_AddStringToObject(res, "png_base64", b);
        free(b);
        cJSON_AddNumberToObject(res, "width", w);
        cJSON_AddNumberToObject(res, "height", h);
    }
    cJSON *views = cJSON_AddArrayToObject(res, "views");
    for (int v = 0; v < 4; v++) cJSON_AddItemToArray(views, cJSON_CreateString(names[v]));
    cJSON_AddItemToObject(res, "bounds_min", vec_arr(mn));
    cJSON_AddItemToObject(res, "bounds_max", vec_arr(mx));
    return res;
}

// ---------------------------------------------------------------------------
// the call tools (run on g_ed.proj == p, entered by run_tool)

static void add_counts(cJSON *res, EditorProject *p) {
    cJSON_AddNumberToObject(res, "undo", (double) ed_history_steps(0));
    cJSON_AddNumberToObject(res, "redo", (double) ed_history_steps(1));
    cJSON_AddNumberToObject(res, "revision", p->revision);
    cJSON_AddBoolToObject(res, "dirty", p->dirty);
}

static cJSON *paste_objects(EditorProject *p, const cJSON *objects, const cJSON *args, const char **err) {
    if (!cJSON_GetArraySize(objects)) {
        *err = "nothing to paste (copy objects first)";
        return NULL;
    }
    cJSON *arr = cJSON_Duplicate(objects, 1);
    const cJSON *at = cJSON_GetObjectItemCaseSensitive(args, "at"), *off = cJSON_GetObjectItemCaseSensitive(args, "offset");
    double d[3] = {0, 0, 0};
    if (cJSON_GetArraySize(off) >= 3) {
        for (int k = 0; k < 3; k++) d[k] = cJSON_GetArrayItem(off, k)->valuedouble;
    } else {
        // relative transforms kept: the group's centre (findCenter: x, minY, z) goes to `at`
        // (default: the target camera's near position, like an editor paste)
        double c[6];
        ed_find_center(arr, c);
        vec3 target = editor_near_position();
        if (cJSON_GetArraySize(at) >= 3) target = (vec3) {(float) cJSON_GetArrayItem(at, 0)->valuedouble, (float) cJSON_GetArrayItem(at, 1)->valuedouble, (float) cJSON_GetArrayItem(at, 2)->valuedouble};
        d[0] = target.x - c[0];
        d[1] = target.y - c[1];
        d[2] = target.z - c[2];
    }
    const uint32_t batch = ed_history_batch_begin(ed_json_str(args, "label", "paste objects"));
    cJSON *res = cJSON_CreateObject();
    cJSON *uuids = cJSON_AddArrayToObject(res, "uuids");
    cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        cJSON *pp = cJSON_GetObjectItemCaseSensitive(o, "p");
        if (cJSON_GetArraySize(pp) >= 3) {
            for (int k = 0; k < 3; k++) {
                cJSON *n = cJSON_GetArrayItem(pp, k);
                cJSON_SetNumberValue(n, ed_round(n->valuedouble + d[k], 3));
            }
        }
        EditorObject *added = add_object(o, cJSON_GetObjectItemCaseSensitive(args, "colors"));
        if (added) cJSON_AddItemToArray(uuids, cJSON_CreateNumber(added->uuid));
    }
    ed_history_batch_end(batch);
    cJSON_Delete(arr);
    cJSON_AddNumberToObject(res, "undo_steps", 1);
    add_counts(res, p);
    return res;
}

// a map's "scripts" ({client, server}: base64 or raw, as an export carries them) -> the
// project's decoded scripts; a missing side is the template ("")
static void set_scripts(EditorProject *p, const cJSON *v) {
    for (int k = 0; k < 2; k++) {
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(v, k ? "server" : "client"));
        char *dec = t ? ed_model_b64_decode(t) : NULL;
        ed_ks_script_store(p->scripts, k, dec ? dec : t ? t : "");
        free(dec);
    }
}

static cJSON *tool_batch(EditorProject *p, const cJSON *args, const char **err, char *msg, const size_t mn) {
    const cJSON *colors = cJSON_GetObjectItemCaseSensitive(args, "colors");
    const cJSON *add = cJSON_GetObjectItemCaseSensitive(args, "add"), *rep = cJSON_GetObjectItemCaseSensitive(args, "replace"),
                *rem = cJSON_GetObjectItemCaseSensitive(args, "remove"), *mset = cJSON_GetObjectItemCaseSensitive(args, "map_set"),
                *munset = cJSON_GetObjectItemCaseSensitive(args, "map_unset");
    // validate first: a batch applies whole or not at all
    const cJSON *it;
    cJSON_ArrayForEach(it, rep) {
        if (!ed_obj_find(json_uuid(cJSON_GetObjectItemCaseSensitive(it, "uuid"))) || !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(it, "data"))) {
            snprintf(msg, mn, "replace: no object %u (or no data)", json_uuid(cJSON_GetObjectItemCaseSensitive(it, "uuid")));
            *err = msg;
            return NULL;
        }
    }
    cJSON_ArrayForEach(it, rem) {
        if (!ed_obj_find(json_uuid(it))) {
            snprintf(msg, mn, "remove: no object %u", json_uuid(it));
            *err = msg;
            return NULL;
        }
    }
    cJSON_ArrayForEach(it, add) {
        if (!cJSON_IsObject(it)) {
            *err = "add: every entry must be an object";
            return NULL;
        }
    }
    const uint32_t batch = ed_history_batch_begin(ed_json_str(args, "label", "batch edit"));
    cJSON *res = cJSON_CreateObject();
    cJSON *uuids = cJSON_AddArrayToObject(res, "uuids");
    if (cJSON_GetArraySize(mset) || cJSON_GetArraySize(munset)) {
        ed_history_add_map();
        int terrain = 0, zone = 0, ocean = 0;
        cJSON_ArrayForEach(it, mset) {
            if (!strcmp(it->string, "scripts")) {
                set_scripts(p, it); // KrunkScript: the project's code (the script window follows)
                continue;
            }
            cJSON *target = map_key_known(it->string) ? p->map_config : p->extra_map;
            cJSON_DeleteItemFromObjectCaseSensitive(target, it->string);
            if (!cJSON_IsNull(it)) cJSON_AddItemToObject(target, it->string, cJSON_Duplicate(it, 1));
            terrain |= !strncmp(it->string, "terrain", 7) || !strcmp(it->string, "sizeMlt");
            zone |= !strncmp(it->string, "zone", 4);
            ocean |= !strncmp(it->string, "oc", 2);
        }
        cJSON_ArrayForEach(it, munset) {
            if (!cJSON_IsString(it)) continue;
            if (!strcmp(it->valuestring, "scripts")) {
                set_scripts(p, NULL); // both back to the template
                continue;
            }
            cJSON_DeleteItemFromObjectCaseSensitive(p->extra_map, it->valuestring);
            // known keys go back to their default (the editor always has every map key)
            if (map_key_known(it->valuestring)) {
                EdMapState defaults;
                ed_model_map_init(&defaults);
                cJSON *def = cJSON_DetachItemFromObjectCaseSensitive(defaults.map_config, it->valuestring);
                ed_model_map_free(&defaults);
                if (def) cJSON_ReplaceItemInObjectCaseSensitive(p->map_config, it->valuestring, def);
            }
            terrain |= !strncmp(it->valuestring, "terrain", 7);
            zone |= !strncmp(it->valuestring, "zone", 4);
            ocean |= !strncmp(it->valuestring, "oc", 2);
        }
        map_changed(terrain, zone, ocean);
    }
    cJSON_ArrayForEach(it, rep) replace_object(ed_obj_find(json_uuid(cJSON_GetObjectItemCaseSensitive(it, "uuid"))), cJSON_GetObjectItemCaseSensitive(it, "data"), colors);
    cJSON_ArrayForEach(it, rem) {
        EditorObject *o = ed_obj_find(json_uuid(it));
        if (o) ed_obj_remove(o, 0, 1);
    }
    cJSON_ArrayForEach(it, add) {
        EditorObject *o = add_object(it, colors);
        if (o) cJSON_AddItemToArray(uuids, cJSON_CreateNumber(o->uuid));
    }
    ed_history_batch_end(batch);
    cJSON_AddNumberToObject(res, "undo_steps", 1);
    add_counts(res, p);
    return res;
}

static cJSON *run_tool_in(EditorProject *p, const char *tool, const cJSON *args, const char **code, const char **err, char *msg, const size_t mn) {
    *code = "bad_args";
    if (!strcmp(tool, "info")) {
        cJSON *r = project_json(p);
        cJSON_AddNumberToObject(r, "spawns", p->obj_count[2]);
        cJSON_AddNumberToObject(r, "collidable", p->obj_count[1]);
        if (p->selected) cJSON_AddNumberToObject(r, "selected", p->selected->uuid);
        else cJSON_AddNullToObject(r, "selected");
        return r;
    }
    if (!strcmp(tool, "query")) {
        const cJSON *prefab = cJSON_GetObjectItemCaseSensitive(args, "prefab"), *near = cJSON_GetObjectItemCaseSensitive(args, "near");
        const char *texture = ed_json_str(args, "texture", "");
        const uint32_t uuid = json_uuid(cJSON_GetObjectItemCaseSensitive(args, "uuid"));
        const int limit = (int) ed_json_num(args, "limit", 100), offset = (int) ed_json_num(args, "offset", 0);
        const char *fields = ed_json_str(args, "fields", "summary");
        cJSON *res = cJSON_CreateObject();
        cJSON *list = cJSON_CreateArray();
        int total = 0;
        for (size_t i = 0; i < p->count; i++) {
            const EditorObject *o = p->objects[i];
            if (uuid && o->uuid != uuid) continue;
            if (!prefab_matches(o, prefab)) continue;
            if (texture[0]) {
                cJSON *t = ed_prop_get(o, "texture");
                const int ok = cJSON_IsString(t) && ci_equal(t->valuestring, texture);
                cJSON_Delete(t);
                if (!ok) continue;
            }
            if (cJSON_GetArraySize(near) >= 4) {
                const vec3 q = ed_obj_vec(o, "p", (vec3) {0});
                const float dx = q.x - (float) cJSON_GetArrayItem(near, 0)->valuedouble, dy = q.y - (float) cJSON_GetArrayItem(near, 1)->valuedouble,
                            dz = q.z - (float) cJSON_GetArrayItem(near, 2)->valuedouble, r = (float) cJSON_GetArrayItem(near, 3)->valuedouble;
                if (dx * dx + dy * dy + dz * dz > r * r) continue;
            }
            if (total >= offset && (limit <= 0 || cJSON_GetArraySize(list) < limit)) {
                if (!strcmp(fields, "ids")) cJSON_AddItemToArray(list, cJSON_CreateNumber(o->uuid));
                else cJSON_AddItemToArray(list, object_json(o, !strcmp(fields, "full")));
            }
            total++;
        }
        cJSON_AddNumberToObject(res, "total", total);
        cJSON_AddNumberToObject(res, "count", cJSON_GetArraySize(list));
        const int next = offset + cJSON_GetArraySize(list);
        if (next < total) cJSON_AddNumberToObject(res, "next_offset", next);
        else cJSON_AddNullToObject(res, "next_offset");
        cJSON_AddItemToObject(res, "objects", list);
        return res;
    }
    if (!strcmp(tool, "get")) {
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(args, "uuid");
        EditorObject *o = ed_obj_find(json_uuid(u ? u : cJSON_GetObjectItemCaseSensitive(args, "id")));
        if (!o) {
            *code = "not_found";
            *err = "no such object";
            return NULL;
        }
        return object_json(o, 1);
    }
    if (!strcmp(tool, "export")) {
        cJSON *res = cJSON_CreateObject();
        // the mirror of kn_editor_mcp: sizes stay inline "s" (the bus "replace" / "add" objects
        // carry no xyz pool)
        const int full = ed_json_truthy(cJSON_GetObjectItemCaseSensitive(args, "full"));
        cJSON_AddItemToObject(res, "map", ed_map_export_flags(full ? ED_EXPORT_RAW | ED_EXPORT_NO_SIZE_POOL : 0));
        add_counts(res, p);
        return res;
    }
    if (!strcmp(tool, "history")) {
        const int limit = (int) ed_json_num(args, "limit", 50);
        cJSON *res = cJSON_CreateObject();
        cJSON_AddItemToObject(res, "undo", ed_history_labels(0, limit));
        cJSON_AddItemToObject(res, "redo", ed_history_labels(1, limit));
        return res;
    }
    if (!strcmp(tool, "log")) {
        const int tail = (int) ed_json_num(args, "tail", 50);
        cJSON *res = cJSON_CreateObject();
        cJSON *lines = cJSON_AddArrayToObject(res, "lines");
        const int start = B.log_count > tail && tail > 0 ? B.log_count - tail : 0;
        for (int i = start; i < B.log_count; i++) cJSON_AddItemToArray(lines, cJSON_CreateString(B.log[(B.log_head + i) % BUS_LOG_LINES]));
        return res;
    }
    if (!strcmp(tool, "render")) {
        const char *e = NULL;
        cJSON *r = tool_render(p, args, &e);
        if (!r) {
            *code = "internal";
            *err = e;
        }
        return r;
    }
    if (!strcmp(tool, "copy_objects")) {
        cJSON *res = cJSON_CreateObject();
        cJSON *objs = cJSON_AddArrayToObject(res, "objects");
        const cJSON *uuids = cJSON_GetObjectItemCaseSensitive(args, "uuids");
        if (cJSON_GetArraySize(uuids)) {
            const cJSON *u;
            cJSON_ArrayForEach(u, uuids) {
                EditorObject *o = ed_obj_find(json_uuid(u));
                if (!o) {
                    cJSON_Delete(res);
                    snprintf(msg, mn, "no object %u", json_uuid(u));
                    *code = "not_found";
                    *err = msg;
                    return NULL;
                }
                cJSON_AddItemToArray(objs, ed_obj_serialize(o));
            }
        } else if (p->selected) {
            const EditorGroup *g = ed_group_of(p->selected);
            for (size_t i = 0; g && i < g->count; i++) {
                EditorObject *o = ed_obj_find(g->objects[i]);
                if (o) cJSON_AddItemToArray(objs, ed_obj_serialize(o));
            }
            if (!g) cJSON_AddItemToArray(objs, ed_obj_serialize(p->selected));
        }
        if (!cJSON_GetArraySize(objs)) {
            cJSON_Delete(res);
            *err = "nothing selected and no uuids given";
            return NULL;
        }
        cJSON_AddNumberToObject(res, "count", cJSON_GetArraySize(objs));
        return res;
    }
    // ---- edits
    if (!strcmp(tool, "paste_objects")) return paste_objects(p, cJSON_GetObjectItemCaseSensitive(args, "objects"), args, err);
    if (!strcmp(tool, "batch")) return tool_batch(p, args, err, msg, mn);
    if (!strcmp(tool, "add")) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddItemToObject(b, "add", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(args, "objects"), 1));
        if (cJSON_GetObjectItemCaseSensitive(args, "colors")) cJSON_AddItemToObject(b, "colors", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(args, "colors"), 1));
        cJSON_AddStringToObject(b, "label", ed_json_str(args, "label", "add objects"));
        cJSON *r = tool_batch(p, b, err, msg, mn);
        cJSON_Delete(b);
        return r;
    }
    if (!strcmp(tool, "remove")) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddItemToObject(b, "remove", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(args, "uuids"), 1));
        cJSON_AddStringToObject(b, "label", ed_json_str(args, "label", "remove objects"));
        cJSON *r = tool_batch(p, b, err, msg, mn);
        cJSON_Delete(b);
        return r;
    }
    if (!strcmp(tool, "update")) {
        EditorObject *o = ed_obj_find(json_uuid(cJSON_GetObjectItemCaseSensitive(args, "uuid")));
        if (!o) {
            *code = "not_found";
            *err = "no such object";
            return NULL;
        }
        const uint32_t batch = ed_history_batch_begin(ed_json_str(args, "label", "edit object"));
        cJSON *before = cJSON_Duplicate(o->data, 1);
        const int cls = ed_obj_count_class(o);
        const cJSON *it;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(args, "set")) {
            cJSON_DeleteItemFromObjectCaseSensitive(o->data, it->string);
            if (!cJSON_IsNull(it)) cJSON_AddItemToObject(o->data, it->string, cJSON_Duplicate(it, 1));
        }
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(args, "props")) ed_prop_set(o, it->string, it);
        p->obj_count[cls]--;
        p->obj_count[ed_obj_count_class(o)]++;
        ed_obj_rebuild(o);
        if (!cJSON_Compare(before, o->data, 1)) {
            ed_history_add('u', o, before);
            before = NULL;
            ed_mark_changed();
        }
        cJSON_Delete(before);
        cJSON_Delete(o->history);
        o->history = cJSON_Duplicate(o->data, 1);
        ed_history_batch_end(batch);
        g_ed.dirty_counts = 1;
        if (o == p->selected) g_ed.dirty_panel = 1;
        cJSON *r = object_json(o, 1);
        add_counts(r, p);
        return r;
    }
    if (!strcmp(tool, "select")) {
        const cJSON *uuids = cJSON_GetObjectItemCaseSensitive(args, "uuids");
        EditorObject *o = cJSON_GetArraySize(uuids) ? ed_obj_find(json_uuid(cJSON_GetArrayItem(uuids, 0))) : NULL;
        if (cJSON_GetArraySize(uuids) && !o) {
            *code = "not_found";
            *err = "no such object";
            return NULL;
        }
        ed_obj_select(o);
        cJSON *r = cJSON_CreateObject();
        if (o) cJSON_AddNumberToObject(r, "selected", o->uuid);
        else cJSON_AddNullToObject(r, "selected");
        return r;
    }
    if (!strcmp(tool, "undo") || !strcmp(tool, "redo")) {
        const int redo = tool[0] == 'r';
        if (p == g_ed.shown) ed_history_poll(); // the last second's UI edits are steps too
        const int steps = (int) ed_json_num(args, "steps", 1);
        int done = 0;
        while (done < steps && ed_history_step(redo)) done++;
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, redo ? "redone" : "undone", done);
        add_counts(r, p);
        return r;
    }
    if (!strcmp(tool, "save")) {
        const char *path = ed_json_str(args, "path", "");
        const int pretty = !strcmp(ed_json_str(args, "format", "compact"), "pretty");
        if (!path[0] && !p->path[0]) {
            *err = "the project has no file yet: give a path";
            return NULL;
        }
        const long bytes = ed_project_save(p, path[0] ? path : NULL, ed_json_truthy(cJSON_GetObjectItemCaseSensitive(args, "as")), pretty);
        if (bytes < 0) {
            snprintf(msg, mn, "could not write %s", path[0] ? path : p->path);
            *code = "internal";
            *err = msg;
            return NULL;
        }
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "path", path[0] ? path : p->path);
        cJSON_AddNumberToObject(r, "bytes", (double) bytes);
        add_counts(r, p);
        return r;
    }
    snprintf(msg, mn, "unknown tool \"%s\"", tool);
    *err = msg;
    return NULL;
}

static cJSON *run_tool(EditorProject *p, const char *tool, const cJSON *args, const char **code, const char **err, char *msg, const size_t mn) {
    EditorProject *prev;
    ed_project_enter(p, &prev);
    cJSON *r = run_tool_in(p, tool ? tool : "", args, code, err, msg, mn);
    ed_project_leave(prev);
    return r;
}

// ---------------------------------------------------------------------------
// ops on this instance (host: its own projects; client: the host's forwarded requests)

static cJSON *local_op(const char *op, const cJSON *args, const char **code, const char **err, char *msg, const size_t mn) {
    *code = "bad_args";
    const int local = (int) ed_json_num(args, "local", 0);
    const char *ref = ed_json_str(args, "project", "");
    EditorProject *p = local ? ed_project_find(local) : ed_project_resolve(ref);
    if (!strcmp(op, "open")) {
        const char *path = ed_json_str(args, "path", "");
        if (!path[0]) {
            *err = "open: no path";
            return NULL;
        }
        EditorProject *np = ed_project_open_file(path, cJSON_GetObjectItemCaseSensitive(args, "activate") ? ed_json_truthy(cJSON_GetObjectItemCaseSensitive(args, "activate")) : 1);
        if (!np) {
            snprintf(msg, mn, "cannot open %s", path);
            *code = "not_found";
            *err = msg;
            return NULL;
        }
        return project_json(np);
    }
    if (!strcmp(op, "new")) {
        EditorProject *np = ed_project_new(1);
        if (!np) {
            *code = "internal";
            *err = "new project failed";
            return NULL;
        }
        const char *name = ed_json_str(args, "name", "");
        if (name[0]) ed_json_set_str(np->map_config, "name", name);
        return project_json(np);
    }
    if (!p) {
        *code = "not_found";
        snprintf(msg, mn, "no project \"%s\"", ref);
        *err = msg;
        return NULL;
    }
    if (!strcmp(op, "close")) {
        if (p->dirty && !ed_json_truthy(cJSON_GetObjectItemCaseSensitive(args, "force"))) {
            *code = "dirty";
            snprintf(msg, mn, "\"%s\" has unsaved changes (save it, or close with force)", ed_project_title(p));
            *err = msg;
            return NULL;
        }
        cJSON *r = project_json(p);
        ed_project_close(p, 1);
        cJSON_AddBoolToObject(r, "closed", 1);
        return r;
    }
    if (!strcmp(op, "focus")) {
        ed_project_activate(p);
        if (!headless_enabled() && g_ed.client && g_ed.client->window) glfwFocusWindow(g_ed.client->window);
        return project_json(p);
    }
    if (!strcmp(op, "call")) return run_tool(p, ed_json_str(args, "tool", ""), cJSON_GetObjectItemCaseSensitive(args, "args"), code, err, msg, mn);
    snprintf(msg, mn, "unknown op \"%s\"", op);
    *err = msg;
    return NULL;
}

// ---------------------------------------------------------------------------
// sending

static void peer_send(Peer *pe, cJSON *msg) {
    if (pe && !pe->conn.closed) edbus_send(&pe->conn, msg);
    cJSON_Delete(msg);
}

static void broadcast_ev(cJSON *ev, const unsigned mask, const int skip_peer) {
    char *text = cJSON_PrintUnformatted(ev);
    cJSON_Delete(ev);
    if (!text) return;
    const size_t len = strlen(text);
    for (int i = 0; i < B.peer_count; i++) {
        Peer *pe = B.peers[i];
        if (pe->id == skip_peer || pe->conn.closed) continue;
        const int wants = (pe->events & mask) || (mask == BUS_EV_CLIPBOARD && pe->role == ROLE_EDITOR);
        if (wants) edbus_send_text(&pe->conn, text, len);
    }
    cJSON_free(text);
}

static cJSON *make_ev(const char *name) {
    cJSON *e = edbus_msg("ev");
    cJSON_AddStringToObject(e, "ev", name);
    return e;
}

// ---------------------------------------------------------------------------
// permissions (host)

static cJSON *grants(void) {
    cJSON *g = cJSON_GetObjectItemCaseSensitive(g_ed.settings, "mcpGrants");
    if (!cJSON_IsObject(g)) {
        cJSON_DeleteItemFromObjectCaseSensitive(g_ed.settings, "mcpGrants");
        g = cJSON_AddObjectToObject(g_ed.settings, "mcpGrants");
    }
    return g;
}

static void scope_text(const unsigned mask, char *out, const size_t n) {
    out[0] = 0;
    if (mask & EDBUS_SCOPE_READ) strncat(out, "\n - read: query objects, render views", n - strlen(out) - 1);
    if (mask & EDBUS_SCOPE_EDIT) strncat(out, "\n - edit: modify objects, undo, save files", n - strlen(out) - 1);
    if (mask & EDBUS_SCOPE_PROJECT) strncat(out, "\n - project: open, close and switch projects", n - strlen(out) - 1);
}

static void process_message(Peer *pe, cJSON *msg);

static void permission_finish(Peer *pe, const unsigned granted, const char *decision, const char *reason) {
    pe->granted = granted;
    pe->perm_state = PERM_DECIDED;
    snprintf(pe->decision, sizeof(pe->decision), "%s", decision);
    snprintf(pe->reason, sizeof(pe->reason), "%s", reason ? reason : "");
    cJSON *r = edbus_msg("permission_result");
    cJSON_AddItemToObject(r, "id", pe->perm_id ? cJSON_Duplicate(pe->perm_id, 1) : cJSON_CreateNull());
    cJSON_AddItemToObject(r, "granted", edbus_scope_json(granted));
    cJSON_AddStringToObject(r, "decision", decision);
    cJSON_AddStringToObject(r, "reason", reason ? reason : "");
    char scopes[64] = "";
    if (granted & EDBUS_SCOPE_READ) strcat(scopes, " read");
    if (granted & EDBUS_SCOPE_EDIT) strcat(scopes, " edit");
    if (granted & EDBUS_SCOPE_PROJECT) strcat(scopes, " project");
    bus_log("permission %s for %s (%s, pid %d):%s%s%s", decision, pe->name, pe->client_id, pe->pid, granted ? scopes : " nothing",
            reason && *reason ? " - " : "", reason ? reason : "");
    peer_send(pe, r);
    cJSON_Delete(pe->perm_id);
    pe->perm_id = NULL;
    // the requests that waited for the decision
    cJSON *queued = pe->queued;
    pe->queued = NULL;
    cJSON *m;
    while (queued && (m = cJSON_DetachItemFromArray(queued, 0))) process_message(pe, m);
    cJSON_Delete(queued);
}

static void permission_answer(void *user, const int choice) {
    Peer *pe = peer_by_id((int) (intptr_t) user);
    if (!pe || pe->perm_state != PERM_PENDING) return;
    if (choice == 0) {
        permission_finish(pe, pe->perm_wanted, "allow_once", "allowed by the user for this connection");
    } else if (choice == 1) {
        cJSON *g = grants();
        cJSON_DeleteItemFromObjectCaseSensitive(g, pe->client_id);
        cJSON *e = cJSON_AddObjectToObject(g, pe->client_id);
        cJSON_AddItemToObject(e, "scopes", edbus_scope_json(pe->perm_wanted));
        cJSON_AddStringToObject(e, "name", pe->name);
        char when[32];
        const time_t t = time(NULL);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&t));
        cJSON_AddStringToObject(e, "since", when);
        editor_settings_save();
        permission_finish(pe, pe->perm_wanted, "always", "always allowed by the user (File > Projects > MCP Access... revokes)");
    } else {
        permission_finish(pe, 0, "deny", choice < 0 ? "dismissed by the user" : "denied by the user");
    }
}

static void permission_request(Peer *pe, cJSON *msg) {
    cJSON_Delete(pe->perm_id);
    pe->perm_id = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(msg, "id"), 1);
    unsigned wanted = edbus_scope_from_json(cJSON_GetObjectItemCaseSensitive(msg, "scopes"));
    if (!wanted) wanted = EDBUS_SCOPE_ALL;
    pe->perm_wanted = wanted;
    if (pe->role == ROLE_EDITOR) {
        permission_finish(pe, EDBUS_SCOPE_ALL, "auto", "editor instance");
        return;
    }
    if (B.auto_deny) {
        permission_finish(pe, 0, "deny", "--mcp-auto-deny");
        return;
    }
    if (B.auto_accept) {
        const unsigned g = wanted & B.auto_scopes;
        permission_finish(pe, g, g ? "auto" : "deny", g == wanted ? "--mcp-auto-accept" : "--mcp-auto-accept (scopes limited by the flag)");
        return;
    }
    const cJSON *saved = cJSON_GetObjectItemCaseSensitive(grants(), pe->client_id);
    if (saved) {
        const unsigned g = wanted & edbus_scope_from_json(cJSON_GetObjectItemCaseSensitive(saved, "scopes"));
        if (g == wanted) {
            permission_finish(pe, g, "always", "remembered grant");
            return;
        }
    }
    if (headless_enabled() || !g_ed.active) {
        permission_finish(pe, 0, "deny", "headless: no user to ask (start the editor with --mcp-auto-accept)");
        return;
    }
    pe->perm_state = PERM_PENDING;
    char scopes[256], text[900];
    scope_text(wanted, scopes, sizeof(scopes));
    snprintf(text, sizeof(text), "%s%s%s (pid %d, client id \"%s\") wants to connect to this editor and asks for:%s", pe->name,
             pe->version[0] ? " " : "", pe->version, pe->pid, pe->client_id, scopes);
    static const char *const buttons[] = {"Allow once", "Always allow this client", "Deny"};
    bus_log("permission requested by %s (%s, pid %d): waiting for the user", pe->name, pe->client_id, pe->pid);
    ed_choice("MCP access request", text, buttons, 3, permission_answer, (void *) (intptr_t) pe->id);
}

static void revoke_prompted(void *user, const char *value) {
    (void) user;
    if (!value || !*value) return;
    cJSON *g = grants();
    if (!strcmp(value, "*")) {
        cJSON_DeleteItemFromObjectCaseSensitive(g_ed.settings, "mcpGrants");
    } else if (cJSON_GetObjectItemCaseSensitive(g, value)) {
        cJSON_DeleteItemFromObjectCaseSensitive(g, value);
    } else {
        editor_message("No remembered client with that id.");
        return;
    }
    editor_settings_save();
    // connected clients of that id lose their access now
    for (int i = 0; i < B.peer_count; i++) {
        Peer *pe = B.peers[i];
        if (pe->role == ROLE_TOOL && (!strcmp(value, "*") || !strcmp(pe->client_id, value)) && !strcmp(pe->decision, "always")) {
            pe->granted = 0;
            snprintf(pe->decision, sizeof(pe->decision), "deny");
            snprintf(pe->reason, sizeof(pe->reason), "grant revoked by the user");
        }
    }
    editor_message(!strcmp(value, "*") ? "Every remembered MCP grant was revoked." : "MCP grant revoked.");
}

static void access_choice(void *user, const int choice) {
    (void) user;
    if (choice == 0) {
        const cJSON *first = grants()->child;
        ed_prompt("Client id to revoke (* = all)", first ? first->string : "*", revoke_prompted, NULL);
    } else if (choice == 1) {
        revoke_prompted(NULL, "*");
    }
}

void ed_bus_manage_access(void) {
    char text[2048] = "";
    size_t n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, grants()) {
        char scopes[64] = "";
        const cJSON *s;
        cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(e, "scopes")) {
            if (cJSON_IsString(s)) {
                strncat(scopes, scopes[0] ? ", " : "", sizeof(scopes) - strlen(scopes) - 1);
                strncat(scopes, s->valuestring, sizeof(scopes) - strlen(scopes) - 1);
            }
        }
        n += (size_t) snprintf(text + n, n < sizeof(text) ? sizeof(text) - n : 0, "\n - %s (%s): %s, since %s", e->string, ed_json_str(e, "name", "?"), scopes,
                               ed_json_str(e, "since", "?"));
        if (n >= sizeof(text)) break;
    }
    char msg[2300];
    snprintf(msg, sizeof(msg), "Clients always allowed to use this editor over the editor bus:%s%s\n\nConnected now: %d. Bus: %s.",
             text[0] ? "" : "\n (none)", text, B.peer_count, B.mode == BUS_HOST ? "hosting" : B.mode == BUS_CLIENT ? "joined" : "off");
    static const char *const buttons[] = {"Revoke...", "Revoke all", "Close"};
    ed_choice("MCP Access", msg, buttons, 3, access_choice, NULL);
}

// ---------------------------------------------------------------------------
// host: requests

static void reply_err(Peer *pe, const cJSON *id, const char *code, const char *message) { peer_send(pe, edbus_res_err(id, code, message)); }

static int forward(Peer *from, const cJSON *orig_id, const int to_inst, const char *op, cJSON *args, const int kind) {
    Peer *to = peer_by_id(to_inst);
    if (!to || to->role != ROLE_EDITOR) {
        cJSON_Delete(args);
        reply_err(from, orig_id, "no_instance", "that editor instance is gone");
        return 0;
    }
    int slot = -1;
    for (int i = 0; i < BUS_MAX_FWD && slot < 0; i++) if (!B.fwd[i].used) slot = i;
    if (slot < 0) {
        cJSON_Delete(args);
        reply_err(from, orig_id, "busy", "too many requests in flight");
        return 0;
    }
    Fwd *f = &B.fwd[slot];
    f->used = 1;
    f->id = ++B.next_fwd;
    f->to_peer = to->id;
    f->from_peer = from ? from->id : 0;
    f->orig_id = cJSON_Duplicate(orig_id, 1);
    f->kind = kind;
    peer_send(to, edbus_req(f->id, op, args));
    return 1;
}

static void store_clip(cJSON *objects, const int from_peer) {
    cJSON_Delete(B.clip);
    B.clip = objects;
    // every editor pastes it with Ctrl+V (and the host's own page)
    cJSON_Delete(g_ed.copy);
    g_ed.copy = cJSON_Duplicate(objects, 1);
    cJSON *ev = make_ev("clipboard");
    cJSON_AddNumberToObject(ev, "count", cJSON_GetArraySize(objects));
    cJSON_AddItemToObject(ev, "objects", cJSON_Duplicate(objects, 1));
    broadcast_ev(ev, BUS_EV_CLIPBOARD, from_peer);
}

static void host_request(Peer *pe, cJSON *msg) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    const char *op = ed_json_str(msg, "op", "");
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(msg, "args");
    const char *tool = !strcmp(op, "call") ? ed_json_str(args, "tool", "") : NULL;
    if (pe->role == ROLE_TOOL) {
        const unsigned need = edbus_scope_for(op, tool);
        if ((pe->granted & need) != need) {
            char m[400];
            if (pe->perm_state == PERM_NONE) snprintf(m, sizeof(m), "no access yet: send permission_request first");
            else snprintf(m, sizeof(m), "%s has no %s access to this editor (%s: %s)", pe->name,
                          need & EDBUS_SCOPE_EDIT ? "edit" : need & EDBUS_SCOPE_PROJECT ? "project" : "read", pe->decision, pe->reason);
            reply_err(pe, id, "permission_denied", m);
            return;
        }
    }
    char msg_buf[600];
    const char *code = NULL, *err = NULL;
    if (!strcmp(op, "ping")) {
        peer_send(pe, edbus_res_ok(id, NULL));
        return;
    }
    if (!strcmp(op, "list")) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddItemToObject(r, "instances", instances_json());
        cJSON_AddNumberToObject(r, "self", pe->id);
        peer_send(pe, edbus_res_ok(id, r));
        return;
    }
    if (!strcmp(op, "subscribe")) {
        unsigned mask = 0;
        const cJSON *e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(args, "events")) {
            if (!cJSON_IsString(e)) continue;
            if (!strcmp(e->valuestring, "selection")) mask |= BUS_EV_SELECTION;
            else if (!strcmp(e->valuestring, "change")) mask |= BUS_EV_CHANGE;
            else if (!strcmp(e->valuestring, "projects")) mask |= BUS_EV_PROJECTS;
            else if (!strcmp(e->valuestring, "clipboard")) mask |= BUS_EV_CLIPBOARD;
            else if (!strcmp(e->valuestring, "all")) mask |= 15;
        }
        pe->events = mask;
        peer_send(pe, edbus_res_ok(id, NULL));
        return;
    }
    // open / new: on an instance (default: this one)
    if (!strcmp(op, "open") || !strcmp(op, "new")) {
        const int inst = (int) ed_json_num(args, "instance", B.self_id);
        if (inst != B.self_id) {
            forward(pe, id, inst, op, cJSON_Duplicate(args, 1), FWD_RELAY);
            return;
        }
        cJSON *r = local_op(op, args, &code, &err, msg_buf, sizeof(msg_buf));
        peer_send(pe, r ? edbus_res_ok(id, r) : edbus_res_err(id, code, err));
        return;
    }
    // everything else names a project
    const char *ref = ed_json_str(args, "project", "");
    int local = 0;
    const int inst = resolve(ref, &local);
    if (!inst) {
        snprintf(msg_buf, sizeof(msg_buf), "no project \"%s\" (projects: list)", ref);
        reply_err(pe, id, "not_found", msg_buf);
        return;
    }
    if (!strcmp(op, "copy")) {
        cJSON *cargs = cJSON_CreateObject();
        cJSON_AddNumberToObject(cargs, "local", local);
        cJSON_AddStringToObject(cargs, "tool", "copy_objects");
        cJSON_AddItemToObject(cargs, "args", args ? cJSON_Duplicate(args, 1) : cJSON_CreateObject());
        if (inst != B.self_id) {
            forward(pe, id, inst, "call", cargs, FWD_COPY);
            return;
        }
        cJSON *r = local_op("call", cargs, &code, &err, msg_buf, sizeof(msg_buf));
        cJSON_Delete(cargs);
        if (!r) {
            reply_err(pe, id, code, err);
            return;
        }
        cJSON *objs = cJSON_DetachItemFromObjectCaseSensitive(r, "objects");
        cJSON *out = cJSON_CreateObject();
        cJSON_AddNumberToObject(out, "count", cJSON_GetArraySize(objs));
        store_clip(objs, pe->id);
        cJSON_Delete(r);
        peer_send(pe, edbus_res_ok(id, out));
        return;
    }
    cJSON *fargs = NULL;
    const char *fop = op;
    if (!strcmp(op, "paste")) {
        if (!B.clip) {
            reply_err(pe, id, "bad_args", "the bus clipboard is empty (copy first)");
            return;
        }
        fargs = cJSON_CreateObject();
        cJSON_AddNumberToObject(fargs, "local", local);
        cJSON_AddStringToObject(fargs, "tool", "paste_objects");
        cJSON *pa = args ? cJSON_Duplicate(args, 1) : cJSON_CreateObject();
        cJSON_DeleteItemFromObjectCaseSensitive(pa, "objects");
        cJSON_AddItemToObject(pa, "objects", cJSON_Duplicate(B.clip, 1));
        cJSON_AddItemToObject(fargs, "args", pa);
        fop = "call";
    } else if (!strcmp(op, "call") || !strcmp(op, "close") || !strcmp(op, "focus")) {
        fargs = args ? cJSON_Duplicate(args, 1) : cJSON_CreateObject();
        cJSON_DeleteItemFromObjectCaseSensitive(fargs, "project");
        cJSON_AddNumberToObject(fargs, "local", local);
    } else {
        snprintf(msg_buf, sizeof(msg_buf), "unknown op \"%s\"", op);
        reply_err(pe, id, "bad_args", msg_buf);
        return;
    }
    if (inst != B.self_id) {
        forward(pe, id, inst, fop, fargs, FWD_RELAY);
        return;
    }
    cJSON *r = local_op(fop, fargs, &code, &err, msg_buf, sizeof(msg_buf));
    cJSON_Delete(fargs);
    peer_send(pe, r ? edbus_res_ok(id, r) : edbus_res_err(id, code, err));
}

static void host_response(Peer *pe, cJSON *msg) {
    const int id = (int) ed_json_num(msg, "id", 0);
    for (int i = 0; i < BUS_MAX_FWD; i++) {
        Fwd *f = &B.fwd[i];
        if (!f->used || f->id != id || f->to_peer != pe->id) continue;
        Peer *from = peer_by_id(f->from_peer);
        if (f->kind == FWD_COPY && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(msg, "ok"))) {
            cJSON *r = cJSON_GetObjectItemCaseSensitive(msg, "result");
            cJSON *objs = cJSON_DetachItemFromObjectCaseSensitive(r, "objects");
            cJSON *out = cJSON_CreateObject();
            cJSON_AddNumberToObject(out, "count", cJSON_GetArraySize(objs));
            store_clip(objs, from ? from->id : 0);
            if (from) peer_send(from, edbus_res_ok(f->orig_id, out));
            else cJSON_Delete(out);
        } else if (from) {
            // relay with the origin's id
            cJSON_DeleteItemFromObjectCaseSensitive(msg, "id");
            cJSON_AddItemToObject(msg, "id", cJSON_Duplicate(f->orig_id, 1));
            edbus_send(&from->conn, msg);
        }
        cJSON_Delete(f->orig_id);
        memset(f, 0, sizeof(*f));
        return;
    }
}

static void host_event(Peer *pe, cJSON *msg) {
    const char *ev = ed_json_str(msg, "ev", "");
    if (pe->role != ROLE_EDITOR) return;
    if (!strcmp(ev, "projects")) {
        cJSON_Delete(pe->projects);
        pe->projects = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(msg, "projects"), 1);
        cJSON *e = make_ev("projects");
        cJSON_AddItemToObject(e, "instances", instances_json());
        broadcast_ev(e, BUS_EV_PROJECTS, -1);
    } else if (!strcmp(ev, "clipboard")) {
        cJSON *objs = cJSON_DetachItemFromObjectCaseSensitive(msg, "objects");
        if (objs) store_clip(objs, pe->id);
    } else if (!strcmp(ev, "change")) {
        broadcast_ev(cJSON_Duplicate(msg, 1), BUS_EV_CHANGE, pe->id);
    } else if (!strcmp(ev, "selection")) {
        broadcast_ev(cJSON_Duplicate(msg, 1), BUS_EV_SELECTION, pe->id);
    }
}

static void hello(Peer *pe, cJSON *msg) {
    const char *token = ed_json_str(msg, "token", "");
    if (!edbus_token_equal(token, B.token)) {
        cJSON *e = edbus_msg("error");
        cJSON_AddStringToObject(e, "code", "bad_token");
        cJSON_AddStringToObject(e, "message", "wrong bus token");
        edbus_send(&pe->conn, e);
        cJSON_Delete(e);
        edbus_flush(&pe->conn);
        pe->conn.closed = 1;
        bus_log("refused a client with a wrong token");
        return;
    }
    const char *role = ed_json_str(msg, "role", "tool");
    pe->role = !strcmp(role, "editor") ? ROLE_EDITOR : ROLE_TOOL;
    snprintf(pe->client_id, sizeof(pe->client_id), "%s", ed_json_str(msg, "client_id", "tool"));
    snprintf(pe->name, sizeof(pe->name), "%s", ed_json_str(msg, "name", pe->client_id));
    snprintf(pe->version, sizeof(pe->version), "%s", ed_json_str(msg, "version", ""));
    pe->pid = (int) ed_json_num(msg, "pid", 0);
    if (pe->role == ROLE_EDITOR) {
        pe->granted = EDBUS_SCOPE_ALL;
        pe->perm_state = PERM_DECIDED;
        snprintf(pe->decision, sizeof(pe->decision), "auto");
    }
    cJSON *w = edbus_msg("welcome");
    cJSON_AddNumberToObject(w, "proto", EDBUS_PROTO);
    cJSON_AddNumberToObject(w, "peer", pe->id);
    cJSON_AddNumberToObject(w, "host_pid", (double) getpid());
    cJSON_AddNumberToObject(w, "host_instance", B.self_id);
    peer_send(pe, w);
    bus_log("%s %s joined (peer %d, pid %d%s%s)", pe->role == ROLE_EDITOR ? "editor" : "client", pe->name, pe->id, pe->pid,
            pe->version[0] ? ", version " : "", pe->version);
}

static void process_message(Peer *pe, cJSON *msg) {
    const char *t = ed_json_str(msg, "t", "");
    if (pe->role == ROLE_NONE) {
        if (!strcmp(t, "hello")) hello(pe, msg);
        else {
            cJSON *e = edbus_msg("error");
            cJSON_AddStringToObject(e, "code", "hello_required");
            cJSON_AddStringToObject(e, "message", "send hello with the bus token first");
            peer_send(pe, e);
            pe->conn.closed = 1;
        }
        cJSON_Delete(msg);
        return;
    }
    if (!strcmp(t, "permission_request")) {
        if (pe->perm_state == PERM_PENDING) {
            cJSON_Delete(pe->perm_id);
            pe->perm_id = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(msg, "id"), 1);
        } else {
            permission_request(pe, msg);
        }
    } else if (!strcmp(t, "req")) {
        if (pe->perm_state == PERM_PENDING) {
            // waits for the user's answer
            if (!pe->queued) pe->queued = cJSON_CreateArray();
            cJSON_AddItemToArray(pe->queued, msg);
            return;
        }
        host_request(pe, msg);
    } else if (!strcmp(t, "res")) {
        host_response(pe, msg);
    } else if (!strcmp(t, "ev")) {
        host_event(pe, msg);
    }
    cJSON_Delete(msg);
}

static void peer_free(Peer *pe) {
    edbus_conn_close(&pe->conn);
    cJSON_Delete(pe->perm_id);
    cJSON_Delete(pe->queued);
    cJSON_Delete(pe->projects);
    free(pe);
}

static void drop_peer(const int index) {
    Peer *pe = B.peers[index];
    const int was_editor = pe->role == ROLE_EDITOR;
    bus_log("%s %s left (peer %d)", was_editor ? "editor" : "client", pe->name[0] ? pe->name : "?", pe->id);
    // requests forwarded to it fail; requests from it are dropped
    for (int i = 0; i < BUS_MAX_FWD; i++) {
        Fwd *f = &B.fwd[i];
        if (!f->used) continue;
        if (f->to_peer == pe->id) {
            Peer *from = peer_by_id(f->from_peer);
            if (from) reply_err(from, f->orig_id, "no_instance", "the editor that had the project closed");
        }
        if (f->to_peer == pe->id || f->from_peer == pe->id) {
            cJSON_Delete(f->orig_id);
            memset(f, 0, sizeof(*f));
        }
    }
    peer_free(pe);
    B.peers[index] = B.peers[--B.peer_count];
    if (was_editor) B.projects_dirty = 1;
}

// ---------------------------------------------------------------------------
// client editors: requests from the host

static void client_message(cJSON *msg) {
    const char *t = ed_json_str(msg, "t", "");
    if (!strcmp(t, "req")) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
        char buf[600];
        const char *code = NULL, *err = NULL;
        cJSON *r = local_op(ed_json_str(msg, "op", ""), cJSON_GetObjectItemCaseSensitive(msg, "args"), &code, &err, buf, sizeof(buf));
        cJSON *res = r ? edbus_res_ok(id, r) : edbus_res_err(id, code, err);
        edbus_send(&B.up, res);
        cJSON_Delete(res);
    } else if (!strcmp(t, "ev") && !strcmp(ed_json_str(msg, "ev", ""), "clipboard")) {
        const cJSON *objs = cJSON_GetObjectItemCaseSensitive(msg, "objects");
        if (cJSON_IsArray(objs)) {
            cJSON_Delete(g_ed.copy);
            g_ed.copy = cJSON_Duplicate(objs, 1);
        }
    }
}

// ---------------------------------------------------------------------------
// web: the editor tabs of one browser share a BroadcastChannel (html_channel_*, web_dom.c
// glue) instead of the bus: {"t":"projects","tab","projects":[...]} announces a tab's maps,
// {"t":"clipboard","tab","objects":[...]} shares Ctrl+C, {"t":"bye","tab"} on leave. A
// browser tab cannot listen on a socket, so tools (kn_editor_mcp) attach to native editors only.

#define WEB_CHANNEL "krunknative-editor"
static int g_web_on;
static unsigned g_web_tab;
static cJSON *g_web_tabs; // tab id -> its project list

static void web_post(cJSON *m) {
    cJSON_AddNumberToObject(m, "tab", g_web_tab);
    char *text = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    if (text) html_channel_post(WEB_CHANNEL, text);
    cJSON_free(text);
}

static void web_announce(void) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "t", "projects");
    cJSON_AddItemToObject(m, "projects", local_projects());
    web_post(m);
}

static void web_start(void) {
    if (g_web_on || !html_channel_open(WEB_CHANNEL)) return;
    g_web_on = 1;
    g_web_tab = (unsigned) (now_s() * 1000.0) ^ (unsigned) rand();
    g_web_tabs = cJSON_CreateObject();
    web_announce();
    // the tabs already open answer with their lists
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "t", "hello");
    web_post(m);
}

static void web_poll(void) {
    if (!g_web_on) return;
    char *text;
    while ((text = html_channel_take(WEB_CHANNEL))) {
        cJSON *m = cJSON_Parse(text);
        free(text);
        const char *t = ed_json_str(m, "t", "");
        char tab[24];
        snprintf(tab, sizeof(tab), "%u", (unsigned) ed_json_num(m, "tab", 0));
        if (!strcmp(t, "hello")) {
            web_announce();
        } else if (!strcmp(t, "projects")) {
            cJSON_DeleteItemFromObjectCaseSensitive(g_web_tabs, tab);
            cJSON_AddItemToObject(g_web_tabs, tab, cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(m, "projects"), 1));
        } else if (!strcmp(t, "bye")) {
            cJSON_DeleteItemFromObjectCaseSensitive(g_web_tabs, tab);
        } else if (!strcmp(t, "clipboard")) {
            const cJSON *objs = cJSON_GetObjectItemCaseSensitive(m, "objects");
            if (cJSON_IsArray(objs)) {
                cJSON_Delete(g_ed.copy);
                g_ed.copy = cJSON_Duplicate(objs, 1);
            }
        }
        cJSON_Delete(m);
    }
    if (B.projects_dirty) {
        B.projects_dirty = 0;
        web_announce();
    }
}

static void web_stop(void) {
    if (!g_web_on) return;
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "t", "bye");
    web_post(m);
    cJSON_Delete(g_web_tabs);
    g_web_tabs = NULL;
    g_web_on = 0;
}

// ---------------------------------------------------------------------------
// lifetime

static int try_join(void) {
    cJSON *welcome = NULL;
    char version[32];
    snprintf(version, sizeof(version), "%s", BUS_VERSION);
    if (!edbus_client_connect(&B.up, &B.paths, "editor", "krunknative-editor", "KrunkNative Editor", version, 1500, &welcome)) return 0;
    B.self_id = (int) ed_json_num(welcome, "peer", 0);
    cJSON_Delete(welcome);
    B.mode = BUS_CLIENT;
    B.projects_dirty = 1;
    bus_log("joined the editor bus at %s as instance %d", B.paths.tcp ? B.paths.port : B.paths.sock, B.self_id);
    return 1;
}

static int try_host(void) {
    const int lock = edbus_lock_try(&B.paths);
    if (lock < 0) return 0;
    edbus_token_generate(B.token);
    const int fd = edbus_listen(&B.paths);
    if (fd < 0 || !edbus_token_write(&B.paths, B.token)) {
        if (fd >= 0) edbus_close_fd(fd);
        edbus_lock_release(lock);
        bus_log("cannot listen on %s", B.paths.sock);
        return 0;
    }
    B.lock_fd = lock;
    B.listen_fd = fd;
    B.mode = BUS_HOST;
    B.self_id = 1;
    B.next_peer = 2;
    B.projects_dirty = 1;
    bus_log("hosting the editor bus at %s (instance 1)%s", B.paths.tcp ? B.paths.port : B.paths.sock,
            B.auto_deny ? "; clients: auto-deny" : B.auto_accept ? "; clients: auto-accept" : headless_enabled() ? "; clients: denied (headless, no --mcp-auto-accept)" : "");
    if (B.auto_accept) bus_log("--mcp-auto-accept: bus clients get their scopes without a dialog");
    return 1;
}

static void connect_or_host(void) {
    if (!B.paths.dir[0] && !edbus_paths_init(&B.paths, B.dir[0] ? B.dir : NULL)) {
        bus_log("no usable bus directory; the editor bus is off");
        B.want = 0;
        return;
    }
    if (try_join() || try_host()) return;
    // a host is starting, or just left: try again shortly (jittered so editors don't collide)
    B.retry_at = now_s() + 0.15 + (double) (getpid() % 7) * 0.05;
}

// process exit without leaving the page (window closed): the next host must not find our
// socket/token files (the lock goes with the process anyway)
static void bus_atexit(void) {
    if (B.mode == BUS_HOST) {
        edbus_close_fd(B.listen_fd);
        edbus_unlink(&B.paths);
    }
}

void ed_bus_start(void) {
#ifdef __EMSCRIPTEN__
    B.want = 0;
    web_start();
#else
    static int registered;
    if (!registered) {
        registered = 1;
        atexit(bus_atexit);
    }
    read_env_policy();
    if (!B.want || B.mode != BUS_OFF) return;
    connect_or_host();
#endif
}

void ed_bus_stop(void) {
    web_stop();
    if (B.mode == BUS_HOST) {
        for (int i = 0; i < B.peer_count; i++) peer_free(B.peers[i]);
        B.peer_count = 0;
        for (int i = 0; i < BUS_MAX_FWD; i++) cJSON_Delete(B.fwd[i].orig_id);
        memset(B.fwd, 0, sizeof(B.fwd));
        edbus_close_fd(B.listen_fd);
        edbus_unlink(&B.paths);
        edbus_lock_release(B.lock_fd);
        B.listen_fd = B.lock_fd = -1;
        bus_log("stopped hosting the editor bus");
    } else if (B.mode == BUS_CLIENT) {
        edbus_conn_close(&B.up);
    }
    cJSON_Delete(B.clip);
    B.clip = NULL;
    B.mode = BUS_OFF;
}

int ed_bus_active(void) { return B.mode != BUS_OFF; }

void ed_bus_toggle(void) {
    if (B.want) {
        B.want = 0;
        ed_bus_stop();
        editor_message("Editor bus off.");
    } else {
        B.want = 1;
        ed_bus_start();
        char msg[700];
        snprintf(msg, sizeof(msg), B.mode == BUS_HOST ? "Editor bus on: hosting at %s" : B.mode == BUS_CLIENT ? "Editor bus on: joined %s" : "Editor bus: waiting for %s",
                 B.paths.tcp ? B.paths.port : B.paths.sock);
        editor_message(msg);
    }
    ed_toolbar_show(); // the menu item's label
}

void ed_bus_notify_projects(void) { B.projects_dirty = 1; }
void ed_bus_notify_change(EditorProject *p) { (void) p; } // polled (revision) in announce()
void ed_bus_notify_selection(EditorProject *p) { (void) p; }

void ed_bus_clipboard_set(const cJSON *objects) {
    if (g_web_on && objects) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "t", "clipboard");
        cJSON_AddItemToObject(m, "objects", cJSON_Duplicate(objects, 1));
        web_post(m);
    }
    if (B.mode == BUS_OFF || !objects) return;
    if (B.mode == BUS_HOST) {
        cJSON_Delete(B.clip);
        B.clip = cJSON_Duplicate(objects, 1);
        cJSON *ev = make_ev("clipboard");
        cJSON_AddNumberToObject(ev, "count", cJSON_GetArraySize(objects));
        cJSON_AddItemToObject(ev, "objects", cJSON_Duplicate(objects, 1));
        broadcast_ev(ev, BUS_EV_CLIPBOARD, -1);
    } else {
        cJSON *ev = make_ev("clipboard");
        cJSON_AddItemToObject(ev, "objects", cJSON_Duplicate(objects, 1));
        edbus_send(&B.up, ev);
        cJSON_Delete(ev);
    }
}

// change / selection / project-list events of this instance's projects
static void announce(void) {
    for (size_t i = 0; i < g_ed.project_count; i++) {
        EditorProject *p = g_ed.projects[i];
        char ref[32];
        project_ref(p, ref, sizeof(ref));
        if (p->bus_revision != p->revision) {
            p->bus_revision = p->revision;
            cJSON *e = make_ev("change");
            cJSON_AddStringToObject(e, "project", ref);
            cJSON_AddNumberToObject(e, "revision", p->revision);
            cJSON_AddNumberToObject(e, "objects", (double) p->count);
            cJSON_AddBoolToObject(e, "dirty", p->dirty);
            if (B.mode == BUS_HOST) broadcast_ev(e, BUS_EV_CHANGE, -1);
            else {
                edbus_send(&B.up, e);
                cJSON_Delete(e);
            }
            B.projects_dirty = 1; // dirty / counts in the lists
        }
        const uint32_t sel = p->selected ? p->selected->uuid : 0;
        if (p->bus_selected != sel) {
            p->bus_selected = sel;
            cJSON *e = make_ev("selection");
            cJSON_AddStringToObject(e, "project", ref);
            if (sel) cJSON_AddNumberToObject(e, "uuid", sel);
            else cJSON_AddNullToObject(e, "uuid");
            if (B.mode == BUS_HOST) broadcast_ev(e, BUS_EV_SELECTION, -1);
            else {
                edbus_send(&B.up, e);
                cJSON_Delete(e);
            }
        }
    }
    if (!B.projects_dirty) return;
    B.projects_dirty = 0;
    if (B.mode == BUS_HOST) {
        cJSON *e = make_ev("projects");
        cJSON_AddItemToObject(e, "instances", instances_json());
        broadcast_ev(e, BUS_EV_PROJECTS, -1);
    } else {
        cJSON *e = make_ev("projects");
        cJSON_AddItemToObject(e, "projects", local_projects());
        edbus_send(&B.up, e);
        cJSON_Delete(e);
    }
}

void ed_bus_poll(void) {
    web_poll();
    if (!B.want) return;
    if (B.mode == BUS_OFF) {
        if (now_s() >= B.retry_at) connect_or_host();
        if (B.mode == BUS_OFF) return;
    }
    if (B.mode == BUS_CLIENT) {
        edbus_read(&B.up);
        int bad = 0;
        cJSON *m;
        while ((m = edbus_next(&B.up, &bad))) {
            client_message(m);
            cJSON_Delete(m);
        }
        if (B.up.closed || bad) {
            edbus_conn_close(&B.up);
            B.mode = BUS_OFF;
            bus_log("lost the editor bus host: taking over or reconnecting");
            B.retry_at = now_s() + 0.05 + (double) (getpid() % 7) * 0.05;
            return;
        }
        announce();
        edbus_flush(&B.up);
        return;
    }
    // host: new peers, messages, events, writes
    for (int fd; B.peer_count < BUS_MAX_PEERS && (fd = edbus_accept(B.listen_fd)) >= 0;) {
        Peer *pe = calloc(1, sizeof(Peer));
        if (!pe) {
            edbus_close_fd(fd);
            break;
        }
        edbus_conn_init(&pe->conn, fd);
        pe->id = B.next_peer++;
        B.peers[B.peer_count++] = pe;
    }
    for (int i = 0; i < B.peer_count; i++) {
        Peer *pe = B.peers[i];
        edbus_read(&pe->conn);
        int bad = 0;
        cJSON *m;
        while (!pe->conn.closed && (m = edbus_next(&pe->conn, &bad))) process_message(pe, m);
        if (bad) pe->conn.closed = 1;
    }
    announce();
    for (int i = B.peer_count - 1; i >= 0; i--) {
        Peer *pe = B.peers[i];
        if (!pe->conn.closed) edbus_flush(&pe->conn);
        if (pe->conn.closed && pe->conn.out_off >= pe->conn.out_len) drop_peer(i);
        else if (pe->conn.closed) drop_peer(i);
    }
}
