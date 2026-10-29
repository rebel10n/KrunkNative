// Krunker v10.0.4 counter skins (client/include/skin_counter.h). loadCounter (deob-10.0.4.js:204390-204505) builds one
// geometry of boxes (iíïiìíï, :196836-196848, faces :196835): a back plate, the icon block, the digit window, the icon's
// 7x7 pixels and `digits` x 15 digit pixels; each vertex carries the box colour and a "pixel" attribute (position, digit
// mask, flip delay) its node material reads: a lit pixel (iíïîíií, :196850-196859) loses its colour, glows in the attach
// colour (emissiveNode) and stands 0.012 out (positionNode). KN draws that split as meshes: one per box colour plus one
// for the lit pixels (black, emissive glow), rebuilt when the count changes. The bundle's 100 ms staggered flip between
// two counts (:204459-204465) is not drawn: the new digits show at once.
// SKIN_COUNTER_PURE (client/tests/skin_counter_test.c includes this file): only the bitmaps and the lit tests, no GL.
#ifdef SKIN_COUNTER_PURE
int skin_counter_digit_lit(long count, int position, int pixel);
int skin_counter_icon_lit(const char *stat, int pixel);
void skin_counter_set_base(int uid, int skin, long base);
void skin_counter_set_seed(int uid, int skin, int seed);
long skin_counter_base(int uid, int skin);
int skin_counter_seed(int uid, int skin);
void skin_counter_reset_bases(void);
int skin_counter_take_jumps(int uid, unsigned short events);
#else
#include <gl.h> // first: glad must precede any GL header
#include <skin_counter.h>
#include "viewer/viewer_store.h"
#include <math.h>
#include <stdlib.h>
#endif
#include <string.h>

#define COUNTER_PIXEL 0.085 // iîììiíî (deob-10.0.4.js:196834)
#define COUNTER_GROUPS 5    // back, face, window, unlit pixels, lit pixels
#define COUNTER_POP 0.012   // a lit pixel's z offset (positionNode, deob-10.0.4.js:204478)

// iîîîïíí (deob-10.0.4.js:196823): digits 0-9 as 3x5 pixels, row-major from the top
static const char *const k_digits[10] = {"111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001",
                                         "111100111001111", "111100111101111", "111001001001001", "111101111101111", "111101111001111"};
// iîìïííi (deob-10.0.4.js:196824-196833): the stat icons, 7x7 row-major
static const struct {
    const char *stat, *bits;
} k_icons[] = {
    {"kills", "0111110111111110010011101011011111001010100101010"},
    {"hskills", "0011100010101010010011110111100100101010100011100"},
    {"aikills", "0100010011111011010111111111011111001000101000001"},
    {"headshots", "1111111100000110101011000001101110110000011111111"},
    {"hits", "1000001010001000101000000000001010001000101000001"},
    {"shots", "0001000001110001111100111110011111000000000111110"},
    {"jumps", "0001000001110001111101111111001110000111000011100"},
    {"candy", "1000001110101111111111111111111111111010111000001"},
};
#define ICON_COUNT ((int) (sizeof(k_icons) / sizeof(k_icons[0])))

int skin_counter_digit_lit(const long count, const int position, const int pixel) {
    if (count < 0 || position < 0 || pixel < 0 || pixel >= 15) return 0;
    long rest = count;
    int length = 1; // String(count).length (iiîíìiï, deob-10.0.4.js:196860-196866)
    for (long c = count; c >= 10; c /= 10) length++;
    if (position >= length) return 0; // step(position + 0.5, length)
    for (int p = 0; p < position; p++) rest /= 10;
    return k_digits[rest % 10][pixel] == '1';
}

static int icon_index(const char *stat) {
    for (int i = 0; stat && i < ICON_COUNT; i++)
        if (!strcmp(k_icons[i].stat, stat)) return i;
    return 0; // iíìîííî: anything else counts "kills"
}

int skin_counter_icon_lit(const char *stat, const int pixel) {
    return pixel >= 0 && pixel < 49 && k_icons[icon_index(stat)].bits[pixel] == '1';
}

// The worn copies' data: per player and store skin, the copy's stat (the counter base, generateMeshes' IÌiiïíï[stat],
// deob-10.0.4.js:172727-172728) and its seed (iîîíïiï, :24344-24353: the seeded custom shader 15), from
// NET_EXT_KN_SKIN_COPIES (Box Office) or the account's copy meta (krunker.io). A full table drops the oldest entry.
#define BASE_MAX 64
static struct {
    int used, uid, skin, has_base, seed;
    long base;
} g_base[BASE_MAX];
static int g_base_next;

static int copy_slot(const int uid, const int skin, const int create) {
    for (int i = 0; i < BASE_MAX; i++)
        if (g_base[i].used && g_base[i].uid == uid && g_base[i].skin == skin) return i;
    if (!create) return -1;
    int slot = -1;
    for (int i = 0; i < BASE_MAX && slot < 0; i++)
        if (!g_base[i].used) slot = i;
    if (slot < 0) {
        slot = g_base_next;
        g_base_next = (g_base_next + 1) % BASE_MAX;
    }
    memset(&g_base[slot], 0, sizeof(g_base[slot]));
    g_base[slot].used = 1;
    g_base[slot].uid = uid;
    g_base[slot].skin = skin;
    return slot;
}

static void copy_drop_empty(const int slot) {
    if (slot >= 0 && !g_base[slot].has_base && !g_base[slot].seed) g_base[slot].used = 0;
}

void skin_counter_set_base(const int uid, const int skin, const long base) {
    if (skin < 0) return;
    const int slot = copy_slot(uid, skin, base >= 0);
    if (slot < 0) return;
    g_base[slot].has_base = base >= 0;
    g_base[slot].base = base >= 0 ? base : 0;
    copy_drop_empty(slot);
}

void skin_counter_set_seed(const int uid, const int skin, const int seed) {
    if (skin < 0) return;
    const int slot = copy_slot(uid, skin, seed > 0);
    if (slot < 0) return;
    g_base[slot].seed = seed > 0 ? seed : 0;
    copy_drop_empty(slot);
}

long skin_counter_base(const int uid, const int skin) {
    const int slot = copy_slot(uid, skin, 0);
    return slot >= 0 && g_base[slot].has_base ? g_base[slot].base : 0;
}

int skin_counter_seed(const int uid, const int skin) {
    const int slot = copy_slot(uid, skin, 0);
    return slot >= 0 ? g_base[slot].seed : 0;
}

void skin_counter_reset_bases(void) {
    memset(g_base, 0, sizeof(g_base));
    g_base_next = 0;
}

// The local player's jump_events last seen: the jumps since then (0 the first time a player is seen, or after a respawn's
// reset made the counter smaller).
static struct {
    int uid, seen;
    unsigned short events;
} g_jumps;

int skin_counter_take_jumps(const int uid, const unsigned short events) {
    if (!g_jumps.seen || g_jumps.uid != uid || events < g_jumps.events) {
        g_jumps.seen = 1;
        g_jumps.uid = uid;
        g_jumps.events = events;
        return 0;
    }
    const int n = (int) (unsigned short) (events - g_jumps.events);
    g_jumps.events = events;
    return n;
}

#ifndef SKIN_COUNTER_PURE

// iìiíîìï (deob-10.0.4.js:196835): per face its normal, then the two edge axes
static const float k_faces[6][3][3] = {
    {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}, {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
    {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}, {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
};

static const float k_colors[COUNTER_GROUPS - 1][3] = {
    {0.09f, 0.1f, 0.115f},   // the back plate
    {0.17f, 0.19f, 0.22f},   // the icon block
    {0.02f, 0.025f, 0.03f},  // the digit window
    {0.1f, 0.12f, 0.145f},   // a pixel (icon or digit)
};

typedef struct {
    vertex *v;
    unsigned int *idx;
    size_t nv, ni, cap_v, cap_i;
} Builder;

typedef struct {
    Mesh *mesh;
    unsigned int vbo; // Geometry keeps vao / ebo; the VBO is ours to delete
} CounterPart;

struct SkinCounter {
    MeshTransform root; // attach x / y / z, rot, scl on the weapon mesh
    int digits;
    long count, max;
    int stat;  // k_icons index
    int glow;  // 0xRRGGBB
    int visible;
    Scene *scene; // the scene its meshes are in (skin_counter_scene_add), NULL none
    CounterPart parts[COUNTER_GROUPS];
};


static int builder_reserve(Builder *b, const size_t nv, const size_t ni) {
    if (b->nv + nv > b->cap_v) {
        const size_t cap = (b->cap_v ? b->cap_v * 2 : 256) + nv;
        vertex *v = realloc(b->v, cap * sizeof(vertex));
        if (!v) return 0;
        b->v = v;
        b->cap_v = cap;
    }
    if (b->ni + ni > b->cap_i) {
        const size_t cap = (b->cap_i ? b->cap_i * 2 : 384) + ni;
        unsigned int *idx = realloc(b->idx, cap * sizeof(unsigned int));
        if (!idx) return 0;
        b->idx = idx;
        b->cap_i = cap;
    }
    return 1;
}

// iíïiìíï: a w x h x d box at (x, y, z), its back face (-z) dropped for a pixel (deob-10.0.4.js:196837)
static void builder_box(Builder *b, const float w, const float h, const float d, const float x, const float y, const float z, const int pixel) {
    const int faces = pixel ? 5 : 6;
    if (!builder_reserve(b, (size_t) faces * 4, (size_t) faces * 6)) return;
    const float half[3] = {w / 2.0f, h / 2.0f, d / 2.0f}, at[3] = {x, y, z};
    for (int f = 0; f < faces; f++) {
        const unsigned int base = (unsigned int) b->nv;
        for (int j = 0; j < 4; j++) {
            const float su = j == 0 || j == 3 ? -1.0f : 1.0f, sv = j < 2 ? -1.0f : 1.0f;
            float p[3];
            for (int c = 0; c < 3; c++) p[c] = at[c] + (k_faces[f][0][c] + k_faces[f][1][c] * su + k_faces[f][2][c] * sv) * half[c];
            b->v[b->nv++] = (vertex) {{p[0], p[1], p[2]}, {0.0f, 0.0f}};
        }
        const unsigned int tri[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
        memcpy(b->idx + b->ni, tri, sizeof(tri));
        b->ni += 6;
    }
}

static void part_free(CounterPart *part) {
    if (!part->mesh) return;
    Geometry *geometry = part->mesh->geometry;
    mesh_fini(part->mesh);
    if (geometry) {
        glDeleteVertexArrays(1, &geometry->vao);
        glDeleteBuffers(1, &geometry->ebo);
        free(geometry);
    }
    if (part->vbo) glDeleteBuffers(1, &part->vbo);
    memset(part, 0, sizeof(*part));
}

static int part_upload(SkinCounter *c, CounterPart *part, Builder *b, const int group) {
    if (!b->nv) return 1;
    Geometry *geometry = calloc(1, sizeof(Geometry));
    BasicMaterial *material = basic_material_init();
    if (!geometry || !material) {
        free(geometry);
        if (material) material_fini((Material *) material);
        return 0;
    }
    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, &part->vbo);
    glBindVertexArray(geometry->vao);
    glBindBuffer(GL_ARRAY_BUFFER, part->vbo);
    glBufferData(GL_ARRAY_BUFFER, (long) (b->nv * sizeof(vertex)), b->v, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long) (b->ni * sizeof(unsigned int)), b->idx, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
    geometry->index_count = (int) b->ni;
    geometry_set_bounds(geometry, b->v, b->nv);

    material->double_sided = 0;
    if (group == COUNTER_GROUPS - 1) { // lit: colour * (1 - lit) = black, emissive = glow * lit (deob-10.0.4.js:204479-204480)
        material->color = (vec4) {0.0f, 0.0f, 0.0f, 1.0f};
        material->emissive = (vec4) {(float) ((c->glow >> 16) & 255) / 255.0f, (float) ((c->glow >> 8) & 255) / 255.0f, (float) (c->glow & 255) / 255.0f, 1.0f};
    } else {
        material->color = (vec4) {k_colors[group][0], k_colors[group][1], k_colors[group][2], 1.0f};
    }
    part->mesh = mesh_init(geometry, (Material *) material);
    if (!part->mesh) {
        material_fini((Material *) material);
        glDeleteVertexArrays(1, &geometry->vao);
        glDeleteBuffers(1, &geometry->ebo);
        glDeleteBuffers(1, &part->vbo);
        free(geometry);
        part->vbo = 0;
        return 0;
    }
    part->mesh->transform.parent = &c->root;
    part->mesh->visible = c->visible;
    return 1;
}

// The panel and its pixels for the current count (the static boxes too: they are few)
static void counter_build(SkinCounter *c) {
    Scene *scene = c->scene;
    if (scene) skin_counter_scene_remove(scene, c);
    for (int i = 0; i < COUNTER_GROUPS; i++) part_free(&c->parts[i]);
    Builder b[COUNTER_GROUPS];
    memset(b, 0, sizeof(b));
    const float P = (float) COUNTER_PIXEL;
    const float w = 4.0f * (float) c->digits * P - P + 0.16f; // iìIiîîì
    const float x0 = -(0.5f + w) / 2.0f;                       // iîÎíîìí
    builder_box(&b[0], 0.5f + w + 0.08f, 0.74f, 0.08f, 0.0f, 0.0f, 0.04f, 0);
    builder_box(&b[1], 0.5f, 0.9f, 0.11f, x0 + 0.25f, 0.0f, 0.055f, 0);
    builder_box(&b[2], w, 0.56f, 0.01f, x0 + 0.5f + w / 2.0f, 0.0f, 0.082f, 0);
    // the icon: always lit (step(pixel.x, -0.5), its mask 1023)
    const float s = 5.0f * P / 7.0f;
    for (int i = 0; i < 49; i++) {
        if (k_icons[c->stat].bits[i] != '1') continue;
        const float size = 0.82f * s;
        builder_box(&b[COUNTER_GROUPS - 1], size, size, 0.02f, x0 + 0.25f + (float) (i % 7 - 3) * s, (float) (3 - i / 7) * s,
                    0.115f + (float) COUNTER_POP, 1);
    }
    // the digits, most significant first; a digit position past the count's length stays unlit
    const float xd = x0 + 0.5f + 0.08f + P;
    for (int d = 0; d < c->digits; d++) {
        const int position = c->digits - 1 - d;
        for (int k = 0; k < 15; k++) {
            const int lit = skin_counter_digit_lit(c->count, position, k);
            builder_box(&b[lit ? COUNTER_GROUPS - 1 : 3], 0.07f, 0.07f, 0.02f, xd + 4.0f * (float) d * P + (float) (k % 3 - 1) * P,
                        (float) (2 - k / 3) * P, 0.09f + (lit ? (float) COUNTER_POP : 0.0f), 1);
        }
    }
    for (int i = 0; i < COUNTER_GROUPS; i++) {
        part_upload(c, &c->parts[i], &b[i], i);
        free(b[i].v);
        free(b[i].idx);
    }
    if (scene) skin_counter_scene_add(scene, c);
}

static long counter_clamp(const SkinCounter *c, const long count) { return count < 0 ? 0 : count > c->max ? c->max : count; }

SkinCounter *skin_counter_create(const ViewerSkin *skin, const long count, MeshTransform *parent) {
    if (!skin || skin->counter_digits <= 0) return NULL;
    SkinCounter *c = calloc(1, sizeof(SkinCounter));
    if (!c) return NULL;
    // Math.min(10, Math.max(1, Math.round(digits) || 5)) (deob-10.0.4.js:204392)
    c->digits = skin->counter_digits < 1 ? 1 : skin->counter_digits > 10 ? 10 : skin->counter_digits;
    c->max = 1;
    for (int i = 0; i < c->digits; i++) c->max *= 10;
    c->max -= 1;
    c->stat = icon_index(skin->counter_stat);
    c->glow = skin->counter_color >= 0 ? skin->counter_color : 0x7fe3ff; // color || "#7fe3ff" (:204487)
    c->count = counter_clamp(c, count);
    c->visible = 1;
    // position / rotation / setScalar(scl) (deob-10.0.4.js:204499-204504)
    c->root.position = (vec3) {skin->counter_pos[0], skin->counter_pos[1], skin->counter_pos[2]};
    c->root.rotation = (vec3) {skin->counter_rot[0], skin->counter_rot[1], skin->counter_rot[2]};
    const float scl = skin->counter_scl > 0.0f ? skin->counter_scl : 1.0f;
    c->root.scale = (vec3) {scl, scl, scl};
    c->root.parent = parent;
    counter_build(c);
    return c;
}

void skin_counter_fini(SkinCounter *c) {
    if (!c) return;
    if (c->scene) skin_counter_scene_remove(c->scene, c);
    for (int i = 0; i < COUNTER_GROUPS; i++) part_free(&c->parts[i]);
    free(c);
}

void skin_counter_scene_add(Scene *scene, SkinCounter *c) {
    if (!scene || !c) return;
    c->scene = scene;
    for (int i = 0; i < COUNTER_GROUPS; i++)
        if (c->parts[i].mesh) scene_add_mesh(scene, c->parts[i].mesh);
}

void skin_counter_scene_remove(Scene *scene, SkinCounter *c) {
    if (!scene || !c) return;
    for (int i = 0; i < COUNTER_GROUPS; i++)
        if (c->parts[i].mesh) scene_remove_mesh(scene, c->parts[i].mesh);
    if (c->scene == scene) c->scene = NULL;
}

void skin_counter_set_visible(SkinCounter *c, const int visible) {
    if (!c) return;
    c->visible = visible;
    for (int i = 0; i < COUNTER_GROUPS; i++)
        if (c->parts[i].mesh) c->parts[i].mesh->visible = visible;
}

void skin_counter_set_count(SkinCounter *c, const long count) {
    if (!c) return;
    const long n = counter_clamp(c, count);
    if (n == c->count) return;
    c->count = n;
    counter_build(c);
}

long skin_counter_count(const SkinCounter *c) { return c ? c->count : 0; }

const char *skin_counter_stat(const SkinCounter *c) { return c ? k_icons[c->stat].stat : "kills"; }

// counterLive (deob-10.0.4.js:170932, :172729-172734): per player and weapon, the stat and what this match counted
#define LIVE_MAX 64
static struct {
    int player, weapon, stat, used;
    long n;
} g_live[LIVE_MAX];

long skin_counter_live(const int player_id, const int weapon_id, const char *stat) {
    const int s = icon_index(stat);
    for (int i = 0; i < LIVE_MAX; i++)
        if (g_live[i].used && g_live[i].player == player_id && g_live[i].weapon == weapon_id && g_live[i].stat == s) return g_live[i].n;
    return 0;
}

void skin_counter_count_stat(SkinCounter *c, const int player_id, const int weapon_id, const char *stat, const int amount) {
    if (!c || !stat || icon_index(stat) != c->stat || strcmp(k_icons[c->stat].stat, stat)) return; // iiÎìïîì.stat == stat
    int slot = -1;
    for (int i = 0; i < LIVE_MAX && slot < 0; i++)
        if (g_live[i].used && g_live[i].player == player_id && g_live[i].weapon == weapon_id && g_live[i].stat == c->stat) slot = i;
    for (int i = 0; i < LIVE_MAX && slot < 0; i++)
        if (!g_live[i].used) slot = i;
    if (slot < 0) slot = 0; // full: the first entry goes
    if (!g_live[slot].used || g_live[slot].player != player_id || g_live[slot].weapon != weapon_id) {
        g_live[slot].used = 1;
        g_live[slot].player = player_id;
        g_live[slot].weapon = weapon_id;
        g_live[slot].stat = c->stat;
        g_live[slot].n = 0;
    }
    g_live[slot].n += amount > 0 ? amount : 1; // iíIïíïi || 1
    skin_counter_set_count(c, c->count + (amount > 0 ? amount : 1));
}

void skin_counter_reset_live(void) { memset(g_live, 0, sizeof(g_live)); }

void skin_counter_count_player(const Player *player, const int weapon_id, const char *stat, const int amount) {
    const PlayerMesh *mesh = player ? player->mesh : NULL;
    if (!mesh || !mesh->arms || !player->loadout) return;
    for (int i = 0; i < player->loadout_size; i++) {
        PlayerArms *arms = mesh->arms[i];
        if (!arms || !arms->counter || (weapon_id >= 0 && player->loadout[i] != weapon_id)) continue;
        skin_counter_count_stat(arms->counter, player->uid, player->loadout[i], stat, amount);
    }
}

void skin_counter_frame(const Player *me) {
    if (!me) return;
    const int jumps = skin_counter_take_jumps(me->uid, me->jump_events);
    if (jumps > 0 && me->loadout && me->loadout_index >= 0 && me->loadout_index < me->loadout_size)
        skin_counter_count_player(me, me->loadout[me->loadout_index], "jumps", jumps);
}
#endif // SKIN_COUNTER_PURE
