// The prize wheel's canvas and the unboxing confetti (see client/include/spin_wheel.h).
// GL state: everything this pass changes (program, VAO/VBO, blend, depth test, colour mask,
// viewport) is set explicitly and restored to the defaults the page/world passes expect.
#include <gl.h>
#include <client.h>
#include <spin_wheel.h>
#include <strike_ui.h>
#include <html_dom.h>
#include <cosmetics.h>
#include <fast_obj.h>
#include <gltf.h>
#include <skin_fx.h>
#include "../viewer/viewer_store.h"
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
    int u_pointer_rotation, u_pointer_count, u_pointer_scale, u_shadow_sigma;
    Confetto confetti[CONFETTI_MAX];
    int confetti_count;
    double confetti_time; // glfwGetTime of the last confetti step
    float *verts;
    size_t verts_cap;
    // Krunker Strike's SPIN canvas: the render target shown in the page's <canvas> (html_dom_canvas_gl)
    unsigned canvas_fbo, canvas_tex;
    int canvas_w, canvas_h;
} g_sw;

static float frand(void) { return (float) rand() / ((float) RAND_MAX + 1.0f); }
static void render_item(const SpinWheelDraw *d); // the won item (below)

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
    g_sw.u_pointer_count = glGetUniformLocation(p, "pointer_count");
    g_sw.u_pointer_scale = glGetUniformLocation(p, "pointer_scale");
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

static void set_wheel_uniforms(const SpinWheelDraw *d) {
    glUniform1i(g_sw.u_mode, 0);
    glUniform2f(g_sw.u_center, d->canvas_w / 2.0f + 2.0f, d->canvas_h / 2.0f + 2.0f); // spinCanvasContext.translate(2, 2)
    glUniform1f(g_sw.u_radius, d->radius);
    glUniform1f(g_sw.u_slice_rotation, d->slice_rotation);
    glUniform1i(g_sw.u_slice_count, d->slices);
    glUniform1fv(g_sw.u_slice_from, SPIN_WHEEL_MAX_SLICES, d->slice_from);
    glUniform1fv(g_sw.u_slice_to, SPIN_WHEEL_MAX_SLICES, d->slice_to);
    glUniform3fv(g_sw.u_slice_rgb, SPIN_WHEEL_MAX_SLICES, &d->slice_rgb[0][0]);
    glUniform1fv(g_sw.u_pointer_rotation, SPIN_WHEEL_MAX_POINTERS, d->pointer_rotation);
    glUniform1i(g_sw.u_pointer_count, d->pointer_count);
    glUniform1f(g_sw.u_pointer_scale, d->pointer_scale);
    glUniform1f(g_sw.u_shadow_sigma, SHADOW_SIGMA);
}

// Krunker Strike's SPIN (aue.render, the same 2D-canvas wheel as v7's with the centre at the middle of
// the canvas): drawn into a canvas_w x canvas_h render target that the page's own <canvas> shows
// (html_dom_canvas_gl), so the page paints it in its place: over the screen's .frvr-dark-blur, under the
// cost buttons. GL state: the framebuffer binding, viewport and clear colour are restored, the rest as
// pass_begin / pass_end leave it.
static void render_into_canvas(const SpinWheelDraw *d) {
    const int w = (int) d->canvas_w, h = (int) d->canvas_h;
    if (w <= 0 || h <= 0 || !ensure_program()) return;
    if (!g_sw.canvas_tex || g_sw.canvas_w != w || g_sw.canvas_h != h) {
        if (!g_sw.canvas_tex) {
            glGenTextures(1, &g_sw.canvas_tex);
            glGenFramebuffers(1, &g_sw.canvas_fbo);
        }
        glBindTexture(GL_TEXTURE_2D, g_sw.canvas_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // one level: the page's GL backend samples canvas images with a mipmap filter
        // (gl_backend.c GLB_IMAGE_MIN_FILTER), complete only when the texture says it has no others
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        g_sw.canvas_w = w;
        g_sw.canvas_h = h;
    }
    float *v = verts_reserve(6);
    if (!v) return;
    static const float white[4] = {1, 1, 1, 1};
    // the whole target; canvas px + 2 so that the shader's translate(2, 2) centre is the middle
    const float xs[6] = {0, 1, 1, 0, 1, 0}, ys[6] = {0, 0, 1, 0, 1, 1};
    const CssToClip unit = {2.0f, 2.0f};
    for (int i = 0; i < 6; i++) put_vertex(v + i * VERTEX_FLOATS, unit, xs[i], ys[i], xs[i] * d->canvas_w + 2.0f, ys[i] * d->canvas_h + 2.0f, white);
    GLint prev_fbo = 0, vp[4] = {0, 0, 0, 0};
    GLfloat clear[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    glBindFramebuffer(GL_FRAMEBUFFER, g_sw.canvas_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_sw.canvas_tex, 0);
    glViewport(0, 0, w, h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // premultiplied, as html_dom_canvas_gl wants it
    glUseProgram(g_sw.program);
    glBindVertexArray(g_sw.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_sw.vbo);
    set_wheel_uniforms(d);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (6 * VERTEX_FLOATS * sizeof(float)), v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    pass_end();
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prev_fbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    html_dom_canvas_gl((HtmlElem) d->canvas_elem, g_sw.canvas_tex, g_sw.canvas_fbo, w, h);
}

void spin_wheel_render(Client *client) {
    if (strike_ui_active()) { // the Strike page has its own wheel (strike_menu.c SPIN), no store_ui.c
        const SpinWheelDraw *s = strike_menu_wheel_draw();
        if (s && s->visible && s->canvas_elem) render_into_canvas(s);
        return;
    }
    const SpinWheelDraw *d = store_ui_wheel_draw();
    render_item(d); // the won item's canvas (the page paints it)
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
    set_wheel_uniforms(d);
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
        // canvas-confetti draws a piece only after its first update (updateFetti steps, then
        // draws): before it, wobbleX/wobbleY are 0 and its quad reaches the page's top-left
        // corner. A fast frame right after confetti() (no 1/60 s tick yet: NVIDIA, not
        // llvmpipe) drew every piece that way, as a huge white/rarity-coloured fan.
        if (c->tick == 0) continue;
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
    if (!n) return;
    pass_begin(client);
    glUniform1i(g_sw.u_mode, 1);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (n * VERTEX_FLOATS * sizeof(float)), v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei) n);
    pass_end();
}

// ------------------------------------------------------------------ the won item (#spinItemCanvas)
//
// v10 (game.deob.js g:217378-217443, prizeWheel's item loader g:217741-217884, the Free KR coin
// g:218263-218282, the spin render loop g:221605-221686): its own WebGLRenderer (setPixelRatio(0.6 *
// devicePixelRatio), LinearSRGB output) whose <canvas> sits in #spinItemCanvas, sized 2.1 x
// #spinWindow.clientWidth / 2 every frame; a PerspectiveCamera(45, 1, 0.1, 100) at (camX, 10, 0)
// looking at the origin, camX flying 300 -> 30 (x -= 0.24 (x - 30) per frame); a Scene "Wheel" (the
// coin's "KR") with AmbientLight(0x4f4f4f, PI) and a white DirectionalLight(0.5 PI) at (3, 1, 0);
// the item's holder genObj3D(0, 1 + yOff, xOff) turned rotateX(-PI / 7, a dye -PI / 11),
// rotateY(PI + 0.5 + zRota), rotateZ(-0.5) (not the player card / emoji / paint planes), bobbing
// y = orgYP + 0.55 sin(t), rotation.x = orgXR - 0.042 sin(0.9 t), t += 0.0018 dt.
// The item: loadMesh / loadAnimMesh(src, texSrc, ..., centerZ) at the holder's origin turned
// (type && !melee ? PI : PI / 2) + xRot, scale 2.2 * (spnScl || scl || 1); a spray a 9 x 7.2 plane
// turned 1.1 PI; a player card a 13 x 3.7 plane, the emoji / paint (17 / 18) 9 x 9 planes, turned
// PI / 2 (double sided); a dye a class-9 preview Player (not built here: the caller shows the PNG).
// The coin: krunkie_0 turned 0.3, scale 2.2 * 2.2, on a holder at (0, 1, 0) turned (-PI / 7, 1.07,
// -2.45).
// Here the scene is drawn offscreen into a texture that the page's <canvas> shows (html_dom_canvas_gl),
// so the page composites it in its place (under #spinItemName, over the wheel). Light intensities are
// three.js's divided by PI (basic.frag's legacy units, like class_preview.c's AmbientLight(0x97a0a8, PI)).
#define SPIN_ITEM_AMBIENT_COLOR 0x4f4f4f
#define SPIN_ITEM_AMBIENT_INTENSITY 1.0f // PI / PI
#define SPIN_ITEM_LIGHT_INTENSITY 0.5f   // 0.5 PI / PI
#define SPIN_ITEM_FOV_DEG 45.0f
#define SPIN_ITEM_CAMERA_Y 10.0f
#define SPIN_ITEM_PIXEL_RATIO 0.6f // renderer.setPixelRatio(0.6 * devicePixelRatio)
#define SPIN_ITEM_MAX_SIZE 2048
#define SPIN_ITEM_MAX_MESHES 2 // a spray's plane is one mesh, a model one, the coin one

static struct {
    Scene *scene;
    MeshTransform holder;     // the item's holder (iïiïiîí): its children are the item's meshes
    MeshTransform plane;      // a plane item's scale / turn under the holder
    Mesh *meshes[SPIN_ITEM_MAX_MESHES];
    int mesh_count;
    GltfInstance *gltf;
    float org_y, org_xr;      // orgYP / orgXR: the holder's rest height / x rotation
    int ready;                // an item is built
    uint32_t bound;           // the page canvas showing our texture (0 none)
    Camera camera;
    unsigned fbo, color_tex, depth_rb;
    int target_w, target_h;
} g_item;

static char *item_asset(const char *dir, const char *key, const char *ext) {
    const size_t n = strlen(assets_path()) + strlen(dir) + strlen(key) + strlen(ext) + 2;
    char *path = malloc(n);
    if (path) snprintf(path, n, "%s%s/%s%s", assets_path(), dir, key, ext);
    return path;
}

// the model's bounding-box centre x (loadMesh centerZ: holder.translateZ(centre.x * scale))
static float item_center_x(const char *key) {
    char *path = item_asset("models", key, ".obj");
    if (!path) return 0.0f;
    ensure_asset_file(path);
    fastObjMesh *mesh = fast_obj_read(path);
    free(path);
    if (!mesh) return 0.0f;
    float lo = INFINITY, hi = -INFINITY;
    for (unsigned int i = 1; i < mesh->position_count; i++) {
        const float x = mesh->positions[i * 3];
        if (x < lo) lo = x;
        if (x > hi) hi = x;
    }
    fast_obj_destroy(mesh);
    return lo <= hi ? (lo + hi) * 0.5f : 0.0f;
}

static unsigned int item_texture_ext(const char *key, const char *ext) {
    // texExt (player cards, emoji, paints: "webp"): the PNG sidecar "<key>.webp.png" the asset import
    // writes next to it (the HTML engine reads the same, image.c)
    char suffix[24];
    snprintf(suffix, sizeof(suffix), ext && strcmp(ext, "png") ? ".%s.png" : ".png", ext);
    char *path = item_asset("textures", key, suffix);
    const unsigned int texture = path ? load_texture(path) : 0;
    free(path);
    return texture;
}

static unsigned int item_texture(const char *key) {
    char *path = item_asset("textures", key, ".png");
    const unsigned int texture = path ? load_texture(path) : 0;
    free(path);
    return texture;
}

void spin_item_clear(void) {
    for (int i = 0; i < g_item.mesh_count; i++) {
        scene_remove_mesh(g_item.scene, g_item.meshes[i]);
        skin_fx_forget(g_item.meshes[i]);
        mesh_fini(g_item.meshes[i]);
    }
    g_item.mesh_count = 0;
    if (g_item.gltf) {
        scene_remove_gltf(g_item.scene, g_item.gltf);
        skin_fx_forget_gltf(g_item.gltf);
        gltf_instance_fini(g_item.gltf);
        g_item.gltf = NULL;
    }
    g_item.ready = 0;
}

static int item_scene(void) {
    spin_item_clear();
    if (!g_item.scene) g_item.scene = scene_init();
    return g_item.scene != NULL;
}

static void item_add(Mesh *mesh) {
    if (g_item.mesh_count >= SPIN_ITEM_MAX_MESHES) {
        mesh_fini(mesh);
        return;
    }
    g_item.meshes[g_item.mesh_count++] = mesh;
    scene_add_mesh(g_item.scene, mesh);
}

// genObj3D(x, y, z) then rotateX / rotateY / rotateZ from identity: Euler XYZ (three.js's default)
static void holder_set(const vec3 position, const float rx, const float ry, const float rz) {
    g_item.holder = (MeshTransform) {0};
    g_item.holder.scale = (vec3) {1.0f, 1.0f, 1.0f};
    g_item.holder.position = position;
    g_item.holder.rotation = (vec3) {rx, ry, rz};
    g_item.holder.rotation_order = ROTATION_ORDER_INTRINSIC;
    g_item.org_y = position.y;
    g_item.org_xr = rx;
}

// loadMesh centerZ: holder.translateZ(d), along the holder's own z: R(XYZ) * (0, 0, 1) = (sy, -sx cy, cx cy).
// orgYP was taken before (the bob then sets y from it, as in v10).
static void holder_translate_z(const float d) {
    const float cx = cosf(g_item.holder.rotation.x), sx = sinf(g_item.holder.rotation.x);
    const float cy = cosf(g_item.holder.rotation.y), sy = sinf(g_item.holder.rotation.y);
    g_item.holder.position.x += sy * d;
    g_item.holder.position.y += -sx * cy * d;
    g_item.holder.position.z += cx * cy * d;
}

// RENDER.loadMesh / loadAnimMesh(model, texture) under the holder, turned rot_y, scaled `scale`,
// with the item's material options (skin_fx.c); NULL n: a plain model (the coin)
static int item_model(const ViewerSkin *n, const char *model, const char *texture, const float rot_y, const float scale) {
    if (n && (n->flags & VS_HAS_ANIM)) {
        char rel[192];
        snprintf(rel, sizeof(rel), "models/%s.%s", model, n->file_ext ? n->file_ext : "gltf");
        char *path = concat(assets_path(), rel);
        GltfInstance *instance = path ? gltf_instance_create(gltf_model_load(path), n->anim_s > 0.0f ? n->anim_s : 1.0f) : NULL;
        free(path);
        if (!instance) return 0;
        instance->root.position = (vec3) {0};
        instance->root.rotation.y = rot_y;
        instance->root.scale = (vec3) {scale, scale, scale};
        instance->root.parent = &g_item.holder;
        skin_fx_apply_gltf(instance, n, model);
        g_item.gltf = instance;
        scene_add_gltf(g_item.scene, instance);
        return 1;
    }
    char *path = item_asset("models", model, ".obj");
    Geometry *geometry = path ? load_obj_model(path, 0) : NULL;
    free(path);
    if (!geometry) return 0;
    holder_translate_z(item_center_x(model) * scale);
    unsigned int tex = texture && texture[0] ? item_texture(texture) : 0;
    if (!tex) tex = item_texture(model);
    BasicMaterial *material = basic_material_init();
    if (!material) return 0;
    material->texture = tex;
    Mesh *mesh = mesh_init(geometry, (Material *) material);
    mesh->transform.rotation.y = rot_y;
    mesh->transform.scale = (vec3) {scale, scale, scale};
    mesh->transform.parent = &g_item.holder;
    if (n) {
        const int glow = (n->flags & (VS_GLOW | VS_SAME_GLOW)) != 0;
        skin_fx_apply(mesh, n, texture && texture[0] ? texture : NULL, model, glow, glow); // glowText = emissive = glow || sameGlow
    }
    item_add(mesh);
    return 1;
}

// new Mesh(PlaneGeometry(1, 1)) scaled (w, h, 1), turned rot_y, getMat(key, {transparent, depthWrite
// false, side}): the unit quad is a cube's front face, 0 deep (viewer_scene.c's spray plane)
static int item_plane(const ViewerSkin *n, const char *key, const float w, const float h, const float rot_y, const int double_sided) {
    const unsigned int tex = item_texture_ext(key, n->tex_ext);
    if (!tex) return 0;
    g_item.plane = (MeshTransform) {0};
    g_item.plane.scale = (vec3) {w, h, 1.0f};
    g_item.plane.rotation.y = rot_y;
    g_item.plane.parent = &g_item.holder;
    BasicMaterial *material = basic_material_init();
    if (!material) return 0;
    material->texture = tex;
    material->base.transparent = 1;
    material->no_depth_write = 1;
    material->double_sided = double_sided;
    if (n->opacity > 0.0f) material->color.w = n->opacity;
    Mesh *quad = mesh_init(create_cube_geo(), (Material *) material);
    quad->transform.scale = (vec3) {1.0f, 1.0f, 0.0001f};
    quad->transform.position.y = -0.5f;
    quad->transform.parent = &g_item.plane;
    skin_fx_apply(quad, n, key, key, 0, 0);
    item_add(quad);
    return 1;
}

// prizeWheel's item loader (g:217741) for store skin `n`; 0 when this path can't draw it
static int item_build(const ViewerSkin *n) {
    const int type = n->type;
    if (type == ITEM_DYE || type < 0 || type >= g_viewer_type_count) return 0; // a dye wears a preview Player
    const ViewerPreview *preview = type ? NULL : viewer_preview(n); // iíÎîìïî = type ? item : store.getPreview(item)
    const float y_off = preview ? preview->y_off : n->y_off, x_off = preview ? preview->x_off : n->x_off;
    const float z_rota = preview ? preview->z_rota : n->z_rota, x_rot = preview ? preview->x_rot : n->x_rot;
    const float scl = preview ? preview->scl : n->scl;
    const int flat = type == ITEM_PLAYER_CARD || type == 17 || type == 18; // iîîíïiì / iiííiïí / iìïìííî
    const vec3 at = {0.0f, 1.0f + y_off, x_off};
    if (flat) holder_set(at, 0.0f, 0.0f, 0.0f);
    else holder_set(at, -(float) M_PI / 7.0f, (float) M_PI + 0.5f + z_rota, -0.5f);

    char model[128], texture[192];
    viewer_item_keys(n, model, sizeof(model), texture, sizeof(texture));
    if (type == ITEM_SPRAY) {
        char key[64];
        snprintf(key, sizeof(key), "sprays/%d", n->id > 0 ? n->id : 0);
        return item_plane(n, key, 9.0f, 7.2f, 1.1f * (float) M_PI, 0);
    }
    if (flat) {
        char key[96];
        snprintf(key, sizeof(key), "%s%d", g_viewer_types[type], n->id > 0 ? n->id : 0);
        return item_plane(n, key, type == ITEM_PLAYER_CARD ? 13.0f : 9.0f, type == ITEM_PLAYER_CARD ? 3.7f : 9.0f, (float) M_PI / 2.0f, 1);
    }
    const float rot_y = (type && type != ITEM_MELEE ? (float) M_PI : (float) M_PI / 2.0f) + x_rot;
    return item_model(n, model, texture, rot_y, 2.2f * (n->spn_scl ? n->spn_scl : scl ? scl : 1.0f));
}

int spin_item_set(const int skin_index) {
    const ViewerSkin *n = viewer_skin(skin_index);
    if (!item_scene() || !n) return 0;
    g_item.ready = item_build(n);
    if (!g_item.ready) spin_item_clear();
    return g_item.ready;
}

int spin_item_set_coin(void) {
    if (!item_scene()) return 0;
    holder_set((vec3) {0.0f, 1.0f, 0.0f}, -(float) M_PI / 7.0f, 1.07f, -2.45f);
    g_item.ready = item_model(NULL, "krunkie_0", "krunkie_0", 0.3f, 2.2f * 2.2f);
    if (!g_item.ready) spin_item_clear();
    return g_item.ready;
}

static int item_target(const int w, const int h) {
    if (g_item.fbo && g_item.target_w == w && g_item.target_h == h) return 1;
    GLint prev_fbo = 0, prev_tex = 0, prev_rb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rb);
    if (!g_item.fbo) {
        glGenFramebuffers(1, &g_item.fbo);
        glGenTextures(1, &g_item.color_tex);
        glGenRenderbuffers(1, &g_item.depth_rb);
    }
    glBindTexture(GL_TEXTURE_2D, g_item.color_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // one level: the page's GL backend samples canvas images with a mipmap filter (as render_into_canvas)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindRenderbuffer(GL_RENDERBUFFER, g_item.depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, g_item.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_item.color_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_item.depth_rb);
    const int complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prev_fbo);
    glBindTexture(GL_TEXTURE_2D, (GLuint) prev_tex);
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint) prev_rb);
    g_item.target_w = complete ? w : 0;
    g_item.target_h = complete ? h : 0;
    return complete;
}

static vec4 hex_vec(const unsigned hex) {
    return (vec4) {((hex >> 16) & 255) / 255.0f, ((hex >> 8) & 255) / 255.0f, (hex & 255) / 255.0f, 1.0f};
}

static void item_lighting_apply(RenderLighting *saved) {
    *saved = g_render_lighting;
    g_render_lighting.ambient_color = hex_vec(SPIN_ITEM_AMBIENT_COLOR);
    g_render_lighting.ambient_intensity = SPIN_ITEM_AMBIENT_INTENSITY;
    g_render_lighting.light_color = hex_vec(0xffffff);
    g_render_lighting.light_intensity = SPIN_ITEM_LIGHT_INTENSITY;
    const float len = sqrtf(3.0f * 3.0f + 1.0f);
    g_render_lighting.light_direction = (vec3) {3.0f / len, 1.0f / len, 0.0f}; // towards the light at (3, 1, 0)
    g_render_lighting.enabled = 1;
    g_render_lighting.fog_enabled = 0;
}

// Releases the page canvas that showed the item (it paints nothing again)
static void item_unbind(void) {
    if (g_item.bound) html_dom_canvas_gl((HtmlElem) g_item.bound, 0, 0, 0, 0);
    g_item.bound = 0;
}

// The item this frame into d->item_canvas: item_size CSS px times the renderer's pixel ratio, the
// camera at (item_cam_x, 10, 0) looking at the origin, the holder's bob at item_bob
static void render_item(const SpinWheelDraw *d) {
    if (!d || !d->visible || !d->item_canvas || !g_item.ready || !g_item.scene) {
        item_unbind();
        return;
    }
    float vw = 0, vh = 0, dpr = 1;
    html_dom_viewport(&vw, &vh, &dpr);
    const int px = (int) lroundf(d->item_size * SPIN_ITEM_PIXEL_RATIO * dpr);
    if (px <= 0 || px > SPIN_ITEM_MAX_SIZE || !item_target(px, px)) return;
    if (g_item.bound && g_item.bound != d->item_canvas) item_unbind();

    g_item.holder.position.y = g_item.org_y + 0.55f * sinf(d->item_bob);
    g_item.holder.rotation.x = g_item.org_xr - 0.042f * sinf(0.9f * d->item_bob);
    Camera *camera = &g_item.camera;
    camera->fov = SPIN_ITEM_FOV_DEG * (float) M_PI / 180.0f;
    camera->near = 0.1f;
    camera->far = 100.0f;
    camera->zoom = 1.0f;
    camera->position = (vec3) {d->item_cam_x, SPIN_ITEM_CAMERA_Y, 0.0f};
    { // lookAt(origin)
        const float len = sqrtf(d->item_cam_x * d->item_cam_x + SPIN_ITEM_CAMERA_Y * SPIN_ITEM_CAMERA_Y);
        const vec3 dir = {-d->item_cam_x / len, -SPIN_ITEM_CAMERA_Y / len, 0.0f};
        camera->rotation = (vec3) {asinf(dir.y), atan2f(-dir.x, -dir.z), 0.0f};
    }

    GLint prev_fbo = 0, prev_viewport[4];
    GLfloat prev_clear[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, prev_clear);
    const GLboolean prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean prev_blend = glIsEnabled(GL_BLEND);
    const GLboolean prev_depth = glIsEnabled(GL_DEPTH_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, g_item.fbo);
    glViewport(0, 0, px, px);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    RenderLighting saved_lighting;
    item_lighting_apply(&saved_lighting);
    scene_set_alpha_target(1); // premultiplied, transparent where the item isn't (WebGLRenderer({alpha: true}))
    scene_render(g_item.scene, camera);
    scene_set_alpha_target(0);
    g_render_lighting = saved_lighting;

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prev_fbo);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glClearColor(prev_clear[0], prev_clear[1], prev_clear[2], prev_clear[3]);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST);
    if (prev_blend) glEnable(GL_BLEND);
    else glDisable(GL_BLEND);
    if (prev_depth) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);

    html_dom_canvas_gl((HtmlElem) d->item_canvas, g_item.color_tex, g_item.fbo, px, px);
    g_item.bound = d->item_canvas;
}

static void spin_item_fini(void) {
    spin_item_clear();
    if (g_item.scene) {
        scene_fini(g_item.scene);
        g_item.scene = NULL;
    }
    if (g_item.fbo) glDeleteFramebuffers(1, &g_item.fbo);
    if (g_item.color_tex) glDeleteTextures(1, &g_item.color_tex);
    if (g_item.depth_rb) glDeleteRenderbuffers(1, &g_item.depth_rb);
    g_item.fbo = g_item.color_tex = g_item.depth_rb = 0;
    g_item.target_w = g_item.target_h = 0;
    g_item.bound = 0;
}

void spin_wheel_fini(void) {
    spin_item_fini();
    if (g_sw.canvas_fbo) glDeleteFramebuffers(1, &g_sw.canvas_fbo);
    if (g_sw.canvas_tex) glDeleteTextures(1, &g_sw.canvas_tex);
    if (g_sw.program) glDeleteProgram(g_sw.program);
    if (g_sw.vbo) glDeleteBuffers(1, &g_sw.vbo);
    if (g_sw.vao) glDeleteVertexArrays(1, &g_sw.vao);
    free(g_sw.verts);
    memset(&g_sw, 0, sizeof(g_sw));
}
