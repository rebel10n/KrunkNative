// Map editor: File > Export GLTF / Export MTL (Krunker v7.1.4 Editor.exportScene /
// exportScene2, libs/GLTFExporter.js and libs/OBJMTLExporter.js), GL-free. editor_export.c.
#pragma once
#include <stddef.h>
#include "editor_model.h"

// One file of the MTL export besides the OBJ / MTL: `name` in the zip, read from `path`.
typedef struct {
    char name[128];
    char path[512];
} EdExportImage;

typedef struct {
    char *obj;               // <name>.obj (OBJMTLExporter.parse(scene, name).obj)
    char *mtl;               // <name>.mtl
    EdExportImage *images;   // the textures the MTL references (files that exist under assets_root)
    size_t image_count;
    int parts;               // meshes written
} EdExportObj;

// `objs`: the editor objects (editor_model.h form). `assets_root`: the directory holding
// models/ textures/ user-assets/ (with or without a trailing '/'); NULL or a missing directory
// exports the primitives only, without model meshes or texture images.
//
// The OBJ / MTL texts. 0 on allocation failure. ed_export_obj_free releases the members.
int ed_export_obj(const char *name, const EdModelObj *objs, size_t n, const char *assets_root, EdExportObj *out);
void ed_export_obj_free(EdExportObj *out);
// "<name>.zip" (stored, no compression): <name>.obj, <name>.mtl and the texture PNGs. malloc'd.
unsigned char *ed_export_mtl_zip(const char *name, const EdModelObj *objs, size_t n, const char *assets_root, size_t *len);
// The .gltf JSON (JSON.stringify(gltf, null, 2)), buffer and images embedded as data URIs. malloc'd.
char *ed_export_gltf(const char *name, const EdModelObj *objs, size_t n, const char *assets_root, size_t *len);

// store-only zip of `count` files (names and contents); malloc'd, *len its size
typedef struct {
    const char *name;
    const unsigned char *data;
    size_t size;
} EdZipEntry;
unsigned char *ed_zip_store(const EdZipEntry *entries, size_t count, size_t *len);
unsigned int ed_crc32(const unsigned char *data, size_t size);

// editor glue (editor_export.c, not in GL-free builds that define ED_EXPORT_NO_EDITOR):
// the toolbar's Export GLTF / Export MTL
void ed_file_export_gltf(void);
void ed_file_export_mtl(void);
