#include <glad/glad.h>
#include <client.h>
#include <fast_obj.h>
#include <stb_image.h>

unsigned long long load_texture(char *path) {
    asset_cache_map_itr cached = vt_get(&g_texture_cache, path);
    if (!vt_is_end(cached)) return cached.data->val;

    stbi_set_flip_vertically_on_load(1);

    int width, height, _;
    unsigned char *texture = stbi_load(path, &width, &height, &_, 4);

    if (!texture) return 0;

    unsigned int texture_id;

    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, texture);
    glGenerateMipmap(GL_TEXTURE_2D);

    stbi_image_free(texture);

    const size_t path_length = strlen(path);
    char *cache_path = malloc(path_length + 1);

    if (cache_path) {
        memcpy(cache_path, path, path_length + 1);
        vt_insert(&g_texture_cache, cache_path, texture_id);
    }

    return texture_id;
}

GlyphCacheEntry *load_glyph(const char c) {
    glyph_cache_map_itr cached = vt_get(&g_glyph_cache, c);

    if (!vt_is_end(cached)) return cached.data->val;

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

    entry->size.x = (float) g_game_font->glyph->bitmap.width;
    entry->size.y = (float) g_game_font->glyph->bitmap.rows;
    entry->bearing.x = (float) g_game_font->glyph->bitmap_left;
    entry->bearing.y = (float) g_game_font->glyph->bitmap_top;
    entry->advance = (float) g_game_font->glyph->advance.x / 64.0f;

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    vt_insert(&g_glyph_cache, c, entry);
    return entry;
}

unsigned long long load_obj_model(char *path) {
    asset_cache_map_itr cached = vt_get(&g_model_cache, path);
    if (!vt_is_end(cached)) return cached.data->val;

    fastObjMesh *mesh = fast_obj_read(path);
    size_t idx = 0;

    size_t naive_vertex_count = 0;
    size_t alloc_vertex_count = 0;

    float min_y = 0.0f;

    for (size_t f = 0; f < mesh->face_count; f++) {
        const unsigned int fv = mesh->face_vertices[f];
        const unsigned int extra = fv - 3;

        naive_vertex_count += 3 + 3 * extra;
    }

    vertex *vertices = calloc(naive_vertex_count, sizeof(vertex));
    if (!vertices) return 0;

    for (size_t f = 0; f < mesh->face_count; f++) {
        const unsigned int fv = mesh->face_vertices[f];
        fastObjIndex anchor = {0};

        for (size_t v = 0; v < fv; v++) {
            const fastObjIndex indices = mesh->indices[idx];

            if (!v || v < 2) {
                if (!v) anchor = indices;

                idx++;
                continue;
            }

            const fastObjIndex prev = mesh->indices[idx-1];

            vertex v0 = {};
            vertex v1 = {};
            vertex v2 = {};

            v0.position.x = mesh->positions[anchor.p * 3];
            v0.position.y = mesh->positions[anchor.p * 3 + 1];
            v0.position.z = mesh->positions[anchor.p * 3 + 2];
            v0.tex_coord.x = mesh->texcoords[anchor.t * 2];
            v0.tex_coord.y = mesh->texcoords[anchor.t * 2 + 1];

            v1.position.x = mesh->positions[prev.p * 3];
            v1.position.y = mesh->positions[prev.p * 3 + 1];
            v1.position.z = mesh->positions[prev.p * 3 + 2];
            v1.tex_coord.x = mesh->texcoords[prev.t * 2];
            v1.tex_coord.y = mesh->texcoords[prev.t * 2 + 1];

            v2.position.x = mesh->positions[indices.p * 3];
            v2.position.y = mesh->positions[indices.p * 3 + 1];
            v2.position.z = mesh->positions[indices.p * 3 + 2];
            v2.tex_coord.x = mesh->texcoords[indices.t * 2];
            v2.tex_coord.y = mesh->texcoords[indices.t * 2 + 1];

            if (v0.position.y < min_y) min_y = v0.position.y;
            if (v1.position.y < min_y) min_y = v1.position.y;
            if (v2.position.y < min_y) min_y = v2.position.y;

            vertices[alloc_vertex_count] = v0;
            vertices[alloc_vertex_count + 1] = v1;
            vertices[alloc_vertex_count + 2] = v2;

            alloc_vertex_count += 3;
            idx++;
        }
    }

    for (size_t i = 0; i < naive_vertex_count; i++) {
        vertex *vertex = &vertices[i];
        vertex->position.y -= min_y;
    }

    unsigned int vbo;

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (long long) (naive_vertex_count * sizeof(vertex)), vertices, GL_STATIC_DRAW);

    free(vertices);
    fast_obj_destroy(mesh);

    const size_t path_length = strlen(path);
    char *cache_path = malloc(path_length + 1);

    if (cache_path) {
        memcpy(cache_path, path, path_length + 1);
        vt_insert(&g_model_cache, cache_path, vbo);
    }

    return vbo;
}

unsigned long long create_cube_model() {
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
    unsigned int ebo;

    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    return (unsigned long long) ebo << 32 | vbo;
}

unsigned long long create_plane_model() {
    static const vertex vertices[] = {
        {-0.5f, 1.0f, 0.5f, 0.0f, 0.0f},
        {0.5f, 1.0f, -0.5f, 1.0f, 1.0f},
        {-0.5f, 1.0f, -0.5f, 0.0f, 1.0f},
        {0.5f, 1.0f, 0.5f, 1.0f, 0.0f},
    };

    static const unsigned int indices[] = {0, 1, 2, 0, 3, 1};

    unsigned int vbo;
    unsigned int ebo;

    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    return (unsigned long long) ebo << 32 | vbo;
}

unsigned long long create_ramp_model() {
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
    unsigned int ebo;

    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    return (unsigned long long) ebo << 32 | vbo;
}