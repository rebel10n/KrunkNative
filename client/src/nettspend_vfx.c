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

#define CUP_SIDES 24

// A styrofoam wall from z0 (radius r0) to z1 (r1) in `bands` matte off-white bands, each band's
// lower edge a slightly proud, slightly darker ring: the moulded ridges of a foam cup.
static void gb_foam_wall(GeoBuilder *b, const float z0, const float r0, const float z1, const float r1, const int bands, const vec3 foam,
                         const vec3 shade) {
    for (int i = 0; i < bands; i++) {
        const float t0 = (float) i / (float) bands, t1 = (float) (i + 1) / (float) bands;
        const float za = z0 + (z1 - z0) * t0, zb = z0 + (z1 - z0) * t1;
        const float ra = r0 + (r1 - r0) * t0, rb = r0 + (r1 - r0) * t1;
        const float ridge = (zb - za) * 0.16f; // the ridge's height along the wall
        gb_frustum(b, za, ra + 0.006f, shade, za + ridge, ra + 0.006f, shade, CUP_SIDES); // the ridge
        gb_frustum(b, za + ridge, ra, i & 1 ? foam : rgb_scale(foam, 0.985f), zb, rb, foam, CUP_SIDES);
    }
}

// The lean cup: a WHITE styrofoam double cup (two stacked foam cups: the outer one's rolled rim sits
// a little below the inner one's and shows as a second lip), matte off-white with moulded ridges,
// the purple lean just under the top rim. Axis z, base at +length / 2, rim at -length / 2.
static Geometry *cup_geometry(void) {
    static Geometry *cached;
    if (cached) return cached;
    GeoBuilder b = {0};
    const float h = NETTSPEND_CUP_LENGTH * 0.5f, rim = NETTSPEND_CUP_RADIUS, base = rim * 0.7f;
    const vec3 foam = rgb(NETTSPEND_FOAM_COLOR), shade = rgb(0xe9e7e1), lip = rgb(0xfdfdfb), bottom = rgb(0xdcdad3);
    const vec3 lean = rgb(NETTSPEND_DRANK_COLOR), lean_edge = rgb(0x4a1a7a), lean_shine = rgb(0x9b5bd6);
    // the outer cup: from the base to its own rim, 0.16 below the top, then its rolled lip
    const float outer_top = -h + 0.16f * NETTSPEND_CUP_LENGTH;
    const float outer_rim = base + (rim - base) * ((h - outer_top) / (2.0f * h)) + 0.012f;
    gb_disc(&b, h, base + 0.01f, bottom, CUP_SIDES, 0);
    gb_foam_wall(&b, h, base + 0.01f, outer_top, outer_rim, 6, foam, shade);
    gb_frustum(&b, outer_top, outer_rim, lip, outer_top - 0.02f, outer_rim + 0.022f, lip, CUP_SIDES);   // the rolled lip
    gb_frustum(&b, outer_top - 0.02f, outer_rim + 0.022f, lip, outer_top - 0.035f, outer_rim - 0.004f, shade, CUP_SIDES);
    // the inner cup above it: its wall from inside the outer one up to the top, its rolled lip
    gb_foam_wall(&b, outer_top + 0.06f, outer_rim - 0.01f, -h + 0.025f, rim, 2, foam, shade);
    gb_frustum(&b, -h + 0.025f, rim, lip, -h, rim + 0.026f, lip, CUP_SIDES);
    gb_frustum(&b, -h, rim + 0.026f, lip, -h + 0.012f, rim - 0.006f, shade, CUP_SIDES);
    // the lean: a dark edge, the surface, a soft highlight off-centre
    gb_disc(&b, -h + 0.07f, rim - 0.01f, lean_edge, CUP_SIDES, 1);
    gb_disc(&b, -h + 0.069f, rim - 0.05f, lean, CUP_SIDES, 1);
    if (gb_reserve(&b, 4, 6)) {
        const float z = -h + 0.068f, cx = -rim * 0.3f, cy = rim * 0.25f, w = rim * 0.22f, d = rim * 0.1f;
        const unsigned int v0 = gb_vertex(&b, (vec3) {cx - w, cy - d, z}, lean_shine), v1 = gb_vertex(&b, (vec3) {cx + w, cy - d, z}, lean_shine);
        const unsigned int v2 = gb_vertex(&b, (vec3) {cx + w, cy + d, z}, lean_shine), v3 = gb_vertex(&b, (vec3) {cx - w, cy + d, z}, lean_shine);
        gb_tri(&b, v0, v1, v2);
        gb_tri(&b, v0, v2, v3);
    }
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

// The Money Spread: a fan of bills held up out of a banded brick of cash, like a hand of cards.
// Local frame (the hose's convention): z is the long axis, the fan's top at -length / 2 (the "tip"
// the bills burst from), the brick at +length / 2 in the fist; a bill's face is the xz plane (its
// thickness along y), so the pose's rotation.x = PI / 2 turns the faces toward the camera.
static Geometry *cash_geometry(void) {
    static Geometry *cached;
    if (cached) return cached;
    GeoBuilder b = {0};
    const float h = NETTSPEND_CASH_LENGTH * 0.5f;
    const vec3 bill = rgb(NETTSPEND_CASH_COLOR), bill_dark = rgb_scale(rgb(NETTSPEND_CASH_COLOR), 0.82f), edge = rgb(NETTSPEND_CASH_BAND_COLOR);
    const vec3 seal = rgb(0xa8dcb5), band = rgb(0xfaf6ea), band_ink = rgb(0x3b2a6b);
    // the brick in the fist: bills stacked along y, x +-0.2 (a bill's width), z from +h up 0.2
    const vec3 brick = {0.0f, 0.0f, h - 0.1f};
    gb_box(&b, brick, (vec3) {0.2f, 0.0f, 0.0f}, (vec3) {0.0f, 0.0f, 0.1f}, (vec3) {0.0f, 0.09f, 0.0f}, bill, edge);
    gb_box(&b, brick, (vec3) {0.2f + 0.004f, 0.0f, 0.0f}, (vec3) {0.0f, 0.0f, 0.035f}, (vec3) {0.0f, 0.094f, 0.0f}, band, band);
    gb_box(&b, brick, (vec3) {0.2f + 0.006f, 0.0f, 0.0f}, (vec3) {0.0f, 0.0f, 0.01f}, (vec3) {0.0f, 0.096f, 0.0f}, band_ink, band_ink);
    // the fan: NETTSPEND_CASH_FAN bills pivoting about a point in the brick, each turned about y
    const float pivot = h - 0.12f, reach = 0.36f, half_long = 0.3f, half_short = 0.13f;
    for (int i = 0; i < NETTSPEND_CASH_FAN; i++) {
        const float a = ((float) i - (float) (NETTSPEND_CASH_FAN - 1) * 0.5f) * 0.28f, c = cosf(a), s = sinf(a);
        const vec3 up = {s, 0.0f, -c}, across = {c, 0.0f, s};
        const float y = -0.06f + 0.014f * (float) i; // later bills nearer the camera (local +y)
        const vec3 centre = {up.x * reach, y, pivot + up.z * reach};
        const vec3 face = i & 1 ? bill_dark : bill;
        gb_box(&b, centre, (vec3) {across.x * half_short, 0.0f, across.z * half_short}, (vec3) {up.x * half_long, 0.0f, up.z * half_long},
               (vec3) {0.0f, 0.004f, 0.0f}, face, edge);
        // the printed oval in the middle of the face (both sides)
        gb_box(&b, centre, (vec3) {across.x * half_short * 0.55f, 0.0f, across.z * half_short * 0.55f},
               (vec3) {up.x * half_long * 0.35f, 0.0f, up.z * half_long * 0.35f}, (vec3) {0.0f, 0.0055f, 0.0f}, seal, seal);
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
    Mesh *mesh = colored_mesh(cup_geometry());
    // styrofoam is white even in the shade: a little self-lighting keeps it white, not grey
    if (mesh) ((BasicMaterial *) mesh->material)->emissive = (vec4) {1.0f, 1.0f, 1.0f, NETTSPEND_FOAM_GLOW};
    return mesh;
}

Mesh *nettspend_bottle_mesh(void) {
    return colored_mesh(bottle_geometry());
}

Mesh *nettspend_cash_mesh(void) {
    Mesh *mesh = colored_mesh(cash_geometry());
    // the bills keep their green in the shade (a little green self-lighting)
    if (mesh) {
        const vec3 green = rgb(NETTSPEND_CASH_COLOR);
        ((BasicMaterial *) mesh->material)->emissive = (vec4) {green.x, green.y, green.z, NETTSPEND_CASH_GLOW};
    }
    return mesh;
}

// ---------------------------------------------------------------- poses

// The cup in the fist, rim up: rotation.x = PI / 2 maps the local -z (the rim) onto +y; the tilt
// leans the rim a little away from the eye (-z), so the view (from above and behind) sees the white
// foam wall with its ridges and double lip, and the lean over the rim.
#define CUP_REST_OFFSET_X 0.0f
#define CUP_REST_OFFSET_Y 0.66f   // the base on top of the fist (half the hand's thickness 0.41 + a little)
#define CUP_REST_OFFSET_Z (-0.45f)
#define CUP_REST_TILT (-0.12f)    // the rim leans away a little: the white wall faces the eye, the lean still shows
#define CASH_REST_OFFSET_X (-0.05f)
#define CASH_REST_OFFSET_Y 0.6f   // the brick on top of the fist, the fan clear above it
#define CASH_REST_OFFSET_Z (-0.1f)
#define CASH_REST_LEAN 0.25f      // the fan leans back (top away from the eye)
#define CASH_REST_TURN 0.35f      // ... turned toward the middle of the view
#define CASH_REST_ROLL 0.2f
// the pour: the cup rises toward the eye and a little to the trigger hand's side, so the bottle
// comes in on the off hand's side (its arm never crosses in front of the cup)
#define POUR_LIFT_X 0.25f
#define POUR_LIFT_Y 0.7f
#define POUR_LIFT_Z 0.25f
// the tipped bottle: its mouth this far out (toward the off hand) and up from the rim's centre, tipped
// this far (radians from upright): the fist stays out beside the rim, its forearm clear of the cup
#define POUR_MOUTH_X 0.15f
#define POUR_MOUTH_Y 0.12f
#define POUR_TIP 1.95f

static float smooth01(const float x) {
    const float t = fminf(fmaxf(x, 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void nettspend_pose_cup_rest(Mesh *cup, const vec3 hand, const float mirror) {
    if (!cup) return;
    // the cup's scale (third person: the mesh's own) grows about its centre: lift it so the base stays on the fist
    const float scale = cup->transform.scale.x;
    cup->transform.position = (vec3) {hand.x + CUP_REST_OFFSET_X * mirror, hand.y + CUP_REST_OFFSET_Y + (scale - 1.0f) * NETTSPEND_CUP_LENGTH * 0.5f,
                                      hand.z + CUP_REST_OFFSET_Z};
    cup->transform.rotation = (vec3) {(float) M_PI * 0.5f + CUP_REST_TILT, 0.0f, 0.0f};
}

void nettspend_pose_cash(Mesh *cash, const vec3 hand, const float mirror) {
    if (!cash) return;
    // the brick in the fist, the fan standing up out of it and facing the camera (rotation.x = PI / 2
    // turns the local -z, the fan's top, onto +y and the bills' faces toward +z), leaning back a
    // little and turned in toward the middle of the view
    cash->transform.position = (vec3) {hand.x + CASH_REST_OFFSET_X * mirror, hand.y + CASH_REST_OFFSET_Y, hand.z + CASH_REST_OFFSET_Z};
    cash->transform.rotation = (vec3) {(float) M_PI * 0.5f - CASH_REST_LEAN, -CASH_REST_TURN * mirror, CASH_REST_ROLL * mirror};
}

float nettspend_pour_strength(const float progress) {
    if (progress < 0.0f || progress > 1.0f) return 0.0f;
    return smooth01((progress - 0.22f) / 0.18f) * (1.0f - smooth01((progress - 0.74f) / 0.1f));
}

// The pour-up in three beats of the reload: the off hand brings the bottle up (upright, held low on
// its body, NETTSPEND_BOTTLE_GRIP: nothing comes out of the air) and over the cup (0..0.25), tips it
// over the rim and pours (0.25..0.8), then takes it back down (0.8..1). The trigger hand holds the
// cup up toward the eye meanwhile. The hands follow (cup_hand / bottle_hand: player_mesh.c
// turns the arms to reach them), so the cup and the bottle never leave a fist.
float nettspend_pose_pour(Mesh *cup, Mesh *bottle, const vec3 hand, const vec3 off_hand, const float mirror, const float scale, const float progress,
                          vec3 *cup_hand, vec3 *bottle_hand) {
    nettspend_pose_cup_rest(cup, hand, mirror);
    if (cup_hand) *cup_hand = hand;
    if (bottle_hand) *bottle_hand = off_hand;
    if (progress < 0.0f || progress > 1.0f) {
        if (bottle) bottle->visible = 0;
        return 0.0f;
    }
    const float in = smooth01(progress / 0.25f);                 // the bottle's carry over the cup
    const float out = smooth01((progress - 0.8f) / 0.2f);        // its way back down
    const float pour = nettspend_pour_strength(progress);        // the tip
    const float carry = in * (1.0f - out);                       // 0 at the off hand's rest, 1 over the cup
    // across and up scale with the cup (third person: the body's bigger cup goes clear of the head, seen
    // from the chase camera behind it); toward the eye stays (scaled, it went into the head)
    const vec3 lift = {POUR_LIFT_X * scale * mirror * carry, POUR_LIFT_Y * scale * carry, POUR_LIFT_Z * carry};
    if (cup) {
        cup->transform.position.x += lift.x;
        cup->transform.position.y += lift.y;
        cup->transform.position.z += lift.z;
    }
    if (cup_hand) *cup_hand = (vec3) {hand.x + lift.x, hand.y + lift.y, hand.z + lift.z};
    if (!bottle) return pour;
    bottle->visible = 1;
    // the rim's centre: the cup's local (0, 0, -h) through its rotation (PI / 2 + tilt about x)
    const float tilt = (float) M_PI * 0.5f + CUP_REST_TILT, h = NETTSPEND_CUP_LENGTH * 0.5f * scale;
    const vec3 rim = {cup ? cup->transform.position.x : hand.x + lift.x, (cup ? cup->transform.position.y : hand.y + lift.y) + sinf(tilt) * h,
                      (cup ? cup->transform.position.z : hand.z + lift.z) - cosf(tilt) * h};
    // the bottle works on the off hand's side of the cup (side: -1 = the screen's left for the
    // right-handed) and tips toward the trigger hand's. rotation (PI / 2, psi, 0): the mouth (local
    // -z) ends up along (-sin psi, cos psi, 0), so psi past PI / 2 points it down toward the cup; psi
    // takes the side's sign. Upright (psi 0) in the hand: the tip only tilts once over the cup.
    const float side = -mirror;
    const float psi0 = POUR_TIP * pour;
    const vec3 mouth_dir = {-sinf(psi0) * side, cosf(psi0), 0.0f};
    const float hb = NETTSPEND_BOTTLE_LENGTH * 0.5f * scale; // the bottle's half length, scaled like its mesh
    // the mouth over the rim while pouring, else beside and above the cup
    const float beside = 1.0f - pour;
    const vec3 mouth = {rim.x + (POUR_MOUTH_X + 0.3f * beside) * scale * side, rim.y + (POUR_MOUTH_Y + 0.3f * beside) * scale, rim.z + 0.02f * scale};
    // the fist's place on the bottle: low on its body (NETTSPEND_BOTTLE_GRIP of the half length from the centre toward the mouth)
    const float to_grip = hb * (1.0f - NETTSPEND_BOTTLE_GRIP);
    const vec3 grip_over = {mouth.x - mouth_dir.x * to_grip, mouth.y - mouth_dir.y * to_grip, mouth.z - mouth_dir.z * to_grip};
    const vec3 grip = {off_hand.x + (grip_over.x - off_hand.x) * carry, off_hand.y + (grip_over.y - off_hand.y) * carry,
                       off_hand.z + (grip_over.z - off_hand.z) * carry};
    bottle->transform.position = (vec3) {grip.x - mouth_dir.x * hb * NETTSPEND_BOTTLE_GRIP, grip.y - mouth_dir.y * hb * NETTSPEND_BOTTLE_GRIP,
                                         grip.z - mouth_dir.z * hb * NETTSPEND_BOTTLE_GRIP};
    bottle->transform.rotation = (vec3) {(float) M_PI * 0.5f, psi0 * side, 0.0f};
    bottle->transform.scale = (vec3) {scale, scale, scale};
    if (bottle_hand) *bottle_hand = grip;
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
