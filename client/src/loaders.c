#include <gl.h>
#include <client.h>
#include <fast_obj.h>
#include <math.h>
#include <stdio.h>
#include <stb_image.h>
#include <pcg_basic.h>

void gl_release_texture(const unsigned int texture) {
    if (!texture) return;
    GLint active = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    for (int unit = 0; unit < GL_RENDER_TEXTURE_UNITS; unit++) {
        GLint bound = 0;
        glActiveTexture(GL_TEXTURE0 + (GLenum) unit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        if ((unsigned int) bound != texture) continue;
        glBindTexture(GL_TEXTURE_2D, 0);
        if (unit == 0) g_active_texture = 0; // unit 0 is empty now, whatever the cache said
    }
    glActiveTexture((GLenum) active);
    if (g_active_texture == texture) g_active_texture = 0;
}

static void cache_texture(const char *path, const unsigned int texture_id) {
    char *cache_path = strdup(path);
    if (cache_path) vt_insert(&g_texture_cache, cache_path, texture_id);
}

// The missing-texture checker: the 2x2 magenta / black texture render.js's missingTexture draws for a
// texture that fails to load (its loadTexture error path). One GL texture for the process lifetime,
// built on first use. Every texture loader falls back to it: load_texture (so the map, user-asset,
// sky, glTF, HUD and VFX textures), prefab.c's textures/<src>.png, sky.c, user_asset.c and gltf.c.
// It is never cached under a path (texture_cache_reload would overwrite it), so a file that arrives
// later still loads. KRUNKNATIVE_MISSING_TEXTURE=rrggbb paints it one flat colour (tools count its pixels).
static const unsigned char *checker_pixels(void) {
    static unsigned char pixels[16] = {0, 0, 0, 253, 255, 0, 219, 255, 255, 0, 219, 252, 0, 0, 0, 255};
    static int painted;
    if (painted) return pixels;
    painted = 1;
    // KRUNKNATIVE_MISSING_TEXTURE=rrggbb: one flat colour instead of the checker
    const char *debug = getenv("KRUNKNATIVE_MISSING_TEXTURE");
    unsigned int rgb;
    if (debug && strlen(debug) == 6 && sscanf(debug, "%6x", &rgb) == 1) {
        for (int i = 0; i < 4; i++) {
            pixels[i * 4] = (unsigned char) (rgb >> 16);
            pixels[i * 4 + 1] = (unsigned char) (rgb >> 8);
            pixels[i * 4 + 2] = (unsigned char) rgb;
            pixels[i * 4 + 3] = 255;
        }
    }
    return pixels; // rows bottom-up (GL): the PNG's palette indices are magenta / black, black / magenta
}

unsigned int texture_checker(void) {
    static unsigned int checker;
    if (checker) return checker;
    glGenTextures(1, &checker);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, checker);
    g_active_texture = checker;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, checker_pixels());
    return checker;
}

// The placeholders still waiting for their file (texture_placeholder), for texture_placeholders_check.
typedef struct {
    char *path;
    unsigned int texture;
} PendingTexture;
static PendingTexture *g_pending_textures;
static size_t g_pending_count;

// A file still downloading (natively asset_fetch.h, asset_prefetch() == 0): a 1x1 (0, 0, 0, 0)
// texture cached under its path, what three.js binds for a texture whose image has not loaded yet
// (WebGLState emptyTextures: black for opaque materials, nothing for transparent ones);
// texture_cache_reload() fills it in place when the file arrives (asset_fetch_client.c).
static unsigned int texture_placeholder(const char *path) {
    static const unsigned char black[4] = {0, 0, 0, 0};
    unsigned int texture_id;
    glGenTextures(1, &texture_id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    g_active_texture = texture_id;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
    glGenerateMipmap(GL_TEXTURE_2D);
    cache_texture(path, texture_id);
    char *pending_path = strdup(path);
    PendingTexture *grown = pending_path ? realloc(g_pending_textures, (g_pending_count + 1) * sizeof(PendingTexture)) : NULL;
    if (grown) {
        g_pending_textures = grown;
        g_pending_textures[g_pending_count++] = (PendingTexture) {pending_path, texture_id};
    } else {
        free(pending_path); // untracked: a give-up leaves this one blank
    }
    return texture_id;
}

// Each frame (asset_fetch_client.c): a placeholder whose file the fetch gave up (asset_prefetch() == -1)
// becomes the checker, so a missing user asset shows it instead of staying blank. A placeholder still
// downloading stays; one whose file arrived (texture_cache_reload filled it) or is present leaves the list.
// Returns how many placeholders became the checker.
int texture_placeholders_check(void) {
    int given_up = 0;
    size_t kept = 0;
    for (size_t i = 0; i < g_pending_count; i++) {
        PendingTexture pending = g_pending_textures[i];
        const int state = asset_prefetch(pending.path);
        if (state == 0) {
            g_pending_textures[kept++] = pending;
            continue;
        }
        if (state < 0) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, pending.texture);
            g_active_texture = pending.texture;
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, checker_pixels());
            given_up++;
        }
        free(pending.path);
    }
    g_pending_count = kept;
    return given_up;
}

unsigned int load_texture(char *path) {
    texture_cache_map_itr cached = vt_get(&g_texture_cache, path);
    if (!vt_is_end(cached)) return cached.data->val;

    const int available = ensure_asset_file(path);

    // GL wants the bottom row first. The HTML engine (html/engine/image.c) sets stb's
    // thread-local flip flag to 0 on this thread, which overrides the global flag, so the
    // flip must be set thread-locally too; restore stb's top-first default afterwards.
    stbi_set_flip_vertically_on_load_thread(1);

    int width, height, _;
    char swapped[1024];
    unsigned char *texture = stbi_load(file_redirect(path, swapped, sizeof(swapped)), &width, &height, &_, 4); // resource swapper
    stbi_set_flip_vertically_on_load_thread(0);

    if (!texture) {
        if (!available && asset_prefetch(path) == 0) return texture_placeholder(path); // downloading
        fprintf(stderr, "failed to load texture %s: %s\n", path, stbi_failure_reason());
        return texture_checker(); // missing or undecodable: the checker (not cached: a later call still tries the file)
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
    cache_texture(path, texture_id);
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
    geometry_free_wire(old);
    glDeleteBuffers(1, &old->ebo);
    *old = *fresh; // every mesh holding `old` now draws the new buffers
    geometry_cache_map_itr again = vt_get(&g_geometry_cache, key);
    if (!vt_is_end(again)) again.data->val = old;
    free(fresh);
    free(key);
    return 1;
}

// The cache keys first: geometry_cache_reload erases and re-inserts its entry.
static char **cache_keys(const int textures, size_t *count) {
    *count = 0;
    const size_t size = textures ? vt_size(&g_texture_cache) : vt_size(&g_geometry_cache);
    char **keys = size ? malloc(size * sizeof(char *)) : NULL;
    if (!keys) return NULL;
    if (textures) {
        for (texture_cache_map_itr it = vt_first(&g_texture_cache); !vt_is_end(it) && *count < size; it = vt_next(it))
            keys[(*count)++] = strdup(it.data->key);
    } else {
        for (geometry_cache_map_itr it = vt_first(&g_geometry_cache); !vt_is_end(it) && *count < size; it = vt_next(it))
            keys[(*count)++] = strdup(it.data->key);
    }
    return keys;
}

// Only keys that name a readable file: the cache also holds generated textures (v7sign:...
// canvas signs), which have no file to read again.
static int cache_key_is_file(const char *key) {
    char swapped[1024];
    FILE *f = fopen(file_redirect(key, swapped, sizeof(swapped)), "rb");
    if (f) fclose(f);
    return f != NULL;
}

static int cache_reload_all(const int textures) {
    size_t count;
    char **keys = cache_keys(textures, &count);
    int done = 0;
    for (size_t i = 0; i < count; i++) {
        if (keys[i] && cache_key_is_file(keys[i])) done += textures ? texture_cache_reload(keys[i]) : geometry_cache_reload(keys[i]);
        free(keys[i]);
    }
    free(keys);
    return done;
}

int texture_cache_reload_all(void) {
    return cache_reload_all(1);
}

int geometry_cache_reload_all(void) {
    return cache_reload_all(0);
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

void geometry_compute_normals(const vertex *vertices, const size_t vertex_count, const unsigned int *indices, const size_t index_count,
                              float *out) {
    memset(out, 0, vertex_count * 3 * sizeof(float));
    for (size_t t = 0; t + 2 < index_count; t += 3) {
        const unsigned int a = indices[t], b = indices[t + 1], c = indices[t + 2];
        if (a >= vertex_count || b >= vertex_count || c >= vertex_count) continue;
        const vec3 pa = vertices[a].position, pb = vertices[b].position, pc = vertices[c].position;
        const float e1x = pb.x - pa.x, e1y = pb.y - pa.y, e1z = pb.z - pa.z;
        const float e2x = pc.x - pa.x, e2y = pc.y - pa.y, e2z = pc.z - pa.z;
        // cb x ab as three.js (the face's counter-clockwise normal, its length twice the area: area-weighted)
        const float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
        const unsigned int v[3] = {a, b, c};
        for (int k = 0; k < 3; k++) {
            out[v[k] * 3] += nx;
            out[v[k] * 3 + 1] += ny;
            out[v[k] * 3 + 2] += nz;
        }
    }
    for (size_t i = 0; i < vertex_count; i++) {
        float *n = &out[i * 3];
        const float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 0.0f) {
            n[0] /= len;
            n[1] /= len;
            n[2] /= len;
        }
    }
}

void geometry_bind_normals(const float *normals, const size_t vertex_count) {
    if (!normals || !vertex_count) return;
    unsigned int vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (vertex_count * 3 * sizeof(float)), normals, GL_STATIC_DRAW);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), NULL);
    glEnableVertexAttribArray(3);
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

// Wireframe = GL_LINES over every triangle's three edges, from the OBJ model's CPU triangle
// list. Desktop GL could draw glPolygonMode(GL_LINE) instead, but that call does not exist on
// GLES (web, ANGLE), where the same skin came out filled; one edge buffer draws the same lines
// on every platform. glGetBufferSubData is not in GLES 3.0 either, hence the kept copy. The
// copy-buffer target leaves the VAO's element binding alone.
unsigned int geometry_wire_ebo(Geometry *geometry) {
    if (!geometry || geometry->draw_mode || !geometry->tri_indices || geometry->index_count < 3) return 0;
    if (geometry->wire_ebo) return geometry->wire_ebo;

    const size_t tri_count = (size_t) geometry->index_count / 3;
    unsigned int *edges = malloc(tri_count * 6 * sizeof(unsigned int));
    if (!edges) return 0;
    for (size_t t = 0; t < tri_count; t++) {
        const unsigned int *v = &geometry->tri_indices[t * 3];
        unsigned int *e = &edges[t * 6];
        e[0] = v[0]; e[1] = v[1];
        e[2] = v[1]; e[3] = v[2];
        e[4] = v[2]; e[5] = v[0];
    }

    unsigned int ebo = 0;
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_COPY_WRITE_BUFFER, ebo);
    glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr) (tri_count * 6 * sizeof(unsigned int)), edges, GL_STATIC_DRAW);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    free(edges);

    geometry->wire_ebo = ebo;
    geometry->wire_index_count = (int) (tri_count * 6);
    return ebo;
}

void geometry_free_wire(Geometry *geometry) {
    if (!geometry) return;
    if (geometry->wire_ebo) glDeleteBuffers(1, &geometry->wire_ebo);
    free(geometry->tri_indices);
    geometry->wire_ebo = 0;
    geometry->wire_index_count = 0;
    geometry->tri_indices = NULL;
}

Geometry *load_obj_model(char *path, const int transform_y) {
    geometry_cache_map_itr cached = vt_get(&g_geometry_cache, path);
    if (!vt_is_end(cached)) return cached.data->val;

    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!geometry) return NULL;

    const int available = ensure_asset_file(path);
    char swapped[1024];
    fastObjMesh *mesh = fast_obj_read(file_redirect(path, swapped, sizeof(swapped))); // resource pack / swapper

    if (!mesh) {
        if (!available && asset_prefetch(path) == 0) {
            // still downloading (asset_fetch.h): an empty model cached under the path, drawn as
            // nothing like v7's loadMesh before its OBJ arrives; geometry_cache_reload() rebuilds
            // it in place when the file is there, so the meshes holding it pop in
            glGenVertexArrays(1, &geometry->vao);
            glGenBuffers(1, &geometry->ebo);
            glBindVertexArray(geometry->vao);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
            glBindVertexArray(0);
            char *cache_path = strdup(path);
            if (cache_path) vt_insert(&g_geometry_cache, cache_path, geometry);
            return geometry;
        }
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
    // each corner's vn index (0 = none, fast_obj's reserved slot): the vertex normals of attribute 3
    unsigned int *corner_normal = vertex_count ? calloc(vertex_count, sizeof(unsigned int)) : NULL;

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
        if (corner_normal) {
            corner_normal[anchor_idx] = anchor.n;
            corner_normal[prev_idx] = prev.n;
        }

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
            if (corner_normal) corner_normal[current_idx] = current.n;

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

    // attribute 3: the file's vn, else the face normal (three.js OBJLoader keeps vn and computes the rest:
    // computeVertexNormals on its non-indexed corners is the face normal)
    float *normals = alloc_vertex_count ? malloc(alloc_vertex_count * 3 * sizeof(float)) : NULL;
    if (normals && corner_normal) {
        geometry_compute_normals(vertices, alloc_vertex_count, indices, alloc_index_count, normals);
        for (size_t i = 0; i < alloc_vertex_count; i++) {
            const unsigned int n = corner_normal[i];
            if (!n || n >= mesh->normal_count) continue;
            memcpy(&normals[i * 3], &mesh->normals[n * 3], 3 * sizeof(float));
        }
        geometry_bind_normals(normals, alloc_vertex_count);
    }
    free(normals);
    free(corner_normal);

    geometry->index_count = (int) alloc_index_count;
    if (alloc_index_count) { // the wireframe edges come from this list (geometry_wire_ebo)
        geometry->tri_indices = malloc(alloc_index_count * sizeof(unsigned int));
        if (geometry->tri_indices) memcpy(geometry->tri_indices, indices, alloc_index_count * sizeof(unsigned int));
    }
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

// KrunkNative Piss: render.js genHose (the e1624e4 branch) - a CylinderGeometry(radius, radius,
// length, sides) along z (rotation.x = PI / 2) with a SphereGeometry(radius, sides, max(8, sides / 2))
// at each end; the Piss's weapon mesh (player_hose_mesh). One cached geometry per size, never freed
// (like the cube).
#define HOSE_GEO_CACHE 4
typedef struct {
    float radius, length;
    int sides;
    Geometry *geometry;
} HoseGeoEntry;

Geometry *create_hose_geo(const float radius, const float length, const int sides_in) {
    static HoseGeoEntry cache[HOSE_GEO_CACHE];
    static int cached;
    const int sides = sides_in >= 3 ? sides_in : 18;
    for (int i = 0; i < cached; i++)
        if (cache[i].radius == radius && cache[i].length == length && cache[i].sides == sides) return cache[i].geometry;

    const int rings = sides / 2 > 8 ? sides / 2 : 8;
    const int cyl_vertices = (sides + 1) * 2, sphere_vertices = (sides + 1) * (rings + 1);
    const size_t nv = (size_t) (cyl_vertices + 2 * sphere_vertices);
    const size_t ni = (size_t) (sides * 6 + 2 * sides * rings * 6);
    vertex *v = calloc(nv, sizeof(vertex));
    unsigned int *idx = calloc(ni, sizeof(unsigned int));
    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!v || !idx || !geometry) {
        free(v);
        free(idx);
        free(geometry);
        return NULL;
    }

    size_t n = 0, k = 0;
    for (int e = 0; e < 2; e++) { // the tube: rings at z -length / 2 and +length / 2
        for (int x = 0; x <= sides; x++) {
            const float th = (float) x / (float) sides * 2.0f * (float) M_PI;
            v[n++] = (vertex) {{radius * sinf(th), radius * cosf(th), (e ? 0.5f : -0.5f) * length}, {(float) x / (float) sides, (float) e}};
        }
    }
    for (int x = 0; x < sides; x++) {
        const unsigned int a = (unsigned int) x, b = (unsigned int) (sides + 1 + x);
        const unsigned int tri[6] = {a, a + 1, b, b, a + 1, b + 1};
        memcpy(idx + k, tri, sizeof(tri));
        k += 6;
    }
    for (int e = 0; e < 2; e++) { // the end caps
        const unsigned int base = (unsigned int) n;
        const float cz = (e ? 0.5f : -0.5f) * length;
        for (int y = 0; y <= rings; y++) {
            const float ph = (float) y / (float) rings * (float) M_PI;
            for (int x = 0; x <= sides; x++) {
                const float th = (float) x / (float) sides * 2.0f * (float) M_PI;
                v[n++] = (vertex) {{radius * sinf(ph) * sinf(th), radius * sinf(ph) * cosf(th), cz + radius * cosf(ph)},
                                   {(float) x / (float) sides, (float) y / (float) rings}};
            }
        }
        for (int y = 0; y < rings; y++) {
            for (int x = 0; x < sides; x++) {
                const unsigned int a = base + (unsigned int) (y * (sides + 1) + x), b = a + (unsigned int) (sides + 1);
                const unsigned int tri[6] = {a, b, a + 1, b, b + 1, a + 1};
                memcpy(idx + k, tri, sizeof(tri));
                k += 6;
            }
        }
    }

    unsigned int vbo;
    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, &vbo);
    glBindVertexArray(geometry->vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (long) (nv * sizeof(vertex)), v, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long) (k * sizeof(unsigned int)), idx, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    geometry->index_count = (int) k;
    geometry_set_bounds(geometry, v, nv);
    free(v);
    free(idx);

    if (cached < HOSE_GEO_CACHE) cache[cached++] = (HoseGeoEntry) {radius, length, sides, geometry};
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
