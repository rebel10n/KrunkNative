// FBX class models: ufbx -> an in-memory GLB, so the whole glTF path (skins, clips, materials)
// is shared with GLB/GLTF assets (client/src/gltf.c gltf_model_load's .fbx branch).
// Space: glTF's right-handed Y-up, metres, baked by ufbx (load_opts.target_axes / target_unit_meters).
// Animations are baked to 30 fps TRS keys per anim stack (FBX curves have no direct glTF equivalent).
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <strings.h>
#include "../lib/ufbx/ufbx.h"

typedef struct {
    unsigned char *p;
    size_t n, cap;
} Buf;
static void put(Buf *b, const void *src, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 4096;
        b->p = realloc(b->p, b->cap);
        if (!b->p) { fprintf(stderr, "fbx: out of memory\n"); exit(1); }
    }
    memcpy(b->p + b->n, src, n);
    b->n += n;
}
static size_t tell(const Buf *b) { return b->n; }
static void align4(Buf *b) { static const unsigned char z[4]; put(b, z, (4 - b->n % 4) % 4); }

typedef struct {
    char *p;
    size_t n, cap;
} Str;
static void addf(Str *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[4096];
    const int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (s->n + (size_t) n + 1 > s->cap) {
        s->cap = (s->n + (size_t) n + 1) * 2 + 4096;
        s->p = realloc(s->p, s->cap);
        if (!s->p) { fprintf(stderr, "fbx: out of memory\n"); exit(1); }
    }
    memcpy(s->p + s->n, tmp, (size_t) n);
    s->n += (size_t) n;
    s->p[s->n] = 0;
}
static void esc(Str *s, const char *text) {
    addf(s, "\"");
    for (const char *c = text; *c; c++) {
        if (*c == '"' || *c == '\\') addf(s, "\\%c", *c);
        else if ((unsigned char) *c < 0x20) addf(s, "\\u%04x", *c);
        else addf(s, "%c", *c);
    }
    addf(s, "\"");
}

typedef struct {
    Str *v;
    size_t n, cap;
} List;
static void push(List *l, Str *s) {
    if (l->n == l->cap) {
        l->cap = l->cap * 2 + 8;
        l->v = realloc(l->v, l->cap * sizeof(Str));
    }
    l->v[l->n++] = *s;
    *s = (Str) {0};
}
static void join(Str *out, const List *l) {
    for (size_t i = 0; i < l->n; i++) addf(out, "%s%s", i ? "," : "", l->v[i].p ? l->v[i].p : "");
}

static int bv_add(Buf *bin, List *views, const void *data, size_t size) {
    align4(bin);
    const size_t off = tell(bin);
    put(bin, data, size);
    Str s = {0};
    addf(&s, "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}", off, size);
    push(views, &s);
    return (int) views->n - 1;
}
static int acc_add(List *accs, const int view, const int type, const int count, const char *kind,
                   const float *minmax, const int minmax_count) {
    Str s = {0};
    addf(&s, "{\"bufferView\":%d,\"componentType\":%d,\"count\":%d,\"type\":\"%s\"", view, type, count, kind);
    if (minmax) {
        addf(&s, ",\"min\":[");
        for (int i = 0; i < minmax_count / 2; i++) addf(&s, "%s%.9g", i ? "," : "", minmax[i]);
        addf(&s, "],\"max\":[");
        for (int i = 0; i < minmax_count / 2; i++) addf(&s, "%s%.9g", i ? "," : "", minmax[minmax_count / 2 + i]);
        addf(&s, "]");
    }
    addf(&s, "}");
    push(accs, &s);
    return (int) accs->n - 1;
}

static char *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 64 << 20) { fclose(f); return NULL; }
    char *data = malloc((size_t) n);
    const size_t got = data ? fread(data, 1, (size_t) n, f) : 0;
    fclose(f);
    if (!data || got != (size_t) n) { free(data); return NULL; }
    *size = (size_t) n;
    return data;
}
static const char *mime_of(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return "image/png";
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(dot, ".webp")) return "image/webp";
    return "image/png";
}

typedef struct {
    const ufbx_texture *tex;
    int index;
} TexRef;

int fbx_to_glb(const char *path, unsigned char **out, size_t *out_size) {
    ufbx_load_opts opts = {0};
    opts.generate_missing_normals = true;
    opts.space_conversion = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0;
    ufbx_error err;
    ufbx_scene *scene = ufbx_load_file(path, &opts, &err);
    if (!scene) {
        fprintf(stderr, "fbx: %s: %s\n", path, err.description.data);
        return 0;
    }
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    else snprintf(dir, sizeof(dir), ".");

    Buf bin = {0};
    List views = {0}, accs = {0}, meshes = {0}, mats = {0}, images = {0}, texs = {0}, skins = {0}, anims = {0};
    TexRef texref[256];
    int texref_count = 0;

    // materials: one per ufbx material, with any embedded or sibling image files as GLB bufferViews
    int *mat_of = calloc(scene->materials.count, sizeof(int));
    for (size_t i = 0; i < scene->materials.count; i++) {
        const ufbx_material *m = scene->materials.data[i];
        Str s = {0};
        addf(&s, "{\"name\":");
        esc(&s, m->name.data);
        const ufbx_vec4 base = m->pbr.base_color.value_vec4;
        const float rough = (float) m->pbr.roughness.value_real;
        const float metal = (float) m->pbr.metalness.value_real;
        addf(&s, ",\"pbrMetallicRoughness\":{\"baseColorFactor\":[%.6g,%.6g,%.6g,%.6g],\"metallicFactor\":%.6g,\"roughnessFactor\":%.6g}",
             base.x, base.y, base.z, base.w, metal, rough > 0.0f ? rough : 1.0f);
        const struct { const ufbx_material_map *map; int slot; } maps[] = {
            {&m->pbr.base_color, 0}, {&m->pbr.emission_color, 1}, {&m->pbr.normal_map, 2},
        };
        int base_tex = -1, normal_tex = -1;
        for (int k = 0; k < 3; k++) {
            const ufbx_texture *t = maps[k].map->texture;
            if (!t) continue;
            int found = -1;
            for (int r = 0; r < texref_count; r++) if (texref[r].tex == t) found = texref[r].index;
            if (found < 0) {
                size_t size = 0;
                char *bytes = NULL;
                const char *name = t->filename.data;
                if (t->content.size) {
                    size = t->content.size;
                    bytes = malloc(size);
                    memcpy(bytes, t->content.data, size);
                } else {
                    char file[1200];
                    const char *rel = t->relative_filename.data[0] ? t->relative_filename.data : t->filename.data;
                    snprintf(file, sizeof(file), "%s/%s", dir, rel);
                    bytes = read_file(file, &size);
                }
                if (!bytes) continue;
                const int view = bv_add(&bin, &views, bytes, size);
                free(bytes);
                Str im = {0};
                addf(&im, "{\"bufferView\":%d,\"mimeType\":\"%s\",\"name\":", view, mime_of(name));
                esc(&im, name);
                addf(&im, "}");
                push(&images, &im);
                Str tx = {0};
                addf(&tx, "{\"source\":%zu}", images.n - 1);
                push(&texs, &tx);
                found = (int) texs.n - 1;
                if (texref_count < 256) texref[texref_count++] = (TexRef) {t, found};
            }
            if (maps[k].slot == 0) base_tex = found;
            else if (maps[k].slot == 2) normal_tex = found;
        }
        if (base_tex >= 0) addf(&s, ",\"baseColorTexture\":{\"index\":%d}", base_tex);
        addf(&s, "}");
        if (normal_tex >= 0) addf(&s, ",\"normalTexture\":{\"index\":%d}", normal_tex);
        const ufbx_vec3 em = m->pbr.emission_color.value_vec3;
        if (em.x || em.y || em.z) addf(&s, ",\"emissiveFactor\":[%.6g,%.6g,%.6g]", em.x, em.y, em.z);
        addf(&s, "}");
        mat_of[i] = (int) i;
        push(&mats, &s);
    }

    // meshes, split per material, every corner its own vertex (small class models; no dedup needed)
    int *mesh_of = calloc(scene->meshes.count, sizeof(int));
    for (size_t i = 0; i < scene->meshes.count; i++) mesh_of[i] = -1;
    int *skin_of_mesh = calloc(scene->meshes.count, sizeof(int));
    for (size_t i = 0; i < scene->meshes.count; i++) skin_of_mesh[i] = -1;

    for (size_t mi = 0; mi < scene->meshes.count; mi++) {
        const ufbx_mesh *mesh = scene->meshes.data[mi];
        mesh_of[mi] = (int) meshes.n;
        Str prim_list = {0};
        for (size_t mati = 0; mati < (mesh->materials.count ? mesh->materials.count : 1); mati++) {
            // gather this material's triangles
            uint32_t *tris = malloc(mesh->max_face_triangles * 3 * sizeof(uint32_t));
            size_t *corners = NULL;
            size_t corner_count = 0, corner_cap = 0;
            for (size_t fi = 0; fi < mesh->faces.count; fi++) {
                if (mesh->materials.count && mesh->face_material.count && mesh->face_material.data[fi] != mati) continue;
                const ufbx_face face = mesh->faces.data[fi];
                const uint32_t n = ufbx_triangulate_face(tris, mesh->max_face_triangles * 3, mesh, face);
                for (uint32_t t = 0; t < n * 3; t++) { // n is a triangle count; tris are absolute mesh indices
                    if (corner_count == corner_cap) {
                        corner_cap = corner_cap * 2 + 1024;
                        corners = realloc(corners, corner_cap * sizeof(size_t));
                    }
                    corners[corner_count++] = tris[t];
                }
            }
            free(tris);
            if (!corner_count) { free(corners); continue; }
            const ufbx_skin_deformer *skin = mesh->skin_deformers.count ? mesh->skin_deformers.data[0] : NULL;
            const size_t vcount = corner_count;
            float *pos = malloc(vcount * 3 * sizeof(float));
            float *norm = malloc(vcount * 3 * sizeof(float));
            float *uv = malloc(vcount * 2 * sizeof(float));
            unsigned short *joints = skin ? calloc(vcount * 4, sizeof(unsigned short)) : NULL;
            float *weights = skin ? calloc(vcount * 4, sizeof(float)) : NULL;
            unsigned int *indices = malloc(vcount * sizeof(unsigned int));
            float minmax[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
            for (size_t c = 0; c < vcount; c++) {
                const size_t index = corners[c];
                const ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh->vertex_position, index);
                const ufbx_vec3 no = mesh->vertex_normal.values.count ? ufbx_get_vertex_vec3(&mesh->vertex_normal, index) : (ufbx_vec3) {0, 1, 0};
                pos[c * 3] = (float) p.x; pos[c * 3 + 1] = (float) p.y; pos[c * 3 + 2] = (float) p.z;
                norm[c * 3] = (float) no.x; norm[c * 3 + 1] = (float) no.y; norm[c * 3 + 2] = (float) no.z;
                if (mesh->vertex_uv.values.count) {
                    const ufbx_vec2 t = ufbx_get_vertex_vec2(&mesh->vertex_uv, index);
                    uv[c * 2] = (float) t.x; uv[c * 2 + 1] = (float) t.y;
                }
                for (int k = 0; k < 3; k++) {
                    if (pos[c * 3 + k] < minmax[k]) minmax[k] = pos[c * 3 + k];
                    if (pos[c * 3 + k] > minmax[3 + k]) minmax[3 + k] = pos[c * 3 + k];
                }
                indices[c] = (unsigned int) c;
                if (skin) {
                    const uint32_t vi = mesh->vertex_indices.data[index];
                    if (vi < skin->vertices.count) {
                        const ufbx_skin_vertex sv = skin->vertices.data[vi];
                        float total = 0.0f;
                        for (uint32_t w = 0; w < sv.num_weights && w < 4; w++) {
                            const ufbx_skin_weight sw = skin->weights.data[sv.weight_begin + w];
                            joints[c * 4 + w] = (unsigned short) sw.cluster_index;
                            weights[c * 4 + w] = (float) sw.weight;
                            total += (float) sw.weight;
                        }
                        if (total > 0.0f) for (int w = 0; w < 4; w++) weights[c * 4 + w] /= total;
                        else weights[c * 4] = 1.0f;
                    } else weights[c * 4] = 1.0f;
                }
            }
            free(corners);
            const int pa = acc_add(&accs, bv_add(&bin, &views, pos, vcount * 12), 5126, (int) vcount, "VEC3", minmax, 6);
            const int na = acc_add(&accs, bv_add(&bin, &views, norm, vcount * 12), 5126, (int) vcount, "VEC3", NULL, 0);
            const int ta = acc_add(&accs, bv_add(&bin, &views, uv, vcount * 8), 5126, (int) vcount, "VEC2", NULL, 0);
            int ja = -1, wa = -1;
            if (skin) {
                ja = acc_add(&accs, bv_add(&bin, &views, joints, vcount * 4 * sizeof(unsigned short)), 5123, (int) vcount, "VEC4", NULL, 0);
                wa = acc_add(&accs, bv_add(&bin, &views, weights, vcount * 4 * sizeof(float)), 5126, (int) vcount, "VEC4", NULL, 0);
                skin_of_mesh[mi] = 1;
            }
            const int ia = acc_add(&accs, bv_add(&bin, &views, indices, vcount * sizeof(unsigned int)), 5125, (int) vcount, "SCALAR", NULL, 0);
            free(pos); free(norm); free(uv); free(joints); free(weights); free(indices);
            size_t mat_index = 0;
            if (mesh->materials.count)
                for (size_t k = 0; k < scene->materials.count; k++) if (scene->materials.data[k] == mesh->materials.data[mati]) mat_index = k;
            addf(&prim_list, "%s{\"attributes\":{\"POSITION\":%d,\"NORMAL\":%d,\"TEXCOORD_0\":%d", prim_list.n ? "," : "", pa, na, ta);
            if (ja >= 0) addf(&prim_list, ",\"JOINTS_0\":%d,\"WEIGHTS_0\":%d", ja, wa);
            addf(&prim_list, "},\"indices\":%d", ia);
            if (mesh->materials.count) addf(&prim_list, ",\"material\":%zu", mat_index);
            addf(&prim_list, "}");
        }
        Str s = {0};
        addf(&s, "{\"primitives\":[%s]}", prim_list.p ? prim_list.p : "");
        free(prim_list.p);
        push(&meshes, &s);
    }

    // skins: one per mesh that has one, joints in cluster order
    int *skin_of = calloc(scene->meshes.count, sizeof(int));
    for (size_t mi = 0; mi < scene->meshes.count; mi++) skin_of[mi] = -1;
    for (size_t mi = 0; mi < scene->meshes.count; mi++) {
        const ufbx_mesh *mesh = scene->meshes.data[mi];
        if (mesh_of[mi] < 0 || !mesh->skin_deformers.count) continue;
        const ufbx_skin_deformer *skin = mesh->skin_deformers.data[0];
        const size_t nj = skin->clusters.count;
        float *ibm = calloc(nj * 16, sizeof(float));
        Str joints = {0};
        for (size_t j = 0; j < nj; j++) {
            const ufbx_skin_cluster *cl = skin->clusters.data[j];
            size_t node_index = 0;
            for (size_t k = 0; k < scene->nodes.count; k++) if (scene->nodes.data[k] == cl->bone_node) node_index = k;
            addf(&joints, "%s%zu", j ? "," : "", node_index);
            const ufbx_matrix m = cl->geometry_to_bone;
            const float v[16] = {
                (float) m.m00, (float) m.m10, (float) m.m20, 0,
                (float) m.m01, (float) m.m11, (float) m.m21, 0,
                (float) m.m02, (float) m.m12, (float) m.m22, 0,
                (float) m.m03, (float) m.m13, (float) m.m23, 1,
            };
            memcpy(ibm + j * 16, v, sizeof(v));
        }
        const int acc = acc_add(&accs, bv_add(&bin, &views, ibm, nj * 16 * sizeof(float)), 5126, (int) nj, "MAT4", NULL, 0);
        free(ibm);
        Str s = {0};
        addf(&s, "{\"inverseBindMatrices\":%d,\"joints\":[%s]}", acc, joints.p);
        free(joints.p);
        skin_of[mi] = (int) skins.n;
        push(&skins, &s);
    }

    // nodes: TRS from local_transform, meshes and skins attached
    Str nodes = {0};
    for (size_t ni = 0; ni < scene->nodes.count; ni++) {
        const ufbx_node *node = scene->nodes.data[ni];
        const ufbx_transform t = node->local_transform;
        addf(&nodes, "%s{\"name\":", ni ? "," : "");
        esc(&nodes, node->name.data);
        addf(&nodes, ",\"translation\":[%.9g,%.9g,%.9g],\"rotation\":[%.9g,%.9g,%.9g,%.9g],\"scale\":[%.9g,%.9g,%.9g]",
             t.translation.x, t.translation.y, t.translation.z, t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w,
             t.scale.x, t.scale.y, t.scale.z);
        if (node->mesh) {
            size_t mi = 0;
            for (size_t k = 0; k < scene->meshes.count; k++) if (scene->meshes.data[k] == node->mesh) mi = k;
            if (mesh_of[mi] >= 0) {
                addf(&nodes, ",\"mesh\":%d", mesh_of[mi]);
                if (skin_of[mi] >= 0) addf(&nodes, ",\"skin\":%d", skin_of[mi]);
            }
        }
        if (node->children.count) {
            addf(&nodes, ",\"children\":[");
            for (size_t c = 0; c < node->children.count; c++) {
                size_t ci = 0;
                for (size_t k = 0; k < scene->nodes.count; k++) if (scene->nodes.data[k] == node->children.data[c]) ci = k;
                addf(&nodes, "%s%zu", c ? "," : "", ci);
            }
            addf(&nodes, "]");
        }
        addf(&nodes, "}");
    }

    // animations: 30 fps TRS keys per stack, constant channels dropped
    for (size_t ai = 0; ai < scene->anim_stacks.count; ai++) {
        const ufbx_anim_stack *stack = scene->anim_stacks.data[ai];
        const double begin = stack->time_begin, end = stack->time_end;
        if (!(end > begin)) continue;
        const int frames = (int) fmin(600.0, ceil((end - begin) * 30.0)) + 1;
        float *times = malloc(frames * sizeof(float));
        for (int f = 0; f < frames; f++) times[f] = (float) ((end - begin) * f / (frames - 1));
        const int time_acc = acc_add(&accs, bv_add(&bin, &views, times, frames * sizeof(float)), 5126, frames, "SCALAR", NULL, 0);
        Str channels = {0}, samplers = {0};
        int sampler_count = 0;
        for (size_t ni = 0; ni < scene->nodes.count; ni++) {
            const ufbx_node *node = scene->nodes.data[ni];
            float *trs = malloc(frames * 3 * sizeof(float));
            float *rot = malloc(frames * 4 * sizeof(float));
            for (int kind = 0; kind < 3; kind++) {
                int constant = 1;
                for (int f = 0; f < frames; f++) {
                    const ufbx_transform t = ufbx_evaluate_transform(stack->anim, node, begin + times[f]);
                    if (kind == 0) { trs[f * 3] = (float) t.translation.x; trs[f * 3 + 1] = (float) t.translation.y; trs[f * 3 + 2] = (float) t.translation.z; }
                    else if (kind == 1) { trs[f * 3] = (float) t.scale.x; trs[f * 3 + 1] = (float) t.scale.y; trs[f * 3 + 2] = (float) t.scale.z; }
                    else { rot[f * 4] = (float) t.rotation.x; rot[f * 4 + 1] = (float) t.rotation.y; rot[f * 4 + 2] = (float) t.rotation.z; rot[f * 4 + 3] = (float) t.rotation.w; }
                    if (f) {
                        if (kind < 2 && memcmp(trs, trs + f * 3, 3 * sizeof(float))) constant = 0;
                        if (kind == 2 && memcmp(rot, rot + f * 4, 4 * sizeof(float))) constant = 0;
                    }
                }
                if (constant) continue;
                const int comps = kind == 2 ? 4 : 3;
                const int acc = acc_add(&accs, bv_add(&bin, &views, kind == 2 ? rot : trs, frames * comps * sizeof(float)),
                                        5126, frames, kind == 2 ? "VEC4" : "VEC3", NULL, 0);
                addf(&samplers, "%s{\"input\":%d,\"output\":%d,\"interpolation\":\"LINEAR\"}", sampler_count ? "," : "", time_acc, acc);
                addf(&channels, "%s{\"sampler\":%d,\"target\":{\"node\":%zu,\"path\":\"%s\"}}", sampler_count ? "," : "",
                     sampler_count, ni, kind == 0 ? "translation" : kind == 1 ? "scale" : "rotation");
                sampler_count++;
            }
            free(trs); free(rot);
        }
        free(times);
        if (!sampler_count) { free(channels.p); free(samplers.p); continue; }
        Str s = {0};
        addf(&s, "{\"name\":");
        esc(&s, stack->name.data);
        addf(&s, ",\"channels\":[%s],\"samplers\":[%s]}", channels.p, samplers.p);
        free(channels.p); free(samplers.p);
        push(&anims, &s);
    }

    // assemble the document
    Str json = {0};
    addf(&json, "{\"asset\":{\"version\":\"2.0\",\"generator\":\"KrunkNative fbx_gltf (ufbx)\"}");
    addf(&json, ",\"scene\":0,\"scenes\":[{\"nodes\":[");
    for (size_t ni = 0; ni < scene->nodes.count; ni++) {
        const ufbx_node *node = scene->nodes.data[ni];
        if (node->parent) continue;
        size_t count = 0;
        for (size_t k = 0; k < ni; k++) if (!scene->nodes.data[k]->parent) count++;
        addf(&json, "%s%zu", count ? "," : "", ni);
    }
    addf(&json, "]}]");
    addf(&json, ",\"nodes\":[%s]", nodes.p ? nodes.p : "");
    if (meshes.n) { addf(&json, ",\"meshes\":["); join(&json, &meshes); addf(&json, "]"); }
    if (mats.n) { addf(&json, ",\"materials\":["); join(&json, &mats); addf(&json, "]"); }
    if (images.n) { addf(&json, ",\"images\":["); join(&json, &images); addf(&json, "]"); }
    if (texs.n) { addf(&json, ",\"textures\":["); join(&json, &texs); addf(&json, "]"); }
    if (skins.n) { addf(&json, ",\"skins\":["); join(&json, &skins); addf(&json, "]"); }
    if (anims.n) { addf(&json, ",\"animations\":["); join(&json, &anims); addf(&json, "]"); }
    addf(&json, ",\"bufferViews\":["); join(&json, &views);
    addf(&json, "],\"accessors\":["); join(&json, &accs);
    addf(&json, "],\"buffers\":[{\"byteLength\":%zu}]}", bin.n);

    align4(&bin);
    const size_t json_len = (json.n + 3) & ~3u;
    const size_t total = 12 + 8 + json_len + 8 + bin.n;
    unsigned char *glb = malloc(total);
    unsigned char *w = glb;
    memcpy(w, "glTF", 4); w += 4;
    const unsigned int version = 2, length = (unsigned int) total;
    memcpy(w, &version, 4); w += 4;
    memcpy(w, &length, 4); w += 4;
    const unsigned int jl = (unsigned int) json_len, jt = 0x4E4F534A;
    memcpy(w, &jl, 4); w += 4;
    memcpy(w, &jt, 4); w += 4;
    memset(w, ' ', json_len);
    memcpy(w, json.p, json.n);
    w += json_len;
    const unsigned int bl = (unsigned int) bin.n, bt = 0x004E4942;
    memcpy(w, &bl, 4); w += 4;
    memcpy(w, &bt, 4); w += 4;
    memcpy(w, bin.p, bin.n);

    free(json.p); free(bin.p);
    free(mat_of); free(mesh_of); free(skin_of_mesh); free(skin_of); free(nodes.p);
    for (size_t i = 0; i < views.n; i++) free(views.v[i].p);
    for (size_t i = 0; i < accs.n; i++) free(accs.v[i].p);
    for (size_t i = 0; i < meshes.n; i++) free(meshes.v[i].p);
    for (size_t i = 0; i < mats.n; i++) free(mats.v[i].p);
    for (size_t i = 0; i < images.n; i++) free(images.v[i].p);
    for (size_t i = 0; i < texs.n; i++) free(texs.v[i].p);
    for (size_t i = 0; i < skins.n; i++) free(skins.v[i].p);
    for (size_t i = 0; i < anims.n; i++) free(anims.v[i].p);
    free(views.v); free(accs.v); free(meshes.v); free(mats.v); free(images.v); free(texs.v); free(skins.v); free(anims.v);
    ufbx_free_scene(scene);
    *out = glb;
    *out_size = total;
    return 1;
}
