// v10.0.4 warmScene's stop rule for KN's renderer (client/include/scene_warm_rule.h; the Scene glue and the model
// preload: scene_warm_preload.c).
#include <scene_warm_rule.h>
#include <stdlib.h>
#include <string.h>

static struct {
    int active, frames, stable;
    size_t last_meshes;
    double until;
} g_warm;

int scene_warm_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("KRUNKNATIVE_SCENE_WARM");
        on = !(v && !strcmp(v, "0"));
    }
    return on;
}

void scene_warm_begin_count(const size_t mesh_count, const double now_ms) {
    memset(&g_warm, 0, sizeof(g_warm));
    if (!scene_warm_enabled()) return;
    g_warm.active = 1;
    g_warm.until = now_ms + SCENE_WARM_MAX_MS;
    g_warm.last_meshes = mesh_count;
}

int scene_warm_frame_count(const size_t mesh_count, const double now_ms) {
    if (!g_warm.active) return 0;
    if (now_ms > g_warm.until) {
        g_warm.active = 0;
        return 0;
    }
    // a model that arrives (a GLB, a fetched asset) adds meshes: the scene is stable once no frame added any
    g_warm.stable = mesh_count == g_warm.last_meshes ? g_warm.stable + 1 : 0;
    g_warm.last_meshes = mesh_count;
    g_warm.frames++;
    if (g_warm.stable > SCENE_WARM_STABLE) { // every mesh has drawn uncull'd on the last SCENE_WARM_STABLE frames
        g_warm.active = 0;
        return 0;
    }
    return 1;
}

int scene_warm_active(void) {
    return g_warm.active;
}
