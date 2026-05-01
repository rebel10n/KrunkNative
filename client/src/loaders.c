#include <glad/glad.h>
#include <client.h>
#include <fast_obj.h>
#include <math.h>
#include <stb_image.h>

unsigned int load_texture(char *path) {
    texture_cache_map_itr cached = vt_get(&g_texture_cache, path);
    if (!vt_is_end(cached)) return cached.data->val;

    stbi_set_flip_vertically_on_load(1);

    int width, height, _;
    unsigned char *texture = stbi_load(path, &width, &height, &_, 4);

    if (!texture) return 0;

    unsigned int texture_id;

    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);

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

GlyphCacheEntry *load_glyph(const char c) {
    glyph_cache_map_itr cached = vt_get(&g_glyph_cache, c);
    if (!vt_is_end(cached)) return cached.data->val;

    FT_Set_Pixel_Sizes(g_game_font, 0, 100);
    if (FT_Load_Char(g_game_font, c, FT_LOAD_RENDER)) return NULL;

    GlyphCacheEntry *entry = calloc(1, sizeof(GlyphCacheEntry));
    if (!entry) return NULL;

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(1, &entry->texture);
    glBindTexture(GL_TEXTURE_2D, entry->texture);

    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RED,
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

Geometry *load_obj_model(char *path) {
    geometry_cache_map_itr cached = vt_get(&g_geometry_cache, path);
    if (!vt_is_end(cached)) return cached.data->val;

    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!geometry) return NULL;

    fastObjMesh *mesh = fast_obj_read(path);
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

    for (size_t i = 0; i < alloc_vertex_count; i++) {
        vertex *vertex = &vertices[i];
        vertex->position.y -= bounds_min.y;
    }

    unsigned int vbo;

    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, &vbo);

    glBindVertexArray(geometry->vao);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (int) (alloc_vertex_count * sizeof(vertex)), vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (int) (alloc_index_count * sizeof(unsigned int)), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexArrayElementBuffer(geometry->vao, geometry->ebo);

    geometry->index_count = (int) alloc_index_count;
    geometry->height = bounds_max.x - bounds_min.y;
    geometry->bounding_sphere_radius = sqrtf(
        powf((bounds_max.x - bounds_min.x) * 0.5f, 2.0f) +
        powf((bounds_max.z - bounds_min.z) * 0.5f, 2.0f) +
        powf(geometry->height * 0.5f, 2.0f)
    );

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

Geometry *create_cube_geo() {
    if (g_cube_geometry) return g_cube_geometry;

    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!geometry) return NULL;

    static const vertex vertices[] = {
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

    static const unsigned int indices[] = {
        0, 1, 2, 0, 3, 1, // FRONT
        4, 5, 6, 4, 7, 5, // BACK
        8, 9, 10, 8, 11, 9, // LEFT
        12, 13, 14, 12, 15, 13, // RIGHT
        16, 17, 18, 16, 19, 17, // TOP
        20, 21, 22, 20, 23, 21, // BOTTOM
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
    glVertexArrayElementBuffer(geometry->vao, geometry->ebo);

    geometry->index_count = sizeof(indices) / sizeof(indices[0]);
    geometry->bounding_sphere_radius = sqrtf(1.5f);

    g_cube_geometry = geometry;
    return geometry;
}

Geometry *create_plane_geo() {
    if (g_plane_geometry) return g_plane_geometry;

    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!geometry) return NULL;

    static const vertex vertices[] = {
        {-0.5f, 1.0f, 0.5f, 0.0f, 0.0f},
        {0.5f, 1.0f, -0.5f, 1.0f, 1.0f},
        {-0.5f, 1.0f, -0.5f, 0.0f, 1.0f},
        {0.5f, 1.0f, 0.5f, 1.0f, 0.0f},
    };

    static const unsigned int indices[] = {0, 1, 2, 0, 3, 1};

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
    glVertexArrayElementBuffer(geometry->vao, geometry->ebo);

    geometry->index_count = sizeof(indices) / sizeof(indices[0]);
    geometry->bounding_sphere_radius = sqrtf(1.5f);

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
    glVertexArrayElementBuffer(geometry->vao, geometry->ebo);

    geometry->index_count = sizeof(indices) / sizeof(indices[0]);
    geometry->bounding_sphere_radius = sqrtf(1.5f);

    g_ramp_geometry = geometry;
    return geometry;
}
