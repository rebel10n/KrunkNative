// Nettspend (client/include/nettspend.h): the Purple Drank's cup and pour-up bottle, the Money
// Spread's cash stack (procedural vertex-coloured geometry, like the Piss's genHose in loaders.c)
// and the "laced" post-effect's state. The weapon tables, the class and the laced roll are
// shared/'s (weapons.h, kn_drank.c, kn_money_spread.c, drank.h); this file keys on the ids.
#include <gl.h>
#include <nettspend.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

NettspendWeapon nettspend_weapon_kind(const int weapon_id) {
    if (weapon_id == WEAPON_ID_DRANK) return NETTSPEND_WEAPON_DRANK;
    if (weapon_id == WEAPON_ID_MONEY_SPREAD) return NETTSPEND_WEAPON_MONEY_SPREAD;
    return NETTSPEND_WEAPON_NONE;
}

const char *nettspend_weapon_icon_src(const char *icon, char *out, const size_t size) {
    if (!icon || strncmp(icon, NETTSPEND_ICON_PREFIX, sizeof(NETTSPEND_ICON_PREFIX) - 1) || !out || !size) return NULL;
    snprintf(out, size, "img/weapons/%s.png", icon);
    return out;
}

const char *nettspend_class_icon_src(const int icon_index, char *out, const size_t size) {
    if (icon_index < CLASS_COUNT || !out || !size) return NULL;
    snprintf(out, size, "img/classes/icon_%d.png", icon_index);
    return out;
}

// ---------------------------------------------------------------- geometry

// A growing vertex-coloured mesh: positions + UVs (attribute 0 / 1, the `vertex` layout) and a
// colour per vertex (attribute 2, like prefab.c's ladder), indexed triangles.
typedef struct {
    vertex *v;
    vec3 *c;
    unsigned int *idx;
    size_t nv, ni, cap_v, cap_i;
    int failed;
} GeoBuilder;

static int gb_reserve(GeoBuilder *b, const size_t add_v, const size_t add_i) {
    if (b->failed) return 0;
    if (b->nv + add_v > b->cap_v) {
        size_t cap = b->cap_v ? b->cap_v * 2 : 256;
        while (cap < b->nv + add_v) cap *= 2;
        vertex *v = realloc(b->v, cap * sizeof(vertex));
        vec3 *c = realloc(b->c, cap * sizeof(vec3));
        if (!v || !c) {
            free(v ? v : b->v);
            free(c ? c : b->c);
            b->v = NULL;
            b->c = NULL;
            b->failed = 1;
            return 0;
        }
        b->v = v;
        b->c = c;
        b->cap_v = cap;
    }
    if (b->ni + add_i > b->cap_i) {
        size_t cap = b->cap_i ? b->cap_i * 2 : 512;
        while (cap < b->ni + add_i) cap *= 2;
        unsigned int *idx = realloc(b->idx, cap * sizeof(unsigned int));
        if (!idx) {
            b->failed = 1;
            return 0;
        }
        b->idx = idx;
        b->cap_i = cap;
    }
    return 1;
}

static vec3 rgb(const int hex) {
    return (vec3) {(float) ((hex >> 16) & 255) / 255.0f, (float) ((hex >> 8) & 255) / 255.0f, (float) (hex & 255) / 255.0f};
}

static vec3 rgb_scale(const vec3 c, const float s) {
    return (vec3) {fminf(c.x * s, 1.0f), fminf(c.y * s, 1.0f), fminf(c.z * s, 1.0f)};
}

static unsigned int gb_vertex(GeoBuilder *b, const vec3 p, const vec3 color) {
    b->v[b->nv] = (vertex) {p, {0.0f, 0.0f}};
    b->c[b->nv] = color;
    return (unsigned int) b->nv++;
}

static void gb_tri(GeoBuilder *b, const unsigned int a, const unsigned int c, const unsigned int d) {
    b->idx[b->ni++] = a;
    b->idx[b->ni++] = c;
    b->idx[b->ni++] = d;
}

// A tube along z from z0 (radius r0, colour c0) to z1 (radius r1, colour c1); winding so the
// outside is the front face when r grows toward -z or stays (both cups and the bottle have z1 < z0)
static void gb_frustum(GeoBuilder *b, const float z0, const float r0, const vec3 c0, const float z1, const float r1, const vec3 c1, const int sides) {
    if (!gb_reserve(b, (size_t) (sides + 1) * 2, (size_t) sides * 6)) return;
    const unsigned int base = (unsigned int) b->nv;
    for (int e = 0; e < 2; e++) {
        for (int x = 0; x <= sides; x++) {
            const float th = (float) x / (float) sides * 2.0f * (float) M_PI;
            const float r = e ? r1 : r0, z = e ? z1 : z0;
            gb_vertex(b, (vec3) {r * sinf(th), r * cosf(th), z}, e ? c1 : c0);
        }
    }
    for (int x = 0; x < sides; x++) {
        const unsigned int a = base + (unsigned int) x, c = base + (unsigned int) (sides + 1 + x);
        gb_tri(b, a, a + 1, c);
        gb_tri(b, c, a + 1, c + 1);
    }
}

// A disc at z of radius r; `up` 1: faces -z (the open top of a cup, seen from above), else +z
static void gb_disc(GeoBuilder *b, const float z, const float r, const vec3 color, const int sides, const int up) {
    if (!gb_reserve(b, (size_t) sides + 1, (size_t) sides * 3)) return;
    const unsigned int centre = gb_vertex(b, (vec3) {0.0f, 0.0f, z}, color);
    const unsigned int base = (unsigned int) b->nv;
    for (int x = 0; x < sides; x++) {
        const float th = (float) x / (float) sides * 2.0f * (float) M_PI;
        gb_vertex(b, (vec3) {r * sinf(th), r * cosf(th), z}, color);
    }
    for (int x = 0; x < sides; x++) {
        const unsigned int a = base + (unsigned int) x, c = base + (unsigned int) ((x + 1) % sides);
        if (up) gb_tri(b, centre, a, c);
        else gb_tri(b, centre, c, a);
    }
}

// A box around `centre` spanned by the half-axes ex / ey / ez (any orientation): the +-ez faces
// in `cap` (a bill's printed face), the four others in `side` (the stack's paper edge)
static void gb_box(GeoBuilder *b, const vec3 centre, const vec3 ex, const vec3 ey, const vec3 ez, const vec3 cap, const vec3 side) {
    if (!gb_reserve(b, 24, 36)) return;
    const vec3 axes[3] = {ex, ey, ez};
    for (int axis = 0; axis < 3; axis++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            const vec3 n = axes[axis], u = axes[(axis + 1) % 3], w = axes[(axis + 2) % 3];
            const vec3 color = axis == 2 ? cap : side;
            const unsigned int base = (unsigned int) b->nv;
            for (int k = 0; k < 4; k++) {
                const float su = (k & 1) ? 1.0f : -1.0f, sw = (k & 2) ? 1.0f : -1.0f;
                const vec3 p = {centre.x + n.x * (float) sign + u.x * su + w.x * sw, centre.y + n.y * (float) sign + u.y * su + w.y * sw,
                                centre.z + n.z * (float) sign + u.z * su + w.z * sw};
                gb_vertex(b, p, color);
            }
            // the face's outward side: (u x w) . n * sign > 0 picks the winding
            const vec3 cross = {u.y * w.z - u.z * w.y, u.z * w.x - u.x * w.z, u.x * w.y - u.y * w.x};
            const float out = (cross.x * n.x + cross.y * n.y + cross.z * n.z) * (float) sign;
            if (out > 0.0f) {
                gb_tri(b, base, base + 1, base + 2);
                gb_tri(b, base + 2, base + 1, base + 3);
            } else {
                gb_tri(b, base, base + 2, base + 1);
                gb_tri(b, base + 1, base + 2, base + 3);
            }
        }
    }
}

// Uploads the builder as a Geometry (owned by the cache below, never freed) and frees its arrays.
static Geometry *gb_upload(GeoBuilder *b) {
    Geometry *geometry = b->failed || !b->nv ? NULL : calloc(1, sizeof(Geometry));
    if (geometry) {
        unsigned int vbo, color_vbo;
        glGenVertexArrays(1, &geometry->vao);
        glGenBuffers(1, &geometry->ebo);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &color_vbo);
        glBindVertexArray(geometry->vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, (long) (b->nv * sizeof(vertex)), b->v, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), NULL);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), (void *) (3 * sizeof(float)));
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glBindBuffer(GL_ARRAY_BUFFER, color_vbo);
        glBufferData(GL_ARRAY_BUFFER, (long) (b->nv * sizeof(vec3)), b->c, GL_STATIC_DRAW);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(vec3), NULL);
        glEnableVertexAttribArray(2);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long) (b->ni * sizeof(unsigned int)), b->idx, GL_STATIC_DRAW);
        glBindVertexArray(0);
        geometry->index_count = (int) b->ni;
        geometry_set_bounds(geometry, b->v, b->nv);
    }
    free(b->v);
    free(b->c);
    free(b->idx);
    memset(b, 0, sizeof(*b));
    return geometry;
}

#define CUP_SIDES 18

// The lean cup: two nested styrofoam cups (the outer one's rim a little below the inner one's: the
// "double cup"), a purple liquid surface just under the top rim. Axis z, rim at -length / 2.
static Geometry *cup_geometry(void) {
    static Geometry *cached;
    if (cached) return cached;
    GeoBuilder b = {0};
    const float h = NETTSPEND_CUP_LENGTH * 0.5f, rim = NETTSPEND_CUP_RADIUS, base = rim * 0.72f;
    const float k = NETTSPEND_CUP_LENGTH / 0.62f; // the details were drawn on a 0.62 cup
    const vec3 foam = rgb(0xf4f2f8), foam_dark = rgb(0xd8d3e4), lip = rgb(0xc9c3d8);
    const vec3 lean = rgb(NETTSPEND_DRANK_COLOR);
    // outer cup: base at +h, its rim 0.11 (x k) below the top
    gb_frustum(&b, h, base, foam_dark, -h + 0.11f * k, rim - 0.008f * k, foam, CUP_SIDES);
    gb_frustum(&b, -h + 0.11f * k, rim - 0.008f * k, foam, -h + (0.11f - 0.025f) * k, rim + 0.012f * k, lip, CUP_SIDES); // its lip
    gb_frustum(&b, -h + (0.11f - 0.025f) * k, rim + 0.012f * k, lip, -h + (0.11f - 0.04f) * k, rim - 0.004f * k, foam, CUP_SIDES);
    // inner cup: nested, its rim is the top
    gb_frustum(&b, h - 0.09f * k, base * 1.02f, foam_dark, -h + 0.03f * k, rim, foam, CUP_SIDES);
    gb_frustum(&b, -h + 0.03f * k, rim, foam, -h, rim + 0.014f * k, lip, CUP_SIDES); // the top lip
    gb_disc(&b, h, base, foam_dark, CUP_SIDES, 0);                                   // the bottom
    gb_disc(&b, -h + 0.055f * k, rim - 0.006f * k, lean, CUP_SIDES, 1);               // the lean
    cached = gb_upload(&b);
    return cached;
}

// The pour-up bottle: a soda bottle (body, label band, shoulder, open neck). Axis z, mouth at
// -length / 2.
static Geometry *bottle_geometry(void) {
    static Geometry *cached;
    if (cached) return cached;
    GeoBuilder b = {0};
    const float h = NETTSPEND_BOTTLE_LENGTH * 0.5f, k = NETTSPEND_BOTTLE_LENGTH / 0.9f; // drawn on a 0.9 bottle
    const vec3 soda = rgb(0xc62d4a), soda_dark = rgb(0x8e1f35), label = rgb(0xf6efe6), label_purple = rgb(NETTSPEND_DRANK_COLOR), neck = rgb(0xd94d66);
    const float r = 0.125f * k;
    gb_disc(&b, h, r, soda_dark, CUP_SIDES, 0);                                                     // the bottom
    gb_frustum(&b, h, r, soda_dark, h - 0.34f * k, r, soda, CUP_SIDES);                             // lower body
    gb_frustum(&b, h - 0.34f * k, r + 0.004f * k, label, h - 0.5f * k, r + 0.004f * k, label, CUP_SIDES); // the label
    gb_frustum(&b, h - 0.5f * k, r + 0.006f * k, label_purple, h - 0.55f * k, r + 0.006f * k, label_purple, CUP_SIDES); // its purple stripe
    gb_frustum(&b, h - 0.55f * k, r, soda, h - 0.62f * k, r, soda, CUP_SIDES);                      // upper body
    gb_frustum(&b, h - 0.62f * k, r, soda, h - 0.76f * k, 0.055f * k, neck, CUP_SIDES);             // the shoulder
    gb_frustum(&b, h - 0.76f * k, 0.055f * k, neck, -h, 0.052f * k, neck, CUP_SIDES);               // the neck, open
    gb_disc(&b, -h + 0.01f * k, 0.045f * k, rgb(0x5a1230), CUP_SIDES, 1);                           // the dark inside of the mouth
    cached = gb_upload(&b);
    return cached;
}

// The cash stack: a banded brick of bills plus three fanned bills on top. Bills are stacked along
// z (a bill's face is the xy plane), the top at -length / 2.
static Geometry *cash_geometry(void) {
    static Geometry *cached;
    if (cached) return cached;
    GeoBuilder b = {0};
    const float h = NETTSPEND_CASH_LENGTH * 0.5f, k = NETTSPEND_CASH_LENGTH / 0.36f; // drawn on a 0.36 stack
    const vec3 bill = rgb(NETTSPEND_CASH_COLOR), bill_dark = rgb_scale(rgb(NETTSPEND_CASH_COLOR), 0.8f), edge = rgb(NETTSPEND_CASH_BAND_COLOR);
    const vec3 band = rgb(0xfaf6ea), band_ink = rgb(0x3b2a6b);
    // the brick: x +-0.27 k (a bill's length), y +-0.115 k (its width), z from +h (bottom) to -h + 0.06 k
    const float top = -h + 0.06f * k, bottom = h, half = (bottom - top) * 0.5f;
    const vec3 brick_c = {0.0f, 0.0f, (top + bottom) * 0.5f};
    gb_box(&b, brick_c, (vec3) {0.27f * k, 0.0f, 0.0f}, (vec3) {0.0f, 0.115f * k, 0.0f}, (vec3) {0.0f, 0.0f, half}, bill, edge);
    // the paper band around its middle, with an ink stripe
    gb_box(&b, brick_c, (vec3) {0.075f * k, 0.0f, 0.0f}, (vec3) {0.0f, 0.121f * k, 0.0f}, (vec3) {0.0f, 0.0f, half + 0.004f * k}, band, band);
    gb_box(&b, brick_c, (vec3) {0.02f * k, 0.0f, 0.0f}, (vec3) {0.0f, 0.124f * k, 0.0f}, (vec3) {0.0f, 0.0f, half + 0.006f * k}, band_ink, band_ink);
    // three fanned bills on top, each turned a little more about z
    for (int i = 0; i < 3; i++) {
        const float a = 0.22f * (float) (i + 1), c = cosf(a), s = sinf(a);
        const vec3 centre = {0.02f * k * (float) i, -0.01f * k * (float) i, top - 0.012f * k * (float) (i + 1)};
        gb_box(&b, centre, (vec3) {0.26f * k * c, 0.26f * k * s, 0.0f}, (vec3) {-0.11f * k * s, 0.11f * k * c, 0.0f}, (vec3) {0.0f, 0.0f, 0.006f * k},
               i & 1 ? bill_dark : bill, edge);
    }
    cached = gb_upload(&b);
    return cached;
}

static Mesh *colored_mesh(Geometry *geometry) {
    if (!geometry) return NULL;
    BasicMaterial *material = basic_material_init();
    if (!material) return NULL;
    material->use_vertex_color = 1;
    material->color = (vec4) {1.0f, 1.0f, 1.0f, 1.0f};
    material->double_sided = 1; // the inside of the cup shows past its rim
    Mesh *mesh = mesh_init(geometry, (Material *) material);
    if (!mesh) {
        material_fini((Material *) material);
        return NULL;
    }
    return mesh;
}

Mesh *nettspend_cup_mesh(void) {
    return colored_mesh(cup_geometry());
}

Mesh *nettspend_bottle_mesh(void) {
    return colored_mesh(bottle_geometry());
}

Mesh *nettspend_cash_mesh(void) {
    return colored_mesh(cash_geometry());
}

// ---------------------------------------------------------------- poses (first person)

// The cup in the fist, rim up and tilted a little toward the camera so the lean shows: rotation.x
// = PI / 2 maps the local -z (the rim) onto +y; the extra 0.2 leans the rim toward +z (the eye).
#define CUP_REST_OFFSET_X 0.0f
#define CUP_REST_OFFSET_Y 0.62f   // the base on top of the fist (half the hand's thickness 0.41 + the cup's lip)
#define CUP_REST_OFFSET_Z (-0.3f)
#define CUP_REST_TILT 0.2f
#define CASH_REST_OFFSET_Y 0.62f  // the stack lies on the fist
#define CASH_REST_OFFSET_Z (-0.32f)
// the pour: the cup rises into the middle of the view (toward the centre and the eye)
#define POUR_LIFT_X 0.45f
#define POUR_LIFT_Y 0.7f
#define POUR_LIFT_Z 0.25f

static float smooth01(const float x) {
    const float t = fminf(fmaxf(x, 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void nettspend_pose_cup_rest(Mesh *cup, const vec3 hand, const float mirror) {
    if (!cup) return;
    cup->transform.position = (vec3) {hand.x + CUP_REST_OFFSET_X * mirror, hand.y + CUP_REST_OFFSET_Y, hand.z + CUP_REST_OFFSET_Z};
    cup->transform.rotation = (vec3) {(float) M_PI * 0.5f + CUP_REST_TILT, 0.0f, 0.0f};
}

void nettspend_pose_cash(Mesh *cash, const vec3 hand, const float mirror) {
    if (!cash) return;
    // the stack lies on the fist, bills horizontal (the local z, the stacking axis, onto +y), turned
    // so its long side runs across the view
    cash->transform.position = (vec3) {hand.x, hand.y + CASH_REST_OFFSET_Y, hand.z + CASH_REST_OFFSET_Z};
    cash->transform.rotation = (vec3) {(float) M_PI * 0.5f, 0.0f, 0.35f * mirror};
}

float nettspend_pour_strength(const float progress) {
    if (progress < 0.0f || progress > 1.0f) return 0.0f;
    return smooth01((progress - 0.22f) / 0.18f) * (1.0f - smooth01((progress - 0.74f) / 0.1f));
}

// The pour-up in three beats of the reload: the bottle drops in from above (0..0.25), tips over
// the rim and pours (0.25..0.8), then lifts out (0.8..1). The cup drifts up and toward the screen
// centre meanwhile so the pour is in view.
float nettspend_pose_pour(Mesh *cup, Mesh *bottle, const vec3 hand, const float mirror, const float progress) {
    nettspend_pose_cup_rest(cup, hand, mirror);
    if (progress < 0.0f || progress > 1.0f) {
        if (bottle) bottle->visible = 0;
        return 0.0f;
    }
    const float in = smooth01(progress / 0.25f);                 // the bottle's descent
    const float out = smooth01((progress - 0.8f) / 0.2f);        // its exit
    const float pour = nettspend_pour_strength(progress);        // the tip
    const float lift = in * (1.0f - out);
    if (cup) {
        cup->transform.position.x -= POUR_LIFT_X * mirror * lift;
        cup->transform.position.y += POUR_LIFT_Y * lift;
        cup->transform.position.z += POUR_LIFT_Z * lift;
    }
    if (!bottle) return pour;
    bottle->visible = 1;
    // the rim's centre: the cup's local (0, 0, -h) through its rotation (PI / 2 + tilt about x)
    const float tilt = (float) M_PI * 0.5f + CUP_REST_TILT, h = NETTSPEND_CUP_LENGTH * 0.5f;
    const vec3 rim = {cup ? cup->transform.position.x : hand.x, (cup ? cup->transform.position.y : hand.y) + sinf(tilt) * h,
                      (cup ? cup->transform.position.z : hand.z) - cosf(tilt) * h};
    // rotation (PI / 2, psi, 0): the mouth (local -z) ends up along (-sin psi, cos psi, 0), so psi
    // past PI / 2 points it down over the cup; psi is mirrored for the left-handed
    const float psi0 = 2.05f * pour;
    const vec3 mouth_dir = {-sinf(psi0) * mirror, cosf(psi0), 0.0f};
    const float hb = NETTSPEND_BOTTLE_LENGTH * 0.5f;
    // where the mouth goes: over the rim while pouring, else above and beside the cup, with the
    // descent / exit lifting it out of view
    const float beside = 1.0f - pour;
    const vec3 mouth = {rim.x + (0.08f + 0.4f * beside) * mirror, rim.y + 0.16f + 0.3f * beside + 1.4f * (1.0f - lift), rim.z + 0.02f};
    bottle->transform.position = (vec3) {mouth.x - mouth_dir.x * hb, mouth.y - mouth_dir.y * hb, mouth.z - mouth_dir.z * hb};
    bottle->transform.rotation = (vec3) {(float) M_PI * 0.5f, psi0 * mirror, 0.0f};
    return pour;
}

// ---------------------------------------------------------------- laced

static struct {
    int active;
    float left_ms, total_ms;
    float clock_ms; // the shader's time, runs while active
} laced;

void nettspend_laced_start(const float duration_ms) {
    const float ms = fminf(fmaxf(duration_ms, 0.0f), NETTSPEND_LACED_MAX_MS);
    if (ms <= 0.0f) return;
    if (!laced.active) laced.clock_ms = 0.0f;
    // a second dose while high keeps the longer remainder, the fade-in never restarts
    const float elapsed = laced.active ? laced.total_ms - laced.left_ms : 0.0f;
    laced.left_ms = fmaxf(laced.active ? laced.left_ms : 0.0f, ms);
    laced.total_ms = elapsed + laced.left_ms;
    laced.active = 1;
}

void nettspend_laced_tick(const float dt_ms) {
    if (!laced.active) return;
    laced.clock_ms += fmaxf(dt_ms, 0.0f);
    laced.left_ms -= fmaxf(dt_ms, 0.0f);
    if (laced.left_ms <= 0.0f) nettspend_laced_reset();
}

void nettspend_laced_reset(void) {
    memset(&laced, 0, sizeof(laced));
}

float nettspend_laced_intensity(void) {
    if (!laced.active) return 0.0f;
    const float elapsed = laced.total_ms - laced.left_ms;
    const float in = NETTSPEND_LACED_FADE_IN_MS > 0.0f ? fminf(elapsed / NETTSPEND_LACED_FADE_IN_MS, 1.0f) : 1.0f;
    const float out = NETTSPEND_LACED_FADE_OUT_MS > 0.0f ? fminf(laced.left_ms / NETTSPEND_LACED_FADE_OUT_MS, 1.0f) : 1.0f;
    return smooth01(fminf(in, out));
}

float nettspend_laced_time(void) {
    return laced.clock_ms * 0.001f;
}

float nettspend_laced_left_ms(void) {
    return laced.active ? laced.left_ms : 0.0f;
}

void nettspend_on_laced(Client *client, const uint8_t *body, const uint16_t body_length) {
    NetLacedPacket packet;
    if (!client || body_length < sizeof(packet)) return;
    memcpy(&packet, body, sizeof(packet));
    const int me = client->me && client->me->uid == packet.uid;
    fprintf(stderr, "[nettspend] laced uid=%d %ums%s\n", packet.uid, (unsigned) packet.duration_ms, me ? " (you)" : "");
    // others' doses: nothing to show yet (a spectator / teammate indicator would start here)
    if (me) nettspend_laced_start((float) packet.duration_ms);
}
