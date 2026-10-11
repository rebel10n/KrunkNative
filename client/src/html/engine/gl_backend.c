// GL backend of the HTML/CSS engine. See gl_backend.h and
// doc/html-engine-plan.md "GL rendering".
//
// Design
// - One program (assets/shaders/html_ui.{vert,frag}), one vertex format
//   (GlbVertex), one VAO, streaming VBO/IBO orphaned per frame.
// - Geometry is transformed on the CPU: CSS px -> current transform ->
//   device px (x device scale) -> target px (minus the render target origin).
// - SDF primitives carry their local position in `uv` (local CSS px x device
//   scale, relative to the primitive center) so rounded corners, borders and
//   shadows stay exact under scale/rotate transforms.
// - Clips: an axis-aligned rect clip becomes a glScissor; one level of
//   axis-aligned, uniformly scaled rounded-rect clip travels per vertex
//   (clip + clip_radius); everything else (rotated clips, nested rounded
//   clips, mixed radii, clip-path polygons) goes to the stencil buffer with
//   an incrementing reference per nesting level.
// - Groups (opacity / filter): rendered into a pooled power-of-two RGBA8 FBO
//   with a DEPTH24_STENCIL8 renderbuffer, composited with mode 7. A group
//   holding a single primitive and no filters is drawn directly with the
//   opacity folded into the vertex color.
// - Draw calls break only on texture (unit 0), scissor, stencil or FBO
//   changes; every GL state change flushes the pending batch first so that
//   batch state == GL state.
//
// Texture units: 0 = primary (image / glyph atlas / gradient LUT / group),
// 1 = text gamma LUT (256x8 R8).
#include <gl.h>
#include <client.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gl_backend.h"

#define GLB_MAX_CLIPS 32
#define GLB_MAX_XFORMS 64
#define GLB_MAX_GROUPS 16
#define GLB_MAX_POLY 64
#define GLB_GRADIENT_ROWS 64
#define GLB_GRADIENT_W 256
#define GLB_MAX_FBOS 24
// Minification filter for image-rendering:auto images (DL_IMAGE, nearest = 0).
// html_ui.frag picks the mip level itself (Skia's floored level, then
// textureLod at an integer lod), so this only has to be a mipmap filter with
// linear filtering inside a level; at integer lods LINEAR_MIPMAP_LINEAR and
// LINEAR_MIPMAP_NEAREST sample identically.
#ifndef GLB_IMAGE_MIN_FILTER
#define GLB_IMAGE_MIN_FILTER GL_LINEAR_MIPMAP_LINEAR
#endif

enum {
    MODE_SOLID = 0,
    MODE_RRECT = 1,
    MODE_IMAGE = 2,
    MODE_GLYPH = 3,
    MODE_GRADIENT = 4,
    MODE_SHADOW = 5,
    MODE_BORDER = 6,
    MODE_GROUP = 7,
    MODE_BLUR = 8,
    MODE_SHADOW_INSET = 9
};

typedef struct {
    float x, y;
    float u, v;
    uint8_t rgba[4];
    float p0[4], p1[4], p2[4];
    float clip[4];
    float clip_r;
    float mode;
} GlbVertex;

typedef struct {
    GLuint fbo, tex, rbo;
    int w, h;
    int in_use;
} FboEntry;

typedef struct {
    GLuint fbo;
    int w, h;           // size in pixels
    float ox, oy;       // device-space origin of pixel (0, 0)
    int entry;          // pool index or -1
} Target;

// Clip and group frames snapshot the mutable clip state so POP restores it.
typedef struct {
    int scissor_on;
    float scissor[4];           // target px, x0 y0 x1 y1
    int shader_clip_on;
    float shader_clip[4];
    float shader_clip_r;
    int stencil_ref;
} ClipState;

typedef struct {
    ClipState saved;
    Mat2D saved_xform;
    int saved_n_xform;
    int stencil;                // 1: this frame incremented the stencil
    int shape_poly;             // 1: polygon in device pts, 0: rrect in local space
    DlRect rect;
    DlRadii radii;
    Mat2D xform;
    float pts[GLB_MAX_POLY * 2];
    int n_pts;
} ClipFrame;

typedef struct {
    int fast;                   // opacity folded into vertex colors
    int skip;                   // empty bounds: items are dropped
    float opacity;
    DlGroup group;
    Target saved_target;
    ClipState saved_clip;
    Mat2D saved_xform;
    int saved_n_xform;
    int entry;                  // pool entry rendered into
    float bx, by, bw, bh;       // device bounds of the group
} GroupFrame;

typedef struct {
    uint64_t hash;
    unsigned tick;
    int used;
} GradientRow;

struct GlBackend {
    GLuint program, vao, vbo, ibo, gamma_tex, gradient_tex;
    GlbImageResolver image_resolver;   // SVG rasters at device size (glb_set_image_resolver)
    void *image_resolver_user;
    GLint loc_viewport, loc_tex, loc_gamma, loc_cm, loc_cm_offset, loc_opacity, loc_blur_dir, loc_stencil_pass;

    // frame
    int in_frame;
    int fb_w, fb_h;
    float scale;
    GLint window_fbo;
    int cull_was_on;
    Target target;

    Mat2D xform;
    Mat2D xform_stack[GLB_MAX_XFORMS];
    int n_xform;

    ClipState clip;
    ClipFrame clips[GLB_MAX_CLIPS];
    int n_clips;

    GroupFrame groups[GLB_MAX_GROUPS];
    int n_groups;
    float opacity_mul;
    int skip_depth;

    // batch
    GlbVertex *verts;
    int nv, cap_v;
    uint32_t *idx;
    int ni, cap_i;
    size_t vbo_cap, vbo_off, ibo_cap, ibo_off;
    GLuint batch_tex;
    int batch_nearest;
    GLuint bound_tex;
    int stencil_pass;

    FboEntry fbos[GLB_MAX_FBOS];
    int n_fbos;

    GradientRow grad_rows[GLB_GRADIENT_ROWS];
    unsigned grad_tick;

    GlbStats stats, frame_stats;
};

// ---------------------------------------------------------------------------
// small math
// ---------------------------------------------------------------------------

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static float maxf(float a, float b) { return a > b ? a : b; }
static float minf(float a, float b) { return a < b ? a : b; }

static void xf_point(const Mat2D *m, float x, float y, float *ox, float *oy) {
    *ox = m->a * x + m->c * y + m->e;
    *oy = m->b * x + m->d * y + m->f;
}

static Mat2D mat_mul(const Mat2D *cur, const Mat2D *m) {
    Mat2D r;
    r.a = cur->a * m->a + cur->c * m->b;
    r.b = cur->b * m->a + cur->d * m->b;
    r.c = cur->a * m->c + cur->c * m->d;
    r.d = cur->b * m->c + cur->d * m->d;
    r.e = cur->a * m->e + cur->c * m->f + cur->e;
    r.f = cur->b * m->e + cur->d * m->f + cur->f;
    return r;
}

static int mat_axis_aligned(const Mat2D *m) {
    return fabsf(m->b) < 1e-5f && fabsf(m->c) < 1e-5f && m->a > 0.0f && m->d > 0.0f;
}

static int mat_uniform_scale(const Mat2D *m) {
    return mat_axis_aligned(m) && fabsf(m->a - m->d) < 1e-4f;
}

static float css_len(CssLength l, float ref) {
    return l.px + l.pct * 0.01f * ref;
}

// Straight CssColor -> premultiplied rgba8, scaled by `opacity`.
static void color_premul(CssColor c, float opacity, uint8_t out[4]) {
    const float a = clampf((float) c.a / 255.0f * opacity, 0.0f, 1.0f);
    out[0] = (uint8_t) (c.r * a + 0.5f);
    out[1] = (uint8_t) (c.g * a + 0.5f);
    out[2] = (uint8_t) (c.b * a + 0.5f);
    out[3] = (uint8_t) (a * 255.0f + 0.5f);
}

static int color_visible(CssColor c) {
    return c.a != 0;
}

static int pot(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

// ---------------------------------------------------------------------------
// device / target mapping
// ---------------------------------------------------------------------------

// CSS px in the current local space -> target pixels.
static void to_target(const GlBackend *b, float x, float y, float *tx, float *ty) {
    float dx, dy;
    xf_point(&b->xform, x, y, &dx, &dy);
    *tx = dx * b->scale - b->target.ox;
    *ty = dy * b->scale - b->target.oy;
}

// Axis-aligned bounding box in target px of a local rect under the current transform.
static void local_rect_bounds(const GlBackend *b, DlRect r, float out[4]) {
    const float xs[4] = {r.x, r.x + r.w, r.x + r.w, r.x};
    const float ys[4] = {r.y, r.y, r.y + r.h, r.y + r.h};
    out[0] = out[1] = 1e30f;
    out[2] = out[3] = -1e30f;
    for (int i = 0; i < 4; i++) {
        float tx, ty;
        to_target(b, xs[i], ys[i], &tx, &ty);
        out[0] = minf(out[0], tx);
        out[1] = minf(out[1], ty);
        out[2] = maxf(out[2], tx);
        out[3] = maxf(out[3], ty);
    }
}

// ---------------------------------------------------------------------------
// batch
// ---------------------------------------------------------------------------

static void stats_draw(GlBackend *b, int verts) {
    b->frame_stats.draw_calls++;
    b->frame_stats.vertices += verts;
}

static void flush(GlBackend *b) {
    if (!b->ni) {
        b->batch_tex = 0;
        return;
    }

    const size_t vbytes = (size_t) b->nv * sizeof(GlbVertex);
    const size_t ibytes = (size_t) b->ni * sizeof(uint32_t);

    glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
    if (b->vbo_off + vbytes > b->vbo_cap) {
        while (b->vbo_cap < vbytes) b->vbo_cap *= 2;
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) b->vbo_cap, NULL, GL_STREAM_DRAW); // orphan
        b->vbo_off = 0;
    }

    const uint32_t base = (uint32_t) (b->vbo_off / sizeof(GlbVertex));
    for (int i = 0; i < b->ni; i++) b->idx[i] += base;

    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr) b->vbo_off, (GLsizeiptr) vbytes, b->verts);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b->ibo);
    if (b->ibo_off + ibytes > b->ibo_cap) {
        while (b->ibo_cap < ibytes) b->ibo_cap *= 2;
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr) b->ibo_cap, NULL, GL_STREAM_DRAW);
        b->ibo_off = 0;
    }
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr) b->ibo_off, (GLsizeiptr) ibytes, b->idx);

    if (b->batch_tex && b->bound_tex != b->batch_tex) {
        glBindTexture(GL_TEXTURE_2D, b->batch_tex);
        b->bound_tex = b->batch_tex;
    }
    if (b->batch_tex && b->batch_nearest >= 0) {
        // DL_IMAGE textures come from image.c: the smooth one carries a full
        // mip chain (the shader selects the level like Skia does), the
        // pixelated twin has MAX_LEVEL 0, so the mipmap filter is complete on
        // both. Glyph atlas, gradient LUT and FBO textures pass nearest = -1
        // and never get here.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, b->batch_nearest ? GL_NEAREST : GLB_IMAGE_MIN_FILTER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, b->batch_nearest ? GL_NEAREST : GL_LINEAR);
    }

    glDrawElements(GL_TRIANGLES, b->ni, GL_UNSIGNED_INT, (const void *) (uintptr_t) b->ibo_off);
    stats_draw(b, b->nv);

    b->vbo_off += vbytes;
    b->ibo_off += ibytes;
    b->nv = b->ni = 0;
    b->batch_tex = 0;
    b->batch_nearest = -1;
}

// Makes `tex` the batch texture, flushing if the batch already uses another one.
// nearest < 0 leaves the texture's filtering alone.
static void batch_texture(GlBackend *b, GLuint tex, int nearest) {
    if (!tex) return;
    if (b->batch_tex && (b->batch_tex != tex || b->batch_nearest != nearest)) flush(b);
    b->batch_tex = tex;
    b->batch_nearest = nearest;
}

static int reserve(GlBackend *b, int nverts, int nidx) {
    if (b->nv + nverts > b->cap_v) {
        int cap = b->cap_v ? b->cap_v : 1024;
        while (cap < b->nv + nverts) cap *= 2;
        GlbVertex *v = realloc(b->verts, (size_t) cap * sizeof(GlbVertex));
        if (!v) return -1;
        b->verts = v;
        b->cap_v = cap;
    }
    if (b->ni + nidx > b->cap_i) {
        int cap = b->cap_i ? b->cap_i : 2048;
        while (cap < b->ni + nidx) cap *= 2;
        uint32_t *i = realloc(b->idx, (size_t) cap * sizeof(uint32_t));
        if (!i) return -1;
        b->idx = i;
        b->cap_i = cap;
    }
    return 0;
}

// Template vertex: everything but position/uv, filled from the current clip state.
static GlbVertex vertex_template(const GlBackend *b, int mode, const uint8_t rgba[4]) {
    GlbVertex v;
    memset(&v, 0, sizeof(v));
    memcpy(v.rgba, rgba, 4);
    v.mode = (float) mode;
    if (b->clip.shader_clip_on) {
        memcpy(v.clip, b->clip.shader_clip, sizeof(v.clip));
        v.clip_r = b->clip.shader_clip_r;
    } else {
        v.clip[0] = v.clip[1] = -1e7f;
        v.clip[2] = v.clip[3] = 1e7f;
        v.clip_r = 0.0f;
    }
    return v;
}

static void set4(float *dst, float a, float b, float c, float d) {
    dst[0] = a; dst[1] = b; dst[2] = c; dst[3] = d;
}

// Emits a quad from four corner vertices (already positioned, uv set), in order.
static void emit_quad(GlBackend *b, const GlbVertex corners[4]) {
    if (reserve(b, 4, 6)) return;
    const uint32_t base = (uint32_t) b->nv;
    memcpy(b->verts + b->nv, corners, 4 * sizeof(GlbVertex));
    b->nv += 4;
    const uint32_t tri[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
    memcpy(b->idx + b->ni, tri, sizeof(tri));
    b->ni += 6;
}

// Emits a local-space quad with the SDF parametrisation: uv = (local - center) * scale.
// Corners are the local polygon (x0,y0 .. x3,y3); center is the SDF origin.
static void emit_local_quad(GlBackend *b, const GlbVertex *tmpl, const float xy[8], float cx, float cy) {
    GlbVertex c[4];
    for (int i = 0; i < 4; i++) {
        c[i] = *tmpl;
        to_target(b, xy[i * 2], xy[i * 2 + 1], &c[i].x, &c[i].y);
        c[i].u = (xy[i * 2] - cx) * b->scale;
        c[i].v = (xy[i * 2 + 1] - cy) * b->scale;
    }
    emit_quad(b, c);
}

static void rect_corners(DlRect r, float expand, float xy[8]) {
    const float x0 = r.x - expand, y0 = r.y - expand, x1 = r.x + r.w + expand, y1 = r.y + r.h + expand;
    xy[0] = x0; xy[1] = y0;
    xy[2] = x1; xy[3] = y0;
    xy[4] = x1; xy[5] = y1;
    xy[6] = x0; xy[7] = y1;
}

// Local units spanning one target pixel under the current transform: SDF
// primitives grow their quad by this much so their analytic AA reaches
// fractional edges (Chrome draws box edges at fractional device positions,
// e.g. under #uiBase scale(0.627), instead of snapping them).
static float aa_pad(const GlBackend *b) {
    const float k = sqrtf(fabsf(b->xform.a * b->xform.d - b->xform.b * b->xform.c)) * b->scale;
    return k > 1e-6f ? 1.0f / k : 0.0f;
}

static int xform_axis_aligned(const GlBackend *b) {
    return fabsf(b->xform.b) < 1e-6f && fabsf(b->xform.c) < 1e-6f;
}

// Target-pixel rect (x0, y0, x1, y1) of a local rect under an axis-aligned transform.
static void target_rect(const GlBackend *b, DlRect r, float out[4]) {
    float x0, y0, x1, y1;
    to_target(b, r.x, r.y, &x0, &y0);
    to_target(b, r.x + r.w, r.y + r.h, &x1, &y1);
    out[0] = minf(x0, x1); out[1] = minf(y0, y1);
    out[2] = maxf(x0, x1); out[3] = maxf(y0, y1);
}

// Emits a local-space rect with explicit texcoords (u0,v0)-(u1,v1).
static void emit_textured_quad(GlBackend *b, const GlbVertex *tmpl, DlRect r, const float uv[4]) {
    float xy[8];
    rect_corners(r, 0.0f, xy);
    GlbVertex c[4];
    const float us[4] = {uv[0], uv[2], uv[2], uv[0]};
    const float vs[4] = {uv[1], uv[1], uv[3], uv[3]};
    for (int i = 0; i < 4; i++) {
        c[i] = *tmpl;
        to_target(b, xy[i * 2], xy[i * 2 + 1], &c[i].x, &c[i].y);
        c[i].u = us[i];
        c[i].v = vs[i];
    }
    emit_quad(b, c);
}

// ---------------------------------------------------------------------------
// GL state application (scissor / stencil / target)
// ---------------------------------------------------------------------------

static void apply_scissor(GlBackend *b) {
    flush(b);
    if (!b->clip.scissor_on) {
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    const int x0 = (int) floorf(b->clip.scissor[0]);
    const int y0 = (int) floorf(b->clip.scissor[1]);
    const int x1 = (int) ceilf(b->clip.scissor[2]);
    const int y1 = (int) ceilf(b->clip.scissor[3]);
    const int w = x1 > x0 ? x1 - x0 : 0;
    const int h = y1 > y0 ? y1 - y0 : 0;
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0, b->target.h - y1, w, h);  // GL scissor origin is bottom-left
}

static void apply_stencil(GlBackend *b) {
    flush(b);
    if (b->clip.stencil_ref <= 0) {
        glDisable(GL_STENCIL_TEST);
        return;
    }
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_EQUAL, b->clip.stencil_ref, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
}

static void intersect_scissor(GlBackend *b, const float r[4]) {
    if (!b->clip.scissor_on) {
        memcpy(b->clip.scissor, r, sizeof(float) * 4);
        b->clip.scissor_on = 1;
    } else {
        b->clip.scissor[0] = maxf(b->clip.scissor[0], r[0]);
        b->clip.scissor[1] = maxf(b->clip.scissor[1], r[1]);
        b->clip.scissor[2] = minf(b->clip.scissor[2], r[2]);
        b->clip.scissor[3] = minf(b->clip.scissor[3], r[3]);
    }
    if (b->clip.scissor[2] < b->clip.scissor[0]) b->clip.scissor[2] = b->clip.scissor[0];
    if (b->clip.scissor[3] < b->clip.scissor[1]) b->clip.scissor[3] = b->clip.scissor[1];
}

static void set_target(GlBackend *b, const Target *t) {
    flush(b);
    b->target = *t;
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, t->w, t->h);
    glUniform2f(b->loc_viewport, (float) t->w, (float) t->h);
}

static void set_stencil_pass(GlBackend *b, int on) {
    flush(b);
    if (b->stencil_pass == on) return;
    b->stencil_pass = on;
    glUniform1i(b->loc_stencil_pass, on);
    glColorMask(!on, !on, !on, !on);
}

// ---------------------------------------------------------------------------
// FBO pool
// ---------------------------------------------------------------------------

static int fbo_acquire(GlBackend *b, int w, int h) {
    w = pot(w < 1 ? 1 : w);
    h = pot(h < 1 ? 1 : h);

    int best = -1;
    for (int i = 0; i < b->n_fbos; i++) {
        FboEntry *e = &b->fbos[i];
        if (e->in_use || e->w < w || e->h < h) continue;
        if (best < 0 || e->w * e->h < b->fbos[best].w * b->fbos[best].h) best = i;
    }
    if (best >= 0) {
        b->fbos[best].in_use = 1;
        return best;
    }
    if (b->n_fbos == GLB_MAX_FBOS) return -1;

    FboEntry *e = &b->fbos[b->n_fbos];
    memset(e, 0, sizeof(*e));
    e->w = w;
    e->h = h;

    glGenTextures(1, &e->tex);
    glBindTexture(GL_TEXTURE_2D, e->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    b->bound_tex = e->tex;

    glGenRenderbuffers(1, &e->rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, e->rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glGenFramebuffers(1, &e->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, e->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e->tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, e->rbo);

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, b->target.fbo);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        printf("html gl backend: group framebuffer incomplete (0x%x)\n", (unsigned) status);
        glDeleteFramebuffers(1, &e->fbo);
        glDeleteRenderbuffers(1, &e->rbo);
        glDeleteTextures(1, &e->tex);
        return -1;
    }

    e->in_use = 1;
    return b->n_fbos++;
}

static void fbo_release(GlBackend *b, int i) {
    if (i >= 0 && i < b->n_fbos) b->fbos[i].in_use = 0;
}

// Switches rendering to pool entry `i` with device origin (ox, oy), clears it
// (whole texture, so blur taps outside the content read transparent) and
// resets the clip state to the content rect (bw x bh).
static void begin_offscreen(GlBackend *b, int i, float ox, float oy, int bw, int bh) {
    const FboEntry *e = &b->fbos[i];
    Target t = {e->fbo, e->w, e->h, ox, oy, i};
    set_target(b, &t);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    // Clear to transparent without leaking the clear colour: the client clears the
    // world with the map's sky colour and must find it unchanged after the UI pass.
    GLfloat prev_clear[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, prev_clear);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glClearColor(prev_clear[0], prev_clear[1], prev_clear[2], prev_clear[3]);

    memset(&b->clip, 0, sizeof(b->clip));
    const float r[4] = {0.0f, 0.0f, (float) bw, (float) bh};
    intersect_scissor(b, r);
    apply_scissor(b);
    b->frame_stats.fbo_passes++;
}

static void restore_target(GlBackend *b, const Target *t, const ClipState *clip) {
    set_target(b, t);
    b->clip = *clip;
    apply_scissor(b);
    apply_stencil(b);
}

// Texcoords of the (bw x bh) content region of pool entry i (top row at v = 1).
static void entry_uv(const GlBackend *b, int i, float bw, float bh, float uv[4]) {
    const FboEntry *e = &b->fbos[i];
    uv[0] = 0.0f;
    uv[1] = 1.0f;
    uv[2] = bw / (float) e->w;
    uv[3] = 1.0f - bh / (float) e->h;
}

// Emits a target-space quad (no transform) with texcoords.
static void emit_target_quad(GlBackend *b, const GlbVertex *tmpl, float x, float y, float w, float h, const float uv[4]) {
    GlbVertex c[4];
    const float xs[4] = {x, x + w, x + w, x};
    const float ys[4] = {y, y, y + h, y + h};
    const float us[4] = {uv[0], uv[2], uv[2], uv[0]};
    const float vs[4] = {uv[1], uv[1], uv[3], uv[3]};
    for (int i = 0; i < 4; i++) {
        c[i] = *tmpl;
        c[i].x = xs[i];
        c[i].y = ys[i];
        c[i].u = us[i];
        c[i].v = vs[i];
    }
    emit_quad(b, c);
}

// Separable 7-tap Gaussian blur of entry `src` (content bw x bh) with the given
// sigma in pixels. Returns a new pool entry holding the blurred content, or -1.
// The caller's target/clip state is saved and restored.
static int blur_entry(GlBackend *b, int src, int bw, int bh, float sigma) {
    const Target saved_target = b->target;
    const ClipState saved_clip = b->clip;

    const int tmp = fbo_acquire(b, bw, bh);
    const int dst = fbo_acquire(b, bw, bh);
    if (tmp < 0 || dst < 0) {
        fbo_release(b, tmp);
        fbo_release(b, dst);
        return -1;
    }

    const float step = maxf(sigma / 1.5f, 0.0f);
    const uint8_t white[4] = {255, 255, 255, 255};
    float uv[4];

    for (int pass = 0; pass < 2; pass++) {
        const int from = pass ? tmp : src;
        const int to = pass ? dst : tmp;
        begin_offscreen(b, to, 0.0f, 0.0f, bw, bh);
        glDisable(GL_SCISSOR_TEST);
        b->clip.scissor_on = 0;
        const FboEntry *fe = &b->fbos[from];
        glUniform2f(b->loc_blur_dir, pass ? 0.0f : step / (float) fe->w, pass ? step / (float) fe->h : 0.0f);
        entry_uv(b, from, (float) bw, (float) bh, uv);
        batch_texture(b, fe->tex, -1);
        GlbVertex tmpl = vertex_template(b, MODE_BLUR, white);
        emit_target_quad(b, &tmpl, 0.0f, 0.0f, (float) bw, (float) bh, uv);
        flush(b);
    }

    fbo_release(b, tmp);
    restore_target(b, &saved_target, &saved_clip);
    return dst;
}

// ---------------------------------------------------------------------------
// gradient LUT atlas (256 x GLB_GRADIENT_ROWS RGBA8, one row per gradient)
// ---------------------------------------------------------------------------

static uint64_t hash_bytes(uint64_t h, const void *p, size_t n) {
    const uint8_t *s = p;
    for (size_t i = 0; i < n; i++) {
        h ^= s[i];
        h *= 1099511628211ull;
    }
    return h;
}

// Resolves stop positions to 0..1 along a gradient line of `len` px
// (CSS Images 3 §3.4.3 rules), returning the stop count.
static int resolve_stops(const CssGradient *g, float len, float t[CSS_MAX_GRADIENT_STOPS], CssColor c[CSS_MAX_GRADIENT_STOPS]) {
    int n = g->stop_count;
    if (n > CSS_MAX_GRADIENT_STOPS) n = CSS_MAX_GRADIENT_STOPS;
    if (n == 0) return 0;

    int has[CSS_MAX_GRADIENT_STOPS];
    for (int i = 0; i < n; i++) {
        c[i] = g->stops[i].color;
        has[i] = g->stops[i].has_pos;
        t[i] = has[i] ? (len > 0.0f ? css_len(g->stops[i].pos, len) / len : 0.0f) : 0.0f;
    }
    if (!has[0]) { t[0] = 0.0f; has[0] = 1; }
    if (!has[n - 1]) { t[n - 1] = 1.0f; has[n - 1] = 1; }

    for (int i = 1; i < n; i++) {
        if (has[i]) {
            if (t[i] < t[i - 1]) t[i] = t[i - 1];
            continue;
        }
        int j = i;
        while (!has[j]) j++;
        const int gap = j - i + 1;
        for (int k = i; k < j; k++) {
            t[k] = t[i - 1] + (t[j] - t[i - 1]) * (float) (k - i + 1) / (float) gap;
            has[k] = 1;
        }
        i = j;
    }
    return n;
}

// Returns the LUT row (0..rows-1) holding the gradient, uploading it when new.
static int gradient_row(GlBackend *b, const CssGradient *g, float len) {
    float t[CSS_MAX_GRADIENT_STOPS];
    CssColor c[CSS_MAX_GRADIENT_STOPS];
    const int n = resolve_stops(g, len, t, c);

    uint64_t h = 1469598103934665603ull;
    h = hash_bytes(h, &n, sizeof(n));
    h = hash_bytes(h, t, sizeof(float) * (size_t) n);
    h = hash_bytes(h, c, sizeof(CssColor) * (size_t) n);

    b->grad_tick++;
    int row = -1, victim = 0;
    for (int i = 0; i < GLB_GRADIENT_ROWS; i++) {
        GradientRow *r = &b->grad_rows[i];
        if (r->used && r->hash == h) {
            r->tick = b->grad_tick;
            return i;
        }
        if (!r->used) { if (row < 0) row = i; }
        else if (r->tick < b->grad_rows[victim].tick) victim = i;
    }
    if (row < 0) row = victim;
    b->grad_rows[row].used = 1;
    b->grad_rows[row].hash = h;
    b->grad_rows[row].tick = b->grad_tick;

    uint8_t texels[GLB_GRADIENT_W * 4];
    for (int x = 0; x < GLB_GRADIENT_W; x++) {
        const float u = (float) x / (float) (GLB_GRADIENT_W - 1);
        float rgba[4] = {0, 0, 0, 0};
        if (n > 0) {
            // Largest i with t[i] <= u: u lies in [t[i], t[i+1]); coincident
            // stops (hard stop) therefore switch exactly at their position.
            int i = 0;
            while (i + 1 < n && u >= t[i + 1]) i++;
            const int j = i + 1 < n ? i + 1 : i;
            float f = 0.0f;
            if (u < t[0]) f = 0.0f;
            else if (j > i && t[j] > t[i]) f = clampf((u - t[i]) / (t[j] - t[i]), 0.0f, 1.0f);
            float p0[4], p1[4];
            const CssColor a = c[i], d = c[j];
            const float aa = a.a / 255.0f, da = d.a / 255.0f;
            p0[0] = a.r / 255.0f * aa; p0[1] = a.g / 255.0f * aa; p0[2] = a.b / 255.0f * aa; p0[3] = aa;
            p1[0] = d.r / 255.0f * da; p1[1] = d.g / 255.0f * da; p1[2] = d.b / 255.0f * da; p1[3] = da;
            for (int k = 0; k < 4; k++) rgba[k] = p0[k] + (p1[k] - p0[k]) * f;   // premultiplied interpolation
        }
        for (int k = 0; k < 4; k++) texels[x * 4 + k] = (uint8_t) (clampf(rgba[k], 0.0f, 1.0f) * 255.0f + 0.5f);
    }

    flush(b);
    glBindTexture(GL_TEXTURE_2D, b->gradient_tex);
    b->bound_tex = b->gradient_tex;
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, GLB_GRADIENT_W, 1, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    return row;
}

// ---------------------------------------------------------------------------
// primitives
// ---------------------------------------------------------------------------

static void draw_rrect(GlBackend *b, DlRect r, DlRadii rad, CssColor color) {
    if (!color_visible(color) || r.w <= 0.0f || r.h <= 0.0f) return;
    uint8_t rgba[4];
    color_premul(color, b->opacity_mul, rgba);
    GlbVertex t = vertex_template(b, MODE_RRECT, rgba);
    const float s = b->scale;
    set4(t.p0, rad.tl * s, rad.tr * s, rad.br * s, rad.bl * s);
    set4(t.p1, r.w * 0.5f * s, r.h * 0.5f * s, 0.0f, 0.0f);
    float xy[8];
    rect_corners(r, aa_pad(b), xy);   // AA band outside fractional edges
    emit_local_quad(b, &t, xy, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
}

static float group_device_scale(const GlBackend *b);

// Texture of a DL_IMAGE drawn stretched over dst: vector sources (cropped
// uv or rotated/skewed) are rasterised at the device size of the whole image
// (dst covers the uv span of it).
static GLuint image_draw_texture(GlBackend *b, const DlItem *it) {
    const DlRect d = it->u.image.dst;
    const float su = fabsf(it->u.image.uv[2] - it->u.image.uv[0]), sv = fabsf(it->u.image.uv[3] - it->u.image.uv[1]);
    if (!it->u.image.vector || !b->image_resolver || d.w <= 0.0f || d.h <= 0.0f || su <= 1e-6f || sv <= 1e-6f)
        return it->u.image.tex;
    const float k = group_device_scale(b);
    const float w = roundf(d.w * k / su), h = roundf(d.h * k / sv);
    int tw = 0, th = 0;
    const GLuint tex = b->image_resolver(b->image_resolver_user, it->u.image.vector, it->u.image.nearest, w < 1 ? 1 : w,
                                         h < 1 ? 1 : h, 0.0f, 0.0f, &tw, &th);
    return tex ? tex : it->u.image.tex;
}

// Vector DL_IMAGE (SVG) under an axis-aligned transform with the whole image
// shown (uv 0..1): the resolver rasterises it at its exact target size and
// sub-pixel offset, and the quad covers whole target pixels texel for texel.
// Returns 0 when not applicable (draw_image then stretches `tex` over dst).
static int draw_vector_image(GlBackend *b, const DlItem *it, const uint8_t rgba[4]) {
    const DlRect d = it->u.image.dst;
    const float *uv = it->u.image.uv;
    if (!it->u.image.vector || !b->image_resolver || !xform_axis_aligned(b) || d.w <= 0.0f || d.h <= 0.0f) return 0;
    if (fabsf(uv[0]) > 1e-6f || fabsf(uv[1]) > 1e-6f || fabsf(uv[2] - 1.0f) > 1e-6f || fabsf(uv[3] - 1.0f) > 1e-6f) return 0;
    float r[4];
    target_rect(b, d, r);
    const float tw = r[2] - r[0], th = r[3] - r[1];
    if (tw <= 0.0f || th <= 0.0f) return 0;   // mirrored transforms: stretched path
    const float ix = floorf(r[0]), iy = floorf(r[1]);
    int w = 0, h = 0;
    const GLuint tex = b->image_resolver(b->image_resolver_user, it->u.image.vector, it->u.image.nearest, tw, th, r[0] - ix,
                                         r[1] - iy, &w, &h);
    if (!tex || w <= 0 || h <= 0) return 0;
    // target pixels [ix, ix + w) x [iy, iy + h) back in local space
    const float sx = d.w / tw, sy = d.h / th;
    const DlRect q = dl_rect(d.x + (ix - r[0]) * sx, d.y + (iy - r[1]) * sy, (float) w * sx, (float) h * sy);
    const float full[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    batch_texture(b, tex, 0);
    GlbVertex t = vertex_template(b, MODE_IMAGE, rgba);
    t.p0[0] = it->u.image.premultiplied ? 0.0f : 1.0f;
    emit_textured_quad(b, &t, q, full);
    return 1;
}

static void draw_image(GlBackend *b, const DlItem *it) {
    const float op = it->u.image.opacity * b->opacity_mul;
    if (!it->u.image.tex || op <= 0.0f) return;
    const CssColor white = {255, 255, 255, 255, CSS_COLOR_RGBA};
    uint8_t rgba[4];
    color_premul(white, op, rgba);
    if (draw_vector_image(b, it, rgba)) return;
    batch_texture(b, image_draw_texture(b, it), it->u.image.nearest ? 1 : 0);
    GlbVertex t = vertex_template(b, MODE_IMAGE, rgba);
    t.p0[0] = it->u.image.premultiplied ? 0.0f : 1.0f;
    const DlRect d = it->u.image.dst;
    if (!xform_axis_aligned(b) || d.w <= 0.0f || d.h <= 0.0f) {
        emit_textured_quad(b, &t, d, it->u.image.uv);
        return;
    }
    // Fractional, anti-aliased edges: grow the quad by one target pixel,
    // extrapolate the texcoords and let the shader cover the exact rect (p2).
    const float pad = aa_pad(b);
    const float du = (it->u.image.uv[2] - it->u.image.uv[0]) / d.w, dv = (it->u.image.uv[3] - it->u.image.uv[1]) / d.h;
    const float uv[4] = {it->u.image.uv[0] - pad * du, it->u.image.uv[1] - pad * dv,
                         it->u.image.uv[2] + pad * du, it->u.image.uv[3] + pad * dv};
    t.p0[1] = 1.0f;
    target_rect(b, d, t.p2);
    emit_textured_quad(b, &t, dl_rect(d.x - pad, d.y - pad, d.w + 2.0f * pad, d.h + 2.0f * pad), uv);
}

static void draw_glyphs(GlBackend *b, const DlGlyphQuad *quads, int n, CssColor color, float dx, float dy) {
    if (!color_visible(color) || n <= 0 || !quads) return;
    uint8_t rgba[4];
    color_premul(color, b->opacity_mul, rgba);
    GlbVertex t = vertex_template(b, MODE_GLYPH, rgba);
    // Blink rounds the baseline to a whole device pixel after every
    // transform: round each quad's top once here, in final target space
    // (y_off is integral, so this is the baseline rounding).  x is chosen by
    // paint (Skia subpixel rule) and must not be rounded again.
    const int snap_y = xform_axis_aligned(b) && fabsf(b->xform.d * b->scale) > 1e-6f;
    for (int i = 0; i < n; i++) {
        const DlGlyphQuad *q = &quads[i];
        if (!q->tex || q->w <= 0.0f || q->h <= 0.0f) continue;
        DlRect r = dl_rect(q->x + dx, q->y + dy, q->w, q->h);
        if (snap_y) {
            float tx, ty;
            to_target(b, r.x, r.y, &tx, &ty);
            r.y += (floorf(ty + 0.5f) - ty) / (b->xform.d * b->scale);
        }
        batch_texture(b, q->tex, -1);
        emit_textured_quad(b, &t, r, q->uv);
    }
}

static void radial_radii(const CssGradient *g, DlRect box, float cx, float cy, float *rx, float *ry) {
    const float dl = fabsf(cx), dr = fabsf(box.w - cx), dt = fabsf(cy), db = fabsf(box.h - cy);
    const float closest_x = minf(dl, dr), farthest_x = maxf(dl, dr);
    const float closest_y = minf(dt, db), farthest_y = maxf(dt, db);
    const int circle = g->shape == CSS_RADIAL_CIRCLE;

    switch (g->extent) {
        case CSS_RADIAL_CLOSEST_SIDE:
            if (circle) *rx = *ry = minf(closest_x, closest_y);
            else { *rx = closest_x; *ry = closest_y; }
            break;
        case CSS_RADIAL_FARTHEST_SIDE:
            if (circle) *rx = *ry = maxf(farthest_x, farthest_y);
            else { *rx = farthest_x; *ry = farthest_y; }
            break;
        case CSS_RADIAL_CLOSEST_CORNER:
            if (circle) *rx = *ry = sqrtf(closest_x * closest_x + closest_y * closest_y);
            else {
                // same aspect as closest-side, scaled to pass through the closest corner
                const float k = closest_x > 0.0f ? closest_y / closest_x : 1.0f;
                *rx = sqrtf(closest_x * closest_x + (k > 0.0f ? closest_y * closest_y / (k * k) : 0.0f));
                *ry = *rx * k;
            }
            break;
        case CSS_RADIAL_FARTHEST_CORNER:
        default:
            if (circle) *rx = *ry = sqrtf(farthest_x * farthest_x + farthest_y * farthest_y);
            else {
                const float k = farthest_x > 0.0f ? farthest_y / farthest_x : 1.0f;
                *rx = sqrtf(farthest_x * farthest_x + (k > 0.0f ? farthest_y * farthest_y / (k * k) : 0.0f));
                *ry = *rx * k;
            }
            break;
    }
    if (*rx < 1e-3f) *rx = 1e-3f;
    if (*ry < 1e-3f) *ry = 1e-3f;
}

static void draw_gradient(GlBackend *b, const DlItem *it) {
    const CssGradient *g = &it->u.gradient.gradient;
    const DlRect rect = it->u.gradient.rect;
    DlRect box = it->u.gradient.box;
    if (box.w <= 0.0f || box.h <= 0.0f) box = rect;
    if (rect.w <= 0.0f || rect.h <= 0.0f || g->stop_count == 0) return;

    const float s = b->scale;
    const CssColor white = {255, 255, 255, 255, CSS_COLOR_RGBA};
    uint8_t rgba[4];
    color_premul(white, b->opacity_mul, rgba);
    GlbVertex t = vertex_template(b, MODE_GRADIENT, rgba);

    float len, cx, cy;
    if (g->kind == CSS_GRADIENT_RADIAL) {
        cx = css_len(g->cx, box.w);
        cy = css_len(g->cy, box.h);
        float rx, ry;
        radial_radii(g, box, cx, cy, &rx, &ry);
        len = rx;
        set4(t.p0, 1.0f, 1.0f / (rx * s), 1.0f / (ry * s), 0.0f);
    } else {
        const float a = g->angle_deg * 3.14159265358979f / 180.0f;
        const float dx = sinf(a), dy = -cosf(a);
        len = fabsf(box.w * dx) + fabsf(box.h * dy);
        if (len < 1e-3f) len = 1e-3f;
        cx = box.w * 0.5f;
        cy = box.h * 0.5f;
        set4(t.p0, 0.0f, dx, dy, 1.0f / (len * s));
    }

    const int row = gradient_row(b, g, len);
    set4(t.p1, cx * s, cy * s, ((float) row + 0.5f) / (float) GLB_GRADIENT_ROWS, 0.0f);
    batch_texture(b, b->gradient_tex, -1);

    // uv = (local - box origin) * scale; axis-aligned: AA edges at the exact rect (p2)
    float xy[8];
    float pad = 0.0f;
    if (xform_axis_aligned(b)) {
        pad = aa_pad(b);
        t.p1[3] = 1.0f;
        target_rect(b, rect, t.p2);
    }
    rect_corners(rect, pad, xy);
    emit_local_quad(b, &t, xy, box.x, box.y);
}

static float spread_radius(float r, float spread) {
    if (r <= 0.0f) return 0.0f;
    return maxf(0.0f, r + spread);
}

static void draw_box_shadow(GlBackend *b, const DlItem *it) {
    if (!color_visible(it->u.box_shadow.color)) return;

    const float s = b->scale;
    const float sigma = maxf(it->u.box_shadow.blur, 0.0f) * 0.5f;
    uint8_t rgba[4];
    color_premul(it->u.box_shadow.color, b->opacity_mul, rgba);

    if (!it->u.box_shadow.inset) {
        // Shadow rect: border box offset and spread. The border box is cut out.
        const DlRect box = it->u.box_shadow.rect;
        const DlRect srect = dl_rect(box.x + it->u.box_shadow.dx - it->u.box_shadow.spread, box.y + it->u.box_shadow.dy - it->u.box_shadow.spread,
                                     box.w + 2.0f * it->u.box_shadow.spread, box.h + 2.0f * it->u.box_shadow.spread);
        if (srect.w <= 0.0f || srect.h <= 0.0f) return;
        const DlRadii sr = dl_radii(spread_radius(it->u.box_shadow.radii.tl, it->u.box_shadow.spread), spread_radius(it->u.box_shadow.radii.tr, it->u.box_shadow.spread),
                                    spread_radius(it->u.box_shadow.radii.br, it->u.box_shadow.spread), spread_radius(it->u.box_shadow.radii.bl, it->u.box_shadow.spread));
        const float scx = srect.x + srect.w * 0.5f, scy = srect.y + srect.h * 0.5f;
        const float bcx = box.x + box.w * 0.5f, bcy = box.y + box.h * 0.5f;

        GlbVertex t = vertex_template(b, MODE_SHADOW, rgba);
        set4(t.p0, sr.tl * s, sr.tr * s, sr.br * s, sr.bl * s);
        set4(t.p1, srect.w * 0.5f * s, srect.h * 0.5f * s, sigma * s, -it->u.box_shadow.spread * s);
        set4(t.p2, (bcx - scx) * s, (bcy - scy) * s, box.w * 0.5f * s, box.h * 0.5f * s);

        float xy[8];
        rect_corners(srect, 3.0f * sigma + 1.0f, xy);
        emit_local_quad(b, &t, xy, scx, scy);
        return;
    }

    // Inset: padding box = rect - border widths; shadow rect = padding box
    // offset by (dx, dy) and shrunk by spread; coverage = 1 - shadow, clipped
    // to the padding box.
    const float *pad = it->u.box_shadow.inset_pad;
    const DlRect box = dl_rect(it->u.box_shadow.rect.x + pad[3], it->u.box_shadow.rect.y + pad[0],
                               it->u.box_shadow.rect.w - pad[1] - pad[3], it->u.box_shadow.rect.h - pad[0] - pad[2]);
    if (box.w <= 0.0f || box.h <= 0.0f) return;
    const DlRadii br = dl_radii(maxf(0.0f, it->u.box_shadow.radii.tl - maxf(pad[0], pad[3])), maxf(0.0f, it->u.box_shadow.radii.tr - maxf(pad[0], pad[1])),
                                maxf(0.0f, it->u.box_shadow.radii.br - maxf(pad[2], pad[1])), maxf(0.0f, it->u.box_shadow.radii.bl - maxf(pad[2], pad[3])));
    const DlRect srect = dl_rect(box.x + it->u.box_shadow.dx + it->u.box_shadow.spread, box.y + it->u.box_shadow.dy + it->u.box_shadow.spread,
                                 box.w - 2.0f * it->u.box_shadow.spread, box.h - 2.0f * it->u.box_shadow.spread);
    const DlRadii sr = dl_radii(maxf(0.0f, br.tl - it->u.box_shadow.spread), maxf(0.0f, br.tr - it->u.box_shadow.spread),
                                maxf(0.0f, br.br - it->u.box_shadow.spread), maxf(0.0f, br.bl - it->u.box_shadow.spread));
    const float scx = srect.x + srect.w * 0.5f, scy = srect.y + srect.h * 0.5f;
    const float bcx = box.x + box.w * 0.5f, bcy = box.y + box.h * 0.5f;

    GlbVertex t = vertex_template(b, MODE_SHADOW_INSET, rgba);
    set4(t.p0, sr.tl * s, sr.tr * s, sr.br * s, sr.bl * s);
    set4(t.p1, maxf(srect.w, 0.0f) * 0.5f * s, maxf(srect.h, 0.0f) * 0.5f * s, sigma * s, it->u.box_shadow.spread * s);
    set4(t.p2, (bcx - scx) * s, (bcy - scy) * s, box.w * 0.5f * s, box.h * 0.5f * s);

    float xy[8];
    rect_corners(box, aa_pad(b), xy);   // the shader clips to the box (p2) with AA
    emit_local_quad(b, &t, xy, scx, scy);
}

static void draw_border(GlBackend *b, const DlItem *it) {
    const DlRect r = it->u.border.rect;
    if (r.w <= 0.0f || r.h <= 0.0f) return;

    const float s = b->scale;
    const float wt = maxf(it->u.border.width[0], 0.0f), wr = maxf(it->u.border.width[1], 0.0f);
    const float wb = maxf(it->u.border.width[2], 0.0f), wl = maxf(it->u.border.width[3], 0.0f);

    // Inner box corners, clamped so trapezoids never invert.
    float ix0 = minf(r.x + wl, r.x + r.w);
    float ix1 = maxf(r.x + r.w - wr, ix0);
    float iy0 = minf(r.y + wt, r.y + r.h);
    float iy1 = maxf(r.y + r.h - wb, iy0);
    {
        const float ip = aa_pad(b);
        if (ix1 - ix0 > 2.0f * ip && iy1 - iy0 > 2.0f * ip) { ix0 += ip; ix1 -= ip; iy0 += ip; iy1 -= ip; }
    }
    // Inner corner points pulled in by each corner's inner radius: with
    // rounded corners the curved part of the ring lies inside the inner box's
    // corner squares, which trapezoids ending at the straight inner edges would
    // leave uncovered (the background showed through, e.g. round swatches).
    const DlRadii orad = it->u.border.radii;
    const float rtl = maxf(0.0f, orad.tl - minf(wt, wl)), rtr = maxf(0.0f, orad.tr - minf(wt, wr));
    const float rbr = maxf(0.0f, orad.br - minf(wb, wr)), rbl = maxf(0.0f, orad.bl - minf(wb, wl));
    float tlx = ix0 + rtl, tly = iy0 + rtl, trx = ix1 - rtr, try_ = iy0 + rtr;
    float brx = ix1 - rbr, bry = iy1 - rbr, blx = ix0 + rbl, bly = iy1 - rbl;
    const float mx = (ix0 + ix1) * 0.5f, my = (iy0 + iy1) * 0.5f;
    if (tlx > trx) { tlx = trx = mx; }
    if (blx > brx) { blx = brx = mx; }
    if (tly > bly) { tly = bly = my; }
    if (try_ > bry) { try_ = bry = my; }
    // Outer corners grow and inner corners shrink by one target pixel along
    // the shared diagonals, so the SDF AA reaches fractional edges without
    // adjacent edge trapezoids overlapping.
    const float pad = aa_pad(b);
    const float ox0 = r.x - pad, oy0 = r.y - pad, ox1 = r.x + r.w + pad, oy1 = r.y + r.h + pad;

    const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    const float edge_len[4] = {r.w, r.h, r.w, r.h};

    for (int e = 0; e < 4; e++) {
        const float w = it->u.border.width[e];
        if (w <= 0.0f || it->u.border.style[e] == DL_BORDER_NONE || !color_visible(it->u.border.color[e])) continue;

        float period = 0.0f;
        if (it->u.border.style[e] == DL_BORDER_DASHED || it->u.border.style[e] == DL_BORDER_DOTTED) {
            const float base = it->u.border.style[e] == DL_BORDER_DASHED ? 5.0f * w : 2.0f * w;
            float n = floorf(edge_len[e] / base + 0.5f);
            if (n < 1.0f) n = 1.0f;
            period = edge_len[e] / n;
        }

        uint8_t rgba[4];
        color_premul(it->u.border.color[e], b->opacity_mul, rgba);
        GlbVertex t = vertex_template(b, MODE_BORDER, rgba);
        set4(t.p0, it->u.border.radii.tl * s, it->u.border.radii.tr * s, it->u.border.radii.br * s, it->u.border.radii.bl * s);
        set4(t.p1, r.w * 0.5f * s, r.h * 0.5f * s, (float) it->u.border.style[e] + 4.0f * (float) e, period * s);
        set4(t.p2, wt * s, wr * s, wb * s, wl * s);

        float xy[8];
        switch (e) {
            case DL_EDGE_TOP:    xy[0] = ox0; xy[1] = oy0; xy[2] = ox1; xy[3] = oy0; xy[4] = trx; xy[5] = try_; xy[6] = tlx; xy[7] = tly; break;
            case DL_EDGE_RIGHT:  xy[0] = ox1; xy[1] = oy0; xy[2] = ox1; xy[3] = oy1; xy[4] = brx; xy[5] = bry; xy[6] = trx; xy[7] = try_; break;
            case DL_EDGE_BOTTOM: xy[0] = ox1; xy[1] = oy1; xy[2] = ox0; xy[3] = oy1; xy[4] = blx; xy[5] = bly; xy[6] = brx; xy[7] = bry; break;
            default:             xy[0] = ox0; xy[1] = oy1; xy[2] = ox0; xy[3] = oy0; xy[4] = tlx; xy[5] = tly; xy[6] = blx; xy[7] = bly; break;
        }
        emit_local_quad(b, &t, xy, cx, cy);
    }
}

// ---------------------------------------------------------------------------
// polygon triangulation (ear clipping, device space) for stencil clip-paths
// ---------------------------------------------------------------------------

static float tri_area2(const float *p, int a, int b, int c) {
    return (p[b * 2] - p[a * 2]) * (p[c * 2 + 1] - p[a * 2 + 1]) - (p[c * 2] - p[a * 2]) * (p[b * 2 + 1] - p[a * 2 + 1]);
}

static int point_in_tri(const float *p, int a, int b, int c, int q) {
    const float s1 = tri_area2(p, a, b, q), s2 = tri_area2(p, b, c, q), s3 = tri_area2(p, c, a, q);
    const int neg = s1 < 0.0f || s2 < 0.0f || s3 < 0.0f;
    const int pos = s1 > 0.0f || s2 > 0.0f || s3 > 0.0f;
    return !(neg && pos);
}

// Fills `tris` with up to 3*(n-2) indices. Returns the index count.
static int triangulate(const float *pts, int n, int *tris) {
    if (n < 3) return 0;
    int ring[GLB_MAX_POLY];
    for (int i = 0; i < n; i++) ring[i] = i;

    float area = 0.0f;
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        area += pts[i * 2] * pts[j * 2 + 1] - pts[j * 2] * pts[i * 2 + 1];
    }
    const float orient = area >= 0.0f ? 1.0f : -1.0f;

    int count = n, out = 0, guard = 0;
    while (count > 3 && guard++ < 4 * GLB_MAX_POLY) {
        int found = 0;
        for (int i = 0; i < count; i++) {
            const int a = ring[(i + count - 1) % count], bb = ring[i], c = ring[(i + 1) % count];
            if (tri_area2(pts, a, bb, c) * orient <= 1e-9f) continue;   // reflex
            int inside = 0;
            for (int k = 0; k < count && !inside; k++) {
                const int q = ring[k];
                if (q == a || q == bb || q == c) continue;
                inside = point_in_tri(pts, a, bb, c, q);
            }
            if (inside) continue;
            tris[out++] = a; tris[out++] = bb; tris[out++] = c;
            for (int k = i; k < count - 1; k++) ring[k] = ring[k + 1];
            count--;
            found = 1;
            break;
        }
        if (!found) break;      // degenerate: fall through with a fan of what is left
    }
    if (count >= 3) {
        for (int i = 1; i + 1 < count; i++) {
            tris[out++] = ring[0]; tris[out++] = ring[i]; tris[out++] = ring[i + 1];
        }
    }
    return out;
}

// Emits a device-space polygon (mode 0) as triangles.
static void emit_polygon(GlBackend *b, const float *pts, int n) {
    int tris[GLB_MAX_POLY * 3];
    const int nt = triangulate(pts, n, tris);
    if (!nt || reserve(b, n, nt)) return;
    const uint8_t white[4] = {255, 255, 255, 255};
    GlbVertex t = vertex_template(b, MODE_SOLID, white);
    const uint32_t base = (uint32_t) b->nv;
    for (int i = 0; i < n; i++) {
        GlbVertex v = t;
        v.x = pts[i * 2];
        v.y = pts[i * 2 + 1];
        b->verts[b->nv++] = v;
    }
    for (int i = 0; i < nt; i++) b->idx[b->ni++] = base + (uint32_t) tris[i];
}

// ---------------------------------------------------------------------------
// clips
// ---------------------------------------------------------------------------

static void emit_clip_shape(GlBackend *b, const ClipFrame *f) {
    if (f->shape_poly) {
        emit_polygon(b, f->pts, f->n_pts);
        return;
    }
    const Mat2D saved = b->xform;
    b->xform = f->xform;
    const CssColor white = {255, 255, 255, 255, CSS_COLOR_RGBA};
    const float saved_op = b->opacity_mul;
    b->opacity_mul = 1.0f;
    draw_rrect(b, f->rect, f->radii, white);
    b->opacity_mul = saved_op;
    b->xform = saved;
}

// Draws the frame's shape into the stencil buffer, incrementing where it passes
// the current test, then raises the reference.
static void stencil_push(GlBackend *b, ClipFrame *f) {
    flush(b);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_EQUAL, b->clip.stencil_ref, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
    set_stencil_pass(b, 1);
    emit_clip_shape(b, f);
    set_stencil_pass(b, 0);
    b->clip.stencil_ref++;
    f->stencil = 1;
    apply_stencil(b);
}

static void stencil_pop(GlBackend *b, const ClipFrame *f) {
    flush(b);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_EQUAL, b->clip.stencil_ref, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_DECR);
    set_stencil_pass(b, 1);
    emit_clip_shape(b, f);
    set_stencil_pass(b, 0);
    b->clip.stencil_ref--;
    apply_stencil(b);
}

static ClipFrame *clip_frame_begin(GlBackend *b) {
    if (b->n_clips == GLB_MAX_CLIPS) return NULL;
    ClipFrame *f = &b->clips[b->n_clips++];
    memset(f, 0, sizeof(*f));
    f->saved = b->clip;
    f->saved_xform = b->xform;
    f->saved_n_xform = b->n_xform;
    return f;
}

static int radii_uniform(DlRadii r) {
    return fabsf(r.tl - r.tr) < 1e-4f && fabsf(r.tl - r.br) < 1e-4f && fabsf(r.tl - r.bl) < 1e-4f;
}

static int radii_zero(DlRadii r) {
    return r.tl <= 0.0f && r.tr <= 0.0f && r.br <= 0.0f && r.bl <= 0.0f;
}

static void push_clip_rrect(GlBackend *b, DlRect rect, DlRadii radii) {
    ClipFrame *f = clip_frame_begin(b);
    if (!f) return;
    f->rect = rect;
    f->radii = radii;
    f->xform = b->xform;

    float bounds[4];
    local_rect_bounds(b, rect, bounds);

    if (mat_axis_aligned(&b->xform)) {
        intersect_scissor(b, bounds);
        apply_scissor(b);
        if (radii_zero(radii)) return;
        if (radii_uniform(radii) && mat_uniform_scale(&b->xform) && !b->clip.shader_clip_on) {
            b->clip.shader_clip_on = 1;
            memcpy(b->clip.shader_clip, bounds, sizeof(bounds));
            b->clip.shader_clip_r = radii.tl * b->xform.a * b->scale;
            return;
        }
    } else {
        intersect_scissor(b, bounds);
        apply_scissor(b);
    }
    stencil_push(b, f);
}

static void push_clip_poly(GlBackend *b, const float *pts, int n) {
    ClipFrame *f = clip_frame_begin(b);
    if (!f) return;
    if (n > GLB_MAX_POLY) n = GLB_MAX_POLY;
    f->shape_poly = 1;
    f->n_pts = n;
    float bounds[4] = {1e30f, 1e30f, -1e30f, -1e30f};
    for (int i = 0; i < n; i++) {
        float tx, ty;
        to_target(b, pts[i * 2], pts[i * 2 + 1], &tx, &ty);
        f->pts[i * 2] = tx;
        f->pts[i * 2 + 1] = ty;
        bounds[0] = minf(bounds[0], tx); bounds[1] = minf(bounds[1], ty);
        bounds[2] = maxf(bounds[2], tx); bounds[3] = maxf(bounds[3], ty);
    }
    if (n < 3) {
        const float empty[4] = {0, 0, 0, 0};
        intersect_scissor(b, empty);
        apply_scissor(b);
        return;
    }
    intersect_scissor(b, bounds);
    apply_scissor(b);
    stencil_push(b, f);
}

static void pop_clip(GlBackend *b) {
    if (!b->n_clips) return;
    ClipFrame *f = &b->clips[--b->n_clips];
    if (f->stencil) stencil_pop(b, f);
    b->clip = f->saved;
    b->xform = f->saved_xform;
    b->n_xform = f->saved_n_xform;
    apply_scissor(b);
    apply_stencil(b);
}

// ---------------------------------------------------------------------------
// groups
// ---------------------------------------------------------------------------

static int item_is_primitive(DlOp op) {
    return op == DL_RECT || op == DL_RRECT || op == DL_BORDER || op == DL_IMAGE || op == DL_GRADIENT ||
           op == DL_GLYPHS || op == DL_BOX_SHADOW || op == DL_TEXT_SHADOW;
}

// Index of the POP_GROUP matching the PUSH_GROUP at `start`, or list->n.
static int find_group_end(const DisplayList *list, int start, int *n_primitives) {
    int depth = 0, prims = 0;
    for (int i = start; i < list->n; i++) {
        const DlOp op = list->items[i].op;
        if (op == DL_PUSH_GROUP) depth++;
        else if (op == DL_POP_GROUP && --depth == 0) {
            *n_primitives = prims;
            return i;
        } else if (item_is_primitive(op)) prims++;
    }
    *n_primitives = prims;
    return list->n;
}


// Color matrix (rgba, column-major mat4 + offset) for the group's filters,
// per CSS Filter Effects Module Level 1.
static void filter_matrix(const DlGroup *g, float m[16], float off[4]) {
    float cur[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    float cur_off[4] = {0, 0, 0, 0};

    for (int i = 0; i < g->n_filters && i < CSS_MAX_FILTER_OPS; i++) {
        const CssFilterOp *f = &g->filters[i];
        float k[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
        float k_off[4] = {0, 0, 0, 0};
        const float a = f->amount;
        float s;
        switch (f->kind) {
            case CSS_FILTER_GRAYSCALE:
                s = 1.0f - clampf(a, 0.0f, 1.0f);
                k[0][0] = 0.2126f + 0.7874f * s; k[0][1] = 0.7152f - 0.7152f * s; k[0][2] = 0.0722f - 0.0722f * s;
                k[1][0] = 0.2126f - 0.2126f * s; k[1][1] = 0.7152f + 0.2848f * s; k[1][2] = 0.0722f - 0.0722f * s;
                k[2][0] = 0.2126f - 0.2126f * s; k[2][1] = 0.7152f - 0.7152f * s; k[2][2] = 0.0722f + 0.9278f * s;
                break;
            case CSS_FILTER_SEPIA:
                s = 1.0f - clampf(a, 0.0f, 1.0f);
                k[0][0] = 0.393f + 0.607f * s; k[0][1] = 0.769f - 0.769f * s; k[0][2] = 0.189f - 0.189f * s;
                k[1][0] = 0.349f - 0.349f * s; k[1][1] = 0.686f + 0.314f * s; k[1][2] = 0.168f - 0.168f * s;
                k[2][0] = 0.272f - 0.272f * s; k[2][1] = 0.534f - 0.534f * s; k[2][2] = 0.131f + 0.869f * s;
                break;
            case CSS_FILTER_SATURATE:
                s = maxf(a, 0.0f);
                k[0][0] = 0.213f + 0.787f * s; k[0][1] = 0.715f - 0.715f * s; k[0][2] = 0.072f - 0.072f * s;
                k[1][0] = 0.213f - 0.213f * s; k[1][1] = 0.715f + 0.285f * s; k[1][2] = 0.072f - 0.072f * s;
                k[2][0] = 0.213f - 0.213f * s; k[2][1] = 0.715f - 0.715f * s; k[2][2] = 0.072f + 0.928f * s;
                break;
            case CSS_FILTER_HUE_ROTATE: {
                const float rad = a * 3.14159265358979f / 180.0f, c = cosf(rad), sn = sinf(rad);
                k[0][0] = 0.213f + c * 0.787f - sn * 0.213f; k[0][1] = 0.715f - c * 0.715f - sn * 0.715f; k[0][2] = 0.072f - c * 0.072f + sn * 0.928f;
                k[1][0] = 0.213f - c * 0.213f + sn * 0.143f; k[1][1] = 0.715f + c * 0.285f + sn * 0.140f; k[1][2] = 0.072f - c * 0.072f - sn * 0.283f;
                k[2][0] = 0.213f - c * 0.213f - sn * 0.787f; k[2][1] = 0.715f - c * 0.715f + sn * 0.715f; k[2][2] = 0.072f + c * 0.928f + sn * 0.072f;
                break;
            }
            case CSS_FILTER_BRIGHTNESS:
                k[0][0] = k[1][1] = k[2][2] = maxf(a, 0.0f);
                break;
            case CSS_FILTER_CONTRAST:
                k[0][0] = k[1][1] = k[2][2] = maxf(a, 0.0f);
                k_off[0] = k_off[1] = k_off[2] = 0.5f - 0.5f * maxf(a, 0.0f);
                break;
            case CSS_FILTER_INVERT:
                s = clampf(a, 0.0f, 1.0f);
                k[0][0] = k[1][1] = k[2][2] = 1.0f - 2.0f * s;
                k_off[0] = k_off[1] = k_off[2] = s;
                break;
            case CSS_FILTER_OPACITY:
                k[3][3] = clampf(a, 0.0f, 1.0f);
                break;
            default:
                continue;   // drop-shadow handled separately, blur unsupported
        }
        // cur = k * cur ; off = k * off + k_off
        float next[4][4], next_off[4];
        for (int r = 0; r < 4; r++) {
            next_off[r] = k_off[r];
            for (int c = 0; c < 4; c++) {
                float v = 0.0f;
                for (int t = 0; t < 4; t++) v += k[r][t] * cur[t][c];
                next[r][c] = v;
                next_off[r] += k[r][c] * cur_off[c];
            }
        }
        memcpy(cur, next, sizeof(cur));
        memcpy(cur_off, next_off, sizeof(cur_off));
    }
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) m[c * 4 + r] = cur[r][c];
    memcpy(off, cur_off, sizeof(float) * 4);
}

static float group_device_scale(const GlBackend *b);

static void push_group(GlBackend *b, const DisplayList *list, int index) {
    const DlGroup *g = &list->items[index].u.group;
    if (b->n_groups == GLB_MAX_GROUPS) {
        b->skip_depth = 1;      // too deep: drop the content rather than mis-nest
        return;
    }
    GroupFrame *f = &b->groups[b->n_groups++];
    memset(f, 0, sizeof(*f));
    f->group = *g;
    f->opacity = clampf(g->opacity, 0.0f, 1.0f);
    f->entry = -1;

    int prims = 0;
    find_group_end(list, index, &prims);
    if (prims <= 1 && g->n_filters == 0) {
        f->fast = 1;
        b->opacity_mul *= f->opacity;
        return;
    }

    // Room for every drop-shadow of the chain (each one offsets/blurs the
    // result of the previous one, so the extents add up).
    float pad = 0.0f;
    {
        const float k = group_device_scale(b);
        for (int i = 0; i < g->n_filters && i < CSS_MAX_FILTER_OPS; i++) {
            const CssFilterOp *ds = &g->filters[i];
            if (ds->kind != CSS_FILTER_DROP_SHADOW) continue;
            pad += ds->shadow.blur * 1.5f * k + maxf(fabsf(ds->shadow.x), fabsf(ds->shadow.y)) * k;
        }
        if (pad > 0.0f) pad = ceilf(pad + 1.0f);
    }

    float bounds[4];
    local_rect_bounds(b, g->rect, bounds);
    bounds[0] = floorf(bounds[0]) - pad; bounds[1] = floorf(bounds[1]) - pad;
    bounds[2] = ceilf(bounds[2]) + pad;  bounds[3] = ceilf(bounds[3]) + pad;

    // Clamp to what can end up visible: the current scissor (or target) grown by the shadow pad.
    float limit[4] = {-pad, -pad, (float) b->target.w + pad, (float) b->target.h + pad};
    if (b->clip.scissor_on) {
        limit[0] = b->clip.scissor[0] - pad; limit[1] = b->clip.scissor[1] - pad;
        limit[2] = b->clip.scissor[2] + pad; limit[3] = b->clip.scissor[3] + pad;
    }
    bounds[0] = maxf(bounds[0], limit[0]); bounds[1] = maxf(bounds[1], limit[1]);
    bounds[2] = minf(bounds[2], limit[2]); bounds[3] = minf(bounds[3], limit[3]);

    const int bw = (int) ceilf(bounds[2] - bounds[0]);
    const int bh = (int) ceilf(bounds[3] - bounds[1]);
    if (bw <= 0 || bh <= 0 || f->opacity <= 0.0f) {
        f->skip = 1;
        b->skip_depth = 1;
        return;
    }

    const int entry = fbo_acquire(b, bw, bh);
    if (entry < 0) {
        f->skip = 1;
        b->skip_depth = 1;
        return;
    }

    f->entry = entry;
    f->bx = bounds[0]; f->by = bounds[1];
    f->bw = (float) bw; f->bh = (float) bh;
    f->saved_target = b->target;
    f->saved_clip = b->clip;
    f->saved_xform = b->xform;
    f->saved_n_xform = b->n_xform;

    begin_offscreen(b, entry, b->target.ox + f->bx, b->target.oy + f->by, bw, bh);
}

// Device px per CSS px in the group's local space (device scale x transform).
static float group_device_scale(const GlBackend *b) {
    const float k = sqrtf(fabsf(b->xform.a * b->xform.d - b->xform.b * b->xform.c)) * b->scale;
    return k > 1e-6f ? k : b->scale;
}

static void set_group_uniforms(GlBackend *b, const float m[16], const float off[4], float opacity) {
    flush(b);
    glUniformMatrix4fv(b->loc_cm, 1, GL_FALSE, m);
    glUniform4fv(b->loc_cm_offset, 1, off);
    glUniform1f(b->loc_opacity, opacity);
}

// Color matrix of filters [i0, i1) of the group (drop-shadows excluded).
static void filter_matrix_range(const DlGroup *g, int i0, int i1, float m[16], float off[4]) {
    DlGroup sub = *g;
    sub.n_filters = 0;
    for (int i = i0; i < i1 && i < CSS_MAX_FILTER_OPS; i++)
        if (g->filters[i].kind != CSS_FILTER_DROP_SHADOW) sub.filters[sub.n_filters++] = g->filters[i];
    filter_matrix(&sub, m, off);
}

// One filter pass: src (bw x bh pool entry) -> new entry.  Returns the new
// entry and releases src, or returns src unchanged when out of FBOs.
static int filter_pass_matrix(GlBackend *b, int src, int bw, int bh, const DlGroup *g, int i0, int i1) {
    float m[16], off[4], uv[4];
    filter_matrix_range(g, i0, i1, m, off);
    const Target st = b->target;
    const ClipState sc = b->clip;
    const int dst = fbo_acquire(b, bw, bh);
    if (dst < 0) return src;
    begin_offscreen(b, dst, 0.0f, 0.0f, bw, bh);
    set_group_uniforms(b, m, off, 1.0f);
    const uint8_t white[4] = {255, 255, 255, 255};
    batch_texture(b, b->fbos[src].tex, -1);
    GlbVertex t = vertex_template(b, MODE_GROUP, white);
    entry_uv(b, src, (float) bw, (float) bh, uv);
    emit_target_quad(b, &t, 0.0f, 0.0f, (float) bw, (float) bh, uv);
    flush(b);
    restore_target(b, &st, &sc);
    fbo_release(b, src);
    return dst;
}

// drop-shadow(dx dy blur color) applied to src: tinted (blurred) copy at the
// offset, src over it.  blur 0 -> plain offset + tint (no Gaussian pass).
static int filter_pass_drop_shadow(GlBackend *b, int src, int bw, int bh, const CssFilterOp *ds, float k) {
    if (!color_visible(ds->shadow.color)) return src;
    float uv[4];
    int shadow = src;
    if (ds->shadow.blur > 0.0f) {
        shadow = blur_entry(b, src, bw, bh, ds->shadow.blur * 0.5f * k);
        if (shadow < 0) shadow = src;
    }
    const Target st = b->target;
    const ClipState sc = b->clip;
    const int dst = fbo_acquire(b, bw, bh);
    if (dst < 0) {
        if (shadow != src) fbo_release(b, shadow);
        return src;
    }
    begin_offscreen(b, dst, 0.0f, 0.0f, bw, bh);
    static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    static const float zero[4] = {0, 0, 0, 0};
    set_group_uniforms(b, ident, zero, 1.0f);

    uint8_t rgba[4];
    color_premul(ds->shadow.color, 1.0f, rgba);
    batch_texture(b, b->fbos[shadow].tex, -1);
    GlbVertex ts = vertex_template(b, MODE_GROUP, rgba);
    ts.p0[0] = 1.0f;
    entry_uv(b, shadow, (float) bw, (float) bh, uv);
    emit_target_quad(b, &ts, ds->shadow.x * k, ds->shadow.y * k, (float) bw, (float) bh, uv);

    const uint8_t white[4] = {255, 255, 255, 255};
    batch_texture(b, b->fbos[src].tex, -1);
    GlbVertex tc = vertex_template(b, MODE_GROUP, white);
    entry_uv(b, src, (float) bw, (float) bh, uv);
    emit_target_quad(b, &tc, 0.0f, 0.0f, (float) bw, (float) bh, uv);
    flush(b);
    restore_target(b, &st, &sc);
    if (shadow != src) fbo_release(b, shadow);
    fbo_release(b, src);
    return dst;
}

static void pop_group(GlBackend *b) {
    if (!b->n_groups) return;
    GroupFrame *f = &b->groups[--b->n_groups];
    if (f->fast) {
        // recompute from the remaining fast frames instead of dividing (no drift)
        b->opacity_mul = 1.0f;
        for (int i = 0; i < b->n_groups; i++) if (b->groups[i].fast) b->opacity_mul *= b->groups[i].opacity;
        return;
    }
    if (f->skip) return;

    flush(b);
    restore_target(b, &f->saved_target, &f->saved_clip);
    b->xform = f->saved_xform;
    b->n_xform = f->saved_n_xform;

    const int bw = (int) f->bw, bh = (int) f->bh;
    float uv[4];

    // CSS Filter Effects: filters apply in order, each to the previous
    // result.  Runs of color-matrix filters fold into one matrix; every
    // drop-shadow is its own offscreen pass (so 4-layer outline chains
    // thicken progressively).  The last run + opacity composite to the target.
    const DlGroup *g = &f->group;
    const float k = group_device_scale(b);
    int cur = f->entry, seg = 0;
    for (int i = 0; i < g->n_filters && i < CSS_MAX_FILTER_OPS; i++) {
        if (g->filters[i].kind != CSS_FILTER_DROP_SHADOW) continue;
        if (i > seg) cur = filter_pass_matrix(b, cur, bw, bh, g, seg, i);
        cur = filter_pass_drop_shadow(b, cur, bw, bh, &g->filters[i], k);
        seg = i + 1;
    }

    float m[16], off[4];
    filter_matrix_range(g, seg, g->n_filters, m, off);
    set_group_uniforms(b, m, off, f->opacity * b->opacity_mul);

    const uint8_t white[4] = {255, 255, 255, 255};
    batch_texture(b, b->fbos[cur].tex, -1);
    GlbVertex t = vertex_template(b, MODE_GROUP, white);
    entry_uv(b, cur, f->bw, f->bh, uv);
    emit_target_quad(b, &t, f->bx, f->by, f->bw, f->bh, uv);
    flush(b);
    fbo_release(b, cur);
}

// text-shadow with blur: glyphs -> FBO -> blur -> composite.
static void draw_text_shadow_blurred(GlBackend *b, const DlItem *it) {
    const float pad = ceilf(it->u.text_shadow.blur * 1.5f * b->scale + 1.0f);
    float bounds[4];
    local_rect_bounds(b, dl_rect(it->u.text_shadow.bounds.x + it->u.text_shadow.dx, it->u.text_shadow.bounds.y + it->u.text_shadow.dy, it->u.text_shadow.bounds.w, it->u.text_shadow.bounds.h), bounds);
    bounds[0] = floorf(bounds[0]) - pad; bounds[1] = floorf(bounds[1]) - pad;
    bounds[2] = ceilf(bounds[2]) + pad;  bounds[3] = ceilf(bounds[3]) + pad;
    const int bw = (int) (bounds[2] - bounds[0]), bh = (int) (bounds[3] - bounds[1]);
    if (bw <= 0 || bh <= 0) return;

    const int entry = fbo_acquire(b, bw, bh);
    if (entry < 0) return;

    const Target saved_target = b->target;
    const ClipState saved_clip = b->clip;
    const float saved_op = b->opacity_mul;

    begin_offscreen(b, entry, b->target.ox + bounds[0], b->target.oy + bounds[1], bw, bh);
    b->opacity_mul = 1.0f;
    draw_glyphs(b, it->u.text_shadow.quads, it->u.text_shadow.n, it->u.text_shadow.color, it->u.text_shadow.dx, it->u.text_shadow.dy);
    flush(b);
    b->opacity_mul = saved_op;
    restore_target(b, &saved_target, &saved_clip);

    const int blurred = blur_entry(b, entry, bw, bh, it->u.text_shadow.blur * 0.5f * b->scale);
    fbo_release(b, entry);
    if (blurred < 0) return;

    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float zero[4] = {0, 0, 0, 0};
    flush(b);
    glUniformMatrix4fv(b->loc_cm, 1, GL_FALSE, identity);
    glUniform4fv(b->loc_cm_offset, 1, zero);
    glUniform1f(b->loc_opacity, b->opacity_mul);

    const uint8_t white[4] = {255, 255, 255, 255};
    float uv[4];
    batch_texture(b, b->fbos[blurred].tex, -1);
    GlbVertex t = vertex_template(b, MODE_GROUP, white);
    entry_uv(b, blurred, (float) bw, (float) bh, uv);
    emit_target_quad(b, &t, bounds[0], bounds[1], (float) bw, (float) bh, uv);
    flush(b);
    fbo_release(b, blurred);
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

GlBackend *glb_create(void) {
    GlBackend *b = calloc(1, sizeof(GlBackend));
    if (!b) return NULL;

    char *vert = concat(assets_path(), "shaders/html_ui.vert");
    char *frag = concat(assets_path(), "shaders/html_ui.frag");
    b->program = shader_compile(vert, frag);
    free(vert);
    free(frag);
    if (!b->program) {
        free(b);
        return NULL;
    }

    b->loc_viewport = glGetUniformLocation(b->program, "u_viewport");
    b->loc_tex = glGetUniformLocation(b->program, "u_tex");
    b->loc_gamma = glGetUniformLocation(b->program, "u_gamma");
    b->loc_cm = glGetUniformLocation(b->program, "u_cm");
    b->loc_cm_offset = glGetUniformLocation(b->program, "u_cm_offset");
    b->loc_opacity = glGetUniformLocation(b->program, "u_opacity");
    b->loc_blur_dir = glGetUniformLocation(b->program, "u_blur_dir");
    b->loc_stencil_pass = glGetUniformLocation(b->program, "u_stencil_pass");

    glGenVertexArrays(1, &b->vao);
    glBindVertexArray(b->vao);
    glGenBuffers(1, &b->vbo);
    glGenBuffers(1, &b->ibo);
    glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b->ibo);

    b->vbo_cap = 256 * 1024;
    b->ibo_cap = 64 * 1024;
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) b->vbo_cap, NULL, GL_STREAM_DRAW);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr) b->ibo_cap, NULL, GL_STREAM_DRAW);

    const GLsizei stride = sizeof(GlbVertex);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void *) offsetof(GlbVertex, x));
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *) offsetof(GlbVertex, u));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void *) offsetof(GlbVertex, rgba));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, (void *) offsetof(GlbVertex, p0));
    glEnableVertexAttribArray(4); glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (void *) offsetof(GlbVertex, p1));
    glEnableVertexAttribArray(5); glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride, (void *) offsetof(GlbVertex, p2));
    glEnableVertexAttribArray(6); glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, stride, (void *) offsetof(GlbVertex, clip));
    glEnableVertexAttribArray(7); glVertexAttribPointer(7, 1, GL_FLOAT, GL_FALSE, stride, (void *) offsetof(GlbVertex, clip_r));
    glEnableVertexAttribArray(8); glVertexAttribPointer(8, 1, GL_FLOAT, GL_FALSE, stride, (void *) offsetof(GlbVertex, mode));
    glBindVertexArray(0);

    // gamma LUT: identity until glb_set_text_gamma()
    uint8_t identity[8][256];
    for (int r = 0; r < 8; r++)
        for (int i = 0; i < 256; i++) identity[r][i] = (uint8_t) i;
    glGenTextures(1, &b->gamma_tex);
    glBindTexture(GL_TEXTURE_2D, b->gamma_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, 8, 0, GL_RED, GL_UNSIGNED_BYTE, identity);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &b->gradient_tex);
    glBindTexture(GL_TEXTURE_2D, b->gradient_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLB_GRADIENT_W, GLB_GRADIENT_ROWS, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    g_active_texture = 0;

    glUseProgram(b->program);
    glUniform1i(b->loc_tex, 0);
    glUniform1i(b->loc_gamma, 1);
    glUseProgram(0);
    g_active_shader = 0;

    b->batch_nearest = -1;
    return b;
}

void glb_destroy(GlBackend *b) {
    if (!b) return;
    for (int i = 0; i < b->n_fbos; i++) {
        glDeleteFramebuffers(1, &b->fbos[i].fbo);
        glDeleteRenderbuffers(1, &b->fbos[i].rbo);
        glDeleteTextures(1, &b->fbos[i].tex);
    }
    glDeleteTextures(1, &b->gamma_tex);
    glDeleteTextures(1, &b->gradient_tex);
    glDeleteBuffers(1, &b->vbo);
    glDeleteBuffers(1, &b->ibo);
    glDeleteVertexArrays(1, &b->vao);
    glDeleteProgram(b->program);
    free(b->verts);
    free(b->idx);
    free(b);
}

void glb_set_image_resolver(GlBackend *b, GlbImageResolver fn, void *user) {
    if (!b) return;
    b->image_resolver = fn;
    b->image_resolver_user = user;
}

void glb_set_text_gamma(GlBackend *b, const uint8_t lut[8][256]) {
    if (!b || !lut) return;
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, b->gamma_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 8, GL_RED, GL_UNSIGNED_BYTE, lut);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glActiveTexture(GL_TEXTURE0);
}

void glb_begin(GlBackend *b, int fb_w, int fb_h, float device_scale) {
    if (!b || b->in_frame) return;
    b->in_frame = 1;
    b->fb_w = fb_w;
    b->fb_h = fb_h;
    b->scale = device_scale > 0.0f ? device_scale : 1.0f;
    memset(&b->frame_stats, 0, sizeof(b->frame_stats));

    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &b->window_fbo);
    b->cull_was_on = glIsEnabled(GL_CULL_FACE);

    glUseProgram(b->program);
    g_active_shader = b->program;
    glBindVertexArray(b->vao);
    glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b->ibo);

    // orphan the streaming buffers
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) b->vbo_cap, NULL, GL_STREAM_DRAW);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr) b->ibo_cap, NULL, GL_STREAM_DRAW);
    b->vbo_off = b->ibo_off = 0;
    b->nv = b->ni = 0;
    b->batch_tex = 0;
    b->batch_nearest = -1;
    b->bound_tex = 0;

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, b->gamma_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glColorMask(1, 1, 1, 1);
    // Stencil clips count up from 0 (clip.stencil_ref), so the window target's
    // stencil must start at 0. Nothing else in the client writes or clears it
    // (the scene clears colour + depth only) and its initial contents are
    // undefined: on NVIDIA it is non-zero (llvmpipe happens to start at 0), which
    // failed the GL_EQUAL 0 test of the first stencil clip and dropped everything
    // inside nested rounded clips (class cards, attachment/reticle previews).
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    b->stencil_pass = 0;
    glUniform1i(b->loc_stencil_pass, 0);

    Target t = {(GLuint) b->window_fbo, fb_w, fb_h, 0.0f, 0.0f, -1};
    b->target = t;
    glViewport(0, 0, fb_w, fb_h);
    glUniform2f(b->loc_viewport, (float) fb_w, (float) fb_h);

    b->xform = dl_mat2d_identity();
    b->n_xform = 0;
    memset(&b->clip, 0, sizeof(b->clip));
    b->n_clips = 0;
    b->n_groups = 0;
    b->opacity_mul = 1.0f;
    b->skip_depth = 0;
}

void glb_submit(GlBackend *b, const DisplayList *list) {
    if (!b || !b->in_frame || !list) return;

    for (int i = 0; i < list->n; i++) {
        const DlItem *it = &list->items[i];

        if (b->skip_depth) {
            if (it->op == DL_PUSH_GROUP) b->skip_depth++;
            else if (it->op == DL_POP_GROUP && --b->skip_depth == 0) pop_group(b);
            continue;
        }

        switch (it->op) {
            case DL_RECT:
                draw_rrect(b, it->u.rect.rect, dl_radii_all(0.0f), it->u.rect.color);
                break;
            case DL_RRECT:
                draw_rrect(b, it->u.rrect.rect, it->u.rrect.radii, it->u.rrect.color);
                break;
            case DL_BORDER:
                draw_border(b, it);
                break;
            case DL_IMAGE:
                draw_image(b, it);
                break;
            case DL_GRADIENT:
                draw_gradient(b, it);
                break;
            case DL_GLYPHS:
                draw_glyphs(b, it->u.glyphs.quads, it->u.glyphs.n, it->u.glyphs.color, 0.0f, 0.0f);
                break;
            case DL_BOX_SHADOW:
                draw_box_shadow(b, it);
                break;
            case DL_TEXT_SHADOW:
                if (it->u.text_shadow.blur > 0.0f) draw_text_shadow_blurred(b, it);
                else draw_glyphs(b, it->u.text_shadow.quads, it->u.text_shadow.n, it->u.text_shadow.color, it->u.text_shadow.dx, it->u.text_shadow.dy);
                break;
            case DL_PUSH_CLIP_RECT:
                push_clip_rrect(b, it->u.clip_rect.rect, dl_radii_all(0.0f));
                break;
            case DL_PUSH_CLIP_RRECT:
                push_clip_rrect(b, it->u.clip_rrect.rect, it->u.clip_rrect.radii);
                break;
            case DL_PUSH_CLIP_POLY:
                push_clip_poly(b, it->u.clip_poly.pts, it->u.clip_poly.n);
                break;
            case DL_POP_CLIP:
                pop_clip(b);
                break;
            case DL_PUSH_TRANSFORM:
                if (b->n_xform < GLB_MAX_XFORMS) {
                    b->xform_stack[b->n_xform++] = b->xform;
                    b->xform = mat_mul(&b->xform, &it->u.transform.m);
                }
                break;
            case DL_POP_TRANSFORM:
                if (b->n_xform > 0) b->xform = b->xform_stack[--b->n_xform];
                break;
            case DL_PUSH_GROUP:
                push_group(b, list, i);
                break;
            case DL_POP_GROUP:
                pop_group(b);
                break;
            case DL_SCROLL_OFFSET: {
                const Mat2D t = {1, 0, 0, 1, it->u.scroll.dx, it->u.scroll.dy};
                b->xform = mat_mul(&b->xform, &t);
                break;
            }
            default:
                break;
        }
    }

    // Unbalanced lists: unwind so the next list starts clean.
    while (b->n_groups) pop_group(b);
    while (b->n_clips) pop_clip(b);
    b->xform = dl_mat2d_identity();
    b->n_xform = 0;
    b->skip_depth = 0;
    b->opacity_mul = 1.0f;
}

void glb_end(GlBackend *b) {
    if (!b || !b->in_frame) return;
    flush(b);
    b->in_frame = 0;
    b->stats = b->frame_stats;

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) b->window_fbo);
    glViewport(0, 0, b->fb_w, b->fb_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(1, 1, 1, 1);
    if (b->cull_was_on) glEnable(GL_CULL_FACE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    g_active_texture = 0;
}

GlbStats glb_stats(const GlBackend *b) {
    GlbStats s = {0, 0, 0};
    return b ? b->stats : s;
}
