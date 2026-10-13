#include <gl.h>
#include <client.h>
#include <fast_obj.h>
#include <math.h>
#include <stdio.h>
#include <stb_image.h>
#include <pcg_basic.h>

unsigned int load_texture(char *path) {
    texture_cache_map_itr cached = vt_get(&g_texture_cache, path);
    if (!vt_is_end(cached)) return cached.data->val;

    ensure_asset_file(path);

    // GL wants the bottom row first. The HTML engine (html/engine/image.c) sets stb's
    // thread-local flip flag to 0 on this thread, which overrides the global flag, so the
    // flip must be set thread-locally too; restore stb's top-first default afterwards.
    stbi_set_flip_vertically_on_load_thread(1);

    int width, height, _;
    char swapped[1024];
    unsigned char *texture = stbi_load(file_redirect(path, swapped, sizeof(swapped)), &width, &height, &_, 4); // resource swapper
    stbi_set_flip_vertically_on_load_thread(0);

    if (!texture) {
        fprintf(stderr, "failed to load texture %s: %s\n", path, stbi_failure_reason());
        return 0;
    }
    unsigned int texture_id;

    glGenTextures(1, &texture_id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    g_active_texture = texture_id;

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, texture);
    glGenerateMipmap(GL_TEXTURE_2D);

    stbi_image_free(texture);

    const size_t path_length = strlen(path);
    char *cache_path = malloc(path_length + 1);

    if (cache_path) {
        memcpy(cache_path, path, path_length);
        cache_path[path_length] = 0;

        vt_insert(&g_texture_cache, cache_path, texture_id);
    }

    return texture_id;
}

int texture_cache_reload(const char *path) {
    texture_cache_map_itr cached = vt_get(&g_texture_cache, (char *) path);
    if (vt_is_end(cached)) return 0;
    const unsigned int texture_id = cached.data->val;
    ensure_asset_file(path);
    stbi_set_flip_vertically_on_load_thread(1); // as load_texture
    int width, height, _;
    char swapped[1024];
    unsigned char *pixels = stbi_load(file_redirect(path, swapped, sizeof(swapped)), &width, &height, &_, 4);
    stbi_set_flip_vertically_on_load_thread(0);
    if (!pixels) {
        fprintf(stderr, "failed to reload texture %s: %s\n", path, stbi_failure_reason());
        return 0;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    g_active_texture = texture_id;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
    stbi_image_free(pixels);
    return 1;
}

int geometry_cache_reload(const char *path) {
    geometry_cache_map_itr cached = vt_get(&g_geometry_cache, (char *) path);
    if (vt_is_end(cached)) return 0;
    char *key = cached.data->key;
    Geometry *old = cached.data->val;
    vt_erase_itr(&g_geometry_cache, cached);
    Geometry *fresh = load_obj_model(key, 0); // parses the file again and caches it under a new key
    if (!fresh) {
        vt_insert(&g_geometry_cache, key, old); // unreadable: keep the model it had
        return 0;
    }
    glDeleteVertexArrays(1, &old->vao);
    glDeleteBuffers(1, &old->ebo);
    *old = *fresh; // every mesh holding `old` now draws the new buffers
    geometry_cache_map_itr again = vt_get(&g_geometry_cache, key);
    if (!vt_is_end(again)) again.data->val = old;
    free(fresh);
    free(key);
    return 1;
}

GlyphCacheEntry *load_glyph(const char c) {
    glyph_cache_map_itr cached = vt_get(&g_glyph_cache, c);
    if (!vt_is_end(cached)) return cached.data->val;

    FT_Set_Pixel_Sizes(g_game_font, 0, 100);
    if (FT_Load_Char(g_game_font, c, FT_LOAD_RENDER)) return NULL;

    GlyphCacheEntry *entry = calloc(1, sizeof(GlyphCacheEntry));
    if (!entry) return NULL;

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(1, &entry->texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, entry->texture);
    g_active_texture = entry->texture;

    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_R8,
        (int) g_game_font->glyph->bitmap.width,
        (int) g_game_font->glyph->bitmap.rows,
        0, GL_RED, GL_UNSIGNED_BYTE,
        g_game_font->glyph->bitmap.buffer
    );

    entry->size.x = (float) g_game_font->glyph->metrics.width / 64.0f;
    entry->size.y = (float) g_game_font->glyph->metrics.height / 64.0f;
    entry->h_bearing.x = (float) g_game_font->glyph->metrics.horiBearingX / 64.0f;
    entry->h_bearing.y = (float) g_game_font->glyph->metrics.horiBearingY / 64.0f;
    entry->v_bearing.x = (float) g_game_font->glyph->metrics.vertBearingX / 64.0f;
    entry->v_bearing.y = (float) g_game_font->glyph->metrics.vertBearingY / 64.0f;
    entry->advance = (float) g_game_font->glyph->advance.x / 64.0f;

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    vt_insert(&g_glyph_cache, c, entry);
    return entry;
}

void geometry_set_bounds(Geometry *geometry, const vertex *vertices, const size_t vertex_count) {
    if (!geometry || !vertices || !vertex_count) return;

    vec3 min = vertices[0].position;
    vec3 max = vertices[0].position;
    for (size_t i = 1; i < vertex_count; i++) {
        const vec3 p = vertices[i].position;
        if (p.x < min.x) min.x = p.x;
        if (p.y < min.y) min.y = p.y;
        if (p.z < min.z) min.z = p.z;
        if (p.x > max.x) max.x = p.x;
        if (p.y > max.y) max.y = p.y;
        if (p.z > max.z) max.z = p.z;
    }

    const float cx = (min.x + max.x) * 0.5f, cy = (min.y + max.y) * 0.5f, cz = (min.z + max.z) * 0.5f;
    float radius_sq = 0.0f;
    for (size_t i = 0; i < vertex_count; i++) {
        const vec3 p = vertices[i].position;
        const float dx = p.x - cx, dy = p.y - cy, dz = p.z - cz;
        const float d = dx * dx + dy * dy + dz * dz;
        if (d > radius_sq) radius_sq = d;
    }

    geometry->bound_center[0] = cx;
    geometry->bound_center[1] = cy;
    geometry->bound_center[2] = cz;
    geometry->bound_radius = sqrtf(radius_sq) * 1.001f + 1e-4f; // float slack: never cull a touching mesh
}

Geometry *load_obj_model(char *path, const int transform_y) {
    geometry_cache_map_itr cached = vt_get(&g_geometry_cache, path);
    if (!vt_is_end(cached)) return cached.data->val;

    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!geometry) return NULL;

    ensure_asset_file(path);
    char swapped[1024];
    fastObjMesh *mesh = fast_obj_read(file_redirect(path, swapped, sizeof(swapped))); // resource pack / swapper

    if (!mesh) {
        fprintf(stderr, "failed to load model %s\n", path);
        free(geometry);
        return NULL;
    }

    size_t idx = 0;

    size_t vertex_count = 0;
    size_t index_count = 0;

    size_t alloc_vertex_count = 0;
    size_t alloc_index_count = 0;

    vec3 bounds_min = {0};
    vec3 bounds_max = {0};

    for (size_t f = 0; f < mesh->face_count; f++) {
        const unsigned int fv = mesh->face_vertices[f];
        const unsigned int extra = fv - 3;

        if (fv < 3) continue;

        vertex_count += fv;
        index_count += 3 * (1 + extra);
    }

    vertex vertices[vertex_count];
    unsigned int indices[index_count];

    for (size_t f = 0; f < mesh->face_count; f++) {
        const unsigned int fv = mesh->face_vertices[f];

        // Polylines (`l` records, e.g. weapon_14.obj) come through as faces with < 3 vertices; they were not
        // counted above, so skip them here too instead of writing past the end of `vertices`.
        if (fv < 3) {
            idx += fv;
            continue;
        }

        const fastObjIndex anchor = mesh->indices[idx++];
        const size_t anchor_idx = alloc_vertex_count++;

        const fastObjIndex prev = mesh->indices[idx++];
        size_t prev_idx = alloc_vertex_count++;

        const vertex v0 = {
            mesh->positions[anchor.p * 3],
            mesh->positions[anchor.p * 3 + 1],
            mesh->positions[anchor.p * 3 + 2],
            mesh->texcoords[anchor.t * 2],
            mesh->texcoords[anchor.t * 2 + 1],
        };

        const vertex v1 = {
            mesh->positions[prev.p * 3],
            mesh->positions[prev.p * 3 + 1],
            mesh->positions[prev.p * 3 + 2],
            mesh->texcoords[prev.t * 2],
            mesh->texcoords[prev.t * 2 + 1],
        };

        if (v0.position.x < bounds_min.x) bounds_min.x = v0.position.x;
        if (v0.position.y < bounds_min.y) bounds_min.y = v0.position.y;
        if (v0.position.z < bounds_min.z) bounds_min.z = v0.position.z;
        if (v0.position.x > bounds_max.x) bounds_max.x = v0.position.x;
        if (v0.position.y > bounds_max.y) bounds_max.y = v0.position.y;
        if (v0.position.z > bounds_max.z) bounds_max.z = v0.position.z;

        if (v1.position.x < bounds_min.x) bounds_min.x = v1.position.x;
        if (v1.position.y < bounds_min.y) bounds_min.y = v1.position.y;
        if (v1.position.z < bounds_min.z) bounds_min.z = v1.position.z;
        if (v1.position.x > bounds_max.x) bounds_max.x = v1.position.x;
        if (v1.position.y > bounds_max.y) bounds_max.y = v1.position.y;
        if (v1.position.z > bounds_max.z) bounds_max.z = v1.position.z;

        vertices[anchor_idx] = v0;
        vertices[prev_idx] = v1;

        for (size_t v = 2; v < fv; v++) {
            const fastObjIndex current = mesh->indices[idx++];
            const size_t current_idx = alloc_vertex_count++;

            const vertex v2 = {
                mesh->positions[current.p * 3],
                mesh->positions[current.p * 3 + 1],
                mesh->positions[current.p * 3 + 2],
                mesh->texcoords[current.t * 2],
                mesh->texcoords[current.t * 2 + 1],
            };

            if (v2.position.x < bounds_min.x) bounds_min.x = v2.position.x;
            if (v2.position.y < bounds_min.y) bounds_min.y = v2.position.y;
            if (v2.position.z < bounds_min.z) bounds_min.z = v2.position.z;
            if (v2.position.x > bounds_max.x) bounds_max.x = v2.position.x;
            if (v2.position.y > bounds_max.y) bounds_max.y = v2.position.y;
            if (v2.position.z > bounds_max.z) bounds_max.z = v2.position.z;

            vertices[current_idx] = v2;

            indices[alloc_index_count++] = anchor_idx;
            indices[alloc_index_count++] = prev_idx;
            indices[alloc_index_count++] = current_idx;

            prev_idx = current_idx;
        }
    }

    if (transform_y) {
        for (size_t i = 0; i < alloc_vertex_count; i++) {
            vertex *vertex = &vertices[i];
            vertex->position.y -= bounds_min.y;
        }
    }

    unsigned int vbo;

    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, &vbo);

    glBindVertexArray(geometry->vao);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (int) (alloc_vertex_count * sizeof(vertex)), vertices, GL_STATIC_DRAW);
    geometry_set_bounds(geometry, vertices, alloc_vertex_count);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (int) (alloc_index_count * sizeof(unsigned int)), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    geometry->index_count = (int) alloc_index_count;
    fast_obj_destroy(mesh);

    const size_t path_length = strlen(path);
    char *cache_path = malloc(path_length + 1);

    if (cache_path) {
        memcpy(cache_path, path, path_length);
        cache_path[path_length] = 0;

        vt_insert(&g_geometry_cache, cache_path, geometry);
    }

    return geometry;
}

static const vertex cube_vertices[] = {
    // FRONT
    {-0.5f, 0.0f, 0.5f, 0.0f, 0.0f},
    {0.5f, 1.0f, 0.5f, 1.0f, 1.0f},
    {-0.5f, 1.0f, 0.5f, 0.0f, 1.0f},
    {0.5f, 0.0f, 0.5f, 1.0f, 0.0f},

    // BACK
    {0.5f, 0.0f, -0.5f, 0.0f, 0.0f},
    {-0.5f, 1.0f, -0.5f, 1.0f, 1.0f},
    {0.5f, 1.0f, -0.5f, 0.0f, 1.0f},
    {-0.5f, 0.0f, -0.5f, 1.0f, 0.0f},

    // LEFT
    {-0.5f, 0.0f, -0.5f, 0.0f, 0.0f},
    {-0.5f, 1.0f, 0.5f, 1.0f, 1.0f},
    {-0.5f, 1.0f, -0.5f, 0.0f, 1.0f},
    {-0.5f, 0.0f, 0.5f, 1.0f, 0.0f},

    // RIGHT
    {0.5f, 0.0f, 0.5f, 0.0f, 0.0f},
    {0.5f, 1.0f, -0.5f, 1.0f, 1.0f},
    {0.5f, 1.0f, 0.5f, 0.0f, 1.0f},
    {0.5f, 0.0f, -0.5f, 1.0f, 0.0f},

    // TOP
    {-0.5f, 1.0f, 0.5f, 0.0f, 0.0f},
    {0.5f, 1.0f, -0.5f, 1.0f, 1.0f},
    {-0.5f, 1.0f, -0.5f, 0.0f, 1.0f},
    {0.5f, 1.0f, 0.5f, 1.0f, 0.0f},

    // BOTTOM
    {-0.5f, 0.0f, -0.5f, 0.0f, 0.0f},
    {0.5f, 0.0f, 0.5f, 1.0f, 1.0f},
    {-0.5f, 0.0f, 0.5f, 0.0f, 1.0f},
    {0.5f, 0.0f, -0.5f, 1.0f, 0.0f},
};

static const unsigned int cube_indices[] = {
    0, 1, 2, 0, 3, 1, // FRONT
    4, 5, 6, 4, 7, 5, // BACK
    8, 9, 10, 8, 11, 9, // LEFT
    12, 13, 14, 12, 15, 13, // RIGHT
    16, 17, 18, 16, 19, 17, // TOP
    20, 21, 22, 20, 23, 21, // BOTTOM
};

// cube/plane/ramp vertices span x, z in [-0.5, 0.5] and y in [0, 1]
static void geometry_set_unit_bounds(Geometry *geometry) {
    geometry->bound_center[0] = 0.0f;
    geometry->bound_center[1] = 0.5f;
    geometry->bound_center[2] = 0.0f;
    geometry->bound_radius = 0.867f; // >= sqrt(3 * 0.5^2), the corner distance
}

Geometry *create_cube_geo() {
    if (g_cube_geometry) return g_cube_geometry;

    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!geometry) return NULL;

    unsigned int vbo;

    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, &vbo);

    glBindVertexArray(geometry->vao);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cube_vertices), cube_vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(cube_indices), cube_indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    geometry->index_count = sizeof(cube_indices) / sizeof(cube_indices[0]);

    geometry_set_unit_bounds(geometry);
    g_cube_geometry = geometry;
    return geometry;
}

static const vertex plane_vertices[] = {
    {-0.5f, 1.0f, 0.5f, 0.0f, 0.0f},
    {0.5f, 1.0f, -0.5f, 1.0f, 1.0f},
    {-0.5f, 1.0f, -0.5f, 0.0f, 1.0f},
    {0.5f, 1.0f, 0.5f, 1.0f, 0.0f},
};

static const unsigned int plane_indices[] = {0, 1, 2, 0, 3, 1};


Geometry *create_plane_geo() {
    if (g_plane_geometry) return g_plane_geometry;

    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!geometry) return NULL;

    unsigned int vbo;

    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, &vbo);

    glBindVertexArray(geometry->vao);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(plane_vertices), plane_vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(plane_indices), plane_indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    geometry->index_count = sizeof(plane_indices) / sizeof(plane_indices[0]);

    geometry_set_unit_bounds(geometry);
    g_plane_geometry = geometry;
    return geometry;
}

Geometry *create_ramp_geo() {
    if (g_ramp_geometry) return g_ramp_geometry;

    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!geometry) return NULL;

    static const vertex vertices[] = {
        // TOP
        {-0.5f, 0.0f, 0.5f, 0.0f, 0.0f},
        {0.5f, 1.0f, -0.5f, 1.0f, 1.0f},
        {-0.5f, 1.0f, -0.5f, 0.0f, 1.0f},
        {0.5f, 0.0f, 0.5f, 1.0f, 0.0f},

        // BOTTOM
        {-0.5f, 0.0f, -0.5f, 0.0f, 0.0f},
        {0.5f, 0.0f, 0.5f, 1.0f, 1.0f},
        {-0.5f, 0.0f, 0.5f, 0.0f, 1.0f},
        {0.5f, 0.0f, -0.5f, 1.0f, 0.0f},

        // LEFT
        {-0.5f, 0.0f, -0.5f, 0.0f, 0.0f},
        {-0.5f, 0.0f, 0.5f, 1.0f, 0.0f},
        {-0.5f, 1.0f, -0.5f, 0.0f, 1.0f},

        // RIGHT
        {0.5f, 0.0f, 0.5f, 0.0f, 0.0f},
        {0.5f, 0.0f, -0.5f, 1.0f, 0.0f},
        {0.5f, 1.0f, -0.5f, 0.0f, 1.0f},

        // BACK
        {0.5f, 0.0f, -0.5f, 0.0f, 0.0f},
        {-0.5f, 1.0f, -0.5f, 1.0f, 1.0f},
        {0.5f, 1.0f, -0.5f, 0.0f, 1.0f},
        {-0.5f, 0.0f, -0.5f, 1.0f, 0.0f},
    };

    static const unsigned int indices[] = {
        0, 1, 2, 0, 3, 1, // TOP
        4, 5, 6, 4, 7, 5, // BOTTOM
        8, 9, 10, // LEFT
        11, 12, 13, // RIGHT
        14, 15, 16, 14, 17, 15, // BACK
    };

    unsigned int vbo;

    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, &vbo);

    glBindVertexArray(geometry->vao);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    geometry->index_count = sizeof(indices) / sizeof(indices[0]);

    geometry_set_unit_bounds(geometry);
    g_ramp_geometry = geometry;
    return geometry;
}
