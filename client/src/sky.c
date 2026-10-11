// Krunker v3.1.1 sky dome port (src/libs/SkyDome.js + render.js init/render).
//
// JS reference:
//   if (map.skyDome && mode && mode.skyCol == null) skyDome = SkyDome.fromConfig(...)
//   SphereGeometry(50, 32, 32); K = 1 - (y - bbox.min.y) / bbox.size.y;
//   stops {0: col0, 0.5: col1, 1: col2}, vertex colour = lerp within the stop pair;
//   MeshBasicMaterial({vertexColors, side: BackSide, depthWrite: false, transparent: true});
//   each frame: camera world position -> skyDome.position, render backgroundScene first.
// KrunkNative has no mode with a `skyCol` override (only FFA), so the mode test is
// always satisfied here.
#include <gl.h>
#include <client.h>
#include <sky.h>
#include <render_settings.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SKY_RADIUS 50.0f
#define SKY_SEGMENTS 32
#define SKY_VERTEX_FLOATS 8 // position(3) + uv(2) + colour(3)

typedef struct {
    int enabled;
    int textured;
    unsigned int texture;
    int mov_dir;      // skyDomeMovD: 0 scrolls U, anything else scrolls V
    float mov_speed;  // skyDomeMovT / 1e6 (offset per millisecond)
    float tex_offset;
    vec3 stops[3];

    unsigned int program;
    unsigned int vao;
    unsigned int vbo;
    unsigned int ebo;
    int index_count;
} SkyDome;

static SkyDome sky;

// COLORS.skyDome (src/data/colors.js).
static const char *const sky_default_colors[3] = {"#74a4b9", "#dce8ed", "#dce8ed"};

// `new THREE.Color(value || COLORS.skyDome[i])`: a falsy value (absent, 0, "") falls back.
static vec3 sky_color_or_default(const cJSON *json, const int index) {
    vec4 color = {1.0f, 1.0f, 1.0f, 1.0f};

    if (cJSON_IsNumber(json) && cJSON_GetNumberValue(json) != 0) {
        color = hex_to_vec((int) cJSON_GetNumberValue(json));
    } else if (cJSON_IsString(json) && cJSON_GetStringValue(json)[0]) {
        parse_hex_color(cJSON_GetStringValue(json), &color);
    } else {
        parse_hex_color(sky_default_colors[index], &color);
    }

    return (vec3) {color.x, color.y, color.z};
}

static int json_truthy(const cJSON *json) {
    if (!json || cJSON_IsNull(json) || cJSON_IsFalse(json)) return 0;
    if (cJSON_IsNumber(json)) return cJSON_GetNumberValue(json) != 0;
    if (cJSON_IsString(json)) return cJSON_GetStringValue(json)[0] != 0;
    return 1;
}

// Gradient position K -> colour, matching the stop loop in SkyDome.js
// (a value on a shared stop takes the later segment, which yields the same colour).
static vec3 sky_gradient(const float k) {
    const float stops[3] = {0.0f, 0.5f, 1.0f};
    vec3 out = sky.stops[0];

    for (int n = 0; n < 2; n++) {
        if (k >= stops[n] && k <= stops[n + 1]) {
            const float t = (k - stops[n]) / (stops[n + 1] - stops[n]);
            out.x = sky.stops[n].x + (sky.stops[n + 1].x - sky.stops[n].x) * t;
            out.y = sky.stops[n].y + (sky.stops[n + 1].y - sky.stops[n].y) * t;
            out.z = sky.stops[n].z + (sky.stops[n + 1].z - sky.stops[n].z) * t;
        }
    }

    return out;
}

// THREE.SphereBufferGeometry(50, 32, 32) (r121) with per-vertex gradient colours.
static void sky_build_geometry(void) {
    const int row = SKY_SEGMENTS + 1;
    const int vertex_count = row * row;
    float *vertices = malloc(sizeof(float) * SKY_VERTEX_FLOATS * vertex_count);
    unsigned int *indices = malloc(sizeof(unsigned int) * SKY_SEGMENTS * SKY_SEGMENTS * 6);

    if (!vertices || !indices) {
        free(vertices);
        free(indices);
        return;
    }

    for (int iy = 0; iy <= SKY_SEGMENTS; iy++) {
        const float v = (float) iy / SKY_SEGMENTS;
        float u_offset = 0.0f;
        if (iy == 0) u_offset = 0.5f / SKY_SEGMENTS;
        else if (iy == SKY_SEGMENTS) u_offset = -0.5f / SKY_SEGMENTS;

        for (int ix = 0; ix <= SKY_SEGMENTS; ix++) {
            const float u = (float) ix / SKY_SEGMENTS;
            const float phi = u * 2.0f * (float) M_PI;
            const float theta = v * (float) M_PI;
            float *out = &vertices[(iy * row + ix) * SKY_VERTEX_FLOATS];

            out[0] = -SKY_RADIUS * cosf(phi) * sinf(theta);
            out[1] = SKY_RADIUS * cosf(theta);
            out[2] = SKY_RADIUS * sinf(phi) * sinf(theta);
            out[3] = u + u_offset;
            out[4] = 1.0f - v;

            // Bounding box spans y = -50..50 (the poles), so K = 1 - (y + 50) / 100.
            const vec3 color = sky_gradient(1.0f - (out[1] + SKY_RADIUS) / (2.0f * SKY_RADIUS));
            out[5] = color.x;
            out[6] = color.y;
            out[7] = color.z;
        }
    }

    int index_count = 0;
    for (int iy = 0; iy < SKY_SEGMENTS; iy++) {
        for (int ix = 0; ix < SKY_SEGMENTS; ix++) {
            const unsigned int a = iy * row + ix + 1;
            const unsigned int b = iy * row + ix;
            const unsigned int c = (iy + 1) * row + ix;
            const unsigned int d = (iy + 1) * row + ix + 1;

            if (iy != 0) {
                indices[index_count++] = a;
                indices[index_count++] = b;
                indices[index_count++] = d;
            }
            if (iy != SKY_SEGMENTS - 1) {
                indices[index_count++] = b;
                indices[index_count++] = c;
                indices[index_count++] = d;
            }
        }
    }

    if (!sky.vao) {
        glGenVertexArrays(1, &sky.vao);
        glGenBuffers(1, &sky.vbo);
        glGenBuffers(1, &sky.ebo);
    }

    glBindVertexArray(sky.vao);
    glBindBuffer(GL_ARRAY_BUFFER, sky.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (sizeof(float) * SKY_VERTEX_FLOATS * vertex_count), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sky.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr) (sizeof(unsigned int) * index_count), indices, GL_STATIC_DRAW);

    const GLsizei stride = sizeof(float) * SKY_VERTEX_FLOATS;
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *) 0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *) (sizeof(float) * 3));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, (void *) (sizeof(float) * 5));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    sky.index_count = index_count;

    free(vertices);
    free(indices);
}

void sky_load(const cJSON *map_raw) {
    sky.enabled = 0;
    sky.textured = 0;
    sky.tex_offset = 0.0f;
    sky.mov_speed = 0.0f;
    sky.mov_dir = 0;

    if (!map_raw || !json_truthy(cJSON_GetObjectItem(map_raw, "skyDome"))) return;

    if (!sky.program) {
        char *vert = concat(assets_path(), "shaders/sky.vert");
        char *frag = concat(assets_path(), "shaders/sky.frag");
        sky.program = shader_compile(vert, frag);
        free(vert);
        free(frag);
        if (!sky.program) return;
    }

    sky.stops[0] = sky_color_or_default(cJSON_GetObjectItem(map_raw, "skyDomeCol0"), 0);
    sky.stops[1] = sky_color_or_default(cJSON_GetObjectItem(map_raw, "skyDomeCol1"), 1);
    sky.stops[2] = sky_color_or_default(cJSON_GetObjectItem(map_raw, "skyDomeCol2"), 2);

    if (json_truthy(cJSON_GetObjectItem(map_raw, "skyDomeTex"))) {
        char *path = concat(assets_path(), "textures/sky_6.png");
        sky.texture = load_texture(path); // cached by path; owned by the texture cache
        free(path);
        sky.textured = sky.texture != 0;

        // movT: D != null && D / 1000000 ; movD: skyDomeMovD (0 / absent = U axis).
        const cJSON *mov_t = cJSON_GetObjectItem(map_raw, "skyDomeMovT");
        const cJSON *mov_d = cJSON_GetObjectItem(map_raw, "skyDomeMovD");
        if (cJSON_IsNumber(mov_t)) sky.mov_speed = (float) cJSON_GetNumberValue(mov_t) / 1000000.0f;
        if (cJSON_IsNumber(mov_d)) sky.mov_dir = (int) cJSON_GetNumberValue(mov_d);
    }

    sky_build_geometry();
    sky.enabled = sky.index_count > 0;
}

void sky_tick(const float delta) {
    // render.js render(): movTextures only advance while the textureAnim setting is on.
    if (!sky.enabled || !sky.textured || sky.mov_speed == 0.0f || !client_render_settings()->texture_anim) return;
    sky.tex_offset = fmodf(sky.tex_offset + sky.mov_speed * delta, 1.0f);
}

void sky_render(Camera *camera) {
    if (!sky.enabled) return;

    int viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    camera_update_projection_matrix(camera, (float) viewport[2] / (float) viewport[3]);
    camera_update_world_inverse_matrix(camera);

    const float transform[16] = {
        1.0f, 0.0f, 0.0f, camera->position.x,
        0.0f, 1.0f, 0.0f, camera->position.y,
        0.0f, 0.0f, 1.0f, camera->position.z,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    glUseProgram(sky.program);
    g_active_shader = sky.program;

    glUniformMatrix4fv(glGetUniformLocation(sky.program, "transform"), 1, GL_TRUE, transform);
    glUniformMatrix4fv(glGetUniformLocation(sky.program, "camera_world_inverse"), 1, GL_TRUE, camera->world_inverse_matrix);
    glUniformMatrix4fv(glGetUniformLocation(sky.program, "camera_projection"), 1, GL_TRUE, camera->projection_matrix);
    glUniform2f(glGetUniformLocation(sky.program, "tex_offset"),
        sky.mov_dir == 0 ? sky.tex_offset : 0.0f, sky.mov_dir == 0 ? 0.0f : sky.tex_offset);
    glUniform1i(glGetUniformLocation(sky.program, "use_texture"), sky.textured);
    glUniform1i(glGetUniformLocation(sky.program, "tex"), 0);

    if (sky.textured) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sky.texture);
        g_active_texture = sky.texture;
    }

    // BackSide + depthWrite:false + transparent (normal blending) over a cleared depth buffer.
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // the renderer-wide default (main.c)
#ifndef KN_GLES
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
#endif

    glBindVertexArray(sky.vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sky.ebo);
    glDrawElements(GL_TRIANGLES, sky.index_count, GL_UNSIGNED_INT, NULL);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

void sky_fini(void) {
    if (sky.vao) glDeleteVertexArrays(1, &sky.vao);
    if (sky.vbo) glDeleteBuffers(1, &sky.vbo);
    if (sky.ebo) glDeleteBuffers(1, &sky.ebo);
    if (sky.program) glDeleteProgram(sky.program);
    if (g_active_shader == sky.program) g_active_shader = 0;
    memset(&sky, 0, sizeof(sky));
}
