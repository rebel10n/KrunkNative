// Ray tracing: the CPU acceleration structure (client/include/rt.h, doc/raytracing-plan.md).
//
// One BLAS (binned-SAH BVH over local-space triangles) per triangle Geometry, cached by pointer and
// refit in place when its positions move (Geometry.tri_epoch); one instance per visible world mesh
// (the shared unit cube / plane / ramp are analytic kinds, no triangles); a TLAS over the instances'
// world boxes in two halves (rt.h RtScene: the settled map objects, built once; everything else,
// rebuilt every frame) under one root. The reference tracer
// here walks exactly the data rt_gpu.c uploads, with the traversal rt_trace.frag will run, so a
// mismatch shows up on the CPU first (`client_bin --rt-selftest`, rt_selftest.c).
//
// Index conventions: a BLAS's node links and leaf primitive indices are local to that BLAS (node 0
// its root, triangle 0 its first, i.e. RtScene.tris[(tri_first + i) * 9]); rt_gpu.c adds the node
// base and tri_first when it packs every BLAS into the one node table. The TLAS's leaf primitives
// are global instance indices.
#include <rt.h>
#include <shadow.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RT_LEAF_MAX_TRIS 4
#define RT_LEAF_MAX_INSTANCES 2
#define RT_MAX_DEPTH 32
#define RT_SAH_BINS 12
#define RT_STACK 64
#define RT_EPS 1e-7f
#define RT_PLANE_PAD 1e-4f // the unit plane's y extent in its AABB

// ---------------------------------------------------------------- small vector / matrix helpers

static float v3_dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void v3_cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
static void v3_sub(const float a[3], const float b[3], float out[3]) {
    for (int i = 0; i < 3; i++) out[i] = a[i] - b[i];
}
static void v3_normalize(float v[3]) {
    const float l = sqrtf(v3_dot(v, v));
    if (l > 0.0f) for (int i = 0; i < 3; i++) v[i] /= l;
}
static float f_min(const float a, const float b) { return a < b ? a : b; }
static float f_max(const float a, const float b) { return a > b ? a : b; }

// 3x4 row-major affine: p' = M[0..2] . (p, 1)
static void mat34_point(const float m[12], const float p[3], float out[3]) {
    for (int r = 0; r < 3; r++) out[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
}
static void mat34_vector(const float m[12], const float v[3], float out[3]) {
    for (int r = 0; r < 3; r++) out[r] = m[r * 4] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2];
}
// normal: n' = (M^-1)^T n, i.e. with w2l = M^-1: n'[i] = sum_j w2l[j][i] n[j]
static void mat34_normal(const float w2l[12], const float n[3], float out[3]) {
    for (int i = 0; i < 3; i++) out[i] = w2l[i] * n[0] + w2l[4 + i] * n[1] + w2l[8 + i] * n[2];
}
static float mat34_det3(const float m[12]) {
    return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
}
// inverse of the affine 3x4 (0 when singular: a zero-scaled object)
static int mat34_invert(const float m[12], float out[12]) {
    const float det = mat34_det3(m);
    if (fabsf(det) < 1e-20f) return 0;
    const float inv = 1.0f / det;
    out[0] = (m[5] * m[10] - m[6] * m[9]) * inv;
    out[1] = (m[2] * m[9] - m[1] * m[10]) * inv;
    out[2] = (m[1] * m[6] - m[2] * m[5]) * inv;
    out[4] = (m[6] * m[8] - m[4] * m[10]) * inv;
    out[5] = (m[0] * m[10] - m[2] * m[8]) * inv;
    out[6] = (m[2] * m[4] - m[0] * m[6]) * inv;
    out[8] = (m[4] * m[9] - m[5] * m[8]) * inv;
    out[9] = (m[1] * m[8] - m[0] * m[9]) * inv;
    out[10] = (m[0] * m[5] - m[1] * m[4]) * inv;
    const float t[3] = {m[3], m[7], m[11]};
    for (int r = 0; r < 3; r++) out[r * 4 + 3] = -(out[r * 4] * t[0] + out[r * 4 + 1] * t[1] + out[r * 4 + 2] * t[2]);
    return 1;
}

static void aabb_reset(float min[3], float max[3]) {
    for (int i = 0; i < 3; i++) { min[i] = FLT_MAX; max[i] = -FLT_MAX; }
}
static void aabb_add_point(float min[3], float max[3], const float p[3]) {
    for (int i = 0; i < 3; i++) { min[i] = f_min(min[i], p[i]); max[i] = f_max(max[i], p[i]); }
}
static void aabb_add_box(float min[3], float max[3], const float bmin[3], const float bmax[3]) {
    aabb_add_point(min, max, bmin);
    aabb_add_point(min, max, bmax);
}
static float aabb_area(const float min[3], const float max[3]) {
    const float d[3] = {f_max(max[0] - min[0], 0.0f), f_max(max[1] - min[1], 0.0f), f_max(max[2] - min[2], 0.0f)};
    return 2.0f * (d[0] * d[1] + d[1] * d[2] + d[2] * d[0]);
}

// ---------------------------------------------------------------- pointer -> int map (open addressing)

typedef struct { const void *key; int value; } PtrEntry;
typedef struct { PtrEntry *slots; size_t cap, count; } PtrMap;

static size_t ptr_hash(const void *p) {
    uint64_t x = (uint64_t) (uintptr_t) p;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
    return (size_t) x;
}
static int ptr_map_get(const PtrMap *map, const void *key, int *value) {
    if (!map->cap) return 0;
    for (size_t i = ptr_hash(key) & (map->cap - 1);; i = (i + 1) & (map->cap - 1)) {
        const PtrEntry *e = &map->slots[i];
        if (!e->key) return 0;
        if (e->key == key) { if (value) *value = e->value; return 1; }
    }
}
static void ptr_map_set(PtrMap *map, const void *key, int value);
static void ptr_map_grow(PtrMap *map) {
    const PtrMap old = *map;
    map->cap = old.cap ? old.cap * 2 : 256;
    map->count = 0;
    map->slots = calloc(map->cap, sizeof(PtrEntry));
    if (!map->slots) { *map = old; return; }
    for (size_t i = 0; i < old.cap; i++) if (old.slots[i].key) ptr_map_set(map, old.slots[i].key, old.slots[i].value);
    free(old.slots);
}
static void ptr_map_set(PtrMap *map, const void *key, const int value) {
    if ((map->count + 1) * 2 > map->cap) ptr_map_grow(map);
    if (!map->cap) return;
    for (size_t i = ptr_hash(key) & (map->cap - 1);; i = (i + 1) & (map->cap - 1)) {
        PtrEntry *e = &map->slots[i];
        if (!e->key) { e->key = key; e->value = value; map->count++; return; }
        if (e->key == key) { e->value = value; return; }
    }
}
static void ptr_map_clear(PtrMap *map) {
    if (map->slots) memset(map->slots, 0, map->cap * sizeof(PtrEntry));
    map->count = 0;
}
static void ptr_map_free(PtrMap *map) {
    free(map->slots);
    *map = (PtrMap) {0};
}

// ---------------------------------------------------------------- private state
// Attached to the one RtScene the renderer keeps (rt_pass.c); another owner resets it.

// What the builder remembers about a mesh between frames (static / dynamic split, rt.h RtScene).
typedef struct {
    const Mesh *mesh;
    float matrix[16];     // the transform last seen
    float prev_matrix[16]; // the transform of the build before that (motion vectors, RtInstance.prev_local_to_world)
    int seen_frame;       // last frame the mesh was visible and traceable
    int moved_frame;      // last frame the transform differed from the frame before
    int map_casts;        // shadow_mesh_casts(): 1 / 0 a map object, -1 not from the map (always dynamic)
    int static_index;     // its slot in the static half, -1 none, -2 the last static build could not trace it
} MeshState;

typedef struct {
    RtNode *nodes;
    int count, cap;
} NodeList;

typedef struct {
    const RtScene *owner;
    PtrMap blas_by_geometry;   // Geometry* -> index into rt->blas (-1: not traceable, remembered)
    PtrMap state_by_mesh;      // Mesh* -> index into states
    MeshState *states;
    int state_count, state_cap;
    int frame;                 // rt_scene_build calls
    int static_built;          // the static half exists (else the first build makes it)
    const Mesh **stat, **dyn;  // this frame's static candidates / dynamic meshes, scene order
    int stat_count, stat_cap, dyn_count, dyn_cap;
    const Mesh **self_list;    // this frame's shadow proxy meshes (rt_self.c), RT_INST_SHADOW_ONLY in the dynamic half
    int self_count, self_cap;
    NodeList seg;              // scratch for one half's BVH
    int skipped_logged;
    // material dedupe: a hash of the RtMaterial bytes -> chain of material indices (mat_next per material)
    int mat_bucket[4096];
    int *mat_next;
    int mat_next_cap;
    // rt_scene_build's per-position cache of the state lookup: the scene keeps its mesh order from frame to frame,
    // so slot_mesh[i] == mesh at position i is a pointer compare instead of a hash lookup (slot_state[i] its state)
    const Mesh **slot_mesh;
    int *slot_state;
    int slot_cap;
} RtPrivate;

static RtPrivate g_priv;

static void priv_materials_clear(RtPrivate *priv) {
    for (int i = 0; i < 4096; i++) priv->mat_bucket[i] = -1;
}

static void priv_slots_clear(RtPrivate *priv) { // state indices are about to change: the slots forget them
    if (priv->slot_mesh) memset(priv->slot_mesh, 0, (size_t) priv->slot_cap * sizeof(*priv->slot_mesh));
}

static void priv_states_clear(RtPrivate *priv) {
    priv_slots_clear(priv);
    ptr_map_clear(&priv->state_by_mesh);
    priv->state_count = 0;
    priv->static_built = 0;
}

static RtPrivate *priv_for(const RtScene *rt) {
    if (g_priv.owner != rt) {
        ptr_map_clear(&g_priv.blas_by_geometry);
        priv_states_clear(&g_priv);
        priv_materials_clear(&g_priv);
        g_priv.frame = 0;
        g_priv.owner = rt;
    }
    return &g_priv;
}

// ---------------------------------------------------------------- growable arrays

static void *grow(void *ptr, int *cap, const int wanted, const size_t size) {
    if (wanted <= *cap) return ptr;
    int cap2 = *cap ? *cap : 64;
    while (cap2 < wanted) cap2 *= 2;
    void *grown = realloc(ptr, (size_t) cap2 * size);
    if (!grown) return NULL;
    *cap = cap2;
    return grown;
}

// ---------------------------------------------------------------- BVH builder (binned SAH)

typedef struct {
    float min[3], max[3], centroid[3];
    int index; // the primitive's original index
} BuildPrim;

static int node_alloc(NodeList *list) {
    RtNode *nodes = grow(list->nodes, &list->cap, list->count + 1, sizeof(RtNode));
    if (!nodes) return -1;
    list->nodes = nodes;
    memset(&nodes[list->count], 0, sizeof(RtNode));
    return list->count++;
}

// Partitions prims[first, first + count) into two halves and returns the split position (first + left
// count); 0 = make a leaf. SAH over RT_SAH_BINS centroid bins on the widest centroid axis; a range the
// SAH will not split (equal centroids, or a leaf is cheaper) above leaf_max falls back to a median split
// so the depth stays bounded.
static int build_split(BuildPrim *prims, const int first, const int count, const int leaf_max, const float node_min[3], const float node_max[3]) {
    float cmin[3], cmax[3];
    aabb_reset(cmin, cmax);
    for (int i = first; i < first + count; i++) aabb_add_point(cmin, cmax, prims[i].centroid);
    int axis = 0;
    for (int a = 1; a < 3; a++) if (cmax[a] - cmin[a] > cmax[axis] - cmin[axis]) axis = a;
    const float extent = cmax[axis] - cmin[axis];

    int best_bin = -1;
    if (extent > 0.0f) {
        float bin_min[RT_SAH_BINS][3], bin_max[RT_SAH_BINS][3];
        int bin_count[RT_SAH_BINS] = {0};
        for (int b = 0; b < RT_SAH_BINS; b++) aabb_reset(bin_min[b], bin_max[b]);
        const float scale = (float) RT_SAH_BINS / extent;
        for (int i = first; i < first + count; i++) {
            int b = (int) ((prims[i].centroid[axis] - cmin[axis]) * scale);
            if (b >= RT_SAH_BINS) b = RT_SAH_BINS - 1;
            if (b < 0) b = 0;
            bin_count[b]++;
            aabb_add_box(bin_min[b], bin_max[b], prims[i].min, prims[i].max);
        }
        // sweep from the right for the right-side areas, then from the left for the costs
        float right_area[RT_SAH_BINS];
        int right_count[RT_SAH_BINS];
        float rmin[3], rmax[3];
        aabb_reset(rmin, rmax);
        int rc = 0;
        for (int b = RT_SAH_BINS - 1; b > 0; b--) {
            if (bin_count[b]) aabb_add_box(rmin, rmax, bin_min[b], bin_max[b]);
            rc += bin_count[b];
            right_area[b] = rc ? aabb_area(rmin, rmax) : 0.0f;
            right_count[b] = rc;
        }
        float lmin[3], lmax[3];
        aabb_reset(lmin, lmax);
        int lc = 0;
        const float node_area = aabb_area(node_min, node_max);
        float best_cost = (float) count; // the leaf's cost (one unit per primitive)
        for (int b = 0; b < RT_SAH_BINS - 1; b++) {
            if (bin_count[b]) aabb_add_box(lmin, lmax, bin_min[b], bin_max[b]);
            lc += bin_count[b];
            if (!lc || !right_count[b + 1]) continue;
            const float cost = 1.0f + (aabb_area(lmin, lmax) * (float) lc + right_area[b + 1] * (float) right_count[b + 1]) / (node_area > 0.0f ? node_area : 1.0f);
            if (cost < best_cost) { best_cost = cost; best_bin = b; }
        }
        if (best_bin >= 0) {
            const float split = cmin[axis] + (float) (best_bin + 1) / scale;
            int lo = first, hi = first + count - 1;
            while (lo <= hi) {
                if (prims[lo].centroid[axis] < split) { lo++; continue; }
                const BuildPrim tmp = prims[lo]; prims[lo] = prims[hi]; prims[hi] = tmp;
                hi--;
            }
            if (lo > first && lo < first + count) return lo;
            best_bin = -1; // the bins' rounding put everything on one side
        }
    }
    if (count <= leaf_max) return 0;
    // median split along the widest axis (nth_element would do; a full sort is fine at these sizes)
    const int mid = first + count / 2;
    for (int i = first + 1; i < first + count; i++) { // insertion sort by centroid[axis]: ranges here are small
        const BuildPrim key = prims[i];
        int j = i - 1;
        while (j >= first && prims[j].centroid[axis] > key.centroid[axis]) { prims[j + 1] = prims[j]; j--; }
        prims[j + 1] = key;
    }
    return mid;
}

// `leaf_offset` is added to a leaf's first primitive index (a TLAS half over instances [base, ...) keeps
// global instance indices in its leaves).
static int build_node(NodeList *list, BuildPrim *prims, const int first, const int count, const int depth, const int leaf_max, const int leaf_offset) {
    const int index = node_alloc(list);
    if (index < 0) return -1;
    float min[3], max[3];
    aabb_reset(min, max);
    for (int i = first; i < first + count; i++) aabb_add_box(min, max, prims[i].min, prims[i].max);

    int split = 0;
    if (depth < RT_MAX_DEPTH - 1 && count > 1) split = build_split(prims, first, count, leaf_max, min, max);
    if (!split) {
        RtNode *node = &list->nodes[index];
        memcpy(node->aabb_min, min, sizeof(min));
        memcpy(node->aabb_max, max, sizeof(max));
        node->a = -(first + leaf_offset) - 1;
        node->b = count;
        return index;
    }
    const int left = build_node(list, prims, first, split - first, depth + 1, leaf_max, leaf_offset);
    const int right = build_node(list, prims, split, first + count - split, depth + 1, leaf_max, leaf_offset);
    if (left < 0 || right < 0) return -1;
    RtNode *node = &list->nodes[index]; // after the children: the list may have moved
    memcpy(node->aabb_min, min, sizeof(min));
    memcpy(node->aabb_max, max, sizeof(max));
    node->a = left;
    node->b = right;
    return index;
}

// ---------------------------------------------------------------- BLAS

static int blas_build(RtScene *rt, RtPrivate *priv, const Geometry *geometry) {
    const int tri_count = geometry->index_count / 3;
    if (tri_count <= 0 || !geometry->tri_positions || !geometry->tri_indices) return -1;

    BuildPrim *prims = malloc((size_t) tri_count * sizeof(BuildPrim));
    if (!prims) return -1;
    int valid = 0;
    for (int t = 0; t < tri_count; t++) {
        BuildPrim *p = &prims[valid];
        aabb_reset(p->min, p->max);
        int ok = 1;
        for (int k = 0; k < 3 && ok; k++) {
            const unsigned int vi = geometry->tri_indices[t * 3 + k];
            if ((int) vi >= geometry->tri_vertex_count) { ok = 0; break; }
            aabb_add_point(p->min, p->max, &geometry->tri_positions[vi * 3]);
        }
        if (!ok) continue;
        for (int i = 0; i < 3; i++) p->centroid[i] = 0.5f * (p->min[i] + p->max[i]);
        p->index = t;
        valid++;
    }
    if (!valid) { free(prims); return -1; }

    NodeList list = {0};
    if (build_node(&list, prims, 0, valid, 0, RT_LEAF_MAX_TRIS, 0) < 0) { free(prims); free(list.nodes); return -1; }

    float *tris = grow(rt->tris, &rt->tri_cap, (rt->tri_count + valid) * 9, sizeof(float));
    RtBlas *blas_list = grow(rt->blas, &rt->blas_cap, rt->blas_count + 1, sizeof(RtBlas));
    if (!tris || !blas_list) { free(prims); free(list.nodes); if (tris) rt->tris = tris; if (blas_list) rt->blas = blas_list; return -1; }
    rt->tris = tris;
    rt->blas = blas_list;

    RtBlas *blas = &rt->blas[rt->blas_count];
    memset(blas, 0, sizeof(*blas));
    blas->geometry = geometry;
    blas->nodes = list.nodes;
    blas->node_count = list.count;
    blas->tri_first = rt->tri_count;
    blas->tri_count = valid;
    blas->source_tri_count = tri_count;
    blas->tri_epoch = geometry->tri_epoch;
    blas->tri_order = malloc((size_t) valid * sizeof(int));
    for (int i = 0; i < valid; i++) { // leaf order: leaves address contiguous triangles
        float *out = &rt->tris[(size_t) (blas->tri_first + i) * 9];
        for (int k = 0; k < 3; k++) memcpy(&out[k * 3], &geometry->tri_positions[geometry->tri_indices[prims[i].index * 3 + k] * 3], 3 * sizeof(float));
        if (blas->tri_order) blas->tri_order[i] = prims[i].index;
    }
    rt->tri_count += valid;
    free(prims);

    ptr_map_set(&priv->blas_by_geometry, geometry, rt->blas_count);
    rt->static_epoch++;
    return rt->blas_count++;
}

// The geometry's positions changed (Geometry.tri_epoch; a skinned glTF pose, a build batch edit) but not its
// triangle count: re-read the triangles in leaf order and recompute the node boxes bottom-up (build_node
// allocates a parent before its children, so a reverse walk sees every child first). No re-split.
static void blas_refit(RtScene *rt, RtBlas *blas, const Geometry *geometry) {
    if (!blas->tri_order) return;
    for (int i = 0; i < blas->tri_count; i++) {
        float *out = &rt->tris[(size_t) (blas->tri_first + i) * 9];
        const unsigned int *idx = &geometry->tri_indices[blas->tri_order[i] * 3];
        for (int k = 0; k < 3; k++) {
            if ((int) idx[k] >= geometry->tri_vertex_count) continue;
            memcpy(&out[k * 3], &geometry->tri_positions[idx[k] * 3], 3 * sizeof(float));
        }
    }
    for (int n = blas->node_count - 1; n >= 0; n--) {
        RtNode *node = &blas->nodes[n];
        aabb_reset(node->aabb_min, node->aabb_max);
        if (node->a < 0) {
            const int first = -node->a - 1;
            for (int i = 0; i < node->b; i++) {
                const float *v = &rt->tris[(size_t) (blas->tri_first + first + i) * 9];
                for (int k = 0; k < 3; k++) aabb_add_point(node->aabb_min, node->aabb_max, &v[k * 3]);
            }
        } else {
            aabb_add_box(node->aabb_min, node->aabb_max, blas->nodes[node->a].aabb_min, blas->nodes[node->a].aabb_max);
            aabb_add_box(node->aabb_min, node->aabb_max, blas->nodes[node->b].aabb_min, blas->nodes[node->b].aabb_max);
        }
    }
    blas->tri_epoch = geometry->tri_epoch;
    blas->refit_serial++;
    rt->refit_epoch++;
}

// The BLAS of `geometry`, built on first sight, refit when its positions moved, rebuilt (a new entry; the
// old one dies in place) when its triangle count changed. *changed = 1 when the boxes are not last frame's.
static int blas_for(RtScene *rt, RtPrivate *priv, const Geometry *geometry, int *changed) {
    int index;
    if (ptr_map_get(&priv->blas_by_geometry, geometry, &index)) {
        if (index >= 0 && rt->blas[index].tri_epoch != geometry->tri_epoch) {
            RtBlas *blas = &rt->blas[index];
            if (changed) *changed = 1;
            if (blas->source_tri_count == geometry->index_count / 3 && blas->tri_order) {
                blas_refit(rt, blas, geometry);
            } else {
                free(blas->nodes); free(blas->tri_order);
                blas->nodes = NULL; blas->tri_order = NULL; blas->node_count = 0; blas->geometry = NULL;
                index = blas_build(rt, priv, geometry);
                if (index < 0) ptr_map_set(&priv->blas_by_geometry, geometry, -1);
            }
        }
        return index;
    }
    index = blas_build(rt, priv, geometry);
    if (index < 0) ptr_map_set(&priv->blas_by_geometry, geometry, -1); // not traceable: remembered, not retried every frame
    return index;
}

// ---------------------------------------------------------------- materials

static unsigned material_hash(const RtMaterial *m) {
    const unsigned char *bytes = (const unsigned char *) m;
    unsigned h = 2166136261u;
    for (size_t i = 0; i < sizeof(*m); i++) { h ^= bytes[i]; h *= 16777619u; }
    return h & 4095u;
}

static int alpha_cast_forced(void) {
    static int forced = -1;
    if (forced < 0) {
        const char *env = getenv("KRUNKNATIVE_RT_ALPHA_CAST");
        forced = env && *env && strcmp(env, "0") != 0;
    }
    return forced;
}

static int material_index(RtScene *rt, RtPrivate *priv, const BasicMaterial *m) {
    RtMaterial want;
    memset(&want, 0, sizeof(want));
    want.color[0] = m->color.x; want.color[1] = m->color.y; want.color[2] = m->color.z; want.color[3] = m->color.w;
    want.emissive[0] = m->emissive.x; want.emissive[1] = m->emissive.y; want.emissive[2] = m->emissive.z;
    want.alpha_test = m->alpha_test;
    want.metalness = m->metalness_factor;
    want.roughness = m->roughness_factor;
    want.reflect_intensity = m->env_map ? m->env_map_intensity : m->scene_env_intensity;
    want.unlit = m->unlit || m->emissive.x > 0.0f || m->emissive.y > 0.0f || m->emissive.z > 0.0f;
    rt_atlas_material(&want, m); // phase 4: the atlas cell / UV mode, a skin env's metal look, forced reflections (rt_atlas.c)
    const unsigned bucket = material_hash(&want);
    for (int i = priv->mat_bucket[bucket]; i >= 0; i = priv->mat_next[i]) if (!memcmp(&rt->materials[i], &want, sizeof(want))) return i;
    if (rt->material_count >= 65536) return 0;
    RtMaterial *materials = grow(rt->materials, &rt->material_cap, rt->material_count + 1, sizeof(RtMaterial));
    int *next = grow(priv->mat_next, &priv->mat_next_cap, rt->material_count + 1, sizeof(int));
    if (!materials || !next) { if (materials) rt->materials = materials; if (next) priv->mat_next = next; return 0; }
    rt->materials = materials;
    priv->mat_next = next;
    rt->materials[rt->material_count] = want;
    priv->mat_next[rt->material_count] = priv->mat_bucket[bucket];
    priv->mat_bucket[bucket] = rt->material_count;
    rt->static_epoch++;
    return rt->material_count++;
}

// ---------------------------------------------------------------- instances

// Local extents of the unit shapes (loaders.c: x, z in [-0.5, 0.5], y in [0, 1]; the plane is the y = 1 face).
static void unit_local_aabb(const int kind, float min[3], float max[3]) {
    min[0] = -0.5f; min[1] = kind == RT_INST_PLANE ? 1.0f - RT_PLANE_PAD : 0.0f; min[2] = -0.5f;
    max[0] = 0.5f; max[1] = kind == RT_INST_PLANE ? 1.0f + RT_PLANE_PAD : 1.0f; max[2] = 0.5f;
}

static void world_aabb(const float l2w[12], const float lmin[3], const float lmax[3], float wmin[3], float wmax[3]) {
    aabb_reset(wmin, wmax);
    for (int c = 0; c < 8; c++) {
        const float p[3] = {c & 1 ? lmax[0] : lmin[0], c & 2 ? lmax[1] : lmin[1], c & 4 ? lmax[2] : lmin[2]};
        float w[3];
        mat34_point(l2w, p, w);
        aabb_add_point(wmin, wmax, w);
    }
}

// ---- per-mesh memory (MeshState)

static MeshState *mesh_state(RtPrivate *priv, const Mesh *mesh, int *fresh) {
    int index;
    *fresh = 0;
    if (ptr_map_get(&priv->state_by_mesh, mesh, &index)) return &priv->states[index];
    MeshState *states = grow(priv->states, &priv->state_cap, priv->state_count + 1, sizeof(MeshState));
    if (!states) return NULL;
    priv->states = states;
    MeshState *st = &priv->states[priv->state_count];
    memset(st, 0, sizeof(*st));
    st->mesh = mesh;
    st->map_casts = shadow_mesh_casts(mesh);
    st->static_index = -1;
    ptr_map_set(&priv->state_by_mesh, mesh, priv->state_count);
    priv->state_count++;
    *fresh = 1;
    return st;
}

// Every RT_STATE_SWEEP_FRAMES frames the states of meshes not seen since the last sweep are dropped (freed
// meshes; a new mesh at the same address would otherwise inherit a transform history).
#define RT_STATE_SWEEP_FRAMES 1024
static void states_sweep(RtPrivate *priv) {
    int kept = 0;
    priv_slots_clear(priv);
    ptr_map_clear(&priv->state_by_mesh);
    for (int i = 0; i < priv->state_count; i++) {
        if (priv->frame - priv->states[i].seen_frame > RT_STATE_SWEEP_FRAMES) continue;
        priv->states[kept] = priv->states[i];
        ptr_map_set(&priv->state_by_mesh, priv->states[kept].mesh, kept);
        kept++;
    }
    priv->state_count = kept;
}

static int push_mesh(const Mesh ***list, int *count, int *cap, const Mesh *mesh) {
    const Mesh **grown = grow((void *) *list, cap, *count + 1, sizeof(const Mesh *));
    if (!grown) return 0;
    *list = grown;
    grown[(*count)++] = mesh;
    return 1;
}

// Appends the instance of `mesh` (static half: dynamic 0). 0 = not traceable (nothing appended).
// `extra_flags`: RT_INST_SHADOW_ONLY for the local player's shadow proxy (rt_self.c), else 0.
static int instance_add(RtScene *rt, RtPrivate *priv, const Mesh *mesh, const int map_casts, const int dynamic, const unsigned extra_flags) {
    const Geometry *geometry = mesh->geometry;
    const BasicMaterial *bm = (const BasicMaterial *) mesh->material;
    if (bm->sprite) return 0; // camera-facing quads have no world orientation to trace

    int kind, blas = -1;
    if (geometry == g_cube_geometry) kind = RT_INST_BOX;
    else if (geometry == g_plane_geometry) kind = RT_INST_PLANE;
    else if (geometry == g_ramp_geometry) kind = RT_INST_RAMP;
    else {
        if (geometry->draw_mode) return 0; // lines
        blas = blas_for(rt, priv, geometry, NULL);
        if (blas < 0) return 0;
        kind = RT_INST_TRIANGLES;
    }

    RtInstance *list = grow(rt->instances, &rt->instance_cap, rt->instance_count + 1, sizeof(RtInstance));
    if (!list) return 0;
    rt->instances = list;
    RtInstance *inst = &rt->instances[rt->instance_count];
    memset(inst, 0, sizeof(*inst));

    const float *m = mesh->transform_matrix;
    memcpy(inst->local_to_world, m, 12 * sizeof(float)); // rows 0..2 of the row-major 4x4
    if (!mat34_invert(inst->local_to_world, inst->world_to_local)) return 0; // zero scale: nothing to hit
    {   // motion vectors (phase 6): the pose of the previous build from the mesh's state (this one's when unknown)
        int state_index;
        const float *prev = ptr_map_get(&priv->state_by_mesh, mesh, &state_index) ? priv->states[state_index].prev_matrix : m;
        memcpy(inst->prev_local_to_world, prev, 12 * sizeof(float));
    }

    float lmin[3], lmax[3];
    if (kind == RT_INST_TRIANGLES) {
        memcpy(lmin, rt->blas[blas].nodes[0].aabb_min, sizeof(lmin));
        memcpy(lmax, rt->blas[blas].nodes[0].aabb_max, sizeof(lmax));
    } else {
        unit_local_aabb(kind, lmin, lmax);
    }
    world_aabb(inst->local_to_world, lmin, lmax, inst->aabb_min, inst->aabb_max);

    inst->kind = kind;
    inst->blas = blas;
    inst->material = material_index(rt, priv, bm);
    inst->mesh = mesh;

    unsigned flags = 0;
    const int transparent = mesh->material->transparent;
    if (transparent) flags |= RT_INST_TRANSPARENT;
    if (bm->alpha_test > 0.0f) flags |= RT_INST_ALPHA_TEST;
    // a mirrored transform flips the winding: single-sided culling would cull the wrong faces, so
    // such instances are traced double-sided (conservative; rare: negative map scales)
    if (bm->double_sided || mat34_det3(inst->local_to_world) < 0.0f) flags |= RT_INST_DOUBLE_SIDED;
    if (rt->material_count && rt->materials[inst->material].unlit) flags |= RT_INST_UNLIT;
    if (map_casts == 1 || (map_casts < 0 && !transparent)) flags |= RT_INST_CASTS_SHADOW;
    // dev: KRUNKNATIVE_RT_ALPHA_CAST=1 makes the alpha-tested map objects cast too (Krunker's chain-link cubes do not:
    // cube shadows = opaque, shadow.c), so the cut-out shadow path (rt_common.glsl rt_alpha_passes) can be looked at
    if ((flags & RT_INST_ALPHA_TEST) && map_casts == 0 && alpha_cast_forced()) flags |= RT_INST_CASTS_SHADOW;
    if (dynamic) flags |= RT_INST_DYNAMIC;
    flags |= extra_flags; // the shadow proxy casts (map_casts -1 above) but is never drawn / reflected (RT_INST_SHADOW_ONLY)
    inst->flags = flags;

    rt->instance_count++;
    return 1;
}

// ---- the TLAS halves

// A BVH over instances [base, base + count), which it permutes into leaf order (leaf primitive indices stay
// global), into priv->seg. An empty range is one leaf of count 0 with an inverted box. 0 = out of memory.
static int half_build(RtScene *rt, RtPrivate *priv, const int base, const int count) {
    priv->seg.count = 0;
    if (count <= 0) {
        const int index = node_alloc(&priv->seg);
        if (index < 0) return 0;
        RtNode *node = &priv->seg.nodes[index];
        aabb_reset(node->aabb_min, node->aabb_max);
        node->a = -base - 1;
        node->b = 0;
        return 1;
    }
    BuildPrim *prims = malloc((size_t) count * sizeof(BuildPrim));
    RtInstance *sorted = malloc((size_t) count * sizeof(RtInstance));
    if (!prims || !sorted) { free(prims); free(sorted); return 0; }
    for (int i = 0; i < count; i++) {
        BuildPrim *p = &prims[i];
        memcpy(p->min, rt->instances[base + i].aabb_min, sizeof(p->min));
        memcpy(p->max, rt->instances[base + i].aabb_max, sizeof(p->max));
        for (int k = 0; k < 3; k++) p->centroid[k] = 0.5f * (p->min[k] + p->max[k]);
        p->index = base + i;
    }
    const int ok = build_node(&priv->seg, prims, 0, count, 0, RT_LEAF_MAX_INSTANCES, base) >= 0;
    if (ok) {
        for (int i = 0; i < count; i++) sorted[i] = rt->instances[prims[i].index]; // leaf order
        memcpy(&rt->instances[base], sorted, (size_t) count * sizeof(RtInstance));
    }
    free(prims);
    free(sorted);
    return ok;
}

// Copies priv->seg into rt->tlas at node `at` (inner links shifted by `at`). 0 = out of memory.
static int half_place(RtScene *rt, const RtPrivate *priv, const int at) {
    RtNode *tlas = grow(rt->tlas, &rt->tlas_cap, at + priv->seg.count, sizeof(RtNode));
    if (!tlas) return 0;
    rt->tlas = tlas;
    for (int n = 0; n < priv->seg.count; n++) {
        RtNode node = priv->seg.nodes[n];
        if (node.a >= 0) { node.a += at; node.b += at; }
        tlas[at + n] = node;
    }
    return 1;
}

// The static half: instances [0, n) from priv->stat, the BVH at nodes [1, 1 + static_tlas_count).
static void static_rebuild(RtScene *rt, RtPrivate *priv) {
    for (int i = 0; i < priv->state_count; i++) if (priv->states[i].static_index >= 0) priv->states[i].static_index = -1;
    rt->instance_count = 0;
    for (int i = 0; i < priv->stat_count; i++) {
        int fresh;
        MeshState *st = mesh_state(priv, priv->stat[i], &fresh);
        if (!st) continue;
        st->static_index = instance_add(rt, priv, priv->stat[i], st->map_casts, 0, 0) ? 0 : -2; // 0: placed below
    }
    rt->static_instance_count = rt->instance_count;
    if (!half_build(rt, priv, 0, rt->static_instance_count) || !half_place(rt, priv, 1)) {
        rt->static_instance_count = rt->instance_count = 0;
        priv->seg.count = 0;
        half_build(rt, priv, 0, 0);
        half_place(rt, priv, 1);
    }
    rt->static_tlas_count = priv->seg.count;
    for (int i = 0; i < rt->static_instance_count; i++) { // leaf order: the slots as placed
        int index;
        if (ptr_map_get(&priv->state_by_mesh, rt->instances[i].mesh, &index)) priv->states[index].static_index = i;
    }
    priv->static_built = 1;
    rt->static_tlas_epoch++;
}

void rt_scene_build(RtScene *rt, const Scene *scene, const Scene *shadow_only) {
    RtPrivate *priv = priv_for(rt);
    priv->frame++;
    rt->skipped = 0;
    priv->stat_count = priv->dyn_count = priv->self_count = 0;
    int dirty = !priv->static_built;
    int static_seen = 0; // members of the static half still eligible this frame

    // the scene's meshes, then the shadow proxy's (rt_self.c): those are never map objects, so they always land in
    // the dynamic half (their MeshState keeps the previous pose for the motion vectors like any player part's)
    const size_t scene_count = scene->mesh_count, shadow_count = shadow_only ? shadow_only->mesh_count : 0;
    if ((int) (scene_count + shadow_count) > priv->slot_cap) { // the slot cache grows with the scene (zeroed: no hits)
        const int cap = (int) (scene_count + shadow_count) + 256;
        const Mesh **slot_mesh = realloc(priv->slot_mesh, (size_t) cap * sizeof(*slot_mesh));
        int *slot_state = slot_mesh ? realloc(priv->slot_state, (size_t) cap * sizeof(*slot_state)) : NULL;
        if (slot_mesh) priv->slot_mesh = slot_mesh;
        if (slot_state) {
            priv->slot_state = slot_state;
            priv->slot_cap = cap;
        }
        priv_slots_clear(priv);
    }
    for (size_t i = 0; i < scene_count + shadow_count; i++) {
        const int self = i >= scene_count;
        Mesh *mesh = self ? shadow_only->meshes[i - scene_count] : scene->meshes[i];
        if (!mesh->visible || !mesh->geometry || !mesh->material) continue;
        mesh_update_transform_matrix(mesh);
        if (mesh->material->vtable != &basic_material_vtable) { rt->skipped++; continue; }
        int fresh = 0;
        MeshState *st = (int) i < priv->slot_cap && priv->slot_mesh[i] == mesh ? &priv->states[priv->slot_state[i]] : NULL;
        if (!st) {
            st = mesh_state(priv, mesh, &fresh);
            if (!st) { rt->skipped++; continue; }
            if ((int) i < priv->slot_cap) { priv->slot_mesh[i] = mesh; priv->slot_state[i] = (int) (st - priv->states); }
        }
        const float *m = mesh->transform_matrix;
        if (fresh) {
            memcpy(st->matrix, m, sizeof(st->matrix));
            memcpy(st->prev_matrix, m, sizeof(st->prev_matrix)); // no earlier pose: velocity is the camera's alone
            // a map object starts settled (the whole map would otherwise be dynamic for its first second);
            // anything else counts as moved
            st->moved_frame = st->map_casts >= 0 ? priv->frame - RT_STATIC_SETTLE_FRAMES - 1 : priv->frame;
        } else if (memcmp(st->matrix, m, sizeof(st->matrix)) != 0) {
            memcpy(st->prev_matrix, st->matrix, sizeof(st->prev_matrix)); // motion vectors: the pose of the build before
            memcpy(st->matrix, m, sizeof(st->matrix));
            st->moved_frame = priv->frame;
        } else {
            memcpy(st->prev_matrix, m, sizeof(st->prev_matrix)); // still: both poses are this one
        }
        st->seen_frame = priv->frame;

        // a geometry whose positions changed: refit / rebuild its BLAS now; a static member with one is stale
        const Geometry *geometry = mesh->geometry;
        if (geometry != g_cube_geometry && geometry != g_plane_geometry && geometry != g_ramp_geometry && !geometry->draw_mode) {
            int changed = 0;
            blas_for(rt, priv, geometry, &changed);
            if (changed && st->static_index >= 0) dirty = 1;
        }

        if (self) {
            if (!push_mesh(&priv->self_list, &priv->self_count, &priv->self_cap, mesh)) rt->skipped++;
            continue;
        }
        const int eligible = st->map_casts >= 0 && priv->frame - st->moved_frame > RT_STATIC_SETTLE_FRAMES;
        if (eligible) {
            if (!push_mesh(&priv->stat, &priv->stat_count, &priv->stat_cap, mesh)) { rt->skipped++; continue; }
            if (st->static_index == -1) dirty = 1;      // not in the half yet (-2 = known untraceable: no rebuild)
            else if (st->static_index >= 0) static_seen++;
        } else {
            if (!push_mesh(&priv->dyn, &priv->dyn_count, &priv->dyn_cap, mesh)) { rt->skipped++; continue; }
            if (st->static_index >= 0) dirty = 1;       // a member started moving
        }
    }
    if (!dirty && static_seen != rt->static_instance_count) dirty = 1; // a member is gone (hidden, removed)

    if (dirty) static_rebuild(rt, priv);
    else rt->instance_count = rt->static_instance_count;

    // the dynamic half: instances after the static ones, nodes after the static BVH
    const int dyn_base = rt->instance_count;
    for (int i = 0; i < priv->dyn_count; i++) {
        int fresh;
        const MeshState *st = mesh_state(priv, priv->dyn[i], &fresh);
        if (!instance_add(rt, priv, priv->dyn[i], st ? st->map_casts : -1, 1, 0)) rt->skipped++;
    }
    const int dyn_at = 1 + rt->static_tlas_count;
    if (!half_build(rt, priv, dyn_base, rt->instance_count - dyn_base) || !half_place(rt, priv, dyn_at)) {
        rt->instance_count = dyn_base;
        priv->seg.count = 0;
        half_build(rt, priv, dyn_base, 0);
        half_place(rt, priv, dyn_at);
    }
    rt->tlas_count = dyn_at + priv->seg.count;

    // the shadow puppet (rt_self.c): casters only (map_casts -1: not a map object), its own BVH after the dynamic
    // half's nodes and outside node 0's tree (RtScene.self_tlas_root): only the sun shadow ray starts there too.
    // Its instances and nodes sit inside the ranges rt_gpu re-uploads every frame (after the static half).
    rt->self_tlas_root = -1;
    const int self_base = rt->instance_count;
    for (int i = 0; i < priv->self_count; i++) {
        if (!instance_add(rt, priv, priv->self_list[i], -1, 1, RT_INST_SHADOW_ONLY)) rt->skipped++;
    }
    if (rt->instance_count > self_base) {
        const int self_at = rt->tlas_count;
        if (half_build(rt, priv, self_base, rt->instance_count - self_base) && half_place(rt, priv, self_at)) {
            rt->tlas_count = self_at + priv->seg.count;
            rt->self_tlas_root = self_at;
        } else {
            rt->instance_count = self_base;
        }
    }

    // the root over both halves (an empty half's inverted box adds nothing)
    RtNode *root = &rt->tlas[0];
    aabb_reset(root->aabb_min, root->aabb_max);
    aabb_add_box(root->aabb_min, root->aabb_max, rt->tlas[1].aabb_min, rt->tlas[1].aabb_max);
    aabb_add_box(root->aabb_min, root->aabb_max, rt->tlas[dyn_at].aabb_min, rt->tlas[dyn_at].aabb_max);
    root->a = 1;
    root->b = dyn_at;
    if (!rt->instance_count) rt->tlas_count = 0;

    if (priv->frame % RT_STATE_SWEEP_FRAMES == 0) states_sweep(priv);
    rt->alpha_test_count = 0; // the alpha-tested casters (rt_pass.c binds the atlas to the trace pass only when there are some)
    for (int i = 0; i < rt->instance_count; i++) {
        const unsigned flags = rt->instances[i].flags;
        if ((flags & RT_INST_CASTS_SHADOW) && (flags & RT_INST_ALPHA_TEST)) rt->alpha_test_count++;
    }
    rt->frame_epoch++;
    if (rt->skipped && !priv->skipped_logged) {
        priv->skipped_logged = 1;
        fprintf(stderr, "[rt] %d visible meshes are not traceable (no triangle positions / other material); logged once\n", rt->skipped);
    }
}

void rt_scene_unload_map(RtScene *rt) {
    RtPrivate *priv = priv_for(rt);
    for (int i = 0; i < rt->blas_count; i++) { free(rt->blas[i].nodes); free(rt->blas[i].tri_order); }
    rt->blas_count = 0;
    rt->tri_count = 0;
    rt->material_count = 0;
    rt->instance_count = 0;
    rt->tlas_count = 0;
    rt->static_instance_count = 0;
    rt->static_tlas_count = 0;
    rt->self_tlas_root = -1;
    ptr_map_clear(&priv->blas_by_geometry);
    priv_states_clear(priv);
    priv_materials_clear(priv);
    priv->skipped_logged = 0;
    rt->static_epoch++;
    rt->static_tlas_epoch++;
}

void rt_scene_free(RtScene *rt) {
    rt_scene_unload_map(rt);
    free(rt->instances); free(rt->tlas); free(rt->blas); free(rt->tris); free(rt->materials);
    memset(rt, 0, sizeof(*rt));
    if (g_priv.owner == rt) {
        ptr_map_free(&g_priv.blas_by_geometry);
        ptr_map_free(&g_priv.state_by_mesh);
        free(g_priv.states);
        free(g_priv.stat);
        free(g_priv.dyn);
        free(g_priv.self_list);
        free(g_priv.seg.nodes);
        free(g_priv.mat_next);
        free(g_priv.slot_mesh);
        free(g_priv.slot_state);
        memset(&g_priv, 0, sizeof(g_priv));
    }
}

// ---------------------------------------------------------------- intersection

typedef struct {
    float o[3], d[3], inv[3];
} Ray;

static void ray_init(Ray *ray, const float o[3], const float d[3]) {
    for (int i = 0; i < 3; i++) {
        ray->o[i] = o[i];
        ray->d[i] = d[i];
        ray->inv[i] = fabsf(d[i]) > 1e-20f ? 1.0f / d[i] : (d[i] < 0.0f ? -1e30f : 1e30f);
    }
}

// slab test; t_enter may be negative (the origin inside)
static int aabb_hit(const float min[3], const float max[3], const Ray *ray, const float t_max, float *t_enter) {
    float tmin = -FLT_MAX, tmax = t_max;
    for (int i = 0; i < 3; i++) {
        const float t1 = (min[i] - ray->o[i]) * ray->inv[i];
        const float t2 = (max[i] - ray->o[i]) * ray->inv[i];
        tmin = f_max(tmin, f_min(t1, t2));
        tmax = f_min(tmax, f_max(t1, t2));
    }
    if (tmax < f_max(tmin, 0.0f)) return 0;
    *t_enter = tmin;
    return 1;
}

typedef struct {
    float t;
    float normal[3]; // local space, geometric (not yet facing the ray)
    int triangle;
} LocalHit;

// Möller-Trumbore; single-sided skips back faces (CCW front, like GL_CCW + GL_BACK culling)
static int tri_hit(const float *v, const Ray *ray, const float t_max, const int double_sided, LocalHit *hit) {
    float e1[3], e2[3], p[3], tv[3], q[3];
    v3_sub(&v[3], &v[0], e1);
    v3_sub(&v[6], &v[0], e2);
    v3_cross(ray->d, e2, p);
    const float det = v3_dot(e1, p);
    if (double_sided ? fabsf(det) < RT_EPS : det < RT_EPS) return 0;
    const float inv = 1.0f / det;
    v3_sub(ray->o, &v[0], tv);
    const float u = v3_dot(tv, p) * inv;
    if (u < 0.0f || u > 1.0f) return 0;
    v3_cross(tv, e1, q);
    const float w = v3_dot(ray->d, q) * inv;
    if (w < 0.0f || u + w > 1.0f) return 0;
    const float t = v3_dot(e2, q) * inv;
    if (t <= RT_EPS || t >= t_max) return 0;
    hit->t = t;
    v3_cross(e1, e2, hit->normal);
    return 1;
}

// unit box [-0.5, 0.5] x [0, 1] x [-0.5, 0.5]: closed, so an origin inside hits the exit face
static int box_hit(const Ray *ray, const float t_max, LocalHit *hit) {
    static const float min[3] = {-0.5f, 0.0f, -0.5f}, max[3] = {0.5f, 1.0f, 0.5f};
    float tmin = -FLT_MAX, tmax = FLT_MAX;
    int axis_in = 0, axis_out = 0;
    for (int i = 0; i < 3; i++) {
        float t1 = (min[i] - ray->o[i]) * ray->inv[i];
        float t2 = (max[i] - ray->o[i]) * ray->inv[i];
        if (t1 > t2) { const float s = t1; t1 = t2; t2 = s; }
        if (t1 > tmin) { tmin = t1; axis_in = i; }
        if (t2 < tmax) { tmax = t2; axis_out = i; }
    }
    if (tmax < tmin || tmax <= RT_EPS) return 0;
    const int inside = tmin <= RT_EPS;
    const float t = inside ? tmax : tmin;
    if (t >= t_max) return 0;
    const int axis = inside ? axis_out : axis_in;
    hit->t = t;
    memset(hit->normal, 0, sizeof(hit->normal));
    // outward normal of the face: entering through the face the ray moves away from (-sign(d)), leaving through +sign(d)
    hit->normal[axis] = inside ? (ray->d[axis] > 0.0f ? 1.0f : -1.0f) : (ray->d[axis] > 0.0f ? -1.0f : 1.0f);
    hit->triangle = -1;
    return 1;
}

// unit wedge (loaders.c ramp): x in [-0.5, 0.5], y >= 0, z >= -0.5, slope y + z <= 0.5 (y = 1 at z = -0.5, 0 at z = 0.5)
static int ramp_hit(const Ray *ray, const float t_max, LocalHit *hit) {
    static const float planes[5][4] = { // n . p <= d
        {-1.0f, 0.0f, 0.0f, 0.5f},
        {1.0f, 0.0f, 0.0f, 0.5f},
        {0.0f, -1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, -1.0f, 0.5f},
        {0.0f, 1.0f, 1.0f, 0.5f},
    };
    float tmin = -FLT_MAX, tmax = FLT_MAX;
    int plane_in = -1, plane_out = -1;
    for (int i = 0; i < 5; i++) {
        const float denom = v3_dot(planes[i], ray->d);
        const float num = planes[i][3] - v3_dot(planes[i], ray->o);
        if (fabsf(denom) < 1e-20f) {
            if (num < 0.0f) return 0; // parallel and outside
            continue;
        }
        const float t = num / denom;
        if (denom > 0.0f) { if (t < tmax) { tmax = t; plane_out = i; } }
        else { if (t > tmin) { tmin = t; plane_in = i; } }
    }
    if (tmax < tmin || tmax <= RT_EPS) return 0;
    const int inside = tmin <= RT_EPS;
    const float t = inside ? tmax : tmin;
    if (t >= t_max) return 0;
    const int plane = inside ? plane_out : plane_in;
    if (plane < 0) return 0;
    hit->t = t;
    memcpy(hit->normal, planes[plane], sizeof(hit->normal));
    hit->triangle = -1;
    return 1;
}

// unit quad at y = 1, front face +y (loaders.c plane_vertices winding); single-sided unless the material is double-sided
static int plane_hit(const Ray *ray, const float t_max, const int double_sided, LocalHit *hit) {
    if (fabsf(ray->d[1]) < 1e-20f) return 0;
    if (!double_sided && ray->d[1] >= 0.0f) return 0;
    const float t = (1.0f - ray->o[1]) * ray->inv[1];
    if (t <= RT_EPS || t >= t_max) return 0;
    const float x = ray->o[0] + t * ray->d[0], z = ray->o[2] + t * ray->d[2];
    if (x < -0.5f || x > 0.5f || z < -0.5f || z > 0.5f) return 0;
    hit->t = t;
    hit->normal[0] = 0.0f; hit->normal[1] = 1.0f; hit->normal[2] = 0.0f;
    hit->triangle = -1;
    return 1;
}

static int blas_hit(const RtScene *rt, const RtBlas *blas, const Ray *ray, float t_max, const int double_sided, const int any, LocalHit *hit) {
    int stack[RT_STACK];
    int sp = 0;
    int found = 0;
    stack[sp++] = 0;
    while (sp) {
        const RtNode *node = &blas->nodes[stack[--sp]];
        float t_enter;
        if (!aabb_hit(node->aabb_min, node->aabb_max, ray, t_max, &t_enter)) continue;
        if (node->a < 0) {
            const int first = -node->a - 1;
            for (int i = 0; i < node->b; i++) {
                LocalHit h;
                if (!tri_hit(&rt->tris[(size_t) (blas->tri_first + first + i) * 9], ray, t_max, double_sided, &h)) continue;
                h.triangle = first + i;
                *hit = h;
                t_max = h.t;
                found = 1;
                if (any) return 1;
            }
        } else if (sp + 2 <= RT_STACK) {
            // nearer child first: push the farther one below it
            float ta, tb;
            const int ha = aabb_hit(blas->nodes[node->a].aabb_min, blas->nodes[node->a].aabb_max, ray, t_max, &ta);
            const int hb = aabb_hit(blas->nodes[node->b].aabb_min, blas->nodes[node->b].aabb_max, ray, t_max, &tb);
            if (ha && hb) {
                if (ta < tb) { stack[sp++] = node->b; stack[sp++] = node->a; }
                else { stack[sp++] = node->a; stack[sp++] = node->b; }
            } else if (ha) stack[sp++] = node->a;
            else if (hb) stack[sp++] = node->b;
        }
    }
    return found;
}

// Traces the world ray against one instance; on a hit closer than *t_max fills `hit` and lowers *t_max.
static int instance_hit(const RtScene *rt, const int index, const Ray *world, float *t_max, const int any, RtHit *hit) {
    const RtInstance *inst = &rt->instances[index];
    float o[3], d[3];
    mat34_point(inst->world_to_local, world->o, o);
    mat34_vector(inst->world_to_local, world->d, d);
    Ray local;
    ray_init(&local, o, d);
    const int double_sided = (inst->flags & RT_INST_DOUBLE_SIDED) != 0;

    LocalHit lh;
    int ok = 0;
    switch (inst->kind) {
        case RT_INST_BOX: ok = box_hit(&local, *t_max, &lh); break;
        case RT_INST_PLANE: ok = plane_hit(&local, *t_max, double_sided, &lh); break;
        case RT_INST_RAMP: ok = ramp_hit(&local, *t_max, &lh); break;
        default: ok = inst->blas >= 0 && blas_hit(rt, &rt->blas[inst->blas], &local, *t_max, double_sided, any, &lh); break;
    }
    if (!ok) return 0;

    *t_max = lh.t;
    if (!hit) return 1;
    hit->t = lh.t;
    hit->instance = index;
    hit->triangle = lh.triangle;
    mat34_normal(inst->world_to_local, lh.normal, hit->normal);
    v3_normalize(hit->normal);
    if (v3_dot(hit->normal, world->d) > 0.0f) for (int i = 0; i < 3; i++) hit->normal[i] = -hit->normal[i];
    for (int i = 0; i < 3; i++) hit->position[i] = world->o[i] + lh.t * world->d[i];
    return 1;
}

// The CPU tracer has no textures: alpha-tested casters (transparent in three's sense) never occlude here,
// while the GPU lets rays through their cut-outs and blocks them elsewhere (rt_common.glsl rt_occluder).
static int occluder(const RtInstance *inst) {
    return (inst->flags & RT_INST_CASTS_SHADOW) && !(inst->flags & RT_INST_TRANSPARENT);
}

static int tlas_trace(const RtScene *rt, const Ray *ray, float t_max, const int any, RtHit *hit) {
    if (!rt->tlas_count) return 0;
    int stack[RT_STACK];
    int sp = 0;
    int found = 0;
    if (any && rt->self_tlas_root >= 0) stack[sp++] = rt->self_tlas_root; // the shadow puppet: occlusion only
    stack[sp++] = 0;
    while (sp) {
        const RtNode *node = &rt->tlas[stack[--sp]];
        float t_enter;
        if (!aabb_hit(node->aabb_min, node->aabb_max, ray, t_max, &t_enter)) continue;
        if (node->a < 0) {
            const int first = -node->a - 1;
            for (int i = 0; i < node->b; i++) {
                const int index = first + i;
                if (any ? !occluder(&rt->instances[index]) : (rt->instances[index].flags & RT_INST_SHADOW_ONLY) != 0) continue;
                if (!instance_hit(rt, index, ray, &t_max, any, hit)) continue;
                found = 1;
                if (any) return 1;
            }
        } else if (sp + 2 <= RT_STACK) {
            float ta, tb;
            const int ha = aabb_hit(rt->tlas[node->a].aabb_min, rt->tlas[node->a].aabb_max, ray, t_max, &ta);
            const int hb = aabb_hit(rt->tlas[node->b].aabb_min, rt->tlas[node->b].aabb_max, ray, t_max, &tb);
            if (ha && hb) {
                if (ta < tb) { stack[sp++] = node->b; stack[sp++] = node->a; }
                else { stack[sp++] = node->a; stack[sp++] = node->b; }
            } else if (ha) stack[sp++] = node->a;
            else if (hb) stack[sp++] = node->b;
        }
    }
    return found;
}

int rt_scene_trace(const RtScene *rt, const float origin[3], const float dir[3], const float t_max, RtHit *hit) {
    Ray ray;
    ray_init(&ray, origin, dir);
    RtHit local;
    local.instance = -1;
    const int found = tlas_trace(rt, &ray, t_max, 0, &local);
    if (hit) { if (found) *hit = local; else hit->instance = -1; }
    return found;
}

int rt_scene_occluded(const RtScene *rt, const float origin[3], const float dir[3], const float t_max) {
    Ray ray;
    ray_init(&ray, origin, dir);
    return tlas_trace(rt, &ray, t_max, 1, NULL);
}

int rt_scene_trace_brute(const RtScene *rt, const float origin[3], const float dir[3], float t_max, RtHit *hit) {
    Ray ray;
    ray_init(&ray, origin, dir);
    RtHit best;
    best.instance = -1;
    int found = 0;
    for (int i = 0; i < rt->instance_count; i++) {
        RtHit h;
        if (instance_hit(rt, i, &ray, &t_max, 0, &h)) { best = h; found = 1; }
    }
    if (hit) *hit = best;
    return found;
}

// ---------------------------------------------------------------- emissive lights (rt.h, GI next-event estimation)

static int no_emissive_lights(void) { // KRUNKNATIVE_RT_NO_EMISSIVE_LIGHTS=1 (dev): the list stays empty (A/B)
    static int off = -1;
    if (off < 0) { const char *e = getenv("KRUNKNATIVE_RT_NO_EMISSIVE_LIGHTS"); off = e && *e && strcmp(e, "0") != 0; }
    return off;
}

// The radiance rt_gi.frag gives a bounce that hits this material (material colour x the texture's mean + emissive),
// so the sampled light and an unsampled hit agree.
static void emitter_radiance(const RtMaterial *m, float out[3]) {
    float mean[3] = {1.0f, 1.0f, 1.0f};
    const int code = (int) (m->atlas[0] + 0.5f);
    if (code > 0 && !rt_atlas_mean_rgb(code % RT_ATLAS_UV_MODE_STEP - 1, mean)) mean[0] = mean[1] = mean[2] = 0.5f; // not drawn yet
    for (int c = 0; c < 3; c++) out[c] = (m->color[c] * mean[c] + m->emissive[c]) * RT_EMISSIVE_LIGHT_SCALE;
}

typedef struct {
    float score;
    int index;
} EmitterScore;

// 1 when `a` ranks after `b`: the lower score, equal scores in instance order (what the stable sort the light
// list used before, glibc's merge-sort qsort, gave)
static int emitter_after(const EmitterScore *a, const EmitterScore *b) {
    return a->score < b->score || (a->score == b->score && a->index > b->index);
}

static void emitter_sift_down(EmitterScore *heap, const int n, int i) {
    for (;;) { // a heap whose root is the entry ranked last
        const int l = 2 * i + 1, r = l + 1;
        int last = i;
        if (l < n && emitter_after(&heap[l], &heap[last])) last = l;
        if (r < n && emitter_after(&heap[r], &heap[last])) last = r;
        if (last == i) return;
        const EmitterScore t = heap[i]; heap[i] = heap[last]; heap[last] = t;
        i = last;
    }
}

// The best `max` of `count` scores, best first, in scores[0..max): a bounded heap over the candidates
// (O(count log max)) instead of sorting every emitter of the map each frame (Kanji: ~770)
static void emitter_top(EmitterScore *scores, const int count, const int max) {
    for (int i = max / 2 - 1; i >= 0; i--) emitter_sift_down(scores, max, i);
    for (int i = max; i < count; i++) {
        if (!emitter_after(&scores[0], &scores[i])) continue; // not better than the worst kept
        scores[0] = scores[i];
        emitter_sift_down(scores, max, 0);
    }
    for (int i = 1; i < max; i++) { // the kept ones best first (max is RT_EMISSIVE_MAX: 64)
        const EmitterScore e = scores[i];
        int j = i - 1;
        while (j >= 0 && emitter_after(&scores[j], &e)) { scores[j + 1] = scores[j]; j--; }
        scores[j + 1] = e;
    }
}

int rt_scene_emissive_lights(const RtScene *rt, const float camera[3], RtEmissiveLight *out, const int max) {
    if (no_emissive_lights() || max <= 0 || !rt->material_count) return 0;
    static EmitterScore *scores;
    static int score_cap;
    int count = 0;
    for (int i = 0; i < rt->instance_count; i++) {
        const RtInstance *inst = &rt->instances[i];
        if (!(inst->flags & RT_INST_UNLIT) || (inst->flags & (RT_INST_TRANSPARENT | RT_INST_SHADOW_ONLY))) continue;
        float le[3];
        emitter_radiance(&rt->materials[inst->material], le);
        const float lum = 0.2126f * le[0] + 0.7152f * le[1] + 0.0722f * le[2];
        if (lum < RT_EMISSIVE_MIN_LUM) continue;
        float dist2 = 0.0f, extent2 = 0.0f;
        for (int k = 0; k < 3; k++) {
            const float c = 0.5f * (inst->aabb_min[k] + inst->aabb_max[k]), h = 0.5f * (inst->aabb_max[k] - inst->aabb_min[k]);
            dist2 += (c - camera[k]) * (c - camera[k]);
            extent2 += h * h;
        }
        const float hx = inst->aabb_max[0] - inst->aabb_min[0], hy = inst->aabb_max[1] - inst->aabb_min[1], hz = inst->aabb_max[2] - inst->aabb_min[2];
        const float area = 2.0f * (hx * hy + hy * hz + hx * hz);
        if (count >= score_cap) {
            const int cap = score_cap ? score_cap * 2 : 256;
            EmitterScore *grown = realloc(scores, (size_t) cap * sizeof(EmitterScore));
            if (!grown) break;
            scores = grown;
            score_cap = cap;
        }
        // what it can give the area around the camera: power over the squared distance (the near field capped)
        scores[count++] = (EmitterScore) {lum * area / (dist2 + extent2 + 1.0f), i};
    }
    if (count > max) emitter_top(scores, count, max);
    const int n = count < max ? count : max;
    for (int j = 0; j < n; j++) {
        const RtInstance *inst = &rt->instances[scores[j].index];
        float lmin[3], lmax[3];
        if (inst->kind == RT_INST_TRIANGLES && inst->blas >= 0 && rt->blas[inst->blas].node_count > 0) {
            memcpy(lmin, rt->blas[inst->blas].nodes[0].aabb_min, sizeof(lmin));
            memcpy(lmax, rt->blas[inst->blas].nodes[0].aabb_max, sizeof(lmax));
        } else {
            unit_local_aabb(inst->kind, lmin, lmax);
            if (inst->kind == RT_INST_PLANE) lmin[1] = lmax[1] = 1.0f; // the y = 1 face itself
        }
        RtEmissiveLight *l = &out[j];
        const float lc[3] = {0.5f * (lmin[0] + lmax[0]), 0.5f * (lmin[1] + lmax[1]), 0.5f * (lmin[2] + lmax[2])};
        mat34_point(inst->local_to_world, lc, l->center);
        for (int k = 0; k < 3; k++) {
            float v[3] = {0.0f, 0.0f, 0.0f};
            v[k] = 0.5f * (lmax[k] - lmin[k]);
            mat34_vector(inst->local_to_world, v, l->axis[k]);
        }
        emitter_radiance(&rt->materials[inst->material], l->radiance);
        l->instance = scores[j].index;
    }
    return n;
}
