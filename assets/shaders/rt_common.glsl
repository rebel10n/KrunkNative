// Ray tracing: BVH traversal over the float tables of client/include/rt.h (uploaded by
// client/src/rt/rt_gpu.c). Pulled into rt_trace.frag / rt_reflect.frag / rt_debug.frag through shader.c's
// `#include "rt_common.glsl"`. The same traversal as rt_scene.c's CPU tracer: a TLAS over
// instances, each an analytic unit box / plane / ramp or a BLAS of local-space triangles.
// GLSL ES 3.00 subset: texelFetch only, fixed-size stacks, bounded loops.

uniform highp usampler2D rt_tlas; // TLAS nodes (RT_NODE_TEXELS each, RGBA32UI; node 0 the root)
uniform highp usampler2D rt_blas; // every BLAS's nodes, one table (links already offset by the BLAS base)
uniform sampler2D rt_tris;   // triangles (RT_TRI_TEXELS each; BLAS leaves index them globally)
uniform sampler2D rt_inst;   // instances (RT_INST_TEXELS each)
uniform sampler2D rt_mats;   // materials (RT_MAT_TEXELS each)
uniform int rt_tlas_count;   // 0 = nothing to trace

#define RT_TEX_SHIFT 11      // RT_TEX_WIDTH 2048
#define RT_TEX_MASK 2047
#define RT_STACK 32          // ONE stack for the TLAS and the BLAS under it (rt_tlas_trace)
#define RT_MAX_STEPS 2048    // traversal iterations a ray may spend (bounded loops for ES)
#define RT_EPS 1e-7
#define RT_INF 1e30

#define RT_KIND_BOX 0
#define RT_KIND_PLANE 1
#define RT_KIND_RAMP 2
#define RT_KIND_TRIANGLES 3

#define RT_INST_CASTS_SHADOW 1
#define RT_INST_DOUBLE_SIDED 2
#define RT_INST_ALPHA_TEST 4
#define RT_INST_TRANSPARENT 8
#define RT_INST_DYNAMIC 16
#define RT_INST_UNLIT 32
#define RT_INST_SHADOW_ONLY 64 // the local player's shadow proxy (rt_self.c): occludes, never a closest hit

vec4 rt_fetch(sampler2D table, int texel) {
    return texelFetch(table, ivec2(texel & RT_TEX_MASK, texel >> RT_TEX_SHIFT), 0);
}

uvec4 rt_fetch_node(highp usampler2D table, int texel) {
    return texelFetch(table, ivec2(texel & RT_TEX_MASK, texel >> RT_TEX_SHIFT), 0);
}

struct RtRay {
    vec3 o;
    vec3 d;
    vec3 inv;
    vec3 oinv; // o * inv: the slab test is one fma per axis (t = b * inv - oinv)
};

RtRay rt_ray(vec3 o, vec3 d) {
    RtRay r;
    r.o = o;
    r.d = d;
    // a zero component: a huge inverse keeps the slab test finite (rt_scene.c ray_init)
    r.inv = vec3(
        abs(d.x) > 1e-20 ? 1.0 / d.x : (d.x < 0.0 ? -RT_INF : RT_INF),
        abs(d.y) > 1e-20 ? 1.0 / d.y : (d.y < 0.0 ? -RT_INF : RT_INF),
        abs(d.z) > 1e-20 ? 1.0 / d.z : (d.z < 0.0 ? -RT_INF : RT_INF));
    r.oinv = o * r.inv;
    return r;
}

// slab test; t_enter may be negative (the origin inside the box). An empty slot's inverted box never passes.
bool rt_aabb(vec3 bmin, vec3 bmax, RtRay ray, float t_max, out float t_enter) {
    vec3 t1 = bmin * ray.inv - ray.oinv;
    vec3 t2 = bmax * ray.inv - ray.oinv;
    vec3 tn = min(t1, t2);
    vec3 tf = max(t1, t2);
    float tmin = max(max(tn.x, tn.y), tn.z);
    float tmax = min(min(min(tf.x, tf.y), tf.z), t_max);
    t_enter = tmin;
    return tmax >= max(tmin, 0.0);
}

struct RtLocalHit {
    float t;
    vec3 normal;   // local space, geometric
    int triangle;  // -1 analytic
};

// Moeller-Trumbore; single-sided skips back faces (CCW front)
bool rt_tri(int tri, RtRay ray, float t_max, bool double_sided, inout RtLocalHit hit) {
    vec3 v0 = rt_fetch(rt_tris, tri * 3).xyz;
    vec3 v1 = rt_fetch(rt_tris, tri * 3 + 1).xyz;
    vec3 v2 = rt_fetch(rt_tris, tri * 3 + 2).xyz;
    vec3 e1 = v1 - v0, e2 = v2 - v0;
    vec3 p = cross(ray.d, e2);
    float det = dot(e1, p);
    if (double_sided ? abs(det) < RT_EPS : det < RT_EPS) return false;
    float inv = 1.0 / det;
    vec3 tv = ray.o - v0;
    float u = dot(tv, p) * inv;
    if (u < 0.0 || u > 1.0) return false;
    vec3 q = cross(tv, e1);
    float w = dot(ray.d, q) * inv;
    if (w < 0.0 || u + w > 1.0) return false;
    float t = dot(e2, q) * inv;
    if (t <= RT_EPS || t >= t_max) return false;
    hit.t = t;
    hit.normal = cross(e1, e2);
    hit.triangle = tri;
    return true;
}

// unit box [-0.5, 0.5] x [0, 1] x [-0.5, 0.5]: closed, an origin inside hits the exit face
bool rt_box(RtRay ray, float t_max, inout RtLocalHit hit) {
    vec3 t1 = vec3(-0.5, 0.0, -0.5) * ray.inv - ray.oinv;
    vec3 t2 = vec3(0.5, 1.0, 0.5) * ray.inv - ray.oinv;
    vec3 tn = min(t1, t2);
    vec3 tf = max(t1, t2);
    float tmin = max(max(tn.x, tn.y), tn.z);
    float tmax = min(min(tf.x, tf.y), tf.z);
    if (tmax < tmin || tmax <= RT_EPS) return false;
    bool inside = tmin <= RT_EPS;
    float t = inside ? tmax : tmin;
    if (t >= t_max) return false;
    vec3 n;
    if (inside) n = vec3(equal(tf, vec3(tmax))) * sign(ray.d);
    else n = -vec3(equal(tn, vec3(tmin))) * sign(ray.d);
    // several equal components (an edge hit): keep one axis
    if (abs(n.x) > 0.0) n = vec3(n.x, 0.0, 0.0);
    else if (abs(n.y) > 0.0) n = vec3(0.0, n.y, 0.0);
    hit.t = t;
    hit.normal = n;
    hit.triangle = -1;
    return true;
}

// unit wedge (loaders.c ramp): x in [-0.5, 0.5], y >= 0, z >= -0.5, slope y + z <= 0.5
bool rt_ramp(RtRay ray, float t_max, inout RtLocalHit hit) {
    vec4 planes[5];
    planes[0] = vec4(-1.0, 0.0, 0.0, 0.5);
    planes[1] = vec4(1.0, 0.0, 0.0, 0.5);
    planes[2] = vec4(0.0, -1.0, 0.0, 0.0);
    planes[3] = vec4(0.0, 0.0, -1.0, 0.5);
    planes[4] = vec4(0.0, 1.0, 1.0, 0.5);
    float tmin = -RT_INF, tmax = RT_INF;
    int plane_in = -1, plane_out = -1;
    for (int i = 0; i < 5; i++) {
        float denom = dot(planes[i].xyz, ray.d);
        float num = planes[i].w - dot(planes[i].xyz, ray.o);
        if (abs(denom) < 1e-20) {
            if (num < 0.0) return false;
            continue;
        }
        float t = num / denom;
        if (denom > 0.0) { if (t < tmax) { tmax = t; plane_out = i; } }
        else { if (t > tmin) { tmin = t; plane_in = i; } }
    }
    if (tmax < tmin || tmax <= RT_EPS) return false;
    bool inside = tmin <= RT_EPS;
    float t = inside ? tmax : tmin;
    if (t >= t_max) return false;
    int plane = inside ? plane_out : plane_in;
    if (plane < 0) return false;
    hit.t = t;
    hit.normal = planes[plane].xyz;
    hit.triangle = -1;
    return true;
}

// unit quad at y = 1, front face +y; single-sided unless double_sided
bool rt_plane(RtRay ray, float t_max, bool double_sided, inout RtLocalHit hit) {
    if (abs(ray.d.y) < 1e-20) return false;
    if (!double_sided && ray.d.y >= 0.0) return false;
    float t = (1.0 - ray.o.y) * ray.inv.y;
    if (t <= RT_EPS || t >= t_max) return false;
    float x = ray.o.x + t * ray.d.x, z = ray.o.z + t * ray.d.z;
    if (x < -0.5 || x > 0.5 || z < -0.5 || z > 0.5) return false;
    hit.t = t;
    hit.normal = vec3(0.0, 1.0, 0.0);
    hit.triangle = -1;
    return true;
}

// ---- BVH nodes (rt.h "GPU table layout"): RT_NODE_TEXELS 2 texels of an RGBA32UI table, one per CHILD slot:
//   xyz = the child's box as half floats (min rounded down, max up), w = RT_SLOT_INNER | node index (an inner child), or
//   first << 11 | count (a leaf child; count 0: an empty slot). A node is entered without a box test of its own (its
// parent tested it), so a traversal step is 2 fetches and 2 slab tests.
// Traversal entries: a node index (>= 0), or a leaf as -(first << 11 | count) - 1.
#define RT_SLOT_INNER 0x80000000u
#define RT_SLOT_COUNT_BITS 11
#define RT_SLOT_COUNT_MASK 2047
#define RT_NONE (-2147483647)

bool rt_slot(uvec4 s, RtRay ray, float t_max, out float t_enter, out int entry) {
    entry = (s.w & RT_SLOT_INNER) != 0u ? int(s.w & 0x7FFFFFFFu) : -int(s.w) - 1;
    t_enter = RT_INF;
    if (s.w == 0u) return false; // an empty slot
    vec2 a = unpackHalf2x16(s.x), b = unpackHalf2x16(s.y), c = unpackHalf2x16(s.z);
    return rt_aabb(vec3(a.x, a.y, b.x), vec3(b.y, c.x, c.y), ray, t_max, t_enter);
}

// The children of node `index` of `table` the ray enters before t_max, nearer first: returns 0, 1 or 2.
int rt_node_visit(highp usampler2D table, int index, RtRay ray, float t_max, out int near, out float t_near, out int far, out float t_far) {
    uvec4 l = rt_fetch_node(table, index * 2), r = rt_fetch_node(table, index * 2 + 1);
    float tl, tr;
    int el, er;
    bool hl = rt_slot(l, ray, t_max, tl, el);
    bool hr = rt_slot(r, ray, t_max, tr, er);
    near = RT_NONE; far = RT_NONE; t_near = RT_INF; t_far = RT_INF;
    if (hl && hr) {
        if (tl <= tr) { near = el; t_near = tl; far = er; t_far = tr; }
        else { near = er; t_near = tr; far = el; t_far = tl; }
        return 2;
    }
    if (hl) { near = el; t_near = tl; return 1; }
    if (hr) { near = er; t_near = tr; return 1; }
    return 0;
}

struct RtHit {
    float t;
    int instance;  // -1 miss
    int triangle;
    int material;
    int flags;
    vec3 normal;   // world space, facing the ray
    vec3 position; // world space
};

// Fills `hit` from a local hit `lh` on instance `inst` (`info` its t6, w0..w2 its world_to_local rows).
// need_normal 0 (an occlusion query on a solid caster) skips the world normal and position.
void rt_record(inout RtHit hit, int inst, vec4 info, RtLocalHit lh, vec4 w0, vec4 w1, vec4 w2, RtRay world, bool need_normal) {
    hit.t = lh.t;
    hit.instance = inst;
    hit.triangle = lh.triangle;
    hit.material = int(info.z);
    hit.flags = int(info.w);
    if (!need_normal) return;
    // n' = (M^-1)^T n with w2l = M^-1
    vec3 n = normalize(vec3(dot(vec3(w0.x, w1.x, w2.x), lh.normal),
                            dot(vec3(w0.y, w1.y, w2.y), lh.normal),
                            dot(vec3(w0.z, w1.z, w2.z), lh.normal)));
    if (dot(n, world.d) > 0.0) n = -n;
    hit.normal = n;
    hit.position = world.o + lh.t * world.d;
}

// ---- the texture atlas (client/src/rt/rt_atlas.c, rt.h "atlas"): RT_ATLAS_COLS x RT_ATLAS_COLS cells of
// RT_ATLAS_CELL px; material t2 = (slot + 1 + RT_ATLAS_UV_MODE_STEP * uv_mode, uv_size.xyz), 0 = no texture.
// Bound by the reflection pass (its hit albedo) and by the trace pass while some caster is alpha-tested
// (rt_atlas_ready 0 otherwise: rt_pass.c trace()).
uniform sampler2D rt_atlas;
uniform int rt_atlas_ready;
// 1 inside the first-person viewmodel passes (rt_pass.c rt_viewmodel_frame): closest hits may land on the
// shadow proxy, so a ray leaving the weapon towards the player's own body reflects / bounces off it instead of
// passing through to whatever lies behind (the sky, at a map edge). 0 for the world's rays.
uniform int rt_hit_shadow_only;
#define RT_ATLAS_COLS 16
#define RT_ATLAS_CELL 128.0
#define RT_ATLAS_CELL_UV (1.0 / 16.0)
#define RT_ATLAS_UV_MODE_STEP 4096

// basic.frag krunker_face_uv / krunker_cube_uv without the flips: u = 0.5 + a * size_u, v = (b + 0.5) * size_v
vec2 rt_face_uv(vec3 p, vec3 center, vec3 axis_u, vec3 axis_v, vec2 size) {
    vec3 d = p - center;
    return vec2(0.5 + dot(d, axis_u) * size.x, (dot(d, axis_v) + 0.5) * size.y);
}

vec2 rt_cube_uv(vec3 p, vec3 ln, vec3 s) {
    vec3 a = abs(ln);
    if (a.z >= a.x && a.z >= a.y) {
        if (ln.z > 0.0) return rt_face_uv(p, vec3(0.0, 0.5, 0.5), vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), s.xy);
        return rt_face_uv(p, vec3(0.0, 0.5, -0.5), vec3(-1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), s.xy);
    }
    if (a.x >= a.y) {
        if (ln.x < 0.0) return rt_face_uv(p, vec3(-0.5, 0.5, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 0.0), s.zy);
        return rt_face_uv(p, vec3(0.5, 0.5, 0.0), vec3(0.0, 0.0, -1.0), vec3(0.0, 1.0, 0.0), s.zy);
    }
    if (ln.y > 0.0) return rt_face_uv(p, vec3(0.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, -1.0), s.xz);
    return rt_face_uv(p, vec3(0.0), vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, 1.0), s.xz);
}

// The atlas texture coordinate of a hit on instance `instance` (world position / normal) with material
// t2 `m2`: the Krunker cube / plane UV rebuilt from the hit's local position (no texture repeat / offset /
// rotation / flips), wrapped inside the material's cell. False when the material has no cell or its
// geometry has mesh UVs (OBJ / glTF: positions only in the triangle table).
bool rt_atlas_coord(int instance, vec3 position, vec3 normal, vec4 m2, out vec2 coord) {
    coord = vec2(0.0);
    int code = int(m2.x + 0.5);
    if (code <= 0 || rt_atlas_ready == 0) return false;
    int uv_mode = code / RT_ATLAS_UV_MODE_STEP;
    int slot = code - uv_mode * RT_ATLAS_UV_MODE_STEP - 1;
    if (uv_mode == 0) return false;
    // the hit in the instance's local space (its unit box / plane), and the local normal n_l = M^T n_w
    int base = instance * 8;
    vec4 w2l0 = rt_fetch(rt_inst, base), w2l1 = rt_fetch(rt_inst, base + 1), w2l2 = rt_fetch(rt_inst, base + 2);
    vec4 l2w0 = rt_fetch(rt_inst, base + 3), l2w1 = rt_fetch(rt_inst, base + 4), l2w2 = rt_fetch(rt_inst, base + 5);
    vec4 wp = vec4(position, 1.0);
    vec3 lp = vec3(dot(w2l0, wp), dot(w2l1, wp), dot(w2l2, wp));
    vec3 ln = vec3(dot(l2w0.xyz, normal), dot(l2w1.xyz, normal), dot(l2w2.xyz, normal));
    vec2 tc = uv_mode == 1 ? rt_cube_uv(lp, ln, m2.yzw) : vec2(0.5 + lp.x * m2.y, (lp.y + 0.5) * m2.z);
    vec2 cell = vec2(float(slot % RT_ATLAS_COLS), float(slot / RT_ATLAS_COLS));
    // wrap inside the cell, half a texel in from its edges (the neighbours must not bleed in)
    vec2 inner = fract(tc) * (1.0 - 1.0 / RT_ATLAS_CELL) + 0.5 / RT_ATLAS_CELL;
    coord = (cell + inner) * RT_ATLAS_CELL_UV;
    return true;
}

// rgba of the material's texture at the hit (lod 0: the cells have no mips, and the traversal's control
// flow is divergent, so no implicit derivatives). a = 1 where the UV could not be rebuilt.
vec4 rt_atlas_sample(int instance, vec3 position, vec3 normal, vec4 m2) {
    vec2 coord;
    if (!rt_atlas_coord(instance, position, normal, m2, coord)) return vec4(1.0);
    return textureLod(rt_atlas, coord, 0.0);
}

// Occlusion queries count the shadow casters that are not transparent, plus, while the atlas is bound
// (rt_atlas_ready), the alpha-tested casters (RT_INST_ALPHA_TEST: chain-link, foliage, cut-out user
// assets; transparent in three's sense, so skipped otherwise): rt_alpha_passes() lets the ray through
// their cut-outs. Analytic instances test the texel of the face the ray enters (a thin fence has the same
// pattern on both faces); triangle instances have no UVs in the triangle table and occlude as solid.
// The CPU tracer (rt_scene.c occluder) has no textures and keeps the solid rule.
bool rt_occluder(int flags) {
    if ((flags & RT_INST_CASTS_SHADOW) == 0) return false;
    if ((flags & RT_INST_TRANSPARENT) == 0) return true;
    return (flags & RT_INST_ALPHA_TEST) != 0 && rt_atlas_ready != 0;
}

// true when an any-hit on an alpha-tested instance lands in a cut-out (alpha below the material's alpha_test)
bool rt_alpha_passes(RtHit hit) {
    if ((hit.flags & RT_INST_ALPHA_TEST) == 0 || rt_atlas_ready == 0 || hit.triangle >= 0) return false;
    int mb = hit.material * 4;
    vec4 m1 = rt_fetch(rt_mats, mb + 1), m2 = rt_fetch(rt_mats, mb + 2);
    vec2 coord;
    if (!rt_atlas_coord(hit.instance, hit.position, hit.normal, m2, coord)) return false;
    return textureLod(rt_atlas, coord, 0.0).a < m1.w;
}

// Closest-hit queries skip transparent instances while this is set (rt_reflect.frag: a reflection ray
// passes through glass, liquids and the AO strips that lie on every face; it never shades them).
bool rt_skip_transparent = false;

// Occlusion queries skip the local player's shadow puppet (RT_INST_SHADOW_ONLY, rt_self.c) while this is set.
// Only the sun shadow ray may see it (that is the feature: your own shadow on the ground); AO, local-light
// shadow, GI and reflection rays never enter its ~40 boxes, which sit around the camera and were traversed
// by every ray leaving a nearby surface (~3 ms of the RT pass at 1080p / 0.5). So your own body does not
// occlude AO or GI, like Krunker, which has no self-AO either. rt_hit_shadow_only (1: closest hits may land on it) is 0
// for every pass: the viewmodel's rays started inside the puppet's arms and rifle (rt_pass.c).
bool rt_skip_shadow_only = false;
// The puppet's own TLAS subtree (RtScene.self_tlas_root; -1 none): node 0's tree does not link to it, so a query that
// may see the puppet pushes it as a second start node, and every other ray pays nothing for it.
uniform int rt_self_root;

// TLAS traversal, and the BLAS of every triangle instance on the SAME stack: a triangle instance pushes the rest of its
// TLAS leaf and a sentinel, then the traversal continues in rt_blas with the instance's local ray (an affine map keeps
// the ray parameter, so t_max carries over) until the sentinel pops it back to the TLAS. One stack instead of one per
// nesting level keeps the traversal state small (the stacks live in slow local memory). any = occlusion query (only
// RT_INST_CASTS_SHADOW, non-transparent instances count).
#define RT_SENTINEL (-2147483646)
bool rt_tlas_trace(RtRay ray, float t_max, bool any, inout RtHit hit, inout int steps) {
    hit.instance = -1;
    if (rt_tlas_count == 0) return false;
    int stack[RT_STACK];
    int sp = 0;
    bool found = false;
    if (rt_self_root >= 0 && (any ? !rt_skip_shadow_only : rt_hit_shadow_only != 0)) {
        stack[sp] = rt_self_root; sp++;
    }
    int cur = 0; // the world before the puppet
    // inside a triangle instance's BLAS: its local ray, t6 and world_to_local rows
    bool in_blas = false;
    RtRay local = ray;
    int b_inst = -1;
    vec4 b_info = vec4(0.0), w0 = vec4(0.0), w1 = vec4(0.0), w2 = vec4(0.0);
    bool b_double = false, b_alpha = false;
    for (int iter = 0; iter < RT_MAX_STEPS; iter++) {
        if (cur == RT_NONE) {
            if (sp == 0) break;
            sp--;
            cur = stack[sp];
            if (cur == RT_SENTINEL) { in_blas = false; cur = RT_NONE; continue; } // back to the TLAS
        }
        steps++;
        if (cur >= 0) {
            int near, far;
            float tn, tf;
            int c = in_blas ? rt_node_visit(rt_blas, cur, local, t_max, near, tn, far, tf)
                            : rt_node_visit(rt_tlas, cur, ray, t_max, near, tn, far, tf);
            if (c == 2 && sp < RT_STACK) { stack[sp] = far; sp++; }
            cur = near;
            continue;
        }
        int code = -cur - 1;
        int first = code >> RT_SLOT_COUNT_BITS;
        int count = code & RT_SLOT_COUNT_MASK;
        cur = RT_NONE;
        if (in_blas) {
            for (int i = 0; i < count; i++) {
                RtLocalHit lh;
                lh.t = t_max;
                lh.normal = vec3(0.0, 1.0, 0.0);
                lh.triangle = -1;
                if (!rt_tri(first + i, local, t_max, b_double, lh)) continue;
                // triangles have no atlas UVs: an alpha-tested triangle instance occludes as solid (rt_alpha_passes)
                t_max = lh.t;
                rt_record(hit, b_inst, b_info, lh, w0, w1, w2, ray, !any || b_alpha);
                found = true;
                if (any) return true;
            }
            continue;
        }
        for (int i = 0; i < count; i++) {
            int inst = first + i;
            vec4 info = rt_fetch(rt_inst, inst * 8 + 6);
            int flags = int(info.w);
            // any-hit: casters only; closest hit: never the shadow proxy (you cast a shadow but no reflection
            // or GI ray ever sees your own body, the viewmodel's included), nor
            // transparent instances when rt_skip_transparent
            if (any ? (!rt_occluder(flags) || (rt_skip_shadow_only && (flags & RT_INST_SHADOW_ONLY) != 0))
                    : (((flags & RT_INST_SHADOW_ONLY) != 0 && rt_hit_shadow_only == 0) || (rt_skip_transparent && (flags & RT_INST_TRANSPARENT) != 0))) continue;
            int base = inst * 8;
            vec4 r0 = rt_fetch(rt_inst, base), r1 = rt_fetch(rt_inst, base + 1), r2 = rt_fetch(rt_inst, base + 2);
            vec4 wo = vec4(ray.o, 1.0), wd = vec4(ray.d, 0.0);
            RtRay l = rt_ray(vec3(dot(r0, wo), dot(r1, wo), dot(r2, wo)), vec3(dot(r0, wd), dot(r1, wd), dot(r2, wd)));
            int kind = int(info.x);
            bool double_sided = (flags & RT_INST_DOUBLE_SIDED) != 0;
            bool alpha = any && (flags & RT_INST_ALPHA_TEST) != 0;
            if (kind == RT_KIND_TRIANGLES) {
                if (sp + 2 > RT_STACK) continue; // no room (as a full stack drops a subtree)
                if (i + 1 < count) { stack[sp] = -(((first + i + 1) << RT_SLOT_COUNT_BITS) | (count - i - 1)) - 1; sp++; }
                stack[sp] = RT_SENTINEL; sp++;
                in_blas = true;
                local = l;
                b_inst = inst; b_info = info; w0 = r0; w1 = r1; w2 = r2;
                b_double = double_sided; b_alpha = alpha;
                cur = int(info.y);
                break;
            }
            RtLocalHit lh;
            lh.t = t_max;
            lh.normal = vec3(0.0, 1.0, 0.0);
            lh.triangle = -1;
            bool ok;
            if (kind == RT_KIND_BOX) ok = rt_box(l, t_max, lh);
            else if (kind == RT_KIND_PLANE) ok = rt_plane(l, t_max, double_sided, lh);
            else ok = rt_ramp(l, t_max, lh);
            if (!ok) continue;
            rt_record(hit, inst, info, lh, r0, r1, r2, ray, !any || alpha);
            if (alpha && rt_alpha_passes(hit)) continue; // through a cut-out: t_max untouched
            t_max = lh.t;
            found = true;
            if (any) return true;
        }
    }
    return found;
}

bool rt_occluded(vec3 origin, vec3 dir, float t_max, inout int steps) {
    RtHit hit;
    return rt_tlas_trace(rt_ray(origin, dir), t_max, true, hit, steps);
}

// occlusion query that also reports the first occluder's distance (any hit, so not the nearest:
// good enough for the AO falloff, which only needs "something close" vs "something far")
bool rt_occluded_t(vec3 origin, vec3 dir, float t_max, out float t_hit, inout int steps) {
    RtHit hit;
    hit.t = t_max;
    bool blocked = rt_tlas_trace(rt_ray(origin, dir), t_max, true, hit, steps);
    t_hit = blocked ? hit.t : t_max;
    return blocked;
}

bool rt_closest(vec3 origin, vec3 dir, float t_max, out RtHit hit, inout int steps) {
    hit.t = t_max;
    hit.instance = -1;
    hit.triangle = -1;
    hit.material = 0;
    hit.flags = 0;
    hit.normal = vec3(0.0, 1.0, 0.0);
    hit.position = origin;
    return rt_tlas_trace(rt_ray(origin, dir), t_max, false, hit, steps);
}

// ---- per-pixel noise: a small integer hash (no texture), decorrelated by the frame index
float rt_hash(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return float(v.x) * (1.0 / 4294967296.0);
}

// orthonormal basis around n (Frisvad / Duff et al.)
void rt_basis(vec3 n, out vec3 t, out vec3 b) {
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float c = n.x * n.y * a;
    t = vec3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = vec3(c, s + n.y * n.y * a, -n.y);
}
