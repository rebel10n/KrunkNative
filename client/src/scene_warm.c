// v10.0.4 warmScene for KN's renderer (client/include/scene_warm.h).
#include <scene_warm.h>
#include <stdlib.h>
#include <string.h>

#define SCENE_WARM_MAX_MS 4000.0 // 0xfa0: the model wait, the compile wait and the race all give up after 4 s
#define SCENE_WARM_STABLE 3      // frames without a new mesh (iíÎîíìí >= 3, :205042)

static struct {
    int active, frames, stable;
    size_t last_meshes;
    double until;
} g_warm;

static int warm_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("KRUNKNATIVE_SCENE_WARM");
        on = !(v && !strcmp(v, "0"));
    }
    return on;
}

void scene_warm_begin(const Scene *scene, const double now_ms) {
    memset(&g_warm, 0, sizeof(g_warm));
    if (!warm_enabled()) return;
    g_warm.active = 1;
    g_warm.until = now_ms + SCENE_WARM_MAX_MS;
    g_warm.last_meshes = scene ? scene->mesh_count : 0;
}

int scene_warm_frame(const Scene *scene, const double now_ms) {
    if (!g_warm.active) return 0;
    if (now_ms > g_warm.until) {
        g_warm.active = 0;
        return 0;
    }
    // a model that arrives (a GLB, a fetched asset) adds meshes: the scene is stable once no frame added any
    const size_t meshes = scene ? scene->mesh_count : 0;
    g_warm.stable = meshes == g_warm.last_meshes ? g_warm.stable + 1 : 0;
    g_warm.last_meshes = meshes;
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
