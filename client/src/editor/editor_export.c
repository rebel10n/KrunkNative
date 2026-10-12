// Map editor: File > Export GLTF and File > Export MTL (Krunker v7.1.4 editor.js
// Editor.exportScene E:3608 / exportScene2 E:3656, libs/GLTFExporter.js, libs/OBJMTLExporter.js).
//
// v7 hands its three.js scene to the exporters: every object that is not a tool prefab, borders
// off, each mesh material replaced by a MeshLambertMaterial copy. Export GLTF calls
// GLTFExporter.parse(scene, {onlyVisible, trs, embedImages, includeCustomExtensions}) and saves
// JSON.stringify(gltf, null, 2) as "<name>.gltf"; Export MTL alerts "Big maps can take a while",
// runs OBJMTLExporter.parse(scene, name) over the visible objects and zips "<name>.obj",
// "<name>.mtl" and the texture images into "<name>.zip". The output formats below are those
// exporters' (line order, number formatting = Number#toString, JSON key order and pretty print).
//
// Intentional differences from v7.1.4 (doc/v7/editor.md section 1.13):
// - The meshes are built here from the object data (GL-free), not copied from what the renderer
//   uploaded: one mesh per object with the shapes of client/src/prefab.c (cube faces "f", planes,
//   ramps, cylinders, cones, spheres) and the prefab's OBJ model (models/<src>.obj, variants "tv",
//   mSize "ms"). Objects that need more than the object data are left out: CUSTOM_ASSET and asset
//   models ("aid"), ladders, sprites, particles, lights, AI, pads / machines / showcases, canvas
//   textures (signs, leaderboards: the plane is exported without its canvas), random billboards
//   (the plane, no image). Vertex colours (getColor gradients) are not exported: the material
//   colour is the object's colour "c" (v7's Lambert copy keeps three's white vertex-colour
//   material), "e" is the emissive, "o" the opacity.
// - Each object is one node holding its mesh with the object's full transform (v7 nests the mesh
//   under the ObjectInstance group).
// - GLTF: the texture PNGs are embedded as they are and TEXCOORD_0 is flipped (v = 1 - v); v7
//   re-encodes each image upside down through a canvas (flipY) and keeps three's UVs.
// - MTL: OBJMTLExporter cuts the last character off the texture file name and keeps the leading
//   slash ("map_Kd /wall_0.pn", zip entry "wall_0.pn"); the full relative name is written
//   ("wall_0.png"). Asset textures (user-assets/<id>/texture.png, all "texture.pn" in v7, so they
//   overwrite each other in the zip) are named "<id>_texture.png".
// - Natively the files are written to the user maps directory (editor_io.c ed_download_bytes);
//   the web build downloads them like v7.
#include "editor_export.h"
#include <cJSON.h>
#include <ctype.h>
#include <fast_obj.h>
#include <math.h>
#include <prefabs.h>
#include <shared.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORLD_UV 60.0 // game_constants.world_uv_scale (config.worldUV)

// ---------------------------------------------------------------------------
// growable text

typedef struct {
    char *buf;
    size_t len, cap;
    int fail;
} Sb;

static void sb_grow(Sb *s, const size_t add) {
    if (s->fail) return;
    if (s->len + add + 1 <= s->cap) return;
    size_t cap = s->cap ? s->cap : 4096;
    while (cap < s->len + add + 1) cap *= 2;
    char *b = realloc(s->buf, cap);
    if (!b) {
        s->fail = 1;
        return;
    }
    s->buf = b;
    s->cap = cap;
}

static void sb_addn(Sb *s, const char *t, const size_t n) {
    sb_grow(s, n);
    if (s->fail) return;
    memcpy(s->buf + s->len, t, n);
    s->len += n;
    s->buf[s->len] = 0;
}

static void sb_add(Sb *s, const char *t) { sb_addn(s, t, strlen(t)); }

static void sb_num(Sb *s, const double v) {
    char b[64];
    ed_model_number(v, b, sizeof(b));
    sb_add(s, b);
}

// ---------------------------------------------------------------------------
// geometry (three.js BufferGeometry: float32 position / normal / uv, an index)

typedef struct {
    float *pos, *nrm, *uv;
    unsigned int *idx;
    int nv, ni, cv, ci;
    int fail;
} Geo;

static int geo_vert(Geo *g, const double px, const double py, const double pz, const double nx, const double ny, const double nz,
                    const double u, const double v) {
    if (g->fail) return 0;
    if (g->nv == g->cv) {
        const int cap = g->cv ? g->cv * 2 : 64;
        float *p = realloc(g->pos, (size_t) cap * 3 * sizeof(float));
        if (p) g->pos = p;
        float *n = realloc(g->nrm, (size_t) cap * 3 * sizeof(float));
        if (n) g->nrm = n;
        float *t = realloc(g->uv, (size_t) cap * 2 * sizeof(float));
        if (t) g->uv = t;
        if (!p || !n || !t) {
            g->fail = 1;
            return 0;
        }
        g->cv = cap;
    }
    const int i = g->nv++;
    g->pos[i * 3] = (float) px;
    g->pos[i * 3 + 1] = (float) py;
    g->pos[i * 3 + 2] = (float) pz;
    g->nrm[i * 3] = (float) nx;
    g->nrm[i * 3 + 1] = (float) ny;
    g->nrm[i * 3 + 2] = (float) nz;
    g->uv[i * 2] = (float) u;
    g->uv[i * 2 + 1] = (float) v;
    return i;
}

static void geo_tri(Geo *g, const int a, const int b, const int c) {
    if (g->fail) return;
    if (g->ni + 3 > g->ci) {
        const int cap = g->ci ? g->ci * 2 : 96;
        unsigned int *p = realloc(g->idx, (size_t) cap * sizeof(unsigned int));
        if (!p) {
            g->fail = 1;
            return;
        }
        g->idx = p;
        g->ci = cap;
    }
    g->idx[g->ni++] = (unsigned int) a;
    g->idx[g->ni++] = (unsigned int) b;
    g->idx[g->ni++] = (unsigned int) c;
}

static void geo_free(Geo *g) {
    free(g->pos);
    free(g->nrm);
    free(g->uv);
    free(g->idx);
    memset(g, 0, sizeof(*g));
}

// prefab.c prefab_cube_geo: x / z in -0.5..0.5, y 0..1; faces in Krunker order (Right, Left,
// Top, Bottom, Front, Back), each with its own four corners. uv scaled by the face's size / worldUV
// * m (the cube shader's BASIC_UV_CUBE) when m is not 0.
static void geo_cube(Geo *g, const int mask, const double sx, const double sy, const double sz, const double m) {
    // corners (x, y, z, u, v) of each Krunker face, then its normal
    static const float face[6][4][5] = {
        {{0.5f, 0.0f, 0.5f, 0, 0}, {0.5f, 1.0f, -0.5f, 1, 1}, {0.5f, 1.0f, 0.5f, 0, 1}, {0.5f, 0.0f, -0.5f, 1, 0}},     // right
        {{-0.5f, 0.0f, -0.5f, 0, 0}, {-0.5f, 1.0f, 0.5f, 1, 1}, {-0.5f, 1.0f, -0.5f, 0, 1}, {-0.5f, 0.0f, 0.5f, 1, 0}}, // left
        {{-0.5f, 1.0f, 0.5f, 0, 0}, {0.5f, 1.0f, -0.5f, 1, 1}, {-0.5f, 1.0f, -0.5f, 0, 1}, {0.5f, 1.0f, 0.5f, 1, 0}},   // top
        {{-0.5f, 0.0f, -0.5f, 0, 0}, {0.5f, 0.0f, 0.5f, 1, 1}, {-0.5f, 0.0f, 0.5f, 0, 1}, {0.5f, 0.0f, -0.5f, 1, 0}},   // bottom
        {{-0.5f, 0.0f, 0.5f, 0, 0}, {0.5f, 1.0f, 0.5f, 1, 1}, {-0.5f, 1.0f, 0.5f, 0, 1}, {0.5f, 0.0f, 0.5f, 1, 0}},     // front
        {{0.5f, 0.0f, -0.5f, 0, 0}, {-0.5f, 1.0f, -0.5f, 1, 1}, {0.5f, 1.0f, -0.5f, 0, 1}, {-0.5f, 0.0f, -0.5f, 1, 0}}, // back
    };
    static const float normal[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    // the face's u / v extents: right / left z by y, top / bottom x by z, front / back x by y
    const double su[6] = {sz, sz, sx, sx, sx, sx}, sv[6] = {sy, sy, sz, sz, sy, sy};
    for (int f = 0; f < 6; f++) {
        if (!(mask & (1 << f))) continue;
        const double ku = m != 0 ? su[f] / WORLD_UV * m : 1, kv = m != 0 ? sv[f] / WORLD_UV * m : 1;
        int v[4];
        for (int c = 0; c < 4; c++) {
            const float *p = face[f][c];
            v[c] = geo_vert(g, p[0], p[1], p[2], normal[f][0], normal[f][1], normal[f][2], p[3] * ku, p[4] * kv);
        }
        geo_tri(g, v[0], v[1], v[2]);
        geo_tri(g, v[0], v[3], v[1]);
    }
}

// prefab.c prefab_plane_xy_geo: the unit quad in the XY plane facing +z
static void geo_plane(Geo *g, const double ku, const double kv) {
    const int a = geo_vert(g, -0.5, -0.5, 0, 0, 0, 1, 0, 0);
    const int b = geo_vert(g, 0.5, 0.5, 0, 0, 0, 1, ku, kv);
    const int c = geo_vert(g, -0.5, 0.5, 0, 0, 0, 1, 0, kv);
    const int d = geo_vert(g, 0.5, -0.5, 0, 0, 0, 1, ku, 0);
    geo_tri(g, a, b, c);
    geo_tri(g, a, d, b);
}

// prefab.c prefab_sphere_geo: THREE.SphereGeometry(1, 12, 8) halved, y 0..1
static void geo_sphere(Geo *g) {
    const int rings = 8, segments = 12;
    const int base = g->nv;
    for (int y = 0; y <= rings; y++) {
        const double vy = (double) y / rings, phi = vy * M_PI;
        for (int x = 0; x <= segments; x++) {
            const double vx = (double) x / segments, theta = vx * 2 * M_PI;
            const double nx = -cos(theta) * sin(phi), ny = cos(phi), nz = sin(theta) * sin(phi);
            geo_vert(g, nx * 0.5, 0.5 + ny * 0.5, nz * 0.5, nx, ny, nz, vx, 1 - vy);
        }
    }
    for (int y = 0; y < rings; y++) {
        for (int x = 0; x < segments; x++) {
            const int a = base + y * (segments + 1) + x, b = a + segments + 1;
            geo_tri(g, a, b, a + 1);
            geo_tri(g, a + 1, b, b + 1);
        }
    }
}

// prefab.c prefab_cylinder_geo (THREE.CylinderGeometry): radii top / bottom, y 0..1; the side
// normals lean by the slope like three's
static void geo_cylinder(Geo *g, const double r_top, const double r_bot, int sides, const int open, const double theta_start,
                         const double theta_length, const double uv_u, const double uv_v) {
    if (sides < 3) sides = 3;
    if (sides > 64) sides = 64;
    const double slope = r_bot - r_top; // height 1
    const int base = g->nv;
    for (int y = 0; y <= 1; y++) {
        const double radius = y * (r_bot - r_top) + r_top;
        for (int x = 0; x <= sides; x++) {
            const double u = (double) x / sides, theta = u * theta_length + theta_start;
            double nx = sin(theta), ny = slope, nz = cos(theta);
            const double l = sqrt(nx * nx + ny * ny + nz * nz);
            nx /= l, ny /= l, nz /= l;
            double tu = u, tv = 1.0 - y;
            if (uv_u != 0) {
                tu = 0.5 + (tu - 0.5) * uv_u;
                tv *= uv_v;
            }
            geo_vert(g, radius * sin(theta), 1.0 - y, radius * cos(theta), nx, ny, nz, tu, tv);
        }
    }
    for (int x = 0; x < sides; x++) {
        const int a = base + x, b = base + sides + 1 + x, c = b + 1, d = a + 1;
        geo_tri(g, a, b, d);
        geo_tri(g, b, c, d);
    }
    for (int cap = 0; cap < 2 && !open; cap++) {
        const int top = cap == 0;
        const double radius = top ? r_top : r_bot;
        if (radius <= 0) continue;
        const double sign = top ? 1 : -1;
        const int centers = g->nv;
        for (int x = 1; x <= sides; x++) {
            double tu = 0.5, tv = 0.5;
            if (uv_u != 0) tv *= uv_v;
            geo_vert(g, 0, 0.5 * sign + 0.5, 0, 0, sign, 0, tu, tv);
        }
        const int ring = g->nv;
        for (int x = 0; x <= sides; x++) {
            const double theta = (double) x / sides * theta_length + theta_start;
            double tu = cos(theta) * 0.5 + 0.5, tv = sin(theta) * 0.5 * sign + 0.5;
            if (uv_u != 0) {
                tu = 0.5 + (tu - 0.5) * uv_u;
                tv *= uv_v;
            }
            geo_vert(g, radius * sin(theta), 0.5 * sign + 0.5, radius * cos(theta), 0, sign, 0, tu, tv);
        }
        for (int x = 0; x < sides; x++) {
            const int c = centers + x, i = ring + x;
            if (top) geo_tri(g, i, i + 1, c);
            else geo_tri(g, i + 1, i, c);
        }
    }
}

// prefab.c prefab_ramp_prism_geo (an enclosed ramp, "ec"): the extruded triangle (0,0) (0,1)
// (1,0), centred
static void geo_ramp_prism(Geo *g, const double uv_u, const double uv_v) {
    static const double contour[3][2] = {{0, 0}, {0, 1}, {1, 0}};
    double p[24][5];
    double nrm[24][3];
    int n = 0;
    for (int layer = 0; layer < 2; layer++) {
        for (int i = 0; i < 3; i++) {
            const double *c = contour[layer ? i : 2 - i];
            p[n][0] = c[0], p[n][1] = c[1], p[n][2] = layer, p[n][3] = c[0], p[n][4] = c[1];
            nrm[n][0] = 0, nrm[n][1] = 0, nrm[n][2] = layer ? 1 : -1;
            n++;
        }
    }
    for (int i = 2; i >= 0; i--) {
        const int j = i, k = (i + 2) % 3;
        const double ax = contour[j][0], ay = contour[j][1], bx = contour[k][0], by = contour[k][1];
        const double q[4][3] = {{ax, ay, 0}, {bx, by, 0}, {bx, by, 1}, {ax, ay, 1}};
        const int along_x = fabs(ay - by) < fabs(ax - bx);
        // outward normal of the wall (the contour centroid is inside)
        double nx = by - ay, ny = -(bx - ax);
        const double cx = 1.0 / 3, cy = 1.0 / 3, mx = (ax + bx) / 2, my = (ay + by) / 2;
        if (nx * (mx - cx) + ny * (my - cy) < 0) nx = -nx, ny = -ny;
        const double l = sqrt(nx * nx + ny * ny);
        static const int order[6] = {0, 1, 3, 1, 2, 3};
        for (int c = 0; c < 6; c++) {
            const double *v = q[order[c]];
            p[n][0] = v[0], p[n][1] = v[1], p[n][2] = v[2];
            p[n][3] = along_x ? v[0] : v[1];
            p[n][4] = 1 - v[2];
            nrm[n][0] = nx / l, nrm[n][1] = ny / l, nrm[n][2] = 0;
            n++;
        }
    }
    const int base = g->nv;
    for (int i = 0; i < n; i++) {
        double u = p[i][3], v = p[i][4];
        if (uv_u != 0) {
            u = 0.5 + (u - 0.5) * uv_u;
            v *= uv_v;
        }
        geo_vert(g, p[i][0] - 0.5, p[i][1] - 0.5, p[i][2] - 0.5, nrm[i][0], nrm[i][1], nrm[i][2], u, v);
    }
    for (int i = 0; i + 2 < n; i += 3) geo_tri(g, base + i, base + i + 1, base + i + 2);
}

// models/<model>.obj as loaders.c load_obj_model reads it (one vertex per face corner, faces
// fanned); normals from the file, else the face normal. *center_y: the bounding box centre y.
static int geo_obj_model(Geo *g, const char *path, double *center_y) {
    ensure_asset_file(path);
    FILE *probe = fopen(path, "rb");
    if (!probe) return 0;
    fclose(probe);
    fastObjMesh *m = fast_obj_read(path);
    if (!m) return 0;
    double lo = INFINITY, hi = -INFINITY;
    size_t idx = 0;
    for (unsigned int f = 0; f < m->face_count; f++) {
        const unsigned int fv = m->face_vertices[f];
        if (fv < 3) {
            idx += fv;
            continue;
        }
        int vi[64];
        const unsigned int count = fv < 64 ? fv : 64;
        const float *p0 = m->positions + m->indices[idx].p * 3, *p1 = m->positions + m->indices[idx + 1].p * 3,
                    *p2 = m->positions + m->indices[idx + 2].p * 3;
        double fn[3] = {(p1[1] - p0[1]) * (p2[2] - p0[2]) - (p1[2] - p0[2]) * (p2[1] - p0[1]),
                        (p1[2] - p0[2]) * (p2[0] - p0[0]) - (p1[0] - p0[0]) * (p2[2] - p0[2]),
                        (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p1[1] - p0[1]) * (p2[0] - p0[0])};
        const double fl = sqrt(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]);
        if (fl > 0) fn[0] /= fl, fn[1] /= fl, fn[2] /= fl;
        else fn[0] = 0, fn[1] = 1, fn[2] = 0;
        for (unsigned int c = 0; c < fv; c++) {
            const fastObjIndex ix = m->indices[idx + c];
            if (c >= count) continue;
            const float *p = m->positions + ix.p * 3;
            const float *t = m->texcoords + ix.t * 2;
            double n[3] = {fn[0], fn[1], fn[2]};
            if (ix.n > 0 && ix.n < m->normal_count) {
                const float *q = m->normals + ix.n * 3;
                const double l = sqrt((double) q[0] * q[0] + (double) q[1] * q[1] + (double) q[2] * q[2]);
                if (l > 0) n[0] = q[0] / l, n[1] = q[1] / l, n[2] = q[2] / l;
            }
            vi[c] = geo_vert(g, p[0], p[1], p[2], n[0], n[1], n[2], t[0], t[1]);
            if (p[1] < lo) lo = p[1];
            if (p[1] > hi) hi = p[1];
        }
        for (unsigned int c = 2; c < count; c++) geo_tri(g, vi[0], vi[c - 1], vi[c]);
        idx += fv;
    }
    fast_obj_destroy(m);
    *center_y = isfinite(lo) ? (float) ((float) lo + (float) hi) * 0.5f : 0;
    return g->ni > 0 && !g->fail;
}

// ---------------------------------------------------------------------------
// objects -> parts (one mesh each: local geometry, node TRS, material)

typedef struct {
    Geo g;
    double t[3], q[4], s[3];
    double color[3], emissive[3], opacity;
    int double_sided, transparent;
    char tex_path[512]; // full path of the texture image, "" none
    char tex_name[128]; // its file name in the MTL / zip
} Part;

typedef struct {
    Part *v;
    size_t n, cap;
} Parts;

static const cJSON *G(const cJSON *o, const char *k) { return cJSON_GetObjectItemCaseSensitive(o, k); }

static double num_or(const cJSON *v, const double def) {
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring[0]) {
        char *end = NULL;
        const double d = strtod(v->valuestring, &end);
        return end != v->valuestring ? d : def;
    }
    return def;
}

static int present(const cJSON *v) { return v && !cJSON_IsNull(v); }

static double vec_at(const cJSON *a, const int i, const double def) {
    const cJSON *v = cJSON_GetArrayItem(a, i);
    return v ? num_or(v, def) : def;
}

static void parse_color(const cJSON *v, const unsigned int def, double out[3]) {
    unsigned int hex = def;
    if (cJSON_IsNumber(v)) hex = (unsigned int) (long long) v->valuedouble & 0xffffffu;
    else if (cJSON_IsString(v) && v->valuestring[0]) {
        const char *s = v->valuestring + (v->valuestring[0] == '#');
        size_t n = 0;
        while (isxdigit((unsigned char) s[n])) n++;
        if (n == 3) {
            const unsigned int r = (unsigned int) strtoul((char[]) {s[0], 0}, NULL, 16), gg = (unsigned int) strtoul((char[]) {s[1], 0}, NULL, 16),
                               b = (unsigned int) strtoul((char[]) {s[2], 0}, NULL, 16);
            hex = (r * 17) << 16 | (gg * 17) << 8 | b * 17;
        } else if (n >= 6) {
            hex = (unsigned int) strtoul(s, NULL, 16) & 0xffffffu;
        }
    }
    out[0] = (double) ((hex >> 16) & 255) / 255.0;
    out[1] = (double) ((hex >> 8) & 255) / 255.0;
    out[2] = (double) (hex & 255) / 255.0;
}

static void quat_euler(const double rx, const double ry, const double rz, const int yxz, double q[4]) {
    const double c1 = cos(rx / 2), c2 = cos(ry / 2), c3 = cos(rz / 2), s1 = sin(rx / 2), s2 = sin(ry / 2), s3 = sin(rz / 2);
    if (!yxz) { // three.js Quaternion.setFromEuler "XYZ"
        q[0] = s1 * c2 * c3 + c1 * s2 * s3;
        q[1] = c1 * s2 * c3 - s1 * c2 * s3;
        q[2] = c1 * c2 * s3 + s1 * s2 * c3;
        q[3] = c1 * c2 * c3 - s1 * s2 * s3;
    } else { // "YXZ"
        q[0] = s1 * c2 * c3 + c1 * s2 * s3;
        q[1] = c1 * s2 * c3 - s1 * c2 * s3;
        q[2] = c1 * c2 * s3 - s1 * s2 * c3;
        q[3] = c1 * c2 * c3 + s1 * s2 * s3;
    }
}

static void quat_mul(const double a[4], const double b[4], double out[4]) {
    const double x = a[0] * b[3] + a[3] * b[0] + a[1] * b[2] - a[2] * b[1];
    const double y = a[1] * b[3] + a[3] * b[1] + a[2] * b[0] - a[0] * b[2];
    const double z = a[2] * b[3] + a[3] * b[2] + a[0] * b[1] - a[1] * b[0];
    const double w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = x, out[1] = y, out[2] = z, out[3] = w;
}

// Matrix4.compose(t, q, s), column-major
static void compose(const double t[3], const double q[4], const double s[3], double e[16]) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double x2 = x + x, y2 = y + y, z2 = z + z;
    const double xx = x * x2, xy = x * y2, xz = x * z2, yy = y * y2, yz = y * z2, zz = z * z2, wx = w * x2, wy = w * y2, wz = w * z2;
    e[0] = (1 - (yy + zz)) * s[0], e[1] = (xy + wz) * s[0], e[2] = (xz - wy) * s[0], e[3] = 0;
    e[4] = (xy - wz) * s[1], e[5] = (1 - (xx + zz)) * s[1], e[6] = (yz + wx) * s[1], e[7] = 0;
    e[8] = (xz + wy) * s[2], e[9] = (yz - wx) * s[2], e[10] = (1 - (xx + yy)) * s[2], e[11] = 0;
    e[12] = t[0], e[13] = t[1], e[14] = t[2], e[15] = 1;
}

static void quat_rotate(const double q[4], const double v[3], double out[3]) {
    static const double one[3] = {1, 1, 1}, zero[3] = {0, 0, 0};
    double e[16];
    compose(zero, q, one, e);
    out[0] = e[0] * v[0] + e[4] * v[1] + e[8] * v[2];
    out[1] = e[1] * v[0] + e[5] * v[1] + e[9] * v[2];
    out[2] = e[2] * v[0] + e[6] * v[1] + e[10] * v[2];
}

static void path_join(char *out, const size_t n, const char *root, const char *rel) {
    const size_t l = strlen(root);
    snprintf(out, n, "%s%s%s", root, l && root[l - 1] != '/' ? "/" : "", rel);
}

static int file_exists(const char *path) {
    ensure_asset_file(path);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

// the texture the object renders with: "<src>.png" under textures/, or an asset's texture.png
static void part_texture(Part *pt, const char *root, const char *src, const char *asset) {
    char rel[256];
    if (asset) {
        snprintf(rel, sizeof(rel), "user-assets/%s/texture.png", asset);
        snprintf(pt->tex_name, sizeof(pt->tex_name), "%s_texture.png", asset);
    } else {
        snprintf(rel, sizeof(rel), "textures/%s.png", src);
        snprintf(pt->tex_name, sizeof(pt->tex_name), "%s.png", src);
    }
    if (root) path_join(pt->tex_path, sizeof(pt->tex_path), root, rel);
    else pt->tex_path[0] = 0;
}

// the style texture of a texturable prefab: "at" (asset id) or textures[t] with variant "tv"
static void style_texture(Part *pt, const char *root, const cJSON *o, const int prefab) {
    const cJSON *at = G(o, "at");
    if (present(at)) {
        char id[48];
        if (cJSON_IsNumber(at)) snprintf(id, sizeof(id), "%.0f", at->valuedouble);
        else snprintf(id, sizeof(id), "%s", cJSON_IsString(at) ? at->valuestring : "");
        int digits = id[0] != 0;
        for (const char *c = id; *c; c++) digits &= isdigit((unsigned char) *c) != 0;
        if (digits) part_texture(pt, root, NULL, id); // "<id>v": a video, no image
        return;
    }
    const cJSON *t = G(o, "t");
    const int tex = prefab_texture_resolve(prefab, present(t), (int) num_or(t, 0));
    char src[96];
    if (prefab_texture_src(tex, (int) num_or(G(o, "tv"), 0), src, sizeof(src)) && strcmp(src, "default") != 0) part_texture(pt, root, src, NULL);
}

static Part *parts_add(Parts *ps) {
    if (ps->n == ps->cap) {
        const size_t cap = ps->cap ? ps->cap * 2 : 32;
        Part *v = realloc(ps->v, cap * sizeof(Part));
        if (!v) return NULL;
        ps->v = v;
        ps->cap = cap;
    }
    Part *p = &ps->v[ps->n++];
    memset(p, 0, sizeof(*p));
    p->q[3] = 1;
    p->s[0] = p->s[1] = p->s[2] = 1;
    p->opacity = 1;
    return p;
}

static void parts_free(Parts *ps) {
    for (size_t i = 0; i < ps->n; i++) geo_free(&ps->v[i].g);
    free(ps->v);
    memset(ps, 0, sizeof(*ps));
}

static int face_mask(const cJSON *f) {
    // prefab.c prefab_face_mask: "f" hex, bit 5 - i = Krunker face i
    if (!cJSON_IsString(f)) return 0x3f;
    const long bits = strtol(f->valuestring, NULL, 16);
    int mask = 0;
    for (int i = 0; i < 6; i++) if (bits & (1L << (5 - i))) mask |= 1 << i;
    return mask;
}

// Builds the part of one object; 0 = nothing exported for it.
static int build_part(Parts *ps, const cJSON *o, const int prefab, const char *root) {
    if (prefab < 0 || prefab >= EDITOR_PREFAB_COUNT) return 0;
    if (editor_prefab_has(prefab, EP_TOOL)) return 0;   // !prefab.tool
    if (present(G(o, "v"))) return 0;                    // visible = false (onlyVisible)
    const PrefabInfo *info = prefab_info(prefab);
    if (!info) return 0;
    const cJSON *p = G(o, "p"), *r = G(o, "r"), *s = G(o, "s");
    const double px = vec_at(p, 0, 0), py = vec_at(p, 1, 0), pz = vec_at(p, 2, 0);
    const double rx = vec_at(r, 0, 0), ry = vec_at(r, 1, 0), rz = vec_at(r, 2, 0);
    const double sx = vec_at(s, 0, 1), sy = vec_at(s, 1, 1), sz = vec_at(s, 2, 1);
    const double tsm = num_or(G(o, "tsm"), 0);
    const int stretch = present(G(o, "tsr"));
    const size_t mark = ps->n;
    Part *pt = parts_add(ps);
    if (!pt) return 0;
    pt->t[0] = px, pt->t[1] = py, pt->t[2] = pz;
    quat_euler(rx, ry, rz, 0, pt->q);
    parse_color(G(o, "c"), 0xffffff, pt->color);
    parse_color(G(o, "e"), 0x000000, pt->emissive);
    pt->opacity = present(G(o, "o")) ? num_or(G(o, "o"), 1) : 1;
    pt->transparent = pt->opacity < 1;
    const int texturable = editor_prefab_has(prefab, EP_TEXTURABLE);
    int ok = 1;
    switch (prefab) {
        case PREFAB_CUBE:
        case PREFAB_GATE:
        case PREFAB_DEPOSIT_BOX:
        case PREFAB_TRIGGER:
        case PREFAB_ROPE_LADDER: {
            if (prefab == PREFAB_TRIGGER && present(G(o, "aid"))) {
                ok = 0;
                break;
            }
            double w = sx, d = sz;
            if (prefab == PREFAB_ROPE_LADDER) w = d = fmin(sx, sz);
            const double m = tsm != 0 ? tsm : stretch ? 0 : 1;
            geo_cube(&pt->g, prefab == PREFAB_ROPE_LADDER ? 0x3f : face_mask(G(o, "f")), w, sy, d, m);
            pt->s[0] = w, pt->s[1] = sy, pt->s[2] = d;
            pt->double_sided = prefab == PREFAB_CUBE && (int) num_or(G(o, "rs"), 0) == 2;
            if (texturable) style_texture(pt, root, o, prefab);
            break;
        }
        case PREFAB_PLANE:
        case PREFAB_LIQUID:
        case PREFAB_RUNE:
        case PREFAB_LIGHT_BAR:
        case PREFAB_BILLBOARD:
        case PREFAB_SPRAY:
        case PREFAB_SIGN: {
            // addFixedPlane: quaternion Rx(r0) Ry(r1) Rz(r2) Rx(-PI/2), scale (s0, s2)
            double scale = prefab == PREFAB_PLANE ? (stretch ? 0 : 1) : prefab == PREFAB_LIQUID ? 2 : 0;
            const int keeps_scale = prefab == PREFAB_LIQUID || prefab == PREFAB_SIGN || prefab == PREFAB_SPRAY || prefab == PREFAB_LIGHT_BAR;
            const double m = keeps_scale || tsm == 0 ? scale : tsm;
            geo_plane(&pt->g, m != 0 ? sx / WORLD_UV * m : 1, m != 0 ? sz / WORLD_UV * m : 1);
            double qx[4];
            quat_euler(-M_PI / 2, 0, 0, 0, qx);
            double q[4];
            quat_mul(pt->q, qx, q);
            memcpy(pt->q, q, sizeof(q));
            if (prefab == PREFAB_RUNE) pt->t[1] += 0.01;
            pt->s[0] = sx, pt->s[1] = sz, pt->s[2] = 1;
            pt->double_sided = 1;
            if (prefab == PREFAB_RUNE) part_texture(pt, root, "rune_0", NULL);
            else if (prefab == PREFAB_LIGHT_BAR) part_texture(pt, root, "lightcone_1", NULL);
            else if (prefab == PREFAB_PLANE || prefab == PREFAB_LIQUID) style_texture(pt, root, o, prefab);
            break;
        }
        case PREFAB_CYLINDER:
        case PREFAB_LIGHT_CONE: {
            int sides = (int) num_or(G(o, "sds"), 0);
            if (!sides) sides = 12;
            const cJSON *thl = G(o, "thl");
            double tl = present(thl) ? num_or(thl, 0) : 360;
            if (tl == 0) tl = 0.1;
            tl = fmin(fmax(round(tl * 100) / 100, 0.1), 360);
            const double radius = fmin(sx, sz) / 2;
            if (prefab == PREFAB_LIGHT_CONE) {
                geo_cylinder(&pt->g, 0, 1, sides, 1, M_PI / 4, tl * M_PI / 180, 0, 0);
                pt->s[0] = sx / 2, pt->s[1] = sy, pt->s[2] = sz / 2;
                pt->double_sided = 1;
                pt->transparent = 1;
                part_texture(pt, root, present(G(o, "fc")) ? "lightcone_1" : "lightcone_0", NULL);
            } else {
                const cJSON *rtp = G(o, "rtp"), *rbt = G(o, "rbt"), *ths = G(o, "ths");
                const double rt = present(rtp) ? fmin(fmax(round(num_or(rtp, 0) * 100) / 100, 0), 1) : 1;
                const double rb = present(rbt) ? fmin(fmax(round(num_or(rbt, 0) * 100) / 100, 0), 1) : 1;
                const double ts = fmin(fmax(round(num_or(ths, 0) * 100) / 100, 0), 360);
                const int open = present(G(o, "opn"));
                const double m = tsm;
                geo_cylinder(&pt->g, rt, rb, sides, open, ts * M_PI / 180, tl * M_PI / 180, m != 0 ? radius / WORLD_UV * m : 0,
                             m != 0 ? sy / WORLD_UV * m : 0);
                pt->s[0] = radius, pt->s[1] = sy, pt->s[2] = radius;
                pt->double_sided = open;
                if (texturable) style_texture(pt, root, o, prefab);
            }
            break;
        }
        case PREFAB_SPHERE:
            geo_sphere(&pt->g);
            pt->s[0] = sx, pt->s[1] = sy, pt->s[2] = sz;
            pt->double_sided = 1;
            if (texturable) style_texture(pt, root, o, prefab);
            break;
        case PREFAB_RAMP: {
            const int dir = (int) num_or(G(o, "d"), 0), odd = dir % 2 != 0;
            const double width = odd ? sx : sz, length = odd ? sz : sx, height = sy;
            const double slant = sqrt(height * height + length * length), direction = M_PI * 0.5 * dir;
            const double m = tsm != 0 ? tsm : stretch ? 0 : 1;
            pt->t[1] += height * 0.5;
            if (present(G(o, "ec"))) {
                geo_ramp_prism(&pt->g, m != 0 ? length / 2 / WORLD_UV * m : 0, m != 0 ? width / WORLD_UV * m : 0);
                quat_euler(0, M_PI - direction, 0, 0, pt->q);
                pt->s[0] = odd ? sz : sx, pt->s[1] = height, pt->s[2] = odd ? sx : sz;
            } else {
                geo_plane(&pt->g, m != 0 ? width / WORLD_UV * m : 1, m != 0 ? length / WORLD_UV * m : 1);
                quat_euler((slant > 0 ? asin(height / slant) : 0) - M_PI / 2, -M_PI / 2 - direction, 0, 1, pt->q);
                pt->s[0] = width, pt->s[1] = slant, pt->s[2] = 1;
            }
            pt->double_sided = 1;
            if (texturable) style_texture(pt, root, o, prefab);
            break;
        }
        default: {
            const PrefabData *data = prefab_data(prefab);
            if (info->geometry != PREFAB_GEO_MODEL || !data || (!data->model && !data->src) || !root) {
                ok = 0;
                break;
            }
            // prefab.c prefab_model_src (the random texRange texture is not drawn: the base one)
            char model[96], texture[96];
            snprintf(model, sizeof(model), "%s", data->model ? data->model : data->src);
            snprintf(texture, sizeof(texture), "%s", model);
            int tv = (int) num_or(G(o, "tv"), 0);
            if (data->variant_count > 0) {
                if (tv < 0) tv = 0;
                if (tv > data->variant_count - 1) tv = data->variant_count - 1;
            } else {
                tv = 0;
            }
            if (tv && data->variants) {
                const PrefabVariant *v = &data->variants[tv];
                char b[96];
                snprintf(b, sizeof(b), "%s", model);
                if (v->diff_model) {
                    snprintf(model, sizeof(model), "%s_%d", b, v->id);
                    snprintf(texture, sizeof(texture), "%s", model);
                } else {
                    snprintf(texture, sizeof(texture), "%s_%d", b, v->id);
                }
            }
            char rel[160], path[700];
            snprintf(rel, sizeof(rel), "models/%s.obj", model);
            path_join(path, sizeof(path), root, rel);
            double center_y = 0;
            if (!geo_obj_model(&pt->g, path, &center_y)) {
                ok = 0;
                break;
            }
            // mSize: limitMM(ms, 0.01, 100).round(2) || prefab scale; prefab_place_model
            double scale = data->scale;
            const double ms = num_or(G(o, "ms"), 0);
            if (ms != 0) scale = fmin(fmax(round(ms * 100) / 100, 0.01), 100);
            const double local[3] = {0, sy * 0.5 - center_y * scale, 0};
            double off[3];
            quat_rotate(pt->q, local, off);
            pt->t[0] += off[0], pt->t[1] += off[1], pt->t[2] += off[2];
            pt->s[0] = pt->s[1] = pt->s[2] = scale;
            pt->double_sided = prefab_has_flag(prefab, PF_DOUBLESIDE);
            pt->transparent = prefab_has_flag(prefab, PF_TRANSPARENT) || pt->opacity != 1;
            if (!present(G(o, "c"))) pt->color[0] = pt->color[1] = pt->color[2] = 1;
            pt->emissive[0] = pt->emissive[1] = pt->emissive[2] = 0;
            part_texture(pt, root, texture, NULL);
            break;
        }
    }
    if (!ok || pt->g.fail || pt->g.ni == 0) {
        geo_free(&pt->g);
        ps->n = mark;
        return 0;
    }
    return 1;
}

static void build_parts(Parts *ps, const EdModelObj *objs, const size_t n, const char *root) {
    for (size_t i = 0; i < n; i++) build_part(ps, objs[i].data, objs[i].prefab, root);
}

// ---------------------------------------------------------------------------
// OBJ / MTL (OBJMTLExporter.parse)

int ed_export_obj(const char *name, const EdModelObj *objs, const size_t n, const char *assets_root, EdExportObj *out) {
    memset(out, 0, sizeof(*out));
    if (!name || !*name) name = "map";
    Parts ps = {0};
    build_parts(&ps, objs, n, assets_root);
    Sb obj = {0}, mtl = {0};
    sb_add(&obj, "mtllib ");
    sb_add(&obj, name);
    sb_add(&obj, ".mtl\n");
    size_t off_v = 0, off_t = 0, off_n = 0;
    char line[64];
    for (size_t i = 0; i < ps.n; i++) {
        const Part *pt = &ps.v[i];
        const Geo *g = &pt->g;
        double e[16], nm[9];
        compose(pt->t, pt->q, pt->s, e);
        // normal matrix: inverse transpose of the upper 3x3
        {
            const double a = e[0], b = e[4], c = e[8], d = e[1], f = e[5], h = e[9], k = e[2], l = e[6], mm = e[10];
            const double A = f * mm - h * l, B = -(d * mm - h * k), C = d * l - f * k;
            const double det = a * A + b * B + c * C;
            const double inv = det != 0 ? 1 / det : 0;
            // inverse (row-major) then transposed: nm rows = cofactor rows / det
            nm[0] = A * inv, nm[1] = B * inv, nm[2] = C * inv;
            nm[3] = -(b * mm - c * l) * inv, nm[4] = (a * mm - c * k) * inv, nm[5] = -(a * l - b * k) * inv;
            nm[6] = (b * h - c * f) * inv, nm[7] = -(a * h - c * d) * inv, nm[8] = (a * f - b * d) * inv;
        }
        sb_add(&obj, "o \n"); // mesh.name: map meshes have none
        for (int v = 0; v < g->nv; v++) {
            const double x = g->pos[v * 3], y = g->pos[v * 3 + 1], z = g->pos[v * 3 + 2];
            sb_add(&obj, "v ");
            sb_num(&obj, e[0] * x + e[4] * y + e[8] * z + e[12]);
            sb_add(&obj, " ");
            sb_num(&obj, e[1] * x + e[5] * y + e[9] * z + e[13]);
            sb_add(&obj, " ");
            sb_num(&obj, e[2] * x + e[6] * y + e[10] * z + e[14]);
            sb_add(&obj, "\n");
        }
        for (int v = 0; v < g->nv; v++) {
            sb_add(&obj, "vt ");
            sb_num(&obj, g->uv[v * 2]);
            sb_add(&obj, " ");
            sb_num(&obj, g->uv[v * 2 + 1]);
            sb_add(&obj, "\n");
        }
        for (int v = 0; v < g->nv; v++) {
            const double x = g->nrm[v * 3], y = g->nrm[v * 3 + 1], z = g->nrm[v * 3 + 2];
            double w[3] = {nm[0] * x + nm[1] * y + nm[2] * z, nm[3] * x + nm[4] * y + nm[5] * z, nm[6] * x + nm[7] * y + nm[8] * z};
            const double l = sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
            if (l > 0) w[0] /= l, w[1] /= l, w[2] /= l;
            sb_add(&obj, "vn ");
            sb_num(&obj, w[0]);
            sb_add(&obj, " ");
            sb_num(&obj, w[1]);
            sb_add(&obj, " ");
            sb_num(&obj, w[2]);
            sb_add(&obj, "\n");
        }
        snprintf(line, sizeof(line), "usemtl material%zu\n", i);
        sb_add(&obj, line);
        for (int f = 0; f + 2 < g->ni; f += 3) {
            sb_add(&obj, "f");
            for (int c = 0; c < 3; c++) {
                const size_t ix = g->idx[f + c] + 1;
                snprintf(line, sizeof(line), " %zu/%zu/%zu", off_v + ix, off_t + ix, off_n + ix);
                sb_add(&obj, line);
            }
            sb_add(&obj, "\n");
        }
        off_v += (size_t) g->nv;
        off_t += (size_t) g->nv;
        off_n += (size_t) g->nv;

        snprintf(line, sizeof(line), "newmtl material%zu\n", i);
        sb_add(&mtl, line);
        sb_add(&mtl, "Ns 10.0000\nNi 1.5000\nd 1.0000\nTr 0.0000\nTf 1.0000 1.0000 1.0000\nillum 2\n");
        for (int k = 0; k < 2; k++) {
            sb_add(&mtl, k ? "Kd " : "Ka ");
            for (int c = 0; c < 3; c++) {
                sb_num(&mtl, pt->color[c]);
                sb_add(&mtl, " ");
            }
            sb_add(&mtl, "\n");
        }
        sb_add(&mtl, "Ks 0.0000 0.0000 0.0000\nKe 0.0000 0.0000 0.0000\n");
        if (pt->tex_name[0]) {
            // intentional difference from v7.1.4: the full file name (v7: "/<name minus its last char>")
            sb_add(&mtl, "map_Ka ");
            sb_add(&mtl, pt->tex_name);
            sb_add(&mtl, "\nmap_Kd ");
            sb_add(&mtl, pt->tex_name);
            sb_add(&mtl, "\n");
            int seen = 0;
            for (size_t k = 0; k < out->image_count && !seen; k++) seen = !strcmp(out->images[k].name, pt->tex_name);
            if (!seen && pt->tex_path[0] && file_exists(pt->tex_path)) {
                EdExportImage *im = realloc(out->images, (out->image_count + 1) * sizeof(EdExportImage));
                if (im) {
                    out->images = im;
                    snprintf(im[out->image_count].name, sizeof(im[0].name), "%s", pt->tex_name);
                    snprintf(im[out->image_count].path, sizeof(im[0].path), "%s", pt->tex_path);
                    out->image_count++;
                }
            }
        }
    }
    if (!mtl.buf) sb_add(&mtl, "");
    out->parts = (int) ps.n;
    parts_free(&ps);
    if (obj.fail || mtl.fail) {
        free(obj.buf);
        free(mtl.buf);
        free(out->images);
        memset(out, 0, sizeof(*out));
        return 0;
    }
    out->obj = obj.buf;
    out->mtl = mtl.buf;
    return 1;
}

void ed_export_obj_free(EdExportObj *out) {
    if (!out) return;
    free(out->obj);
    free(out->mtl);
    free(out->images);
    memset(out, 0, sizeof(*out));
}

// ---------------------------------------------------------------------------
// files, base64, crc32, zip

static unsigned char *export_read_file(const char *path, size_t *len) {
    *len = 0;
    ensure_asset_file(path);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = size >= 0 ? malloc((size_t) size + 1) : NULL;
    if (b && fread(b, 1, (size_t) size, f) != (size_t) size) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) *len = (size_t) size;
    return b;
}

static void b64_append(Sb *s, const unsigned char *d, const size_t n) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    sb_grow(s, (n + 2) / 3 * 4);
    if (s->fail) return;
    char *o = s->buf + s->len;
    for (size_t i = 0; i < n; i += 3) {
        const unsigned v = (unsigned) d[i] << 16 | (i + 1 < n ? (unsigned) d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        *o++ = tab[v >> 18 & 63];
        *o++ = tab[v >> 12 & 63];
        *o++ = i + 1 < n ? tab[v >> 6 & 63] : '=';
        *o++ = i + 2 < n ? tab[v & 63] : '=';
    }
    s->len = (size_t) (o - s->buf);
    s->buf[s->len] = 0;
}

unsigned int ed_crc32(const unsigned char *data, const size_t size) {
    static unsigned int table[256];
    static int ready;
    if (!ready) {
        for (unsigned int i = 0; i < 256; i++) {
            unsigned int c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = 1;
    }
    unsigned int c = 0xffffffffu;
    for (size_t i = 0; i < size; i++) c = table[(c ^ data[i]) & 255] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static void put16(unsigned char *p, const unsigned v) {
    p[0] = (unsigned char) (v & 255);
    p[1] = (unsigned char) (v >> 8 & 255);
}

static void put32(unsigned char *p, const unsigned v) {
    put16(p, v & 0xffff);
    put16(p + 2, v >> 16);
}

unsigned char *ed_zip_store(const EdZipEntry *entries, const size_t count, size_t *len) {
    // local file headers + data, the central directory, the end record; stored (method 0),
    // a fixed DOS date (1980-01-01 00:00) so the output only depends on the files
    size_t total = 22;
    for (size_t i = 0; i < count; i++) total += 30 + 46 + 2 * strlen(entries[i].name) + entries[i].size;
    unsigned char *z = calloc(1, total);
    if (!z) return NULL;
    unsigned int *offsets = calloc(count ? count : 1, sizeof(unsigned int));
    unsigned int *crcs = calloc(count ? count : 1, sizeof(unsigned int));
    if (!offsets || !crcs) {
        free(z);
        free(offsets);
        free(crcs);
        return NULL;
    }
    size_t o = 0;
    for (size_t i = 0; i < count; i++) {
        const size_t nl = strlen(entries[i].name);
        crcs[i] = ed_crc32(entries[i].data, entries[i].size);
        offsets[i] = (unsigned int) o;
        put32(z + o, 0x04034b50u);
        put16(z + o + 4, 20);
        put16(z + o + 6, 0x0800); // UTF-8 names
        put16(z + o + 8, 0);
        put16(z + o + 10, 0);
        put16(z + o + 12, 0x0021);
        put32(z + o + 14, crcs[i]);
        put32(z + o + 18, (unsigned) entries[i].size);
        put32(z + o + 22, (unsigned) entries[i].size);
        put16(z + o + 26, (unsigned) nl);
        put16(z + o + 28, 0);
        memcpy(z + o + 30, entries[i].name, nl);
        if (entries[i].size) memcpy(z + o + 30 + nl, entries[i].data, entries[i].size);
        o += 30 + nl + entries[i].size;
    }
    const size_t cd = o;
    for (size_t i = 0; i < count; i++) {
        const size_t nl = strlen(entries[i].name);
        put32(z + o, 0x02014b50u);
        put16(z + o + 4, 20);
        put16(z + o + 6, 20);
        put16(z + o + 8, 0x0800);
        put16(z + o + 10, 0);
        put16(z + o + 12, 0);
        put16(z + o + 14, 0x0021);
        put32(z + o + 16, crcs[i]);
        put32(z + o + 20, (unsigned) entries[i].size);
        put32(z + o + 24, (unsigned) entries[i].size);
        put16(z + o + 28, (unsigned) nl);
        put32(z + o + 42, offsets[i]);
        memcpy(z + o + 46, entries[i].name, nl);
        o += 46 + nl;
    }
    put32(z + o, 0x06054b50u);
    put16(z + o + 8, (unsigned) count);
    put16(z + o + 10, (unsigned) count);
    put32(z + o + 12, (unsigned) (o - cd));
    put32(z + o + 16, (unsigned) cd);
    o += 22;
    free(offsets);
    free(crcs);
    *len = o;
    return z;
}

unsigned char *ed_export_mtl_zip(const char *name, const EdModelObj *objs, const size_t n, const char *assets_root, size_t *len) {
    *len = 0;
    if (!name || !*name) name = "map";
    EdExportObj e;
    if (!ed_export_obj(name, objs, n, assets_root, &e)) return NULL;
    const size_t count = 2 + e.image_count;
    EdZipEntry *z = calloc(count, sizeof(EdZipEntry));
    unsigned char **files = calloc(count, sizeof(unsigned char *));
    char obj_name[160], mtl_name[160];
    snprintf(obj_name, sizeof(obj_name), "%s.obj", name);
    snprintf(mtl_name, sizeof(mtl_name), "%s.mtl", name);
    unsigned char *out = NULL;
    if (z && files) {
        z[0] = (EdZipEntry) {obj_name, (const unsigned char *) e.obj, strlen(e.obj)};
        z[1] = (EdZipEntry) {mtl_name, (const unsigned char *) e.mtl, strlen(e.mtl)};
        size_t k = 2;
        for (size_t i = 0; i < e.image_count; i++) {
            size_t size = 0;
            files[k] = export_read_file(e.images[i].path, &size);
            if (!files[k]) continue;
            z[k] = (EdZipEntry) {e.images[i].name, files[k], size};
            k++;
        }
        out = ed_zip_store(z, k, len);
        for (size_t i = 2; i < k; i++) free(files[i]);
    }
    free(z);
    free(files);
    ed_export_obj_free(&e);
    return out;
}

// ---------------------------------------------------------------------------
// GLTF (GLTFExporter.parse, trs, embedImages; JSON.stringify(gltf, null, 2))

static void json_pretty(Sb *s, const cJSON *v, const int depth) {
    char pad[256];
    const int n = depth * 2 < (int) sizeof(pad) - 3 ? depth * 2 : (int) sizeof(pad) - 3;
    if (cJSON_IsObject(v) || cJSON_IsArray(v)) {
        const int obj = cJSON_IsObject(v);
        if (!v->child) {
            sb_add(s, obj ? "{}" : "[]");
            return;
        }
        sb_add(s, obj ? "{\n" : "[\n");
        memset(pad, ' ', (size_t) n + 2);
        pad[n + 2] = 0;
        for (const cJSON *c = v->child; c; c = c->next) {
            sb_add(s, pad);
            if (obj) {
                cJSON *key = cJSON_CreateString(c->string);
                char *k = key ? ed_model_print(key) : NULL;
                if (k) sb_add(s, k);
                free(k);
                cJSON_Delete(key);
                sb_add(s, ": ");
            }
            json_pretty(s, c, depth + 1);
            sb_add(s, c->next ? ",\n" : "\n");
        }
        pad[n] = 0;
        sb_add(s, pad);
        sb_add(s, obj ? "}" : "]");
    } else if (cJSON_IsNumber(v)) {
        sb_num(s, v->valuedouble);
    } else if (cJSON_IsString(v)) {
        char *t = ed_model_print(v);
        if (t) sb_add(s, t);
        else s->fail = 1;
        free(t);
    } else if (cJSON_IsBool(v)) {
        sb_add(s, cJSON_IsTrue(v) ? "true" : "false");
    } else {
        sb_add(s, "null");
    }
}

static cJSON *arr_of(cJSON *root, const char *key) {
    cJSON *a = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!a) a = cJSON_AddArrayToObject(root, key);
    return a;
}

static cJSON *num_array(const double *v, const int n) {
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(a, cJSON_CreateNumber(v[i]));
    return a;
}

static int all_eq(const double *v, const int n, const double x) {
    for (int i = 0; i < n; i++) if (v[i] != x) return 0;
    return 1;
}

typedef struct {
    cJSON *root;
    Sb bin; // the buffer (binary, base64 at the end)
} Gltf;

enum { GL_FLOAT_ = 5126, GL_UNSIGNED_SHORT_ = 5123, GL_UNSIGNED_INT_ = 5125, GL_ARRAY_BUFFER_ = 34962, GL_ELEMENT_ARRAY_BUFFER_ = 34963 };

// processAccessor: the data as a bufferView (padded to 4 bytes) + an accessor with min / max
static int gltf_accessor(Gltf *g, const void *data, const int count, const int item, const int component, const int target) {
    const int bytes = component == GL_UNSIGNED_SHORT_ ? 2 : 4;
    const size_t raw = (size_t) count * (size_t) item * (size_t) bytes, padded = (raw + 3) & ~(size_t) 3;
    double mn[4] = {INFINITY, INFINITY, INFINITY, INFINITY}, mx[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
    const size_t at = g->bin.len;
    sb_grow(&g->bin, padded);
    if (g->bin.fail) return -1;
    unsigned char *o = (unsigned char *) g->bin.buf + at;
    memset(o, 0, padded);
    for (int i = 0; i < count; i++) {
        for (int k = 0; k < item; k++) {
            double v;
            if (component == GL_FLOAT_) {
                const float f = ((const float *) data)[i * item + k];
                v = f;
                memcpy(o + ((size_t) i * (size_t) item + (size_t) k) * 4, &f, 4); // little-endian hosts
            } else {
                const unsigned int u = ((const unsigned int *) data)[i * item + k];
                v = u;
                if (component == GL_UNSIGNED_SHORT_) put16(o + ((size_t) i * (size_t) item + (size_t) k) * 2, u);
                else put32(o + ((size_t) i * (size_t) item + (size_t) k) * 4, u);
            }
            if (v < mn[k]) mn[k] = v;
            if (v > mx[k]) mx[k] = v;
        }
    }
    g->bin.len = at + padded;
    cJSON *views = arr_of(g->root, "bufferViews");
    if (!cJSON_GetObjectItemCaseSensitive(g->root, "buffers")) {
        cJSON *b = cJSON_AddArrayToObject(g->root, "buffers");
        cJSON *b0 = cJSON_CreateObject();
        cJSON_AddNumberToObject(b0, "byteLength", 0);
        cJSON_AddItemToArray(b, b0);
    }
    cJSON *view = cJSON_CreateObject();
    cJSON_AddNumberToObject(view, "buffer", 0);
    cJSON_AddNumberToObject(view, "byteOffset", (double) at);
    cJSON_AddNumberToObject(view, "byteLength", (double) padded);
    cJSON_AddNumberToObject(view, "target", target);
    if (target == GL_ARRAY_BUFFER_) cJSON_AddNumberToObject(view, "byteStride", item * bytes);
    cJSON_AddItemToArray(views, view);
    cJSON *acc = cJSON_CreateObject();
    cJSON_AddNumberToObject(acc, "bufferView", cJSON_GetArraySize(views) - 1);
    cJSON_AddNumberToObject(acc, "componentType", component);
    cJSON_AddNumberToObject(acc, "count", count);
    cJSON_AddItemToObject(acc, "max", num_array(mx, item));
    cJSON_AddItemToObject(acc, "min", num_array(mn, item));
    static const char *const types[5] = {"", "SCALAR", "VEC2", "VEC3", "VEC4"};
    cJSON_AddStringToObject(acc, "type", types[item]);
    cJSON *accs = arr_of(g->root, "accessors");
    cJSON_AddItemToArray(accs, acc);
    return cJSON_GetArraySize(accs) - 1;
}

typedef struct {
    char path[512];
    int texture;
} TexSlot;

char *ed_export_gltf(const char *name, const EdModelObj *objs, const size_t n, const char *assets_root, size_t *len) {
    if (len) *len = 0;
    if (!name) name = "";
    Parts ps = {0};
    build_parts(&ps, objs, n, assets_root);
    Gltf g = {cJSON_CreateObject(), {0}};
    cJSON *asset = cJSON_AddObjectToObject(g.root, "asset");
    cJSON_AddStringToObject(asset, "version", "2.0");
    cJSON_AddStringToObject(asset, "generator", "THREE.GLTFExporter");
    cJSON *scenes = cJSON_AddArrayToObject(g.root, "scenes");
    cJSON_AddNumberToObject(g.root, "scene", 0);
    cJSON *scene = cJSON_CreateObject();
    if (name[0]) cJSON_AddStringToObject(scene, "name", name);
    cJSON_AddItemToArray(scenes, scene);
    cJSON *scene_nodes = cJSON_CreateArray();
    TexSlot *slots = NULL;
    size_t slot_count = 0;
    for (size_t i = 0; i < ps.n; i++) {
        Part *pt = &ps.v[i];
        Geo *geo = &pt->g;
        cJSON *nodes = arr_of(g.root, "nodes");
        (void) nodes;
        // TEXCOORD_0 flipped instead of the image (intentional difference, see the header)
        for (int v = 0; v < geo->nv; v++) geo->uv[v * 2 + 1] = 1.0f - geo->uv[v * 2 + 1];
        cJSON *attrs = cJSON_CreateObject();
        cJSON_AddNumberToObject(attrs, "POSITION", gltf_accessor(&g, geo->pos, geo->nv, 3, GL_FLOAT_, GL_ARRAY_BUFFER_));
        cJSON_AddNumberToObject(attrs, "NORMAL", gltf_accessor(&g, geo->nrm, geo->nv, 3, GL_FLOAT_, GL_ARRAY_BUFFER_));
        cJSON_AddNumberToObject(attrs, "TEXCOORD_0", gltf_accessor(&g, geo->uv, geo->nv, 2, GL_FLOAT_, GL_ARRAY_BUFFER_));
        const int indices = gltf_accessor(&g, geo->idx, geo->ni, 1, geo->nv > 65535 ? GL_UNSIGNED_INT_ : GL_UNSIGNED_SHORT_, GL_ELEMENT_ARRAY_BUFFER_);
        // processMaterial (a MeshLambertMaterial: metallic / roughness 0.5)
        cJSON *mats = arr_of(g.root, "materials");
        cJSON *mat = cJSON_CreateObject();
        cJSON *pbr = cJSON_AddObjectToObject(mat, "pbrMetallicRoughness");
        const double base[4] = {pt->color[0], pt->color[1], pt->color[2], pt->opacity};
        if (!(base[0] == 1 && base[1] == 1 && base[2] == 1 && base[3] == 1)) cJSON_AddItemToObject(pbr, "baseColorFactor", num_array(base, 4));
        cJSON_AddNumberToObject(pbr, "metallicFactor", 0.5);
        cJSON_AddNumberToObject(pbr, "roughnessFactor", 0.5);
        if (pt->tex_path[0] && file_exists(pt->tex_path)) {
            int tex = -1;
            for (size_t k = 0; k < slot_count && tex < 0; k++) if (!strcmp(slots[k].path, pt->tex_path)) tex = slots[k].texture;
            if (tex < 0) {
                size_t size = 0;
                unsigned char *png = export_read_file(pt->tex_path, &size);
                if (png) {
                    cJSON *textures = arr_of(g.root, "textures");
                    cJSON *samplers = arr_of(g.root, "samplers");
                    cJSON *sampler = cJSON_CreateObject();
                    cJSON_AddNumberToObject(sampler, "magFilter", 9728); // Krunker textures: NearestFilter
                    cJSON_AddNumberToObject(sampler, "minFilter", 9728);
                    cJSON_AddNumberToObject(sampler, "wrapS", 10497); // RepeatWrapping
                    cJSON_AddNumberToObject(sampler, "wrapT", 10497);
                    cJSON_AddItemToArray(samplers, sampler);
                    cJSON *images = arr_of(g.root, "images");
                    Sb uri = {0};
                    sb_add(&uri, "data:image/png;base64,");
                    b64_append(&uri, png, size);
                    free(png);
                    cJSON *image = cJSON_CreateObject();
                    cJSON_AddStringToObject(image, "mimeType", "image/png");
                    cJSON_AddStringToObject(image, "uri", uri.buf ? uri.buf : "");
                    free(uri.buf);
                    cJSON_AddItemToArray(images, image);
                    cJSON *t = cJSON_CreateObject();
                    cJSON_AddNumberToObject(t, "sampler", cJSON_GetArraySize(samplers) - 1);
                    cJSON_AddNumberToObject(t, "source", cJSON_GetArraySize(images) - 1);
                    cJSON_AddItemToArray(textures, t);
                    tex = cJSON_GetArraySize(textures) - 1;
                    TexSlot *grown = realloc(slots, (slot_count + 1) * sizeof(TexSlot));
                    if (grown) {
                        slots = grown;
                        snprintf(slots[slot_count].path, sizeof(slots[0].path), "%s", pt->tex_path);
                        slots[slot_count++].texture = tex;
                    }
                }
            }
            if (tex >= 0) {
                cJSON *bt = cJSON_AddObjectToObject(pbr, "baseColorTexture");
                cJSON_AddNumberToObject(bt, "index", tex);
            }
        }
        if (fmax(pt->emissive[0], fmax(pt->emissive[1], pt->emissive[2])) > 0) cJSON_AddItemToObject(mat, "emissiveFactor", num_array(pt->emissive, 3));
        if (pt->transparent) cJSON_AddStringToObject(mat, "alphaMode", "BLEND");
        if (pt->double_sided) cJSON_AddTrueToObject(mat, "doubleSided");
        cJSON_AddItemToArray(mats, mat);
        cJSON *prim = cJSON_CreateObject();
        cJSON_AddNumberToObject(prim, "mode", 4);
        cJSON_AddItemToObject(prim, "attributes", attrs);
        cJSON_AddNumberToObject(prim, "indices", indices);
        cJSON_AddNumberToObject(prim, "material", cJSON_GetArraySize(mats) - 1);
        cJSON *mesh = cJSON_CreateObject();
        cJSON *prims = cJSON_AddArrayToObject(mesh, "primitives");
        cJSON_AddItemToArray(prims, prim);
        cJSON *meshes = arr_of(g.root, "meshes");
        cJSON_AddItemToArray(meshes, mesh);
        // processNode (trs)
        cJSON *node = cJSON_CreateObject();
        if (!(pt->q[0] == 0 && pt->q[1] == 0 && pt->q[2] == 0 && pt->q[3] == 1)) cJSON_AddItemToObject(node, "rotation", num_array(pt->q, 4));
        if (!all_eq(pt->t, 3, 0)) cJSON_AddItemToObject(node, "translation", num_array(pt->t, 3));
        if (!all_eq(pt->s, 3, 1)) cJSON_AddItemToObject(node, "scale", num_array(pt->s, 3));
        cJSON_AddNumberToObject(node, "mesh", cJSON_GetArraySize(meshes) - 1);
        cJSON *nodes_arr = arr_of(g.root, "nodes");
        cJSON_AddItemToArray(nodes_arr, node);
        cJSON_AddItemToArray(scene_nodes, cJSON_CreateNumber(cJSON_GetArraySize(nodes_arr) - 1));
    }
    if (cJSON_GetArraySize(scene_nodes)) cJSON_AddItemToObject(scene, "nodes", scene_nodes);
    else cJSON_Delete(scene_nodes);
    free(slots);
    parts_free(&ps);
    cJSON *buffers = cJSON_GetObjectItemCaseSensitive(g.root, "buffers");
    if (buffers) {
        cJSON *b0 = cJSON_GetArrayItem(buffers, 0);
        cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(b0, "byteLength"), (double) g.bin.len);
        Sb uri = {0};
        sb_add(&uri, "data:application/octet-stream;base64,");
        b64_append(&uri, (const unsigned char *) g.bin.buf, g.bin.len);
        cJSON_AddStringToObject(b0, "uri", uri.buf ? uri.buf : "");
        free(uri.buf);
    }
    const int bin_fail = g.bin.fail;
    free(g.bin.buf);
    Sb out = {0};
    json_pretty(&out, g.root, 0);
    cJSON_Delete(g.root);
    if (out.fail || bin_fail) {
        free(out.buf);
        return NULL;
    }
    if (len) *len = out.len;
    return out.buf;
}

// ---------------------------------------------------------------------------
// editor glue

#ifndef ED_EXPORT_NO_EDITOR
#include "editor_internal.h"

static EdModelObj *project_objects(size_t *count, char *name, const size_t name_size) {
    const EditorProject *p = g_ed.proj;
    *count = p ? p->count : 0;
    snprintf(name, name_size, "%s", p ? ed_json_str(p->map_config, "name", "") : "");
    EdModelObj *list = calloc(*count ? *count : 1, sizeof(EdModelObj));
    if (!list) return NULL;
    for (size_t i = 0; i < *count; i++) list[i] = (EdModelObj) {p->objects[i]->data, p->objects[i]->prefab, p->objects[i]->uuid};
    return list;
}

void ed_file_export_gltf(void) {
    // exportScene: "Nothing to Export" without objects
    size_t n;
    char name[160], file[200];
    EdModelObj *list = project_objects(&n, name, sizeof(name));
    if (!list) return;
    if (!n) {
        free(list);
        editor_message("Nothing to Export");
        return;
    }
    size_t len = 0;
    char *text = ed_export_gltf(name, list, n, assets_path(), &len);
    free(list);
    if (!text) {
        editor_message("Export failed.");
        return;
    }
    snprintf(file, sizeof(file), "%s.gltf", name);
    ed_download_bytes(text, len, file, "application/json");
    free(text);
}

void ed_file_export_mtl(void) {
    // exportScene2: "Nothing to Export", then alert("Big maps can take a while")
    size_t n;
    char name[160], file[200];
    EdModelObj *list = project_objects(&n, name, sizeof(name));
    if (!list) return;
    if (!n) {
        free(list);
        editor_message("Nothing to Export");
        return;
    }
    editor_message("Big maps can take a while");
    size_t len = 0;
    unsigned char *zip = ed_export_mtl_zip(name, list, n, assets_path(), &len);
    free(list);
    if (!zip) {
        editor_message("Export failed.");
        return;
    }
    snprintf(file, sizeof(file), "%s.zip", name);
    ed_download_bytes(zip, len, file, "application/zip");
    free(zip);
}
#endif
