// shader_graph_ui.c - v10.0.7's shader graph node canvas editor (shader_graph_ui.h)
//
// v10's sgEditor (svelte-upgo13, deob-10.0.7.js:122896-124145) as markup with inline handlers. The node catalogue
// (titles, categories, descriptions, ports, param widgets) and the Templates menu are v10's own data
// (shader_graph_defs.inc, tools/modshader/gen_editor.cjs). The layout constants are iïïiiìï's (:122303): a node is
// 210 px wide, a 26 px header, 24 px rows (outputs, then params, then inputs), 6 px under the last.
//
// v10 draws the wires as an inline <svg>; here they are one SVG image (data: URL) over the nodes' bounds, and a click
// on a wire is hit-tested against the curves in C. Pointer capture becomes the held-buttons bit of each mousemove
// (a release outside the canvas ends the drag on the next move). Two-finger pinch is not ported. Pointer offsets
// are divided by v10's getBoundingClientRect / offsetWidth ratio (canvas_scale), the window's --ui-scale.
#include <shader_graph_ui.h>
#include <mod_shader.h>
#include <cJSON.h>
#include <i18n.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "shader_graph_defs.inc"

#define NODE_W 210
#define HEADER_H 26
#define ROW_H 24
#define HISTORY_MAX 200          // :122940
#define COALESCE_MS 800.0        // :122940
#define CHANGE_DEBOUNCE_MS 100.0 // :122943
#define MESSAGE_MS 2500.0        // :123156
#define MAX_SEL 256
#define ID_N 64
#define MAX_ERRORS 64

enum { DRAG_NONE, DRAG_PAN, DRAG_NODES, DRAG_WIRE };
enum { IO_NONE, IO_IMPORT, IO_EXPORT };

typedef struct {
    char node[ID_N], port[ID_N], dir[4], type[8];
} SgLink;

struct SgEditor {
    char prefix[24], call[64], kind[8];
    int readonly;
    SgChangeFn fn;
    void *user;
    cJSON *graph;
    char *last_json; // iÌïîíìï: the document the editor last set
    char **hist;
    int nhist, hidx;
    char hkey[16];
    double htime;
    double vx, vy, vk;
    int fit_due;
    char sel[MAX_SEL][ID_N];
    int nsel;
    int wire_sel;
    char wire_node[ID_N], wire_port[ID_N];
    // the pointer drag (iÌíïìîï)
    int drag, moved, detached;
    double sx, sy, ovx, ovy, lastx, lasty, wx, wy;
    SgLink dl;
    // the palette (iÌìììii): screen (canvas) and world position, the link it would connect
    int pal, plink, pactive;
    double psx, psy, pwx, pwy;
    SgLink pl;
    char psearch[64];
    int tpl_open;
    int io;
    char io_err[256];
    char msg[200];
    double msg_until;
    double change_due;
};

// ------------------------------------------------------------------ helpers

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6;
}

static const char *tr(const char *key, const char *english) { return i18n_tr(key, english); }

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static double jnum(const cJSON *o, const char *k, const double def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) ? v->valuedouble : def;
}

static void esc(StrBuf *sb, const char *s) { sb_append_esc(sb, s ? s : ""); }

// a JS string literal inside an attribute: '...' with \ and ' escaped, then HTML-escaped
static void js(StrBuf *sb, const char *s) {
    StrBuf t = {0};
    sb_append(&t, "'");
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '\\' || *p == '\'') sb_append_n(&t, "\\", 1);
        if (*p == '\n') sb_append(&t, "\\n");
        else sb_append_n(&t, p, 1);
    }
    sb_append(&t, "'");
    char *v = sb_finish(&t);
    sb_append_esc(sb, v);
    free(v);
}

static void fmt_num(char *out, const size_t n, const double v) {
    snprintf(out, n, "%.10g", fabs(v) < 1e-12 ? 0.0 : v);
}

// ------------------------------------------------------------------ the catalogue (iîiìíìí, :120989)

static cJSON *g_cat;

static const cJSON *catalogue(void) {
    if (!g_cat) g_cat = cJSON_Parse(k_shader_graph_defs);
    return g_cat;
}

static const cJSON *def_of(const char *type) {
    const cJSON *d;
    cJSON_ArrayForEach(d, cJSON_GetObjectItemCaseSensitive(catalogue(), "defs"))
        if (!strcmp(jstr(d, "type"), type)) return d;
    return NULL;
}

// iïììîíî (:121007): a def is offered for a kind unless its kinds leave the kind out
static int def_for_kind(const cJSON *d, const char *kind) {
    const cJSON *kinds = cJSON_GetObjectItemCaseSensitive(d, "kinds");
    if (!cJSON_IsArray(kinds)) return 1;
    const cJSON *k;
    cJSON_ArrayForEach(k, kinds) if (cJSON_IsString(k) && !strcmp(k->valuestring, kind)) return 1;
    return 0;
}

static const cJSON *def_list(const cJSON *d, const char *what, const char *kind) {
    char by[24];
    snprintf(by, sizeof(by), "%sByKind", what);
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(d, by), kind);
    return cJSON_IsArray(k) ? k : cJSON_GetObjectItemCaseSensitive(d, what);
}

static const char *port_color(const char *type) { // iìïiiiì (:122288)
    if (!strcmp(type, "float")) return "#8bc34a";
    if (!strcmp(type, "vec2")) return "#4fc3f7";
    if (!strcmp(type, "vec3")) return "#ffb74d";
    if (!strcmp(type, "vec4")) return "#ce93d8";
    return "#b0bec5";
}

static int numeric_type(const char *t) { // iïîiììì (:122287)
    return !strcmp(t, "float") || !strcmp(t, "vec2") || !strcmp(t, "vec3") || !strcmp(t, "vec4") || !strcmp(t, "any");
}

static int compatible(const char *a, const char *b) { return !strcmp(a, b) || (numeric_type(a) && numeric_type(b)); } // iíïíîíï

static int type_width(const char *t) { return !strcmp(t, "vec2") ? 2 : !strcmp(t, "vec3") ? 3 : !strcmp(t, "vec4") ? 4 : 1; }

// ------------------------------------------------------------------ the layout (iïïiiìï, :122303)

typedef struct {
    const cJSON *node, *def;
    double x, y, h;
    const cJSON *outs, *params, *ins; // the def's lists (NULL without a def)
    int nout, nparam, nin;
} Layout;

static Layout layout_of(const SgEditor *ed, const cJSON *node) {
    Layout l = {0};
    l.node = node;
    l.def = def_of(jstr(node, "type"));
    l.x = jnum(node, "x", 0);
    l.y = jnum(node, "y", 0);
    if (l.def) {
        l.outs = cJSON_GetObjectItemCaseSensitive(l.def, "outputs");
        l.params = def_list(l.def, "params", ed->kind);
        l.ins = def_list(l.def, "inputs", ed->kind);
        l.nout = cJSON_GetArraySize(l.outs);
        l.nparam = cJSON_GetArraySize(l.params);
        l.nin = cJSON_GetArraySize(l.ins);
    }
    const int rows = l.nout + l.nparam + l.nin;
    l.h = HEADER_H + ROW_H * (rows > 1 ? rows : 1) + 6;
    return l;
}

// a port's y inside its node: outputs, then params, then inputs; -1 when the node has no such port
static double port_y(const Layout *l, const char *name, const int out) {
    int i = 0;
    const cJSON *p;
    cJSON_ArrayForEach(p, l->outs) {
        if (out && !strcmp(jstr(p, "name"), name)) return HEADER_H + ROW_H * i + 12;
        i++;
    }
    i = l->nout + l->nparam;
    cJSON_ArrayForEach(p, l->ins) {
        if (!out && !strcmp(jstr(p, "name"), name)) return HEADER_H + ROW_H * i + 12;
        i++;
    }
    return -1;
}

static const cJSON *port_def(const Layout *l, const char *name, const int out) {
    const cJSON *p;
    cJSON_ArrayForEach(p, (out ? l->outs : l->ins)) if (!strcmp(jstr(p, "name"), name)) return p;
    return NULL;
}

static const char *first_output(const SgEditor *ed, const char *node_id);

// ------------------------------------------------------------------ the graph

static cJSON *nodes_of(const SgEditor *ed) { return cJSON_GetObjectItemCaseSensitive(ed->graph, "nodes"); }

static cJSON *node_by_id(const SgEditor *ed, const char *id) {
    cJSON *n;
    cJSON_ArrayForEach(n, nodes_of(ed)) if (!strcmp(jstr(n, "id"), id)) return n;
    return NULL;
}

static const char *first_output(const SgEditor *ed, const char *node_id) {
    const cJSON *n = node_by_id(ed, node_id);
    const cJSON *d = n ? def_of(jstr(n, "type")) : NULL;
    const cJSON *o = d ? cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(d, "outputs"), 0) : NULL;
    return o ? jstr(o, "name") : "";
}

// iìïìïïï (:122528)
static int graph_doc(const cJSON *g) {
    return cJSON_IsObject(g) && jnum(g, "v", 0) == 1 && cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(g, "nodes")) &&
           cJSON_IsString(cJSON_GetObjectItemCaseSensitive(g, "output"));
}

// iìíïïìí (:122361-122424): the kind's starting graph
static cJSON *default_graph(const char *kind) {
    const char *nodes = !strcmp(kind, "surface") ?
        "{\"id\":\"base1\",\"type\":\"baseColor\",\"x\":120,\"y\":90},{\"id\":\"out\",\"type\":\"output\",\"x\":420,\"y\":80,"
        "\"inputs\":{\"color\":{\"node\":\"base1\"},\"alpha\":{\"value\":1}}}" :
        !strcmp(kind, "paint") ?
        "{\"id\":\"color1\",\"type\":\"color\",\"x\":120,\"y\":90,\"params\":{\"value\":\"#ff8800\"}},{\"id\":\"out\",\"type\":\"output\","
        "\"x\":420,\"y\":80,\"inputs\":{\"color\":{\"node\":\"color1\"}}}" :
        "{\"id\":\"screenUV1\",\"type\":\"screenUV\",\"x\":40,\"y\":90},{\"id\":\"sceneColor1\",\"type\":\"sceneColor\",\"x\":220,\"y\":90,"
        "\"inputs\":{\"uv\":{\"node\":\"screenUV1\"}}},{\"id\":\"out\",\"type\":\"output\",\"x\":420,\"y\":80,\"inputs\":{\"color\":{\"node\":\"sceneColor1\"}}}";
    char buf[700];
    snprintf(buf, sizeof(buf), "{\"v\":1,\"kind\":\"%s\",\"nodes\":[%s],\"output\":\"out\"}", kind, nodes);
    return cJSON_Parse(buf);
}

static int selected(const SgEditor *ed, const char *id) {
    for (int i = 0; i < ed->nsel; i++) if (!strcmp(ed->sel[i], id)) return 1;
    return 0;
}

static void sel_clear(SgEditor *ed) { ed->nsel = 0; }

static void sel_add(SgEditor *ed, const char *id) {
    if (selected(ed, id) || ed->nsel >= MAX_SEL) return;
    snprintf(ed->sel[ed->nsel++], ID_N, "%s", id);
}

static void sel_remove(SgEditor *ed, const char *id) {
    for (int i = 0; i < ed->nsel; i++)
        if (!strcmp(ed->sel[i], id)) {
            memmove(ed->sel[i], ed->sel[i + 1], (size_t) (ed->nsel - i - 1) * ID_N);
            ed->nsel--;
            return;
        }
}

// ------------------------------------------------------------------ history and onChange (:122925-122952)

static void history_reset(SgEditor *ed, const char *json) {
    for (int i = 0; i < ed->nhist; i++) free(ed->hist[i]);
    ed->nhist = 0;
    ed->hidx = -1;
    if (!ed->hist) ed->hist = calloc(HISTORY_MAX + 1, sizeof(char *));
    if (ed->hist) {
        ed->hist[0] = strdup(json);
        ed->nhist = 1;
        ed->hidx = 0;
    }
    ed->hkey[0] = 0;
    ed->htime = 0;
}

// iÍíîíïí: the current document onto the history, coalesced with the last entry of the same key within 800 ms
static void history_push(SgEditor *ed, const char *key) {
    if (!ed->hist || !ed->last_json) return;
    const double t = now_ms();
    if (key[0] && !strcmp(key, ed->hkey) && t - ed->htime < COALESCE_MS && ed->hidx >= 0) {
        free(ed->hist[ed->hidx]);
        ed->hist[ed->hidx] = strdup(ed->last_json);
    } else {
        for (int i = ed->hidx + 1; i < ed->nhist; i++) free(ed->hist[i]);
        ed->nhist = ed->hidx + 1;
        if (ed->hidx < 0 || strcmp(ed->hist[ed->hidx], ed->last_json)) {
            ed->hist[ed->nhist++] = strdup(ed->last_json);
            if (ed->nhist > HISTORY_MAX) {
                free(ed->hist[0]);
                memmove(ed->hist, ed->hist + 1, (size_t) (ed->nhist - 1) * sizeof(char *));
                ed->nhist--;
            }
            ed->hidx = ed->nhist - 1;
        }
    }
    snprintf(ed->hkey, sizeof(ed->hkey), "%s", key);
    ed->htime = t;
}

// iÌiîíìi: the graph is now `g` (taken); onChange 100 ms on; onto the history unless key is NULL
static void set_graph(SgEditor *ed, cJSON *g, const char *key) {
    if (g != ed->graph) {
        cJSON_Delete(ed->graph);
        ed->graph = g;
    }
    free(ed->last_json);
    ed->last_json = cJSON_PrintUnformatted(ed->graph);
    ed->change_due = now_ms() + CHANGE_DEBOUNCE_MS;
    if (key) history_push(ed, key);
}

// iÍîìíîi: a history entry back (undo / redo)
static void restore(SgEditor *ed, const char *json) {
    cJSON *g = cJSON_Parse(json);
    if (!g) return;
    cJSON_Delete(ed->graph);
    ed->graph = g;
    free(ed->last_json);
    ed->last_json = strdup(json);
    ed->change_due = now_ms() + CHANGE_DEBOUNCE_MS;
    ed->hkey[0] = 0;
    ed->htime = 0;
}

// ------------------------------------------------------------------ graph edits (:122425-122452)

static cJSON *edit_copy(const SgEditor *ed) { return cJSON_Duplicate(ed->graph, 1); }

static cJSON *node_in(cJSON *g, const char *id) {
    cJSON *n;
    cJSON_ArrayForEach(n, cJSON_GetObjectItemCaseSensitive(g, "nodes")) if (!strcmp(jstr(n, "id"), id)) return n;
    return NULL;
}

static cJSON *obj_member(cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsObject(v)) {
        cJSON_DeleteItemFromObjectCaseSensitive(o, k);
        v = cJSON_AddObjectToObject(o, k);
    }
    return v;
}

static void put(cJSON *o, const char *k, cJSON *v) {
    if (cJSON_GetObjectItemCaseSensitive(o, k)) cJSON_ReplaceItemInObjectCaseSensitive(o, k, v);
    else cJSON_AddItemToObject(o, k, v);
}

// iíîîìíî: to.toPort = {node: from, port} (no port: the first output)
static void connect(cJSON *g, const char *from, const char *port, const char *to, const char *to_port) {
    cJSON *n = node_in(g, to);
    if (!n) return;
    cJSON *link = cJSON_CreateObject();
    cJSON_AddStringToObject(link, "node", from);
    if (port && port[0]) cJSON_AddStringToObject(link, "port", port);
    put(obj_member(n, "inputs"), to_port, link);
}

// iíiiíîi: the input unset
static void disconnect(cJSON *g, const char *node, const char *port) {
    cJSON *n = node_in(g, node);
    cJSON *in = n ? cJSON_GetObjectItemCaseSensitive(n, "inputs") : NULL;
    if (cJSON_IsObject(in)) cJSON_DeleteItemFromObjectCaseSensitive(in, port);
}

static const cJSON *link_of(const cJSON *node, const char *port) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(node, "inputs"), port);
    return cJSON_IsObject(v) && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(v, "node")) ? v : NULL;
}

// would from -> to make a loop (iÌîíiíì's walk, :123290-123310)
static int reaches(const SgEditor *ed, const char *start, const char *target) {
    if (!strcmp(start, target)) return 1;
    const int count = cJSON_GetArraySize(nodes_of(ed));
    char (*stack)[ID_N] = calloc((size_t) count * 8 + 8, ID_N);
    char (*seen)[ID_N] = calloc((size_t) count + 1, ID_N);
    int sp = 0, ns = 0, found = 0;
    if (!stack || !seen) {
        free(stack);
        free(seen);
        return 0;
    }
    snprintf(stack[sp++], ID_N, "%s", start);
    while (sp && !found) {
        char cur[ID_N];
        snprintf(cur, sizeof(cur), "%s", stack[--sp]);
        if (!strcmp(cur, target)) {
            found = 1;
            break;
        }
        int dup = 0;
        for (int i = 0; i < ns && !dup; i++) dup = !strcmp(seen[i], cur);
        if (dup || ns > count) continue;
        snprintf(seen[ns++], ID_N, "%s", cur);
        const cJSON *n = node_by_id(ed, cur);
        const cJSON *in;
        cJSON_ArrayForEach(in, cJSON_GetObjectItemCaseSensitive(n, "inputs")) {
            const cJSON *src = cJSON_GetObjectItemCaseSensitive(in, "node");
            if (cJSON_IsString(src) && sp < count * 8 + 8) snprintf(stack[sp++], ID_N, "%s", src->valuestring);
        }
    }
    free(stack);
    free(seen);
    return found;
}

// iÌîíiíì: why from (of type ft) cannot feed to (of type tt), NULL when it can
static const char *connect_error(const SgEditor *ed, const char *from, const char *ft, const char *to, const char *tt, char *buf, const size_t n) {
    if (!strcmp(from, to)) return tr("shaderGraph.err.selfConnect", "A node cannot feed itself");
    if (!compatible(ft, tt)) {
        const char *f = tr("shaderGraph.err.incompatible", "Incompatible types: {0} \xe2\x86\x92 {1}");
        StrBuf sb = {0};
        sb_append(&sb, f);
        char *a = sb_finish(&sb);
        StrBuf b1 = {0};
        sb_append_replace_all(&b1, a, "{0}", ft);
        char *a1 = sb_finish(&b1);
        StrBuf b2 = {0};
        sb_append_replace_all(&b2, a1, "{1}", tt);
        char *a2 = sb_finish(&b2);
        snprintf(buf, n, "%s", a2);
        free(a);
        free(a1);
        free(a2);
        return buf;
    }
    if (reaches(ed, from, to)) return tr("shaderGraph.err.cycle", "That connection would create a loop");
    return NULL;
}

// iîîïîii (:122348): the type's characters (at most 12) and the lowest free number
static void new_id(const SgEditor *ed, const char *type, char *out, const size_t n) {
    char base[16];
    size_t k = 0;
    for (const char *p = type; *p && k < 12; p++)
        if (isalnum((unsigned char) *p) || *p == '_' || *p == '-') base[k++] = *p;
    base[k] = 0;
    if (!k) snprintf(base, sizeof(base), "n");
    for (int i = 1;; i++) {
        snprintf(out, n, "%s%d", base, i);
        if (!node_by_id(ed, out)) return;
    }
}

// ------------------------------------------------------------------ the message, the canvas

static void message(SgEditor *ed, const char *text) {
    snprintf(ed->msg, sizeof(ed->msg), "%s", text);
    ed->msg_until = now_ms() + MESSAGE_MS;
}

static HtmlElem el(const SgEditor *ed, const char *suffix) {
    char id[64];
    snprintf(id, sizeof(id), "%s%s", ed->prefix, suffix);
    return html_dom_by_id(id);
}

static void canvas_size(const SgEditor *ed, double *w, double *h) { // clientWidth / clientHeight || 800 / 400 (layout px)
    const HtmlElem c = el(ed, "Canvas");
    const float cw = c ? html_dom_offset_width(c) : 0, ch = c ? html_dom_offset_height(c) : 0;
    *w = cw > 0 ? cw : 800;
    *h = ch > 0 ? ch : 400;
}

// iÌiíïïî (:123076-123079): the canvas's on-screen scale, getBoundingClientRect().width / offsetWidth (the window's
// scale(--ui-scale) wrapper), 1 when it has no width
static double canvas_scale(const SgEditor *ed, const HtmlRect *r) {
    const HtmlElem c = el(ed, "Canvas");
    const float ow = c ? html_dom_offset_width(c) : 0;
    return ow > 0 && r->w > 0 ? r->w / ow : 1.0;
}

static int canvas_rect(const SgEditor *ed, HtmlRect *r) {
    const HtmlElem c = el(ed, "Canvas");
    return c && html_dom_get_rect(c, r);
}

// iÎïìîií: viewport -> canvas px; iIììïiî: viewport -> world
static void to_canvas(const SgEditor *ed, const double x, const double y, double *cx, double *cy) {
    HtmlRect r = {0};
    canvas_rect(ed, &r);
    const double k = canvas_scale(ed, &r);
    *cx = (x - r.x) / k;
    *cy = (y - r.y) / k;
}

static void to_world(const SgEditor *ed, const double x, const double y, double *wx, double *wy) {
    double cx, cy;
    to_canvas(ed, x, y, &cx, &cy);
    *wx = (cx - ed->vx) / ed->vk;
    *wy = (cy - ed->vy) / ed->vk;
}

static void focus_canvas(const SgEditor *ed) {
    const HtmlElem c = el(ed, "Canvas");
    if (c) html_dom_focus(c);
}

// iÌiìíîí (:123081-123132): the nodes' bounds into the canvas, 80 px margin, scale 0.2-1.25
static void fit(SgEditor *ed) {
    double minx = INFINITY, miny = INFINITY, maxx = -INFINITY, maxy = -INFINITY;
    int any = 0;
    const cJSON *n;
    cJSON_ArrayForEach(n, nodes_of(ed)) {
        const Layout l = layout_of(ed, n);
        minx = fmin(minx, l.x);
        miny = fmin(miny, l.y);
        maxx = fmax(maxx, l.x + NODE_W);
        maxy = fmax(maxy, l.y + l.h);
        any = 1;
    }
    if (!any) return;
    double cw, ch;
    canvas_size(ed, &cw, &ch);
    const double bw = maxx - minx, bh = maxy - miny;
    double k = fmin((cw - 80) / bw, (ch - 80) / bh);
    k = fmin(1.25, fmax(0.2, k));
    ed->vk = k;
    ed->vx = (cw - bw * k) / 2 - minx * k;
    ed->vy = (ch - bh * k) / 2 - miny * k;
}

// iÍïiìïí (:123141-123152): zoom by factor about the canvas point (cx, cy), scale 0.2-3
static void zoom_at(SgEditor *ed, const double cx, const double cy, const double factor) {
    const double k = fmin(3.0, fmax(0.2, ed->vk * factor));
    const double r = k / ed->vk;
    ed->vx = cx - (cx - ed->vx) * r;
    ed->vy = cy - (cy - ed->vy) * r;
    ed->vk = k;
}

// ------------------------------------------------------------------ wires (:123015-123047)

typedef struct {
    double x1, y1, x2, y2;
    const char *color;
    char to[ID_N], to_port[ID_N];
} Wire;

static int wires_of(const SgEditor *ed, Wire **out) {
    int n = 0, cap = 0;
    Wire *w = NULL;
    const cJSON *node;
    cJSON_ArrayForEach(node, nodes_of(ed)) {
        const Layout to = layout_of(ed, node);
        if (!to.def) continue;
        const cJSON *in;
        cJSON_ArrayForEach(in, cJSON_GetObjectItemCaseSensitive(node, "inputs")) {
            const cJSON *src = cJSON_GetObjectItemCaseSensitive(in, "node");
            if (!cJSON_IsObject(in) || !cJSON_IsString(src)) continue;
            const cJSON *from_node = node_by_id(ed, src->valuestring);
            const double ty = port_y(&to, in->string, 0);
            if (!from_node || ty < 0) continue;
            const Layout from = layout_of(ed, from_node);
            const char *pn = jstr(in, "port");
            const cJSON *op = pn[0] ? port_def(&from, pn, 1) : NULL;
            if (!op) op = cJSON_GetArrayItem(from.outs, 0);
            if (!op) continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                Wire *g = realloc(w, (size_t) cap * sizeof(Wire));
                if (!g) break;
                w = g;
            }
            Wire *e = &w[n++];
            e->x1 = from.x + NODE_W;
            e->y1 = from.y + port_y(&from, jstr(op, "name"), 1);
            e->x2 = to.x;
            e->y2 = to.y + ty;
            e->color = port_color(jstr(op, "type"));
            snprintf(e->to, ID_N, "%s", jstr(node, "id"));
            snprintf(e->to_port, ID_N, "%s", in->string);
        }
    }
    *out = w;
    return n;
}

static void bezier_at(const double x1, const double y1, const double x2, const double y2, const double t, double *x, double *y) {
    const double d = fmax(40.0, fabs(x2 - x1) / 2.0);
    const double cx1 = x1 + d, cx2 = x2 - d, u = 1.0 - t;
    *x = u * u * u * x1 + 3 * u * u * t * cx1 + 3 * u * t * t * cx2 + t * t * t * x2;
    *y = u * u * u * y1 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y2;
}

// iïïíîíî (:122453)
static void wire_path(StrBuf *sb, const double x1, const double y1, const double x2, const double y2) {
    const double d = fmax(40.0, fabs(x2 - x1) / 2.0);
    char a[6][32];
    fmt_num(a[0], 32, x1);
    fmt_num(a[1], 32, y1);
    fmt_num(a[2], 32, x1 + d);
    fmt_num(a[3], 32, x2 - d);
    fmt_num(a[4], 32, x2);
    fmt_num(a[5], 32, y2);
    sb_appendf(sb, "M %s %s C %s %s, %s %s, %s %s", a[0], a[1], a[2], a[1], a[3], a[5], a[4], a[5]);
}

// the temporary wire of a drag (iÍíîïìï, :123517-123532)
static int temp_wire(const SgEditor *ed, Wire *w) {
    if (ed->drag != DRAG_WIRE) return 0;
    const cJSON *n = node_by_id(ed, ed->dl.node);
    if (!n) return 0;
    const Layout l = layout_of(ed, n);
    const int out = !strcmp(ed->dl.dir, "out");
    const double py = port_y(&l, ed->dl.port, out);
    if (py < 0) return 0;
    const double px = out ? l.x + NODE_W : l.x;
    if (out) {
        w->x1 = px, w->y1 = l.y + py, w->x2 = ed->wx, w->y2 = ed->wy;
    } else {
        w->x1 = ed->wx, w->y1 = ed->wy, w->x2 = px, w->y2 = l.y + py;
    }
    w->color = port_color(ed->dl.type);
    return 1;
}

// the wires as one SVG image over their bounds (v10's svg.sgWires with its paths)
static void wires_img(const SgEditor *ed, StrBuf *style, StrBuf *src) {
    Wire *w = NULL;
    int n = wires_of(ed, &w);
    Wire tmp;
    const int has_tmp = temp_wire(ed, &tmp);
    double minx = INFINITY, miny = INFINITY, maxx = -INFINITY, maxy = -INFINITY;
    for (int i = 0; i < n + has_tmp; i++) {
        const Wire *e = i < n ? &w[i] : &tmp;
        const double d = fmax(40.0, fabs(e->x2 - e->x1) / 2.0);
        minx = fmin(minx, fmin(e->x1, e->x2 - d));
        maxx = fmax(maxx, fmax(e->x2, e->x1 + d));
        minx = fmin(minx, e->x2);
        maxx = fmax(maxx, e->x1);
        miny = fmin(miny, fmin(e->y1, e->y2));
        maxy = fmax(maxy, fmax(e->y1, e->y2));
    }
    if (n + has_tmp == 0) minx = miny = 0, maxx = maxy = 1;
    minx = floor(minx - 4), miny = floor(miny - 4), maxx = ceil(maxx + 4), maxy = ceil(maxy + 4);
    const double bw = maxx - minx, bh = maxy - miny;
    sb_appendf(style, "left:%gpx;top:%gpx;width:%gpx;height:%gpx", minx, miny, bw, bh);
    StrBuf svg = {0};
    sb_appendf(&svg, "<svg xmlns='http://www.w3.org/2000/svg' width='%g' height='%g' viewBox='%g %g %g %g'>", bw, bh, minx, miny, bw, bh);
    for (int i = 0; i < n + has_tmp; i++) {
        const Wire *e = i < n ? &w[i] : &tmp;
        const int sel = i < n && ed->wire_sel && !strcmp(e->to, ed->wire_node) && !strcmp(e->to_port, ed->wire_port);
        sb_append(&svg, "<path d='");
        wire_path(&svg, e->x1, e->y1, e->x2, e->y2);
        // .sgWire: fill none, stroke-width 2, opacity 0.85; .selected: #fff, 3, 1; .sgWireTemp: dash 6 4
        sb_appendf(&svg, "' fill='none' stroke='%s' stroke-width='%d' stroke-opacity='%s'%s/>", sel ? "#fff" : e->color, sel ? 3 : 2,
                   sel ? "1" : "0.85", i >= n ? " stroke-dasharray='6 4'" : "");
    }
    sb_append(&svg, "</svg>");
    char *text = sb_finish(&svg);
    sb_append(src, "data:image/svg+xml,");
    for (const unsigned char *p = (const unsigned char *) text; *p; p++) {
        if (isalnum(*p) || strchr(" -_.'=/:,", *p)) sb_append_n(src, (const char *) p, 1);
        else sb_appendf(src, "%%%02X", *p);
    }
    free(text);
    free(w);
}

// a wire within 6 world px of (x, y) (the 12 px sgWireHit stroke)
static int wire_hit(const SgEditor *ed, const double x, const double y, char *node, char *port) {
    Wire *w = NULL;
    const int n = wires_of(ed, &w);
    int hit = 0;
    for (int i = n - 1; i >= 0 && !hit; i--) {
        double px, py;
        bezier_at(w[i].x1, w[i].y1, w[i].x2, w[i].y2, 0, &px, &py);
        for (int s = 1; s <= 32 && !hit; s++) {
            double qx, qy;
            bezier_at(w[i].x1, w[i].y1, w[i].x2, w[i].y2, s / 32.0, &qx, &qy);
            const double dx = qx - px, dy = qy - py, len2 = dx * dx + dy * dy;
            double t = len2 > 0 ? ((x - px) * dx + (y - py) * dy) / len2 : 0;
            t = fmin(1, fmax(0, t));
            const double ex = px + t * dx - x, ey = py + t * dy - y;
            if (ex * ex + ey * ey <= 36.0) {
                hit = 1;
                snprintf(node, ID_N, "%s", w[i].to);
                snprintf(port, ID_N, "%s", w[i].to_port);
            }
            px = qx, py = qy;
        }
    }
    free(w);
    return hit;
}

// ------------------------------------------------------------------ markup

static void handler(const SgEditor *ed, StrBuf *sb, const char *op) { sb_appendf(sb, "%s,'%s'", ed->call, op); }

// portState (iÎííïïî, :123315-123321): "ok" / "bad" against the wire being dragged
static const char *port_state(const SgEditor *ed, const char *node, const char *type, const int out) {
    if (ed->drag != DRAG_WIRE || (!strcmp(ed->dl.dir, "out")) == out) return "";
    char buf[160];
    const char *err = !out ? connect_error(ed, ed->dl.node, ed->dl.type, node, type, buf, sizeof(buf))
                           : connect_error(ed, node, type, ed->dl.node, ed->dl.type, buf, sizeof(buf));
    return err ? "bad" : "ok";
}

// the value editor (svelte-1i1uhp7 :122551-122640): a colour, a number with an optional slider, or a vector
static void value_widget(const SgEditor *ed, StrBuf *sb, const char *type, const cJSON *value, const cJSON *min, const cJSON *max,
                         const cJSON *step, const int slider, const char *op, const char *node, const char *name, const char *idbase) {
    const char *dis = ed->readonly ? " disabled" : "";
    if (!strcmp(type, "color")) {
        const char *v = cJSON_IsString(value) && strlen(value->valuestring) == 7 && value->valuestring[0] == '#' ? value->valuestring : "#ffffff";
        sb_appendf(sb, "<span class=\"sgValue sgValueColor svelte-1i1uhp7\"><input type=\"color\" class=\"sgColorInput svelte-1i1uhp7\" id=\"%sC\" value=\"%s\"%s oninput=\"",
                   idbase, v, dis);
        handler(ed, sb, op);
        sb_append(sb, ",");
        js(sb, node);
        sb_append(sb, ",");
        js(sb, name);
        sb_append(sb, ",'color',this.value)\" onchange=\"");
        handler(ed, sb, "commit");
        sb_appendf(sb, ")\"> <input type=\"text\" class=\"sgNum sgHex svelte-1i1uhp7\" maxlength=\"7\" spellcheck=\"false\" id=\"%sH\" value=\"%s\"%s oninput=\"",
                   idbase, v, dis);
        handler(ed, sb, op);
        sb_append(sb, ",");
        js(sb, node);
        sb_append(sb, ",");
        js(sb, name);
        sb_append(sb, ",'hex',this.value)\" onchange=\"");
        handler(ed, sb, "commit");
        sb_append(sb, ")\"></span>");
        return;
    }
    const int width = type_width(type);
    char sv[32];
    if (cJSON_IsNumber(step)) fmt_num(sv, sizeof(sv), step->valuedouble);
    else snprintf(sv, sizeof(sv), "0.01");
    if (width == 1) {
        const cJSON *v0 = cJSON_IsArray(value) ? cJSON_GetArrayItem(value, 0) : value;
        const double v = cJSON_IsNumber(v0) && isfinite(v0->valuedouble) ? v0->valuedouble : 0;
        char vs[32];
        fmt_num(vs, sizeof(vs), v);
        sb_appendf(sb, "<span class=\"sgValue svelte-1i1uhp7\"><input type=\"number\" class=\"sgNum svelte-1i1uhp7\" id=\"%sN\" step=\"%s\" value=\"%s\"%s oninput=\"",
                   idbase, sv, vs, dis);
        handler(ed, sb, op);
        sb_append(sb, ",");
        js(sb, node);
        sb_append(sb, ",");
        js(sb, name);
        sb_append(sb, ",'num',this.value)\" onchange=\"");
        handler(ed, sb, "commit");
        sb_append(sb, ")\"> ");
        if (slider) {
            const double lo = cJSON_IsNumber(min) ? min->valuedouble : 0;
            const double hi = cJSON_IsNumber(max) ? max->valuedouble : fmax(1, 4 * fabs(v != 0 ? v : 1));
            char a[32], b[32], st[32];
            fmt_num(a, sizeof(a), lo);
            fmt_num(b, sizeof(b), hi);
            if (cJSON_IsNumber(step)) fmt_num(st, sizeof(st), step->valuedouble);
            else fmt_num(st, sizeof(st), (hi - lo) / 200.0);
            sb_appendf(sb, "<input type=\"range\" class=\"sgSlider svelte-1i1uhp7\" id=\"%sS\" min=\"%s\" max=\"%s\" step=\"%s\" value=\"%s\"%s oninput=\"",
                       idbase, a, b, st, vs, dis);
            handler(ed, sb, op);
            sb_append(sb, ",");
            js(sb, node);
            sb_append(sb, ",");
            js(sb, name);
            sb_append(sb, ",'num',this.value)\" onchange=\"");
            handler(ed, sb, "commit");
            sb_append(sb, ")\">");
        }
        sb_append(sb, "</span>");
        return;
    }
    static const char *const axis[] = {"x", "y", "z", "w"};
    sb_append(sb, "<span class=\"sgValue sgValueVec svelte-1i1uhp7\">");
    for (int i = 0; i < width; i++) {
        const cJSON *e = cJSON_IsArray(value) ? cJSON_GetArrayItem(value, i) : NULL;
        char vs[32];
        fmt_num(vs, sizeof(vs), cJSON_IsNumber(e) && isfinite(e->valuedouble) ? e->valuedouble : 0);
        sb_appendf(sb, "<input type=\"number\" class=\"sgNum sgNumVec svelte-1i1uhp7\" id=\"%s%d\" step=\"%s\" placeholder=\"%s\" title=\"%s\" value=\"%s\"%s oninput=\"",
                   idbase, i, sv, axis[i], axis[i], vs, dis);
        handler(ed, sb, op);
        sb_append(sb, ",");
        js(sb, node);
        sb_append(sb, ",");
        js(sb, name);
        sb_appendf(sb, ",'v%d',this.value)\" onchange=\"", i);
        handler(ed, sb, "commit");
        sb_append(sb, ")\">");
    }
    sb_append(sb, "</span>");
}

static const char *const STOP = " onmousedown=\"event.stopPropagation()\" onkeydown=\"event.stopPropagation()\"";

// one param row (:123672-123821)
static void param_row(const SgEditor *ed, StrBuf *sb, const cJSON *node, const cJSON *p, const double y, const char *idbase) {
    const char *name = jstr(p, "name"), *widget = jstr(p, "widget"), *id = jstr(node, "id");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(node, "params");
    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(params, name);
    const int unset = !cur || cJSON_IsNull(cur);
    const cJSON *val = unset ? cJSON_GetObjectItemCaseSensitive(p, "default") : cur; // iÍíîíïi
    const int optional = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "optional"));
    const int num = !strcmp(widget, "number") || !strcmp(widget, "int");
    const char *dis = ed->readonly ? " disabled" : "";
    const cJSON *min = cJSON_GetObjectItemCaseSensitive(p, "min"), *max = cJSON_GetObjectItemCaseSensitive(p, "max");
    sb_appendf(sb, "<div class=\"sgRow sgRowParam svelte-wbsbny\" role=\"presentation\" style=\"top:%gpx;height:24px\"%s><span class=\"sgPortLabel svelte-wbsbny\">", y, STOP);
    esc(sb, name);
    sb_append(sb, "</span> ");
    if (optional && unset && !num) { // the unset button (iîíîïìï): white, true or 0
        sb_appendf(sb, "<button type=\"button\" class=\"sgUnset svelte-wbsbny\" id=\"%sU\" title=\"", idbase);
        esc(sb, name);
        sb_appendf(sb, "\"%s onclick=\"", dis);
        handler(ed, sb, "unset");
        sb_append(sb, ",");
        js(sb, id);
        sb_append(sb, ",");
        js(sb, name);
        sb_append(sb, ",");
        js(sb, widget);
        sb_append(sb, ")\">\xe2\x80\x94</button>");
    } else if (optional && num) {
        char vs[32] = "";
        if (!unset && cJSON_IsNumber(val)) fmt_num(vs, sizeof(vs), val->valuedouble);
        sb_appendf(sb, "<input type=\"number\" class=\"sgText sgInt svelte-wbsbny\" placeholder=\"\xe2\x80\x94\" id=\"%sO\" value=\"%s\"", idbase, vs);
        if (cJSON_IsNumber(min)) sb_appendf(sb, " min=\"%g\"", min->valuedouble);
        if (cJSON_IsNumber(max)) sb_appendf(sb, " max=\"%g\"", max->valuedouble);
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(p, "step");
        sb_appendf(sb, " step=\"%g\"%s oninput=\"", !strcmp(widget, "int") ? 1.0 : cJSON_IsNumber(st) ? st->valuedouble : 0.01, dis);
        handler(ed, sb, "param");
        sb_append(sb, ",");
        js(sb, id);
        sb_append(sb, ",");
        js(sb, name);
        sb_appendf(sb, ",'%s',this.value)\" onchange=\"", !strcmp(widget, "int") ? "optint" : "optnum");
        handler(ed, sb, "commit");
        sb_append(sb, ")\">");
    } else if (!strcmp(widget, "enum")) {
        const char *v = cJSON_IsString(val) ? val->valuestring : "";
        sb_appendf(sb, "<select class=\"sgSelect svelte-wbsbny\" id=\"%sE\"%s onchange=\"", idbase, dis);
        handler(ed, sb, "param");
        sb_append(sb, ",");
        js(sb, id);
        sb_append(sb, ",");
        js(sb, name);
        sb_append(sb, ",'str',this.value);");
        handler(ed, sb, "commit");
        sb_append(sb, ")\">");
        const cJSON *o;
        cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(p, "options")) {
            if (!cJSON_IsString(o)) continue;
            sb_append(sb, "<option class=\"svelte-wbsbny\" value=\"");
            esc(sb, o->valuestring);
            sb_appendf(sb, "\"%s>", !strcmp(o->valuestring, v) ? " selected" : "");
            esc(sb, o->valuestring);
            sb_append(sb, "</option>");
        }
        sb_append(sb, "</select>");
    } else if (!strcmp(widget, "bool")) {
        sb_appendf(sb, "<input type=\"checkbox\" class=\"sgCheck svelte-wbsbny\" id=\"%sB\"%s%s onchange=\"", idbase, cJSON_IsTrue(val) ? " checked" : "", dis);
        handler(ed, sb, "param");
        sb_append(sb, ",");
        js(sb, id);
        sb_append(sb, ",");
        js(sb, name);
        sb_append(sb, ",'bool',this.checked);");
        handler(ed, sb, "commit");
        sb_append(sb, ")\">");
    } else if (!strcmp(widget, "string")) {
        sb_appendf(sb, "<input type=\"text\" class=\"sgText svelte-wbsbny\" spellcheck=\"false\" id=\"%sT\" value=\"", idbase);
        esc(sb, cJSON_IsString(val) ? val->valuestring : "");
        sb_appendf(sb, "\"%s oninput=\"", dis);
        handler(ed, sb, "param");
        sb_append(sb, ",");
        js(sb, id);
        sb_append(sb, ",");
        js(sb, name);
        sb_append(sb, ",'str',this.value)\" onchange=\"");
        handler(ed, sb, "commit");
        sb_append(sb, ")\">");
    } else if (!strcmp(widget, "int")) {
        sb_appendf(sb, "<input type=\"number\" class=\"sgText sgInt svelte-wbsbny\" step=\"1\" id=\"%sI\" value=\"%g\"", idbase, cJSON_IsNumber(val) ? val->valuedouble : 0.0);
        if (cJSON_IsNumber(min)) sb_appendf(sb, " min=\"%g\"", min->valuedouble);
        if (cJSON_IsNumber(max)) sb_appendf(sb, " max=\"%g\"", max->valuedouble);
        sb_appendf(sb, "%s oninput=\"", dis);
        handler(ed, sb, "param");
        sb_append(sb, ",");
        js(sb, id);
        sb_append(sb, ",");
        js(sb, name);
        sb_append(sb, ",'int',this.value)\" onchange=\"");
        handler(ed, sb, "commit");
        sb_append(sb, ")\">");
    } else {
        // the value editor: a param node's default / min / max take its type (:123771-123778)
        const char *vt = "float";
        if (!strcmp(jstr(node, "type"), "param") && (!strcmp(name, "default") || !strcmp(name, "min") || !strcmp(name, "max"))) {
            const char *t = jstr(params, "type");
            vt = !strcmp(name, "default") ? (t[0] ? t : "float") : "float";
        } else if (!strcmp(widget, "color")) vt = "color";
        else if (!strcmp(widget, "vec2") || !strcmp(widget, "vec3") || !strcmp(widget, "vec4")) vt = widget;
        value_widget(ed, sb, vt, val, min, max, cJSON_GetObjectItemCaseSensitive(p, "step"), cJSON_IsNumber(min) && cJSON_IsNumber(max), "param", id,
                     name, idbase);
    }
    if (optional && !unset && !num) { // the clear button (iïííïìí)
        sb_appendf(sb, " <button type=\"button\" class=\"sgUnset sgClear svelte-wbsbny\" title=\"\xc3\x97\" id=\"%sX\"%s onclick=\"", idbase, dis);
        handler(ed, sb, "clear");
        sb_append(sb, ",");
        js(sb, id);
        sb_append(sb, ",");
        js(sb, name);
        sb_append(sb, ")\">\xc3\x97</button>");
    }
    sb_append(sb, "</div>");
}

static void port_attrs(const SgEditor *ed, StrBuf *sb, const char *node, const char *name, const char *type, const char *dir) {
    sb_append(sb, " data-port=\"");
    esc(sb, name);
    sb_append(sb, "\" data-node=\"");
    esc(sb, node);
    sb_appendf(sb, "\" data-type=\"%s\" data-dir=\"%s\" title=\"", type, dir);
    esc(sb, name);
    sb_appendf(sb, ": %s\" onmousedown=\"event.stopPropagation();", type);
    if (!ed->readonly) {
        handler(ed, sb, "portDown");
        sb_append(sb, ",");
        js(sb, node);
        sb_append(sb, ",");
        js(sb, name);
        sb_appendf(sb, ",'%s')", dir);
    }
    sb_append(sb, "\"");
}

// one node (svelte-wbsbny, :123636-123912)
static void node_html(const SgEditor *ed, StrBuf *sb, const cJSON *node, const int index, const ModShaderGraphError *errs, const int nerr) {
    const Layout l = layout_of(ed, node);
    const char *id = jstr(node, "id");
    int has_error = 0;
    for (int i = 0; i < nerr && !has_error; i++) has_error = errs[i].node[0] && !strcmp(errs[i].node, id);
    const int is_out = !strcmp(id, jstr(ed->graph, "output"));
    const char *title = l.def ? jstr(l.def, "title") : jstr(node, "type");
    sb_appendf(sb, "<div class=\"sgNode svelte-wbsbny%s%s%s\" role=\"group\" id=\"%sN%d\" data-node=\"", selected(ed, id) ? " selected" : "",
               has_error ? " hasError" : "", is_out ? " isOutput" : "", ed->prefix, index);
    esc(sb, id);
    sb_appendf(sb, "\" style=\"left:%gpx;top:%gpx;width:%dpx;height:%gpx\" aria-label=\"", l.x, l.y, NODE_W, l.h);
    esc(sb, title[0] ? title : jstr(node, "type"));
    sb_append(sb, "\" onmousedown=\"");
    handler(ed, sb, "nodeDown");
    sb_append(sb, ",");
    js(sb, id);
    sb_append(sb, ")\"><div class=\"sgNodeHeader svelte-wbsbny\" style=\"height:26px\" title=\"");
    const char *desc = l.def ? jstr(l.def, "description") : "";
    esc(sb, desc[0] ? desc : jstr(node, "type"));
    sb_append(sb, "\"><span class=\"sgNodeTitle svelte-wbsbny\">");
    esc(sb, title[0] ? title : jstr(node, "type"));
    sb_append(sb, "</span> <span class=\"sgNodeId svelte-wbsbny\">");
    esc(sb, id);
    sb_append(sb, "</span></div> ");
    if (!l.def) {
        sb_append(sb, "<div class=\"sgRow sgRowUnknown svelte-wbsbny\" style=\"top:26px;height:24px\">");
        esc(sb, tr("shaderGraph.unknownNode", "Unknown node type"));
        sb_append(sb, "</div>");
    }
    sb_append(sb, " ");
    int row = 0;
    const cJSON *p;
    cJSON_ArrayForEach(p, l.outs) {
        const double y = HEADER_H + ROW_H * row++ + 12;
        const char *name = jstr(p, "name"), *type = jstr(p, "type");
        sb_appendf(sb, "<div class=\"sgRow sgRowOut svelte-wbsbny\" style=\"top:%gpx;height:24px\"><span class=\"sgPortLabel svelte-wbsbny\">", y - 12);
        esc(sb, name);
        sb_append(sb, "</span> <span class=\"sgPortType svelte-wbsbny\">");
        esc(sb, type);
        sb_appendf(sb, "</span></div> <div class=\"sgPort sgPortOut %s svelte-wbsbny\" style=\"top:%gpx;background:%s\"", port_state(ed, id, type, 1), y - 6,
                   port_color(type));
        port_attrs(ed, sb, id, name, type, "out");
        sb_append(sb, "></div>");
    }
    sb_append(sb, " ");
    int pi = 0;
    cJSON_ArrayForEach(p, l.params) {
        const double y = HEADER_H + ROW_H * row++;
        char idbase[48];
        snprintf(idbase, sizeof(idbase), "%sP%d_%d", ed->prefix, index, pi++);
        param_row(ed, sb, node, p, y, idbase);
    }
    sb_append(sb, " ");
    int ii = 0;
    cJSON_ArrayForEach(p, l.ins) {
        const double y = HEADER_H + ROW_H * row++ + 12;
        const char *name = jstr(p, "name"), *type = jstr(p, "type");
        const int connected = link_of(node, name) != NULL;
        sb_appendf(sb, "<div class=\"sgPort sgPortIn %s svelte-wbsbny%s\" style=\"top:%gpx;background:%s\"", port_state(ed, id, type, 0), connected ? " connected" : "",
                   y - 6, port_color(type));
        port_attrs(ed, sb, id, name, type, "in");
        sb_appendf(sb, "></div> <div class=\"sgRow sgRowIn svelte-wbsbny\" style=\"top:%gpx;height:24px\"><span class=\"sgPortLabel svelte-wbsbny\">", y - 12);
        esc(sb, name);
        sb_append(sb, "</span> ");
        const cJSON *def = cJSON_GetObjectItemCaseSensitive(p, "default");
        if (connected || cJSON_IsNull(def) || !numeric_type(type)) {
            sb_append(sb, "<span class=\"sgPortType svelte-wbsbny\">");
            esc(sb, type);
            sb_append(sb, "</span>");
        } else {
            const cJSON *in = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(node, "inputs"), name);
            const cJSON *v = cJSON_IsObject(in) && cJSON_GetObjectItemCaseSensitive(in, "value") ? cJSON_GetObjectItemCaseSensitive(in, "value") : def;
            char idbase[48];
            snprintf(idbase, sizeof(idbase), "%sI%d_%d", ed->prefix, index, ii);
            sb_appendf(sb, "<span class=\"sgRowWidget svelte-wbsbny\" role=\"presentation\"%s>", STOP);
            value_widget(ed, sb, !strcmp(type, "any") ? "float" : type, v, cJSON_GetObjectItemCaseSensitive(p, "min"), cJSON_GetObjectItemCaseSensitive(p, "max"),
                         NULL, 0, "input", id, name, idbase);
            sb_append(sb, "</span>");
        }
        sb_append(sb, "</div>");
        ii++;
    }
    sb_append(sb, "</div>");
}

// the palette's definitions (iÍìiíiî, :123165-123177; the palette's own filter and sort, :123950-123962)
typedef struct {
    const cJSON *d[256];
    int n;
} DefList;

static const cJSON *link_port(const cJSON *d, const SgLink *link) { // iÎïiìíí
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(d, !strcmp(link->dir, "out") ? "inputs" : "outputs");
    const cJSON *p;
    cJSON_ArrayForEach(p, list) if (!strcmp(jstr(p, "type"), link->type)) return p;
    cJSON_ArrayForEach(p, list) {
        const char *t = jstr(p, "type")[0] ? jstr(p, "type") : "any";
        if (!strcmp(link->dir, "out") ? compatible(link->type, t) : compatible(t, link->type)) return p;
    }
    return NULL;
}

static int def_cmp(const void *a, const void *b) {
    const cJSON *x = *(const cJSON *const *) a, *y = *(const cJSON *const *) b;
    int c = strcasecmp(jstr(x, "category"), jstr(y, "category"));
    if (!c) c = strcasecmp(jstr(x, "title"), jstr(y, "title"));
    return c;
}

static void palette_defs(const SgEditor *ed, DefList *out) {
    out->n = 0;
    char q[64];
    size_t k = 0;
    const char *s = ed->psearch;
    while (*s == ' ') s++;
    for (; *s && k < sizeof(q) - 1; s++) q[k++] = (char) tolower((unsigned char) *s);
    while (k && q[k - 1] == ' ') k--;
    q[k] = 0;
    const cJSON *d;
    cJSON_ArrayForEach(d, cJSON_GetObjectItemCaseSensitive(catalogue(), "defs")) {
        if (out->n >= 256) break;
        if (!strcmp(jstr(d, "type"), "output") || !def_for_kind(d, ed->kind)) continue;
        if (ed->plink && !link_port(d, &ed->pl)) continue;
        if (q[0]) {
            char t[96], ti[96];
            snprintf(t, sizeof(t), "%s", jstr(d, "type"));
            snprintf(ti, sizeof(ti), "%s", jstr(d, "title"));
            for (char *c = t; *c; c++) *c = (char) tolower((unsigned char) *c);
            for (char *c = ti; *c; c++) *c = (char) tolower((unsigned char) *c);
            if (!strstr(t, q) && !strstr(ti, q)) continue;
        }
        out->d[out->n++] = d;
    }
    qsort(out->d, (size_t) out->n, sizeof(out->d[0]), def_cmp);
}

static void palette_list(const SgEditor *ed, StrBuf *sb) {
    DefList l;
    palette_defs(ed, &l);
    if (!l.n) {
        sb_append(sb, "<div class=\"sgPaletteEmpty svelte-sxvv3c\">");
        esc(sb, tr("shaderGraph.noMatches", "No matching nodes"));
        sb_append(sb, "</div>");
    }
    sb_append(sb, " ");
    for (int i = 0; i < l.n; i++) {
        const char *cat = jstr(l.d[i], "category");
        if (!i || strcmp(cat, jstr(l.d[i - 1], "category"))) {
            sb_append(sb, "<div class=\"sgPaletteGroup svelte-sxvv3c\">");
            esc(sb, cat);
            sb_append(sb, "</div> ");
        }
        const char *type = jstr(l.d[i], "type"), *title = jstr(l.d[i], "title"), *desc = jstr(l.d[i], "description");
        sb_appendf(sb, "<button type=\"button\" class=\"sgPaletteItem svelte-sxvv3c%s\" id=\"%sPalItem%d\" data-idx=\"%d\" title=\"", i == ed->pactive ? " active" : "",
                   ed->prefix, i, i);
        esc(sb, desc[0] ? desc : type);
        sb_append(sb, "\" onclick=\"");
        handler(ed, sb, "pick");
        sb_append(sb, ",");
        js(sb, type);
        sb_append(sb, ")\" onmouseenter=\"");
        handler(ed, sb, "palHover");
        sb_appendf(sb, ",%d)\"><span>", i);
        esc(sb, title);
        sb_append(sb, "</span> ");
        if (strcmp(title, type)) {
            sb_append(sb, "<span class=\"sgPaletteType svelte-sxvv3c\">");
            esc(sb, type);
            sb_append(sb, "</span>");
        }
        sb_append(sb, "</button>");
    }
}

static void palette_html(const SgEditor *ed, StrBuf *sb) {
    char ph[160];
    if (ed->plink) {
        const char *f = tr(!strcmp(ed->pl.dir, "out") ? "shaderGraph.searchNodesFor" : "shaderGraph.searchNodesProducing",
                           !strcmp(ed->pl.dir, "out") ? "Nodes that take {0}\xe2\x80\xa6" : "Nodes that produce {0}\xe2\x80\xa6");
        StrBuf t = {0};
        sb_append_replace_all(&t, f, "{0}", ed->pl.type);
        char *s = sb_finish(&t);
        snprintf(ph, sizeof(ph), "%s", s);
        free(s);
    } else snprintf(ph, sizeof(ph), "%s", tr("shaderGraph.searchNodes", "Search nodes\xe2\x80\xa6"));
    sb_appendf(sb, "<div class=\"sgPalette svelte-sxvv3c\" role=\"dialog\" id=\"%sPal\" style=\"left:%gpx;top:%gpx\" onmousedown=\"event.stopPropagation()\" "
                   "oncontextmenu=\"return false\" onwheel=\"event.stopPropagation()\"><input class=\"sgPaletteSearch svelte-sxvv3c\" type=\"text\" spellcheck=\"false\" "
                   "id=\"%sPalSearch\" placeholder=\"",
               ed->prefix, ed->psx, ed->psy, ed->prefix);
    esc(sb, ph);
    sb_append(sb, "\" value=\"");
    esc(sb, ed->psearch);
    sb_append(sb, "\" oninput=\"");
    handler(ed, sb, "palSearch");
    sb_append(sb, ",this.value)\" onkeydown=\"event.stopPropagation();return ");
    handler(ed, sb, "palKey");
    sb_appendf(sb, ")\"> <div class=\"sgPaletteList svelte-sxvv3c\" id=\"%sPalList\">", ed->prefix);
    palette_list(ed, sb);
    sb_append(sb, "</div></div>");
}

static void footer_inner(const SgEditor *ed, StrBuf *sb, const ModShaderGraphError *errs, const int nerr) {
    if (ed->msg[0]) {
        sb_append(sb, "<div class=\"sgNotice svelte-upgo13\">");
        esc(sb, ed->msg);
        sb_append(sb, "</div>");
    }
    sb_append(sb, " ");
    if (nerr) {
        sb_appendf(sb, "<div class=\"sgErrorList svelte-upgo13\" id=\"%sErrors\">", ed->prefix);
        for (int i = 0; i < nerr && i < MAX_ERRORS; i++) {
            const int click = errs[i].node[0] != 0;
            sb_appendf(sb, "<button type=\"button\" class=\"sgErrorItem svelte-upgo13%s\"%s onclick=\"", click ? " clickable" : "", click ? "" : " disabled");
            handler(ed, sb, "errNode");
            sb_append(sb, ",");
            js(sb, errs[i].node);
            sb_append(sb, ")\"><i class=\"material-icons svelte-upgo13\">error_outline</i> ");
            if (click) {
                sb_append(sb, "<code class=\"svelte-upgo13\">");
                esc(sb, errs[i].node);
                sb_append(sb, "</code>");
            }
            sb_append(sb, " ");
            esc(sb, errs[i].message);
            sb_append(sb, "</button>");
        }
        sb_append(sb, "</div>");
    } else {
        StrBuf t = {0};
        char n[16];
        snprintf(n, sizeof(n), "%d", cJSON_GetArraySize(nodes_of(ed)));
        sb_append_replace_all(&t, tr("shaderGraph.valid", "Valid graph \xc2\xb7 {0} nodes"), "{0}", n);
        char *s = sb_finish(&t);
        sb_appendf(sb, "<div class=\"sgOk svelte-upgo13\" id=\"%sOk\"><i class=\"material-icons svelte-upgo13\">check_circle</i> ", ed->prefix);
        esc(sb, s);
        sb_append(sb, "</div>");
        free(s);
    }
}

static int graph_errors(const SgEditor *ed, ModShaderGraphError *errs) {
    const int n = graph_doc(ed->graph) ? mod_shader_graph_errors(ed->graph, ed->kind, errs, MAX_ERRORS) : 0;
    return n > MAX_ERRORS ? MAX_ERRORS : n;
}

static void canvas_style(const SgEditor *ed, StrBuf *sb) {
    sb_appendf(sb, "background-position:%gpx %gpx;background-size:%gpx %gpx", ed->vx, ed->vy, 24 * ed->vk, 24 * ed->vk);
}

static void world_style(const SgEditor *ed, StrBuf *sb) { sb_appendf(sb, "transform:translate(%gpx,%gpx) scale(%g)", ed->vx, ed->vy, ed->vk); }

static void inner_html(SgEditor *ed, StrBuf *sb) {
    ModShaderGraphError errs[MAX_ERRORS];
    const int nerr = graph_errors(ed, errs);
    // the toolbar
    sb_append(sb, "<div class=\"sgToolbar svelte-upgo13\">");
    if (!ed->readonly) {
        sb_appendf(sb, "<button type=\"button\" class=\"sgBtn sgBtnPrimary svelte-upgo13\" id=\"%sAdd\" onclick=\"", ed->prefix);
        handler(ed, sb, "add");
        sb_appendf(sb, ")\"><i class=\"material-icons svelte-upgo13\">add</i> %s</button> <button type=\"button\" class=\"sgBtn svelte-upgo13\" title=\"Ctrl+Z\" id=\"%sUndo\"%s onclick=\"",
                   tr("shaderGraph.addNode", "Add node"), ed->prefix, ed->hidx > 0 ? "" : " disabled");
        handler(ed, sb, "undo");
        sb_appendf(sb, ")\"><i class=\"material-icons svelte-upgo13\">undo</i></button> <button type=\"button\" class=\"sgBtn svelte-upgo13\" title=\"Ctrl+Shift+Z\" id=\"%sRedo\"%s onclick=\"",
                   ed->prefix, ed->hidx < ed->nhist - 1 ? "" : " disabled");
        handler(ed, sb, "redo");
        sb_append(sb, ")\"><i class=\"material-icons svelte-upgo13\">redo</i></button>");
    }
    sb_appendf(sb, " <button type=\"button\" class=\"sgBtn svelte-upgo13\" title=\"F\" id=\"%sFit\" onclick=\"", ed->prefix);
    handler(ed, sb, "fit");
    sb_appendf(sb, ")\"><i class=\"material-icons svelte-upgo13\">fit_screen</i> %s</button> ", tr("shaderGraph.fitView", "Fit"));
    if (!ed->readonly) {
        int ntpl = 0;
        const cJSON *t;
        cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(catalogue(), "templates"))
            ntpl += !strcmp(jstr(cJSON_GetArrayItem(t, 1), "kind"), ed->kind);
        sb_appendf(sb, "<span class=\"sgMenuWrap svelte-upgo13\"><button type=\"button\" class=\"sgBtn svelte-upgo13\" id=\"%sTpl\"%s onclick=\"", ed->prefix,
                   ntpl ? "" : " disabled");
        handler(ed, sb, "tplMenu");
        sb_appendf(sb, ")\"><i class=\"material-icons svelte-upgo13\">auto_awesome</i> %s <i class=\"material-icons svelte-upgo13\">arrow_drop_down</i></button> ",
                   tr("shaderGraph.templates", "Templates"));
        if (ed->tpl_open) {
            sb_append(sb, "<div class=\"sgMenu svelte-upgo13\" role=\"menu\">");
            int i = 0;
            cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(catalogue(), "templates")) {
                const cJSON *g = cJSON_GetArrayItem(t, 1);
                const char *tid = cJSON_GetStringValue(cJSON_GetArrayItem(t, 0));
                if (!tid || strcmp(jstr(g, "kind"), ed->kind)) continue;
                // iíííïîì: the id in Title Case (no template carries a name)
                char name[96];
                snprintf(name, sizeof(name), "%s", jstr(g, "name")[0] ? jstr(g, "name") : tid);
                if (!jstr(g, "name")[0]) {
                    int start = 1;
                    for (char *c = name; *c; c++) {
                        if (*c == '-' || *c == '_') {
                            *c = ' ';
                            start = 1;
                        } else if (start) {
                            *c = (char) toupper((unsigned char) *c);
                            start = 0;
                        }
                    }
                }
                sb_appendf(sb, "<button type=\"button\" class=\"sgMenuItem svelte-upgo13\" role=\"menuitem\" id=\"%sTpl%d\" onclick=\"", ed->prefix, i++);
                handler(ed, sb, "tpl");
                sb_append(sb, ",");
                js(sb, tid);
                sb_append(sb, ")\">");
                esc(sb, name);
                sb_append(sb, "</button>");
            }
            sb_append(sb, "</div>");
        }
        sb_appendf(sb, "</span> <button type=\"button\" class=\"sgBtn svelte-upgo13\" id=\"%sImport\" onclick=\"", ed->prefix);
        handler(ed, sb, "import");
        sb_appendf(sb, ")\"><i class=\"material-icons svelte-upgo13\">file_upload</i> %s</button>", tr("shaderGraph.import", "Import JSON"));
    }
    sb_appendf(sb, " <button type=\"button\" class=\"sgBtn svelte-upgo13\" id=\"%sExport\" onclick=\"", ed->prefix);
    handler(ed, sb, "export");
    sb_appendf(sb, ")\"><i class=\"material-icons svelte-upgo13\">file_download</i> %s</button> <span class=\"sgToolbarSpacer svelte-upgo13\"></span> "
                   "<span class=\"sgZoom svelte-upgo13\" id=\"%sZoom\">%d%%</span></div> ",
               tr("shaderGraph.export", "Export JSON"), ed->prefix, (int) lround(100 * ed->vk));
    // the canvas
    sb_appendf(sb, "<div class=\"sgCanvas svelte-upgo13\" tabindex=\"0\" role=\"application\" id=\"%sCanvas\" aria-label=\"", ed->prefix);
    esc(sb, tr("shaderGraph.canvasLabel", "Shader graph canvas"));
    sb_append(sb, "\" style=\"");
    canvas_style(ed, sb);
    sb_append(sb, "\" onmousedown=\"");
    handler(ed, sb, "down");
    sb_append(sb, ")\" onmousemove=\"");
    handler(ed, sb, "move");
    sb_append(sb, ")\" onmouseup=\"");
    handler(ed, sb, "up");
    sb_append(sb, ")\" oncontextmenu=\"return ");
    handler(ed, sb, "ctx");
    sb_append(sb, ")\" onwheel=\"return ");
    handler(ed, sb, "wheel");
    sb_append(sb, ")\" onkeydown=\"return ");
    handler(ed, sb, "key");
    sb_appendf(sb, ")\"><div class=\"sgWorld svelte-upgo13\" id=\"%sWorld\" style=\"", ed->prefix);
    world_style(ed, sb);
    StrBuf ws = {0}, src = {0};
    wires_img(ed, &ws, &src);
    char *wstyle = sb_finish(&ws), *wsrc = sb_finish(&src);
    sb_appendf(sb, "\"><img class=\"sgWires svelte-upgo13\" id=\"%sWires\" alt=\"\" style=\"%s\" src=\"%s\"> ", ed->prefix, wstyle, wsrc);
    free(wstyle);
    free(wsrc);
    int i = 0;
    const cJSON *n;
    cJSON_ArrayForEach(n, nodes_of(ed)) node_html(ed, sb, n, i++, errs, nerr);
    sb_append(sb, "</div> ");
    if (ed->pal) palette_html(ed, sb);
    sb_append(sb, " ");
    if (ed->io) {
        const int ex = ed->io == IO_EXPORT;
        sb_appendf(sb, "<div class=\"sgOverlay svelte-upgo13\" role=\"dialog\" id=\"%sIo\" onmousedown=\"event.stopPropagation()\" oncontextmenu=\"event.stopPropagation()\">"
                       "<div class=\"sgOverlayHead svelte-upgo13\"><span>%s</span> <button type=\"button\" class=\"sgBtn sgBtnIcon svelte-upgo13\" id=\"%sIoClose\" title=\"%s\" onclick=\"",
                   ed->prefix, ex ? tr("shaderGraph.exportTitle", "Export graph JSON") : tr("shaderGraph.importTitle", "Import graph JSON"), ed->prefix,
                   tr("generic.close", "Close"));
        handler(ed, sb, "ioClose");
        sb_appendf(sb, ")\"><i class=\"material-icons svelte-upgo13\">close</i></button></div> <textarea class=\"sgIoText svelte-upgo13\" spellcheck=\"false\" id=\"%sIoText\""
                       " placeholder=\"%s\"%s onkeydown=\"event.stopPropagation()\" oninput=\"",
                   ed->prefix, ex ? "" : tr("shaderGraph.importHint", "Paste a shader graph document here"), ex ? " readonly" : "");
        handler(ed, sb, "ioInput");
        sb_append(sb, ")\">");
        if (ex) {
            char *text = cJSON_Print(ed->graph); // JSON.stringify(graph, null, 2): cJSON indents with tabs
            StrBuf t = {0};
            sb_append_replace_all(&t, text ? text : "", "\t", "  ");
            char *s = sb_finish(&t);
            esc(sb, s);
            free(s);
            free(text);
        }
        sb_append(sb, "</textarea> ");
        if (ed->io_err[0]) {
            sb_append(sb, "<div class=\"sgIoError svelte-upgo13\">");
            esc(sb, ed->io_err);
            sb_append(sb, "</div>");
        }
        sb_append(sb, " <div class=\"sgOverlayActions svelte-upgo13\">");
        if (ex) {
            sb_appendf(sb, "<button type=\"button\" class=\"sgBtn sgBtnPrimary svelte-upgo13\" id=\"%sIoCopy\" onclick=\"", ed->prefix);
            handler(ed, sb, "ioCopy");
            sb_appendf(sb, ")\"><i class=\"material-icons svelte-upgo13\">content_copy</i> %s</button>", tr("shaderGraph.copy", "Copy"));
        } else {
            sb_appendf(sb, "<button type=\"button\" class=\"sgBtn sgBtnPrimary svelte-upgo13\" id=\"%sIoLoad\" disabled onclick=\"", ed->prefix);
            handler(ed, sb, "ioLoad");
            sb_appendf(sb, ")\"><i class=\"material-icons svelte-upgo13\">check</i> %s</button>", tr("shaderGraph.load", "Load"));
        }
        sb_append(sb, "</div></div>");
    }
    sb_append(sb, " ");
    if (!ed->readonly) {
        sb_append(sb, "<div class=\"sgHint svelte-upgo13\">");
        esc(sb, tr("shaderGraph.hint", "Drag: pan \xc2\xb7 Wheel: zoom \xc2\xb7 Right-click: add node \xc2\xb7 Del: remove \xc2\xb7 Ctrl+Z: undo"));
        sb_append(sb, "</div>");
    }
    sb_appendf(sb, "</div> <div class=\"sgFooter svelte-upgo13%s\" id=\"%sFooter\">", nerr ? " hasErrors" : "", ed->prefix);
    footer_inner(ed, sb, errs, nerr);
    sb_append(sb, "</div>");
}

void sg_editor_html(SgEditor *ed, StrBuf *sb) {
    sb_appendf(sb, "<div class=\"sgEditor svelte-upgo13%s\" id=\"%sEd\">", ed->readonly ? " readonly" : "", ed->prefix);
    inner_html(ed, sb);
    sb_append(sb, "</div>");
}

// ------------------------------------------------------------------ partial updates

// the whole editor again (a structural change); the canvas keeps the focus it had
static void refresh(SgEditor *ed) {
    const HtmlElem root = el(ed, "Ed");
    if (!root) return;
    char focus[64] = "";
    const HtmlElem a = html_dom_active_element();
    if (a) html_dom_get_attr(a, "id", focus, sizeof(focus));
    StrBuf sb = {0};
    inner_html(ed, &sb);
    char *html = sb_finish(&sb);
    html_dom_set_html(root, html);
    free(html);
    if (focus[0] && !strncmp(focus, ed->prefix, strlen(ed->prefix))) {
        const HtmlElem f = html_dom_by_id(focus);
        if (f) html_dom_focus(f);
    }
}

static void update_view(const SgEditor *ed) {
    StrBuf a = {0}, b = {0};
    canvas_style(ed, &a);
    world_style(ed, &b);
    char *cs = sb_finish(&a), *wsty = sb_finish(&b);
    const HtmlElem c = el(ed, "Canvas"), w = el(ed, "World"), z = el(ed, "Zoom");
    if (c) html_dom_set_attr(c, "style", cs);
    if (w) html_dom_set_attr(w, "style", wsty);
    if (z) {
        char t[16];
        snprintf(t, sizeof(t), "%d%%", (int) lround(100 * ed->vk));
        html_dom_set_text(z, t);
    }
    free(cs);
    free(wsty);
}

static void update_wires(const SgEditor *ed) {
    const HtmlElem img = el(ed, "Wires");
    if (!img) return;
    StrBuf ws = {0}, src = {0};
    wires_img(ed, &ws, &src);
    char *wstyle = sb_finish(&ws), *wsrc = sb_finish(&src);
    html_dom_set_attr(img, "style", wstyle);
    html_dom_set_attr(img, "src", wsrc);
    free(wstyle);
    free(wsrc);
}

static void update_footer(const SgEditor *ed) {
    ModShaderGraphError errs[MAX_ERRORS];
    const int nerr = graph_errors(ed, errs);
    const HtmlElem f = el(ed, "Footer");
    if (f) {
        StrBuf sb = {0};
        footer_inner(ed, &sb, errs, nerr);
        char *html = sb_finish(&sb);
        html_dom_set_html(f, html);
        free(html);
        html_dom_set_class(f, "hasErrors", nerr > 0);
    }
    int i = 0;
    const cJSON *n;
    cJSON_ArrayForEach(n, nodes_of(ed)) {
        char sfx[16];
        snprintf(sfx, sizeof(sfx), "N%d", i++);
        const HtmlElem e = el(ed, sfx);
        if (!e) continue;
        int bad = 0;
        for (int k = 0; k < nerr && !bad; k++) bad = errs[k].node[0] && !strcmp(errs[k].node, jstr(n, "id"));
        html_dom_set_class(e, "hasError", bad);
    }
}

static void update_nodes_pos(const SgEditor *ed) {
    int i = 0;
    const cJSON *n;
    cJSON_ArrayForEach(n, nodes_of(ed)) {
        char sfx[16];
        snprintf(sfx, sizeof(sfx), "N%d", i++);
        if (!selected(ed, jstr(n, "id"))) continue;
        const HtmlElem e = el(ed, sfx);
        if (!e) continue;
        const Layout l = layout_of(ed, n);
        char st[128];
        snprintf(st, sizeof(st), "left:%gpx;top:%gpx;width:%dpx;height:%gpx", l.x, l.y, NODE_W, l.h);
        html_dom_set_attr(e, "style", st);
    }
}

static void update_selection(const SgEditor *ed) {
    int i = 0;
    const cJSON *n;
    cJSON_ArrayForEach(n, nodes_of(ed)) {
        char sfx[16];
        snprintf(sfx, sizeof(sfx), "N%d", i++);
        const HtmlElem e = el(ed, sfx);
        if (e) html_dom_set_class(e, "selected", selected(ed, jstr(n, "id")));
    }
    update_wires(ed);
}

static void update_toolbar_history(const SgEditor *ed) {
    const HtmlElem u = el(ed, "Undo"), r = el(ed, "Redo");
    if (u) {
        if (ed->hidx > 0) html_dom_remove_attr(u, "disabled");
        else html_dom_set_attr(u, "disabled", "");
    }
    if (r) {
        if (ed->hidx < ed->nhist - 1) html_dom_remove_attr(r, "disabled");
        else html_dom_set_attr(r, "disabled", "");
    }
}

// ------------------------------------------------------------------ the API

SgEditor *sg_editor_new(const char *prefix, const char *call, const char *kind, const SgChangeFn fn, void *user) {
    SgEditor *ed = calloc(1, sizeof(*ed));
    if (!ed) return NULL;
    snprintf(ed->prefix, sizeof(ed->prefix), "%s", prefix);
    snprintf(ed->call, sizeof(ed->call), "%s", call);
    snprintf(ed->kind, sizeof(ed->kind), "%s", kind);
    ed->fn = fn;
    ed->user = user;
    ed->vx = 40, ed->vy = 40, ed->vk = 1;
    ed->hidx = -1;
    sg_editor_set_graph(ed, NULL);
    return ed;
}

void sg_editor_free(SgEditor *ed) {
    if (!ed) return;
    for (int i = 0; i < ed->nhist; i++) free(ed->hist[i]);
    free(ed->hist);
    free(ed->last_json);
    cJSON_Delete(ed->graph);
    free(ed);
}

const cJSON *sg_editor_graph(const SgEditor *ed) { return ed ? ed->graph : NULL; }

void sg_editor_set_readonly(SgEditor *ed, const int readonly) {
    if (ed) ed->readonly = readonly;
}

void sg_editor_set_graph(SgEditor *ed, const cJSON *graph) {
    if (!ed) return;
    char *json = graph_doc(graph) ? cJSON_PrintUnformatted(graph) : NULL;
    if (json && ed->last_json && !strcmp(json, ed->last_json)) {
        free(json);
        return;
    }
    cJSON_Delete(ed->graph);
    if (json) {
        ed->graph = cJSON_Duplicate(graph, 1);
        free(ed->last_json);
        ed->last_json = json;
    } else { // the kind's default, as a change (onChange carries it to the owner)
        ed->graph = default_graph(ed->kind);
        free(ed->last_json);
        ed->last_json = cJSON_PrintUnformatted(ed->graph);
        ed->change_due = now_ms() + CHANGE_DEBOUNCE_MS;
    }
    if (json) ed->change_due = 0; // another document: a change still pending was the old one's
    history_reset(ed, ed->last_json);
    sel_clear(ed);
    ed->wire_sel = 0;
    ed->drag = DRAG_NONE;
    ed->pal = 0;
    ed->fit_due = 1;
}

void sg_editor_flush(SgEditor *ed) {
    if (!ed || ed->change_due <= 0) return;
    ed->change_due = 0;
    if (ed->fn) ed->fn(ed->user, ed->graph);
}

void sg_editor_tick(SgEditor *ed) {
    if (!ed) return;
    const double t = now_ms();
    if (ed->change_due > 0 && t >= ed->change_due) sg_editor_flush(ed);
    if (ed->msg[0] && t >= ed->msg_until) {
        ed->msg[0] = 0;
        if (el(ed, "Footer")) update_footer(ed);
    }
    if (ed->fit_due && el(ed, "Canvas")) { // the tick after the canvas is in the page (tick().then(fit))
        HtmlRect r = {0};
        if (canvas_rect(ed, &r) && r.w > 0) {
            ed->fit_due = 0;
            fit(ed);
            update_view(ed);
        }
    }
}

// ------------------------------------------------------------------ handlers

static const char *arg_s(const HtmlArg *a, const int argc, const int i) {
    if (i >= argc) return "";
    if (a[i].kind == HTML_ARG_STR || a[i].kind == HTML_ARG_THIS_VALUE) return a[i].str ? a[i].str : "";
    return "";
}

static double arg_n(const HtmlArg *a, const int argc, const int i) {
    if (i >= argc) return 0;
    if (a[i].kind == HTML_ARG_NUM) return a[i].num;
    if (a[i].kind == HTML_ARG_BOOL) return a[i].num;
    if (a[i].kind == HTML_ARG_STR || a[i].kind == HTML_ARG_THIS_VALUE) return a[i].str ? atof(a[i].str) : 0;
    return 0;
}

static int arg_bool(const HtmlArg *a, const int argc, const int i, const HtmlEvent *ev) {
    if (i >= argc) return 0;
    if (a[i].kind == HTML_ARG_THIS_CHECKED) return ev && ev->checked;
    if (a[i].kind == HTML_ARG_STR) return a[i].str && (!strcmp(a[i].str, "true") || !strcmp(a[i].str, "1"));
    return a[i].num != 0;
}

static void close_palette(SgEditor *ed) { // iÏiíìïi
    ed->pal = 0;
    ed->tpl_open = 0;
}

// iIîiîïi (:123349-123358): the palette at canvas (sx, sy) for world (wx, wy), clamped inside the canvas
static void open_palette(SgEditor *ed, const double sx, const double sy, const double wx, const double wy, const SgLink *link) {
    double cw, ch;
    canvas_size(ed, &cw, &ch);
    ed->tpl_open = 0;
    ed->pal = 1;
    ed->psx = fmin(fmax(sx, 4), fmax(4, cw - 228));
    ed->psy = fmin(fmax(sy, 4), fmax(4, ch - 200));
    ed->pwx = wx;
    ed->pwy = wy;
    ed->plink = link != NULL;
    if (link) ed->pl = *link;
    ed->psearch[0] = 0;
    ed->pactive = 0;
}

// iÎìíïìí (:123359-123406): the picked def becomes a node at the palette's point, linked when the palette has a link
static void pick(SgEditor *ed, const char *type) {
    const cJSON *d = def_of(type);
    if (!d || ed->readonly) return;
    const double x = ed->plink && !strcmp(ed->pl.dir, "in") ? ed->pwx - NODE_W : ed->pwx;
    const double y = ed->pwy - HEADER_H - 12;
    cJSON *g = edit_copy(ed);
    char id[ID_N];
    new_id(ed, type, id, sizeof(id));
    cJSON *node = cJSON_CreateObject();
    cJSON_AddStringToObject(node, "id", id);
    cJSON_AddStringToObject(node, "type", type);
    cJSON_AddNumberToObject(node, "x", round(x));
    cJSON_AddNumberToObject(node, "y", round(y));
    const cJSON *ps = cJSON_GetObjectItemCaseSensitive(d, "params");
    if (cJSON_GetArraySize(ps)) {
        cJSON *params = cJSON_AddObjectToObject(node, "params");
        const cJSON *p;
        cJSON_ArrayForEach(p, ps) {
            const cJSON *def = cJSON_GetObjectItemCaseSensitive(p, "default");
            if (!def || cJSON_IsNull(def) || (cJSON_IsString(def) && !def->valuestring[0])) continue;
            cJSON_AddItemToObject(params, jstr(p, "name"), cJSON_Duplicate(def, 1));
        }
    }
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(g, "nodes"), node);
    const cJSON *port = ed->plink ? link_port(d, &ed->pl) : NULL;
    if (ed->plink && port) {
        const cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(d, "outputs"), 0);
        if (!strcmp(ed->pl.dir, "out")) connect(g, ed->pl.node, ed->pl.port, id, jstr(port, "name"));
        else connect(g, id, first && !strcmp(jstr(port, "name"), jstr(first, "name")) ? NULL : jstr(port, "name"), ed->pl.node, ed->pl.port);
    }
    set_graph(ed, g, "");
    sel_clear(ed);
    sel_add(ed, id);
    ed->pal = 0;
    refresh(ed);
    focus_canvas(ed);
}

// the Delete key (:123562-123593): the selected wire, else the selected nodes but the output
static void delete_selection(SgEditor *ed) {
    if (ed->readonly) return;
    if (ed->wire_sel) {
        cJSON *g = edit_copy(ed);
        disconnect(g, ed->wire_node, ed->wire_port);
        set_graph(ed, g, "");
        ed->wire_sel = 0;
        refresh(ed);
        return;
    }
    const char *out = jstr(ed->graph, "output");
    int any = 0;
    for (int i = 0; i < ed->nsel; i++) any |= strcmp(ed->sel[i], out) != 0;
    if (!any) return;
    cJSON *g = edit_copy(ed);
    cJSON *nodes = cJSON_GetObjectItemCaseSensitive(g, "nodes");
    for (cJSON *n = nodes->child; n;) {
        cJSON *next = n->next;
        const char *id = jstr(n, "id");
        if (selected(ed, id) && strcmp(id, out)) cJSON_Delete(cJSON_DetachItemViaPointer(nodes, n));
        n = next;
    }
    cJSON *n;
    cJSON_ArrayForEach(n, nodes) {
        cJSON *ins = cJSON_GetObjectItemCaseSensitive(n, "inputs");
        if (!cJSON_IsObject(ins)) continue;
        for (cJSON *in = ins->child; in;) {
            cJSON *next = in->next;
            const char *src = jstr(in, "node");
            if (src[0] && selected(ed, src) && strcmp(src, out)) cJSON_Delete(cJSON_DetachItemViaPointer(ins, in));
            in = next;
        }
    }
    set_graph(ed, g, "");
    sel_clear(ed);
    refresh(ed);
}

// the palette's keys (iïîiïîì, :122711): Escape, the arrows, Enter
static int palette_key(SgEditor *ed, const HtmlEvent *ev) {
    const int k = ev ? ev->key : 0;
#ifdef __EMSCRIPTEN__
    const int down = k == 40, up = k == 38;
#else
    const int down = k == 264, up = k == 265;
#endif
    DefList l;
    palette_defs(ed, &l);
    if (ev && html_key_is_escape(k)) {
        ed->pal = 0;
        refresh(ed);
        focus_canvas(ed);
        return 0;
    }
    if (down || up) {
        ed->pactive = down ? (ed->pactive + 1 < l.n ? ed->pactive + 1 : (l.n ? l.n - 1 : 0)) : (ed->pactive > 0 ? ed->pactive - 1 : 0);
        for (int i = 0; i < l.n; i++) {
            char sfx[24];
            snprintf(sfx, sizeof(sfx), "PalItem%d", i);
            const HtmlElem e = el(ed, sfx);
            if (e) html_dom_set_class(e, "active", i == ed->pactive);
        }
        return 0;
    }
    if (ev && html_key_is_enter(k)) {
        if (ed->pactive < l.n) pick(ed, jstr(l.d[ed->pactive], "type"));
        return 0;
    }
    return 1;
}

static void palette_relist(SgEditor *ed) {
    const HtmlElem list = el(ed, "PalList");
    if (!list) return;
    StrBuf sb = {0};
    palette_list(ed, &sb);
    char *html = sb_finish(&sb);
    html_dom_set_html(list, html);
    free(html);
}

// a widget's value as the node takes it (the value editor's onInput, iÏîîìíí / the hex check)
static cJSON *widget_value(const SgEditor *ed, const char *node, const char *name, const int is_input, const char *how, const char *text, const int checked) {
    if (!strcmp(how, "bool")) return cJSON_CreateBool(checked);
    if (!strcmp(how, "str") || !strcmp(how, "color")) return cJSON_CreateString(text);
    if (!strcmp(how, "hex")) {
        int ok = text[0] == '#' && strlen(text) == 7;
        for (int i = 1; ok && i < 7; i++) ok = isxdigit((unsigned char) text[i]) != 0;
        if (!ok) return NULL;
        char low[8];
        for (int i = 0; i < 8; i++) low[i] = (char) tolower((unsigned char) text[i]);
        return cJSON_CreateString(low);
    }
    const double v = atof(text);
    const int finite = text[0] && isfinite(v);
    if (!strcmp(how, "int")) return cJSON_CreateNumber(round(finite ? v : 0));
    if (!strcmp(how, "optint") || !strcmp(how, "optnum")) {
        if (!finite) return cJSON_CreateNull(); // '' or not a number: unset
        return cJSON_CreateNumber(!strcmp(how, "optint") ? round(v) : v);
    }
    if (!strcmp(how, "num")) return cJSON_CreateNumber(finite ? v : 0);
    if (how[0] == 'v' && isdigit((unsigned char) how[1])) { // a vector component
        const int k = how[1] - '0';
        const cJSON *n = node_by_id(ed, node);
        const cJSON *cur = NULL;
        int width = 1;
        if (is_input) {
            const Layout l = layout_of(ed, n);
            const cJSON *pd = port_def(&l, name, 0);
            const cJSON *in = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(n, "inputs"), name);
            cur = cJSON_IsObject(in) && cJSON_GetObjectItemCaseSensitive(in, "value") ? cJSON_GetObjectItemCaseSensitive(in, "value")
                                                                                      : cJSON_GetObjectItemCaseSensitive(pd, "default");
            width = type_width(jstr(pd, "type"));
        } else {
            const Layout l = layout_of(ed, n);
            const cJSON *p;
            const cJSON *pd = NULL;
            cJSON_ArrayForEach(p, l.params) if (!strcmp(jstr(p, "name"), name)) pd = p;
            const cJSON *pv = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(n, "params"), name);
            cur = pv && !cJSON_IsNull(pv) ? pv : cJSON_GetObjectItemCaseSensitive(pd, "default");
            const char *w = jstr(pd, "widget");
            if (!strcmp(jstr(n, "type"), "param") && !strcmp(name, "default"))
                w = jstr(cJSON_GetObjectItemCaseSensitive(n, "params"), "type");
            width = type_width(w);
        }
        cJSON *a = cJSON_CreateArray();
        for (int i = 0; i < width; i++) {
            const cJSON *e = cJSON_IsArray(cur) ? cJSON_GetArrayItem(cur, i) : NULL;
            const double c = i == k ? (finite ? v : 0) : cJSON_IsNumber(e) && isfinite(e->valuedouble) ? e->valuedouble : 0;
            cJSON_AddItemToArray(a, cJSON_CreateNumber(c));
        }
        return a;
    }
    return cJSON_CreateNumber(finite ? v : 0);
}

// iiïìïiï (:122497): a param node's default in its new type
static cJSON *param_default(const char *type, const cJSON *v) {
    if (!strcmp(type, "color")) {
        int ok = cJSON_IsString(v) && strlen(v->valuestring) == 7 && v->valuestring[0] == '#';
        for (int i = 1; ok && i < 7; i++) ok = isxdigit((unsigned char) v->valuestring[i]) != 0;
        return cJSON_CreateString(ok ? v->valuestring : "#ffffff");
    }
    const int n = type_width(type);
    if (n == 1) return cJSON_CreateNumber(cJSON_IsNumber(v) && isfinite(v->valuedouble) ? v->valuedouble : 0);
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        const cJSON *e = cJSON_IsArray(v) ? cJSON_GetArrayItem(v, i) : NULL;
        const cJSON *first = cJSON_IsArray(v) ? cJSON_GetArrayItem(v, 0) : v;
        cJSON_AddItemToArray(a, cJSON_CreateNumber(cJSON_IsNumber(e) && isfinite(e->valuedouble) ? e->valuedouble
                                                   : cJSON_IsNumber(first) && isfinite(first->valuedouble) ? first->valuedouble : 0));
    }
    return a;
}

// onParam (iÏiîìîï, :123419-123431): the param set (null / "" deletes it); a param node's type change converts its default
static void set_param(SgEditor *ed, const char *node, const char *name, cJSON *value) {
    if (ed->readonly) {
        cJSON_Delete(value);
        return;
    }
    cJSON *g = edit_copy(ed);
    cJSON *n = node_in(g, node);
    if (!n) {
        cJSON_Delete(g);
        cJSON_Delete(value);
        return;
    }
    cJSON *params = obj_member(n, "params");
    const int unset = !value || cJSON_IsNull(value) || (cJSON_IsString(value) && !value->valuestring[0]);
    char *old_type = !strcmp(jstr(n, "type"), "param") && !strcmp(name, "type") ? strdup(jstr(params, "type")) : NULL;
    if (unset) {
        cJSON_DeleteItemFromObjectCaseSensitive(params, name);
        cJSON_Delete(value);
    } else put(params, name, value);
    if (old_type) {
        const char *t = jstr(params, "type");
        put(params, "default", param_default(t[0] ? t : "float", cJSON_GetObjectItemCaseSensitive(params, "default")));
        free(old_type);
    }
    set_graph(ed, g, NULL);
}

// onInputValue (iÍiîîíì, :123407-123418): inputs[port] = {value}
static void set_input(SgEditor *ed, const char *node, const char *port, cJSON *value) {
    if (ed->readonly || !value) {
        cJSON_Delete(value);
        return;
    }
    cJSON *g = edit_copy(ed);
    cJSON *n = node_in(g, node);
    if (!n) {
        cJSON_Delete(g);
        cJSON_Delete(value);
        return;
    }
    cJSON *v = cJSON_CreateObject();
    cJSON_AddItemToObject(v, "value", value);
    put(obj_member(n, "inputs"), port, v);
    set_graph(ed, g, NULL);
}

static void end_drag(SgEditor *ed, const double x, const double y) { // iÎîíîìi (:123311-123348)
    const int kind = ed->drag;
    ed->drag = DRAG_NONE;
    if (kind == DRAG_NODES) {
        if (ed->moved) {
            history_push(ed, "move");
            update_toolbar_history(ed);
        }
        return;
    }
    if (kind != DRAG_WIRE) return;
    const HtmlElem hit = html_dom_element_at((float) x, (float) y);
    const HtmlElem port = hit ? html_dom_closest(hit, "[data-port]") : 0;
    char dir[8] = "", pnode[ID_N] = "", pport[ID_N] = "", ptype[8] = "any";
    if (port) {
        html_dom_get_attr(port, "data-dir", dir, sizeof(dir));
        html_dom_get_attr(port, "data-node", pnode, sizeof(pnode));
        html_dom_get_attr(port, "data-port", pport, sizeof(pport));
        if (html_dom_get_attr(port, "data-type", ptype, sizeof(ptype)) < 0 || !ptype[0]) snprintf(ptype, sizeof(ptype), "any");
    }
    if (port && strcmp(dir, ed->dl.dir)) {
        const int out = !strcmp(ed->dl.dir, "out");
        const char *fn = out ? ed->dl.node : pnode, *fp = out ? ed->dl.port : pport, *ft = out ? ed->dl.type : ptype;
        const char *tn = out ? pnode : ed->dl.node, *tp = out ? pport : ed->dl.port, *tt = out ? ptype : ed->dl.type;
        char buf[160];
        const char *err = connect_error(ed, fn, ft, tn, tt, buf, sizeof(buf));
        if (err) {
            message(ed, err);
            if (ed->detached) history_push(ed, "");
        } else {
            cJSON *g = edit_copy(ed);
            connect(g, fn, !strcmp(fp, first_output(ed, fn)) ? NULL : fp, tn, tp);
            set_graph(ed, g, "");
        }
        refresh(ed);
        return;
    }
    if (ed->detached) history_push(ed, "");
    HtmlRect r = {0};
    if (!ed->readonly && canvas_rect(ed, &r) && x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h) {
        double cx, cy, wx, wy;
        to_canvas(ed, x, y, &cx, &cy);
        to_world(ed, x, y, &wx, &wy);
        open_palette(ed, cx, cy, wx, wy, &ed->dl);
    }
    refresh(ed);
    if (ed->pal) {
        const HtmlElem s = el(ed, "PalSearch");
        if (s) html_dom_focus(s);
    }
}

int sg_editor_handle(SgEditor *ed, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!ed || argc < 1) return 1;
    const char *op = arg_s(args, argc, 0);
    const double x = ev ? ev->x : 0, y = ev ? ev->y : 0;
    const int button = ev ? ev->button : 0;
    const int mods = ev ? ev->mods : 0;
    const int shift = (mods & 1) != 0, ctrl = (mods & (2 | 8)) != 0;

    if (!strcmp(op, "down")) { // the canvas (iíiíìîï, :122711-122744): pan, the selection cleared unless Shift
        const int had_menu = ed->pal || ed->tpl_open;
        close_palette(ed);
        double wx, wy;
        to_world(ed, x, y, &wx, &wy);
        char wn[ID_N], wp[ID_N];
        if (button == 0 && wire_hit(ed, wx, wy, wn, wp)) { // a wire (iîiíîîi, :122863-122874)
            sel_clear(ed);
            ed->wire_sel = 1;
            snprintf(ed->wire_node, sizeof(ed->wire_node), "%s", wn);
            snprintf(ed->wire_port, sizeof(ed->wire_port), "%s", wp);
            refresh(ed);
            focus_canvas(ed);
            return 1;
        }
        int cleared = 0;
        if (button != 2) {
            if (!shift) {
                cleared = ed->nsel || ed->wire_sel;
                sel_clear(ed);
                ed->wire_sel = 0;
            }
            ed->drag = DRAG_PAN;
            ed->sx = x, ed->sy = y, ed->ovx = ed->vx, ed->ovy = ed->vy;
            ed->moved = 0;
        }
        if (had_menu) refresh(ed);
        else if (cleared) update_selection(ed);
        focus_canvas(ed);
        return 1;
    }
    if (!strcmp(op, "nodeDown")) { // iIìîîiî (:123178-123203)
        html_ui_stop_propagation();
        const char *id = arg_s(args, argc, 1);
        const int had_menu = ed->pal || ed->tpl_open;
        close_palette(ed);
        if (button != 0) {
            if (had_menu) refresh(ed);
            return 1;
        }
        ed->wire_sel = 0;
        if (shift || ctrl) {
            if (selected(ed, id)) sel_remove(ed, id);
            else sel_add(ed, id);
        } else if (!selected(ed, id)) {
            sel_clear(ed);
            sel_add(ed, id);
        }
        if (!ed->readonly && selected(ed, id)) {
            ed->drag = DRAG_NODES;
            ed->lastx = x, ed->lasty = y;
            ed->moved = 0;
        }
        if (had_menu) refresh(ed);
        else update_selection(ed);
        focus_canvas(ed);
        return 1;
    }
    if (!strcmp(op, "portDown")) { // iÏîïiii (:123204-123251)
        html_ui_stop_propagation();
        if (button != 0 || ed->readonly) return 1;
        close_palette(ed);
        const char *node = arg_s(args, argc, 1), *port = arg_s(args, argc, 2), *dir = arg_s(args, argc, 3);
        const cJSON *n = node_by_id(ed, node);
        if (!n) return 1;
        double wx, wy;
        to_world(ed, x, y, &wx, &wy);
        const cJSON *ln = !strcmp(dir, "in") ? link_of(n, port) : NULL;
        if (ln) { // a connected input: its wire comes off and follows the pointer from its output
            const char *src = jstr(ln, "node"), *sp = jstr(ln, "port");
            const cJSON *sn = node_by_id(ed, src);
            const Layout sl = layout_of(ed, sn);
            const cJSON *op_ = sn && sp[0] ? port_def(&sl, sp, 1) : NULL;
            if (sn && !op_) op_ = cJSON_GetArrayItem(sl.outs, 0);
            cJSON *g = edit_copy(ed);
            disconnect(g, node, port);
            set_graph(ed, g, NULL);
            if (!sn || !op_) {
                history_push(ed, "");
                refresh(ed);
                return 1;
            }
            ed->drag = DRAG_WIRE;
            snprintf(ed->dl.node, ID_N, "%s", src);
            snprintf(ed->dl.port, ID_N, "%s", jstr(op_, "name"));
            snprintf(ed->dl.dir, sizeof(ed->dl.dir), "out");
            snprintf(ed->dl.type, sizeof(ed->dl.type), "%s", jstr(op_, "type"));
            ed->detached = 1;
        } else {
            const Layout l = layout_of(ed, n);
            const cJSON *pd = port_def(&l, port, !strcmp(dir, "out"));
            ed->drag = DRAG_WIRE;
            snprintf(ed->dl.node, ID_N, "%s", node);
            snprintf(ed->dl.port, ID_N, "%s", port);
            snprintf(ed->dl.dir, sizeof(ed->dl.dir), "%s", dir);
            snprintf(ed->dl.type, sizeof(ed->dl.type), "%s", pd && jstr(pd, "type")[0] ? jstr(pd, "type") : "any");
            ed->detached = 0;
        }
        ed->wx = wx, ed->wy = wy;
        refresh(ed); // the ports' ok / bad marks
        focus_canvas(ed);
        return 1;
    }
    if (!strcmp(op, "move")) { // iííîìîï (:122745-122800)
        if (ed->drag == DRAG_NONE) return 1;
        if (ev && !(ev->key & 1)) { // the button came up outside the canvas
            end_drag(ed, x, y);
            return 1;
        }
        HtmlRect cr = {0};
        canvas_rect(ed, &cr);
        const double scale = canvas_scale(ed, &cr); // the pointer's px are the screen's (:122754-122762)
        if (ed->drag == DRAG_PAN) {
            ed->vx = ed->ovx + (x - ed->sx) / scale;
            ed->vy = ed->ovy + (y - ed->sy) / scale;
            ed->moved = 1;
            update_view(ed);
        } else if (ed->drag == DRAG_NODES) {
            const double dx = (x - ed->lastx) / (ed->vk * scale), dy = (y - ed->lasty) / (ed->vk * scale);
            ed->lastx = x, ed->lasty = y;
            if (dx != 0 || dy != 0) {
                ed->moved = 1;
                cJSON *g = edit_copy(ed);
                cJSON *n;
                cJSON_ArrayForEach(n, cJSON_GetObjectItemCaseSensitive(g, "nodes")) {
                    if (!selected(ed, jstr(n, "id"))) continue;
                    put(n, "x", cJSON_CreateNumber(round(jnum(n, "x", 0) + dx)));
                    put(n, "y", cJSON_CreateNumber(round(jnum(n, "y", 0) + dy)));
                }
                set_graph(ed, g, NULL);
                update_nodes_pos(ed);
                update_wires(ed);
            }
        } else if (ed->drag == DRAG_WIRE) {
            to_world(ed, x, y, &ed->wx, &ed->wy);
            update_wires(ed);
        }
        return 1;
    }
    if (!strcmp(op, "up")) {
        if (ed->drag != DRAG_NONE) end_drag(ed, x, y);
        return 1;
    }
    if (!strcmp(op, "ctx")) { // iîíìiìí (:122806-122815): right-click opens the palette there
        if (ed->readonly) return 0;
        double cx, cy, wx, wy;
        to_canvas(ed, x, y, &cx, &cy);
        to_world(ed, x, y, &wx, &wy);
        ed->drag = DRAG_NONE;
        open_palette(ed, cx, cy, wx, wy, NULL);
        refresh(ed);
        const HtmlElem s = el(ed, "PalSearch");
        if (s) html_dom_focus(s);
        return 0;
    }
    if (!strcmp(op, "wheel")) { // :123499-123510: exp(-deltaY * 0.0015) about the pointer
        double cx, cy;
        to_canvas(ed, x, y, &cx, &cy);
        zoom_at(ed, cx, cy, exp(-(ev ? ev->key : 0) * 0.0015));
        update_view(ed);
        return 0;
    }
    if (!strcmp(op, "key")) { // iíiìiìí (:122826-122842)
        const int k = ev ? ev->key : 0;
        if (html_key_is_escape(k)) {
            close_palette(ed);
            ed->io = IO_NONE;
            sel_clear(ed);
            ed->wire_sel = 0;
            refresh(ed);
            return 1;
        }
#ifdef __EMSCRIPTEN__
        const int del = k == 46 || k == 8;
#else
        const int del = k == 261 || k == 259;
#endif
        if (ctrl && k == 'Z') {
            if (shift) sg_editor_handle(ed, ev, (HtmlArg[]) {{HTML_ARG_STR, 0, "redo"}}, 1);
            else sg_editor_handle(ed, ev, (HtmlArg[]) {{HTML_ARG_STR, 0, "undo"}}, 1);
            return 0;
        }
        if (ctrl && k == 'Y') {
            sg_editor_handle(ed, ev, (HtmlArg[]) {{HTML_ARG_STR, 0, "redo"}}, 1);
            return 0;
        }
        if (ctrl && k == 'A') {
            sel_clear(ed);
            const cJSON *n;
            cJSON_ArrayForEach(n, nodes_of(ed)) sel_add(ed, jstr(n, "id"));
            update_selection(ed);
            return 0;
        }
        if (del) {
            delete_selection(ed);
            focus_canvas(ed);
            return 0;
        }
        if (k == 'F' && !ctrl) {
            fit(ed);
            update_view(ed);
        }
        return 1;
    }
    if (!strcmp(op, "undo") || !strcmp(op, "redo")) { // iÎììíïì / iIïìíii
        const int undo = !strcmp(op, "undo");
        if (undo ? ed->hidx <= 0 : ed->hidx >= ed->nhist - 1) return 1;
        ed->hidx += undo ? -1 : 1;
        restore(ed, ed->hist[ed->hidx]);
        refresh(ed);
        return 1;
    }
    if (!strcmp(op, "fit")) {
        fit(ed);
        update_view(ed);
        return 1;
    }
    if (!strcmp(op, "add")) { // iíïìììï (:122816-122825): the palette in the canvas's middle
        double cw, ch;
        canvas_size(ed, &cw, &ch);
        open_palette(ed, cw / 2 - 110, fmax(8, ch / 2 - 150), (cw / 2 - ed->vx) / ed->vk - 100, (ch / 2 - ed->vy) / ed->vk - 40, NULL);
        refresh(ed);
        const HtmlElem s = el(ed, "PalSearch");
        if (s) html_dom_focus(s);
        return 1;
    }
    if (!strcmp(op, "palSearch")) {
        snprintf(ed->psearch, sizeof(ed->psearch), "%s", arg_s(args, argc, 1));
        ed->pactive = 0;
        palette_relist(ed);
        return 1;
    }
    if (!strcmp(op, "palKey")) return palette_key(ed, ev);
    if (!strcmp(op, "palHover")) {
        const int i = (int) arg_n(args, argc, 1);
        if (i == ed->pactive) return 1;
        char sfx[24];
        snprintf(sfx, sizeof(sfx), "PalItem%d", ed->pactive);
        HtmlElem e = el(ed, sfx);
        if (e) html_dom_set_class(e, "active", 0);
        ed->pactive = i;
        snprintf(sfx, sizeof(sfx), "PalItem%d", i);
        e = el(ed, sfx);
        if (e) html_dom_set_class(e, "active", 1);
        return 1;
    }
    if (!strcmp(op, "pick")) {
        pick(ed, arg_s(args, argc, 1));
        return 1;
    }
    if (!strcmp(op, "tplMenu")) { // iïïïííi: the palette closes, the menu toggles
        ed->pal = 0;
        ed->tpl_open = !ed->tpl_open;
        refresh(ed);
        return 1;
    }
    if (!strcmp(op, "tpl")) { // iÏîîííï (:123437-123444)
        const char *tid = arg_s(args, argc, 1);
        const cJSON *t;
        cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(catalogue(), "templates")) {
            const char *id = cJSON_GetStringValue(cJSON_GetArrayItem(t, 0));
            if (!id || strcmp(id, tid)) continue;
            cJSON *g = cJSON_Duplicate(cJSON_GetArrayItem(t, 1), 1);
            put(g, "kind", cJSON_CreateString(ed->kind));
            set_graph(ed, g, "");
            ed->tpl_open = 0;
            sel_clear(ed);
            ed->wire_sel = 0;
            ed->fit_due = 1;
            refresh(ed);
            break;
        }
        return 1;
    }
    if (!strcmp(op, "import") || !strcmp(op, "export")) { // iíííîïï / iiîïíiï
        close_palette(ed);
        ed->io_err[0] = 0;
        ed->io = !strcmp(op, "import") ? IO_IMPORT : IO_EXPORT;
        refresh(ed);
        return 1;
    }
    if (!strcmp(op, "ioClose")) {
        ed->io = IO_NONE;
        refresh(ed);
        return 1;
    }
    if (!strcmp(op, "ioInput")) { // the Load button follows the text
        const HtmlElem t = el(ed, "IoText"), b = el(ed, "IoLoad");
        char probe[64] = "";
        if (t) html_dom_get_value(t, probe, sizeof(probe));
        int empty = 1;
        for (const char *p = probe; *p && empty; p++) empty = isspace((unsigned char) *p) != 0;
        if (b) {
            if (empty && strlen(probe) < sizeof(probe) - 1) html_dom_set_attr(b, "disabled", "");
            else html_dom_remove_attr(b, "disabled");
        }
        return 1;
    }
    if (!strcmp(op, "ioCopy")) { // iïîîìîí
        char *text = cJSON_Print(ed->graph);
        StrBuf t = {0};
        sb_append_replace_all(&t, text ? text : "", "\t", "  ");
        char *s = sb_finish(&t);
        html_clipboard_write(s);
        free(s);
        free(text);
        message(ed, tr("shaderGraph.copied", "Copied to clipboard"));
        update_footer(ed);
        return 1;
    }
    if (!strcmp(op, "ioLoad")) { // iÌiíîïî (:123445-123455)
        const HtmlElem t = el(ed, "IoText");
        const size_t cap = 4u * 65536u;
        char *text = calloc(1, cap);
        if (!text) return 1;
        const char *given = arg_s(args, argc, 1); // a scripted Load carries the text
        if (given[0]) snprintf(text, cap, "%s", given);
        else if (t) html_dom_get_value(t, text, cap);
        const char *end = NULL;
        cJSON *g = cJSON_ParseWithOpts(text, &end, 1);
        if (!g) {
            const char *f = tr("shaderGraph.err.json", "Invalid JSON: {0}");
            char why[96];
            snprintf(why, sizeof(why), "Unexpected token at position %d", end ? (int) (end - text) : 0);
            StrBuf b = {0};
            sb_append_replace_all(&b, f, "{0}", why);
            char *s = sb_finish(&b);
            snprintf(ed->io_err, sizeof(ed->io_err), "%s", s);
            free(s);
        } else if (!graph_doc(g)) {
            cJSON_Delete(g);
            snprintf(ed->io_err, sizeof(ed->io_err), "%s", tr("shaderGraph.err.notGraph", "Not a shader graph document (needs v, nodes and output)"));
        } else {
            put(g, "kind", cJSON_CreateString(ed->kind));
            set_graph(ed, g, "");
            ed->io = IO_NONE;
            sel_clear(ed);
            ed->wire_sel = 0;
            ed->fit_due = 1;
        }
        free(text);
        refresh(ed);
        return 1;
    }
    if (!strcmp(op, "errNode")) { // iÌîîììî (:123456-123466): the node selected and centred
        const char *id = arg_s(args, argc, 1);
        const cJSON *n = id[0] ? node_by_id(ed, id) : NULL;
        if (!n) return 1;
        sel_clear(ed);
        sel_add(ed, id);
        ed->wire_sel = 0;
        const Layout l = layout_of(ed, n);
        double cw, ch;
        canvas_size(ed, &cw, &ch);
        ed->vx = cw / 2 - (l.x + NODE_W / 2.0) * ed->vk;
        ed->vy = ch / 2 - (l.y + l.h / 2) * ed->vk;
        update_selection(ed);
        update_view(ed);
        return 1;
    }
    if (!strcmp(op, "param") || !strcmp(op, "input")) { // a widget's onInput: no history entry until its change
        const char *node = arg_s(args, argc, 1), *name = arg_s(args, argc, 2), *how = arg_s(args, argc, 3);
        const char *text = argc > 4 && args[4].kind != HTML_ARG_THIS_CHECKED ? arg_s(args, argc, 4) : "";
        const int checked = argc > 4 ? arg_bool(args, argc, 4, ev) : 0;
        const int is_input = !strcmp(op, "input");
        cJSON *v = widget_value(ed, node, name, is_input, how, text, checked);
        if (!v && !strcmp(how, "hex")) return 1;
        const int type_change = !is_input && !strcmp(name, "type");
        if (is_input) set_input(ed, node, name, v);
        else set_param(ed, node, name, v);
        update_footer(ed);
        if (type_change) refresh(ed); // a param node's default editor follows its type
        return 1;
    }
    if (!strcmp(op, "unset")) { // iîíîïìï: the default of the widget, then the commit
        const char *w = arg_s(args, argc, 3);
        set_param(ed, arg_s(args, argc, 1), arg_s(args, argc, 2),
                  !strcmp(w, "color") ? cJSON_CreateString("#ffffff") : !strcmp(w, "bool") ? cJSON_CreateTrue() : cJSON_CreateNumber(0));
        history_push(ed, "widget");
        refresh(ed);
        return 1;
    }
    if (!strcmp(op, "clear")) { // iïííïìí
        set_param(ed, arg_s(args, argc, 1), arg_s(args, argc, 2), NULL);
        history_push(ed, "widget");
        refresh(ed);
        return 1;
    }
    if (!strcmp(op, "commit")) { // onCommit (iÍîïíiî): the history's "widget" entry
        history_push(ed, "widget");
        update_toolbar_history(ed);
        return 1;
    }
    // scripted (tests, the headless UI): a node of a type at a world point, a connection, the selection
    if (!strcmp(op, "addAt")) {
        ed->pwx = arg_n(args, argc, 2);
        ed->pwy = arg_n(args, argc, 3);
        ed->plink = 0;
        pick(ed, arg_s(args, argc, 1));
        return 1;
    }
    if (!strcmp(op, "connect")) {
        const char *fn = arg_s(args, argc, 1), *fp = arg_s(args, argc, 2), *tn = arg_s(args, argc, 3), *tp = arg_s(args, argc, 4);
        const cJSON *a = node_by_id(ed, fn), *b = node_by_id(ed, tn);
        if (!a || !b || ed->readonly) return 1;
        const Layout la = layout_of(ed, a), lb = layout_of(ed, b);
        const cJSON *pa = port_def(&la, fp, 1), *pb = port_def(&lb, tp, 0);
        char buf[160];
        const char *err = connect_error(ed, fn, pa ? jstr(pa, "type") : "any", tn, pb ? jstr(pb, "type") : "any", buf, sizeof(buf));
        if (err) message(ed, err);
        else {
            cJSON *g = edit_copy(ed);
            connect(g, fn, !strcmp(fp, first_output(ed, fn)) ? NULL : fp, tn, tp);
            set_graph(ed, g, "");
        }
        refresh(ed);
        return 1;
    }
    if (!strcmp(op, "select")) {
        sel_clear(ed);
        ed->wire_sel = 0;
        for (int i = 1; i < argc; i++) sel_add(ed, arg_s(args, argc, i));
        update_selection(ed);
        return 1;
    }
    if (!strcmp(op, "delete")) {
        delete_selection(ed);
        return 1;
    }
    return 1;
}
