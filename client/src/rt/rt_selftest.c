// `client_bin --rt-selftest <map.json>`: the CPU BVH tracer (rt_scene.c) against brute force over
// the same instances, on the loaded offline map's world scene. Random rays through the map's box,
// plus rays from surfaces found along the sun direction (the shadow rays the GPU will fire).
// Prints the structure sizes, build / rebuild times, the mismatches and the BVH rays/s; exit 0
// iff nothing mismatched.
#include <rt.h>
#include <gltf.h>
#include <GLFW/glfw3.h> // glfwGetTime (monotonic, cross-platform)
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RANDOM_RAYS 20000
#define SURFACE_RAYS 2000
#define TIMING_RAYS 100000
#define MISMATCH_PRINT 10
#define REBUILD_FRAMES 20
#define MOVED_MESHES 20     // the perturbation pass: meshes nudged into the dynamic half
#define MOVED_RAYS 5000

static uint64_t g_rng = 0x9e3779b97f4a7c15ULL;
static float rnd(void) { // xorshift64*, [0, 1)
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return (float) ((g_rng * 0x2545f4914f6cdd1dULL) >> 40) / (float) (1 << 24);
}
static void rnd_dir(float d[3]) { // uniform on the sphere
    const float z = 2.0f * rnd() - 1.0f, a = 2.0f * (float) M_PI * rnd(), r = sqrtf(1.0f - z * z);
    d[0] = r * cosf(a); d[1] = r * sinf(a); d[2] = z;
}

typedef struct {
    int rays, hit_mismatch, t_mismatch, occl_mismatch, printed;
} Tally;

// brute occlusion: the BVH's occluder filter over every instance, one at a time (rt_scene_trace_brute
// over a one-instance view shares the BLAS / triangle tables)
static int brute_occluded(const RtScene *rt, const float o[3], const float d[3], const float t_max) {
    RtScene one = *rt;
    one.instance_count = 1;
    for (int i = 0; i < rt->instance_count; i++) {
        const RtInstance *inst = &rt->instances[i];
        if (!(inst->flags & RT_INST_CASTS_SHADOW) || (inst->flags & RT_INST_TRANSPARENT)) continue;
        one.instances = &rt->instances[i];
        if (rt_scene_trace_brute(&one, o, d, t_max, NULL)) return 1;
    }
    return 0;
}

static void check_ray(const RtScene *rt, const float o[3], const float d[3], const float t_max, Tally *tally) {
    RtHit a, b;
    const int ha = rt_scene_trace(rt, o, d, t_max, &a);
    const int hb = rt_scene_trace_brute(rt, o, d, t_max, &b);
    tally->rays++;
    int bad = 0;
    const char *what = "";
    if (ha != hb) { bad = 1; what = "hit/miss"; tally->hit_mismatch++; }
    else if (ha) {
        const float tol = 1e-3f * fmaxf(1.0f, fabsf(b.t));
        if (fabsf(a.t - b.t) > tol) { bad = 1; what = "distance"; tally->t_mismatch++; }
        // a different instance at the same distance is a coplanar tie: either is right
    }
    const int oa = rt_scene_occluded(rt, o, d, t_max), ob = brute_occluded(rt, o, d, t_max);
    if (oa != ob) { bad = 1; what = *what ? "hit and occlusion" : "occlusion"; tally->occl_mismatch++; }
    if (bad && tally->printed < MISMATCH_PRINT) {
        tally->printed++;
        printf("  mismatch (%s): o (%.3f %.3f %.3f) d (%.4f %.4f %.4f): bvh %s inst %d t %.4f, brute %s inst %d t %.4f, occluded bvh %d brute %d\n",
               what, o[0], o[1], o[2], d[0], d[1], d[2], ha ? "hit" : "miss", ha ? a.instance : -1, ha ? a.t : 0.0f,
               hb ? "hit" : "miss", hb ? b.instance : -1, hb ? b.t : 0.0f, oa, ob);
        if (ha) {
            const RtInstance *inst = &rt->instances[a.instance];
            printf("    bvh instance kind %d blas %d flags 0x%x aabb (%.2f %.2f %.2f)-(%.2f %.2f %.2f)\n", inst->kind, inst->blas, inst->flags,
                   inst->aabb_min[0], inst->aabb_min[1], inst->aabb_min[2], inst->aabb_max[0], inst->aabb_max[1], inst->aabb_max[2]);
        }
        if (hb && (!ha || b.instance != a.instance)) {
            const RtInstance *inst = &rt->instances[b.instance];
            printf("    brute instance kind %d blas %d flags 0x%x aabb (%.2f %.2f %.2f)-(%.2f %.2f %.2f)\n", inst->kind, inst->blas, inst->flags,
                   inst->aabb_min[0], inst->aabb_min[1], inst->aabb_min[2], inst->aabb_max[0], inst->aabb_max[1], inst->aabb_max[2]);
        }
    }
}

// 0 when every node's box holds its triangles (leaves) / its children (inner nodes), with a small tolerance.
static int blas_check(const RtScene *rt, const RtBlas *blas) {
    int bad = 0;
    for (int n = 0; n < blas->node_count; n++) {
        const RtNode *node = &blas->nodes[n];
        float min[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, max[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        if (node->a < 0) {
            for (int i = 0; i < node->b; i++) {
                const float *v = &rt->tris[(size_t) (blas->tri_first + (-node->a - 1) + i) * 9];
                for (int k = 0; k < 9; k++) { if (v[k] < min[k % 3]) min[k % 3] = v[k]; if (v[k] > max[k % 3]) max[k % 3] = v[k]; }
            }
        } else {
            const RtNode *c[2] = {&blas->nodes[node->a], &blas->nodes[node->b]};
            for (int j = 0; j < 2; j++) for (int k = 0; k < 3; k++) { if (c[j]->aabb_min[k] < min[k]) min[k] = c[j]->aabb_min[k]; if (c[j]->aabb_max[k] > max[k]) max[k] = c[j]->aabb_max[k]; }
        }
        for (int k = 0; k < 3; k++) if (min[k] < node->aabb_min[k] - 1e-4f || max[k] > node->aabb_max[k] + 1e-4f) { bad++; break; }
    }
    return bad;
}

// A skinned, animated glTF (a pet) in a scene of its own: gltf.c's pose upload must move the geometry's
// tri_positions / tri_epoch, and the next build must refit that BLAS (serial moved, boxes hold the
// triangles, the root box differs from the bind pose's). 0 = ok; a missing model is reported, not a failure.
#define SKINNED_MODEL "models/pets/pet_19.glb"
static int skinned_refit_check(void) {
    char *path = concat(assets_path(), SKINNED_MODEL);
    GltfInstance *instance = path ? gltf_instance_create(gltf_model_load(path), 1.0f) : NULL;
    free(path);
    if (!instance) { printf("[rt-selftest] skinned refit: FAIL, %s not loadable here\n", SKINNED_MODEL); return 1; } // every part of the test must run
    Scene *scene = scene_init();
    scene_add_gltf(scene, instance);
    gltf_instance_update(instance, 0.0);
    RtScene rt;
    memset(&rt, 0, sizeof(rt));
    rt_scene_build(&rt, scene, NULL);
    int tri_blas = -1;
    float root_min[3] = {0}, root_max[3] = {0};
    for (int b = 0; b < rt.blas_count; b++) if (rt.blas[b].geometry && rt.blas[b].tri_count > tri_blas) { tri_blas = b; }
    if (tri_blas >= 0) { memcpy(root_min, rt.blas[tri_blas].nodes[0].aabb_min, sizeof(root_min)); memcpy(root_max, rt.blas[tri_blas].nodes[0].aabb_max, sizeof(root_max)); }
    gltf_instance_update(instance, 0.7); // mid-clip
    rt_scene_build(&rt, scene, NULL);
    int bad = 0;
    if (tri_blas < 0 || !rt.instance_count) bad++;
    else {
        const RtBlas *blas = &rt.blas[tri_blas];
        const int moved = memcmp(root_min, blas->nodes[0].aabb_min, sizeof(root_min)) || memcmp(root_max, blas->nodes[0].aabb_max, sizeof(root_max));
        const int checked = blas_check(&rt, blas);
        if (!blas->refit_serial || !moved || checked) bad++;
        printf("[rt-selftest] skinned refit (%s, %d triangles, %d instances): serial %u, root box %s, boxes %s: %s\n", SKINNED_MODEL, blas->tri_count,
               rt.instance_count, blas->refit_serial, moved ? "moved" : "did not move", checked ? "FAIL" : "hold the triangles", bad ? "FAIL" : "ok");
    }
    if (bad && tri_blas < 0) printf("[rt-selftest] skinned refit: FAIL, no triangle BLAS for %s\n", SKINNED_MODEL);
    rt_scene_free(&rt);
    scene_remove_gltf(scene, instance);
    gltf_instance_fini(instance);
    scene_fini(scene);
    return bad;
}

int rt_selftest(const Scene *scene, const char *map_path) {
    RtScene rt;
    memset(&rt, 0, sizeof(rt));
    printf("[rt-selftest] %s: %zu scene meshes\n", map_path ? map_path : "(map)", scene->mesh_count);

    double t0 = glfwGetTime();
    rt_scene_build(&rt, scene, NULL);
    const double build_ms = (glfwGetTime() - t0) * 1000.0;
    t0 = glfwGetTime();
    for (int i = 0; i < REBUILD_FRAMES; i++) rt_scene_build(&rt, scene, NULL); // the static half stands: the per-frame cost
    const double rebuild_ms = (glfwGetTime() - t0) * 1000.0 / REBUILD_FRAMES;

    int blas_nodes = 0, kinds[4] = {0};
    for (int i = 0; i < rt.blas_count; i++) blas_nodes += rt.blas[i].node_count;
    for (int i = 0; i < rt.instance_count; i++) if (rt.instances[i].kind >= 0 && rt.instances[i].kind < 4) kinds[rt.instances[i].kind]++;
    printf("[rt-selftest] instances %d (box %d plane %d ramp %d triangles %d; static %d dynamic %d), skipped %d, blas %d (%d nodes, %d triangles), tlas nodes %d (static %d), materials %d\n",
           rt.instance_count, kinds[0], kinds[1], kinds[2], kinds[3], rt.static_instance_count, rt.instance_count - rt.static_instance_count,
           rt.skipped, rt.blas_count, blas_nodes, rt.tri_count, rt.tlas_count, rt.static_tlas_count, rt.material_count);
    printf("[rt-selftest] first build %.2f ms (BLAS included), per-frame rebuild %.3f ms (static half standing, over %d frames)\n", build_ms, rebuild_ms, REBUILD_FRAMES);
    if (!rt.instance_count || !rt.tlas_count) {
        printf("[rt-selftest] FAIL: nothing to trace\n");
        rt_scene_free(&rt);
        return 1;
    }

    float min[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, max[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (int i = 0; i < rt.instance_count; i++) {
        for (int k = 0; k < 3; k++) {
            if (rt.instances[i].aabb_min[k] < min[k]) min[k] = rt.instances[i].aabb_min[k];
            if (rt.instances[i].aabb_max[k] > max[k]) max[k] = rt.instances[i].aabb_max[k];
        }
    }
    const float extent[3] = {max[0] - min[0], max[1] - min[1], max[2] - min[2]};
    const float t_max = 2.0f * sqrtf(extent[0] * extent[0] + extent[1] * extent[1] + extent[2] * extent[2]);
    printf("[rt-selftest] world box (%.1f %.1f %.1f)-(%.1f %.1f %.1f)\n", min[0], min[1], min[2], max[0], max[1], max[2]);

    Tally tally = {0};
    for (int i = 0; i < RANDOM_RAYS; i++) {
        float o[3], d[3];
        for (int k = 0; k < 3; k++) o[k] = min[k] + rnd() * extent[k];
        rnd_dir(d);
        check_ray(&rt, o, d, t_max, &tally);
    }
    const int random_rays = tally.rays;

    // shadow rays: surfaces found along the sun direction, then from the surface towards and away from it
    float sun[3] = {g_render_lighting.light_direction.x, g_render_lighting.light_direction.y, g_render_lighting.light_direction.z};
    const float sun_len = sqrtf(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
    if (sun_len > 0.0f) for (int k = 0; k < 3; k++) sun[k] /= sun_len;
    else { sun[0] = 0.0f; sun[1] = 1.0f; sun[2] = 0.0f; }
    int surface_rays = 0, tries = 0;
    while (surface_rays < SURFACE_RAYS && tries < SURFACE_RAYS * 20) {
        tries++;
        float o[3], d[3] = {-sun[0], -sun[1], -sun[2]};
        for (int k = 0; k < 3; k++) o[k] = min[k] + rnd() * extent[k];
        RtHit hit;
        if (!rt_scene_trace_brute(&rt, o, d, t_max, &hit)) continue;
        float p[3];
        for (int k = 0; k < 3; k++) p[k] = hit.position[k] + hit.normal[k] * 1e-2f;
        const float towards[3] = {sun[0], sun[1], sun[2]};
        check_ray(&rt, p, towards, t_max, &tally);
        float jitter[3];
        rnd_dir(jitter);
        float d2[3] = {sun[0] + 0.2f * jitter[0], sun[1] + 0.2f * jitter[1], sun[2] + 0.2f * jitter[2]};
        const float l = sqrtf(d2[0] * d2[0] + d2[1] * d2[1] + d2[2] * d2[2]);
        for (int k = 0; k < 3; k++) d2[k] /= l;
        check_ray(&rt, p, d2, t_max, &tally);
        surface_rays += 2;
    }

    // The split: nudge MOVED_MESHES meshes (non-map ones when the scene has any, else map objects), rebuild so they
    // land in the dynamic half and the static half is rebuilt without them, run the oracle again, then let them
    // settle (RT_STATIC_SETTLE_FRAMES + 1 unchanged builds) and check that map objects return to the static half.
    // Every non-map (dynamic) mesh, then static map objects spread over the half, up to MOVED_MESHES in all.
    const Mesh *moved[MOVED_MESHES];
    int moved_count = 0, moved_map = 0;
    for (int i = rt.static_instance_count; i < rt.instance_count && moved_count < MOVED_MESHES; i++) moved[moved_count++] = rt.instances[i].mesh;
    const int stride = rt.static_instance_count > MOVED_MESHES ? rt.static_instance_count / MOVED_MESHES : 1;
    for (int i = 0; i < rt.static_instance_count && moved_count < MOVED_MESHES; i += stride) { moved[moved_count++] = rt.instances[i].mesh; moved_map++; }
    for (int i = 0; i < moved_count; i++) ((Mesh *) moved[i])->transform.position.y += 0.37f;
    // ... and one triangle geometry's positions (a posed glTF does the same through gltf.c): its BLAS is refit
    Geometry *refit_geometry = NULL; // the biggest one
    for (int i = 0; i < rt.instance_count; i++) {
        Geometry *g = rt.instances[i].mesh->geometry;
        if (rt.instances[i].kind == RT_INST_TRIANGLES && g->tri_positions && (!refit_geometry || g->index_count > refit_geometry->index_count)) refit_geometry = g;
    }
    if (refit_geometry) {
        for (int v = 0; v < refit_geometry->tri_vertex_count; v++) refit_geometry->tri_positions[v * 3 + 1] += 0.5f;
        refit_geometry->tri_epoch++;
    }
    const int static_before = rt.static_instance_count;
    t0 = glfwGetTime();
    rt_scene_build(&rt, scene, NULL);
    const double moved_ms = (glfwGetTime() - t0) * 1000.0;
    Tally moved_tally = {0};
    for (int i = 0; i < MOVED_RAYS; i++) {
        float o[3], d[3];
        for (int k = 0; k < 3; k++) o[k] = min[k] + rnd() * extent[k];
        rnd_dir(d);
        check_ray(&rt, o, d, t_max, &moved_tally);
    }
    for (int i = 0; i < moved_count; i++) { // rays at the moved meshes themselves
        const RtInstance *inst = NULL;
        for (int j = 0; j < rt.instance_count; j++) if (rt.instances[j].mesh == moved[i]) { inst = &rt.instances[j]; break; }
        if (!inst) continue;
        for (int r = 0; r < 20; r++) {
            float o[3], d[3], target[3];
            for (int k = 0; k < 3; k++) { o[k] = min[k] + rnd() * extent[k]; target[k] = inst->aabb_min[k] + rnd() * (inst->aabb_max[k] - inst->aabb_min[k]); }
            for (int k = 0; k < 3; k++) d[k] = target[k] - o[k];
            const float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (l <= 0.0f) continue;
            for (int k = 0; k < 3; k++) d[k] /= l;
            check_ray(&rt, o, d, t_max, &moved_tally);
        }
    }
    const int moved_mismatches = moved_tally.hit_mismatch + moved_tally.t_mismatch + moved_tally.occl_mismatch;
    printf("[rt-selftest] moved %d meshes (%d map objects): static %d -> %d, dynamic %d, rebuild %.3f ms; %d rays: %d mismatches\n",
           moved_count, moved_map, static_before, rt.static_instance_count, rt.instance_count - rt.static_instance_count,
           moved_ms, moved_tally.rays, moved_mismatches);
    int refit_bad = 0;
    if (refit_geometry) { // the refit BLAS: every node box holds its triangles / children, and the serial moved
        int refit_seen = 0;
        for (int b = 0; b < rt.blas_count; b++) {
            if (rt.blas[b].geometry != refit_geometry) continue;
            refit_seen = rt.blas[b].refit_serial > 0;
            refit_bad += blas_check(&rt, &rt.blas[b]);
        }
        if (!refit_seen) refit_bad++;
        printf("[rt-selftest] refit geometry %p (%d triangles): %s\n", (const void *) refit_geometry, refit_geometry->index_count / 3,
               refit_bad ? "FAIL: boxes do not hold their triangles / no refit" : "boxes hold their triangles");
        for (int v = 0; v < refit_geometry->tri_vertex_count; v++) refit_geometry->tri_positions[v * 3 + 1] -= 0.5f;
        refit_geometry->tri_epoch++;
    }
    for (int i = 0; i <= RT_STATIC_SETTLE_FRAMES; i++) rt_scene_build(&rt, scene, NULL);
    const int settled_ok = rt.static_instance_count == static_before;
    printf("[rt-selftest] after %d still frames: static %d dynamic %d (%s)\n", RT_STATIC_SETTLE_FRAMES + 1, rt.static_instance_count,
           rt.instance_count - rt.static_instance_count, settled_ok ? "settled" : "FAIL: moved map objects did not return to the static half");
    Tally settled_tally = {0};
    for (int i = 0; i < MOVED_RAYS; i++) {
        float o[3], d[3];
        for (int k = 0; k < 3; k++) o[k] = min[k] + rnd() * extent[k];
        rnd_dir(d);
        check_ray(&rt, o, d, t_max, &settled_tally);
    }
    const int settled_mismatches = settled_tally.hit_mismatch + settled_tally.t_mismatch + settled_tally.occl_mismatch;
    for (int i = 0; i < moved_count; i++) ((Mesh *) moved[i])->transform.position.y -= 0.37f;

    t0 = glfwGetTime();
    int hits = 0;
    for (int i = 0; i < TIMING_RAYS; i++) {
        float o[3], d[3];
        for (int k = 0; k < 3; k++) o[k] = min[k] + rnd() * extent[k];
        rnd_dir(d);
        hits += rt_scene_trace(&rt, o, d, t_max, NULL);
    }
    const double trace_s = glfwGetTime() - t0;

    const int mismatches = tally.hit_mismatch + tally.t_mismatch + tally.occl_mismatch + moved_mismatches + settled_mismatches + !settled_ok + refit_bad;
    // the totals: every count here is a mismatch count (0 = good); the moved / settled mesh counts are printed above
    printf("[rt-selftest] %d random + %d surface rays: %d mismatches in all (hit/miss %d, distance %d, occlusion %d, moved-pass %d, settled-pass %d, settle %d, refit %d)\n",
           random_rays, surface_rays, mismatches, tally.hit_mismatch, tally.t_mismatch, tally.occl_mismatch, moved_mismatches, settled_mismatches,
           !settled_ok, refit_bad);
    printf("[rt-selftest] BVH closest hit: %.0f rays/s (%d of %d hit)\n", trace_s > 0.0 ? TIMING_RAYS / trace_s : 0.0, hits, TIMING_RAYS);
    rt_scene_free(&rt);
    const int skinned_bad = skinned_refit_check();
    printf("[rt-selftest] %s\n", mismatches || skinned_bad ? "FAIL" : "PASS");
    return mismatches || skinned_bad ? 1 : 0;
}
