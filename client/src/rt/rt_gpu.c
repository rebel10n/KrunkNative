// Ray tracing: the RtScene on the GPU (client/include/rt.h "GPU table layout"). Five RGBA32F
// textures of width RT_TEX_WIDTH, read with texelFetch by rt_common.glsl:
//   tlas  the TLAS nodes (root 0; leaf primitives are global instance indices, as built)
//   blas  every BLAS's nodes packed one after the other: a BLAS's node links get its base added,
//         its leaves' first-triangle index gets tri_first added (rt_scene.c keeps both local)
//   A packed node holds its two CHILDREN's boxes and links (put_node; rt.h "GPU table layout"), so the GPU
//   enters a node without testing its own box and never fetches a node only to reject it.
//   tris  RtScene.tris as is (3 texels a triangle)
//   inst  instances; t6.y is the BLAS root in the packed table (-1 for the analytic kinds)
//   mats  materials
// What is uploaded when (rt.h RtScene epochs):
//   static_epoch       blas, tris, mats in full (a BLAS or material was added, a map unloaded)
//   refit_epoch        the node and triangle rows of the BLAS whose refit_serial moved (a posed glTF)
//   static_tlas_epoch  tlas and inst in full (the static half was rebuilt)
//   frame_epoch        otherwise only the root node, the dynamic half's nodes and the dynamic instances
// Each table keeps its packed rows on the CPU (RtTable.data) so a partial glTexSubImage2D of whole rows
// carries the right neighbours. Textures grow (never shrink) by rows; a (re)allocation forces a full upload.
#include <gl.h>
#include <rt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_max_texture_size;

static int table_rows_for(const int texels) {
    return (texels + RT_TEX_WIDTH - 1) / RT_TEX_WIDTH;
}

// Allocates / grows the texture to hold `rows` rows; 0 when the GPU cannot (texture too tall).
static int table_ensure(RtTable *table, const int rows) {
    if (!g_max_texture_size) {
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &g_max_texture_size);
        if (g_max_texture_size <= 0) g_max_texture_size = 2048;
    }
    const int want = rows < 1 ? 1 : rows;
    if (table->texture && table->rows >= want) return 1;
    int alloc = table->rows ? table->rows : 16;
    while (alloc < want) alloc *= 2;
    if (alloc > g_max_texture_size) {
        if (want > g_max_texture_size) return 0;
        alloc = g_max_texture_size;
    }
    if (!table->texture) glGenTextures(1, &table->texture);
    glBindTexture(GL_TEXTURE_2D, table->texture);
    if (table->integer) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, RT_TEX_WIDTH, alloc, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, NULL);
    else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, RT_TEX_WIDTH, alloc, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    table->rows = alloc;
    table->valid = 0;
    return 1;
}

// The table's CPU rows for `texels` texels (the padding past the old contents zeroed) and the texture to
// match; NULL when either does not fit.
static float *table_begin(RtTable *table, const int texels) {
    const size_t floats = (size_t) table_rows_for(texels) * RT_TEX_WIDTH * 4;
    if (floats > table->data_cap) {
        float *grown = realloc(table->data, floats * sizeof(float));
        if (!grown) return NULL;
        memset(grown + table->data_cap, 0, (floats - table->data_cap) * sizeof(float));
        table->data = grown;
        table->data_cap = floats;
    }
    if (!table_ensure(table, table_rows_for(texels))) return NULL;
    if (texels < table->texels) // the table shrank: the stale tail would otherwise stay in the rows uploaded
        memset(table->data + (size_t) texels * 4, 0, ((size_t) table->texels - (size_t) texels) * 4 * sizeof(float));
    table->texels = texels;
    return table->data;
}

// Uploads the rows holding texels [lo, hi) (every row of the table when it is not valid yet).
static void table_upload(RtTable *table, int lo, int hi) {
    const int rows = table_rows_for(table->texels);
    if (!rows) return;
    int row_lo = lo / RT_TEX_WIDTH, row_hi = (hi + RT_TEX_WIDTH - 1) / RT_TEX_WIDTH;
    if (!table->valid) { row_lo = 0; row_hi = rows; }
    if (row_hi > rows) row_hi = rows;
    if (row_hi <= row_lo) return;
    glBindTexture(GL_TEXTURE_2D, table->texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row_lo, RT_TEX_WIDTH, row_hi - row_lo, table->integer ? GL_RGBA_INTEGER : GL_RGBA,
                    table->integer ? GL_UNSIGNED_INT : GL_FLOAT, table->data + (size_t) row_lo * RT_TEX_WIDTH * 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    table->valid = 1;
}

static void table_free(RtTable *table) {
    if (table->texture) glDeleteTextures(1, &table->texture);
    free(table->data);
    memset(table, 0, sizeof(*table));
}

// ---- the node tables (rt.h "GPU table layout"): RGBA32UI, half-float child boxes rounded outward

static float half_to_float(const unsigned h) {
    const unsigned sign = (h >> 15) & 1u, exp = (h >> 10) & 31u, mant = h & 1023u;
    float v;
    if (exp == 0) v = ldexpf((float) mant, -24);
    else if (exp == 31) v = mant ? NAN : INFINITY;
    else v = ldexpf((float) (mant | 1024u), (int) exp - 25);
    return sign ? -v : v;
}

// round-to-nearest-even float -> half (overflow to infinity)
static unsigned half_rne(const float f) {
    unsigned x;
    memcpy(&x, &f, sizeof x);
    const unsigned sign = (x >> 16) & 0x8000u;
    const float a = fabsf(f);
    if (!(a < 65520.0f)) return sign | 0x7C00u;            // inf (and NaN, never packed)
    if (a < 6.103515625e-05f) {                            // subnormal half
        const unsigned m = (unsigned) lrintf(a * 16777216.0f); // a / 2^-24, ties to even
        return sign | m;
    }
    int e;
    const float m = frexpf(a, &e);                         // a = m * 2^e, m in [0.5, 1)
    unsigned mant = (unsigned) lrintf((m * 2.0f - 1.0f) * 1024.0f);
    int exp = e - 1 + 15;
    if (mant == 1024u) { mant = 0; exp++; }
    if (exp >= 31) return sign | 0x7C00u;
    return sign | ((unsigned) exp << 10) | mant;
}

// the half nearest to f in the given direction (up: >= f, else <= f): boxes rounded outward never shrink
static unsigned half_dir(const float f, const int up) {
    if (f >= 1e30f) return 0x7C00u;  // the empty slot's +inf min
    if (f <= -1e30f) return 0xFC00u; // and -inf max
    unsigned h = half_rne(f);
    const float back = half_to_float(h);
    if (up ? back >= f : back <= f) return h;
    const int neg = (h & 0x8000u) != 0;
    if ((h & 0x7FFFu) == 0) return up ? 0x0001u : 0x8001u;     // +-0 stepped to the smallest subnormal
    // magnitude grows when stepping away from zero: up on a positive, down on a negative
    return (up != neg) ? h + 1u : h - 1u;
}

static void put_box(unsigned *t, const float mn[3], const float mx[3]) {
    const unsigned a = half_dir(mn[0], 0), b = half_dir(mn[1], 0), c = half_dir(mn[2], 0);
    const unsigned d = half_dir(mx[0], 1), e = half_dir(mx[1], 1), f = half_dir(mx[2], 1);
    t[0] = a | (b << 16);
    t[1] = c | (d << 16);
    t[2] = e | (f << 16);
}

static int g_slot_overflow_logged;

// A child slot (one texel): the child's box, and its link word (rt.h). `node_offset` / `first_offset` turn the CPU
// tree's local indices into the packed table's (a BLAS's node base / tri_first).
static void put_slot(unsigned *t, const RtNode *child, const int child_index, const int node_offset, const int first_offset) {
    put_box(t, child->aabb_min, child->aabb_max);
    if (child->a < 0) {
        const unsigned first = (unsigned) (first_offset + (-child->a - 1)), count = (unsigned) child->b;
        if ((first >> (31 - RT_SLOT_COUNT_BITS)) || (count >> RT_SLOT_COUNT_BITS)) {
            if (!g_slot_overflow_logged) fprintf(stderr, "[rt] BVH leaf out of the packed range (first %u, count %u)\n", first, count);
            g_slot_overflow_logged = 1;
        }
        t[3] = (first << RT_SLOT_COUNT_BITS) | (count & ((1u << RT_SLOT_COUNT_BITS) - 1u));
    } else {
        t[3] = RT_SLOT_INNER | (unsigned) (node_offset + child_index);
    }
}

static void put_empty_slot(unsigned *t) {
    static const float empty_min[3] = {1e30f, 1e30f, 1e30f}, empty_max[3] = {-1e30f, -1e30f, -1e30f};
    put_box(t, empty_min, empty_max);
    t[3] = 0u; // a leaf of 0 primitives
}

// Packs node `n` of `nodes` (RT_NODE_TEXELS texels at `data`, uint bits in the float table): an inner node's slots are its
// children; a leaf (only ever entered as a root: a tiny TLAS, the self-shadow subtree, a one-leaf BLAS) packs itself as its
// left slot.
static void put_node(float *data, const RtNode *nodes, const int n, const int node_offset, const int first_offset) {
    unsigned t[8];
    const RtNode *node = &nodes[n];
    if (node->a < 0) {
        put_slot(t, node, n, node_offset, first_offset);
        put_empty_slot(t + 4);
    } else {
        put_slot(t, &nodes[node->a], node->a, node_offset, first_offset);
        put_slot(t + 4, &nodes[node->b], node->b, node_offset, first_offset);
    }
    memcpy(data, t, sizeof t);
}

// ---------------------------------------------------------------- the static tables

static void pack_blas(const RtGpu *gpu, const RtScene *rt, const int i, float *data) {
    const RtBlas *blas = &rt->blas[i];
    const int base = gpu->blas_base[i];
    for (int n = 0; n < blas->node_count; n++)
        put_node(&data[(size_t) (base + n) * RT_NODE_TEXELS * 4], blas->nodes, n, base, blas->tri_first);
}

static void pack_tris(const RtScene *rt, const int first, const int count, float *data) {
    for (int i = first; i < first + count; i++) {
        const float *v = &rt->tris[(size_t) i * 9];
        float *t = &data[(size_t) i * RT_TRI_TEXELS * 4];
        for (int k = 0; k < 3; k++) {
            t[k * 4] = v[k * 3];
            t[k * 4 + 1] = v[k * 3 + 1];
            t[k * 4 + 2] = v[k * 3 + 2];
            t[k * 4 + 3] = 0.0f;
        }
    }
}

static int upload_static(RtGpu *gpu, const RtScene *rt) {
    gpu->blas.integer = 1;
    if (rt->blas_count > gpu->blas_base_cap) {
        int *grown = realloc(gpu->blas_base, (size_t) rt->blas_count * sizeof(int));
        unsigned *serials = realloc(gpu->blas_serial, (size_t) rt->blas_count * sizeof(unsigned));
        if (grown) gpu->blas_base = grown;
        if (serials) gpu->blas_serial = serials;
        if (!grown || !serials) return 0;
        gpu->blas_base_cap = rt->blas_count;
    }
    int total = 0;
    for (int i = 0; i < rt->blas_count; i++) {
        gpu->blas_base[i] = total;
        gpu->blas_serial[i] = rt->blas[i].refit_serial;
        total += rt->blas[i].node_count; // a dead BLAS (geometry NULL) has none
    }
    gpu->blas_node_total = total;

    float *data = table_begin(&gpu->blas, total * RT_NODE_TEXELS);
    if (!data) return 0;
    for (int i = 0; i < rt->blas_count; i++) pack_blas(gpu, rt, i, data);
    gpu->blas.valid = 0;
    table_upload(&gpu->blas, 0, gpu->blas.texels);

    data = table_begin(&gpu->tris, rt->tri_count * RT_TRI_TEXELS);
    if (!data) return 0;
    pack_tris(rt, 0, rt->tri_count, data);
    gpu->tris.valid = 0;
    table_upload(&gpu->tris, 0, gpu->tris.texels);

    data = table_begin(&gpu->mats, rt->material_count * RT_MAT_TEXELS);
    if (!data) return 0;
    for (int i = 0; i < rt->material_count; i++) {
        const RtMaterial *m = &rt->materials[i];
        float *t = &data[(size_t) i * RT_MAT_TEXELS * 4];
        memcpy(&t[0], m->color, 4 * sizeof(float));
        t[4] = m->emissive[0]; t[5] = m->emissive[1]; t[6] = m->emissive[2]; t[7] = m->alpha_test;
        memcpy(&t[8], m->atlas, 4 * sizeof(float));
        t[12] = m->metalness; t[13] = m->roughness; t[14] = m->reflect_intensity; t[15] = (float) m->unlit;
    }
    gpu->mats.valid = 0;
    table_upload(&gpu->mats, 0, gpu->mats.texels);

    gpu->uploaded_static_epoch = rt->static_epoch;
    gpu->uploaded_refit_epoch = rt->refit_epoch;
    return 1;
}

// The BLAS whose refit_serial moved since their upload: their node rows and triangle rows only.
static void upload_refits(RtGpu *gpu, const RtScene *rt) {
    for (int i = 0; i < rt->blas_count && i < gpu->blas_base_cap; i++) {
        const RtBlas *blas = &rt->blas[i];
        if (gpu->blas_serial[i] == blas->refit_serial || !blas->node_count) continue;
        gpu->blas_serial[i] = blas->refit_serial;
        pack_blas(gpu, rt, i, gpu->blas.data);
        table_upload(&gpu->blas, gpu->blas_base[i] * RT_NODE_TEXELS, (gpu->blas_base[i] + blas->node_count) * RT_NODE_TEXELS);
        pack_tris(rt, blas->tri_first, blas->tri_count, gpu->tris.data);
        table_upload(&gpu->tris, blas->tri_first * RT_TRI_TEXELS, (blas->tri_first + blas->tri_count) * RT_TRI_TEXELS);
    }
    gpu->uploaded_refit_epoch = rt->refit_epoch;
}

// ---------------------------------------------------------------- the per-frame tables

static void pack_tlas(const RtScene *rt, const int from, const int to, float *data) {
    for (int n = from; n < to; n++) {
        put_node(&data[(size_t) n * RT_NODE_TEXELS * 4], rt->tlas, n, 0, 0);
    }
}

static void pack_instances(const RtGpu *gpu, const RtScene *rt, const int from, const int to, float *data) {
    for (int i = from; i < to; i++) {
        const RtInstance *inst = &rt->instances[i];
        float *t = &data[(size_t) i * RT_INST_TEXELS * 4];
        memcpy(&t[0], inst->world_to_local, 12 * sizeof(float));
        memcpy(&t[12], inst->local_to_world, 12 * sizeof(float));
        t[24] = (float) inst->kind;
        t[25] = inst->kind == RT_INST_TRIANGLES && inst->blas >= 0 && inst->blas < rt->blas_count ? (float) gpu->blas_base[inst->blas] : -1.0f;
        t[26] = (float) inst->material;
        t[27] = (float) inst->flags;
        t[28] = inst->aabb_min[0]; t[29] = inst->aabb_min[1]; t[30] = inst->aabb_min[2]; t[31] = 0.0f;
    }
}

static int upload_frame(RtGpu *gpu, const RtScene *rt) {
    gpu->tlas.integer = 1;
    const int all = !gpu->uploaded_frame || gpu->uploaded_static_tlas_epoch != rt->static_tlas_epoch;
    // tlas: node 0 (the root) and the dynamic half, nodes [1 + static_tlas_count, tlas_count)
    {
        const int dyn_at = rt->tlas_count ? 1 + rt->static_tlas_count : 0;
        float *data = table_begin(&gpu->tlas, rt->tlas_count * RT_NODE_TEXELS);
        if (!data) return 0;
        if (all || !gpu->tlas.valid) {
            pack_tlas(rt, 0, rt->tlas_count, data);
            table_upload(&gpu->tlas, 0, gpu->tlas.texels);
        } else {
            pack_tlas(rt, 0, rt->tlas_count ? 1 : 0, data);
            pack_tlas(rt, dyn_at, rt->tlas_count, data);
            table_upload(&gpu->tlas, 0, RT_NODE_TEXELS);
            table_upload(&gpu->tlas, dyn_at * RT_NODE_TEXELS, gpu->tlas.texels);
        }
    }
    // instances: the dynamic half, [static_instance_count, instance_count)
    {
        float *data = table_begin(&gpu->inst, rt->instance_count * RT_INST_TEXELS);
        if (!data) return 0;
        const int from = all || !gpu->inst.valid ? 0 : rt->static_instance_count;
        pack_instances(gpu, rt, from, rt->instance_count, data);
        table_upload(&gpu->inst, from * RT_INST_TEXELS, gpu->inst.texels);
    }
    gpu->uploaded_frame_epoch = rt->frame_epoch;
    gpu->uploaded_static_tlas_epoch = rt->static_tlas_epoch;
    gpu->tlas_count = rt->tlas_count;
    return 1;
}

int rt_gpu_upload(RtGpu *gpu, const RtScene *rt) {
    int previous_texture_unit;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_texture_unit);
    glActiveTexture(GL_TEXTURE0 + RT_TEXTURE_UNIT); // a spare unit: nothing of the renderer's unit-0 cache is disturbed
    int ok = 1;
    if (!gpu->uploaded_static || gpu->uploaded_static_epoch != rt->static_epoch) {
        ok = upload_static(gpu, rt);
        gpu->uploaded_static = ok;
        gpu->uploaded_frame = 0; // the BLAS bases moved: every instance's t6.y is repacked
    } else if (gpu->uploaded_refit_epoch != rt->refit_epoch) {
        upload_refits(gpu, rt);
    }
    if (ok && (!gpu->uploaded_frame || gpu->uploaded_frame_epoch != rt->frame_epoch)) {
        ok = upload_frame(gpu, rt);
        gpu->uploaded_frame = ok;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture((unsigned int) previous_texture_unit);
    if (!ok && !gpu->failed_logged) {
        gpu->failed_logged = 1;
        fprintf(stderr, "[rt] scene tables do not fit the GPU (max texture size %d): ray tracing off for this map\n", g_max_texture_size);
    }
    return ok;
}

static GlUniform u_tlas = GL_UNIFORM_INIT("rt_tlas");
static GlUniform u_blas = GL_UNIFORM_INIT("rt_blas");
static GlUniform u_tris = GL_UNIFORM_INIT("rt_tris");
static GlUniform u_inst = GL_UNIFORM_INIT("rt_inst");
static GlUniform u_mats = GL_UNIFORM_INIT("rt_mats");
static GlUniform u_tlas_count = GL_UNIFORM_INIT("rt_tlas_count");

// The TLAS / BLAS tables are integer textures (usampler2D) on first_unit and first_unit + 1. A float
// sampler of the same program left on one of those units (a sampler the pass never sets keeps its
// default 0, e.g. rt_atlas without alpha-tested casters) is a sampler-type conflict: desktop GL draws
// anyway, but ES 3.0 / ANGLE / WebGL reject the draw (GL_INVALID_OPERATION), which blacked out every
// traced effect on ANGLE. Once per program, every other sampler still on those two units is moved to
// the rt_tris unit (a float 2D table, so sharing it is legal; no pass reads a sampler it never set).
// A pass that sets a sampler later overrides this as before.
#define RT_GPU_CHECKED_MAX 64
static unsigned int g_checked_programs[RT_GPU_CHECKED_MAX];
static int g_checked_count;

static void move_float_samplers_off_integer_units(const unsigned int program, const int first_unit) {
    for (int i = 0; i < g_checked_count; i++) if (g_checked_programs[i] == program) return;
    if (g_checked_count < RT_GPU_CHECKED_MAX) g_checked_programs[g_checked_count++] = program;
    int count = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
    for (int i = 0; i < count; i++) {
        char name[128];
        int size = 0;
        unsigned int type = 0;
        glGetActiveUniform(program, (unsigned int) i, (int) sizeof(name), NULL, &size, &type, name);
        if (type != GL_SAMPLER_2D && type != GL_SAMPLER_3D && type != GL_SAMPLER_CUBE && type != GL_SAMPLER_2D_SHADOW) continue;
        if (!strcmp(name, "rt_tris") || !strcmp(name, "rt_inst") || !strcmp(name, "rt_mats")) continue;
        const int location = glGetUniformLocation(program, name);
        if (location < 0) continue;
        int unit = 0;
        glGetUniformiv(program, location, &unit);
        if (unit != first_unit && unit != first_unit + 1) continue;
        unsigned int previous = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, (int *) &previous);
        if (previous != program) glUseProgram(program);
        glUniform1i(location, first_unit + 2); // rt_tris's unit: a float 2D table
        if (previous != program) glUseProgram(previous);
    }
}

void rt_gpu_bind(const RtGpu *gpu, const unsigned int program, const int first_unit) {
    const unsigned int textures[5] = {gpu->tlas.texture, gpu->blas.texture, gpu->tris.texture, gpu->inst.texture, gpu->mats.texture};
    GlUniform *uniforms[5] = {&u_tlas, &u_blas, &u_tris, &u_inst, &u_mats};
    for (int i = 0; i < 5; i++) {
        glActiveTexture(GL_TEXTURE0 + (unsigned int) (first_unit + i));
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        gl_uniform_1i(uniforms[i], program, first_unit + i);
    }
    gl_uniform_1i(&u_tlas_count, program, gpu->tlas_count);
    move_float_samplers_off_integer_units(program, first_unit);
}

void rt_gpu_free(RtGpu *gpu) {
    table_free(&gpu->tlas);
    table_free(&gpu->blas);
    table_free(&gpu->tris);
    table_free(&gpu->inst);
    table_free(&gpu->mats);
    free(gpu->blas_base);
    free(gpu->blas_serial);
    memset(gpu, 0, sizeof(*gpu));
}

// ---------------------------------------------------------------- emissive lights (rt.h): one RGBA32F row
void rt_gpu_upload_emissive(RtEmissiveTable *table, const RtEmissiveLight *lights, const int count) {
    static float data[RT_EMISSIVE_MAX * RT_EMISSIVE_TEXELS * 4];
    const int n = count < RT_EMISSIVE_MAX ? count : RT_EMISSIVE_MAX;
    glActiveTexture(GL_TEXTURE0 + RT_EMISSIVE_UNIT); // off unit 0 (loaders.c's binding cache) and the passes' target units
    if (!table->texture) {
        glGenTextures(1, &table->texture);
        glBindTexture(GL_TEXTURE_2D, table->texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, RT_EMISSIVE_MAX * RT_EMISSIVE_TEXELS, 1, 0, GL_RGBA, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, table->texture);
    }
    table->count = n;
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            float *t = &data[i * RT_EMISSIVE_TEXELS * 4];
            const RtEmissiveLight *l = &lights[i];
            float len[3], size2 = 0.0f;
            for (int k = 0; k < 3; k++) {
                const float sq = l->axis[k][0] * l->axis[k][0] + l->axis[k][1] * l->axis[k][1] + l->axis[k][2] * l->axis[k][2];
                len[k] = sqrtf(sq);
                size2 += sq;
            }
            const float area = 8.0f * (len[0] * len[1] + len[1] * len[2] + len[0] * len[2]);
            const float lum = 0.2126f * l->radiance[0] + 0.7152f * l->radiance[1] + 0.0722f * l->radiance[2];
            t[0] = l->center[0]; t[1] = l->center[1]; t[2] = l->center[2]; t[3] = lum * area;
            for (int k = 0; k < 3; k++) { t[4 + k * 4] = l->axis[k][0]; t[5 + k * 4] = l->axis[k][1]; t[6 + k * 4] = l->axis[k][2]; }
            t[7] = size2; t[11] = (float) l->instance; t[15] = 0.0f;
            t[16] = l->radiance[0]; t[17] = l->radiance[1]; t[18] = l->radiance[2]; t[19] = 0.0f;
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, n * RT_EMISSIVE_TEXELS, 1, GL_RGBA, GL_FLOAT, data);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
}

void rt_gpu_free_emissive(RtEmissiveTable *table) {
    if (table->texture) glDeleteTextures(1, &table->texture);
    table->texture = 0;
    table->count = 0;
}
