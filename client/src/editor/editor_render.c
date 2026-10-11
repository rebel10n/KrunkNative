// Map editor GL: everything three.js drew in the editor besides the map meshes themselves
// (those are the game's own meshes from prefab.c, rendered by scene_render()):
//   ObjectInstance.boxShape        bounding box lines (green collidable, blue not, prefab lineCol)
//   tool prefab genGeo volumes     translucent zone / trigger boxes in their line colour
//   GridHelper / death barrier / sun marker
//   TransformControls              translate / rotate / scale gizmo, hover highlight, dragging
// plus ray picking (Raycaster against the bounding meshes) and the gizmo drag math.
//
// One program (assets/shaders/editor.{vert,frag}), one streaming VBO: geometry is built on
// the CPU in world space every frame. GL state is set explicitly and restored (depth test,
// depth mask, blend, cull) around the pass.
#include "editor_internal.h"
#include <gl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static unsigned int g_program, g_vao, g_vbo;
static GlUniform u_vp = GL_UNIFORM_INIT("view_projection");
static GlUniform u_model = GL_UNIFORM_INIT("model");
static GlUniform u_color = GL_UNIFORM_INIT("color");
static GlUniform u_fog_color = GL_UNIFORM_INIT("fog_color");
static GlUniform u_fog_near = GL_UNIFORM_INIT("fog_near");
static GlUniform u_fog_far = GL_UNIFORM_INIT("fog_far");
static GlUniform u_fog_enabled = GL_UNIFORM_INIT("fog_enabled");
static float g_vp[16];

typedef struct {
    float *v;
    size_t n, cap; // floats
} VertBuf;

static VertBuf g_lines, g_tris;

static void vb_push(VertBuf *b, const vec3 p) {
    if (b->n + 3 > b->cap) {
        const size_t cap = b->cap ? b->cap * 2 : 4096;
        float *n = realloc(b->v, cap * sizeof(float));
        if (!n) return;
        b->v = n;
        b->cap = cap;
    }
    b->v[b->n++] = p.x;
    b->v[b->n++] = p.y;
    b->v[b->n++] = p.z;
}

// ---------------------------------------------------------------------------
// math

static vec3 v3(const float x, const float y, const float z) { return (vec3) {x, y, z}; }
static vec3 vadd(const vec3 a, const vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static vec3 vsub(const vec3 a, const vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static vec3 vmul(const vec3 a, const float s) { return v3(a.x * s, a.y * s, a.z * s); }
static float vdot(const vec3 a, const vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static vec3 vcross(const vec3 a, const vec3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float vlen(const vec3 a) { return sqrtf(vdot(a, a)); }
static vec3 vnorm(const vec3 a) {
    const float l = vlen(a);
    return l > 1e-9f ? vmul(a, 1.0f / l) : a;
}

void ed_euler_matrix(const vec3 r, float m[9]) {
    // three.js Euler order XYZ: R = Rx * Ry * Rz (row major)
    const float a = cosf(r.x), b = sinf(r.x), c = cosf(r.y), d = sinf(r.y), e = cosf(r.z), f = sinf(r.z);
    const float ae = a * e, af = a * f, be = b * e, bf = b * f;
    m[0] = c * e;       m[1] = -c * f;      m[2] = d;
    m[3] = af + be * d; m[4] = ae - bf * d; m[5] = -b * c;
    m[6] = bf - ae * d; m[7] = be + af * d; m[8] = a * c;
}

static vec3 rmul(const float m[9], const vec3 v) {
    return v3(m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z, m[6] * v.x + m[7] * v.y + m[8] * v.z);
}

static vec3 rmul_t(const float m[9], const vec3 v) {
    return v3(m[0] * v.x + m[3] * v.y + m[6] * v.z, m[1] * v.x + m[4] * v.y + m[7] * v.z, m[2] * v.x + m[5] * v.y + m[8] * v.z);
}

static void mat3_mul(const float a[9], const float b[9], float out[9]) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
}

static void axis_angle(const vec3 axis, const float angle, float m[9]) {
    const vec3 a = vnorm(axis);
    const float c = cosf(angle), s = sinf(angle), t = 1 - c;
    m[0] = t * a.x * a.x + c;       m[1] = t * a.x * a.y - s * a.z; m[2] = t * a.x * a.z + s * a.y;
    m[3] = t * a.x * a.y + s * a.z; m[4] = t * a.y * a.y + c;       m[5] = t * a.y * a.z - s * a.x;
    m[6] = t * a.x * a.z - s * a.y; m[7] = t * a.y * a.z + s * a.x; m[8] = t * a.z * a.z + c;
}

static vec3 euler_from_matrix(const float m[9]) {
    // Euler.setFromRotationMatrix(m, "XYZ")
    vec3 r;
    const float m13 = fminf(fmaxf(m[2], -1.0f), 1.0f);
    r.y = asinf(m13);
    if (fabsf(m13) < 0.9999999f) {
        r.x = atan2f(-m[5], m[8]);
        r.z = atan2f(-m[1], m[0]);
    } else {
        r.x = atan2f(m[7], m[4]);
        r.z = 0.0f;
    }
    return r;
}

void ed_obj_world_box(const EditorObject *o, float m[16]) {
    // boxShape: position = boundingMesh.position + R * (0, s.y / 2, 0), rotation r, scale s
    const vec3 p = ed_obj_vec(o, "p", v3(0, 0, 0));
    const vec3 s = ed_obj_vec(o, "s", v3(1, 1, 1));
    const vec3 r = ed_obj_vec(o, "r", v3(0, 0, 0));
    float rm[9];
    ed_euler_matrix(r, rm);
    const vec3 c = vadd(p, rmul(rm, v3(0, s.y / 2, 0)));
    for (int row = 0; row < 3; row++) {
        m[row * 4 + 0] = rm[row * 3 + 0] * s.x;
        m[row * 4 + 1] = rm[row * 3 + 1] * s.y;
        m[row * 4 + 2] = rm[row * 3 + 2] * s.z;
    }
    m[3] = c.x;
    m[7] = c.y;
    m[11] = c.z;
    m[12] = m[13] = m[14] = 0.0f;
    m[15] = 1.0f;
}

static vec3 m4_point(const float m[16], const vec3 v) {
    return v3(m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3], m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7], m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11]);
}

// ---------------------------------------------------------------------------
// camera rays

static void camera_basis(vec3 *right, vec3 *up, vec3 *back) {
    // camera world rotation = Ry(yaw) * Rx(pitch)
    const float cy = cosf(g_ed.yaw), sy = sinf(g_ed.yaw), cp = cosf(g_ed.pitch), sp = sinf(g_ed.pitch);
    *right = v3(cy, 0, -sy);
    *up = v3(sy * sp, cp, cy * sp);
    *back = v3(sy * cp, -sp, cy * cp);
}

void editor_camera_dir(vec3 *forward, vec3 *right, vec3 *up) {
    vec3 r, u, b;
    camera_basis(&r, &u, &b);
    if (forward) *forward = vmul(b, -1);
    if (right) *right = r;
    if (up) *up = u;
}

void ed_view_ray(const double fb_x, const double fb_y, vec3 *origin, vec3 *dir) {
    const float w = (float) (g_ed.view_w > 0 ? g_ed.view_w : 1), h = (float) (g_ed.view_h > 0 ? g_ed.view_h : 1);
    // fb y grows down; the view rect's top-left is (view_x, fb_h - view_y - view_h) in fb coords
    const float top = (float) (g_ed.fb_h - g_ed.view_y - g_ed.view_h);
    const float nx = ((float) fb_x - (float) g_ed.view_x) / w * 2.0f - 1.0f;
    const float ny = -(((float) fb_y - top) / h * 2.0f - 1.0f);
    const float t = tanf(g_ed.camera.fov / g_ed.camera.zoom / 2.0f);
    vec3 r, u, b;
    camera_basis(&r, &u, &b);
    *origin = g_ed.camera.position;
    *dir = vnorm(vadd(vadd(vmul(r, nx * t * w / h), vmul(u, ny * t)), vmul(b, -1)));
}

// ray vs unit box [-0.5, 0.5]^3 under matrix m; returns t (world distance) or -1, face index
static float ray_box(const float m[16], const vec3 o, const vec3 d, int *face) {
    // invert the affine box matrix: columns are scaled rotation axes
    vec3 ax[3] = {v3(m[0], m[4], m[8]), v3(m[1], m[5], m[9]), v3(m[2], m[6], m[10])};
    const vec3 c = v3(m[3], m[7], m[11]);
    const vec3 rel = vsub(o, c);
    float lo = -1e30f, hi = 1e30f;
    int lo_face = 0;
    for (int i = 0; i < 3; i++) {
        const float l2 = vdot(ax[i], ax[i]);
        if (l2 < 1e-12f) return -1.0f;
        const float ro = vdot(rel, ax[i]) / l2, rd = vdot(d, ax[i]) / l2; // local coords
        if (fabsf(rd) < 1e-12f) {
            if (ro < -0.5f || ro > 0.5f) return -1.0f;
            continue;
        }
        float t0 = (-0.5f - ro) / rd, t1 = (0.5f - ro) / rd;
        int f0 = i * 2 + 1, f1 = i * 2; // entering the -side face (nx/ny/nz) or the + side
        if (t0 > t1) {
            const float tt = t0; t0 = t1; t1 = tt;
            const int ff = f0; f0 = f1; f1 = ff;
        }
        if (t0 > lo) {
            lo = t0;
            lo_face = f0;
        }
        if (t1 < hi) hi = t1;
        if (lo > hi) return -1.0f;
    }
    if (hi < 0) return -1.0f;
    if (face) *face = lo_face;
    return lo >= 0 ? lo : 0.0f;
}

int ed_pick(const double fb_x, const double fb_y, const int skip_invisible, EditorObject **out, vec3 *hit, int *face) {
    vec3 o, d;
    ed_view_ray(fb_x, fb_y, &o, &d);
    float best = 1e30f;
    EditorObject *best_obj = NULL;
    int best_face = 0;
    for (size_t i = 0; i < g_ed.count; i++) {
        EditorObject *obj = g_ed.objects[i];
        if (skip_invisible && editor_setting_bool("selectThroughInvis")) {
            const int invisible = ed_json_path(obj->data, "v") != NULL;
            if (invisible || obj->prefab == ED_PREFAB_PLACEHOLDER) continue;
        }
        float m[16];
        ed_obj_world_box(obj, m);
        int f = 0;
        const float t = ray_box(m, o, d, &f);
        if (t >= 0 && t < best) {
            best = t;
            best_obj = obj;
            best_face = f;
        }
    }
    if (!best_obj) return 0;
    if (out) *out = best_obj;
    if (hit) *hit = vadd(o, vmul(d, best));
    if (face) *face = best_face;
    return 1;
}

// ---------------------------------------------------------------------------
// drawing

void ed_render_init(void) {
    if (g_program) return;
    char *vert = concat(assets_path(), "shaders/editor.vert");
    char *frag = concat(assets_path(), "shaders/editor.frag");
    if (vert && frag) {
        ensure_asset_file(vert);
        ensure_asset_file(frag);
        g_program = shader_compile(vert, frag);
    }
    free(vert);
    free(frag);
    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *) 0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void ed_render_fini(void) {
    if (g_vbo) glDeleteBuffers(1, &g_vbo);
    if (g_vao) glDeleteVertexArrays(1, &g_vao);
    if (g_program) glDeleteProgram(g_program);
    g_vbo = g_vao = g_program = 0;
    gl_uniform_forget_program(0);
    free(g_lines.v);
    free(g_tris.v);
    memset(&g_lines, 0, sizeof(g_lines));
    memset(&g_tris, 0, sizeof(g_tris));
    g_active_shader = 0;
}

static void flush(VertBuf *b, const unsigned int mode, const vec4 color, const int fogged) {
    if (!b->n || !g_program) {
        b->n = 0;
        return;
    }
    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (g_active_shader != g_program) {
        glUseProgram(g_program);
        g_active_shader = g_program;
    }
    gl_uniform_mat4(&u_vp, g_program, GL_TRUE, g_vp);
    gl_uniform_mat4(&u_model, g_program, GL_TRUE, identity);
    gl_uniform_4f(&u_color, g_program, color.x, color.y, color.z, color.w);
    const RenderLighting *l = &g_render_lighting;
    gl_uniform_1i(&u_fog_enabled, g_program, fogged && l->fog_enabled ? 1 : 0);
    gl_uniform_3f(&u_fog_color, g_program, l->fog_color.x, l->fog_color.y, l->fog_color.z);
    gl_uniform_1f(&u_fog_near, g_program, l->fog_near);
    gl_uniform_1f(&u_fog_far, g_program, l->fog_far);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, (long) (b->n * sizeof(float)), b->v, GL_STREAM_DRAW);
    glDrawArrays(mode, 0, (int) (b->n / 3));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    b->n = 0;
}

static const vec3 k_corners[8] = {
    {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
    {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f},
};

static void push_box_lines(VertBuf *b, const float m[16]) {
    static const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    vec3 w[8];
    for (int i = 0; i < 8; i++) w[i] = m4_point(m, k_corners[i]);
    for (int i = 0; i < 12; i++) {
        vb_push(b, w[edges[i][0]]);
        vb_push(b, w[edges[i][1]]);
    }
}

static void push_box_tris(VertBuf *b, const float m[16]) {
    static const int quads[6][4] = {{1, 2, 6, 5}, {0, 4, 7, 3}, {3, 7, 6, 2}, {0, 1, 5, 4}, {4, 5, 6, 7}, {0, 3, 2, 1}};
    vec3 w[8];
    for (int i = 0; i < 8; i++) w[i] = m4_point(m, k_corners[i]);
    for (int i = 0; i < 6; i++) {
        vb_push(b, w[quads[i][0]]); vb_push(b, w[quads[i][1]]); vb_push(b, w[quads[i][2]]);
        vb_push(b, w[quads[i][0]]); vb_push(b, w[quads[i][2]]); vb_push(b, w[quads[i][3]]);
    }
}

static vec4 hex_rgba(const int hex, const float a) {
    vec4 c = hex_to_vec(hex);
    c.w = a;
    return c;
}

// per-colour line batches for the bounding boxes
#define BOX_COLORS 32
static struct { int hex; VertBuf lines; } g_box_batches[BOX_COLORS];
static int g_box_batch_count;

static VertBuf *box_batch(const int hex) {
    for (int i = 0; i < g_box_batch_count; i++) if (g_box_batches[i].hex == hex) return &g_box_batches[i].lines;
    if (g_box_batch_count == BOX_COLORS) return &g_box_batches[0].lines;
    g_box_batches[g_box_batch_count].hex = hex;
    return &g_box_batches[g_box_batch_count++].lines;
}

static void draw_object_boxes(void) {
    const EditorObject *sel = g_ed.selected;
    for (size_t i = 0; i < g_ed.count; i++) {
        const EditorObject *o = g_ed.objects[i];
        const EditorPrefab *p = &editor_prefabs[o->prefab];
        if (p->flags & EP_HIDEBOUNDINGBOX) continue;
        float m[16];
        ed_obj_world_box(o, m);
        int hex;
        if (p->line_col >= 0) hex = p->line_col;
        else {
            cJSON *c = ed_prop_get(o, "collidable");
            hex = (p->flags & EP_TOOL) || cJSON_IsTrue(c) ? 0x00ff00 : 0x0000ff; // boundingCollidable / Noncollidable
            cJSON_Delete(c);
        }
        if (o->highlighted) hex = 0xffffff;
        if (o == sel) continue; // drawn last, on top
        push_box_lines(box_batch(hex), m);
    }
    for (int i = 0; i < g_box_batch_count; i++) flush(&g_box_batches[i].lines, GL_LINES, hex_rgba(g_box_batches[i].hex, 1.0f), 1);
    g_box_batch_count = 0;
}

static void draw_tool_volumes(void) {
    // tool / zone prefabs: their generated cube at prefab.opacity in the line colour (the
    // game hides them; the editor shows where they are)
    glDepthMask(GL_FALSE);
    for (size_t i = 0; i < g_ed.count; i++) {
        const EditorObject *o = g_ed.objects[i];
        const EditorPrefab *p = &editor_prefabs[o->prefab];
        // generateCube(..., {transparent}) with white vertex colours at prefab.opacity: the
        // faint zone volumes (objectives 0.2, score/death/... zones 0.3, pickups 0.1)
        if (o->mesh || o->helper || !(p->flags & EP_GENGEO) || p->opacity >= 1.0f || p->opacity < 0.01f) continue;
        if (ed_json_path(o->data, "v")) continue;
        float m[16];
        ed_obj_world_box(o, m);
        push_box_tris(&g_tris, m);
        flush(&g_tris, GL_TRIANGLES, hex_rgba(0xffffff, p->opacity), 1);
    }
    glDepthMask(GL_TRUE);
}

static void draw_real_hitboxes(void) {
    // ObjectInstance.realHitbox (Settings > Show Real Hitbox): the unrotated collision box
    // (boundingMesh scale + position, no rotation) of rotated collidable objects, red at 0.2
    if (!editor_setting_bool("showRealHitbox")) return;
    glDepthMask(GL_FALSE);
    for (size_t i = 0; i < g_ed.count; i++) {
        const EditorObject *o = g_ed.objects[i];
        const char *type = editor_prefabs[o->prefab].name;
        if (!strcmp(type, "PLACEHOLDER") || !strcmp(type, "SIGN")) continue;
        const vec3 r = ed_obj_vec(o, "r", v3(0, 0, 0));
        if (r.x == 0 && r.y == 0 && r.z == 0) continue;
        cJSON *c = ed_prop_get(o, "collidable");
        const int col = cJSON_IsTrue(c);
        cJSON_Delete(c);
        if (!col) continue;
        // realHitbox.position = boundingMesh.position (the box centre), scale = s
        const vec3 p = ed_obj_vec(o, "p", v3(0, 0, 0)), s = ed_obj_vec(o, "s", v3(1, 1, 1));
        const float m[16] = {s.x, 0, 0, p.x, 0, s.y, 0, p.y + s.y / 2, 0, 0, s.z, p.z, 0, 0, 0, 1};
        push_box_tris(&g_tris, m);
    }
    flush(&g_tris, GL_TRIANGLES, hex_rgba(0xff0000, 0.2f), 1);
    glDepthMask(GL_TRUE);
}

static vec3 box_centre(const EditorObject *o) {
    float m[16];
    ed_obj_world_box(o, m);
    return v3(m[3], m[7], m[11]);
}

static void draw_trigger_connections(void) {
    // Settings > Object > Trigger Connections: v3.1.1 calls e.checkConnections(), which
    // its editor never defines (the switch only saves the setting). KrunkNative draws what
    // the name promises: a line from every interface-targeting object (TRIGGER "Target",
    // map key "tin") to each object whose interface ID ("in") matches, in the trigger's
    // yellow, drawn over the geometry; the target end gets a small cross.
    if (!editor_setting_bool("triggerCons")) return;
    for (size_t i = 0; i < g_ed.count; i++) {
        const EditorObject *src = g_ed.objects[i];
        if (!(editor_prefabs[src->prefab].flags & EP_CANTARGETINTERFACE)) continue;
        const int target = (int) ed_json_num(src->data, "tin", 0);
        if (!target) continue;
        const vec3 a = box_centre(src);
        for (size_t j = 0; j < g_ed.count; j++) {
            const EditorObject *dst = g_ed.objects[j];
            if (dst == src || !(editor_prefabs[dst->prefab].flags & EP_CANINTERFACE)) continue;
            if ((int) ed_json_num(dst->data, "in", 0) != target) continue;
            const vec3 b = box_centre(dst);
            vb_push(&g_lines, a);
            vb_push(&g_lines, b);
            for (int k = 0; k < 3; k++) {
                vec3 d = v3(0, 0, 0);
                (&d.x)[k] = 2.0f;
                vb_push(&g_lines, vsub(b, d));
                vb_push(&g_lines, vadd(b, d));
            }
        }
    }
    glDisable(GL_DEPTH_TEST);
    flush(&g_lines, GL_LINES, hex_rgba(0xffff00, 1.0f), 0);
    glEnable(GL_DEPTH_TEST);
}

static void draw_grid(void) {
    if (!editor_setting_bool("gridVisibility")) return;
    // THREE.GridHelper(size, size / spacing): lines every `spacing`, centre lines 0x444444,
    // the others 0x888888; Krunker darkens the first 12 colour floats of every 60 (0.26)
    const float size = (float) editor_setting_num("gridSize", 250);
    const float spacing = (float) editor_setting_num("gridSpacing", 10);
    const float opacity = (float) editor_setting_num("gridOpacity", 0.25);
    const int div = spacing > 0 ? (int) (size / spacing) : 25;
    if (div <= 0 || div > 2000) return;
    const float half = size / 2.0f, step = size / (float) div;
    VertBuf dark = {0};
    for (int i = 0; i <= div; i++) {
        const float k = -half + step * (float) i;
        // 60 floats = 20 vertices = 5 lines (x and z pairs of 2.5 grid lines): the 4 first
        // vertices of each group are darkened: every 5th grid line pair starts dark
        VertBuf *b = (i % 5 == 0) ? &dark : &g_lines;
        vb_push(b, v3(-half, 0, k)); vb_push(b, v3(half, 0, k));
        vb_push(b, v3(k, 0, -half)); vb_push(b, v3(k, 0, half));
    }
    glDepthMask(GL_TRUE);
    flush(&g_lines, GL_LINES, (vec4) {0.533f, 0.533f, 0.533f, opacity}, 1);
    flush(&dark, GL_LINES, (vec4) {0.26f, 0.26f, 0.26f, opacity}, 1);
    free(dark.v);
}

static void push_cone(VertBuf *b, vec3 base, vec3 dir, float len, float radius, int segments);

static void draw_arrows(void) {
    // ObjectInstance.arrowHelper (customDirection prefabs): ArrowHelper(dir, origin, 10,
    // 0xff00ff, 5, 4) at the box position + (0, s.y + 5, 0), dir (cos(d PI/2), 0, sin(d PI/2))
    for (size_t i = 0; i < g_ed.count; i++) {
        const EditorObject *o = g_ed.objects[i];
        if (!editor_prefab_has(o->prefab, EP_CUSTOMDIRECTION)) continue;
        const vec3 p = ed_obj_vec(o, "p", v3(0, 0, 0));
        const vec3 s = ed_obj_vec(o, "s", v3(1, 1, 1));
        const float t = (float) ed_json_num(o->data, "d", 0) * (float) M_PI / 2.0f;
        const vec3 dir = v3(cosf(t), 0, sinf(t));
        const vec3 origin = v3(p.x, p.y + s.y + 5.0f, p.z);
        vb_push(&g_lines, origin);
        vb_push(&g_lines, vadd(origin, vmul(dir, 5.0f)));
        push_cone(&g_tris, vadd(origin, vmul(dir, 5.0f)), dir, 5.0f, 2.0f, 5); // CylinderGeometry(0, 0.5, 1, 5)
    }
    flush(&g_lines, GL_LINES, hex_rgba(0xff00ff, 1.0f), 1);
    flush(&g_tris, GL_TRIANGLES, hex_rgba(0xff00ff, 1.0f), 1);
}

static void draw_death_barrier(void) {
    if (!editor_setting_bool("dbVisibility")) return;
    const float y = (float) ed_json_num(g_ed.map_config, "dthY", -100);
    const float h = 5000.0f;
    vb_push(&g_tris, v3(-h, y, -h)); vb_push(&g_tris, v3(h, y, -h)); vb_push(&g_tris, v3(h, y, h));
    vb_push(&g_tris, v3(-h, y, -h)); vb_push(&g_tris, v3(h, y, h)); vb_push(&g_tris, v3(-h, y, h));
    glDepthMask(GL_FALSE);
    flush(&g_tris, GL_TRIANGLES, hex_rgba(0xff0000, (float) editor_setting_num("dbOpacity", 0.5)), 0);
    glDepthMask(GL_TRUE);
}

static void push_circle(VertBuf *b, const vec3 c, const vec3 u, const vec3 v, const float r, const int segments) {
    for (int i = 0; i < segments; i++) {
        const float a0 = (float) i / (float) segments * 2.0f * (float) M_PI, a1 = (float) (i + 1) / (float) segments * 2.0f * (float) M_PI;
        vb_push(b, vadd(c, vadd(vmul(u, cosf(a0) * r), vmul(v, sinf(a0) * r))));
        vb_push(b, vadd(c, vadd(vmul(u, cosf(a1) * r), vmul(v, sinf(a1) * r))));
    }
}

static void draw_sun(void) {
    // Editor.sun: wireframe sphere (radius 5, scale 4) at the light position, in the light colour
    const float u = (float) M_PI * ((float) ed_json_num(g_ed.map_config, "sunAngY", 54) / -180.0f);
    const float t = (float) M_PI * ((float) ed_json_num(g_ed.map_config, "sunAngX", 90) / -180.0f);
    const float d = (float) ed_json_num(g_ed.map_config, "lightD", 500);
    const vec3 c = v3(d * cosf(t), d * sinf(t) * sinf(u), d * sinf(t) * cosf(u));
    for (int i = 0; i < 3; i++) {
        const float a = (float) i * (float) M_PI / 3.0f;
        push_circle(&g_lines, c, v3(cosf(a), 0, sinf(a)), v3(0, 1, 0), 20.0f, 6);
    }
    push_circle(&g_lines, c, v3(1, 0, 0), v3(0, 0, 1), 20.0f, 6);
    vec4 col = g_render_lighting.light_color;
    const cJSON *light = ed_json_path(g_ed.map_config, "light");
    if (cJSON_IsString(light)) parse_hex_color(light->valuestring, &col);
    else if (cJSON_IsNumber(light)) col = hex_to_vec((int) cJSON_GetNumberValue(light));
    col.w = 1.0f;
    flush(&g_lines, GL_LINES, col, 0);
}

// ---------------------------------------------------------------------------
// gizmo (TransformControls)

static vec3 gizmo_origin(void) { return ed_obj_vec(g_ed.selected, "p", v3(0, 0, 0)); }

static float gizmo_scale(void) {
    return vlen(vsub(gizmo_origin(), g_ed.camera.position)) / 6.0f; // distanceTo(camera) / 6 * size
}

static void gizmo_axes(vec3 axes[3]) {
    axes[0] = v3(1, 0, 0);
    axes[1] = v3(0, 1, 0);
    axes[2] = v3(0, 0, 1);
    const int local = g_ed.transform_space == ED_SPACE_LOCAL || g_ed.transform_type == ED_SCALE;
    if (local && g_ed.selected) {
        float m[9];
        ed_euler_matrix(ed_obj_vec(g_ed.selected, "r", v3(0, 0, 0)), m);
        for (int i = 0; i < 3; i++) axes[i] = rmul(m, axes[i]);
    }
}

static const int k_axis_hex[3] = {0xff0000, 0x00ff00, 0x0000ff};

static int project(const vec3 w, float *sx, float *sy) {
    const float *m = g_vp;
    const float x = m[0] * w.x + m[1] * w.y + m[2] * w.z + m[3];
    const float y = m[4] * w.x + m[5] * w.y + m[6] * w.z + m[7];
    const float ww = m[12] * w.x + m[13] * w.y + m[14] * w.z + m[15];
    if (ww <= 1e-5f) return 0;
    const float top = (float) (g_ed.fb_h - g_ed.view_y - g_ed.view_h);
    *sx = (float) g_ed.view_x + (x / ww * 0.5f + 0.5f) * (float) g_ed.view_w;
    *sy = top + (1.0f - (y / ww * 0.5f + 0.5f)) * (float) g_ed.view_h;
    return 1;
}

static float seg_dist(const float px, const float py, const float ax, const float ay, const float bx, const float by) {
    const float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
    t = fminf(fmaxf(t, 0), 1);
    const float cx = ax + dx * t - px, cy = ay + dy * t - py;
    return sqrtf(cx * cx + cy * cy);
}

int ed_gizmo_hit(const double fb_x, const double fb_y) {
    if (!g_ed.selected) return 0;
    vec3 axes[3];
    gizmo_axes(axes);
    const vec3 c = gizmo_origin();
    const float s = gizmo_scale();
    const float px = (float) fb_x, py = (float) fb_y;
    const float tol = 8.0f * (g_ed.css_to_fb > 0 ? g_ed.css_to_fb : 1.0f);
    float best = tol;
    int hit = 0;
    float cx, cy;
    if (!project(c, &cx, &cy)) return 0;
    if (g_ed.transform_type == ED_ROTATE) {
        for (int a = 0; a < 3; a++) {
            const vec3 u = axes[(a + 1) % 3], v = axes[(a + 2) % 3];
            for (int i = 0; i < 48; i++) {
                const float a0 = (float) i / 48.0f * 2.0f * (float) M_PI, a1 = (float) (i + 1) / 48.0f * 2.0f * (float) M_PI;
                float x0, y0, x1, y1;
                if (!project(vadd(c, vadd(vmul(u, cosf(a0) * s), vmul(v, sinf(a0) * s))), &x0, &y0)) continue;
                if (!project(vadd(c, vadd(vmul(u, cosf(a1) * s), vmul(v, sinf(a1) * s))), &x1, &y1)) continue;
                const float d = seg_dist(px, py, x0, y0, x1, y1);
                if (d < best) {
                    best = d;
                    hit = 1 << a;
                }
            }
        }
        return hit;
    }
    if (g_ed.transform_type == ED_TRANSLATE) {
        // plane handles: 0.15..0.445 squares between two axes
        for (int a = 0; a < 3; a++) {
            const vec3 u = axes[(a + 1) % 3], v = axes[(a + 2) % 3];
            const vec3 q = vadd(c, vadd(vmul(u, 0.295f * s), vmul(v, 0.295f * s)));
            float qx, qy;
            if (project(q, &qx, &qy) && fabsf(qx - px) < tol * 1.2f && fabsf(qy - py) < tol * 1.2f) return (1 << ((a + 1) % 3)) | (1 << ((a + 2) % 3));
        }
    }
    if (g_ed.transform_type == ED_SCALE && fabsf(cx - px) < tol && fabsf(cy - py) < tol) return 8; // XYZ uniform
    for (int a = 0; a < 3; a++) {
        float ex, ey;
        if (!project(vadd(c, vmul(axes[a], s)), &ex, &ey)) continue;
        const float d = seg_dist(px, py, cx, cy, ex, ey);
        if (d < best) {
            best = d;
            hit = 1 << a;
        }
    }
    return hit;
}

static void push_cone(VertBuf *b, vec3 base, vec3 dir, float len, float radius, const int segments) {
    vec3 u = vnorm(vcross(dir, fabsf(dir.y) < 0.9f ? v3(0, 1, 0) : v3(1, 0, 0)));
    const vec3 v = vcross(dir, u);
    const vec3 tip = vadd(base, vmul(dir, len));
    for (int i = 0; i < segments; i++) {
        const float a0 = (float) i / (float) segments * 2.0f * (float) M_PI, a1 = (float) (i + 1) / (float) segments * 2.0f * (float) M_PI;
        const vec3 p0 = vadd(base, vadd(vmul(u, cosf(a0) * radius), vmul(v, sinf(a0) * radius)));
        const vec3 p1 = vadd(base, vadd(vmul(u, cosf(a1) * radius), vmul(v, sinf(a1) * radius)));
        vb_push(b, tip); vb_push(b, p0); vb_push(b, p1);
        vb_push(b, base); vb_push(b, p1); vb_push(b, p0);
    }
}

static void push_cube(VertBuf *b, const vec3 c, const vec3 axes[3], const float h) {
    float m[16] = {0};
    for (int r = 0; r < 3; r++) {
        m[r * 4 + 0] = (&axes[0].x)[r] * h * 2;
        m[r * 4 + 1] = (&axes[1].x)[r] * h * 2;
        m[r * 4 + 2] = (&axes[2].x)[r] * h * 2;
    }
    m[3] = c.x; m[7] = c.y; m[11] = c.z; m[15] = 1;
    push_box_tris(b, m);
}

static void draw_gizmo(void) {
    if (!g_ed.selected) return;
    vec3 axes[3];
    gizmo_axes(axes);
    const vec3 c = gizmo_origin();
    const float s = gizmo_scale();
    const int hot = g_ed.gizmo.active ? g_ed.gizmo.axis : g_ed.gizmo.hover;
    glDisable(GL_DEPTH_TEST);
    for (int a = 0; a < 3; a++) {
        const vec4 col = hex_rgba((hot & (1 << a)) && !(hot & ~(1 << a)) ? 0xffff00 : k_axis_hex[a], 1.0f);
        if (g_ed.transform_type == ED_ROTATE) {
            push_circle(&g_lines, c, axes[(a + 1) % 3], axes[(a + 2) % 3], s, 64);
            flush(&g_lines, GL_LINES, col, 0);
            continue;
        }
        vb_push(&g_lines, c);
        vb_push(&g_lines, vadd(c, vmul(axes[a], g_ed.transform_type == ED_SCALE ? s * 0.8f : s)));
        flush(&g_lines, GL_LINES, col, 0);
        if (g_ed.transform_type == ED_TRANSLATE) push_cone(&g_tris, vadd(c, vmul(axes[a], s * 0.8f)), axes[a], s * 0.2f, s * 0.05f, 12);
        else push_cube(&g_tris, vadd(c, vmul(axes[a], s * 0.8f)), axes, s * 0.0625f);
        flush(&g_tris, GL_TRIANGLES, col, 0);
    }
    if (g_ed.transform_type == ED_TRANSLATE) {
        static const int plane_hex[3] = {0x00ffff, 0xff00ff, 0xffff00}; // YZ cyan, XZ magenta, XY yellow
        for (int a = 0; a < 3; a++) {
            const vec3 u = axes[(a + 1) % 3], v = axes[(a + 2) % 3];
            const int mask = (1 << ((a + 1) % 3)) | (1 << ((a + 2) % 3));
            const vec3 p0 = vadd(c, vadd(vmul(u, 0.15f * s), vmul(v, 0.15f * s)));
            const vec3 p1 = vadd(p0, vmul(u, 0.295f * s)), p2 = vadd(p1, vmul(v, 0.295f * s)), p3 = vadd(p0, vmul(v, 0.295f * s));
            vb_push(&g_tris, p0); vb_push(&g_tris, p1); vb_push(&g_tris, p2);
            vb_push(&g_tris, p0); vb_push(&g_tris, p2); vb_push(&g_tris, p3);
            flush(&g_tris, GL_TRIANGLES, hex_rgba(hot == mask ? 0xffff00 : plane_hex[a], 0.5f), 0);
        }
    }
    if (g_ed.transform_type == ED_SCALE) {
        push_cube(&g_tris, c, axes, s * 0.0625f);
        flush(&g_tris, GL_TRIANGLES, hex_rgba(hot == 8 ? 0xffff00 : 0xffffff, 1.0f), 0);
    }
    // the gizmo origin (TransformControls' small blue sphere in the screenshot is the
    // translate "XYZ" handle)
    if (g_ed.transform_type == ED_TRANSLATE) {
        push_cube(&g_tris, c, axes, s * 0.04f);
        flush(&g_tris, GL_TRIANGLES, hex_rgba(0x4040ff, 1.0f), 0);
    }
    glEnable(GL_DEPTH_TEST);
}

// Before the map meshes: three.js draws the (transparent, depth-writing) GridHelper first, so
// coplanar map faces at y = 0 cover it (LessEqual) and it only shows where nothing is built.
void ed_render_grid(void) {
    ed_render_init();
    if (!g_program) return;
    mat4x4(g_ed.camera.projection_matrix, g_ed.camera.world_inverse_matrix, g_vp);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    draw_grid();
    glDepthFunc(GL_LESS);
}

void ed_render_overlays(void) {
    ed_render_init();
    if (!g_program) return;
    mat4x4(g_ed.camera.projection_matrix, g_ed.camera.world_inverse_matrix, g_vp);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_TRUE);
#ifndef KN_GLES
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
#endif
    draw_tool_volumes();
    draw_real_hitboxes();
    draw_object_boxes();
    draw_trigger_connections();
    draw_arrows();
    draw_death_barrier();
    draw_sun();
    if (g_ed.selected && !(editor_prefabs[g_ed.selected->prefab].flags & EP_HIDEBOUNDINGBOX)) {
        float m[16];
        ed_obj_world_box(g_ed.selected, m);
        push_box_lines(&g_lines, m);
        cJSON *c = ed_prop_get(g_ed.selected, "collidable");
        const EditorPrefab *p = &editor_prefabs[g_ed.selected->prefab];
        const int hex = p->line_col >= 0 ? p->line_col : (p->flags & EP_TOOL) || cJSON_IsTrue(c) ? 0x00ff00 : 0x0000ff;
        cJSON_Delete(c);
        flush(&g_lines, GL_LINES, hex_rgba(hex, 1.0f), 0);
    }
    draw_gizmo();
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

// ---------------------------------------------------------------------------
// dragging

static int plane_hit(const vec3 o, const vec3 d, const vec3 p0, const vec3 n, vec3 *hit) {
    const float den = vdot(d, n);
    if (fabsf(den) < 1e-6f) return 0;
    const float t = vdot(vsub(p0, o), n) / den;
    if (t < 0) return 0;
    *hit = vadd(o, vmul(d, t));
    return 1;
}

static int single_axis(const int mask) { return mask == 1 ? 0 : mask == 2 ? 1 : mask == 4 ? 2 : -1; }

static vec3 drag_plane_normal(const int mask, const vec3 axes[3]) {
    vec3 o, d;
    ed_view_ray(g_ed.mouse_x, g_ed.mouse_y, &o, &d);
    const int a = single_axis(mask);
    if (g_ed.transform_type == ED_ROTATE && a >= 0) return axes[a];
    if (a >= 0) {
        // the plane through the axis that faces the camera most
        const vec3 n = vsub(d, vmul(axes[a], vdot(d, axes[a])));
        return vlen(n) > 1e-6f ? vnorm(n) : axes[(a + 1) % 3];
    }
    if (mask == 8) return vnorm(vmul(d, -1));
    for (int i = 0; i < 3; i++) if (!(mask & (1 << i))) return axes[i];
    return v3(0, 1, 0);
}

void ed_gizmo_begin(const double fb_x, const double fb_y, const int axis) {
    EditorObject *o = g_ed.selected;
    if (!o || !axis) return;
    vec3 axes[3];
    gizmo_axes(axes);
    EditorGizmo *g = &g_ed.gizmo;
    g->axis = axis;
    g->start_pos = ed_obj_vec(o, "p", v3(0, 0, 0));
    g->start_size = ed_obj_vec(o, "s", v3(1, 1, 1));
    g->start_rot = ed_obj_vec(o, "r", v3(0, 0, 0));
    g->plane_normal = drag_plane_normal(axis, axes);
    vec3 ro, rd;
    ed_view_ray(fb_x, fb_y, &ro, &rd);
    if (!plane_hit(ro, rd, g->start_pos, g->plane_normal, &g->start_hit)) return;
    g->active = 1;
}

static float snap(const float v, const float step) {
    return step > 0 ? roundf(v / step) * step : v;
}

void ed_gizmo_drag(const double fb_x, const double fb_y) {
    EditorGizmo *g = &g_ed.gizmo;
    EditorObject *o = g_ed.selected;
    if (!g->active || !o) return;
    vec3 ro, rd, hit;
    ed_view_ray(fb_x, fb_y, &ro, &rd);
    if (!plane_hit(ro, rd, g->start_pos, g->plane_normal, &hit)) return;
    vec3 axes[3];
    gizmo_axes(axes);
    const int snapping = g_ed.snapping && !(g_ed.mods & GLFW_MOD_CONTROL);
    const vec3 delta = vsub(hit, g->start_hit);
    const int a = single_axis(g->axis);
    if (g_ed.transform_type == ED_TRANSLATE) {
        vec3 move = delta;
        if (a >= 0) move = vmul(axes[a], vdot(delta, axes[a]));
        vec3 p = vadd(g->start_pos, move);
        if (snapping) {
            const float t = (float) editor_setting_num("translationSnapping", 1);
            p = v3(snap(p.x, t), snap(p.y, t), snap(p.z, t));
        }
        ed_obj_set_vec(o, "p", p);
    } else if (g_ed.transform_type == ED_SCALE) {
        vec3 s = g->start_size;
        if (g->axis == 8) {
            // XYZ: T = 1 + b.y / max(j): uniform scale from the vertical movement
            const float mx = fmaxf(s.x, fmaxf(s.y, s.z));
            const float k = 1.0f + vdot(delta, axes[1]) / (mx > 0 ? mx : 1);
            s = vmul(s, k);
        } else if (a >= 0) {
            const float d = vdot(delta, axes[a]);
            (&s.x)[a] += d;
            if (editor_prefabs[o->prefab].flags & EP_FAKESCALE && a != 1) {
                // fakeScale: X or Z drags scale both horizontal axes
                s.x = s.z = (&g->start_size.x)[a] + d;
            }
            if (snapping) {
                const float t = (float) editor_setting_num("scaleSnapping", 1);
                (&s.x)[a] = snap((&s.x)[a], t);
            }
        }
        ed_obj_set_vec(o, "s", s);
    } else if (g_ed.transform_type == ED_ROTATE && a >= 0) {
        const vec3 v0 = vsub(g->start_hit, g->start_pos), v1 = vsub(hit, g->start_pos);
        float angle = atan2f(vdot(vcross(v0, v1), axes[a]), vdot(v0, v1));
        if (snapping) {
            const float t = (float) editor_setting_num("rotationSnapping", 1) * (float) M_PI / 180.0f;
            angle = snap(angle, t);
        }
        float r0[9], ra[9], out[9];
        ed_euler_matrix(g->start_rot, r0);
        if (g_ed.transform_space == ED_SPACE_LOCAL) {
            static const vec3 basis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
            axis_angle(basis[a], angle, ra);
            mat3_mul(r0, ra, out);
        } else {
            axis_angle(axes[a], angle, ra);
            mat3_mul(ra, r0, out);
        }
        ed_obj_set_vec(o, "r", euler_from_matrix(out));
    }
    ed_obj_rebuild(o);
    g_ed.dirty_storage = 1;
    (void) rmul_t;
}

void ed_gizmo_end(void) {
    if (!g_ed.gizmo.active) return;
    g_ed.gizmo.active = 0;
    g_ed.dirty_panel = 1; // transformControl "mouseUp": updateObjConfigGUI
}
