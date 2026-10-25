// Krunker v10.0.3 AI parts: see shared/include/ai_parts.h. Every rule cites deob-10.0.3.js (the decoded
// v10.0.3 bundle, ~/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js). Maths in doubles like the JS; the
// hit mesh keeps its triangles in floats (the JS Float32Array).
//
// Deliberate differences (documented):
//   - The model is model.glb under assets_path() (the fetch hook, then read_file), not a fetch() of
//     user-assets.krunker.io; a file over 60 MB fails like the JS content-length check.
//   - The pose is computed per call, not cached per (clip, round(t * 30)) like AI.poseAt (:205263): the
//     cache only reuses the first pose of each 1/30 s, so a hit differs at most by that sub-frame.
//   - No lag-compensated AI state (fetchState): KN keeps no AI position history (game_ai_line_hit
//     neither), so a shot tests the AI's current position, yaw and pose.
//   - setPartDamage keeps 32 parts (AI_PARTS_MAX, like glowPart / setNodeVisible); v10.0.3's partDmg has no cap.
//   - A malformed GLB (an accessor out of the buffer, a bad index) leaves the affected data out instead
//     of failing the whole model as the JS throw does.
#include <ai_parts.h>
#include <net_ext.h>
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AI_HIT_MAX_TRIS 120000    // iíìíííï: iiÌîîíí || 120000 (:204471)
#define AI_HIT_CHUNK 48           // iîíïíii (:204603)
#define AI_SOLID_MAX_TRIS 6000    // applySolid: > 6000 sorted by area, cut (:205432)
#define AI_SOLID_MIN_AREA 0.01    // applySolid: area > 0.01 (:205425)
#define AI_REST_MAX_TRIS 6000     // liveObjects.loadModel: iîìïiìî(glb, 6000, true) (:212173): its tris and its hit mesh
#define AI_MODEL_MAX_BYTES 60000000 // loadModel: content-length > 60000000 "model too large" (:204239)

// ---------------------------------------------------------------- the model

typedef struct {
    char *name;
    int parent;
    double t[3], r[4], s[3];
    double center[3];
} RigNode;

typedef struct {
    float *times, *values;
    int count;      // keyframes
    int n;          // 3 (t, s) or 4 (r)
    int mode;       // 0 LINEAR, 1 STEP, 2 CUBICSPLINE
} Track;

typedef struct {
    Track tr[3];    // t, r, s (count 0 = none)
} Channel;

typedef struct {
    char *name;
    double duration;
    Channel *channels; // node_count
} Clip;

typedef struct {
    int start, end; // float offsets into the group's tris
    double min[3], max[3];
} Chunk;

typedef struct {
    int node;
    const char *name;
    float *tris;    // 9 floats per triangle
    int *seq;       // each triangle's place in the reader's walk: under a smaller cap (loadModel's 6000) only seq < cap exist
    int tri_count;
    double min[3], max[3];
    Chunk *chunks;
    int chunk_count;
} Group;

struct AiPartsModel {
    RigNode *nodes;
    int node_count;
    Clip *clips;
    int clip_count;
    Group *groups;
    int group_count;
    int tris;
    float *rest_tris;          // iîiîïîì: the mesh nodes' triangles at the rest pose (model space, 9 floats each)
    int rest_count;
};

typedef struct {
    const unsigned char *data;
    size_t length;
} Bin;

static double num(const cJSON *item, const double fallback) { return cJSON_IsNumber(item) ? item->valuedouble : fallback; }
static int has(const cJSON *item) { return item && !cJSON_IsNull(item); }

static char *dup_str(const char *s) {
    const size_t n = strlen(s ? s : "");
    char *out = malloc(n + 1);
    if (out) memcpy(out, s ? s : "", n + 1);
    return out;
}

static int comps_of(const char *type) { // iiïiîíì (:204037)
    if (!type) return 0;
    if (!strcmp(type, "SCALAR")) return 1;
    if (!strcmp(type, "VEC2")) return 2;
    if (!strcmp(type, "VEC3")) return 3;
    if (!strcmp(type, "VEC4")) return 4;
    if (!strcmp(type, "MAT4")) return 16;
    return 0;
}

static int bytes_of(const int component) { // iîííïii (:204044): Float32, Uint8, Uint16, Uint32 only
    return component == 5126 ? 4 : component == 5121 ? 1 : component == 5123 ? 2 : component == 5125 ? 4 : 0;
}

static double read_component(const unsigned char *p, const int component) {
    switch (component) {
        case 5126: {
            float f;
            memcpy(&f, p, 4);
            return f;
        }
        case 5121: return p[0];
        case 5123: return (double) (p[0] | p[1] << 8);
        default: return (double) ((uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24);
    }
}

// iììiïîî (:204050): an accessor's elements (count * comps, de-strided) as floats, NULL when unusable
static float *accessor_read(const cJSON *gltf, const Bin *bin, const cJSON *index_item, int *out_len) {
    *out_len = 0;
    if (!bin->data || !cJSON_IsNumber(index_item)) return NULL;
    const cJSON *acc = cJSON_GetArrayItem(cJSON_GetObjectItem(gltf, "accessors"), index_item->valueint);
    if (!acc) return NULL;
    const int comps = comps_of(cJSON_GetStringValue(cJSON_GetObjectItem(acc, "type")));
    const int component = (int) num(cJSON_GetObjectItem(acc, "componentType"), 0);
    const int bytes = bytes_of(component);
    const cJSON *view_index = cJSON_GetObjectItem(acc, "bufferView");
    if (!comps || !bytes || !cJSON_IsNumber(view_index)) return NULL;
    const cJSON *view = cJSON_GetArrayItem(cJSON_GetObjectItem(gltf, "bufferViews"), view_index->valueint);
    if (!view) return NULL;
    const double stride_bytes = num(cJSON_GetObjectItem(view, "byteStride"), 0);
    const size_t stride = stride_bytes > 0 ? (size_t) (stride_bytes / bytes) : (size_t) comps;
    const double offset = num(cJSON_GetObjectItem(view, "byteOffset"), 0) + num(cJSON_GetObjectItem(acc, "byteOffset"), 0);
    const double count = num(cJSON_GetObjectItem(acc, "count"), 0);
    if (!(count >= 1) || count > 1e8 || offset < 0) return NULL;
    const size_t n = (size_t) count;
    const size_t span = (n - 1) * stride + (size_t) comps; // elements the typed array view covers
    if ((size_t) offset + span * (size_t) bytes > bin->length) return NULL; // the JS view throws (RangeError)
    float *out = malloc(n * (size_t) comps * sizeof(float));
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < comps; k++)
            out[i * (size_t) comps + (size_t) k] = (float) read_component(bin->data + (size_t) offset + (i * stride + (size_t) k) * (size_t) bytes, component);
    *out_len = (int) (n * (size_t) comps);
    return out;
}

// iíïîïíi (:204165): a primitive's indices (Uint32 / Uint16 / else Uint8, no stride), NULL when none
static uint32_t *indices_read(const cJSON *gltf, const Bin *bin, const cJSON *index_item, int *out_len) {
    *out_len = 0;
    if (!cJSON_IsNumber(index_item) || !bin->data) return NULL;
    const cJSON *acc = cJSON_GetArrayItem(cJSON_GetObjectItem(gltf, "accessors"), index_item->valueint);
    if (!acc) return NULL;
    const cJSON *view = cJSON_GetArrayItem(cJSON_GetObjectItem(gltf, "bufferViews"), (int) num(cJSON_GetObjectItem(acc, "bufferView"), -1));
    if (!view) return NULL;
    const int component = (int) num(cJSON_GetObjectItem(acc, "componentType"), 0);
    const int bytes = component == 5125 ? 4 : component == 5123 ? 2 : 1;
    const double offset = num(cJSON_GetObjectItem(view, "byteOffset"), 0) + num(cJSON_GetObjectItem(acc, "byteOffset"), 0);
    const double count = num(cJSON_GetObjectItem(acc, "count"), 0);
    if (!(count >= 0) || count > 1e9 || offset < 0 || (size_t) offset + (size_t) count * (size_t) bytes > bin->length) return NULL;
    const size_t n = (size_t) count;
    uint32_t *out = malloc((n ? n : 1) * sizeof(uint32_t));
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) out[i] = (uint32_t) read_component(bin->data + (size_t) offset + i * (size_t) bytes, bytes == 4 ? 5125 : bytes == 2 ? 5123 : 5121);
    *out_len = (int) n;
    return out;
}

// ---- matrices (column-major, gl-matrix style like the JS helpers)

static void m4_compose(const double *t, const double *r, const double *s, double *m) { // iìiííïi (:204129)
    const double x = r[0], y = r[1], z = r[2], w = r[3];
    m[0] = (1 - (y * y + z * z) * 2) * s[0];
    m[1] = (x * y + z * w) * 2 * s[0];
    m[2] = (x * z - y * w) * 2 * s[0];
    m[3] = 0;
    m[4] = (x * y - z * w) * 2 * s[1];
    m[5] = (1 - (x * x + z * z) * 2) * s[1];
    m[6] = (y * z + x * w) * 2 * s[1];
    m[7] = 0;
    m[8] = (x * z + y * w) * 2 * s[2];
    m[9] = (y * z - x * w) * 2 * s[2];
    m[10] = (1 - (x * x + y * y) * 2) * s[2];
    m[11] = 0;
    m[12] = t[0];
    m[13] = t[1];
    m[14] = t[2];
    m[15] = 1;
}

static void m4_mul(const double *a, const double *b, double *out) { // iîîìííï (:204152): a * b
    double r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[j] * b[i * 4] + a[4 + j] * b[i * 4 + 1] + a[8 + j] * b[i * 4 + 2] + a[12 + j] * b[i * 4 + 3];
    memcpy(out, r, sizeof(r));
}

static void m4_point(const double *m, const double x, const double y, const double z, double *out) { // iïìììïï (:204260)
    out[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
    out[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
    out[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
}

static int m4_invert(const double *a, double *out) { // iìiíìîi (:204265): 0 when singular
    const double a00 = a[0], a01 = a[1], a02 = a[2], a03 = a[3], a10 = a[4], a11 = a[5], a12 = a[6], a13 = a[7];
    const double a20 = a[8], a21 = a[9], a22 = a[10], a23 = a[11], a30 = a[12], a31 = a[13], a32 = a[14], a33 = a[15];
    const double b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10, b02 = a00 * a13 - a03 * a10;
    const double b03 = a01 * a12 - a02 * a11, b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12;
    const double b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30, b08 = a20 * a33 - a23 * a30;
    const double b09 = a21 * a32 - a22 * a31, b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;
    double det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
    if (!det) return 0;
    det = 1 / det;
    out[0] = (a11 * b11 - a12 * b10 + a13 * b09) * det;
    out[1] = (a02 * b10 - a01 * b11 - a03 * b09) * det;
    out[2] = (a31 * b05 - a32 * b04 + a33 * b03) * det;
    out[3] = (a22 * b04 - a21 * b05 - a23 * b03) * det;
    out[4] = (a12 * b08 - a10 * b11 - a13 * b07) * det;
    out[5] = (a00 * b11 - a02 * b08 + a03 * b07) * det;
    out[6] = (a32 * b02 - a30 * b05 - a33 * b01) * det;
    out[7] = (a20 * b05 - a22 * b02 + a23 * b01) * det;
    out[8] = (a10 * b10 - a11 * b08 + a13 * b06) * det;
    out[9] = (a01 * b08 - a00 * b10 - a03 * b06) * det;
    out[10] = (a30 * b04 - a31 * b02 + a33 * b00) * det;
    out[11] = (a21 * b02 - a20 * b04 - a23 * b00) * det;
    out[12] = (a11 * b07 - a10 * b09 - a12 * b06) * det;
    out[13] = (a00 * b09 - a01 * b07 + a02 * b06) * det;
    out[14] = (a31 * b01 - a30 * b03 - a32 * b00) * det;
    out[15] = (a20 * b03 - a21 * b01 + a22 * b00) * det;
    return 1;
}

// ---- the rig

static int track_mode(const char *s) { return s && !strcmp(s, "STEP") ? 1 : s && !strcmp(s, "CUBICSPLINE") ? 2 : 0; }

static void rig_build(AiPartsModel *m, const cJSON *gltf, const Bin *bin) { // iïÌìììï (:204378)
    const cJSON *nodes = cJSON_GetObjectItem(gltf, "nodes");
    const cJSON *meshes = cJSON_GetObjectItem(gltf, "meshes");
    const cJSON *accessors = cJSON_GetObjectItem(gltf, "accessors");
    const int count = cJSON_IsArray(nodes) ? cJSON_GetArraySize(nodes) : 0;
    m->nodes = calloc(count ? (size_t) count : 1, sizeof(RigNode));
    if (!m->nodes) return;
    m->node_count = count;
    for (int i = 0; i < count; i++) {
        const cJSON *n = cJSON_GetArrayItem(nodes, i);
        RigNode *r = &m->nodes[i];
        r->name = dup_str(cJSON_GetStringValue(cJSON_GetObjectItem(n, "name")));
        r->parent = -1;
        const cJSON *t = cJSON_GetObjectItem(n, "translation"), *q = cJSON_GetObjectItem(n, "rotation");
        const cJSON *s = cJSON_GetObjectItem(n, "scale"), *mat = cJSON_GetObjectItem(n, "matrix");
        // t: translation || (matrix ? matrix[12..14] : 0); r / s never from the matrix (:204386)
        for (int k = 0; k < 3; k++)
            r->t[k] = has(t) ? num(cJSON_GetArrayItem(t, k), 0) : has(mat) ? num(cJSON_GetArrayItem(mat, 12 + k), 0) : 0;
        for (int k = 0; k < 4; k++) r->r[k] = has(q) ? num(cJSON_GetArrayItem(q, k), 0) : k == 3;
        for (int k = 0; k < 3; k++) r->s[k] = has(s) ? num(cJSON_GetArrayItem(s, k), 0) : 1;
    }
    for (int i = 0; i < count; i++) {
        const cJSON *n = cJSON_GetArrayItem(nodes, i);
        const cJSON *child;
        cJSON_ArrayForEach(child, cJSON_GetObjectItem(n, "children")) {
            const int c = cJSON_IsNumber(child) ? child->valueint : -1;
            if (c >= 0 && c < count) m->nodes[c].parent = i;
        }
        const cJSON *mesh_index = cJSON_GetObjectItem(n, "mesh");
        const cJSON *mesh = cJSON_IsNumber(mesh_index) ? cJSON_GetArrayItem(meshes, mesh_index->valueint) : NULL;
        if (!mesh) continue;
        double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
        const cJSON *prim;
        cJSON_ArrayForEach(prim, cJSON_GetObjectItem(mesh, "primitives")) {
            const cJSON *pos = cJSON_GetObjectItem(cJSON_GetObjectItem(prim, "attributes"), "POSITION");
            const cJSON *acc = cJSON_IsNumber(pos) ? cJSON_GetArrayItem(accessors, pos->valueint) : NULL;
            const cJSON *amin = cJSON_GetObjectItem(acc, "min"), *amax = cJSON_GetObjectItem(acc, "max");
            if (!amin || !amax) continue;
            for (int k = 0; k < 3; k++) {
                lo[k] = fmin(lo[k], num(cJSON_GetArrayItem(amin, k), NAN));
                hi[k] = fmax(hi[k], num(cJSON_GetArrayItem(amax, k), NAN));
            }
        }
        if (lo[0] <= hi[0])
            for (int k = 0; k < 3; k++) m->nodes[i].center[k] = (lo[k] + hi[k]) / 2;
    }
    // the clips (:204418-204447): name || "Clip" + index; a later clip of the same name replaces it
    const cJSON *anims = cJSON_GetObjectItem(gltf, "animations");
    const int clip_count = cJSON_IsArray(anims) ? cJSON_GetArraySize(anims) : 0;
    m->clips = calloc(clip_count ? (size_t) clip_count : 1, sizeof(Clip));
    if (!m->clips) return;
    for (int a = 0; a < clip_count; a++) {
        const cJSON *anim = cJSON_GetArrayItem(anims, a);
        char fallback[24];
        snprintf(fallback, sizeof(fallback), "Clip%d", a);
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(anim, "name"));
        if (!name || !name[0]) name = fallback;
        Clip *clip = NULL;
        for (int c = 0; c < m->clip_count && !clip; c++)
            if (!strcmp(m->clips[c].name, name)) clip = &m->clips[c];
        if (clip) { // the same key: the new record replaces the old one
            for (int nd = 0; nd < m->node_count; nd++)
                for (int k = 0; k < 3; k++) {
                    free(clip->channels[nd].tr[k].times);
                    free(clip->channels[nd].tr[k].values);
                }
            free(clip->channels);
        } else {
            clip = &m->clips[m->clip_count++];
            clip->name = dup_str(name);
        }
        clip->duration = 0;
        clip->channels = calloc(m->node_count ? (size_t) m->node_count : 1, sizeof(Channel));
        if (!clip->channels) continue;
        const cJSON *samplers = cJSON_GetObjectItem(anim, "samplers");
        const cJSON *ch;
        cJSON_ArrayForEach(ch, cJSON_GetObjectItem(anim, "channels")) {
            const cJSON *sampler = cJSON_GetArrayItem(samplers, (int) num(cJSON_GetObjectItem(ch, "sampler"), -1));
            const cJSON *target = cJSON_GetObjectItem(ch, "target");
            const cJSON *node = cJSON_GetObjectItem(target, "node");
            if (!sampler || !target || !cJSON_IsNumber(node) || !bin->data) continue;
            const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(target, "path"));
            const int k = path && !strcmp(path, "translation") ? 0 : path && !strcmp(path, "rotation") ? 1 : path && !strcmp(path, "scale") ? 2 : -1;
            if (k < 0 || node->valueint < 0 || node->valueint >= m->node_count) continue;
            int in_len = 0, out_len = 0;
            float *in = accessor_read(gltf, bin, cJSON_GetObjectItem(sampler, "input"), &in_len);
            float *out = accessor_read(gltf, bin, cJSON_GetObjectItem(sampler, "output"), &out_len);
            if (!in || !out || !in_len) {
                free(in);
                free(out);
                continue;
            }
            clip->duration = fmax(clip->duration, in[in_len - 1]);
            Track *tr = &clip->channels[node->valueint].tr[k];
            free(tr->times);
            free(tr->values);
            tr->times = in;
            tr->values = out;
            tr->count = in_len;
            tr->n = k == 1 ? 4 : 3;
            tr->mode = track_mode(cJSON_GetStringValue(cJSON_GetObjectItem(sampler, "interpolation")));
            // the samples must cover the keyframes (the JS would read undefined -> NaN); a short output is dropped
            const int stride = tr->mode == 2 ? tr->n * 3 : tr->n;
            if (out_len < tr->count * stride) {
                free(tr->times);
                free(tr->values);
                memset(tr, 0, sizeof(*tr));
            }
        }
    }
}

// iiïïïíì (:204071): a track sampled at t (seconds) into out[n]
static void track_sample(const Track *tr, const double t, double *out) {
    const int n = tr->n;
    const int cubic = tr->mode == 2;
    const int stride = cubic ? n * 3 : n;
    const int off = cubic ? n : 0;
    const int last = tr->count - 1;
    if (t <= tr->times[0] || last == 0) {
        for (int k = 0; k < n; k++) out[k] = tr->values[off + k];
        return;
    }
    if (t >= tr->times[last]) {
        for (int k = 0; k < n; k++) out[k] = tr->values[last * stride + off + k];
        return;
    }
    int lo = 0, hi = last;
    while (hi - lo > 1) {
        const int mid = (lo + hi) >> 1;
        if (tr->times[mid] <= t) lo = mid;
        else hi = mid;
    }
    const int a = lo * stride + off, b = hi * stride + off;
    if (tr->mode == 1) {
        for (int k = 0; k < n; k++) out[k] = tr->values[a + k];
        return;
    }
    const double u = (t - tr->times[lo]) / (tr->times[hi] - tr->times[lo]);
    if (n == 4) {
        double dot = 0;
        for (int k = 0; k < 4; k++) dot += (double) tr->values[a + k] * tr->values[b + k];
        const double sign = dot < 0 ? -1 : 1;
        double len = 0;
        for (int k = 0; k < 4; k++) {
            out[k] = tr->values[a + k] * (1 - u) + sign * tr->values[b + k] * u;
            len += out[k] * out[k];
        }
        len = sqrt(len);
        if (!len) len = 1;
        for (int k = 0; k < 4; k++) out[k] /= len;
    } else {
        for (int k = 0; k < n; k++) out[k] = tr->values[a + k] * (1 - u) + tr->values[b + k] * u;
    }
}

static const Clip *clip_find(const AiPartsModel *m, const char *name) {
    for (int i = 0; m && name && i < m->clip_count; i++)
        if (!strcmp(m->clips[i].name, name)) return &m->clips[i];
    return NULL;
}

// A node's local matrix: its clip channels at t, else its rest TRS
static void node_local(const AiPartsModel *m, const Clip *clip, const int i, const double t, double *out) {
    const RigNode *n = &m->nodes[i];
    const Channel *ch = clip ? &clip->channels[i] : NULL;
    double tv[3], rv[4], sv[3];
    const double *tp = n->t, *rp = n->r, *sp = n->s;
    if (ch && ch->tr[0].count) track_sample(&ch->tr[0], t, tv), tp = tv;
    if (ch && ch->tr[1].count) track_sample(&ch->tr[1], t, rv), rp = rv;
    if (ch && ch->tr[2].count) track_sample(&ch->tr[2], t, sv), sp = sv;
    m4_compose(tp, rp, sp, out);
}

// iíîìíïî (:204317): every node's model-space matrix (parent * local); `done` guards against a cyclic file
static void pose_node(const AiPartsModel *m, const Clip *clip, const double t, const int i, double *out, unsigned char *done, int depth) {
    if (done[i] || depth > m->node_count) return;
    double local[16];
    node_local(m, clip, i, t, local);
    const int p = m->nodes[i].parent;
    if (p >= 0 && p < m->node_count) {
        pose_node(m, clip, t, p, out, done, depth + 1);
        m4_mul(&out[p * 16], local, &out[i * 16]);
    } else {
        memcpy(&out[i * 16], local, sizeof(local));
    }
    done[i] = 1;
}

static double *pose_alloc(const AiPartsModel *m, const char *clip_name, const double t) {
    double *out = calloc(m->node_count ? (size_t) m->node_count * 16 : 16, sizeof(double));
    unsigned char *done = calloc(m->node_count ? (size_t) m->node_count : 1, 1);
    if (!out || !done) {
        free(out);
        free(done);
        return NULL;
    }
    const Clip *clip = clip_find(m, clip_name);
    for (int i = 0; i < m->node_count; i++) pose_node(m, clip, t, i, out, done, 0);
    free(done);
    return out;
}

void ai_parts_model_pose(const AiPartsModel *m, const char *clip, const double t, double *out) {
    if (!m || !out) return;
    double *pose = pose_alloc(m, clip, t);
    if (!pose) return;
    memcpy(out, pose, (size_t) m->node_count * 16 * sizeof(double));
    free(pose);
}

// ---- the hit mesh (iíìíííï :204466)

typedef struct {
    int node;
    double *v;      // 9 doubles per triangle (the JS array before the Float32Array copy)
    int *seq;       // each one's place in the walk (Group.seq)
    int count, cap;
    double min[3], max[3];
} GroupBuild;

static GroupBuild *group_for(GroupBuild **groups, int *count, int *cap, const int node) {
    for (int i = 0; i < *count; i++)
        if ((*groups)[i].node == node) return &(*groups)[i];
    if (*count == *cap) {
        const int n = *cap ? *cap * 2 : 8;
        GroupBuild *g = realloc(*groups, (size_t) n * sizeof(GroupBuild));
        if (!g) return NULL;
        *groups = g;
        *cap = n;
    }
    GroupBuild *g = &(*groups)[(*count)++];
    memset(g, 0, sizeof(*g));
    g->node = node;
    for (int k = 0; k < 3; k++) {
        g->min[k] = INFINITY;
        g->max[k] = -INFINITY;
    }
    return g;
}

static int group_push(GroupBuild *g, const double *tri, const int seq) {
    if (g->count == g->cap) {
        const int n = g->cap ? g->cap * 2 : 64;
        double *v = realloc(g->v, (size_t) n * 9 * sizeof(double));
        if (!v) return 0;
        g->v = v;
        int *s = realloc(g->seq, (size_t) n * sizeof(int));
        if (!s) return 0;
        g->seq = s;
        g->cap = n;
    }
    memcpy(&g->v[(size_t) g->count * 9], tri, 9 * sizeof(double));
    g->seq[g->count] = seq;
    g->count++;
    for (int i = 0; i < 9; i++) { // iìIîîïï (:204481)
        g->min[i % 3] = fmin(g->min[i % 3], tri[i]);
        g->max[i % 3] = fmax(g->max[i % 3], tri[i]);
    }
    return 1;
}

static int g_sort_axis;
static const double *g_sort_v;
static int sort_cmp(const void *pa, const void *pb) { // stable: ties keep the triangle order (JS sort is stable)
    const int a = *(const int *) pa, b = *(const int *) pb;
    const double *va = &g_sort_v[(size_t) a * 9], *vb = &g_sort_v[(size_t) b * 9];
    const double ka = va[g_sort_axis] + va[3 + g_sort_axis] + va[6 + g_sort_axis];
    const double kb = vb[g_sort_axis] + vb[3 + g_sort_axis] + vb[6 + g_sort_axis];
    return ka < kb ? -1 : ka > kb ? 1 : a < b ? -1 : a > b;
}

static int group_cmp(const void *pa, const void *pb) { // Object.keys of integer keys: ascending
    return ((const GroupBuild *) pa)->node - ((const GroupBuild *) pb)->node;
}

static void hit_mesh_build(AiPartsModel *m, const cJSON *gltf, const Bin *bin) {
    if (!bin->data) return;
    const cJSON *nodes = cJSON_GetObjectItem(gltf, "nodes"), *meshes = cJSON_GetObjectItem(gltf, "meshes");
    const cJSON *skins = cJSON_GetObjectItem(gltf, "skins");
    GroupBuild *groups = NULL;
    int group_count = 0, group_cap = 0, total = 0;
    for (int ni = 0; ni < m->node_count; ni++) {
        const cJSON *node = cJSON_GetArrayItem(nodes, ni);
        const cJSON *mesh_index = cJSON_GetObjectItem(node, "mesh");
        const cJSON *mesh = cJSON_IsNumber(mesh_index) ? cJSON_GetArrayItem(meshes, mesh_index->valueint) : NULL;
        if (!mesh) continue;
        const cJSON *skin_index = cJSON_GetObjectItem(node, "skin");
        const cJSON *skin = cJSON_IsNumber(skin_index) && skins ? cJSON_GetArrayItem(skins, skin_index->valueint) : NULL;
        int ibm_len = 0;
        float *ibm = skin && has(cJSON_GetObjectItem(skin, "inverseBindMatrices")) ? accessor_read(gltf, bin, cJSON_GetObjectItem(skin, "inverseBindMatrices"), &ibm_len) : NULL;
        const cJSON *joints_list = cJSON_GetObjectItem(skin, "joints");
        const cJSON *prim;
        cJSON_ArrayForEach(prim, cJSON_GetObjectItem(mesh, "primitives")) {
            const cJSON *mode = cJSON_GetObjectItem(prim, "mode");
            if (has(mode) && num(mode, -1) != 4) continue;
            const cJSON *attrs = cJSON_GetObjectItem(prim, "attributes");
            int pos_len = 0, idx_len = 0, j_len = 0, w_len = 0;
            float *pos = accessor_read(gltf, bin, cJSON_GetObjectItem(attrs, "POSITION"), &pos_len);
            if (!pos) continue;
            uint32_t *idx = indices_read(gltf, bin, cJSON_GetObjectItem(prim, "indices"), &idx_len);
            const int indexed = has(cJSON_GetObjectItem(prim, "indices")) && idx;
            float *joints = skin && has(cJSON_GetObjectItem(attrs, "JOINTS_0")) ? accessor_read(gltf, bin, cJSON_GetObjectItem(attrs, "JOINTS_0"), &j_len) : NULL;
            float *weights = skin && has(cJSON_GetObjectItem(attrs, "WEIGHTS_0")) ? accessor_read(gltf, bin, cJSON_GetObjectItem(attrs, "WEIGHTS_0"), &w_len) : NULL;
            const int count = indexed ? idx_len : pos_len / 3;
            for (int i = 0; i + 2 < count && total < AI_HIT_MAX_TRIS; i += 3) {
                const int first = indexed ? (int) idx[i] : i;
                int group_node = ni;
                const float *mat = NULL;
                if (joints && weights && ibm) { // the first vertex's heaviest joint (:204518-204527)
                    int best = 0;
                    for (int k = 1; k < 4; k++)
                        if ((size_t) first * 4 + 3 < (size_t) w_len && weights[first * 4 + k] > weights[first * 4 + best]) best = k;
                    const int j = (size_t) first * 4 + 3 < (size_t) j_len ? (int) joints[first * 4 + best] : -1;
                    const cJSON *joint = j >= 0 ? cJSON_GetArrayItem(joints_list, j) : NULL;
                    if (!cJSON_IsNumber(joint) || joint->valueint < 0 || joint->valueint >= m->node_count || (j + 1) * 16 > ibm_len) {
                        total++;
                        continue; // the JS reads past the skin (TypeError / NaN): no usable triangle
                    }
                    group_node = joint->valueint;
                    mat = &ibm[j * 16];
                }
                double tri[9];
                int ok = 1;
                for (int k = 0; k < 3 && ok; k++) {
                    const int v = indexed ? (int) idx[i + k] : i + k;
                    if (v < 0 || v * 3 + 2 >= pos_len) {
                        ok = 0;
                        break;
                    }
                    if (mat) {
                        double dm[16];
                        for (int e = 0; e < 16; e++) dm[e] = mat[e];
                        m4_point(dm, pos[v * 3], pos[v * 3 + 1], pos[v * 3 + 2], &tri[k * 3]);
                    } else {
                        tri[k * 3] = pos[v * 3];
                        tri[k * 3 + 1] = pos[v * 3 + 1];
                        tri[k * 3 + 2] = pos[v * 3 + 2];
                    }
                }
                const int seq = total++;
                if (!ok) continue;
                GroupBuild *g = group_for(&groups, &group_count, &group_cap, group_node);
                if (g) group_push(g, tri, seq);
            }
            free(pos);
            free(idx);
            free(joints);
            free(weights);
        }
        free(ibm);
    }
    m->tris = total;
    if (group_count) qsort(groups, (size_t) group_count, sizeof(GroupBuild), group_cmp);
    m->groups = calloc(group_count ? (size_t) group_count : 1, sizeof(Group));
    for (int gi = 0; m->groups && gi < group_count; gi++) {
        GroupBuild *b = &groups[gi];
        Group *g = &m->groups[m->group_count++];
        g->node = b->node;
        g->name = m->nodes[b->node].name;
        g->tri_count = b->count;
        memcpy(g->min, b->min, sizeof(g->min));
        memcpy(g->max, b->max, sizeof(g->max));
        int axis = 0; // the longest box axis (:204551)
        for (int k = 1; k < 3; k++)
            if (b->max[k] - b->min[k] > b->max[axis] - b->min[axis]) axis = k;
        int *order = malloc((b->count ? (size_t) b->count : 1) * sizeof(int));
        g->tris = malloc((b->count ? (size_t) b->count : 1) * 9 * sizeof(float));
        g->seq = malloc((b->count ? (size_t) b->count : 1) * sizeof(int));
        if (!order || !g->tris || !g->seq) {
            free(order);
            g->tri_count = 0;
            continue;
        }
        for (int i = 0; i < b->count; i++) order[i] = i;
        g_sort_axis = axis;
        g_sort_v = b->v;
        qsort(order, (size_t) b->count, sizeof(int), sort_cmp);
        for (int i = 0; i < b->count; i++) {
            for (int e = 0; e < 9; e++) g->tris[i * 9 + e] = (float) b->v[(size_t) order[i] * 9 + (size_t) e];
            g->seq[i] = b->seq[order[i]];
        }
        free(order);
        g->chunk_count = (b->count + AI_HIT_CHUNK - 1) / AI_HIT_CHUNK;
        g->chunks = calloc(g->chunk_count ? (size_t) g->chunk_count : 1, sizeof(Chunk));
        for (int c = 0; g->chunks && c < g->chunk_count; c++) {
            Chunk *ck = &g->chunks[c];
            const int start = c * AI_HIT_CHUNK, end = start + AI_HIT_CHUNK < b->count ? start + AI_HIT_CHUNK : b->count;
            ck->start = start * 9;
            ck->end = end * 9;
            for (int k = 0; k < 3; k++) {
                ck->min[k] = INFINITY;
                ck->max[k] = -INFINITY;
            }
            for (int e = ck->start; e < ck->end; e += 3)
                for (int k = 0; k < 3; k++) {
                    ck->min[k] = fmin(ck->min[k], g->tris[e + k]);
                    ck->max[k] = fmax(ck->max[k], g->tris[e + k]);
                }
        }
        if (!g->chunks) g->chunk_count = 0;
    }
    for (int i = 0; i < group_count; i++) {
        free(groups[i].v);
        free(groups[i].seq);
    }
    free(groups);
}

// iîiîïîì (:204179-204218): every mesh node's triangles (mode TRIANGLES or none) at the rest pose, through its
// node chain's TRS from the root, in model space; at most `max` triangles (the pushes stop at max * 9 floats). A
// vertex index past the positions pushes NaN as the JS reads undefined.
static void rest_tris_build(AiPartsModel *m, const cJSON *gltf, const Bin *bin, const int max) {
    const cJSON *nodes = cJSON_GetObjectItem(gltf, "nodes"), *meshes = cJSON_GetObjectItem(gltf, "meshes");
    if (!bin->data || !cJSON_IsArray(nodes) || !m->nodes) return;
    const size_t limit = (size_t) max * 9;
    float *out = malloc(limit * sizeof(float));
    if (!out) return;
    size_t len = 0;
    int index = -1;
    const cJSON *node;
    cJSON_ArrayForEach(node, nodes) {
        index++;
        const cJSON *mesh_index = cJSON_GetObjectItem(node, "mesh");
        const cJSON *mesh = cJSON_IsNumber(mesh_index) ? cJSON_GetArrayItem(meshes, mesh_index->valueint) : NULL;
        if (!mesh || index >= m->node_count) continue;
        int chain[256], depth = 0;
        for (int k = index; k >= 0 && k < m->node_count && depth < 256; k = m->nodes[k].parent) chain[depth++] = k;
        static const double zero[3] = {0, 0, 0}, unit_q[4] = {0, 0, 0, 1}, one[3] = {1, 1, 1};
        double world[16];
        m4_compose(zero, unit_q, one, world);
        for (int c = depth - 1; c >= 0; c--) {
            double local[16];
            const RigNode *n = &m->nodes[chain[c]];
            m4_compose(n->t, n->r, n->s, local);
            m4_mul(world, local, world);
        }
        const cJSON *prim;
        cJSON_ArrayForEach(prim, cJSON_GetObjectItem(mesh, "primitives")) {
            const cJSON *mode = cJSON_GetObjectItem(prim, "mode");
            if (has(mode) && num(mode, 4) != 4) continue;
            int pos_len = 0, idx_len = 0;
            float *pos = accessor_read(gltf, bin, cJSON_GetObjectItem(cJSON_GetObjectItem(prim, "attributes"), "POSITION"), &pos_len);
            if (!pos) continue;
            uint32_t *idx = indices_read(gltf, bin, cJSON_GetObjectItem(prim, "indices"), &idx_len);
            const int count = idx ? idx_len : pos_len / 3;
            for (int i = 0; i < count - count % 3 && len < limit; i++) {
                const long v = idx ? (long) idx[i] : i;
                if (v * 3 + 2 >= pos_len) {
                    out[len] = out[len + 1] = out[len + 2] = NAN;
                } else {
                    double q[3];
                    m4_point(world, pos[v * 3], pos[v * 3 + 1], pos[v * 3 + 2], q);
                    out[len] = (float) q[0];
                    out[len + 1] = (float) q[1];
                    out[len + 2] = (float) q[2];
                }
                len += 3;
            }
            free(pos);
            free(idx);
        }
    }
    len -= len % 9;
    m->rest_tris = out;
    m->rest_count = (int) (len / 9);
}

const float *ai_parts_model_rest_tris(const AiPartsModel *m, size_t *count) {
    if (count) *count = m && m->rest_tris ? (size_t) m->rest_count : 0;
    return m ? m->rest_tris : NULL;
}

AiPartsModel *ai_parts_model_parse(const unsigned char *glb, const size_t length) { // iìÏiïìî (:204350)
    if (!glb || length < 12) return NULL;
    const uint32_t magic = (uint32_t) glb[0] | (uint32_t) glb[1] << 8 | (uint32_t) glb[2] << 16 | (uint32_t) glb[3] << 24;
    if (magic != 1179937895u) return NULL; // "not a GLB"
    const unsigned char *json = NULL;
    size_t json_len = 0;
    Bin bin = {0};
    for (size_t at = 12; at + 8 <= length;) {
        const uint32_t len = (uint32_t) glb[at] | (uint32_t) glb[at + 1] << 8 | (uint32_t) glb[at + 2] << 16 | (uint32_t) glb[at + 3] << 24;
        const uint32_t type = (uint32_t) glb[at + 4] | (uint32_t) glb[at + 5] << 8 | (uint32_t) glb[at + 6] << 16 | (uint32_t) glb[at + 7] << 24;
        if (at + 8 + (size_t) len > length) return NULL; // the Uint8Array view throws
        if (type == 1313821514u) json = glb + at + 8, json_len = len;
        else if (type == 5130562u) bin.data = glb + at + 8, bin.length = len;
        at += 8 + (size_t) len;
    }
    if (!json) return NULL; // "GLB has no JSON chunk"
    cJSON *gltf = cJSON_ParseWithLength((const char *) json, json_len);
    if (!gltf) return NULL;
    AiPartsModel *m = calloc(1, sizeof(AiPartsModel));
    if (m) {
        rig_build(m, gltf, &bin);
        hit_mesh_build(m, gltf, &bin);
        rest_tris_build(m, gltf, &bin, AI_REST_MAX_TRIS); // (the live assets' collision, ks_entities.c)
    }
    cJSON_Delete(gltf);
    return m;
}

void ai_parts_model_free(AiPartsModel *m) {
    if (!m) return;
    for (int c = 0; m->clips && c < m->clip_count; c++) {
        for (int n = 0; m->clips[c].channels && n < m->node_count; n++)
            for (int k = 0; k < 3; k++) {
                free(m->clips[c].channels[n].tr[k].times);
                free(m->clips[c].channels[n].tr[k].values);
            }
        free(m->clips[c].channels);
        free(m->clips[c].name);
    }
    free(m->clips);
    for (int g = 0; m->groups && g < m->group_count; g++) {
        free(m->groups[g].tris);
        free(m->groups[g].seq);
        free(m->groups[g].chunks);
    }
    free(m->groups);
    for (int n = 0; m->nodes && n < m->node_count; n++) free(m->nodes[n].name);
    free(m->nodes);
    free(m->rest_tris);
    free(m);
}

int ai_parts_model_node_count(const AiPartsModel *m) { return m ? m->node_count : 0; }
int ai_parts_model_hit_tris(const AiPartsModel *m) { return m ? m->tris : 0; }
const char *ai_parts_model_node_name(const AiPartsModel *m, const int i) { return m && i >= 0 && i < m->node_count ? m->nodes[i].name : ""; }
int ai_parts_model_node_parent(const AiPartsModel *m, const int i) { return m && i >= 0 && i < m->node_count ? m->nodes[i].parent : -1; }
int ai_parts_model_has_clip(const AiPartsModel *m, const char *name) { return clip_find(m, name) != NULL; }
float ai_parts_model_clip_duration(const AiPartsModel *m, const char *name) {
    const Clip *c = clip_find(m, name);
    return c ? (float) c->duration : 0.0f;
}

int ai_parts_model_node_find(const AiPartsModel *m, const char *name) { // byName: the first node of a non-empty name
    if (!m || !name || !name[0]) return -1;
    for (int i = 0; i < m->node_count; i++)
        if (!strcmp(m->nodes[i].name, name)) return i;
    return -1;
}

// ---- the ray (:204604-204687)

// iííìììì: the slab test of origin + t * dir (t in [0, tmax]) against a box; -1 when it misses
static double ray_box(const double *o, const double *d, const double *lo, const double *hi, const double tmax_in) {
    double tmin = 0, tmax = tmax_in;
    for (int k = 0; k < 3; k++) {
        if (fabs(d[k]) < 1e-12) {
            if (o[k] < lo[k] || o[k] > hi[k]) return -1;
        } else {
            double t1 = (lo[k] - o[k]) / d[k], t2 = (hi[k] - o[k]) / d[k];
            if (t1 > t2) {
                const double s = t1;
                t1 = t2;
                t2 = s;
            }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return -1;
        }
    }
    return tmin;
}

typedef int (*HiddenFn)(const AiPartsModel*, const void *user, const char *group_name);

// iïïìîiì: the nearest triangle hit of the posed mesh as read with at most `max_tris` triangles (the walk's first
// max_tris: AI_HIT_MAX_TRIS, all of them, or loadModel's AI_REST_MAX_TRIS; the boxes of the whole mesh only prefilter);
// returns its group or -1
static int mesh_hit(const AiPartsModel *m, const double *worlds, const double *origin, const double *dir, HiddenFn hidden, const void *user, const int max_tris,
                    double *best_t) {
    int best = -1;
    double inv[16], o[3], d[3];
    for (int gi = 0; gi < m->group_count; gi++) {
        const Group *g = &m->groups[gi];
        if (hidden && hidden(m, user, g->name)) continue;
        if (!m4_invert(&worlds[g->node * 16], inv)) continue;
        m4_point(inv, origin[0], origin[1], origin[2], o);
        d[0] = inv[0] * dir[0] + inv[4] * dir[1] + inv[8] * dir[2];
        d[1] = inv[1] * dir[0] + inv[5] * dir[1] + inv[9] * dir[2];
        d[2] = inv[2] * dir[0] + inv[6] * dir[1] + inv[10] * dir[2];
        if (ray_box(o, d, g->min, g->max, best >= 0 ? *best_t : 1) < 0) continue;
        const float *v = g->tris;
        for (int c = 0; c < g->chunk_count; c++) {
            const Chunk *ck = &g->chunks[c];
            if (ray_box(o, d, ck->min, ck->max, best >= 0 ? *best_t : 1) < 0) continue;
            for (int e = ck->start; e < ck->end; e += 9) {
                if (g->seq[e / 9] >= max_tris) continue;
                const double e1x = (double) v[e + 3] - v[e], e1y = (double) v[e + 4] - v[e + 1], e1z = (double) v[e + 5] - v[e + 2];
                const double e2x = (double) v[e + 6] - v[e], e2y = (double) v[e + 7] - v[e + 1], e2z = (double) v[e + 8] - v[e + 2];
                const double px = d[1] * e2z - d[2] * e2y, py = d[2] * e2x - d[0] * e2z, pz = d[0] * e2y - d[1] * e2x;
                const double det = e1x * px + e1y * py + e1z * pz;
                if (fabs(det) < 1e-12) continue;
                const double idet = 1 / det;
                const double sx = o[0] - v[e], sy = o[1] - v[e + 1], sz = o[2] - v[e + 2];
                const double u = (sx * px + sy * py + sz * pz) * idet;
                if (u < 0 || u > 1) continue;
                const double qx = sy * e1z - sz * e1y, qy = sz * e1x - sx * e1z, qz = sx * e1y - sy * e1x;
                const double w = (d[0] * qx + d[1] * qy + d[2] * qz) * idet;
                if (w < 0 || u + w > 1) continue;
                const double t = (e2x * qx + e2y * qy + e2z * qz) * idet;
                if (t >= 0 && t <= 1 && (best < 0 || t < *best_t)) {
                    *best_t = fmax(t, 0.0001);
                    best = gi;
                }
            }
        }
    }
    return best;
}

// ---------------------------------------------------------------- per-game state

typedef struct {
    int asset;
    int state;
    AiPartsModel *model;
} ModelSlot;

struct AiPartsState {
    ModelSlot *models;
    int model_count, model_cap;
    AiParts *entries[GAME_AI_MAX * 2];
    int entry_count;
    char *chunk;          // the "aip" record being assembled (client)
    size_t chunk_len;
    int chunk_next, chunk_parts;
};

static AiPartsState *state_of(Game *game, const int create) {
    GameAIs *list = create ? game_ais(game) : game ? game->ai : NULL;
    if (!list) return NULL;
    if (!list->parts && create) list->parts = calloc(1, sizeof(AiPartsState));
    return list->parts;
}

static void entry_free_solid(AiParts *p) {
    free(p->solid_tris);
    p->solid_tris = NULL;
    p->solid_count = 0;
}

void ai_parts_reset(GameAIs *list) {
    AiPartsState *st = list ? list->parts : NULL;
    if (!st) return;
    for (int i = 0; i < st->entry_count; i++) {
        entry_free_solid(st->entries[i]);
        free(st->entries[i]);
    }
    st->entry_count = 0;
}

void ai_parts_free(GameAIs *list) {
    AiPartsState *st = list ? list->parts : NULL;
    if (!st) return;
    ai_parts_reset(list);
    for (int i = 0; i < st->model_count; i++) ai_parts_model_free(st->models[i].model);
    free(st->models);
    free(st->chunk);
    free(st);
    list->parts = NULL;
}

AiParts *ai_parts_of(Game *game, const GameAI *ai, const int create) {
    if (!game || !ai) return NULL;
    AiPartsState *st = state_of(game, create);
    if (!st) return NULL;
    for (int i = 0; i < st->entry_count; i++)
        if (st->entries[i]->sid == ai->sid && st->entries[i]->serial == ai->serial) return st->entries[i];
    if (!create) return NULL;
    // an AI that is gone (another serial, or no AI of that sid): its entry is recycled
    for (int i = 0; i < st->entry_count; i++) {
        const GameAI *other = game_ai_find(game, st->entries[i]->sid);
        if (!other || other->serial != st->entries[i]->serial) {
            AiParts *p = st->entries[i];
            entry_free_solid(p);
            memset(p, 0, sizeof(*p));
            p->sid = ai->sid;
            p->serial = ai->serial;
            p->body_dmg = 1.0f;
            return p;
        }
    }
    if (st->entry_count >= (int) (sizeof(st->entries) / sizeof(st->entries[0]))) return NULL;
    AiParts *p = calloc(1, sizeof(AiParts));
    if (!p) return NULL;
    p->sid = ai->sid;
    p->serial = ai->serial;
    p->body_dmg = 1.0f; // AI.init: bodyDmg = 1 (:204888)
    st->entries[st->entry_count++] = p;
    return p;
}

void ai_parts_model_put(Game *game, const int asset, AiPartsModel *model) {
    AiPartsState *st = state_of(game, 1);
    if (!st) {
        ai_parts_model_free(model);
        return;
    }
    for (int i = 0; i < st->model_count; i++)
        if (st->models[i].asset == asset) {
            ai_parts_model_free(st->models[i].model);
            st->models[i].model = model;
            st->models[i].state = model ? AI_PARTS_MODEL_READY : AI_PARTS_MODEL_FAILED;
            return;
        }
    if (st->model_count == st->model_cap) {
        const int n = st->model_cap ? st->model_cap * 2 : 8;
        ModelSlot *s = realloc(st->models, (size_t) n * sizeof(ModelSlot));
        if (!s) {
            ai_parts_model_free(model);
            return;
        }
        st->models = s;
        st->model_cap = n;
    }
    st->models[st->model_count++] = (ModelSlot) {asset, model ? AI_PARTS_MODEL_READY : AI_PARTS_MODEL_FAILED, model};
}

static int (*g_fetch)(const char *path);

void ai_parts_set_fetch_hook(int (*fetch)(const char *path)) { g_fetch = fetch; }

// loadModel (:206431): the model.glb of the asset; the fetch hook's 0 = still arriving (retried next call). Without a
// hook (server_bin, kn_brain, tests) the file is read as it is.
static void slot_try_load(ModelSlot *slot) {
    char *path = NULL;
    char rel[64];
    snprintf(rel, sizeof(rel), "user-assets/%d/model.glb", slot->asset);
    path = concat(assets_path(), rel);
    if (!path) return;
    const int ready = g_fetch ? g_fetch(path) : 1;
    if (ready < 0) slot->state = AI_PARTS_MODEL_FAILED;
    if (ready <= 0) {
        free(path);
        return;
    }
    unsigned char *buf = NULL;
    size_t len = 0;
    if (read_file(path, &buf, &len) || len > AI_MODEL_MAX_BYTES) {
        slot->state = AI_PARTS_MODEL_FAILED;
    } else {
        slot->model = ai_parts_model_parse(buf, len);
        slot->state = slot->model ? AI_PARTS_MODEL_READY : AI_PARTS_MODEL_FAILED;
    }
    free(buf);
    free(path);
}

AiPartsModel *ai_parts_model_get(Game *game, const int asset, const int load, int *state) {
    int dummy;
    if (!state) state = &dummy;
    *state = AI_PARTS_MODEL_FAILED;
    if (!game || asset <= 0) return NULL;
    AiPartsState *st = state_of(game, load);
    if (!st) return NULL;
    ModelSlot *slot = NULL;
    for (int i = 0; i < st->model_count && !slot; i++)
        if (st->models[i].asset == asset) slot = &st->models[i];
    if (!slot) {
        if (!load) return NULL;
        ai_parts_model_put(game, asset, NULL);
        for (int i = 0; i < st->model_count && !slot; i++)
            if (st->models[i].asset == asset) slot = &st->models[i];
        if (!slot) return NULL;
        slot->state = AI_PARTS_MODEL_LOADING;
    }
    if (slot->state == AI_PARTS_MODEL_LOADING) slot_try_load(slot);
    *state = slot->state;
    return slot->state == AI_PARTS_MODEL_READY ? slot->model : NULL;
}

// ---------------------------------------------------------------- per AI

static double now_ms(const Game *game) { return game ? (double) game->now * 1000.0 : 0.0; }
static double round_to(const double v, const int digits) { // Number.prototype.round (:27072): Math.round(v * 10^d) / 10^d
    const double p = pow(10.0, digits);
    return floor(v * p + 0.5) / p;
}
static double model_scale(const GameAI *ai) { // AI.modelScale (:205275): (dat.mSize || 4) * (dat.hbw || 1).round(1)
    const double m = ai->data.m_size ? ai->data.m_size : 4.0;
    return m * round_to(ai->data.hbw ? ai->data.hbw : 1.0, 1);
}
static double to_rad(const double deg) { return deg * M_PI / 180.0; } // iîíîïïi (:27207)

void ai_parts_spawned(Game *game, GameAI *ai) { // AI.init: idleT0 = now || 0, movT0 = 0 (:204903)
    if (!ai) return;
    ai->anim_idle_t0 = now_ms(game);
    ai->anim_mov_t0 = 0.0;
    // precise && !RENDER && assetID: loadModel(assetID) (:204908)
    int state;
    if (ai->data.precise && game_is_authority(game) && ai->data.asset) ai_parts_model_get(game, ai->data.asset, 1, &state);
}

void ai_parts_moved(Game *game, GameAI *ai) {
    if (ai) ai->anim_mov_t0 = now_ms(game); // didMov changed -> movT0 = now (:206063)
}

AiPartsModel *ai_parts_hit_model(Game *game, GameAI *ai) { // AI.hitMesh (:205199): !precise || RENDER || !assetID -> null
    if (!ai || !ai->data.precise || !game_is_authority(game) || !ai->data.asset) return NULL;
    int state;
    // hitMesh.groups && rig.nodes: empty arrays are truthy, so a model whose triangles the reader could not use (signed
    // positions, 5120 / 5122) is still "the" hit mesh and nothing hits that AI, as in 10.0.3. EXT_meshopt_compression is
    // not decoded by that reader either (the views are read raw from the BIN chunk), so neither is it here.
    return ai_parts_model_get(game, ai->data.asset, 1, &state);
}

// AI.clipAt (:205216)
static const Clip *clip_at(Game *game, GameAI *ai, const AiPartsModel *m, const double now, double *t) {
    *t = 0;
    if (!m || !m->clip_count) return NULL;
    const double anim_s = ai->data.anim_speed ? ai->data.anim_speed : 1.0;
    AiParts *p = ai_parts_of(game, ai, 0);
    const Clip *state_clip = p && p->clip_set ? clip_find(m, p->clip) : NULL;
    if (state_clip) {
        const double dur = state_clip->duration;
        const double el = (now - p->clip_t0) / 1000.0 * anim_s;
        if (el < dur * p->clip_reps) {
            *t = p->clip_reps > 1 ? fmod(el, dur) : el;
            return state_clip;
        }
        if (p->clip_hold) {
            *t = dur;
            return state_clip;
        }
        if (!p->clip_ended) {
            p->clip_ended = 1;
            ai->anim_idle_t0 = p->clip_t0 + dur * p->clip_reps / anim_s * 1000.0;
        }
    }
    const char *start = ai->data.start_anim[0] ? ai->data.start_anim : "Idle"; // startAnim || "Idle"
    const char *name = clip_find(m, start) ? start : "Idle";
    if (ai->did_move && clip_find(m, "Move")) name = "Move";
    const Clip *c = clip_find(m, name);
    if (!c) {
        if (state_clip) {
            *t = state_clip->duration;
            return state_clip;
        }
        return NULL;
    }
    const double dur = c->duration ? c->duration : 1.0;
    const double since = fmax(ai->anim_idle_t0, ai->anim_mov_t0);
    *t = fmod(fmod((now - since) / 1000.0 * anim_s, dur) + dur, dur);
    return c;
}

int ai_parts_clip_at(Game *game, GameAI *ai, const double now, char *clip, const size_t clip_size, double *t) {
    double tt = 0;
    int state;
    const AiPartsModel *m = ai && ai->data.asset ? ai_parts_model_get(game, ai->data.asset, 0, &state) : NULL;
    const Clip *c = m ? clip_at(game, ai, m, now, &tt) : NULL;
    if (t) *t = tt;
    if (clip && clip_size) snprintf(clip, clip_size, "%s", c ? c->name : "");
    return c != NULL;
}

// the hidden-node filter of hitTest / applySolid: the group's node (byName) or an ancestor is hidden
static int group_hidden(const AiPartsModel *m, const void *user, const char *name) {
    const AiParts *p = user;
    if (!p || !p->hidden_count) return 0;
    int guard = 0;
    for (int i = ai_parts_model_node_find(m, name); i >= 0 && guard++ <= m->node_count; i = m->nodes[i].parent)
        for (int h = 0; h < p->hidden_count; h++)
            if (!strcmp(p->hidden[h], m->nodes[i].name)) return 1;
    return 0;
}

// AI.partMult (:205189): the nearest part (or ancestor) with a setPartDamage, else bodyDmg
static float part_mult(const AiPartsModel *m, const AiParts *p, const char *name) {
    int guard = 0;
    for (int i = ai_parts_model_node_find(m, name); p && i >= 0 && guard++ <= m->node_count; i = m->nodes[i].parent)
        for (int d = 0; d < p->dmg_count; d++)
            if (!strcmp(p->dmg[d].name, m->nodes[i].name)) return p->dmg[d].mult;
    return p ? p->body_dmg : 1.0f;
}

int ai_parts_model_ray(const AiPartsModel *m, const double origin[3], const double dir[3], double *t_out) { // iïïìîiì at rest
    if (!m) return 0;
    double *worlds = pose_alloc(m, NULL, 0.0); // iíîìíïî(rig, null, 0)
    if (!worlds) return 0;
    double best = 1;
    // loadModel's mesh: iîìïiìî(glb, 6000, true) caps the hit mesh at 6000 too (deob-10.0.3.js:212173, :204462, :204470)
    const int g = mesh_hit(m, worlds, origin, dir, NULL, NULL, AI_REST_MAX_TRIS, &best);
    free(worlds);
    if (g < 0) return 0;
    if (t_out) *t_out = best;
    return 1;
}

int ai_parts_hit_test(Game *game, GameAI *ai, const vec3 origin, const vec3 delta, float *t_out, char *part, const size_t part_size, float *mult) {
    AiPartsModel *m = ai_parts_hit_model(game, ai);
    if (!m) return 0;
    // the ray in the AI's model space (:205281-205297): yaw xD, then mROff, scale, mYOff
    const double yaw = ai->x_dir, c = cos(yaw), s = sin(yaw);
    const double rx = origin.x - ai->position.x, ry = origin.y - ai->position.y, rz = origin.z - ai->position.z;
    const double lx = rx * c - rz * s, lz = rx * s + rz * c;
    const double ldx = delta.x * c - delta.z * s, ldz = delta.x * s + delta.z * c;
    const double scale = model_scale(ai);
    const double r = to_rad(ai->data.m_r_off), co = cos(r), si = sin(r);
    const double o[3] = {(lx * co - lz * si) / scale, (ry - ai->data.m_y_off) / scale, (lx * si + lz * co) / scale};
    const double d[3] = {(ldx * co - ldz * si) / scale, delta.y / scale, (ldx * si + ldz * co) / scale};
    double t = 0;
    const Clip *clip = clip_at(game, ai, m, now_ms(game), &t);
    double *worlds = pose_alloc(m, clip ? clip->name : NULL, t);
    if (!worlds) return 0;
    AiParts *p = ai_parts_of(game, ai, 0);
    double best = 1;
    const int g = mesh_hit(m, worlds, o, d, group_hidden, p, AI_HIT_MAX_TRIS, &best);
    free(worlds);
    if (g < 0) return 0;
    if (t_out) *t_out = (float) best;
    if (part && part_size) snprintf(part, part_size, "%s", m->groups[g].name);
    if (mult) *mult = part_mult(m, p, m->groups[g].name);
    return 1;
}

int ai_parts_part_position(Game *game, GameAI *ai, const char *name, vec3 *out) { // AI.partPosition (:205324)
    int state;
    AiPartsModel *m = ai && ai->data.asset ? ai_parts_model_get(game, ai->data.asset, 0, &state) : NULL;
    const int node = ai_parts_model_node_find(m, name);
    if (node < 0) return 0;
    double t = 0;
    const Clip *clip = clip_at(game, ai, m, now_ms(game), &t);
    int chain[256], depth = 0; // the node and its ancestors (unshifted: root first)
    for (int i = node; i >= 0 && depth < 256; i = m->nodes[i].parent) chain[depth++] = i;
    double w[16], local[16];
    m4_compose((const double[3]) {0, 0, 0}, (const double[4]) {0, 0, 0, 1}, (const double[3]) {1, 1, 1}, w);
    for (int k = depth - 1; k >= 0; k--) {
        node_local(m, clip, chain[k], t, local);
        m4_mul(w, local, w);
    }
    double pt[3];
    m4_point(w, m->nodes[node].center[0], m->nodes[node].center[1], m->nodes[node].center[2], pt);
    const double sc = model_scale(ai), rr = to_rad(ai->data.m_r_off);
    const double X = pt[0] * sc, Z = pt[2] * sc;
    const double ox = X * cos(rr) + Z * sin(rr), oz = -X * sin(rr) + Z * cos(rr);
    const double oy = pt[1] * sc + ai->data.m_y_off;
    const double c = cos(ai->x_dir), s = sin(ai->x_dir);
    if (out) *out = (vec3) {(float) (ai->position.x + ox * c + oz * s), (float) (ai->position.y + oy), (float) (ai->position.z - ox * s + oz * c)};
    return 1;
}

void ai_parts_set_part_damage(Game *game, GameAI *ai, const char *part, const float mult) { // AI.setPartDamage (:205154)
    AiParts *p = ai_parts_of(game, ai, 1);
    if (!p || !part || !part[0]) return;
    for (int i = 0; i < p->dmg_count; i++)
        if (!strcmp(p->dmg[i].name, part)) {
            p->dmg[i].mult = mult;
            return;
        }
    if (p->dmg_count >= AI_PARTS_MAX) return; // KN cap (the JS object grows)
    snprintf(p->dmg[p->dmg_count].name, sizeof(p->dmg[0].name), "%s", part);
    p->dmg[p->dmg_count++].mult = mult;
}

void ai_parts_set_body_damage(Game *game, GameAI *ai, const float mult) {
    AiParts *p = ai_parts_of(game, ai, 1);
    if (p) p->body_dmg = mult;
}

float ai_parts_body_damage(Game *game, const GameAI *ai) {
    const AiParts *p = ai_parts_of(game, ai, 0);
    return p ? p->body_dmg : 1.0f;
}

void ai_parts_glow_part(Game *game, GameAI *ai, const char *part, const char *color, const double intensity) { // AI.glowPart (:205157)
    AiParts *p = ai_parts_of(game, ai, 1);
    if (!p || !part) return;
    if (!color) color = "";
    int at = -1;
    for (int i = 0; i < p->glow_count && at < 0; i++)
        if (!strcmp(p->glow[i].name, part)) at = i;
    if (intensity > 0) {
        if (at >= 0 && !strcmp(p->glow[at].color, color) && p->glow[at].intensity == intensity) return;
        if (at < 0 && p->glow_count >= AI_PARTS_MAX) return;
        if (at < 0) at = p->glow_count++;
        snprintf(p->glow[at].name, sizeof(p->glow[0].name), "%s", part);
        snprintf(p->glow[at].color, sizeof(p->glow[0].color), "%s", color);
        p->glow[at].intensity = intensity;
    } else {
        if (at < 0) return;
        memmove(&p->glow[at], &p->glow[at + 1], (size_t) (p->glow_count - at - 1) * sizeof(p->glow[0])); // delete partGlow[part]
        p->glow_count--;
    }
    p->dirty = 1;
}

void ai_parts_node_visible(Game *game, GameAI *ai, const char *node, const int visible) { // AI.setNodeVisible (:205175)
    AiParts *p = ai_parts_of(game, ai, 1);
    if (!p || !node) return;
    int at = -1;
    for (int i = 0; i < p->hidden_count && at < 0; i++)
        if (!strcmp(p->hidden[i], node)) at = i;
    if (visible && at >= 0) {
        memmove(&p->hidden[at], &p->hidden[at + 1], (size_t) (p->hidden_count - at - 1) * sizeof(p->hidden[0]));
        p->hidden_count--;
    } else {
        if (visible || at >= 0 || p->hidden_count >= AI_PARTS_MAX) return;
        snprintf(p->hidden[p->hidden_count++], sizeof(p->hidden[0]), "%s", node);
    }
    p->dirty = 1; // poseCache = null; partsDirty = true
}

void ai_parts_set_dmg_part(Game *game, GameAI *ai, const char *part) {
    AiParts *p = ai_parts_of(game, ai, part != NULL);
    if (!p) return;
    p->dmg_part_set = part != NULL;
    snprintf(p->dmg_part, sizeof(p->dmg_part), "%s", part ? part : "");
}

int ai_parts_take_dmg_part(Game *game, GameAI *ai, char *out, const size_t size) { // changeHealth: dmgPart, then null (:206231)
    AiParts *p = ai_parts_of(game, ai, 0);
    const int set = p && p->dmg_part_set;
    if (out && size) snprintf(out, size, "%s", set ? p->dmg_part : "");
    if (p) p->dmg_part_set = 0;
    return set;
}

// ---- ai.solid (:205368-205447)

int ai_parts_solid(Game *game, const GameAI *ai) {
    const AiParts *p = ai_parts_of(game, ai, 0);
    return p && p->solid;
}

static double tri_area(const float *v) { // THREE.Triangle.getArea: |(c - b) x (a - b)| / 2
    const double cx = (double) v[6] - v[3], cy = (double) v[7] - v[4], cz = (double) v[8] - v[5];
    const double ax = (double) v[0] - v[3], ay = (double) v[1] - v[4], az = (double) v[2] - v[5];
    const double x = cy * az - cz * ay, y = cz * ax - cx * az, z = cx * ay - cy * ax;
    return sqrt(x * x + y * y + z * z) * 0.5;
}

typedef struct {
    float v[9];
    double area;
    int order;
} SolidTri;

static int solid_cmp(const void *pa, const void *pb) { // sort by area, largest first (stable)
    const SolidTri *a = pa, *b = pb;
    return a->area > b->area ? -1 : a->area < b->area ? 1 : a->order - b->order;
}

void ai_parts_apply_solid(Game *game, GameAI *ai) {
    AiParts *p = ai_parts_of(game, ai, 0);
    if (!p || !game || !game->map) return; // the map manager holds the tris
    entry_free_solid(p);
    p->solid_wait = 0;
    if (!p->solid || !ai->data.asset) return;
    int state;
    AiPartsModel *m = ai_parts_model_get(game, ai->data.asset, 1, &state);
    if (!m) {
        p->solid_wait = state == AI_PARTS_MODEL_LOADING; // (!rig || !rig.failed) && (!hm || !hm.failed)
        return;
    }
    const AiSolidPose *sp = &p->solid_pose;
    double *worlds = pose_alloc(m, sp->clip[0] ? sp->clip : NULL, sp->t);
    if (!worlds) return;
    const double rr = to_rad(sp->r), cr = cos(rr), sr = sin(rr), cd = cos(sp->d), sd = sin(sp->d);
    size_t count = 0, cap = 0;
    SolidTri *tris = NULL;
    for (int gi = 0; gi < m->group_count; gi++) {
        const Group *g = &m->groups[gi];
        if (group_hidden(m, p, g->name)) continue;
        const double *w = &worlds[g->node * 16];
        for (int e = 0; e + 8 < g->tri_count * 9; e += 9) {
            SolidTri t;
            for (int k = 0; k < 3; k++) { // iìIîîiî (:205411)
                double q[3];
                m4_point(w, g->tris[e + k * 3], g->tris[e + k * 3 + 1], g->tris[e + k * 3 + 2], q);
                const double x = q[0] * sp->s, y = q[1] * sp->s, z = q[2] * sp->s;
                const double ox = x * cr + z * sr, oz = -x * sr + z * cr;
                t.v[k * 3] = (float) (sp->x + ox * cd + oz * sd);
                t.v[k * 3 + 1] = (float) (sp->y + y + sp->yo);
                t.v[k * 3 + 2] = (float) (sp->z - ox * sd + oz * cd);
            }
            t.area = tri_area(t.v);
            if (!(t.area > AI_SOLID_MIN_AREA)) continue;
            if (count == cap) {
                cap = cap ? cap * 2 : 256;
                SolidTri *n = realloc(tris, cap * sizeof(SolidTri));
                if (!n) break;
                tris = n;
            }
            t.order = (int) count;
            tris[count++] = t;
        }
    }
    free(worlds);
    if (count > AI_SOLID_MAX_TRIS) {
        qsort(tris, count, sizeof(SolidTri), solid_cmp);
        count = AI_SOLID_MAX_TRIS;
    }
    p->solid_tris = count ? malloc(count * 9 * sizeof(float)) : NULL;
    for (size_t i = 0; p->solid_tris && i < count; i++) memcpy(&p->solid_tris[i * 9], tris[i].v, sizeof(tris[i].v));
    p->solid_count = p->solid_tris ? count : 0;
    free(tris);
}

void ai_parts_set_solid(Game *game, GameAI *ai, const int solid) { // AI.setSolid (:205368)
    AiParts *p = ai_parts_of(game, ai, solid);
    if (!p || !!solid == !!p->solid) return;
    memset(&p->solid_pose, 0, sizeof(p->solid_pose));
    if (solid) {
        double t = 0;
        char clip[GAME_AI_ANIM_SIZE];
        const int has_clip = ai_parts_clip_at(game, ai, now_ms(game), clip, sizeof(clip), &t);
        snprintf(p->solid_pose.clip, sizeof(p->solid_pose.clip), "%s", has_clip ? clip : "");
        p->solid_pose.t = has_clip ? t : 0;
        p->solid_pose.x = ai->position.x;
        p->solid_pose.y = ai->position.y;
        p->solid_pose.z = ai->position.z;
        p->solid_pose.d = ai->x_dir;
        p->solid_pose.s = (float) model_scale(ai);
        p->solid_pose.r = ai->data.m_r_off;
        p->solid_pose.yo = ai->data.m_y_off;
    }
    p->solid = (unsigned char) !!solid;
    p->dirty = 1;
    ai_parts_apply_solid(game, ai);
}

void ai_parts_drop_solid(Game *game, GameAI *ai) {
    AiParts *p = ai_parts_of(game, ai, 0);
    if (!p || (!p->solid && !p->solid_count)) return;
    p->solid = 0;
    memset(&p->solid_pose, 0, sizeof(p->solid_pose));
    entry_free_solid(p);
    p->solid_wait = 0;
}

void ai_parts_tick(Game *game) { // the AI manager update (:206420): solidWait -> applySolid
    AiPartsState *st = state_of(game, 0);
    for (int i = 0; st && i < st->entry_count; i++) {
        if (!st->entries[i]->solid_wait) continue;
        GameAI *ai = game_ai_find(game, st->entries[i]->sid);
        if (ai && ai->serial == st->entries[i]->serial) ai_parts_apply_solid(game, ai);
    }
}

const float *ai_parts_solid_tris(Game *game, const int i, size_t *tri_count) {
    AiPartsState *st = state_of(game, 0);
    if (tri_count) *tri_count = 0;
    if (!st || i < 0 || i >= st->entry_count) return NULL;
    static const float none[9];
    const AiParts *p = st->entries[i];
    const GameAI *ai = game_ai_find(game, p->sid);
    if (!ai || ai->serial != p->serial || !p->solid_tris) return none; // gone without remove(): nothing collides
    if (tri_count) *tri_count = p->solid_count;
    return p->solid_tris;
}

void ai_parts_play_anim(Game *game, GameAI *ai, const char *clip, const double reps, const int hold) { // AI.playAnim (:206455-206465)
    if (!ai || !clip || !clip[0]) return;
    AiParts *p = ai_parts_of(game, ai, 1);
    if (!p) return;
    p->clip_set = 1;
    snprintf(p->clip, sizeof(p->clip), "%s", clip);
    p->clip_t0 = now_ms(game);
    p->clip_reps = fmax(1.0, reps ? reps : 1.0);
    p->clip_hold = (unsigned char) !!hold;
    p->clip_ended = 0;
    const char *held = hold ? clip : "";
    if (strcmp(held, p->held_clip)) {
        snprintf(p->held_clip, sizeof(p->held_clip), "%s", held);
        p->dirty = 1;
    }
}

// ---- "aip"

int ai_parts_has_state(Game *game, const GameAI *ai) {
    const AiParts *p = ai_parts_of(game, ai, 0);
    return p && (p->glow_count || p->hidden_count || p->held_clip[0] || p->solid);
}

char *ai_parts_record_json(Game *game, const GameAI *ai) {
    const AiParts *p = ai_parts_of(game, ai, 0);
    cJSON *rec = cJSON_CreateArray();
    if (!rec || !ai) {
        cJSON_Delete(rec);
        return NULL;
    }
    cJSON_AddItemToArray(rec, cJSON_CreateNumber(ai->sid));
    cJSON *glow = cJSON_CreateObject();
    for (int i = 0; p && glow && i < p->glow_count; i++) {
        cJSON *pair = cJSON_CreateArray();
        cJSON_AddItemToArray(pair, cJSON_CreateString(p->glow[i].color));
        cJSON_AddItemToArray(pair, cJSON_CreateNumber(p->glow[i].intensity));
        cJSON_AddItemToObject(glow, p->glow[i].name, pair);
    }
    cJSON_AddItemToArray(rec, glow);
    cJSON *hidden = cJSON_CreateArray();
    for (int i = 0; p && hidden && i < p->hidden_count; i++) cJSON_AddItemToArray(hidden, cJSON_CreateString(p->hidden[i]));
    cJSON_AddItemToArray(rec, hidden);
    cJSON_AddItemToArray(rec, p && p->held_clip[0] ? cJSON_CreateString(p->held_clip) : cJSON_CreateNull());
    if (p && p->solid) {
        const AiSolidPose *sp = &p->solid_pose;
        cJSON *pose = cJSON_CreateObject();
        if (sp->clip[0]) cJSON_AddStringToObject(pose, "n", sp->clip);
        else cJSON_AddNullToObject(pose, "n");
        cJSON_AddNumberToObject(pose, "t", sp->t);
        cJSON_AddNumberToObject(pose, "x", sp->x);
        cJSON_AddNumberToObject(pose, "y", sp->y);
        cJSON_AddNumberToObject(pose, "z", sp->z);
        cJSON_AddNumberToObject(pose, "d", sp->d);
        cJSON_AddNumberToObject(pose, "s", sp->s);
        cJSON_AddNumberToObject(pose, "r", sp->r);
        cJSON_AddNumberToObject(pose, "yo", sp->yo);
        cJSON_AddItemToArray(rec, pose);
    } else {
        cJSON_AddItemToArray(rec, cJSON_CreateNull());
    }
    char *out = cJSON_PrintUnformatted(rec);
    cJSON_Delete(rec);
    return out;
}

static int pose_equal(const AiSolidPose *a, const AiSolidPose *b) {
    return !strcmp(a->clip, b->clip) && a->t == b->t && a->x == b->x && a->y == b->y && a->z == b->z && a->d == b->d &&
           a->s == b->s && a->r == b->r && a->yo == b->yo;
}

void ai_parts_apply_json(Game *game, const char *json, const size_t length) { // AI.syncParts (:206506)
    cJSON *rec = json ? cJSON_ParseWithLength(json, length) : NULL;
    if (!cJSON_IsArray(rec)) {
        cJSON_Delete(rec);
        return;
    }
    GameAI *ai = game_ai_find(game, (int) num(cJSON_GetArrayItem(rec, 0), -1));
    AiParts *p = ai ? ai_parts_of(game, ai, 1) : NULL;
    if (!p) {
        cJSON_Delete(rec);
        return;
    }
    const cJSON *glow = cJSON_GetArrayItem(rec, 1), *hidden = cJSON_GetArrayItem(rec, 2);
    const cJSON *held = cJSON_GetArrayItem(rec, 3), *pose = cJSON_GetArrayItem(rec, 4);
    p->glow_count = 0; // partGlow = object ? it : {}
    const cJSON *g;
    if (cJSON_IsObject(glow)) cJSON_ArrayForEach(g, glow) {
        if (p->glow_count >= AI_PARTS_MAX || !g->string) break;
        AiPartGlow *pg = &p->glow[p->glow_count++];
        snprintf(pg->name, sizeof(pg->name), "%s", g->string);
        const char *color = cJSON_GetStringValue(cJSON_GetArrayItem(g, 0));
        snprintf(pg->color, sizeof(pg->color), "%s", color ? color : "");
        pg->intensity = num(cJSON_GetArrayItem(g, 1), 0);
    }
    p->hidden_count = 0; // hiddenNodes = isArray ? slice(0, 32) : []
    const cJSON *h;
    if (cJSON_IsArray(hidden)) cJSON_ArrayForEach(h, hidden) {
        if (p->hidden_count >= AI_PARTS_MAX) break;
        const char *s = cJSON_GetStringValue(h);
        if (s) snprintf(p->hidden[p->hidden_count++], sizeof(p->hidden[0]), "%s", s);
    }
    const char *held_name = cJSON_GetStringValue(held); // heldClip = string ? it : null; none -> heldShown = null
    snprintf(p->held_clip, sizeof(p->held_clip), "%s", held_name ? held_name : "");
    if (!p->held_clip[0]) p->held_shown[0] = 0;
    AiSolidPose next = {0};
    const int solid = cJSON_IsObject(pose);
    if (solid) {
        const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(pose, "n"));
        snprintf(next.clip, sizeof(next.clip), "%s", n ? n : "");
        next.t = num(cJSON_GetObjectItem(pose, "t"), 0);
        next.x = (float) num(cJSON_GetObjectItem(pose, "x"), 0);
        next.y = (float) num(cJSON_GetObjectItem(pose, "y"), 0);
        next.z = (float) num(cJSON_GetObjectItem(pose, "z"), 0);
        next.d = (float) num(cJSON_GetObjectItem(pose, "d"), 0);
        next.s = (float) num(cJSON_GetObjectItem(pose, "s"), 0);
        next.r = (float) num(cJSON_GetObjectItem(pose, "r"), 0);
        next.yo = (float) num(cJSON_GetObjectItem(pose, "yo"), 0);
    }
    if (solid != p->solid || (solid && !pose_equal(&next, &p->solid_pose))) { // JSON.stringify differs
        p->solid = (unsigned char) solid;
        p->solid_pose = next;
        ai_parts_apply_solid(game, ai);
    }
    cJSON_Delete(rec);
}

void ai_parts_send(Game *game, const GameAI *ai, const int uid) {
    char *json = ai_parts_record_json(game, ai);
    if (!json) return;
    const size_t len = strlen(json);
    const size_t slice = (size_t) NET_EXT_JSON_CHUNK_DATA;
    const size_t parts = len ? (len + slice - 1) / slice : 1;
    if (parts > 255) { // a record cannot be this long (32 glows + 32 names); never send half of one
        free(json);
        return;
    }
    for (size_t i = 0; i < parts; i++) {
        uint8_t body[NET_EXT_BODY_MAX];
        const size_t n = i * slice + slice <= len ? slice : len - i * slice;
        body[0] = (uint8_t) i;
        body[1] = (uint8_t) parts;
        memcpy(body + sizeof(NetExtJsonChunk), json + i * slice, n);
        uint8_t buf[NET_EXT_HEADER_SIZE + NET_EXT_BODY_MAX];
        const uint16_t out = net_ext_pack(buf, sizeof(buf), AI_PARTS_EXT_SUBTYPE, body, (uint16_t) (sizeof(NetExtJsonChunk) + n));
        if (out) game_send(game, uid, NET_PACKET_EXT, buf, out);
    }
    free(json);
}

void ai_parts_on_refl(Game *game, const uint8_t *body, const uint16_t length) {
    NetAIReflPacket p;
    if (!body || length < sizeof(p)) return;
    memcpy(&p, body, sizeof(p));
    GameAI *ai = game_ai_find(game, p.sid);
    if (ai && isfinite(p.refl)) ai->data.refl = p.refl < 0.0f ? 0.0f : p.refl > 4.0f ? 4.0f : p.refl;
}

void ai_parts_on_ext(Game *game, const uint8_t *body, const uint16_t length) {
    if (!body || length < sizeof(NetExtJsonChunk)) return;
    AiPartsState *st = state_of(game, 1);
    if (!st) return;
    const int part = body[0], parts = body[1];
    if (part == 0) { // a new record (a broken one is dropped)
        st->chunk_len = 0;
        st->chunk_next = 0;
        st->chunk_parts = parts;
    }
    if (!parts || parts != st->chunk_parts || part != st->chunk_next) {
        st->chunk_next = -1;
        return;
    }
    const size_t n = length - sizeof(NetExtJsonChunk);
    char *grown = realloc(st->chunk, st->chunk_len + n + 1);
    if (!grown) return;
    st->chunk = grown;
    memcpy(st->chunk + st->chunk_len, body + sizeof(NetExtJsonChunk), n);
    st->chunk_len += n;
    st->chunk[st->chunk_len] = 0;
    st->chunk_next++;
    if (st->chunk_next == parts) {
        ai_parts_apply_json(game, st->chunk, st->chunk_len);
        st->chunk_next = -1;
    }
}
