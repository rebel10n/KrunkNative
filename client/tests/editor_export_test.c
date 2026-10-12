// editor_export_test (doc/v7/editor.md WP6): File > Export GLTF / Export MTL of the map editor
// (client/src/editor/editor_export.c, GL-free).
//
//   editor_export_test <repo> [--update]
//
// 1. Goldens: client/tests/data/editor_export/map.json (cubes incl. a face mask, ramps, a
//    cylinder, a sphere, a plane, a model prefab, textured, semi-transparent, invisible and tool
//    objects) exported without an asset tree must give map.obj.txt (the OBJ), map.mtl and map.gltf of that
//    directory byte for byte (--update rewrites them). Without assets the model is left out, so
//    the goldens do not depend on the gitignored game content.
// 2. Round trips, with and without <repo>/assets (the asset part is skipped when the tree has no
//    models / textures): the OBJ read back with fast_obj has the vertices and triangles written,
//    every normal is unit length; the .gltf parses, loads its data-URI buffer and validates with
//    cgltf, with the OBJ's triangle and vertex counts; the zip's local headers and central
//    directory agree, the CRCs match, and it holds exactly <name>.obj, <name>.mtl and the PNGs the
//    MTL references (with assets: the model and the texture images are there).
#define CGLTF_IMPLEMENTATION
#include "../lib/cgltf/cgltf.h"
#include "editor_export.h"
#include "editor_model.h"
#include <fast_obj.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks;

#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t) n + 1);
    if (buf && fread(buf, 1, (size_t) n, f) != (size_t) n) {
        free(buf);
        buf = NULL;
    }
    if (buf) buf[n] = 0;
    fclose(f);
    if (len) *len = buf ? (size_t) n : 0;
    return buf;
}

static int write_file(const char *path, const void *data, const size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    const int ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static void golden(const char *dir, const char *file, const char *text, const int update) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    if (update) {
        CHECK(write_file(path, text, strlen(text)), "write %s", path);
        printf("wrote %s\n", path);
        return;
    }
    char *want = read_file(path, NULL);
    CHECK(want != NULL, "missing golden %s (run with --update)", path);
    if (!want) return;
    const int same = !strcmp(want, text);
    if (!same) {
        size_t i = 0;
        while (want[i] && want[i] == text[i]) i++;
        size_t line = 1;
        for (size_t k = 0; k < i; k++) line += want[k] == '\n';
        CHECK(0, "%s differs from the golden at line %zu", file, line);
    } else {
        g_checks++;
    }
    free(want);
}

static int count_prefix(const char *text, const char *prefix) {
    int n = 0;
    const size_t l = strlen(prefix);
    for (const char *p = text; p && *p;) {
        if (!strncmp(p, prefix, l)) n++;
        p = strchr(p, '\n');
        if (p) p++;
    }
    return n;
}

static void check_normals(const char *obj) {
    int bad = 0, n = 0;
    for (const char *p = obj; p && *p;) {
        if (!strncmp(p, "vn ", 3)) {
            double x, y, z;
            if (sscanf(p + 3, "%lf %lf %lf", &x, &y, &z) == 3) {
                n++;
                if (fabs(sqrt(x * x + y * y + z * z) - 1) > 1e-6) bad++;
            }
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    CHECK(bad == 0 && n > 0, "%d of %d normals are not unit length", bad, n);
}

static unsigned le16(const unsigned char *p) { return (unsigned) p[0] | (unsigned) p[1] << 8; }
static unsigned le32(const unsigned char *p) { return le16(p) | le16(p + 2) << 16; }

// the zip's files (central directory, cross-checked with the local headers and CRCs)
static int zip_files(const unsigned char *z, const size_t len, char names[][128], const unsigned char **data, size_t *sizes, const int max) {
    if (len < 22 || le32(z + len - 22) != 0x06054b50u) return -1;
    const unsigned count = le16(z + len - 22 + 10), cd = le32(z + len - 22 + 16);
    size_t o = cd;
    int n = 0;
    for (unsigned i = 0; i < count && n < max; i++) {
        if (o + 46 > len || le32(z + o) != 0x02014b50u) return -1;
        const unsigned crc = le32(z + o + 16), size = le32(z + o + 20), nl = le16(z + o + 28), lo = le32(z + o + 42);
        if (le16(z + o + 10) != 0 || le32(z + o + 24) != size || nl >= 128) return -1;
        memcpy(names[n], z + o + 46, nl);
        names[n][nl] = 0;
        if (lo + 30 > len || le32(z + lo) != 0x04034b50u || le16(z + lo + 26) != nl || memcmp(z + lo + 30, names[n], nl)) return -1;
        if (le32(z + lo + 14) != crc || le32(z + lo + 18) != size) return -1;
        const size_t at = lo + 30 + nl + le16(z + lo + 28);
        if (at + size > len || ed_crc32(z + at, size) != crc) return -1;
        data[n] = z + at;
        sizes[n] = size;
        n++;
        o += 46 + nl + le16(z + o + 30) + le16(z + o + 32);
    }
    return n;
}

static void run(const char *label, const EdModelObj *objs, const size_t n, const char *assets, const char *tmp, int *parts_out) {
    EdExportObj e;
    CHECK(ed_export_obj("export_test", objs, n, assets, &e), "%s: ed_export_obj", label);
    if (!e.obj) return;
    *parts_out = e.parts;
    check_normals(e.obj);
    const int verts = count_prefix(e.obj, "v "), tris = count_prefix(e.obj, "f ");
    CHECK(count_prefix(e.obj, "vt ") == verts && count_prefix(e.obj, "vn ") == verts, "%s: v / vt / vn counts differ", label);
    CHECK(count_prefix(e.obj, "o ") == e.parts && count_prefix(e.mtl, "newmtl ") == e.parts, "%s: %d parts, o / newmtl counts differ", label, e.parts);

    // OBJ back through fast_obj
    char obj_path[1024], mtl_path[1024];
    snprintf(obj_path, sizeof(obj_path), "%s/export_test.obj", tmp);
    snprintf(mtl_path, sizeof(mtl_path), "%s/export_test.mtl", tmp);
    CHECK(write_file(obj_path, e.obj, strlen(e.obj)) && write_file(mtl_path, e.mtl, strlen(e.mtl)), "%s: write %s", label, obj_path);
    fastObjMesh *m = fast_obj_read(obj_path);
    CHECK(m != NULL, "%s: fast_obj_read", label);
    if (m) {
        CHECK((int) m->position_count - 1 == verts, "%s: fast_obj positions %u, written %d", label, m->position_count - 1, verts);
        CHECK((int) m->face_count == tris, "%s: fast_obj faces %u, written %d", label, m->face_count, tris);
        int tri_faces = 1;
        for (unsigned f = 0; f < m->face_count; f++) tri_faces &= m->face_vertices[f] == 3;
        CHECK(tri_faces, "%s: non-triangle faces", label);
        CHECK((int) m->material_count == e.parts, "%s: fast_obj materials %u, parts %d", label, m->material_count, e.parts);
        fast_obj_destroy(m);
    }
    remove(obj_path);
    remove(mtl_path);

    // GLTF through cgltf
    size_t glen = 0;
    char *gltf = ed_export_gltf("export_test", objs, n, assets, &glen);
    CHECK(gltf && glen == strlen(gltf), "%s: ed_export_gltf", label);
    if (gltf) {
        cgltf_options opt;
        memset(&opt, 0, sizeof(opt));
        cgltf_data *d = NULL;
        cgltf_result r = cgltf_parse(&opt, gltf, glen, &d);
        CHECK(r == cgltf_result_success, "%s: cgltf_parse %d", label, (int) r);
        if (r == cgltf_result_success) {
            r = cgltf_load_buffers(&opt, d, NULL);
            CHECK(r == cgltf_result_success, "%s: cgltf_load_buffers %d", label, (int) r);
            r = cgltf_validate(d);
            CHECK(r == cgltf_result_success, "%s: cgltf_validate %d", label, (int) r);
            size_t gtris = 0, gverts = 0;
            for (size_t i = 0; i < d->meshes_count; i++) {
                for (size_t k = 0; k < d->meshes[i].primitives_count; k++) {
                    const cgltf_primitive *p = &d->meshes[i].primitives[k];
                    if (p->indices) gtris += p->indices->count / 3;
                    for (size_t a = 0; a < p->attributes_count; a++) if (p->attributes[a].type == cgltf_attribute_type_position) gverts += p->attributes[a].data->count;
                }
            }
            CHECK((int) d->meshes_count == e.parts && (int) d->nodes_count == e.parts, "%s: gltf meshes %zu nodes %zu, parts %d", label, d->meshes_count, d->nodes_count, e.parts);
            CHECK((int) gtris == tris, "%s: gltf triangles %zu, OBJ %d", label, gtris, tris);
            CHECK((int) gverts == verts, "%s: gltf vertices %zu, OBJ %d", label, gverts, verts);
            CHECK(d->accessors_count == (size_t) e.parts * 4, "%s: gltf accessors %zu", label, d->accessors_count);
            CHECK(d->images_count == e.image_count, "%s: gltf images %zu, MTL images %zu", label, d->images_count, e.image_count);
            cgltf_free(d);
        }
        free(gltf);
    }

    // the zip
    size_t zlen = 0;
    unsigned char *zip = ed_export_mtl_zip("export_test", objs, n, assets, &zlen);
    CHECK(zip != NULL, "%s: ed_export_mtl_zip", label);
    if (zip) {
        char names[64][128];
        const unsigned char *data[64];
        size_t sizes[64];
        const int files = zip_files(zip, zlen, names, data, sizes, 64);
        CHECK(files == (int) (2 + e.image_count), "%s: zip holds %d files, want %zu", label, files, 2 + e.image_count);
        if (files >= 2) {
            CHECK(!strcmp(names[0], "export_test.obj") && sizes[0] == strlen(e.obj) && !memcmp(data[0], e.obj, sizes[0]), "%s: zip obj", label);
            CHECK(!strcmp(names[1], "export_test.mtl") && sizes[1] == strlen(e.mtl) && !memcmp(data[1], e.mtl, sizes[1]), "%s: zip mtl", label);
            for (int i = 2; i < files; i++) {
                char ref[160];
                snprintf(ref, sizeof(ref), "map_Kd %s\n", names[i]);
                CHECK(strstr(e.mtl, ref) != NULL, "%s: zip file %s not referenced by the MTL", label, names[i]);
                CHECK(sizes[i] > 8 && !memcmp(data[i], "\x89PNG", 4), "%s: %s is not a PNG", label, names[i]);
            }
        }
        free(zip);
    }
    ed_export_obj_free(&e);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: editor_export_test <repo> [--update]\n");
        return 2;
    }
    const char *repo = argv[1];
    const int update = argc > 2 && !strcmp(argv[2], "--update");
    char dir[1024], path[1100], assets[1100], probe[1200];
    snprintf(dir, sizeof(dir), "%s/client/tests/data/editor_export", repo);
    snprintf(path, sizeof(path), "%s/map.json", dir);
    char *text = read_file(path, NULL);
    cJSON *map = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!map) {
        printf("FAIL: cannot read %s\n", path);
        return 1;
    }
    const cJSON *src = cJSON_GetObjectItemCaseSensitive(map, "objects");
    const int n = cJSON_GetArraySize(src);
    EdModelObj *objs = calloc((size_t) n, sizeof(EdModelObj));
    cJSON **held = calloc((size_t) n, sizeof(cJSON *));
    int count = 0;
    for (int i = 0; i < n; i++) {
        const cJSON *o = cJSON_GetArrayItem(src, i);
        held[count] = ed_model_object(o, ED_MODEL_EXACT_TRANSFORM);
        CHECK(held[count] != NULL, "object %d does not load", i);
        if (!held[count]) continue;
        objs[count] = (EdModelObj) {held[count], ed_model_prefab(held[count]), (uint32_t) (count + 1)};
        count++;
    }

    // 1. goldens (no asset tree)
    EdExportObj e;
    if (ed_export_obj("export_test", objs, (size_t) count, NULL, &e)) {
        golden(dir, "map.obj.txt", e.obj, update); // .txt: *.obj is gitignored
        golden(dir, "map.mtl", e.mtl, update);
        CHECK(e.parts == 8, "parts without assets: %d, want 8 (the model, the invisible cube and the spawn are left out)", e.parts);
        ed_export_obj_free(&e);
    }
    size_t glen = 0;
    char *gltf = ed_export_gltf("export_test", objs, (size_t) count, NULL, &glen);
    if (gltf) golden(dir, "map.gltf", gltf, update);
    CHECK(gltf != NULL, "ed_export_gltf");
    free(gltf);

    // 2. round trips
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = ".";
    int parts = 0;
    run("no assets", objs, (size_t) count, NULL, tmp, &parts);
    snprintf(assets, sizeof(assets), "%s/assets", repo);
    snprintf(probe, sizeof(probe), "%s/models/crate_0.obj", assets);
    FILE *f = fopen(probe, "rb");
    if (f) {
        fclose(f);
        run("assets", objs, (size_t) count, assets, tmp, &parts);
        CHECK(parts == 9, "parts with assets: %d, want 9 (the crate model)", parts);
        EdExportObj a;
        if (ed_export_obj("export_test", objs, (size_t) count, assets, &a)) {
            CHECK(a.image_count >= 3, "texture images with assets: %zu", a.image_count);
            CHECK(strstr(a.mtl, "map_Kd crate_0_1.png\n") != NULL, "the crate variant texture is not referenced");
            ed_export_obj_free(&a);
        }
    } else {
        printf("skip: no %s (game assets not installed): model / texture round trip\n", probe);
    }

    for (int i = 0; i < count; i++) cJSON_Delete(held[i]);
    free(held);
    free(objs);
    cJSON_Delete(map);
    printf("editor_export_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
