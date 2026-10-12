// GLTF 2.0 / GLB models on cgltf (client/include/gltf.h has the scope and the limits).
#include <gl.h>
#include <client.h>
#include <gltf.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stb_image.h>

#define CGLTF_IMPLEMENTATION
#include "../lib/cgltf/cgltf.h"

#define GLTF_MAX_JOINT_INFLUENCES 4

typedef struct {
    vec4 color;               // baseColorFactor
    int base_image;           // baseColorTexture image index, -1 = none
    int base_sampler;         // its sampler index, -1 = default
    vec4 emissive;            // emissiveFactor (w = 1 when non-black)
    int emissive_image;
    int emissive_sampler;
    float alpha_test;         // alphaMode MASK: alphaCutoff
    int blend;                // alphaMode BLEND
    int double_sided;
} GltfMaterial;

typedef struct {
    int node;                 // node owning the mesh
    int material;             // GltfMaterial index, -1 = default
    int skin;                 // cgltf skin index, -1 = none
    Geometry *geometry;       // static primitives: shared by every instance
    unsigned int ebo;         // dynamic primitives: shared index buffer
    unsigned int color_vbo;   // COLOR_0 (vec3, attribute 2, three.js vertexColors), 0 = none
    int index_count;
    // dynamic (morph targets or skin): the CPU data an instance re-evaluates
    int dynamic;
    vertex *base;             // positions + UVs
    size_t vertex_count;
    float *morph;             // morph_count * vertex_count * 3 position deltas
    int morph_count;
    unsigned short *joints;   // vertex_count * 4
    float *weights;           // vertex_count * 4
} GltfPrimitive;

struct GltfModel {
    char *path;
    cgltf_data *data;
    GltfPrimitive *primitives;
    size_t primitive_count;
    GltfMaterial *materials;
    size_t material_count;
    unsigned int *textures;   // per image: GL texture, 0 = not loaded / failed
    unsigned char *texture_tried;
    unsigned char *image_srgb; // per image: used as base colour / emissive map (sRGB)
    float **inverse_bind;     // per skin: joint_count row-major 4x4 matrices
    unsigned char *in_scene;  // per node: reachable from the default scene
    struct GltfModel *next;
};

struct GltfInstanceNode {
    MeshTransform transform;
    float t[3], r[4], s[3];   // current TRS
    float *weights;           // morph weights of the node's mesh (instance-owned)
    int weight_count;
    float local[16];          // row-major local and model-root matrices (skins)
    float root[16];
    int root_valid;
};

struct GltfDynamicPrimitive {
    const GltfPrimitive *primitive;
    Mesh *mesh;
    unsigned int vbo;
    vertex *scratch;
};

static GltfModel *g_gltf_models;
static double g_gltf_clock_override = -1.0;

double gltf_clock(void) {
    return g_gltf_clock_override >= 0.0 ? g_gltf_clock_override : glfwGetTime();
}

void gltf_set_clock_override(const double seconds) {
    g_gltf_clock_override = seconds;
}

// ---------------------------------------------------------------- math (row-major 4x4)

static void m4_identity(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void m4_mul(const float *a, const float *b, float *out) {
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(out, r, sizeof(r));
}

// three.js Euler.setFromRotationMatrix, order XYZ: mesh.c's ROTATION_ORDER_INTRINSIC is
// R = Rx * Ry * Rz, so a node's quaternion maps onto MeshTransform.rotation exactly.
static vec3 quat_to_euler_xyz(const float *q) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float m11 = 1 - 2 * (y * y + z * z), m12 = 2 * (x * y - w * z), m13 = 2 * (x * z + w * y);
    const float m22 = 1 - 2 * (x * x + z * z), m23 = 2 * (y * z - w * x);
    const float m32 = 2 * (y * z + w * x), m33 = 1 - 2 * (x * x + y * y);
    vec3 e;
    e.y = asinf(m13 < -1.0f ? -1.0f : m13 > 1.0f ? 1.0f : m13);
    if (fabsf(m13) < 0.9999999f) {
        e.x = atan2f(-m23, m33);
        e.z = atan2f(-m12, m11);
    } else {
        e.x = atan2f(m32, m22);
        e.z = 0.0f;
    }
    return e;
}

static void quat_normalize(float *q) {
    const float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l > 0.0f) for (int i = 0; i < 4; i++) q[i] /= l;
    else { q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f; }
}

static void quat_slerp(const float *a, const float *b, const float t, float *out) {
    float bb[4] = {b[0], b[1], b[2], b[3]};
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0.0f) { d = -d; for (int i = 0; i < 4; i++) bb[i] = -bb[i]; }
    float ka, kb;
    if (d > 0.9995f) {
        ka = 1.0f - t;
        kb = t;
    } else {
        const float theta = acosf(d), st = sinf(theta);
        ka = sinf((1.0f - t) * theta) / st;
        kb = sinf(t * theta) / st;
    }
    for (int i = 0; i < 4; i++) out[i] = ka * a[i] + kb * bb[i];
    quat_normalize(out);
}

// matrix-only node: T * R * S without shear
static void decompose(const float *cm, float *t, float *q, float *s) {
    // cgltf node matrices are column-major
    t[0] = cm[12]; t[1] = cm[13]; t[2] = cm[14];
    float c[3][3];
    for (int i = 0; i < 3; i++) {
        s[i] = sqrtf(cm[i * 4] * cm[i * 4] + cm[i * 4 + 1] * cm[i * 4 + 1] + cm[i * 4 + 2] * cm[i * 4 + 2]);
        for (int j = 0; j < 3; j++) c[i][j] = s[i] > 0.0f ? cm[i * 4 + j] / s[i] : 0.0f;
    }
    // c[i] is column i: rotation matrix element (row j, col i) = c[i][j]
    const float m00 = c[0][0], m11 = c[1][1], m22 = c[2][2];
    const float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        const float k = 0.5f / sqrtf(tr + 1.0f);
        q[3] = 0.25f / k;
        q[0] = (c[1][2] - c[2][1]) * k;
        q[1] = (c[2][0] - c[0][2]) * k;
        q[2] = (c[0][1] - c[1][0]) * k;
    } else if (m00 > m11 && m00 > m22) {
        const float k = 2.0f * sqrtf(1.0f + m00 - m11 - m22);
        q[3] = (c[1][2] - c[2][1]) / k;
        q[0] = 0.25f * k;
        q[1] = (c[1][0] + c[0][1]) / k;
        q[2] = (c[2][0] + c[0][2]) / k;
    } else if (m11 > m22) {
        const float k = 2.0f * sqrtf(1.0f + m11 - m00 - m22);
        q[3] = (c[2][0] - c[0][2]) / k;
        q[0] = (c[1][0] + c[0][1]) / k;
        q[1] = 0.25f * k;
        q[2] = (c[2][1] + c[1][2]) / k;
    } else {
        const float k = 2.0f * sqrtf(1.0f + m22 - m00 - m11);
        q[3] = (c[0][1] - c[1][0]) / k;
        q[0] = (c[2][0] + c[0][2]) / k;
        q[1] = (c[2][1] + c[1][2]) / k;
        q[2] = 0.25f * k;
    }
    quat_normalize(q);
}

// ---------------------------------------------------------------- files

// cgltf file reader: every open goes through ensure_asset_file (web: fetched into MEMFS) and
// the resource swapper, like load_texture / load_obj_model.
static cgltf_result gltf_read_file(const struct cgltf_memory_options *memory, const struct cgltf_file_options *file, const char *path, cgltf_size *size, void **data) {
    (void) memory;
    (void) file;
    ensure_asset_file(path);
    char swapped[1024];
    FILE *fp = fopen(file_redirect(path, swapped, sizeof(swapped)), "rb");
    if (!fp) return cgltf_result_file_not_found;
    fseek(fp, 0, SEEK_END);
    const long length = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (length < 0) {
        fclose(fp);
        return cgltf_result_io_error;
    }
    void *buffer = malloc(length ? (size_t) length : 1);
    if (!buffer) {
        fclose(fp);
        return cgltf_result_out_of_memory;
    }
    if (fread(buffer, 1, (size_t) length, fp) != (size_t) length) {
        free(buffer);
        fclose(fp);
        return cgltf_result_io_error;
    }
    fclose(fp);
    *size = (cgltf_size) length;
    *data = buffer;
    return cgltf_result_success;
}

static void gltf_release_file(const struct cgltf_memory_options *memory, const struct cgltf_file_options *file, void *data) {
    (void) memory;
    (void) file;
    free(data);
}

// ---------------------------------------------------------------- textures

static GLint gl_filter(const int filter, const GLint fallback) {
    switch (filter) {
        case 9728: case 9729: case 9984: case 9985: case 9986: case 9987: return filter; // NEAREST .. LINEAR_MIPMAP_LINEAR
        default: return fallback;
    }
}

static GLint gl_wrap(const int wrap) {
    return wrap == 33071 ? GL_CLAMP_TO_EDGE : wrap == 33648 ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

// sRGB -> linear, 8-bit: three.js GLTFLoader tags base colour and emissive maps sRGBEncoding
// and Krunker renders with outputEncoding Linear (render.js:406, encodingTypes[0]), so those
// texels reach the shader decoded to linear; BasicMaterial samples raw, so decode at upload.
static void srgb_to_linear(unsigned char *pixels, const size_t count) {
    static unsigned char table[256];
    static int ready;
    if (!ready) {
        for (int i = 0; i < 256; i++) {
            const double c = i / 255.0;
            const double l = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
            table[i] = (unsigned char) (l * 255.0 + 0.5);
        }
        ready = 1;
    }
    for (size_t i = 0; i < count; i++) {
        pixels[i * 4] = table[pixels[i * 4]];
        pixels[i * 4 + 1] = table[pixels[i * 4 + 1]];
        pixels[i * 4 + 2] = table[pixels[i * 4 + 2]];
    }
}

// Texture of image `index`: external URIs through load_texture (texture cache, flipped like
// every texture; no sRGB decode, like the OBJ items' textures); embedded ones (bufferView / data
// URI) decoded here, flipped the same way (gltf UVs are flipped at load, so both kinds sample
// alike). Sampler filters as three.js: default LINEAR_MIPMAP_LINEAR / LINEAR.
static unsigned int gltf_texture(GltfModel *model, const int index, const int sampler_index) {
    if (index < 0 || (size_t) index >= model->data->images_count) return 0;
    if (model->texture_tried[index]) return model->textures[index];
    model->texture_tried[index] = 1;

    const cgltf_image *image = &model->data->images[index];
    const char *mime = image->mime_type ? image->mime_type : "";
    if (strstr(mime, "webp") || (image->uri && strstr(image->uri, ".webp"))) {
        fprintf(stderr, "gltf: %s: image %d is WebP (EXT_texture_webp), not supported\n", model->path, index);
        return 0;
    }

    unsigned char *pixels = NULL;
    int width = 0, height = 0, channels = 0;
    unsigned int texture = 0;

    if (image->buffer_view) {
        const unsigned char *bytes = cgltf_buffer_view_data(image->buffer_view);
        if (bytes) {
            stbi_set_flip_vertically_on_load_thread(1);
            pixels = stbi_load_from_memory(bytes, (int) image->buffer_view->size, &width, &height, &channels, 4);
            stbi_set_flip_vertically_on_load_thread(0);
        }
    } else if (image->uri && !strncmp(image->uri, "data:", 5)) {
        const char *comma = strchr(image->uri, ',');
        if (comma && strstr(image->uri, ";base64")) {
            const size_t encoded = strlen(comma + 1);
            size_t size = encoded / 4 * 3;
            if (encoded >= 1 && comma[encoded] == '=') size--;
            if (encoded >= 2 && comma[encoded - 1] == '=') size--;
            void *bytes = NULL;
            cgltf_options options = {0};
            if (cgltf_load_buffer_base64(&options, size, comma + 1, &bytes) == cgltf_result_success) {
                stbi_set_flip_vertically_on_load_thread(1);
                pixels = stbi_load_from_memory(bytes, (int) size, &width, &height, &channels, 4);
                stbi_set_flip_vertically_on_load_thread(0);
                free(bytes);
            }
        }
    } else if (image->uri) {
        // relative to the model file
        char decoded[512];
        snprintf(decoded, sizeof(decoded), "%s", image->uri);
        cgltf_decode_uri(decoded);
        const char *slash = strrchr(model->path, '/');
        const size_t dir = slash ? (size_t) (slash - model->path + 1) : 0;
        char *full = malloc(dir + strlen(decoded) + 1);
        if (full) {
            memcpy(full, model->path, dir);
            strcpy(full + dir, decoded);
            texture = load_texture(full);
            free(full);
        }
        model->textures[index] = texture;
        return texture;
    }

    if (!pixels) {
        fprintf(stderr, "gltf: %s: cannot decode image %d: %s\n", model->path, index, stbi_failure_reason());
        return 0;
    }

    if (model->image_srgb[index]) srgb_to_linear(pixels, (size_t) width * (size_t) height);

    const cgltf_sampler *sampler = sampler_index >= 0 && (size_t) sampler_index < model->data->samplers_count ? &model->data->samplers[sampler_index] : NULL;
    glGenTextures(1, &texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    g_active_texture = texture;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, sampler ? gl_wrap(sampler->wrap_s) : GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, sampler ? gl_wrap(sampler->wrap_t) : GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter(sampler ? sampler->min_filter : 0, GL_LINEAR_MIPMAP_LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter(sampler ? sampler->mag_filter : 0, GL_LINEAR) == GL_NEAREST ? GL_NEAREST : GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
    stbi_image_free(pixels);

    model->textures[index] = texture;
    return texture;
}

// ---------------------------------------------------------------- loading

static int texture_view_image(const cgltf_texture_view *view, const cgltf_data *data, int *sampler) {
    *sampler = -1;
    if (!view->texture) return -1;
    const cgltf_texture *texture = view->texture;
    if (texture->sampler) *sampler = (int) cgltf_sampler_index(data, texture->sampler);
    if (texture->image) return (int) cgltf_image_index(data, texture->image);
    if (texture->has_webp && texture->webp_image) return (int) cgltf_image_index(data, texture->webp_image);
    return -1;
}

static void load_materials(GltfModel *model) {
    const cgltf_data *data = model->data;
    model->material_count = data->materials_count;
    model->materials = calloc(data->materials_count ? data->materials_count : 1, sizeof(GltfMaterial));
    for (size_t i = 0; i < data->materials_count; i++) {
        const cgltf_material *src = &data->materials[i];
        GltfMaterial *m = &model->materials[i];
        m->color = (vec4) {1.0f, 1.0f, 1.0f, 1.0f};
        m->base_image = m->emissive_image = -1;
        m->base_sampler = m->emissive_sampler = -1;
        if (src->has_pbr_metallic_roughness) {
            const float *f = src->pbr_metallic_roughness.base_color_factor;
            m->color = (vec4) {f[0], f[1], f[2], f[3]};
            m->base_image = texture_view_image(&src->pbr_metallic_roughness.base_color_texture, data, &m->base_sampler);
        } else if (src->has_pbr_specular_glossiness) {
            const float *f = src->pbr_specular_glossiness.diffuse_factor;
            m->color = (vec4) {f[0], f[1], f[2], f[3]};
            m->base_image = texture_view_image(&src->pbr_specular_glossiness.diffuse_texture, data, &m->base_sampler);
        }
        const float *e = src->emissive_factor;
        float strength = src->has_emissive_strength ? src->emissive_strength.emissive_strength : 1.0f;
        m->emissive_image = texture_view_image(&src->emissive_texture, data, &m->emissive_sampler);
        if (e[0] > 0.0f || e[1] > 0.0f || e[2] > 0.0f) m->emissive = (vec4) {e[0], e[1], e[2], strength};
        if (src->alpha_mode == cgltf_alpha_mode_mask) m->alpha_test = src->alpha_cutoff;
        m->blend = src->alpha_mode == cgltf_alpha_mode_blend;
        m->double_sided = src->double_sided;
        if (m->base_image >= 0) model->image_srgb[m->base_image] = 1;
        if (m->emissive_image >= 0) model->image_srgb[m->emissive_image] = 1;
    }
}

// KHR_texture_transform of the base colour texture, baked into the UVs: uv' = T * R * S * uv
static void apply_texture_transform(const cgltf_texture_transform *t, float *u, float *v) {
    const float su = *u * t->scale[0], sv = *v * t->scale[1];
    const float c = cosf(t->rotation), s = sinf(t->rotation);
    *u = c * su + s * sv + t->offset[0];
    *v = -s * su + c * sv + t->offset[1];
}

// attribute 2 = basic.vert v_color (BasicMaterial.use_vertex_color), in the bound VAO
static void bind_color_attribute(const unsigned int vbo) {
    if (!vbo) return;
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), NULL);
    glEnableVertexAttribArray(2);
}

static int load_primitive(GltfModel *model, const cgltf_node *node, const cgltf_primitive *prim, GltfPrimitive *out) {
    const cgltf_data *data = model->data;
    memset(out, 0, sizeof(*out));
    out->node = (int) cgltf_node_index(data, node);
    out->material = prim->material ? (int) cgltf_material_index(data, prim->material) : -1;
    out->skin = node->skin ? (int) cgltf_skin_index(data, node->skin) : -1;

    if (prim->type != cgltf_primitive_type_triangles) {
        fprintf(stderr, "gltf: %s: skipping a non-triangle primitive (mode %d)\n", model->path, (int) prim->type);
        return 0;
    }

    const cgltf_accessor *position = NULL, *texcoord = NULL, *joints = NULL, *weights = NULL, *color = NULL;
    for (size_t i = 0; i < prim->attributes_count; i++) {
        const cgltf_attribute *a = &prim->attributes[i];
        if (a->type == cgltf_attribute_type_position) position = a->data;
        else if (a->type == cgltf_attribute_type_texcoord && a->index == 0) texcoord = a->data;
        else if (a->type == cgltf_attribute_type_joints && a->index == 0) joints = a->data;
        else if (a->type == cgltf_attribute_type_weights && a->index == 0) weights = a->data;
        else if (a->type == cgltf_attribute_type_color && a->index == 0) color = a->data;
    }
    if (!position || !position->count) return 0;

    const size_t count = position->count;
    vertex *vertices = calloc(count, sizeof(vertex));
    if (!vertices) return 0;

    const cgltf_texture_transform *uv_transform = NULL;
    if (prim->material && prim->material->has_pbr_metallic_roughness && prim->material->pbr_metallic_roughness.base_color_texture.has_transform)
        uv_transform = &prim->material->pbr_metallic_roughness.base_color_texture.transform;

    for (size_t i = 0; i < count; i++) {
        float p[3] = {0}, uv[2] = {0};
        cgltf_accessor_read_float(position, i, p, 3);
        if (texcoord) cgltf_accessor_read_float(texcoord, i, uv, 2);
        if (uv_transform) apply_texture_transform(uv_transform, &uv[0], &uv[1]);
        vertices[i].position = (vec3) {p[0], p[1], p[2]};
        // GLTF UV origin is the image's top-left; textures are uploaded bottom row first
        vertices[i].tex_coord = (vec2) {uv[0], 1.0f - uv[1]};
    }

    size_t index_count = prim->indices ? prim->indices->count : count;
    index_count -= index_count % 3;
    unsigned int *indices = malloc((index_count ? index_count : 1) * sizeof(unsigned int));
    if (!indices) {
        free(vertices);
        return 0;
    }
    for (size_t i = 0; i < index_count; i++) {
        const size_t idx = prim->indices ? cgltf_accessor_read_index(prim->indices, i) : i;
        indices[i] = idx < count ? (unsigned int) idx : 0;
    }

    // morph targets (positions only)
    int morph_count = 0;
    for (size_t t = 0; t < prim->targets_count; t++)
        for (size_t a = 0; a < prim->targets[t].attributes_count; a++)
            if (prim->targets[t].attributes[a].type == cgltf_attribute_type_position) morph_count = (int) t + 1;
    if (morph_count) {
        out->morph = calloc((size_t) morph_count * count * 3, sizeof(float));
        out->morph_count = morph_count;
        for (int t = 0; out->morph && t < morph_count; t++) {
            for (size_t a = 0; a < prim->targets[t].attributes_count; a++) {
                const cgltf_attribute *attr = &prim->targets[t].attributes[a];
                if (attr->type != cgltf_attribute_type_position) continue;
                for (size_t i = 0; i < count && i < attr->data->count; i++)
                    cgltf_accessor_read_float(attr->data, i, &out->morph[((size_t) t * count + i) * 3], 3);
            }
        }
    }

    if (out->skin >= 0 && joints && weights) {
        out->joints = calloc(count * 4, sizeof(unsigned short));
        out->weights = calloc(count * 4, sizeof(float));
        for (size_t i = 0; out->joints && out->weights && i < count; i++) {
            cgltf_uint j[4] = {0};
            cgltf_accessor_read_uint(joints, i, j, 4);
            cgltf_accessor_read_float(weights, i, &out->weights[i * 4], 4);
            for (int k = 0; k < 4; k++) out->joints[i * 4 + k] = (unsigned short) j[k];
        }
    } else {
        out->skin = -1;
    }

    out->dynamic = out->morph_count > 0 || out->skin >= 0;
    out->index_count = (int) index_count;

    if (color) {
        float *colors = malloc(count * 3 * sizeof(float));
        if (colors) {
            for (size_t i = 0; i < count; i++) {
                float c[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                if (i >= color->count || !cgltf_accessor_read_float(color, i, c, 4)) c[0] = c[1] = c[2] = 1.0f;
                memcpy(&colors[i * 3], c, 3 * sizeof(float));
            }
            glGenBuffers(1, &out->color_vbo);
            glBindBuffer(GL_ARRAY_BUFFER, out->color_vbo);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (count * 3 * sizeof(float)), colors, GL_STATIC_DRAW);
            free(colors);
        }
    }

    if (out->dynamic) {
        out->base = vertices;
        out->vertex_count = count;
        glGenBuffers(1, &out->ebo);
        glBindVertexArray(0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, out->ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr) (index_count * sizeof(unsigned int)), indices, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    } else {
        Geometry *geometry = calloc(1, sizeof(Geometry));
        if (!geometry) {
            free(vertices);
            free(indices);
            return 0;
        }
        unsigned int vbo;
        glGenVertexArrays(1, &geometry->vao);
        glGenBuffers(1, &geometry->ebo);
        glGenBuffers(1, &vbo);
        glBindVertexArray(geometry->vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (count * sizeof(vertex)), vertices, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr) (index_count * sizeof(unsigned int)), indices, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), NULL);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), (void *) (3 * sizeof(float)));
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        bind_color_attribute(out->color_vbo);
        glBindVertexArray(0);
        geometry->index_count = (int) index_count;
        geometry_set_bounds(geometry, vertices, count);
        out->geometry = geometry;
        free(vertices);
    }
    free(indices);
    return 1;
}

static int node_in_scene(const cgltf_data *data, const cgltf_node *node) {
    const cgltf_scene *scene = data->scene ? data->scene : data->scenes_count ? &data->scenes[0] : NULL;
    while (node->parent) node = node->parent;
    if (!scene) return 1;
    for (size_t i = 0; i < scene->nodes_count; i++)
        if (scene->nodes[i] == node) return 1;
    return 0;
}

static int extension_supported(const char *name) {
    static const char *const supported[] = {"KHR_mesh_quantization", "KHR_texture_transform", "KHR_materials_emissive_strength",
                                            "KHR_materials_pbrSpecularGlossiness", "KHR_materials_unlit"};
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++)
        if (!strcmp(name, supported[i])) return 1;
    return 0;
}

GltfModel *gltf_model_load(const char *path) {
    if (!path) return NULL;
    for (GltfModel *m = g_gltf_models; m; m = m->next)
        if (!strcmp(m->path, path)) return m->data ? m : NULL;

    GltfModel *model = calloc(1, sizeof(GltfModel));
    if (!model) return NULL;
    const size_t path_length = strlen(path);
    model->path = malloc(path_length + 1);
    if (!model->path) {
        free(model);
        return NULL;
    }
    memcpy(model->path, path, path_length + 1);
    model->next = g_gltf_models;
    g_gltf_models = model; // failures are cached too (data NULL): not retried every frame

    cgltf_options options = {0};
    options.file.read = gltf_read_file;
    options.file.release = gltf_release_file;
    cgltf_data *data = NULL;
    cgltf_result result = cgltf_parse_file(&options, path, &data);
    if (result != cgltf_result_success) {
        fprintf(stderr, "gltf: cannot parse %s (cgltf result %d)\n", path, (int) result);
        return NULL;
    }
    for (size_t i = 0; i < data->extensions_required_count; i++) {
        if (!extension_supported(data->extensions_required[i])) {
            fprintf(stderr, "gltf: %s requires %s, not supported\n", path, data->extensions_required[i]);
            cgltf_free(data);
            return NULL;
        }
    }
    // Six GLTF remodels of the v7.1.4 reference (weapon_2_12, 4_9, 15_5, 16_3, 17_5, melee_63) carry
    // corrupt base64 buffers (length % 4 == 1, the stream shifted mid-way). v7's GLTFLoader rejects
    // them ("Failed to load buffer") and draws nothing; cgltf would decode garbage. Refuse them, so
    // the callers fall back (plain weapon / default knife) instead of drawing broken geometry.
    for (size_t i = 0; i < data->buffers_count; i++) {
        const char *uri = data->buffers[i].uri;
        const char *comma = uri && !strncmp(uri, "data:", 5) ? strchr(uri, ',') : NULL;
        if (comma && strstr(uri, ";base64") && strlen(comma + 1) % 4 == 1) {
            fprintf(stderr, "gltf: %s: buffer %zu is not valid base64 (corrupt file)\n", path, i);
            cgltf_free(data);
            return NULL;
        }
    }
    for (size_t i = 0; i < data->buffer_views_count; i++) {
        if (data->buffer_views[i].has_meshopt_compression) {
            fprintf(stderr, "gltf: %s uses EXT_meshopt_compression, not supported\n", path);
            cgltf_free(data);
            return NULL;
        }
    }
    result = cgltf_load_buffers(&options, data, path);
    if (result != cgltf_result_success) {
        fprintf(stderr, "gltf: cannot load the buffers of %s (cgltf result %d)\n", path, (int) result);
        cgltf_free(data);
        return NULL;
    }
    model->data = data;

    model->textures = calloc(data->images_count ? data->images_count : 1, sizeof(unsigned int));
    model->texture_tried = calloc(data->images_count ? data->images_count : 1, 1);
    model->image_srgb = calloc(data->images_count ? data->images_count : 1, 1);
    model->in_scene = calloc(data->nodes_count ? data->nodes_count : 1, 1);
    model->inverse_bind = calloc(data->skins_count ? data->skins_count : 1, sizeof(float *));
    load_materials(model);

    for (size_t s = 0; s < data->skins_count; s++) {
        const cgltf_skin *skin = &data->skins[s];
        float *m = calloc(skin->joints_count ? skin->joints_count * 16 : 1, sizeof(float));
        model->inverse_bind[s] = m;
        for (size_t j = 0; m && j < skin->joints_count; j++) {
            float cm[16];
            if (!skin->inverse_bind_matrices || !cgltf_accessor_read_float(skin->inverse_bind_matrices, j, cm, 16)) {
                m4_identity(&m[j * 16]);
                continue;
            }
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++) m[j * 16 + r * 4 + c] = cm[c * 4 + r];
        }
    }

    size_t capacity = 0;
    for (size_t n = 0; n < data->nodes_count; n++) {
        model->in_scene[n] = (unsigned char) node_in_scene(data, &data->nodes[n]);
        if (data->nodes[n].mesh && model->in_scene[n]) capacity += data->nodes[n].mesh->primitives_count;
    }
    model->primitives = calloc(capacity ? capacity : 1, sizeof(GltfPrimitive));
    for (size_t n = 0; model->primitives && n < data->nodes_count; n++) {
        const cgltf_node *node = &data->nodes[n];
        if (!node->mesh || !model->in_scene[n]) continue;
        for (size_t p = 0; p < node->mesh->primitives_count; p++)
            if (load_primitive(model, node, &node->mesh->primitives[p], &model->primitives[model->primitive_count]))
                model->primitive_count++;
    }
    glBindVertexArray(0);
    return model;
}

int gltf_model_animated(const GltfModel *model) {
    return model && model->data && model->data->animations_count > 0;
}

// ---------------------------------------------------------------- instances

static void node_rest_pose(GltfInstanceNode *node, const cgltf_node *src) {
    node->t[0] = node->t[1] = node->t[2] = 0.0f;
    node->r[0] = node->r[1] = node->r[2] = 0.0f;
    node->r[3] = 1.0f;
    node->s[0] = node->s[1] = node->s[2] = 1.0f;
    if (src->has_matrix) {
        decompose(src->matrix, node->t, node->r, node->s);
        return;
    }
    if (src->has_translation) memcpy(node->t, src->translation, sizeof(node->t));
    if (src->has_rotation) memcpy(node->r, src->rotation, sizeof(node->r));
    if (src->has_scale) memcpy(node->s, src->scale, sizeof(node->s));
}

static void node_apply(GltfInstanceNode *node) {
    node->transform.position = (vec3) {node->t[0], node->t[1], node->t[2]};
    node->transform.rotation = quat_to_euler_xyz(node->r);
    node->transform.scale = (vec3) {node->s[0], node->s[1], node->s[2]};
    node->root_valid = 0;
}

// The node's local matrix from its MeshTransform (T * Rx * Ry * Rz * S, mesh.c's intrinsic order),
// so a bone moved through gltf_instance_bone() deforms the skin like a sampled one.
static void node_local_from_transform(GltfInstanceNode *node) {
    const MeshTransform *tr = &node->transform;
    const float cx = cosf(tr->rotation.x), sx = sinf(tr->rotation.x);
    const float cy = cosf(tr->rotation.y), sy = sinf(tr->rotation.y);
    const float cz = cosf(tr->rotation.z), sz = sinf(tr->rotation.z);
    // R = Rx * Ry * Rz (three.js Euler XYZ)
    const float r[9] = {
        cy * cz, -cy * sz, sy,
        cx * sz + sx * sy * cz, cx * cz - sx * sy * sz, -sx * cy,
        sx * sz - cx * sy * cz, sx * cz + cx * sy * sz, cx * cy,
    };
    const float sc[3] = {tr->scale.x, tr->scale.y, tr->scale.z};
    float *m = node->local;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) m[i * 4 + j] = r[i * 3 + j] * sc[j];
    }
    m[3] = tr->position.x;
    m[7] = tr->position.y;
    m[11] = tr->position.z;
    m[12] = m[13] = m[14] = 0.0f;
    m[15] = 1.0f;
    node->root_valid = 0;
}

static const float *node_root_matrix(GltfInstance *instance, const int index) {
    GltfInstanceNode *node = &instance->nodes[index];
    if (node->root_valid) return node->root;
    const cgltf_node *src = &instance->model->data->nodes[index];
    if (src->parent) {
        const float *parent = node_root_matrix(instance, (int) cgltf_node_index(instance->model->data, src->parent));
        m4_mul(parent, node->local, node->root);
    } else {
        memcpy(node->root, node->local, sizeof(node->root));
    }
    node->root_valid = 1;
    return node->root;
}

static Mesh *instance_mesh(const GltfModel *model, const GltfPrimitive *prim, Geometry *geometry) {
    BasicMaterial *material = basic_material_init();
    if (!material) return NULL;
    material->double_sided = 0;
    if (prim->material >= 0 && (size_t) prim->material < model->material_count) {
        const GltfMaterial *m = &model->materials[prim->material];
        material->color = m->color;
        material->texture = gltf_texture((GltfModel *) model, m->base_image, m->base_sampler);
        material->emissive = m->emissive;
        if (m->emissive_image >= 0) {
            material->emissive_texture = gltf_texture((GltfModel *) model, m->emissive_image, m->emissive_sampler);
            if (material->emissive.w == 0.0f) material->emissive = (vec4) {1.0f, 1.0f, 1.0f, 1.0f};
        }
        material->alpha_test = m->alpha_test;
        material->base.transparent = m->blend;
        // render.js loadAnimMesh: depthWrite = !transparent for gltf/glb
        material->no_depth_write = m->blend;
        material->double_sided = m->double_sided;
    }
    material->use_vertex_color = prim->color_vbo != 0; // three.js GLTFLoader: vertexColors with COLOR_0
    Mesh *mesh = mesh_init(geometry, (Material *) material);
    if (!mesh) material_fini((Material *) material);
    return mesh;
}

static void dynamic_upload(GltfInstance *instance, GltfDynamicPrimitive *d) {
    const GltfPrimitive *prim = d->primitive;
    const GltfInstanceNode *node = &instance->nodes[prim->node];
    const size_t count = prim->vertex_count;
    memcpy(d->scratch, prim->base, count * sizeof(vertex));

    if (prim->morph_count && node->weights) {
        for (int t = 0; t < prim->morph_count && t < node->weight_count; t++) {
            const float w = node->weights[t];
            if (w == 0.0f) continue;
            const float *delta = &prim->morph[(size_t) t * count * 3];
            for (size_t i = 0; i < count; i++) {
                d->scratch[i].position.x += w * delta[i * 3];
                d->scratch[i].position.y += w * delta[i * 3 + 1];
                d->scratch[i].position.z += w * delta[i * 3 + 2];
            }
        }
    }

    if (prim->skin >= 0) {
        for (size_t n = 0; n < instance->model->data->nodes_count; n++) node_local_from_transform(&instance->nodes[n]);
        const cgltf_skin *skin = &instance->model->data->skins[prim->skin];
        const float *inverse_bind = instance->model->inverse_bind[prim->skin];
        float *joint = malloc(skin->joints_count * 16 * sizeof(float));
        if (joint && inverse_bind) {
            for (size_t j = 0; j < skin->joints_count; j++)
                m4_mul(node_root_matrix(instance, (int) cgltf_node_index(instance->model->data, skin->joints[j])), &inverse_bind[j * 16], &joint[j * 16]);
            for (size_t i = 0; i < count; i++) {
                const vec3 p = d->scratch[i].position;
                float out[3] = {0}, total = 0.0f;
                for (int k = 0; k < GLTF_MAX_JOINT_INFLUENCES; k++) {
                    const float w = prim->weights[i * 4 + k];
                    const unsigned short j = prim->joints[i * 4 + k];
                    if (w == 0.0f || j >= skin->joints_count) continue;
                    const float *m = &joint[j * 16];
                    out[0] += w * (m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3]);
                    out[1] += w * (m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7]);
                    out[2] += w * (m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]);
                    total += w;
                }
                if (total > 0.0f) d->scratch[i].position = (vec3) {out[0] / total, out[1] / total, out[2] / total};
            }
        }
        free(joint);
    }

    glBindBuffer(GL_ARRAY_BUFFER, d->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr) (count * sizeof(vertex)), d->scratch);
}

GltfInstance *gltf_instance_create(GltfModel *model, const float time_scale) {
    if (!model || !model->data) return NULL;
    const cgltf_data *data = model->data;
    GltfInstance *instance = calloc(1, sizeof(GltfInstance));
    if (!instance) return NULL;
    instance->model = model;
    instance->root.scale = (vec3) {1.0f, 1.0f, 1.0f};
    instance->time_scale = time_scale > 0.0f ? time_scale : 1.0f;
    instance->last_now = -1.0;
    instance->nodes = calloc(data->nodes_count ? data->nodes_count : 1, sizeof(GltfInstanceNode));
    instance->meshes = calloc(model->primitive_count ? model->primitive_count : 1, sizeof(Mesh *));
    instance->dynamic = calloc(model->primitive_count ? model->primitive_count : 1, sizeof(GltfDynamicPrimitive));
    if (!instance->nodes || !instance->meshes || !instance->dynamic) {
        gltf_instance_fini(instance);
        return NULL;
    }

    for (size_t n = 0; n < data->nodes_count; n++) {
        GltfInstanceNode *node = &instance->nodes[n];
        const cgltf_node *src = &data->nodes[n];
        node->transform.rotation_order = ROTATION_ORDER_INTRINSIC;
        node->transform.parent = src->parent ? &instance->nodes[cgltf_node_index(data, src->parent)].transform : &instance->root;
        node_rest_pose(node, src);
        node_apply(node);
        if (src->mesh) {
            size_t targets = 0;
            for (size_t p = 0; p < src->mesh->primitives_count; p++)
                if (src->mesh->primitives[p].targets_count > targets) targets = src->mesh->primitives[p].targets_count;
            if (targets) {
                node->weights = calloc(targets, sizeof(float));
                node->weight_count = (int) targets;
                const float *w = src->weights_count ? src->weights : src->mesh->weights;
                const size_t wc = src->weights_count ? src->weights_count : src->mesh->weights_count;
                for (size_t i = 0; node->weights && w && i < wc && i < targets; i++) node->weights[i] = w[i];
            }
        }
    }

    for (size_t p = 0; p < model->primitive_count; p++) {
        const GltfPrimitive *prim = &model->primitives[p];
        Mesh *mesh;
        if (!prim->dynamic) {
            mesh = instance_mesh(model, prim, prim->geometry);
            if (!mesh) continue;
            mesh->transform.parent = &instance->nodes[prim->node].transform;
        } else {
            Geometry *geometry = calloc(1, sizeof(Geometry));
            GltfDynamicPrimitive *d = &instance->dynamic[instance->dynamic_count];
            d->scratch = malloc(prim->vertex_count * sizeof(vertex));
            if (!geometry || !d->scratch) {
                free(geometry);
                free(d->scratch);
                d->scratch = NULL;
                continue;
            }
            glGenVertexArrays(1, &geometry->vao);
            glGenBuffers(1, &d->vbo);
            glBindVertexArray(geometry->vao);
            glBindBuffer(GL_ARRAY_BUFFER, d->vbo);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (prim->vertex_count * sizeof(vertex)), prim->base, GL_DYNAMIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, prim->ebo);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), NULL);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), (void *) (3 * sizeof(float)));
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            bind_color_attribute(prim->color_vbo);
            glBindVertexArray(0);
            geometry->ebo = prim->ebo;
            geometry->index_count = prim->index_count;
            geometry->bound_radius = 0.0f; // deforms: never frustum-culled
            mesh = instance_mesh(model, prim, geometry);
            if (!mesh) {
                glDeleteVertexArrays(1, &geometry->vao);
                glDeleteBuffers(1, &d->vbo);
                free(geometry);
                free(d->scratch);
                d->scratch = NULL;
                continue;
            }
            // skinned vertices are in model-root space (the mesh node's own transform is
            // ignored, as the GLTF spec says); morph-only ones stay under their node
            mesh->transform.parent = prim->skin >= 0 ? &instance->root : &instance->nodes[prim->node].transform;
            d->primitive = prim;
            d->mesh = mesh;
            instance->dynamic_count++;
            dynamic_upload(instance, d);
        }
        instance->meshes[instance->mesh_count++] = mesh;
    }
    return instance;
}

// ---------------------------------------------------------------- animation

static size_t keyframe(const cgltf_accessor *input, const float t, float *u) {
    const size_t n = input->count;
    float t0 = 0.0f, t1 = 0.0f;
    cgltf_accessor_read_float(input, 0, &t0, 1);
    if (n < 2 || t <= t0) {
        *u = 0.0f;
        return 0;
    }
    cgltf_accessor_read_float(input, n - 1, &t1, 1);
    if (t >= t1) {
        *u = 1.0f;
        return n - 2;
    }
    size_t lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        float tm;
        cgltf_accessor_read_float(input, mid, &tm, 1);
        if (tm <= t) lo = mid;
        else hi = mid;
    }
    cgltf_accessor_read_float(input, lo, &t0, 1);
    cgltf_accessor_read_float(input, lo + 1, &t1, 1);
    *u = t1 > t0 ? (t - t0) / (t1 - t0) : 0.0f;
    return lo;
}

// Value `value` (width floats) of an output accessor: one VEC3/VEC4 element, or `width`
// consecutive scalars (morph weights: SCALAR accessor, one weight per target).
static void read_value(const cgltf_accessor *accessor, const size_t value, const int width, float *out) {
    if ((int) cgltf_num_components(accessor->type) == width) {
        if (!cgltf_accessor_read_float(accessor, value, out, (cgltf_size) width)) memset(out, 0, (size_t) width * sizeof(float));
        return;
    }
    for (int i = 0; i < width; i++)
        if (!cgltf_accessor_read_float(accessor, value * (size_t) width + (size_t) i, &out[i], 1)) out[i] = 0.0f;
}

// Sample a channel at time t into out[width] (rotation: quaternion, width 4).
static void sample(const cgltf_animation_sampler *sampler, const float t, const int width, const int is_rotation, float *out) {
    float u;
    const size_t k = keyframe(sampler->input, t, &u);
    const size_t keys = sampler->input->count;
    float a[64], b[64];
    if (width > 64 || !keys) return;

    if (sampler->interpolation == cgltf_interpolation_type_cubic_spline) {
        // output: (in-tangent, value, out-tangent) per key
        float in1[64], p0[64], m0[64], p1[64], t0 = 0.0f, t1 = 0.0f;
        read_value(sampler->output, k * 3 + 1, width, p0);
        if (keys < 2) {
            memcpy(out, p0, (size_t) width * sizeof(float));
            if (is_rotation) quat_normalize(out);
            return;
        }
        read_value(sampler->output, k * 3 + 2, width, m0);
        read_value(sampler->output, (k + 1) * 3, width, in1);
        read_value(sampler->output, (k + 1) * 3 + 1, width, p1);
        cgltf_accessor_read_float(sampler->input, k, &t0, 1);
        cgltf_accessor_read_float(sampler->input, k + 1, &t1, 1);
        const float dt = t1 - t0;
        const float u2 = u * u, u3 = u2 * u;
        for (int i = 0; i < width; i++)
            out[i] = (2 * u3 - 3 * u2 + 1) * p0[i] + (u3 - 2 * u2 + u) * dt * m0[i] + (-2 * u3 + 3 * u2) * p1[i] + (u3 - u2) * dt * in1[i];
        if (is_rotation) quat_normalize(out);
        return;
    }

    read_value(sampler->output, k, width, a);
    if (keys > 1) read_value(sampler->output, k + 1, width, b);
    else memcpy(b, a, (size_t) width * sizeof(float));
    if (sampler->interpolation == cgltf_interpolation_type_step) u = u >= 1.0f ? 1.0f : 0.0f;
    if (is_rotation) {
        quat_slerp(a, b, u, out);
        return;
    }
    for (int i = 0; i < width; i++) out[i] = a[i] + (b[i] - a[i]) * u;
}

static float clip_duration(const cgltf_animation *anim) {
    float duration = 0.0f;
    for (size_t s = 0; s < anim->samplers_count; s++) {
        const cgltf_accessor *input = anim->samplers[s].input;
        if (input->has_max && input->max[0] > duration) duration = input->max[0];
        else if (!input->has_max && input->count) {
            float last;
            cgltf_accessor_read_float(input, input->count - 1, &last, 1);
            if (last > duration) duration = last;
        }
    }
    return duration;
}

void gltf_instance_update(GltfInstance *instance, const double now) {
    if (!instance || !instance->model || !instance->model->data) return;
    const cgltf_data *data = instance->model->data;
    if (!data->animations_count) {
        // no clips: only script bone writes can move a skin
        for (size_t i = 0; i < instance->dynamic_count; i++)
            if (instance->dynamic[i].primitive->skin >= 0) dynamic_upload(instance, &instance->dynamic[i]);
        return;
    }

    const double dt = instance->last_now < 0.0 ? 0.0 : now - instance->last_now;
    instance->last_now = now;
    if (dt > 0.0) instance->time += dt * instance->time_scale;

    if (instance->clip_mode == GLTF_CLIPS_STOPPED) {
        // stopAllClips(): no clip drives the nodes; the pose (and script bone writes) stays
        for (size_t i = 0; i < instance->dynamic_count; i++)
            if (instance->dynamic[i].primitive->skin >= 0) dynamic_upload(instance, &instance->dynamic[i]);
        return;
    }

    // three.js playAllClips() (the default): every clip at once, LoopRepeat; playClip(name, reps):
    // that clip alone, looping (reps 0) or `reps` times then clamped on its last frame
    for (size_t a = 0; a < data->animations_count; a++) {
        if (instance->clip_mode != GLTF_CLIPS_ALL && (int) a != instance->clip) continue;
        const cgltf_animation *anim = &data->animations[a];
        const float duration = clip_duration(anim);
        float t;
        if (instance->clip_mode == GLTF_CLIPS_ALL) {
            t = duration > 0.0f ? (float) fmod(instance->time, (double) duration) : 0.0f;
        } else {
            const double elapsed = instance->time - instance->clip_start;
            if (instance->clip_mode == GLTF_CLIPS_ONCE && elapsed >= (double) duration * instance->clip_reps) t = duration;
            else t = duration > 0.0f ? (float) fmod(elapsed < 0.0 ? 0.0 : elapsed, (double) duration) : 0.0f;
        }

        for (size_t c = 0; c < anim->channels_count; c++) {
            const cgltf_animation_channel *channel = &anim->channels[c];
            if (!channel->target_node || !channel->sampler) continue;
            GltfInstanceNode *node = &instance->nodes[cgltf_node_index(data, channel->target_node)];
            switch (channel->target_path) {
                case cgltf_animation_path_type_translation: sample(channel->sampler, t, 3, 0, node->t); break;
                case cgltf_animation_path_type_rotation: sample(channel->sampler, t, 4, 1, node->r); break;
                case cgltf_animation_path_type_scale: sample(channel->sampler, t, 3, 0, node->s); break;
                case cgltf_animation_path_type_weights:
                    if (node->weights && node->weight_count <= 64) sample(channel->sampler, t, node->weight_count, 0, node->weights);
                    break;
                default: break;
            }
        }
    }

    for (size_t n = 0; n < data->nodes_count; n++) node_apply(&instance->nodes[n]);
    for (size_t i = 0; i < instance->dynamic_count; i++) dynamic_upload(instance, &instance->dynamic[i]);
}

// ---------------------------------------------------------------- script API (KrunkScript GAME.SCENE)

MeshTransform *gltf_instance_bone(GltfInstance *instance, const char *name) {
    if (!instance || !name || !instance->model || !instance->model->data) return NULL;
    const cgltf_data *data = instance->model->data;
    for (size_t s = 0; s < data->skins_count; s++) {
        for (size_t j = 0; j < data->skins[s].joints_count; j++) {
            const cgltf_node *joint = data->skins[s].joints[j];
            if (joint && joint->name && !strcmp(joint->name, name)) return &instance->nodes[cgltf_node_index(data, joint)].transform;
        }
    }
    return NULL;
}

int gltf_instance_clip_running(const GltfInstance *instance) {
    if (!instance || instance->clip_mode != GLTF_CLIPS_ONCE || !instance->model || !instance->model->data) return 0;
    if (instance->clip < 0 || (size_t) instance->clip >= instance->model->data->animations_count) return 0;
    const float duration = clip_duration(&instance->model->data->animations[instance->clip]);
    return instance->time - instance->clip_start < (double) duration * instance->clip_reps;
}

void gltf_instance_play_clip(GltfInstance *instance, const char *clip, const int reps) {
    if (!instance || !clip || !instance->model || !instance->model->data) return;
    const cgltf_data *data = instance->model->data;
    int index = -1;
    for (size_t a = 0; a < data->animations_count && index < 0; a++)
        if (data->animations[a].name && !strcmp(data->animations[a].name, clip)) index = (int) a;
    if (index < 0) return; // this.anims[name] undefined: nothing happens
    if (reps > 0) {
        instance->clip_mode = GLTF_CLIPS_ONCE;
        instance->clip_reps = reps;
    } else {
        // already the looping main clip: keep playing it from where it is
        if (instance->clip_mode == GLTF_CLIPS_LOOP && instance->clip == index) return;
        // render.js playClip(name, 0): `!currAnim || !isFinite(currAnim.repetitions) ||
        // !currAnim.isRunning()`, so a loop never cuts a one-shot that is still playing
        if (gltf_instance_clip_running(instance)) return;
        instance->clip_mode = GLTF_CLIPS_LOOP;
        instance->clip_reps = 0;
    }
    instance->clip = index;
    instance->clip_start = instance->time;
}

void gltf_instance_stop_clips(GltfInstance *instance) {
    if (instance) instance->clip_mode = GLTF_CLIPS_STOPPED;
}

void gltf_instance_set_morph(GltfInstance *instance, const char *target_name, float weight) {
    if (!instance || !target_name || !instance->model || !instance->model->data) return;
    weight = weight < 0.0f ? 0.0f : weight > 1.0f ? 1.0f : weight; // UTILS.limitMM(value, 0, 1)
    const cgltf_data *data = instance->model->data;
    int changed = 0;
    for (size_t n = 0; n < data->nodes_count; n++) {
        const cgltf_mesh *mesh = data->nodes[n].mesh;
        GltfInstanceNode *node = &instance->nodes[n];
        if (!mesh || !node->weights) continue;
        for (size_t t = 0; t < mesh->target_names_count && (int) t < node->weight_count; t++) {
            if (mesh->target_names[t] && !strcmp(mesh->target_names[t], target_name)) {
                node->weights[t] = weight;
                changed = 1;
            }
        }
    }
    if (!changed) return;
    for (size_t i = 0; i < instance->dynamic_count; i++)
        if (instance->dynamic[i].primitive->morph_count) dynamic_upload(instance, &instance->dynamic[i]);
}

void gltf_instance_set_time_scale(GltfInstance *instance, const float time_scale) {
    if (instance) instance->time_scale = time_scale;
}

void gltf_instance_set_visible(GltfInstance *instance, const int visible) {
    if (!instance) return;
    for (size_t i = 0; i < instance->mesh_count; i++) instance->meshes[i]->visible = visible;
}

Geometry *gltf_empty_geometry(void) {
    static Geometry geometry;
    if (!geometry.vao) {
        static const unsigned int indices[3] = {0, 0, 0};
        glGenVertexArrays(1, &geometry.vao);
        glGenBuffers(1, &geometry.ebo);
        glBindVertexArray(geometry.vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
        glBindVertexArray(0);
        geometry.index_count = 0; // bound_radius 0: never culled, and nothing to draw
    }
    return &geometry;
}

void scene_add_gltf(Scene *scene, const GltfInstance *instance) {
    if (!instance) return;
    for (size_t i = 0; i < instance->mesh_count; i++) scene_add_mesh(scene, instance->meshes[i]);
}

void scene_remove_gltf(Scene *scene, const GltfInstance *instance) {
    if (!instance) return;
    for (size_t i = 0; i < instance->mesh_count; i++) scene_remove_mesh(scene, instance->meshes[i]);
}

void gltf_instance_fini(GltfInstance *instance) {
    if (!instance) return;
    for (size_t i = 0; i < instance->dynamic_count; i++) {
        GltfDynamicPrimitive *d = &instance->dynamic[i];
        if (d->mesh && d->mesh->geometry) {
            glDeleteVertexArrays(1, &d->mesh->geometry->vao); // the EBO is the model's
            free(d->mesh->geometry);
            d->mesh->geometry = NULL;
        }
        glDeleteBuffers(1, &d->vbo);
        free(d->scratch);
    }
    for (size_t i = 0; i < instance->mesh_count; i++) mesh_fini(instance->meshes[i]);
    if (instance->nodes && instance->model && instance->model->data)
        for (size_t n = 0; n < instance->model->data->nodes_count; n++) free(instance->nodes[n].weights);
    free(instance->nodes);
    free(instance->meshes);
    free(instance->dynamic);
    free(instance);
}
