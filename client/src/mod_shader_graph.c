// Shader graphs -> GLSL (client/include/mod_shader.h): Krunker v10.0.7's graph validator (deob-10.0.7.js:94255-94792)
// and its compiler (iìïîíìì, deob:94824-95560) for kind "post", which builds three.js TSL nodes. Here each node
// becomes GLSL statements in one fragment shader, with the same width rules: an input is converted to the width
// the node asks for (iiïîìíî, deob:94736-94738: broadcast, truncate, or pad with 0 then 1 for w), arithmetic takes
// the widest operand (iÎïiîiì), vector-only functions at least vec2 (iÎïiîìî).
//
// The TSL functions behind the nodes (three.js r18x: MaterialX noise, colour adjustments, oscillators, UV
// helpers) are written out in GLSL below (k_lib_*). In a post pass the mesh inputs describe the full-screen quad
// (Krunker's QuadMesh with its orthographic camera): normal (0,0,1), position its NDC, camera near 0 / far 1;
// texture slots are unbound there (ctx.textures is unset for post, deob:205861-205874), so texture nodes read
// white (iîíííîî -> vec4(1), deob:94812-94815) and textureSize (1,1).
// Documented differences: viewportSharedTexture samples the pass input (three copies the pass's own target,
// which holds the frame from two passes ago); viewportDepthTexture / viewportLinearDepth read 1 (a pass target's
// depth is never written: the far plane).
#include <mod_shader.h>
#include <cJSON.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MSG_T_ANY = 0, MSG_T_FLOAT = 1, MSG_T_VEC2 = 2, MSG_T_VEC3 = 3, MSG_T_VEC4 = 4 };
enum { MSG_P_NUMBER, MSG_P_VEC2, MSG_P_VEC3, MSG_P_VEC4, MSG_P_COLOR, MSG_P_ENUM, MSG_P_STRING, MSG_P_VALUE, MSG_P_BOOL };

typedef struct {
    const char *name;
    int type;   // MSG_T_*
    int ndef;   // 0 = default null
    double def[4];
} MsgPort;

typedef struct {
    const char *name;
    int type; // MSG_P_*
    int optional, null_default, ndef;
    double def[4];
    const char *sdef; // string / color / enum default
    int bdef;
    const char *enums; // "a|b|c"
    int has_min;
    double min;
    int has_max;
    double max;
    int integer;
} MsgParamDef;

typedef struct {
    const char *type;
    int kinds; // 0 every kind, 1 post graphs only
    int nin;
    MsgPort in[6];
    int nout;
    MsgPort out[4];
    int nparams;
    MsgParamDef params[6];
} MsgNodeDef;

#include "mod_shader_nodes.inc"

#define MSG_NODE_COUNT ((int) (sizeof(MSG_NODES) / sizeof(MSG_NODES[0])))

static const MsgNodeDef *node_def(const char *type) {
    if (!type) return NULL;
    for (int i = 0; i < MSG_NODE_COUNT; i++)
        if (!strcmp(MSG_NODES[i].type, type)) return &MSG_NODES[i];
    return NULL;
}

// the post kind's output ports (iíîíîií.post, deob:94232-94236): one vec4 colour, no params
static const MsgPort k_post_output = {"color", MSG_T_VEC4, 0, {0}};

// ---------------------------------------------------------------- small helpers

typedef struct {
    char *data;
    size_t len, cap;
    int oom;
} Buf;

static void buf_add(Buf *b, const char *s, size_t n) {
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        char *grown = realloc(b->data, cap);
        if (!grown) {
            b->oom = 1;
            return;
        }
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

static void buf_puts(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }

static void buf_printf(Buf *b, const char *fmt, ...) {
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t) n < sizeof(tmp)) {
        buf_add(b, tmp, (size_t) n);
        return;
    }
    char *big = malloc((size_t) n + 1);
    if (!big) {
        b->oom = 1;
        return;
    }
    va_start(ap, fmt);
    vsnprintf(big, (size_t) n + 1, fmt, ap);
    va_end(ap);
    buf_add(b, big, (size_t) n);
    free(big);
}

static void set_err(char *err, const size_t n, const char *id, const char *fmt, ...) {
    if (!err || !n || err[0]) return; // the first error is kept
    char msg[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (id) snprintf(err, n, "%s: %s", id, msg);
    else snprintf(err, n, "%s", msg);
}

static int is_obj(const cJSON *j) { return j && cJSON_IsObject(j); }
static int is_finite_num(const cJSON *j) { return j && cJSON_IsNumber(j) && isfinite(j->valuedouble); }

static int match_id(const char *s) { // ^[A-Za-z0-9_-]{1,32}$
    const size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > 32) return 0;
    for (size_t i = 0; i < n; i++)
        if (!(s[i] == '_' || s[i] == '-' || (s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z'))) return 0;
    return 1;
}

static int match_param_name(const char *s) { // ^[A-Za-z_][A-Za-z0-9_]{0,31}$
    const size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > 32) return 0;
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        const int alpha = c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!(alpha || (i && c >= '0' && c <= '9'))) return 0;
    }
    return 1;
}

static int match_swizzle(const char *s) { // ^[xyzw]{1,4}$
    const size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > 4) return 0;
    for (size_t i = 0; i < n; i++)
        if (!strchr("xyzw", s[i])) return 0;
    return 1;
}

static int match_color(const cJSON *j) { // ^#[0-9a-fA-F]{6}$
    if (!cJSON_IsString(j) || strlen(j->valuestring) != 7 || j->valuestring[0] != '#') return 0;
    for (int i = 1; i < 7; i++) {
        const char c = j->valuestring[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
    }
    return 1;
}

static void color_rgb(const char *hex, float rgb[3]) { // iïïïìíï (deob:94742-94745)
    const unsigned long v = strtoul(hex + 1, NULL, 16);
    rgb[0] = (float) ((v >> 16) & 0xff) / 255.0f;
    rgb[1] = (float) ((v >> 8) & 0xff) / 255.0f;
    rgb[2] = (float) (v & 0xff) / 255.0f;
}

// an array of exactly n finite numbers
static int num_array(const cJSON *j, const int n) {
    if (!cJSON_IsArray(j) || cJSON_GetArraySize(j) != n) return 0;
    for (const cJSON *e = j->child; e; e = e->next)
        if (!is_finite_num(e)) return 0;
    return 1;
}

static int too_big(const cJSON *j) { // iíîîiîî: a magnitude above maxAbsValue
    if (cJSON_IsArray(j)) {
        for (const cJSON *e = j->child; e; e = e->next)
            if (too_big(e)) return 1;
        return 0;
    }
    return cJSON_IsNumber(j) && fabs(j->valuedouble) > MSG_MAX_ABS;
}

// iíîíîiï: 1 for a finite number, n for an array of 1..4 finite numbers, else 0
static int value_width(const cJSON *j) {
    if (is_finite_num(j)) return 1;
    if (!cJSON_IsArray(j)) return 0;
    const int n = cJSON_GetArraySize(j);
    if (n < 1 || n > 4) return 0;
    for (const cJSON *e = j->child; e; e = e->next)
        if (!is_finite_num(e)) return 0;
    return n;
}

static int enum_has(const char *enums, const char *v) {
    if (!enums || !v) return 0;
    const size_t n = strlen(v);
    for (const char *p = enums; *p;) {
        const char *bar = strchr(p, '|');
        const size_t m = bar ? (size_t) (bar - p) : strlen(p);
        if (m == n && !strncmp(p, v, n)) return 1;
        if (!bar) break;
        p = bar + 1;
    }
    return 0;
}

// ---------------------------------------------------------------- validation (deob:94280-94792)

// iïìíiíï: NULL when `v` fits the param spec, else the message
static const char *param_error(const MsgParamDef *p, const cJSON *v, char *tmp, const size_t n) {
    switch (p->type) {
        case MSG_P_NUMBER:
            if (!is_finite_num(v)) return "must be a finite number";
            if (too_big(v)) return "is too large";
            if (p->integer && v->valuedouble != floor(v->valuedouble)) return "must be an integer";
            if (p->has_min && v->valuedouble < p->min) { snprintf(tmp, n, "must be >= %g", p->min); return tmp; }
            if (p->has_max && v->valuedouble > p->max) { snprintf(tmp, n, "must be <= %g", p->max); return tmp; }
            return NULL;
        case MSG_P_STRING: return cJSON_IsString(v) && strlen(v->valuestring) <= 64 ? NULL : "must be a string (<= 64 chars)";
        case MSG_P_BOOL: return cJSON_IsBool(v) ? NULL : "must be a boolean";
        case MSG_P_COLOR: return match_color(v) ? NULL : "must be a #rrggbb color";
        case MSG_P_ENUM: return cJSON_IsString(v) && enum_has(p->enums, v->valuestring) ? NULL : "is not one of its values";
        case MSG_P_VEC2:
        case MSG_P_VEC3:
        case MSG_P_VEC4: {
            const int w = p->type - MSG_P_VEC2 + 2;
            if (!num_array(v, w)) { snprintf(tmp, n, "must be an array of %d finite numbers", w); return tmp; }
            return too_big(v) ? "is too large" : NULL;
        }
        case MSG_P_VALUE:
            if (match_color(v)) return NULL;
            if (!value_width(v)) return "must be a number, a number[1..4] or a #rrggbb color";
            return too_big(v) ? "is too large" : NULL;
        default: return "unknown param type";
    }
}

// iîïîíîî: a node's params
static void check_params(const cJSON *node, const char *id, const char *type, const MsgNodeDef *def, char *err, const size_t errn) {
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(node, "params");
    const int is_output = !strcmp(type, "output");
    if (!params) {
        if (!strcmp(type, "param")) set_err(err, errn, id, "param node requires params.name");
        return;
    }
    if (!is_obj(params)) {
        set_err(err, errn, id, "params must be an object");
        return;
    }
    for (const cJSON *p = params->child; p; p = p->next) {
        const MsgParamDef *spec = NULL;
        if (!is_output) // the post kind's output node takes no params (iíîíîií.post.params, deob:94235)
            for (int i = 0; i < def->nparams; i++)
                if (!strcmp(def->params[i].name, p->string)) spec = &def->params[i];
        if (!spec) {
            set_err(err, errn, id, "unknown param \"%s\" for %s", p->string, type);
            continue;
        }
        if (cJSON_IsNull(p)) {
            if (!spec->optional) set_err(err, errn, id, "param \"%s\" must not be null", p->string);
            continue;
        }
        char tmp[64];
        const char *e = param_error(spec, p, tmp, sizeof(tmp));
        if (e) set_err(err, errn, id, "param \"%s\" %s", p->string, e);
    }
    if (!strcmp(type, "param")) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
        if (!cJSON_IsString(name) || !match_param_name(name->valuestring)) set_err(err, errn, id, "param name is not a valid name");
        else if (!strcmp(name->valuestring, "__proto__") || !strcmp(name->valuestring, "constructor") ||
                 !strcmp(name->valuestring, "prototype"))
            set_err(err, errn, id, "param name \"%s\" is reserved", name->valuestring);
        // iÌííïïî (deob:94336-94350): the default fits the param's type
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(params, "type");
        const char *ptype = cJSON_IsString(t) ? t->valuestring : "float";
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(params, "default");
        if (d) {
            if (!strcmp(ptype, "color")) {
                if (!match_color(d)) set_err(err, errn, id, "default must be a #rrggbb color for type color");
            } else {
                const int w = !strcmp(ptype, "vec2") ? 2 : !strcmp(ptype, "vec3") ? 3 : !strcmp(ptype, "vec4") ? 4 : !strcmp(ptype, "float") ? 1 : 0;
                if (w == 1 && !is_finite_num(d)) set_err(err, errn, id, "default must be a number for type float");
                else if (w > 1 && !num_array(d, w)) set_err(err, errn, id, "default must be an array of %d numbers for type %s", w, ptype);
            }
        }
        const cJSON *lo = cJSON_GetObjectItemCaseSensitive(params, "min"), *hi = cJSON_GetObjectItemCaseSensitive(params, "max");
        if (is_finite_num(lo) && is_finite_num(hi) && lo->valuedouble > hi->valuedouble) set_err(err, errn, id, "param min must be <= max");
    }
    if (!strcmp(type, "swizzle")) {
        const cJSON *mask = cJSON_GetObjectItemCaseSensitive(params, "mask");
        if (cJSON_IsString(mask) && !match_swizzle(mask->valuestring)) set_err(err, errn, id, "swizzle mask must be 1-4 of xyzw");
    }
}

static const cJSON *find_node(const cJSON *nodes, const char *id) {
    for (const cJSON *n = nodes ? nodes->child : NULL; n; n = n->next) {
        const cJSON *nid = cJSON_GetObjectItemCaseSensitive(n, "id");
        if (is_obj(n) && cJSON_IsString(nid) && !strcmp(nid->valuestring, id)) return n;
    }
    return NULL;
}

// iiíìïîì: a node's inputs
static void check_inputs(const cJSON *node, const char *id, const char *type, const MsgNodeDef *def, const cJSON *nodes, char *err,
                         const size_t errn) {
    const cJSON *inputs = cJSON_GetObjectItemCaseSensitive(node, "inputs");
    if (!inputs) return;
    if (!is_obj(inputs)) {
        set_err(err, errn, id, "inputs must be an object");
        return;
    }
    const int is_output = !strcmp(type, "output");
    for (const cJSON *in = inputs->child; in; in = in->next) {
        int known = is_output ? !strcmp(in->string, k_post_output.name) : 0;
        for (int i = 0; !is_output && i < def->nin && !known; i++) known = !strcmp(def->in[i].name, in->string);
        if (!known) {
            set_err(err, errn, id, "unknown input port \"%s\" for %s", in->string, type);
            continue;
        }
        if (!is_obj(in)) {
            set_err(err, errn, id, "input \"%s\" must be an object", in->string);
            continue;
        }
        const cJSON *ref = cJSON_GetObjectItemCaseSensitive(in, "node"), *val = cJSON_GetObjectItemCaseSensitive(in, "value");
        const cJSON *port = cJSON_GetObjectItemCaseSensitive(in, "port");
        if (!ref == !val) {
            set_err(err, errn, id, "input \"%s\" must have exactly one of node or value", in->string);
            continue;
        }
        for (const cJSON *k = in->child; k; k = k->next)
            if (strcmp(k->string, "node") && strcmp(k->string, "port") && strcmp(k->string, "value"))
                set_err(err, errn, id, "input \"%s\" has unknown key \"%s\"", in->string, k->string);
        if (val) {
            if (port) set_err(err, errn, id, "input \"%s\" value cannot have a port", in->string);
            if (!value_width(val)) set_err(err, errn, id, "input \"%s\" value must be a finite number or number[1..4]", in->string);
            else if (too_big(val)) set_err(err, errn, id, "input \"%s\" value is too large", in->string);
            continue;
        }
        const cJSON *src = cJSON_IsString(ref) ? find_node(nodes, ref->valuestring) : NULL;
        if (!src) {
            set_err(err, errn, id, "input \"%s\" references an unknown node", in->string);
            continue;
        }
        if (src == node) {
            set_err(err, errn, id, "input \"%s\" references itself", in->string);
            continue;
        }
        const MsgNodeDef *sd = node_def(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(src, "type")));
        if (!sd) continue;
        if (!sd->nout) {
            set_err(err, errn, id, "input \"%s\" references node \"%s\" which has no outputs", in->string, ref->valuestring);
            continue;
        }
        if (port) {
            int ok = 0;
            for (int i = 0; cJSON_IsString(port) && i < sd->nout && !ok; i++) ok = !strcmp(sd->out[i].name, port->valuestring);
            if (!ok) set_err(err, errn, id, "input \"%s\" references an unknown output port on %s", in->string, sd->type);
        }
    }
}

// the cycle check (deob:94746-94783): depth-first over the input references
static int cycle_visit(const cJSON *nodes, const cJSON **list, int *state, const int count, const int i) {
    if (state[i] == 1) return 1;
    if (state[i] == 2) return 0;
    state[i] = 1;
    const cJSON *inputs = cJSON_GetObjectItemCaseSensitive(list[i], "inputs");
    for (const cJSON *in = is_obj(inputs) ? inputs->child : NULL; in; in = in->next) {
        const cJSON *ref = is_obj(in) ? cJSON_GetObjectItemCaseSensitive(in, "node") : NULL;
        if (!cJSON_IsString(ref)) continue;
        const cJSON *src = find_node(nodes, ref->valuestring);
        for (int j = 0; src && j < count; j++)
            if (list[j] == src && cycle_visit(nodes, list, state, count, j)) return 1;
    }
    state[i] = 2;
    return 0;
}

int mod_shader_graph_validate(const cJSON *graph, const char *kind, char *err, const size_t err_size) {
    if (err && err_size) err[0] = 0;
    if (!is_obj(graph)) {
        set_err(err, err_size, NULL, "graph must be an object");
        return 0;
    }
    for (const cJSON *k = graph->child; k; k = k->next)
        if (strcmp(k->string, "v") && strcmp(k->string, "kind") && strcmp(k->string, "nodes") && strcmp(k->string, "output"))
            set_err(err, err_size, NULL, "unknown graph key \"%s\"", k->string);
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(graph, "v");
    if (!cJSON_IsNumber(v) || v->valuedouble != 1.0) set_err(err, err_size, NULL, "v must be 1");
    const char *gkind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(graph, "kind"));
    int kind_ok = 0;
    for (size_t i = 0; gkind && i < sizeof(MSG_KINDS) / sizeof(MSG_KINDS[0]); i++) kind_ok |= !strcmp(MSG_KINDS[i], gkind);
    if (!kind_ok) {
        set_err(err, err_size, NULL, "kind must be one of surface, paint, post");
        return 0;
    }
    if (kind && strcmp(kind, gkind)) set_err(err, err_size, NULL, "kind must be %s", kind);
    char *text = cJSON_PrintUnformatted(graph);
    if (text && strlen(text) > MSG_MAX_BYTES) set_err(err, err_size, NULL, "graph exceeds %d bytes", MSG_MAX_BYTES);
    free(text);
    const cJSON *nodes = cJSON_GetObjectItemCaseSensitive(graph, "nodes");
    if (!cJSON_IsArray(nodes)) {
        set_err(err, err_size, NULL, "nodes must be an array");
        return 0;
    }
    const int count = cJSON_GetArraySize(nodes);
    if (count > MSG_MAX_NODES) set_err(err, err_size, NULL, "too many nodes (max %d)", MSG_MAX_NODES);
    if (!count) set_err(err, err_size, NULL, "graph has no nodes");
    const cJSON **list = calloc((size_t) (count ? count : 1), sizeof(*list));
    int *state = calloc((size_t) (count ? count : 1), sizeof(*state));
    if (!list || !state) {
        free(list);
        free(state);
        set_err(err, err_size, NULL, "out of memory");
        return 0;
    }
    int listed = 0, param_nodes = 0, outputs = 0, index = 0;
    for (const cJSON *n = nodes->child; n; n = n->next, index++) {
        if (!is_obj(n)) {
            set_err(err, err_size, NULL, "nodes[%d] must be an object", index);
            continue;
        }
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(n, "id"));
        if (!id || !match_id(id)) {
            set_err(err, err_size, NULL, "nodes[%d] id must be 1-32 of A-Za-z0-9_-", index);
            continue;
        }
        if (find_node(nodes, id) != n) {
            set_err(err, err_size, id, "duplicate node id");
            continue;
        }
        list[listed++] = n;
        for (const cJSON *k = n->child; k; k = k->next)
            if (strcmp(k->string, "id") && strcmp(k->string, "type") && strcmp(k->string, "params") && strcmp(k->string, "inputs") &&
                strcmp(k->string, "x") && strcmp(k->string, "y"))
                set_err(err, err_size, id, "unknown node key \"%s\"", k->string);
        const cJSON *x = cJSON_GetObjectItemCaseSensitive(n, "x"), *y = cJSON_GetObjectItemCaseSensitive(n, "y");
        if (x && !is_finite_num(x)) set_err(err, err_size, id, "x must be a finite number");
        if (y && !is_finite_num(y)) set_err(err, err_size, id, "y must be a finite number");
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(n, "type"));
        const MsgNodeDef *def = node_def(type);
        if (!def) {
            set_err(err, err_size, id, "unknown node type \"%s\"", type ? type : "");
            continue;
        }
        if (def->kinds == 1 && strcmp(gkind, "post")) set_err(err, err_size, id, "node type \"%s\" is only allowed in post graphs", type);
        if (!strcmp(type, "param")) param_nodes++;
        if (!strcmp(type, "output")) outputs++;
        check_params(n, id, type, def, err, err_size);
    }
    if (param_nodes > MSG_MAX_PARAM_NODES) set_err(err, err_size, NULL, "too many param nodes (max %d)", MSG_MAX_PARAM_NODES);
    for (int i = 0; i < listed; i++) { // duplicate param names
        const cJSON *pi = cJSON_GetObjectItemCaseSensitive(list[i], "params");
        const char *ti = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(list[i], "type"));
        const char *ni = is_obj(pi) ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(pi, "name")) : NULL;
        if (!ti || strcmp(ti, "param") || !ni) continue;
        for (int j = 0; j < i; j++) {
            const cJSON *pj = cJSON_GetObjectItemCaseSensitive(list[j], "params");
            const char *tj = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(list[j], "type"));
            const char *nj = is_obj(pj) ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(pj, "name")) : NULL;
            if (tj && !strcmp(tj, "param") && nj && !strcmp(ni, nj)) {
                set_err(err, err_size, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(list[i], "id")), "duplicate param name \"%s\"", ni);
                break;
            }
        }
    }
    for (int i = 0; i < listed; i++) {
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(list[i], "type"));
        const MsgNodeDef *def = node_def(type);
        if (def) check_inputs(list[i], cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(list[i], "id")), type, def, nodes, err, err_size);
    }
    if (outputs != 1) set_err(err, err_size, NULL, "graph must have exactly one output node (found %d)", outputs);
    const char *out = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(graph, "output"));
    const cJSON *on = out ? find_node(nodes, out) : NULL;
    if (!on) set_err(err, err_size, NULL, "output must reference an existing node");
    else if (strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(on, "type")) ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(on, "type")) : "", "output"))
        set_err(err, err_size, out, "output must reference a node of type output");
    for (int i = 0; i < listed; i++)
        if (cycle_visit(nodes, list, state, listed, i)) {
            set_err(err, err_size, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(list[i], "id")), "graph contains a cycle");
            break;
        }
    free(list);
    free(state);
    return !(err && err[0]);
}

// ---------------------------------------------------------------- the GLSL library (three.js TSL equivalents)

// MaterialX noise (three.js mx_noise: the lookup3 hash, Perlin gradient noise, cell noise, Worley, fractal sums)
static const char k_lib_noise[] =
    "uint kn_rotl(uint x, int k) { return (x << uint(k)) | (x >> uint(32 - k)); }\n"
    "void kn_bjmix(inout uint a, inout uint b, inout uint c) {\n"
    "  a -= c; a ^= kn_rotl(c, 4); c += b; b -= a; b ^= kn_rotl(a, 6); a += c;\n"
    "  c -= b; c ^= kn_rotl(b, 8); b += a; a -= c; a ^= kn_rotl(c, 16); c += b;\n"
    "  b -= a; b ^= kn_rotl(a, 19); a += c; c -= b; c ^= kn_rotl(b, 4); b += a;\n"
    "}\n"
    "uint kn_bjfinal(uint a, uint b, uint c) {\n"
    "  c ^= b; c -= kn_rotl(b, 14); a ^= c; a -= kn_rotl(c, 11); b ^= a; b -= kn_rotl(a, 25);\n"
    "  c ^= b; c -= kn_rotl(b, 16); a ^= c; a -= kn_rotl(c, 4); b ^= a; b -= kn_rotl(a, 14);\n"
    "  c ^= b; c -= kn_rotl(b, 24); return c;\n"
    "}\n"
    "uint kn_hash(int x, int y) { uint s = 0xdeadbeefu + (2u << 2u) + 13u; return kn_bjfinal(s + uint(x), s + uint(y), s); }\n"
    "uint kn_hash(int x, int y, int z) { uint s = 0xdeadbeefu + (3u << 2u) + 13u; return kn_bjfinal(s + uint(x), s + uint(y), s + uint(z)); }\n"
    "uint kn_hash(int x, int y, int z, int w) {\n"
    "  uint s = 0xdeadbeefu + (4u << 2u) + 13u; uint a = s + uint(x), b = s + uint(y), c = s + uint(z);\n"
    "  kn_bjmix(a, b, c); a += uint(w); return kn_bjfinal(a, b, c);\n"
    "}\n"
    "uvec3 kn_hash3(int x, int y) { uint h = kn_hash(x, y); return uvec3(h & 255u, (h >> 8) & 255u, (h >> 16) & 255u); }\n"
    "uvec3 kn_hash3(int x, int y, int z) { uint h = kn_hash(x, y, z); return uvec3(h & 255u, (h >> 8) & 255u, (h >> 16) & 255u); }\n"
    "float kn_bits01(uint b) { return float(b) / 4294967295.0; }\n"
    "float kn_fade(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }\n"
    "float kn_neg(float v, bool b) { return b ? -v : v; }\n"
    "float kn_grad(uint h, float x, float y) {\n"
    "  h &= 7u; float u = h < 4u ? x : y; float v = 2.0 * (h < 4u ? y : x);\n"
    "  return kn_neg(u, (h & 1u) != 0u) + kn_neg(v, (h & 2u) != 0u);\n"
    "}\n"
    "float kn_grad(uint h, float x, float y, float z) {\n"
    "  h &= 15u; float u = h < 8u ? x : y; float v = h < 4u ? y : ((h == 12u || h == 14u) ? x : z);\n"
    "  return kn_neg(u, (h & 1u) != 0u) + kn_neg(v, (h & 2u) != 0u);\n"
    "}\n"
    "float kn_bilerp(float a, float b, float c, float d, float s, float t) { return (1.0 - t) * (a * (1.0 - s) + b * s) + t * (c * (1.0 - s) + d * s); }\n"
    "vec3 kn_bilerp(vec3 a, vec3 b, vec3 c, vec3 d, float s, float t) { return (1.0 - t) * (a * (1.0 - s) + b * s) + t * (c * (1.0 - s) + d * s); }\n"
    "float kn_perlin(vec2 p) {\n"
    "  ivec2 i = ivec2(floor(p)); vec2 f = p - floor(p); float u = kn_fade(f.x), v = kn_fade(f.y);\n"
    "  float r = kn_bilerp(kn_grad(kn_hash(i.x, i.y), f.x, f.y), kn_grad(kn_hash(i.x + 1, i.y), f.x - 1.0, f.y),\n"
    "                      kn_grad(kn_hash(i.x, i.y + 1), f.x, f.y - 1.0), kn_grad(kn_hash(i.x + 1, i.y + 1), f.x - 1.0, f.y - 1.0), u, v);\n"
    "  return 0.6616 * r;\n"
    "}\n"
    "float kn_perlin(vec3 p) {\n"
    "  ivec3 i = ivec3(floor(p)); vec3 f = p - floor(p); float u = kn_fade(f.x), v = kn_fade(f.y), w = kn_fade(f.z);\n"
    "  float lo = kn_bilerp(kn_grad(kn_hash(i.x, i.y, i.z), f.x, f.y, f.z), kn_grad(kn_hash(i.x + 1, i.y, i.z), f.x - 1.0, f.y, f.z),\n"
    "                       kn_grad(kn_hash(i.x, i.y + 1, i.z), f.x, f.y - 1.0, f.z), kn_grad(kn_hash(i.x + 1, i.y + 1, i.z), f.x - 1.0, f.y - 1.0, f.z), u, v);\n"
    "  float hi = kn_bilerp(kn_grad(kn_hash(i.x, i.y, i.z + 1), f.x, f.y, f.z - 1.0), kn_grad(kn_hash(i.x + 1, i.y, i.z + 1), f.x - 1.0, f.y, f.z - 1.0),\n"
    "                       kn_grad(kn_hash(i.x, i.y + 1, i.z + 1), f.x, f.y - 1.0, f.z - 1.0),\n"
    "                       kn_grad(kn_hash(i.x + 1, i.y + 1, i.z + 1), f.x - 1.0, f.y - 1.0, f.z - 1.0), u, v);\n"
    "  return 0.982 * mix(lo, hi, w);\n"
    "}\n"
    "vec3 kn_grad3(uvec3 h, float x, float y) { return vec3(kn_grad(h.x, x, y), kn_grad(h.y, x, y), kn_grad(h.z, x, y)); }\n"
    "vec3 kn_grad3(uvec3 h, float x, float y, float z) { return vec3(kn_grad(h.x, x, y, z), kn_grad(h.y, x, y, z), kn_grad(h.z, x, y, z)); }\n"
    "vec3 kn_perlin3(vec2 p) {\n"
    "  ivec2 i = ivec2(floor(p)); vec2 f = p - floor(p); float u = kn_fade(f.x), v = kn_fade(f.y);\n"
    "  return 0.6616 * kn_bilerp(kn_grad3(kn_hash3(i.x, i.y), f.x, f.y), kn_grad3(kn_hash3(i.x + 1, i.y), f.x - 1.0, f.y),\n"
    "                            kn_grad3(kn_hash3(i.x, i.y + 1), f.x, f.y - 1.0), kn_grad3(kn_hash3(i.x + 1, i.y + 1), f.x - 1.0, f.y - 1.0), u, v);\n"
    "}\n"
    "vec3 kn_perlin3(vec3 p) {\n"
    "  ivec3 i = ivec3(floor(p)); vec3 f = p - floor(p); float u = kn_fade(f.x), v = kn_fade(f.y), w = kn_fade(f.z);\n"
    "  vec3 lo = kn_bilerp(kn_grad3(kn_hash3(i.x, i.y, i.z), f.x, f.y, f.z), kn_grad3(kn_hash3(i.x + 1, i.y, i.z), f.x - 1.0, f.y, f.z),\n"
    "                      kn_grad3(kn_hash3(i.x, i.y + 1, i.z), f.x, f.y - 1.0, f.z), kn_grad3(kn_hash3(i.x + 1, i.y + 1, i.z), f.x - 1.0, f.y - 1.0, f.z), u, v);\n"
    "  vec3 hi = kn_bilerp(kn_grad3(kn_hash3(i.x, i.y, i.z + 1), f.x, f.y, f.z - 1.0), kn_grad3(kn_hash3(i.x + 1, i.y, i.z + 1), f.x - 1.0, f.y, f.z - 1.0),\n"
    "                      kn_grad3(kn_hash3(i.x, i.y + 1, i.z + 1), f.x, f.y - 1.0, f.z - 1.0),\n"
    "                      kn_grad3(kn_hash3(i.x + 1, i.y + 1, i.z + 1), f.x - 1.0, f.y - 1.0, f.z - 1.0), u, v);\n"
    "  return 0.982 * mix(lo, hi, w);\n"
    "}\n"
    "float kn_cell(vec2 p) { ivec2 i = ivec2(floor(p)); return kn_bits01(kn_hash(i.x, i.y)); }\n"
    "float kn_cell(vec3 p) { ivec3 i = ivec3(floor(p)); return kn_bits01(kn_hash(i.x, i.y, i.z)); }\n"
    "vec3 kn_cell3(vec2 p) { ivec2 i = ivec2(floor(p)); return vec3(kn_bits01(kn_hash(i.x, i.y, 0)), kn_bits01(kn_hash(i.x, i.y, 1)), kn_bits01(kn_hash(i.x, i.y, 2))); }\n"
    "vec3 kn_cell3(vec3 p) { ivec3 i = ivec3(floor(p)); return vec3(kn_bits01(kn_hash(i.x, i.y, i.z, 0)), kn_bits01(kn_hash(i.x, i.y, i.z, 1)), kn_bits01(kn_hash(i.x, i.y, i.z, 2))); }\n"
    "float kn_wdist(vec2 p, ivec2 c, ivec2 o, float jitter, int metric) {\n"
    "  vec2 off = (kn_cell3(vec2(c + o)).xy - 0.5) * jitter + 0.5; vec2 d = vec2(c) + off - p;\n"
    "  if (metric == 2) return max(abs(d.x), abs(d.y)); if (metric == 1) return abs(d.x) + abs(d.y); return dot(d, d);\n"
    "}\n"
    "float kn_wdist(vec3 p, ivec3 c, ivec3 o, float jitter, int metric) {\n"
    "  vec3 off = (kn_cell3(vec3(c + o)) - 0.5) * jitter + 0.5; vec3 d = vec3(c) + off - p;\n"
    "  if (metric == 2) return max(max(abs(d.x), abs(d.y)), abs(d.z)); if (metric == 1) return abs(d.x) + abs(d.y) + abs(d.z); return dot(d, d);\n"
    "}\n"
    "vec3 kn_worley_n(vec2 p, float jitter, int metric) {\n"
    "  ivec2 o = ivec2(floor(p)); vec2 l = p - floor(p); vec3 s = vec3(1e6);\n"
    "  for (int x = -1; x <= 1; x++) for (int y = -1; y <= 1; y++) {\n"
    "    float d = kn_wdist(l, ivec2(x, y), o, jitter, metric);\n"
    "    if (d < s.x) { s.z = s.y; s.y = s.x; s.x = d; } else if (d < s.y) { s.z = s.y; s.y = d; } else if (d < s.z) s.z = d;\n"
    "  }\n"
    "  return metric == 0 ? sqrt(s) : s;\n"
    "}\n"
    "vec3 kn_worley_n(vec3 p, float jitter, int metric) {\n"
    "  ivec3 o = ivec3(floor(p)); vec3 l = p - floor(p); vec3 s = vec3(1e6);\n"
    "  for (int x = -1; x <= 1; x++) for (int y = -1; y <= 1; y++) for (int z = -1; z <= 1; z++) {\n"
    "    float d = kn_wdist(l, ivec3(x, y, z), o, jitter, metric);\n"
    "    if (d < s.x) { s.z = s.y; s.y = s.x; s.x = d; } else if (d < s.y) { s.z = s.y; s.y = d; } else if (d < s.z) s.z = d;\n"
    "  }\n"
    "  return metric == 0 ? sqrt(s) : s;\n"
    "}\n"
    "float kn_fractal(vec3 p, int octaves, float lacunarity, float diminish) {\n"
    "  float r = 0.0, a = 1.0; for (int i = 0; i < octaves; i++) { r += a * kn_perlin(p); a *= diminish; p *= lacunarity; } return r;\n"
    "}\n"
    "vec3 kn_fractal3(vec3 p, int octaves, float lacunarity, float diminish) {\n"
    "  vec3 r = vec3(0.0); float a = 1.0; for (int i = 0; i < octaves; i++) { r += a * kn_perlin3(p); a *= diminish; p *= lacunarity; } return r;\n"
    "}\n";

// colour, UV and oscillator helpers (three.js TSL: luminance with Rec. 709 coefficients, hue / saturation / vibrance,
// posterize, the sRGB transfer, MaterialX hsv, aastep, blend modes, rotate / rotateUV, spherizeUV, spritesheetUV,
// equirectUV / equirectDirection, the oscillators) and the voronoi / hash of the compiler itself (deob:94799-94801,
// :95252-95280)
static const char k_lib_common[] =
    "const float KN_PI = 3.141592653589793;\n"
    "float kn_lum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }\n"
    "vec3 kn_saturation(vec3 c, float a) { return mix(vec3(kn_lum(c)), c, a); }\n"
    "vec3 kn_vibrance(vec3 c, float a) { float avg = (c.r + c.g + c.b) / 3.0; float mx = max(c.r, max(c.g, c.b));\n"
    "  return mix(c, vec3(mx), (mx - avg) * a * -3.0); }\n"
    "vec3 kn_hue(vec3 c, float a) { vec3 k = vec3(0.57735); float co = cos(a);\n"
    "  return c * co + cross(k, c) * sin(a) + k * dot(k, c) * (1.0 - co); }\n"
    "vec3 kn_srgb_to_linear(vec3 c) { return mix(pow(c * 0.9478672986 + 0.0521327014, vec3(2.4)), c * 0.0773993808, vec3(lessThanEqual(c, vec3(0.04045)))); }\n"
    "vec3 kn_linear_to_srgb(vec3 c) { return mix(pow(c, vec3(0.41666)) * 1.055 - 0.055, c * 12.92, vec3(lessThanEqual(c, vec3(0.0031308)))); }\n"
    "vec3 kn_hsv2rgb(vec3 c) {\n"
    "  float s = c.y, v = c.z; if (s < 0.0001) return vec3(v);\n"
    "  float h = 6.0 * (c.x - floor(c.x)); int hi = int(trunc(h)); float f = h - float(hi);\n"
    "  float p = v * (1.0 - s), q = v * (1.0 - s * f), t = v * (1.0 - s * (1.0 - f));\n"
    "  if (hi == 0) return vec3(v, t, p); if (hi == 1) return vec3(q, v, p); if (hi == 2) return vec3(p, v, t);\n"
    "  if (hi == 3) return vec3(p, q, v); if (hi == 4) return vec3(t, p, v); return vec3(v, p, q);\n"
    "}\n"
    "vec3 kn_rgb2hsv(vec3 c) {\n"
    "  float mn = min(c.r, min(c.g, c.b)), mx = max(c.r, max(c.g, c.b)), d = mx - mn;\n"
    "  float s = mx > 0.0 ? d / mx : 0.0, h = 0.0;\n"
    "  if (s > 0.0) { if (c.r >= mx) h = (c.g - c.b) / d; else if (c.g >= mx) h = 2.0 + (c.b - c.r) / d; else h = 4.0 + (c.r - c.g) / d;\n"
    "    h /= 6.0; if (h < 0.0) h += 1.0; }\n"
    "  return vec3(h, s, mx);\n"
    "}\n"
    "float kn_aastep(float t, float x) { float w = length(vec2(dFdx(x), dFdy(x))) * 0.70710678118654757; return smoothstep(t - w, t + w, x); }\n"
    "vec3 kn_burn(vec3 a, vec3 b) { return 1.0 - min(vec3(1.0), (1.0 - a) / b); }\n"
    "vec3 kn_dodge(vec3 a, vec3 b) { return min(a / (1.0 - b), vec3(1.0)); }\n"
    "vec3 kn_screen(vec3 a, vec3 b) { return 1.0 - (1.0 - a) * (1.0 - b); }\n"
    "vec3 kn_overlay(vec3 a, vec3 b) { return mix(a * 2.0 * b, 1.0 - (1.0 - a) * 2.0 * (1.0 - b), step(vec3(0.5), a)); }\n"
    "vec4 kn_blend_color(vec4 a, vec4 b) { float o = b.a + a.a * (1.0 - b.a); return vec4((b.rgb * b.a + a.rgb * a.a * (1.0 - b.a)) / o, o); }\n"
    "vec2 kn_rotate2(vec2 p, float r) { float c = cos(r), s = sin(r); return mat2(c, s, -s, c) * p; }\n"
    "vec3 kn_rotate3(vec3 p, vec3 r) {\n"
    "  mat4 rx = mat4(vec4(1.0, 0.0, 0.0, 0.0), vec4(0.0, cos(r.x), -sin(r.x), 0.0), vec4(0.0, sin(r.x), cos(r.x), 0.0), vec4(0.0, 0.0, 0.0, 1.0));\n"
    "  mat4 ry = mat4(vec4(cos(r.y), 0.0, sin(r.y), 0.0), vec4(0.0, 1.0, 0.0, 0.0), vec4(-sin(r.y), 0.0, cos(r.y), 0.0), vec4(0.0, 0.0, 0.0, 1.0));\n"
    "  mat4 rz = mat4(vec4(cos(r.z), -sin(r.z), 0.0, 0.0), vec4(sin(r.z), cos(r.z), 0.0, 0.0), vec4(0.0, 0.0, 1.0, 0.0), vec4(0.0, 0.0, 0.0, 1.0));\n"
    "  return (rx * ry * rz * vec4(p, 1.0)).xyz;\n"
    "}\n"
    "vec2 kn_spherize(vec2 uv, float s, vec2 c) { vec2 d = uv - c; float d2 = dot(d, d); return uv + d * (d2 * d2 * s); }\n"
    "vec2 kn_spritesheet(vec2 count, vec2 uv, float frame) {\n"
    "  float n = floor(mod(frame, count.x * count.y)); float col = mod(n, count.x); float row = count.y - ceil((n + 1.0) / count.x);\n"
    "  return (uv + vec2(col, row)) / count;\n"
    "}\n"
    "vec2 kn_equirect_uv(vec3 d) { return vec2(atan(d.z, d.x) * (0.5 / KN_PI) + 0.5, asin(clamp(d.y, -1.0, 1.0)) / KN_PI + 0.5); }\n"
    "vec3 kn_equirect_dir(vec2 uv) { float phi = (uv.x - 0.5) * 2.0 * KN_PI, th = (uv.y - 0.5) * KN_PI;\n"
    "  return vec3(cos(th) * cos(phi), sin(th), cos(th) * sin(phi)); }\n"
    "float kn_osc_sine(float t) { return sin((t + 0.75) * KN_PI * 2.0) * 0.5 + 0.5; }\n"
    "float kn_osc_square(float t) { return round(fract(t)); }\n"
    "float kn_osc_triangle(float t) { return abs(fract(t + 0.5) * 2.0 - 1.0); }\n"
    "float kn_osc_sawtooth(float t) { return fract(t); }\n"
    "float kn_hash21(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }\n"
    "vec2 kn_voronoi(vec2 p) {\n"
    "  vec2 i = floor(p), f = fract(p); float dist = 8.0, id = 0.0;\n"
    "  for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {\n"
    "    vec2 o = vec2(float(x), float(y)); vec2 c = i + o; float h = kn_hash21(c), h2 = kn_hash21(c + vec2(17.13, 31.7));\n"
    "    float d = length(o + vec2(h, h2) - f); float s = step(d, dist); dist = mix(dist, d, s); id = mix(id, h, s);\n"
    "  }\n"
    "  return vec2(dist, id);\n"
    "}\n";

// ---------------------------------------------------------------- the compiler

typedef struct {
    char expr[40]; // a GLSL variable (or its component)
    int width;
} MsgVal;

typedef struct {
    int state; // 0 not yet, 1 compiling, 2 done
    int nout;
    const char *names[4];
    MsgVal out[4];
} MsgNodeState;

typedef struct {
    const cJSON *nodes;
    int count;
    const cJSON **node;
    const MsgNodeDef **def;
    MsgNodeState *st;
    Buf body;
    int noise;
    const cJSON *instance; // shaderArgs.params
    ModShaderProgram *prog;
    char *err;
    size_t errn;
    int failed;
    int serial; // the next temporary's number
} MsgCtx;

static const char *const TN[5] = {"float", "float", "vec2", "vec3", "vec4"};

static void fmt_num(char *out, const size_t n, const double v) {
    snprintf(out, n, "%.9g", v);
    if (!strpbrk(out, ".eEn")) strncat(out, ".0", n - strlen(out) - 1);
}

// a literal value (iîììíïí, deob:94746-94757) into a fresh variable
static void emit_var(MsgCtx *c, MsgVal *out, const int width, const char *fmt, ...) {
    snprintf(out->expr, sizeof(out->expr), "t%d", c->serial++);
    out->width = width;
    char rhs[1600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rhs, sizeof(rhs), fmt, ap);
    va_end(ap);
    buf_printf(&c->body, "  %s %s = %s;\n", TN[width], out->expr, rhs);
}

static void literal(MsgCtx *c, const double *v, const int n, MsgVal *out) {
    char parts[4][32];
    for (int i = 0; i < n; i++) fmt_num(parts[i], sizeof(parts[i]), v[i]);
    if (n == 1) emit_var(c, out, 1, "%s", parts[0]);
    else if (n == 2) emit_var(c, out, 2, "vec2(%s, %s)", parts[0], parts[1]);
    else if (n == 3) emit_var(c, out, 3, "vec3(%s, %s, %s)", parts[0], parts[1], parts[2]);
    else emit_var(c, out, 4, "vec4(%s, %s, %s, %s)", parts[0], parts[1], parts[2], parts[3]);
}

static int json_values(const cJSON *j, double *v) { // a number or 1..4 numbers
    const int w = value_width(j);
    if (w == 1 && cJSON_IsNumber(j)) v[0] = j->valuedouble;
    else {
        int i = 0;
        for (const cJSON *e = j ? j->child : NULL; e && i < 4; e = e->next) v[i++] = e->valuedouble;
    }
    return w;
}

// iiïîìíî (deob:94736-94738): `in` converted to `to` components
static void convert(char *out, const size_t n, const MsgVal *in, const int to) {
    const int from = in->width;
    if (from == to) snprintf(out, n, "%s", in->expr);
    else if (from == 1) snprintf(out, n, "%s(%s)", TN[to], in->expr);
    else if (from > to) snprintf(out, n, "%s.%s", in->expr, to == 1 ? "x" : to == 2 ? "xy" : "xyz");
    else if (from == 2 && to == 3) snprintf(out, n, "vec3(%s, 0.0)", in->expr);
    else if (from == 2 && to == 4) snprintf(out, n, "vec4(%s, 0.0, 1.0)", in->expr);
    else snprintf(out, n, "vec4(%s, 1.0)", in->expr);
}

static int compile_node(MsgCtx *c, int index);

static int node_index(const MsgCtx *c, const char *id) {
    for (int i = 0; i < c->count; i++)
        if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(c->node[i], "id")), id)) return i;
    return -1;
}

static const MsgPort *def_input(const MsgNodeDef *d, const char *port) {
    for (int i = 0; i < d->nin; i++)
        if (!strcmp(d->in[i].name, port)) return &d->in[i];
    return NULL;
}

// iÌìiìíi (deob:95002-95475): what feeds `port` of node `ni` (a literal or another node's output); 0 = unconnected
static int connected(MsgCtx *c, const int ni, const char *port, MsgVal *out) {
    const cJSON *inputs = cJSON_GetObjectItemCaseSensitive(c->node[ni], "inputs");
    const cJSON *in = is_obj(inputs) ? cJSON_GetObjectItemCaseSensitive(inputs, port) : NULL;
    if (!is_obj(in)) return 0;
    const cJSON *val = cJSON_GetObjectItemCaseSensitive(in, "value");
    if (val) {
        double v[4];
        const int w = json_values(val, v);
        if (!w) return 0;
        literal(c, v, w, out);
        return 1;
    }
    const char *ref = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(in, "node"));
    const int src = ref ? node_index(c, ref) : -1;
    if (src < 0 || !compile_node(c, src)) return 0;
    const MsgNodeState *s = &c->st[src];
    if (!s->nout) return 0;
    const char *want = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(in, "port"));
    if (!want) {
        *out = s->out[0]; // Object.keys(outputs)[0]
        return 1;
    }
    for (int i = 0; i < s->nout; i++)
        if (!strcmp(s->names[i], want)) {
            *out = s->out[i];
            return 1;
        }
    return 0;
}

// iIiiíií: the width an input arrives with (unconnected: its default's, 1 for a null default)
static int in_width(MsgCtx *c, const int ni, const char *port) {
    const cJSON *inputs = cJSON_GetObjectItemCaseSensitive(c->node[ni], "inputs");
    const cJSON *in = is_obj(inputs) ? cJSON_GetObjectItemCaseSensitive(inputs, port) : NULL;
    if (is_obj(in)) {
        const cJSON *val = cJSON_GetObjectItemCaseSensitive(in, "value");
        if (val) return value_width(val) ? value_width(val) : 1;
        const char *ref = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(in, "node"));
        const int src = ref ? node_index(c, ref) : -1;
        if (src >= 0 && compile_node(c, src) && c->st[src].nout) {
            const char *want = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(in, "port"));
            if (!want) return c->st[src].out[0].width;
            for (int i = 0; i < c->st[src].nout; i++)
                if (!strcmp(c->st[src].names[i], want)) return c->st[src].out[i].width;
        }
    }
    const MsgPort *p = def_input(c->def[ni], port);
    return p && p->ndef ? p->ndef : 1;
}

// iÏìiîìì: the input at `width` (unconnected: its default), 0 when it has neither
static int input(MsgCtx *c, const int ni, const char *port, const int width, char *out, const size_t n) {
    MsgVal v;
    if (!connected(c, ni, port, &v)) {
        const MsgPort *p = def_input(c->def[ni], port);
        if (!p || !p->ndef) return 0;
        literal(c, p->def, p->ndef, &v);
    }
    convert(out, n, &v, width);
    return 1;
}

#define IN(port, w, var) char var[256]; const int has_##var = input(c, ni, port, w, var, sizeof(var)); (void) has_##var
#define OUT1(w, ...) do { emit_var(c, &s->out[0], w, __VA_ARGS__); s->names[0] = "out"; s->nout = 1; } while (0)

static int maxi(const int a, const int b) { return a > b ? a : b; }
static int mini(const int a, const int b) { return a < b ? a : b; }

static double param_num(const cJSON *params, const MsgNodeDef *d, const char *name) {
    const cJSON *v = is_obj(params) ? cJSON_GetObjectItemCaseSensitive(params, name) : NULL;
    if (cJSON_IsNumber(v)) return v->valuedouble;
    for (int i = 0; i < d->nparams; i++)
        if (!strcmp(d->params[i].name, name) && d->params[i].ndef) return d->params[i].def[0];
    return 0.0;
}

static const char *param_str(const cJSON *params, const MsgNodeDef *d, const char *name) {
    const char *v = is_obj(params) ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, name)) : NULL;
    if (v) return v;
    for (int i = 0; i < d->nparams; i++)
        if (!strcmp(d->params[i].name, name)) return d->params[i].sdef;
    return NULL;
}

// the post pass's mesh / screen inputs (iÍïïîïi, deob:94840-94976, with the post ctx of deob:205886-205898)
static const char *input_node_expr(const char *type, const cJSON *params) {
    static const struct { const char *type, *expr; } T[] = {
        {"time", "kn_time"}, {"seed", "0.0"}, {"deltaTime", "kn_delta"},
        {"position", "vec3(kn_ndc, 0.0)"}, {"worldPosition", "vec3(kn_ndc, 0.0)"}, {"positionView", "vec3(kn_ndc, 0.0)"},
        {"positionWorldDirection", "vec3(0.0, 0.0, -1.0)"},
        {"normal", "vec3(0.0, 0.0, 1.0)"}, {"normalLocal", "vec3(0.0, 0.0, 1.0)"}, {"normalView", "vec3(0.0, 0.0, 1.0)"},
        {"normalGeometry", "vec3(0.0, 0.0, 1.0)"}, {"tangentLocal", "vec3(1.0, 0.0, 0.0)"}, {"tangentView", "vec3(1.0, 0.0, 0.0)"},
        {"tangentWorld", "vec3(1.0, 0.0, 0.0)"}, {"bitangentLocal", "vec3(0.0, 1.0, 0.0)"}, {"bitangentView", "vec3(0.0, 1.0, 0.0)"},
        {"bitangentWorld", "vec3(0.0, 1.0, 0.0)"}, {"viewDir", "vec3(0.0, 0.0, 1.0)"}, {"reflectView", "vec3(0.0, 0.0, 1.0)"},
        {"reflectVector", "vec3(0.0, 0.0, 1.0)"}, {"matcapUV", "vec2(0.5)"}, {"cameraPosition", "vec3(0.0)"},
        {"cameraNear", "0.0"}, {"cameraFar", "1.0"}, {"modelPosition", "vec3(0.0)"}, {"modelScale", "vec3(1.0)"},
        {"modelDirection", "vec3(0.0, 0.0, 1.0)"}, {"screenUV", "kn_suv"}, {"screenCoordinate", "kn_scoord"},
        {"screenSize", "kn_resolution"}, {"screenDPR", "kn_dpr"}, {"resolution", "kn_resolution"}, {"viewportUV", "kn_suv"},
        {"viewportSize", "kn_resolution"}, {"frontFacing", "1.0"}, {"vertexIndex", "0.0"}, {"instanceIndex", "0.0"},
        {"baseColor", "vec3(1.0)"}, {"vertexColor", "vec3(1.0)"}, {"viewportLinearDepth", "1.0"},
    };
    if (!strcmp(type, "uv")) { // ctx.uv is screenUV in a post pass; uv(index > 0) is the quad's own uv
        const cJSON *ix = is_obj(params) ? cJSON_GetObjectItemCaseSensitive(params, "index") : NULL;
        return cJSON_IsNumber(ix) && ix->valuedouble > 0 ? "kn_quad_uv" : "kn_suv";
    }
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++)
        if (!strcmp(T[i].type, type)) return T[i].expr;
    return NULL;
}

// a param node's uniform (deob:95037-95105): its default (a colour, a clamped float, an array), else the instance
// value shaderArgs.params[name] when it fits the type
static int param_uniform(MsgCtx *c, const cJSON *params, MsgVal *out) {
    if (c->prog->uniform_count >= MOD_SHADER_MAX_UNIFORMS) return 0;
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "name"));
    const char *ptype = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "type"));
    if (!ptype) ptype = "float";
    const int color = !strcmp(ptype, "color");
    const int w = color ? 3 : !strcmp(ptype, "vec2") ? 2 : !strcmp(ptype, "vec3") ? 3 : !strcmp(ptype, "vec4") ? 4 : 1;
    ModShaderUniform *u = &c->prog->uniforms[c->prog->uniform_count];
    memset(u, 0, sizeof(*u));
    snprintf(u->name, sizeof(u->name), "%s", name ? name : "param");
    u->width = w;
    const cJSON *lo = cJSON_GetObjectItemCaseSensitive(params, "min"), *hi = cJSON_GetObjectItemCaseSensitive(params, "max");
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(params, "default");
    if (color) {
        if (cJSON_IsString(d)) color_rgb(d->valuestring, u->value);
        else u->value[0] = u->value[1] = u->value[2] = 1.0f;
    } else if (w == 1) {
        u->value[0] = cJSON_IsNumber(d) ? (float) d->valuedouble : 0.0f;
    } else if (num_array(d, w)) {
        int i = 0;
        for (const cJSON *e = d->child; e; e = e->next) u->value[i++] = (float) e->valuedouble;
    } else if (w == 4) u->value[3] = 1.0f;
    const cJSON *inst = is_obj(c->instance) && name ? cJSON_GetObjectItemCaseSensitive(c->instance, name) : NULL;
    if (inst) {
        if (color && match_color(inst)) color_rgb(inst->valuestring, u->value);
        else if (color && num_array(inst, 3)) {
            int i = 0;
            for (const cJSON *e = inst->child; e; e = e->next) u->value[i++] = (float) e->valuedouble;
        } else if (!color && w == 1 && is_finite_num(inst)) u->value[0] = (float) inst->valuedouble;
        else if (!color && w > 1 && num_array(inst, w)) {
            int i = 0;
            for (const cJSON *e = inst->child; e; e = e->next) u->value[i++] = (float) e->valuedouble;
        }
    }
    if (w == 1 && !color) { // iìíiííi: float values clamp to min / max
        if (cJSON_IsNumber(lo) && u->value[0] < lo->valuedouble) u->value[0] = (float) lo->valuedouble;
        if (cJSON_IsNumber(hi) && u->value[0] > hi->valuedouble) u->value[0] = (float) hi->valuedouble;
    }
    snprintf(out->expr, sizeof(out->expr), "u_p%d", c->prog->uniform_count);
    out->width = w;
    c->prog->uniform_count++;
    return 1;
}

static void set_outs(MsgNodeState *s, const int n, const char *const *names, const MsgVal *vals) {
    s->nout = n;
    for (int i = 0; i < n; i++) {
        s->names[i] = names[i];
        s->out[i] = vals[i];
    }
}

static int compile_node(MsgCtx *c, const int ni) {
    MsgNodeState *s = &c->st[ni];
    if (s->state == 2) return 1;
    if (s->state == 1 || c->failed) return 0; // (the validator refuses cycles)
    s->state = 1;
    const cJSON *node = c->node[ni];
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(node, "type"));
    const MsgNodeDef *d = c->def[ni];
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(node, "params");
    const char *ie = input_node_expr(t, params);
    char a[256], b[256];

    if (ie) {
        OUT1(d->out[0].type ? d->out[0].type : 1, "%s", ie);
    } else if (!strcmp(t, "float")) {
        double v = param_num(params, d, "value");
        literal(c, &v, 1, &s->out[0]);
        s->names[0] = "out";
        s->nout = 1;
    } else if (!strcmp(t, "vec2") || !strcmp(t, "vec3") || !strcmp(t, "vec4")) {
        const cJSON *v = is_obj(params) ? cJSON_GetObjectItemCaseSensitive(params, "value") : NULL;
        double vals[4];
        int w = v ? json_values(v, vals) : 0;
        if (!w) {
            w = d->params[0].ndef;
            memcpy(vals, d->params[0].def, sizeof(vals));
        }
        literal(c, vals, w, &s->out[0]);
        s->names[0] = "out";
        s->nout = 1;
    } else if (!strcmp(t, "color")) {
        const char *hex = param_str(params, d, "value");
        float rgb[3] = {1, 1, 1};
        if (hex && strlen(hex) == 7) color_rgb(hex, rgb);
        const double v[3] = {rgb[0], rgb[1], rgb[2]};
        literal(c, v, 3, &s->out[0]);
        s->names[0] = "out";
        s->nout = 1;
    } else if (!strcmp(t, "constant")) {
        const char *k = param_str(params, d, "value");
        const char *v = !k || !strcmp(k, "PI") ? "3.141592653589793" : !strcmp(k, "TWO_PI") ? "6.283185307179586"
                      : !strcmp(k, "HALF_PI") ? "1.5707963267948966" : "1e-6";
        OUT1(1, "%s", v);
    } else if (!strcmp(t, "param")) {
        if (!param_uniform(c, params, &s->out[0])) return 0;
        s->names[0] = "out";
        s->nout = 1;
    } else if (!strcmp(t, "texture") || !strcmp(t, "textureBicubic") || !strcmp(t, "triplanarTexture")) {
        // no texture slots in a post pass: white (iîíííîî null -> vec4(1, 1, 1, 1))
        MsgVal v;
        emit_var(c, &v, 4, "vec4(1.0)");
        const char *names[3] = {"rgb", "a", "rgba"};
        MsgVal vals[3];
        snprintf(vals[0].expr, sizeof(vals[0].expr), "%.32s.xyz", v.expr);
        vals[0].width = 3;
        snprintf(vals[1].expr, sizeof(vals[1].expr), "%.32s.w", v.expr);
        vals[1].width = 1;
        vals[2] = v;
        set_outs(s, 3, names, vals);
    } else if (!strcmp(t, "textureSize")) {
        OUT1(2, "vec2(1.0)");
    } else if (!strcmp(t, "sceneColor") || !strcmp(t, "viewportSharedTexture")) {
        if (!input(c, ni, "uv", 2, a, sizeof(a))) snprintf(a, sizeof(a), "kn_suv");
        OUT1(4, "kn_scene(%s)", a);
    } else if (!strcmp(t, "viewportDepthTexture")) {
        OUT1(1, "1.0");
    } else if (!strcmp(t, "abs") || !strcmp(t, "floor") || !strcmp(t, "ceil") || !strcmp(t, "fract") || !strcmp(t, "sqrt") ||
               !strcmp(t, "sin") || !strcmp(t, "cos") || !strcmp(t, "tan") || !strcmp(t, "asin") || !strcmp(t, "acos") ||
               !strcmp(t, "atan") || !strcmp(t, "exp") || !strcmp(t, "log") || !strcmp(t, "exp2") || !strcmp(t, "log2") ||
               !strcmp(t, "sign") || !strcmp(t, "round") || !strcmp(t, "trunc") || !strcmp(t, "negate") || !strcmp(t, "oneMinus") ||
               !strcmp(t, "saturate") || !strcmp(t, "normalize") || !strcmp(t, "cbrt") || !strcmp(t, "degrees") ||
               !strcmp(t, "radians") || !strcmp(t, "inverseSqrt") || !strcmp(t, "pow2") || !strcmp(t, "pow3") || !strcmp(t, "pow4") ||
               !strcmp(t, "reciprocal") || !strcmp(t, "dFdx") || !strcmp(t, "dFdy") || !strcmp(t, "fwidth")) {
        const int w = !strcmp(t, "normalize") ? maxi(2, in_width(c, ni, "x")) : in_width(c, ni, "x");
        if (!input(c, ni, "x", w, a, sizeof(a))) return 0;
        const char *tn = TN[w];
        if (!strcmp(t, "negate")) OUT1(w, "-(%s)", a);
        else if (!strcmp(t, "oneMinus")) OUT1(w, "1.0 - (%s)", a);
        else if (!strcmp(t, "saturate")) OUT1(w, "clamp(%s, 0.0, 1.0)", a);
        else if (!strcmp(t, "cbrt")) OUT1(w, "sign(%s) * pow(abs(%s), %s(1.0 / 3.0))", a, a, tn);
        else if (!strcmp(t, "inverseSqrt")) OUT1(w, "inversesqrt(%s)", a);
        else if (!strcmp(t, "pow2")) OUT1(w, "(%s) * (%s)", a, a);
        else if (!strcmp(t, "pow3")) OUT1(w, "(%s) * (%s) * (%s)", a, a, a);
        else if (!strcmp(t, "pow4")) OUT1(w, "(%s) * (%s) * (%s) * (%s)", a, a, a, a);
        else if (!strcmp(t, "reciprocal")) OUT1(w, "1.0 / (%s)", a);
        else OUT1(w, "%s(%s)", t, a);
    } else if (!strcmp(t, "length") || !strcmp(t, "lengthSq")) {
        const int w = maxi(2, in_width(c, ni, "v"));
        if (!input(c, ni, "v", w, a, sizeof(a))) return 0;
        if (t[6]) OUT1(1, "dot(%s, %s)", a, a);
        else OUT1(1, "length(%s)", a);
    } else if (!strcmp(t, "luminance")) {
        if (!input(c, ni, "c", 3, a, sizeof(a))) return 0;
        OUT1(1, "kn_lum(%s)", a);
    } else if (!strcmp(t, "add") || !strcmp(t, "sub") || !strcmp(t, "mul") || !strcmp(t, "div") || !strcmp(t, "mod") ||
               !strcmp(t, "pow") || !strcmp(t, "min") || !strcmp(t, "max") || !strcmp(t, "step") || !strcmp(t, "difference") ||
               !strcmp(t, "atan2") || !strcmp(t, "safePower")) {
        const int w = maxi(in_width(c, ni, "a"), in_width(c, ni, "b"));
        if (!input(c, ni, "a", w, a, sizeof(a)) || !input(c, ni, "b", w, b, sizeof(b))) return 0;
        const char op = !strcmp(t, "add") ? '+' : !strcmp(t, "sub") ? '-' : !strcmp(t, "mul") ? '*' : !strcmp(t, "div") ? '/' : 0;
        if (op) OUT1(w, "(%s) %c (%s)", a, op, b);
        else if (!strcmp(t, "difference")) OUT1(w, "abs((%s) - (%s))", a, b);
        else if (!strcmp(t, "atan2")) OUT1(w, "atan(%s, %s)", a, b);
        else if (!strcmp(t, "safePower")) OUT1(w, "pow(abs(%s), %s) * sign(%s)", a, b, a);
        else OUT1(w, "%s(%s, %s)", t, a, b);
    } else if (!strcmp(t, "dot") || !strcmp(t, "distance")) {
        const int w = maxi(2, maxi(in_width(c, ni, "a"), in_width(c, ni, "b")));
        if (!input(c, ni, "a", w, a, sizeof(a)) || !input(c, ni, "b", w, b, sizeof(b))) return 0;
        OUT1(1, "%s(%s, %s)", t, a, b);
    } else if (!strcmp(t, "cross")) {
        if (!input(c, ni, "a", 3, a, sizeof(a)) || !input(c, ni, "b", 3, b, sizeof(b))) return 0;
        OUT1(3, "cross(%s, %s)", a, b);
    } else if (!strcmp(t, "reflect") || !strcmp(t, "refract")) {
        const int w = maxi(2, maxi(in_width(c, ni, "i"), in_width(c, ni, "n")));
        if (!input(c, ni, "i", w, a, sizeof(a)) || !input(c, ni, "n", w, b, sizeof(b))) return 0;
        if (t[2] == 'f' && t[3] == 'l') OUT1(w, "reflect(%s, %s)", a, b);
        else {
            IN("eta", 1, eta);
            OUT1(w, "refract(%s, %s, %s)", a, b, eta);
        }
    } else if (!strcmp(t, "faceforward")) {
        const int w = maxi(2, maxi(in_width(c, ni, "n"), maxi(in_width(c, ni, "i"), in_width(c, ni, "nref"))));
        IN("nref", w, nref);
        if (!input(c, ni, "n", w, a, sizeof(a)) || !input(c, ni, "i", w, b, sizeof(b))) return 0;
        OUT1(w, "faceforward(%s, %s, %s)", a, b, nref);
    } else if (!strcmp(t, "mix")) {
        const int w = maxi(in_width(c, ni, "a"), in_width(c, ni, "b"));
        const int tw = in_width(c, ni, "t") == 1 ? 1 : w;
        IN("t", tw, tt);
        if (!input(c, ni, "a", w, a, sizeof(a)) || !input(c, ni, "b", w, b, sizeof(b))) return 0;
        OUT1(w, "mix(%s, %s, %s)", a, b, tt);
    } else if (!strcmp(t, "clamp") || !strcmp(t, "smoothstep")) {
        const int w = in_width(c, ni, "x");
        const int bw = maxi(in_width(c, ni, "lo"), in_width(c, ni, "hi")) == 1 ? 1 : w;
        IN("x", w, x);
        IN("lo", bw, lo);
        IN("hi", bw, hi);
        if (t[0] == 'c') OUT1(w, "clamp(%s, %s, %s)", x, lo, hi);
        else OUT1(w, "smoothstep(%s, %s, %s)", lo, hi, x);
    } else if (!strcmp(t, "remap") || !strcmp(t, "remapClamp")) {
        const int w = in_width(c, ni, "x");
        const int bw = maxi(maxi(in_width(c, ni, "inLo"), in_width(c, ni, "inHi")), maxi(in_width(c, ni, "outLo"), in_width(c, ni, "outHi"))) == 1 ? 1 : w;
        IN("x", w, x);
        IN("inLo", bw, il);
        IN("inHi", bw, ih);
        IN("outLo", bw, ol);
        IN("outHi", bw, oh);
        if (t[5]) OUT1(w, "clamp(((%s) - (%s)) / ((%s) - (%s)), 0.0, 1.0) * ((%s) - (%s)) + (%s)", x, il, ih, il, oh, ol, ol);
        else OUT1(w, "((%s) - (%s)) / ((%s) - (%s)) * ((%s) - (%s)) + (%s)", x, il, ih, il, oh, ol, ol);
    } else if (!strcmp(t, "contrast")) {
        const int w = in_width(c, ni, "x");
        IN("x", w, x);
        IN("pivot", 1, pv);
        IN("amount", 1, am);
        OUT1(w, "((%s) - (%s)) * (%s) + (%s)", x, pv, am, pv);
    } else if (!strcmp(t, "select")) {
        const int w = maxi(in_width(c, ni, "a"), in_width(c, ni, "b"));
        IN("cond", 1, cond);
        if (!input(c, ni, "a", w, a, sizeof(a)) || !input(c, ni, "b", w, b, sizeof(b))) return 0;
        OUT1(w, "(%s) > 0.5 ? (%s) : (%s)", cond, a, b);
    } else if (!strcmp(t, "compare")) {
        const char *op = param_str(params, d, "op");
        const char *g = !op || !strcmp(op, "lt") ? "<" : !strcmp(op, "le") ? "<=" : !strcmp(op, "gt") ? ">" : !strcmp(op, "ge") ? ">=" : !strcmp(op, "eq") ? "==" : "!=";
        IN("a", 1, x);
        IN("b", 1, y);
        OUT1(1, "(%s) %s (%s) ? 1.0 : 0.0", x, g, y);
    } else if (!strcmp(t, "and") || !strcmp(t, "or") || !strcmp(t, "xor")) {
        IN("a", 1, x);
        IN("b", 1, y);
        OUT1(1, "((%s) > 0.5) %s ((%s) > 0.5) ? 1.0 : 0.0", x, t[0] == 'a' ? "&&" : t[0] == 'o' ? "||" : "^^", y);
    } else if (!strcmp(t, "not")) {
        IN("x", 1, x);
        OUT1(1, "(%s) > 0.5 ? 0.0 : 1.0", x);
    } else if (!strcmp(t, "all") || !strcmp(t, "any")) {
        const int w = in_width(c, ni, "v");
        IN("v", w, v);
        if (w == 1) OUT1(1, "(%s) > 0.5 ? 1.0 : 0.0", v);
        else OUT1(1, "%s(greaterThan(%s, %s(0.5))) ? 1.0 : 0.0", t, v, TN[w]);
    } else if (!strcmp(t, "split")) {
        IN("v", 4, v);
        MsgVal base;
        emit_var(c, &base, 4, "%s", v);
        const char *names[4] = {"x", "y", "z", "w"};
        MsgVal vals[4];
        for (int i = 0; i < 4; i++) {
            snprintf(vals[i].expr, sizeof(vals[i].expr), "%.32s.%s", base.expr, names[i]);
            vals[i].width = 1;
        }
        set_outs(s, 4, names, vals);
    } else if (!strcmp(t, "combine")) {
        IN("x", 1, x);
        IN("y", 1, y);
        IN("z", 1, z);
        IN("w", 1, w);
        OUT1(4, "vec4(%s, %s, %s, %s)", x, y, z, w);
    } else if (!strcmp(t, "swizzle")) {
        const char *mask = param_str(params, d, "mask");
        if (!mask || !match_swizzle(mask)) mask = "xyz";
        IN("v", 4, v);
        OUT1((int) strlen(mask), "(%s).%s", v, mask);
    } else if (!strcmp(t, "convert")) {
        const char *ty = param_str(params, d, "type");
        const int w = ty && !strcmp(ty, "float") ? 1 : ty && !strcmp(ty, "vec2") ? 2 : ty && !strcmp(ty, "vec4") ? 4 : 3;
        IN("v", w, v);
        OUT1(w, "%s", v);
    } else if (!strcmp(t, "rotate3d")) {
        IN("p", 3, p);
        IN("euler", 3, e);
        OUT1(3, "kn_rotate3(%s, %s)", p, e);
    } else if (!strcmp(t, "packNormalToRGB")) {
        IN("n", 3, n);
        OUT1(3, "(%s) * 0.5 + 0.5", n);
    } else if (!strcmp(t, "unpackRGBToNormal")) {
        IN("c", 3, cc);
        OUT1(3, "(%s) * 2.0 - 1.0", cc);
    } else if (!strcmp(t, "hash")) {
        IN("p", 2, p);
        OUT1(1, "kn_hash21(%s)", p);
    } else if (!strcmp(t, "noise") || !strcmp(t, "fbm") || !strcmp(t, "noise3") || !strcmp(t, "cellNoise") || !strcmp(t, "worley")) {
        // iIìíiìì: p at 2 or 3 components times scale
        const int w = mini(3, maxi(2, in_width(c, ni, "p")));
        IN("p", w, p);
        IN("scale", 1, sc);
        MsgVal pp;
        emit_var(c, &pp, w, "(%s) * (%s)", p, sc);
        c->noise = 1;
        if (!strcmp(t, "noise")) OUT1(1, "kn_perlin(%s)", pp.expr);
        else if (!strcmp(t, "noise3")) OUT1(3, "kn_perlin3(%s)", pp.expr);
        else if (!strcmp(t, "cellNoise")) OUT1(1, "kn_cell(%s)", pp.expr);
        else if (!strcmp(t, "fbm")) { // the compiler's own fbm (deob:95210-95222)
            const int oct = (int) param_num(params, d, "octaves");
            const double gain = param_num(params, d, "gain"), lac = param_num(params, d, "lacunarity");
            double amp = 1.0, freq = 1.0, total = 0.0;
            MsgVal sum;
            emit_var(c, &sum, 1, "0.0");
            for (int i = 0; i < oct; i++) {
                char fa[32], ff[32];
                fmt_num(fa, sizeof(fa), amp);
                fmt_num(ff, sizeof(ff), freq);
                buf_printf(&c->body, "  %s += kn_perlin(%s * %s) * %s;\n", sum.expr, pp.expr, ff, fa);
                total += amp;
                amp *= gain;
                freq *= lac;
            }
            char ft[32];
            fmt_num(ft, sizeof(ft), total);
            if (total > 0.0) OUT1(1, "%s / %s", sum.expr, ft);
            else OUT1(1, "%s", sum.expr);
        } else { // worley: F1 with the style's metric, F1 F2 and F1 F2 F3 with the default (deob:95237-95250)
            IN("jitter", 1, jit);
            const char *style = param_str(params, d, "style");
            const int metric = style && !strcmp(style, "cell") ? 1 : 0;
            MsgVal f1, f0;
            emit_var(c, &f1, 3, "kn_worley_n(%s, %s, %d)", pp.expr, jit, metric);
            emit_var(c, &f0, 3, "kn_worley_n(%s, %s, 0)", pp.expr, jit);
            const char *names[3] = {"out", "f2", "f3"};
            MsgVal vals[3];
            snprintf(vals[0].expr, sizeof(vals[0].expr), "%.32s.x", f1.expr);
            vals[0].width = 1;
            snprintf(vals[1].expr, sizeof(vals[1].expr), "%.32s.xy", f0.expr);
            vals[1].width = 2;
            vals[2] = f0;
            set_outs(s, 3, names, vals);
        }
    } else if (!strcmp(t, "fractalNoise")) {
        const int pw = mini(3, maxi(2, in_width(c, ni, "p")));
        IN("p", pw, p);
        IN("scale", 1, sc);
        MsgVal src, pv;
        char conv[300];
        emit_var(c, &src, pw, "(%s) * (%s)", p, sc);
        convert(conv, sizeof(conv), &src, 3); // mx_fractal_noise takes a vec3 position
        emit_var(c, &pv, 3, "%s", conv);
        const int oct = (int) param_num(params, d, "octaves");
        char lac[32], dim[32];
        fmt_num(lac, sizeof(lac), param_num(params, d, "lacunarity"));
        fmt_num(dim, sizeof(dim), param_num(params, d, "diminish"));
        c->noise = 1;
        const char *names[4] = {"out", "v2", "v3", "v4"};
        MsgVal vals[4];
        emit_var(c, &vals[0], 1, "kn_fractal(%s, %d, %s, %s)", pv.expr, oct, lac, dim);
        emit_var(c, &vals[1], 2, "vec2(%s, kn_fractal(%s + vec3(19.0, 193.0, 17.0), %d, %s, %s))", vals[0].expr, pv.expr, oct, lac, dim);
        emit_var(c, &vals[2], 3, "kn_fractal3(%s, %d, %s, %s)", pv.expr, oct, lac, dim);
        emit_var(c, &vals[3], 4, "vec4(%s, kn_fractal(%s + vec3(19.0, 193.0, 17.0), %d, %s, %s))", vals[2].expr, pv.expr, oct, lac, dim);
        set_outs(s, 4, names, vals);
    } else if (!strcmp(t, "voronoi")) {
        IN("p", 2, p);
        IN("scale", 1, sc);
        MsgVal r;
        emit_var(c, &r, 2, "kn_voronoi((%s) * (%s))", p, sc);
        const char *names[2] = {"dist", "id"};
        MsgVal vals[2];
        snprintf(vals[0].expr, sizeof(vals[0].expr), "%.32s.x", r.expr);
        snprintf(vals[1].expr, sizeof(vals[1].expr), "%.32s.y", r.expr);
        vals[0].width = vals[1].width = 1;
        set_outs(s, 2, names, vals);
    } else if (!strcmp(t, "checker")) {
        IN("uv", 2, uv);
        IN("scale", 1, sc);
        MsgVal f;
        emit_var(c, &f, 2, "floor((%s) * (%s))", uv, sc);
        OUT1(1, "mod(%s.x + %s.y, 2.0)", f.expr, f.expr);
    } else if (!strcmp(t, "rampLR") || !strcmp(t, "rampTB") || !strcmp(t, "splitLR") || !strcmp(t, "splitTB")) {
        const int w = maxi(in_width(c, ni, "a"), in_width(c, ni, "b"));
        char uv[256];
        if (!input(c, ni, "uv", 2, uv, sizeof(uv))) snprintf(uv, sizeof(uv), "kn_suv"); // iÌíîîíí: ctx.uv()
        if (!input(c, ni, "a", w, a, sizeof(a)) || !input(c, ni, "b", w, b, sizeof(b))) return 0;
        const char axis = t[strlen(t) - 2] == 'L' ? 'x' : 'y';
        if (t[0] == 'r') OUT1(w, "mix(%s, %s, clamp((%s).%c, 0.0, 1.0))", a, b, uv, axis);
        else {
            IN("center", 1, ce);
            OUT1(w, "mix(%s, %s, kn_aastep(%s, (%s).%c))", a, b, ce, uv, axis);
        }
    } else if (!strcmp(t, "aastep")) {
        IN("threshold", 1, th);
        IN("x", 1, x);
        OUT1(1, "kn_aastep(%s, %s)", th, x);
    } else if (!strcmp(t, "hsv2rgb") || !strcmp(t, "rgb2hsv") || !strcmp(t, "grayscale") || !strcmp(t, "srgbToLinear") ||
               !strcmp(t, "linearToSrgb")) {
        IN("c", 3, cc);
        const char *fn = !strcmp(t, "hsv2rgb") ? "kn_hsv2rgb" : !strcmp(t, "rgb2hsv") ? "kn_rgb2hsv" : !strcmp(t, "srgbToLinear") ? "kn_srgb_to_linear"
                       : !strcmp(t, "linearToSrgb") ? "kn_linear_to_srgb" : NULL;
        if (fn) OUT1(3, "%s(%s)", fn, cc);
        else OUT1(3, "vec3(kn_lum(%s))", cc);
    } else if (!strcmp(t, "blendBurn") || !strcmp(t, "blendDodge") || !strcmp(t, "blendOverlay") || !strcmp(t, "blendScreen")) {
        IN("base", 3, ba);
        IN("blend", 3, bl);
        OUT1(3, "kn_%s(%s, %s)", !strcmp(t, "blendBurn") ? "burn" : !strcmp(t, "blendDodge") ? "dodge" : !strcmp(t, "blendOverlay") ? "overlay" : "screen", ba, bl);
    } else if (!strcmp(t, "blendColor")) {
        IN("base", 4, ba);
        IN("blend", 4, bl);
        OUT1(4, "kn_blend_color(%s, %s)", ba, bl);
    } else if (!strcmp(t, "saturation") || !strcmp(t, "vibrance")) {
        IN("c", 3, cc);
        IN("amount", 1, am);
        OUT1(3, "kn_%s(%s, %s)", t, cc, am);
    } else if (!strcmp(t, "hue")) {
        IN("c", 3, cc);
        IN("angle", 1, an);
        OUT1(3, "kn_hue(%s, %s)", cc, an);
    } else if (!strcmp(t, "posterize")) {
        const int w = in_width(c, ni, "x");
        IN("x", w, x);
        IN("steps", 1, st);
        OUT1(w, "floor((%s) * (%s)) / (%s)", x, st, st);
    } else if (!strcmp(t, "rotate2d")) {
        IN("uv", 2, uv);
        IN("angle", 1, an);
        IN("center", 2, ce);
        OUT1(2, "kn_rotate2((%s) - (%s), %s) + (%s)", uv, ce, an, ce);
    } else if (!strcmp(t, "spherizeUV")) {
        char uv[256];
        if (!input(c, ni, "uv", 2, uv, sizeof(uv))) snprintf(uv, sizeof(uv), "kn_suv");
        IN("strength", 1, sg);
        IN("center", 2, ce);
        OUT1(2, "kn_spherize(%s, %s, %s)", uv, sg, ce);
    } else if (!strcmp(t, "spritesheetUV")) {
        char uv[256];
        if (!input(c, ni, "uv", 2, uv, sizeof(uv))) snprintf(uv, sizeof(uv), "kn_suv");
        IN("count", 2, ct);
        IN("frame", 1, fr);
        OUT1(2, "kn_spritesheet(%s, %s, %s)", ct, uv, fr);
    } else if (!strcmp(t, "equirectUV")) {
        char dir[256];
        if (!input(c, ni, "dir", 3, dir, sizeof(dir))) snprintf(dir, sizeof(dir), "vec3(0.0, 0.0, -1.0)"); // positionWorldDirection
        OUT1(2, "kn_equirect_uv(%s)", dir);
    } else if (!strcmp(t, "equirectDirection")) {
        char uv[256];
        if (!input(c, ni, "uv", 2, uv, sizeof(uv))) snprintf(uv, sizeof(uv), "kn_suv");
        OUT1(3, "kn_equirect_dir(%s)", uv);
    } else if (!strcmp(t, "polar")) {
        IN("uv", 2, uv);
        IN("center", 2, ce);
        MsgVal dd;
        emit_var(c, &dd, 2, "(%s) - (%s)", uv, ce);
        const char *names[2] = {"r", "theta"};
        MsgVal vals[2];
        emit_var(c, &vals[0], 1, "length(%s)", dd.expr);
        emit_var(c, &vals[1], 1, "atan(%s.y, %s.x)", dd.expr, dd.expr);
        set_outs(s, 2, names, vals);
    } else if (!strcmp(t, "tile")) {
        IN("uv", 2, uv);
        IN("count", 2, ct);
        MsgVal sc;
        emit_var(c, &sc, 2, "(%s) * (%s)", uv, ct);
        const char *names[2] = {"uv", "cell"};
        MsgVal vals[2];
        emit_var(c, &vals[0], 2, "fract(%s)", sc.expr);
        emit_var(c, &vals[1], 2, "floor(%s)", sc.expr);
        set_outs(s, 2, names, vals);
    } else if (!strcmp(t, "oscSine") || !strcmp(t, "oscSquare") || !strcmp(t, "oscTriangle") || !strcmp(t, "oscSawtooth")) {
        char tt[256];
        if (!input(c, ni, "t", 1, tt, sizeof(tt))) snprintf(tt, sizeof(tt), "kn_time");
        OUT1(1, "kn_osc_%s(%s)", !strcmp(t, "oscSine") ? "sine" : !strcmp(t, "oscSquare") ? "square" : !strcmp(t, "oscTriangle") ? "triangle" : "sawtooth", tt);
    } else if (!strcmp(t, "palette")) {
        IN("t", 1, tt);
        IN("a", 3, pa);
        IN("b", 3, pb);
        IN("c", 3, pc);
        IN("d", 3, pd);
        OUT1(3, "(%s) + (%s) * cos(((%s) * (%s) + (%s)) * 6.28318530718)", pa, pb, pc, tt, pd);
    } else if (!strcmp(t, "gradient")) { // evenly spaced stops over saturate(t) (deob:95436-95450)
        float stops[4][3];
        int n = 0;
        static const char *const keys[4] = {"c0", "c1", "c2", "c3"};
        for (int i = 0; i < 4; i++) {
            const char *hex = param_str(params, d, keys[i]);
            if (!hex) break;
            color_rgb(hex, stops[n++]);
        }
        IN("t", 1, tt);
        MsgVal tv, col;
        emit_var(c, &tv, 1, "clamp(%s, 0.0, 1.0)", tt);
        emit_var(c, &col, 3, "vec3(%.9g, %.9g, %.9g)", stops[0][0], stops[0][1], stops[0][2]);
        for (int i = 0; i + 1 < n; i++)
            buf_printf(&c->body, "  %s = mix(%s, vec3(%.9g, %.9g, %.9g), clamp(%s * %d.0 - %d.0, 0.0, 1.0));\n", col.expr, col.expr, stops[i + 1][0],
                       stops[i + 1][1], stops[i + 1][2], tv.expr, n - 1, i);
        s->out[0] = col;
        s->names[0] = "out";
        s->nout = 1;
    } else if (!strcmp(t, "output")) {
        s->nout = 0;
    } else {
        set_err(c->err, c->errn, NULL, "unhandled node type %s", t ? t : "?");
        c->failed = 1;
        return 0;
    }
    s->state = 2;
    return !c->body.oom;
}

int mod_shader_graph_compile(const cJSON *graph, const cJSON *params, ModShaderProgram *out, char *err, const size_t err_size) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!mod_shader_graph_validate(graph, "post", err, err_size)) return 0;
    const cJSON *nodes = cJSON_GetObjectItemCaseSensitive(graph, "nodes");
    MsgCtx c = {0};
    c.nodes = nodes;
    c.count = cJSON_GetArraySize(nodes);
    c.node = calloc((size_t) c.count, sizeof(*c.node));
    c.def = calloc((size_t) c.count, sizeof(*c.def));
    c.st = calloc((size_t) c.count, sizeof(*c.st));
    c.instance = params;
    c.prog = out;
    c.err = err;
    c.errn = err_size;
    if (!c.node || !c.def || !c.st) {
        free(c.node);
        free(c.def);
        free(c.st);
        set_err(err, err_size, NULL, "out of memory");
        return 0;
    }
    int i = 0;
    for (const cJSON *n = nodes->child; n; n = n->next, i++) {
        c.node[i] = n;
        c.def[i] = node_def(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(n, "type")));
    }
    // the graph's param list (iìiîííî): the Settings rows
    for (int k = 0; k < c.count && out->param_count < MOD_SHADER_MAX_UNIFORMS; k++) {
        if (strcmp(c.def[k]->type, "param")) continue;
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(c.node[k], "params");
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, "name"));
        if (!name) continue;
        ModShaderParam *mp = &out->params[out->param_count++];
        snprintf(mp->name, sizeof(mp->name), "%s", name);
        const char *ty = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, "type"));
        snprintf(mp->type, sizeof(mp->type), "%s", ty ? ty : "float");
        const cJSON *dv = cJSON_GetObjectItemCaseSensitive(p, "default"), *lo = cJSON_GetObjectItemCaseSensitive(p, "min"),
                    *hi = cJSON_GetObjectItemCaseSensitive(p, "max");
        mp->has_default = cJSON_IsNumber(dv) || cJSON_IsBool(dv);
        mp->def = cJSON_IsNumber(dv) ? dv->valuedouble : cJSON_IsTrue(dv) ? 1.0 : 0.0;
        mp->has_min = cJSON_IsNumber(lo);
        mp->min = mp->has_min ? lo->valuedouble : 0.0;
        mp->has_max = cJSON_IsNumber(hi);
        mp->max = mp->has_max ? hi->valuedouble : 0.0;
    }
    const char *out_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(graph, "output"));
    const int oi = node_index(&c, out_id);
    char color[256] = "vec4(0.0, 0.0, 0.0, 1.0)"; // (deob:95527-95531) no colour input: black
    int ok = oi >= 0 && compile_node(&c, oi);
    if (ok) {
        MsgVal v;
        if (connected(&c, oi, "color", &v)) convert(color, sizeof(color), &v, 4);
    }
    ok = ok && !c.failed && !c.body.oom;
    if (ok) {
        Buf src = {0};
        buf_puts(&src, "precision highp float;\nprecision highp int;\n");
        buf_puts(&src, "uniform sampler2D kn_input;\nuniform vec2 kn_resolution;\nuniform float kn_time;\nuniform float kn_delta;\nuniform float kn_dpr;\n");
        for (int u = 0; u < out->uniform_count; u++) buf_printf(&src, "uniform %s u_p%d;\n", TN[out->uniforms[u].width], u);
        buf_puts(&src, "out vec4 kn_frag;\n");
        // sceneColor(uv): the previous pass at a screen UV (top-left origin, as three's screenUV)
        buf_puts(&src, "vec4 kn_scene(vec2 uv) { return texture(kn_input, vec2(uv.x, 1.0 - uv.y)); }\n");
        buf_puts(&src, k_lib_common);
        if (c.noise) buf_puts(&src, k_lib_noise);
        buf_puts(&src, "void main() {\n"
                       "  vec2 kn_scoord = vec2(gl_FragCoord.x, kn_resolution.y - gl_FragCoord.y);\n"
                       "  vec2 kn_suv = kn_scoord / kn_resolution;\n"
                       "  vec2 kn_quad_uv = vec2(kn_suv.x, 1.0 - kn_suv.y);\n"
                       "  vec2 kn_ndc = vec2(kn_suv.x * 2.0 - 1.0, 1.0 - kn_suv.y * 2.0);\n");
        if (c.body.data) buf_puts(&src, c.body.data);
        buf_printf(&src, "  kn_frag = %s;\n}\n", color);
        if (src.oom) {
            free(src.data);
            ok = 0;
        } else out->fragment = src.data;
    }
    free(c.body.data);
    free(c.node);
    free(c.def);
    free(c.st);
    if (!ok) {
        set_err(err, err_size, NULL, "the graph did not compile");
        mod_shader_program_free(out);
    }
    return ok;
}

void mod_shader_program_free(ModShaderProgram *program) {
    if (!program) return;
    free(program->fragment);
    program->fragment = NULL;
}
