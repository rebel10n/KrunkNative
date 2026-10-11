// The prize wheel's canvas and the unboxing confetti (see client/include/spin_wheel.h).
// GL state: everything this pass changes (program, VAO/VBO, blend, depth test, colour mask,
// viewport) is set explicitly and restored to the defaults the page/world passes expect.
#include <gl.h>
#include <client.h>
#include <spin_wheel.h>
#include <html_dom.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHADOW_SIGMA 10.0f    // shadowBlur 20 -> Gaussian sigma = blur / 2
#define CONFETTI_MAX 512
#define CONFETTI_TICK (1.0 / 60.0) // canvas-confetti steps at most once per 1000/60 ms
#define CONFETTI_SEGMENTS 12       // circles: ellipse() as a fan
#define VERTEX_FLOATS 8            // pos.xy, canvas.xy, rgba

typedef struct {
    float x, y, wobble, wobble_speed, velocity, angle2d, tilt_angle, tilt_sin, tilt_cos;
    float wobble_x, wobble_y, random;
    float rgb[3];
    int circle, tick, total_ticks;
} Confetto;

static struct {
    unsigned program, vao, vbo;
    int failed;
    int u_mode, u_center, u_radius, u_slice_rotation, u_slice_count, u_slice_from, u_slice_to, u_slice_rgb;
    int u_pointer_rotation, u_pointer_scale, u_pointer_hover, u_shadow_sigma;
    Confetto confetti[CONFETTI_MAX];
    int confetti_count;
    double confetti_time; // glfwGetTime of the last confetti step
    float *verts;
    size_t verts_cap;
} g_sw;

static float frand(void) { return (float) rand() / ((float) RAND_MAX + 1.0f); }

static int ensure_program(void) {
    if (g_sw.program) return 1;
    if (g_sw.failed) return 0;
    char *vert = concat(assets_path(), "shaders/spin_wheel.vert");
    char *frag = concat(assets_path(), "shaders/spin_wheel.frag");
    g_sw.program = vert && frag ? shader_compile(vert, frag) : 0;
    free(vert);
    free(frag);
    if (!g_sw.program) {
        g_sw.failed = 1;
        printf("[spin] wheel shader failed to compile\n");
        return 0;
    }
    const unsigned p = g_sw.program;
    g_sw.u_mode = glGetUniformLocation(p, "mode");
    g_sw.u_center = glGetUniformLocation(p, "center");
    g_sw.u_radius = glGetUniformLocation(p, "radius");
    g_sw.u_slice_rotation = glGetUniformLocation(p, "slice_rotation");
    g_sw.u_slice_count = glGetUniformLocation(p, "slice_count");
    g_sw.u_slice_from = glGetUniformLocation(p, "slice_from");
    g_sw.u_slice_to = glGetUniformLocation(p, "slice_to");
    g_sw.u_slice_rgb = glGetUniformLocation(p, "slice_rgb");
    g_sw.u_pointer_rotation = glGetUniformLocation(p, "pointer_rotation");
    g_sw.u_pointer_scale = glGetUniformLocation(p, "pointer_scale");
    g_sw.u_pointer_hover = glGetUniformLocation(p, "pointer_hover");
    g_sw.u_shadow_sigma = glGetUniformLocation(p, "shadow_sigma");

    glGenVertexArrays(1, &g_sw.vao);
    glGenBuffers(1, &g_sw.vbo);
    glBindVertexArray(g_sw.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_sw.vbo);
    const GLsizei stride = VERTEX_FLOATS * sizeof(float);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void *) 0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *) (2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void *) (4 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return 1;
}

static float *verts_reserve(const size_t count) {
    const size_t need = count * VERTEX_FLOATS;
    if (need > g_sw.verts_cap) {
        float *grown = realloc(g_sw.verts, need * sizeof(float));
        if (!grown) return NULL;
        g_sw.verts = grown;
        g_sw.verts_cap = need;
    }
    return g_sw.verts;
}

// CSS px -> clip space. The page is laid out in CSS px over the whole framebuffer
// (natively CSS px == window px, on the web the canvas is CSS px * devicePixelRatio).
typedef struct { float sx, sy; } CssToClip;

static CssToClip css_to_clip(void) {
    float vw = 0, vh = 0, dpr = 1;
    html_dom_viewport(&vw, &vh, &dpr);
    CssToClip m = {vw > 0 ? 2.0f / vw : 0.0f, vh > 0 ? 2.0f / vh : 0.0f};
    return m;
}

static void put_vertex(float *v, const CssToClip m, const float x, const float y, const float cx, const float cy, const float *rgba) {
    v[0] = x * m.sx - 1.0f;
    v[1] = 1.0f - y * m.sy;
    v[2] = cx;
    v[3] = cy;
    v[4] = rgba[0];
    v[5] = rgba[1];
    v[6] = rgba[2];
    v[7] = rgba[3];
}

static void pass_begin(Client *client) {
    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(client->window, &fb_w, &fb_h);
    glViewport(0, 0, fb_w, fb_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    // leave the destination alpha alone: on the web the browser composites the GL canvas's
    // alpha with the page (see class_preview.c)
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glUseProgram(g_sw.program);
    glBindVertexArray(g_sw.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_sw.vbo);
}

static void pass_end(void) {
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void spin_wheel_render(Client *client) {
    const SpinWheelDraw *d = store_ui_wheel_draw();
    if (!d || !d->visible || d->w <= 0 || d->h <= 0 || d->canvas_w <= 0 || d->canvas_h <= 0 || d->radius <= 0) return;
    if (!ensure_program()) return;
    float *v = verts_reserve(6);
    if (!v) return;
    const CssToClip m = css_to_clip();
    static const float white[4] = {1, 1, 1, 1};
    // two triangles over the canvas element; canvas px run 0..w / 0..h across it
    const float xs[6] = {0, 1, 1, 0, 1, 0}, ys[6] = {0, 0, 1, 0, 1, 1};
    for (int i = 0; i < 6; i++) {
        put_vertex(v + i * VERTEX_FLOATS, m, d->x + xs[i] * d->w, d->y + ys[i] * d->h, xs[i] * d->canvas_w, ys[i] * d->canvas_h, white);
    }
    pass_begin(client);
    glUniform1i(g_sw.u_mode, 0);
    glUniform2f(g_sw.u_center, d->canvas_w / 2.0f + 2.0f, d->canvas_h / 2.0f + 2.0f); // spinCanvasContext.translate(2, 2)
    glUniform1f(g_sw.u_radius, d->radius);
    glUniform1f(g_sw.u_slice_rotation, d->slice_rotation);
    glUniform1i(g_sw.u_slice_count, d->slices);
    glUniform1fv(g_sw.u_slice_from, SPIN_WHEEL_MAX_SLICES, d->slice_from);
    glUniform1fv(g_sw.u_slice_to, SPIN_WHEEL_MAX_SLICES, d->slice_to);
    glUniform3fv(g_sw.u_slice_rgb, SPIN_WHEEL_MAX_SLICES, &d->slice_rgb[0][0]);
    glUniform1f(g_sw.u_pointer_rotation, d->pointer_rotation);
    glUniform1f(g_sw.u_pointer_scale, d->pointer_scale);
    glUniform1f(g_sw.u_pointer_hover, d->pointer_hover);
    glUniform1f(g_sw.u_shadow_sigma, SHADOW_SIGMA);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (6 * VERTEX_FLOATS * sizeof(float)), v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    pass_end();
}

// ------------------------------------------------------------------ confetti (canvas-confetti 1.9.3)

static int hex_rgb(const char *hex, float *rgb) {
    if (!hex) return 0;
    if (*hex == '#') hex++;
    unsigned value = 0;
    if (strlen(hex) == 3) {
        unsigned r, g, b;
        if (sscanf(hex, "%1x%1x%1x", &r, &g, &b) != 3) return 0;
        value = (r * 17) << 16 | (g * 17) << 8 | (b * 17);
    } else if (sscanf(hex, "%6x", &value) != 1) {
        return 0;
    }
    rgb[0] = (float) ((value >> 16) & 255) / 255.0f;
    rgb[1] = (float) ((value >> 8) & 255) / 255.0f;
    rgb[2] = (float) (value & 255) / 255.0f;
    return 1;
}

void spin_wheel_confetti(const int particle_count, const char *const *hex_colors, const int color_count) {
    float vw = 0, vh = 0, dpr = 1;
    html_dom_viewport(&vw, &vh, &dpr);
    if (g_sw.confetti_count == 0) g_sw.confetti_time = glfwGetTime();
    // defaults: angle 90, startVelocity 45, decay 0.9, gravity 1, ticks 200, scalar 1,
    // shapes square + circle; this call: spread 360, origin (0.5, 0.5)
    const float angle = (float) M_PI / 2.0f, spread = 2.0f * (float) M_PI;
    for (int i = 0; i < particle_count && g_sw.confetti_count < CONFETTI_MAX; i++) {
        Confetto *c = &g_sw.confetti[g_sw.confetti_count++];
        memset(c, 0, sizeof(*c));
        c->x = vw * 0.5f;
        c->y = vh * 0.5f;
        c->wobble = 10.0f * frand();
        c->wobble_speed = fminf(0.11f, 0.1f * frand() + 0.05f);
        c->velocity = 0.5f * 45.0f + frand() * 45.0f;
        c->angle2d = -angle + (0.5f * spread - frand() * spread);
        c->tilt_angle = (0.5f * frand() + 0.25f) * (float) M_PI;
        c->random = frand() + 2.0f;
        c->total_ticks = 200;
        c->circle = frand() < 0.5f;
        if (!color_count || !hex_rgb(hex_colors[i % color_count], c->rgb)) c->rgb[0] = c->rgb[1] = c->rgb[2] = 1.0f;
    }
}

void spin_wheel_confetti_reset(void) { g_sw.confetti_count = 0; }

static void confetti_step(void) {
    int alive = 0;
    for (int i = 0; i < g_sw.confetti_count; i++) {
        Confetto *c = &g_sw.confetti[i];
        c->x += cosf(c->angle2d) * c->velocity;
        c->y += sinf(c->angle2d) * c->velocity + 3.0f; // gravity * 3
        c->velocity *= 0.9f;
        c->wobble += c->wobble_speed;
        c->wobble_x = c->x + 10.0f * cosf(c->wobble);
        c->wobble_y = c->y + 10.0f * sinf(c->wobble);
        c->tilt_angle += 0.1f;
        c->tilt_sin = sinf(c->tilt_angle);
        c->tilt_cos = cosf(c->tilt_angle);
        c->random = frand() + 2.0f;
        if (++c->tick < c->total_ticks) g_sw.confetti[alive++] = *c;
    }
    g_sw.confetti_count = alive;
}

void spin_wheel_render_confetti(Client *client) {
    if (!g_sw.confetti_count) return;
    const double now = glfwGetTime();
    int steps = 0;
    while (now - g_sw.confetti_time >= CONFETTI_TICK && steps < 8) {
        g_sw.confetti_time += CONFETTI_TICK;
        confetti_step();
        steps++;
    }
    if (now - g_sw.confetti_time >= CONFETTI_TICK) g_sw.confetti_time = now; // fell behind: drop time
    if (!g_sw.confetti_count || !ensure_program()) return;
    const int per = CONFETTI_SEGMENTS * 3; // a square uses 6 of these
    float *v = verts_reserve((size_t) g_sw.confetti_count * per);
    if (!v) return;
    const CssToClip m = css_to_clip();
    size_t n = 0;
    for (int i = 0; i < g_sw.confetti_count; i++) {
        const Confetto *c = &g_sw.confetti[i];
        // the frame drawn is the state after the step, progress = tick / totalTicks (pre-increment)
        const float rgba[4] = {c->rgb[0], c->rgb[1], c->rgb[2], 1.0f - (float) (c->tick - 1) / (float) c->total_ticks};
        const float x1 = c->x + c->random * c->tilt_cos, y1 = c->y + c->random * c->tilt_sin;
        const float x2 = c->wobble_x + c->random * c->tilt_cos, y2 = c->wobble_y + c->random * c->tilt_sin;
        if (c->circle) {
            // ellipse(x, y, |x2 - x1| * 0.6, |y2 - y1| * 0.6, PI / 10 * wobble, 0, 2 PI)
            const float rx = fabsf(x2 - x1) * 0.6f, ry = fabsf(y2 - y1) * 0.6f, rot = (float) M_PI / 10.0f * c->wobble;
            const float cr = cosf(rot), sr = sinf(rot);
            for (int s = 0; s < CONFETTI_SEGMENTS; s++) {
                const float a0 = (float) s / CONFETTI_SEGMENTS * 2.0f * (float) M_PI;
                const float a1 = (float) (s + 1) / CONFETTI_SEGMENTS * 2.0f * (float) M_PI;
                const float px0 = rx * cosf(a0), py0 = ry * sinf(a0), px1 = rx * cosf(a1), py1 = ry * sinf(a1);
                put_vertex(v + n++ * VERTEX_FLOATS, m, c->x, c->y, 0, 0, rgba);
                put_vertex(v + n++ * VERTEX_FLOATS, m, c->x + px0 * cr - py0 * sr, c->y + px0 * sr + py0 * cr, 0, 0, rgba);
                put_vertex(v + n++ * VERTEX_FLOATS, m, c->x + px1 * cr - py1 * sr, c->y + px1 * sr + py1 * cr, 0, 0, rgba);
            }
        } else {
            // moveTo(x, y) lineTo(wobbleX, y1) lineTo(x2, y2) lineTo(x1, wobbleY), floored
            const float qx[4] = {floorf(c->x), floorf(c->wobble_x), floorf(x2), floorf(x1)};
            const float qy[4] = {floorf(c->y), floorf(y1), floorf(y2), floorf(c->wobble_y)};
            static const int tri[6] = {0, 1, 2, 0, 2, 3};
            for (int k = 0; k < 6; k++) put_vertex(v + n++ * VERTEX_FLOATS, m, qx[tri[k]], qy[tri[k]], 0, 0, rgba);
        }
    }
    pass_begin(client);
    glUniform1i(g_sw.u_mode, 1);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (n * VERTEX_FLOATS * sizeof(float)), v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei) n);
    pass_end();
}

void spin_wheel_fini(void) {
    if (g_sw.program) glDeleteProgram(g_sw.program);
    if (g_sw.vbo) glDeleteBuffers(1, &g_sw.vbo);
    if (g_sw.vao) glDeleteVertexArrays(1, &g_sw.vao);
    free(g_sw.verts);
    memset(&g_sw, 0, sizeof(g_sw));
}
