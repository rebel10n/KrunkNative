// Per-pass GPU / CPU frame timing (gpu_timer.h). Each frame records a GL_TIMESTAMP query at its
// start and at every pass mark into one of GT_SLOTS query sets; a set is read back GT_SLOTS - 1
// frames later, and only when its last query is available, so the readback never waits on the GPU
// (a set still busy is dropped from the averages). GL_TIMESTAMP / glQueryCounter are desktop GL
// (3.3, ARB_timer_query); OpenGL ES 3.0 has no timer queries, so KN_GLES builds time the CPU only.
#include <gpu_timer.h>
#include <gl.h>
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GT_SLOTS 4
#define GT_MARKS (GPU_PASS_COUNT + 1)

static const char *const PASS_NAME[GPU_PASS_COUNT] = {
    "clear", "pre", "sky", "world", "vfx", "viewmodel", "post", "hud", "preview", "page",
};

typedef struct {
#ifndef KN_GLES
    GLuint query[GT_MARKS];
#endif
    int pass[GT_MARKS];      // pass ended by mark i (mark 0: the frame start)
    int count;               // marks recorded
    int pending;             // recorded, not read back yet
} GtSlot;

static struct {
    int state;               // 0 unknown, 1 on, -1 off
    int ready;               // queries created
    GtSlot slot[GT_SLOTS];
    int current;
    int in_frame;
    double cpu_mark;         // glfwGetTime() of the previous mark
    double cpu_sum[GPU_PASS_COUNT], gpu_sum[GPU_PASS_COUNT];
    int cpu_frames, gpu_frames, gpu_dropped;
    double last_print;
} G;

int gpu_timer_enabled(void) {
    if (!G.state) {
        const char *v = getenv("KRUNKNATIVE_GPU_TIMING");
        G.state = v && *v && strcmp(v, "0") ? 1 : -1;
    }
    return G.state > 0;
}

#ifndef KN_GLES
// The query set of a finished frame: accumulated when every timestamp is in, else dropped.
static void read_slot(GtSlot *s) {
    if (!s->pending) return;
    s->pending = 0;
    GLint available = 0;
    glGetQueryObjectiv(s->query[s->count - 1], GL_QUERY_RESULT_AVAILABLE, &available);
    if (!available) {
        G.gpu_dropped++;
        return;
    }
    GLuint64 prev = 0;
    for (int i = 0; i < s->count; i++) {
        GLuint64 t = 0;
        glGetQueryObjectui64v(s->query[i], GL_QUERY_RESULT, &t);
        if (i > 0 && t >= prev) G.gpu_sum[s->pass[i]] += (double) (t - prev) / 1e6;
        prev = t;
    }
    G.gpu_frames++;
}
#endif

static void record(const int pass) {
    GtSlot *s = &G.slot[G.current];
    if (s->count >= GT_MARKS) return;
    s->pass[s->count] = pass;
#ifndef KN_GLES
    glQueryCounter(s->query[s->count], GL_TIMESTAMP);
#endif
    s->count++;
}

void gpu_timer_frame_begin(void) {
    if (!gpu_timer_enabled()) return;
    if (!G.ready) {
#ifndef KN_GLES
        for (int i = 0; i < GT_SLOTS; i++) glGenQueries(GT_MARKS, G.slot[i].query);
#endif
        G.ready = 1;
        G.last_print = glfwGetTime();
    }
    G.current = (G.current + 1) % GT_SLOTS;
    GtSlot *s = &G.slot[G.current];
#ifndef KN_GLES
    read_slot(s); // GT_SLOTS - 1 frames old
#endif
    s->count = 0;
    s->pending = 0;
    G.in_frame = 1;
    G.cpu_mark = glfwGetTime();
    record(-1);
}

void gpu_timer_mark(const GpuPass pass) {
    if (!gpu_timer_enabled() || !G.in_frame || pass < 0 || pass >= GPU_PASS_COUNT) return;
    const double now = glfwGetTime();
    G.cpu_sum[pass] += (now - G.cpu_mark) * 1000.0;
    G.cpu_mark = now;
    record(pass);
}

static void print_line(void) {
    char gpu[512] = "", cpu[512] = "";
    double gpu_total = 0.0, cpu_total = 0.0;
    int gn = 0, cn = 0;
    for (int i = 0; i < GPU_PASS_COUNT; i++) {
        const double g = G.gpu_frames ? G.gpu_sum[i] / G.gpu_frames : 0.0;
        const double c = G.cpu_frames ? G.cpu_sum[i] / G.cpu_frames : 0.0;
        gpu_total += g;
        cpu_total += c;
        gn += snprintf(gpu + gn, sizeof(gpu) - (size_t) gn, "%s%s %.3f", i ? " " : "", PASS_NAME[i], g);
        cn += snprintf(cpu + cn, sizeof(cpu) - (size_t) cn, "%s%s %.3f", i ? " " : "", PASS_NAME[i], c);
    }
    fprintf(stderr, "[gpu] %d frames (%d read, %d dropped): gpu %.3f ms (%s) cpu %.3f ms (%s)\n", G.cpu_frames, G.gpu_frames,
            G.gpu_dropped, gpu_total, gpu, cpu_total, cpu);
    memset(G.cpu_sum, 0, sizeof(G.cpu_sum));
    memset(G.gpu_sum, 0, sizeof(G.gpu_sum));
    G.cpu_frames = G.gpu_frames = G.gpu_dropped = 0;
}

void gpu_timer_frame_end(void) {
    if (!gpu_timer_enabled() || !G.in_frame) return;
    G.in_frame = 0;
    GtSlot *s = &G.slot[G.current];
    s->pending = s->count > 1;
    G.cpu_frames++;
    const double now = glfwGetTime();
    if (now - G.last_print >= 1.0) {
        G.last_print = now;
        print_line();
    }
}
