// Map editor collision meshes (Krunker v10.0.5): Convert to Collision Mesh (the object menu) and the custom asset's
// Collision Size / Fit Collision To Model (the Transform folder). The data side (cmt, collBase, the export bake and
// importOrphanTris) is GL-free in editor_model.c; this file needs the objects' triangles, which only the editor has.
//
// convertToCollisionMesh (editor-10.0.5.js:16793-16825): every model triangle of the object in world space, rounded to
// 2 decimals, replaces the object with one COLLISION_MESH (iïiìíïî, ed_model_collision_mesh_new). KrunkNative reads
// the triangles from what it can rebuild on the CPU (the GL meshes keep no vertex copy):
//   - the generated geometry of CUBE / CYLINDER / SPHERE / PLANE (ed_model_object_world_tris, as the cpx bake),
//   - a GLTF / GLB / FBX custom asset's model as the renderer loads it (gltf_model_triangles: meshopt decoded),
//     placed as loadAnimMesh places it: p, rotation r, scale mSize,
//   - the box of any other genGeo prefab (the zone / tool cubes the editor draws).
// Anything else (OBJ assets, gen model props: their geometry is on the GPU only) has "no model triangles to
// convert", as an object without a mesh has in Krunker.
#include "editor_internal.h"
#include "editor_model.h"
#include <gltf.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <user_asset.h>

static double round2(const double v) { return isfinite(v) ? floor(v * 100 + 0.5) / 100 : 0; } // Math.round(v * 100) / 100

// The model triangles of an asset object (model space, 9 floats each, malloc'd into *out): its GLTF / GLB / FBX model
// as the renderer has it (gltf_model_load: meshopt decoded, quantized positions read); 0 when it has none (an OBJ asset,
// a model not on disk). OBJ geometry lives on the GPU only.
static size_t asset_tris(const EditorObject *o, float **out) {
    *out = NULL;
    cJSON *aid = ed_prop_get(o, "assetID");
    char text[64] = "";
    if (cJSON_IsString(aid)) snprintf(text, sizeof(text), "%s", aid->valuestring);
    else if (cJSON_IsNumber(aid)) snprintf(text, sizeof(text), "%.0f", aid->valuedouble);
    cJSON_Delete(aid);
    UserAssetFlags flags;
    UserAssetModel model;
    if (!text[0] || !user_asset_parse(text, &flags) || !(flags.glb || flags.gltf || flags.fbx) || !user_asset_load(&flags, &model)) return 0;
    return gltf_model_triangles(model.gltf, out); // the cached model: not freed
}

// mSize as user_asset_object_mesh reads it: limitMM(round2(ms), 0.01, 100) || 1
static double asset_scale(const EditorObject *o) {
    const double ms = ed_json_num(o->data, "ms", 0);
    return ms != 0 && isfinite(ms) ? fmin(fmax(round2(ms), 0.01), 100) : 1;
}

// The object's triangles in world space (9 doubles each, malloc'd into *out); the count.
static int object_tris(const EditorObject *o, double **out) {
    *out = NULL;
    int n = ed_model_object_world_tris(o->data, o->prefab, out);
    if (n > 0) return n;
    if (o->prefab == ED_PREFAB_CUSTOM_ASSET) {
        // loadAnimMesh places the model at p, turned by r, scaled by mSize (user_asset_object_mesh)
        float *rest;
        const size_t count = asset_tris(o, &rest);
        double *w = count ? malloc(count * 9 * sizeof(double)) : NULL;
        if (w) {
            const vec3 p = ed_obj_vec(o, "p", (vec3) {0}), r = ed_obj_vec(o, "r", (vec3) {0});
            const double sc = asset_scale(o);
            float rm[9];
            ed_euler_matrix(r, rm);
            for (size_t i = 0; i < count * 3; i++) {
                const double x = rest[i * 3] * sc, y = rest[i * 3 + 1] * sc, z = rest[i * 3 + 2] * sc;
                w[i * 3] = rm[0] * x + rm[1] * y + rm[2] * z + p.x;
                w[i * 3 + 1] = rm[3] * x + rm[4] * y + rm[5] * z + p.y;
                w[i * 3 + 2] = rm[6] * x + rm[7] * y + rm[8] * z + p.z;
            }
            *out = w;
            n = (int) count;
        }
        free(rest);
        return n;
    }
    if (!editor_prefab_has(o->prefab, EP_GENGEO)) return 0;
    // the editor's box of a genGeo tool / zone: the unit cube through ed_obj_world_box, two triangles a face
    static const int faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    float m[16];
    ed_obj_world_box(o, m);
    double c[8][3];
    for (int i = 0; i < 8; i++) {
        const double x = i & 1 ? 0.5 : -0.5, y = i & 2 ? 0.5 : -0.5, z = i & 4 ? 0.5 : -0.5;
        for (int k = 0; k < 3; k++) c[i][k] = m[k * 4] * x + m[k * 4 + 1] * y + m[k * 4 + 2] * z + m[k * 4 + 3];
    }
    double *w = malloc(12 * 9 * sizeof(double));
    if (!w) return 0;
    int t = 0;
    for (int f = 0; f < 6; f++) {
        const int q[6] = {faces[f][0], faces[f][1], faces[f][2], faces[f][0], faces[f][2], faces[f][3]};
        for (int v = 0; v < 6; v++) for (int k = 0; k < 3; k++) w[t++] = c[q[v]][k];
    }
    *out = w;
    return 12;
}

void ed_convert_to_collision_mesh(EditorObject *o) {
    if (!o) o = g_ed.proj->selected;
    if (!o) {
        editor_message("You must select a object first"); // v10's alert text
        return;
    }
    if (!strcmp(editor_prefabs[o->prefab].name, "COLLISION_MESH")) return;
    double *tris;
    const int n = object_tris(o, &tris);
    if (n <= 0) {
        free(tris);
        editor_message("This object has no model triangles to convert");
        return;
    }
    for (int i = 0; i < n * 9; i++) tris[i] = round2(tris[i]); // iííiïïí per vertex
    int *starts = malloc((size_t) n * sizeof(int));
    cJSON *data = NULL;
    if (starts) {
        for (int i = 0; i < n; i++) starts[i] = i * 9;
        data = ed_model_collision_mesh_new(tris, starts, n);
    }
    free(starts);
    free(tris);
    EditorObject *mesh = data ? ed_obj_new(data, 0) : NULL;
    cJSON_Delete(data);
    if (!mesh) return;
    // this.removeObject(object); this.addObject(mesh): two history steps, as in Krunker
    ed_obj_remove(o, 0, 1);
    ed_obj_add(mesh, 0, 1);
}

int ed_asset_model_size(const EditorObject *o, double size[3]) {
    // defaultSize of an asset object: its model's bounds at mSize (the box setFromObject measures unrotated)
    float *rest = NULL;
    const size_t count = o && o->prefab == ED_PREFAB_CUSTOM_ASSET ? asset_tris(o, &rest) : 0;
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (size_t i = 0; i < count * 9; i++) {
        lo[i % 3] = fmin(lo[i % 3], rest[i]);
        hi[i % 3] = fmax(hi[i % 3], rest[i]);
    }
    free(rest);
    if (!count) return 0;
    const double sc = asset_scale(o);
    for (int k = 0; k < 3; k++) size[k] = round2((hi[k] - lo[k]) * sc);
    return 1;
}
