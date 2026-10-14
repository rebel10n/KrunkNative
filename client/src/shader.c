#include <gl.h>
#include <client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(KN_GLES) || defined(__APPLE__)
// The shader sources on disk are written for `#version 450 core`, restricted to
// the subset that is also valid GLSL ES 3.00 and GLSL 4.10. OpenGL ES contexts
// (KN_GLES: WebGL2 and ANGLE, gl.h) only accept GLSL ES (the mandatory default
// precisions are injected after the version line); macOS's desktop GL fallback
// (KRUNKNATIVE_GL_BACKEND=desktop) caps core profiles at OpenGL 4.1, so there the
// version line becomes `410 core`. The returned buffer replaces the original source.
#ifdef KN_GLES
static const char es_header[] = "#version 300 es\nprecision highp float; precision highp int; precision highp sampler2D;\n";
#else
static const char es_header[] = "#version 410 core\n";
#endif

static unsigned char *shader_source_for_webgl(unsigned char *source) {
    static const char version_prefix[] = "#version";

    if (strncmp((const char *) source, version_prefix, sizeof(version_prefix) - 1)) return source;

    const char *line_end = strchr((const char *) source, '\n');
    if (!line_end) return source;

    const char *rest = line_end + 1;
    const size_t rest_length = strlen(rest);

    unsigned char *rewritten = malloc(sizeof(es_header) - 1 + rest_length + 1);
    if (!rewritten) return source;

    memcpy(rewritten, es_header, sizeof(es_header) - 1);
    memcpy(rewritten + sizeof(es_header) - 1, rest, rest_length + 1);

    free(source);
    return rewritten;
}
#endif

#ifdef __EMSCRIPTEN__
// The renderer's shaders are compiled one by one at startup; on the web download the whole
// set in one parallel batch first instead of one blocking round trip per file.
static void shader_prefetch_all(void) {
    static int done;
    if (done) return;
    done = 1;

    static const char *const names[] = {
        "basic.vert", "basic.frag", "quad.vert", "quad.frag", "text.frag", "sky.vert", "sky.frag",
        "scene_composite.vert", "scene_composite.frag", "laced.frag", "shadow.vert", "shadow.frag", "vfx.vert", "vfx.frag",
        "line.vert", "line.frag", // LineMaterial (show_navmeshes): compiled later, from the frame loop
    };
    char *paths[sizeof(names) / sizeof(names[0])];
    size_t count = 0;

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char relative[64];
        snprintf(relative, sizeof(relative), "shaders/%s", names[i]);
        char *path = concat(assets_path(), relative);
        if (path) paths[count++] = path;
    }

    asset_prefetch_wait((const char *const *) paths, count);
    for (size_t i = 0; i < count; i++) free(paths[i]);
}
#endif

unsigned int shader_compile(const char *vertex_shader_path, const char *fragment_shader_path) {
#ifdef __EMSCRIPTEN__
    shader_prefetch_all();
#endif
    size_t vertex_shader_length;
    unsigned char *vertex_shader;

    size_t fragment_shader_length;
    unsigned char *fragment_shader;

    if (
        read_file(vertex_shader_path, &vertex_shader, &vertex_shader_length) ||
        read_file(fragment_shader_path, &fragment_shader, &fragment_shader_length)
    ) return 0;

#if defined(KN_GLES) || defined(__APPLE__)
    vertex_shader = shader_source_for_webgl(vertex_shader);
    fragment_shader = shader_source_for_webgl(fragment_shader);
#endif

    const unsigned int vert = glCreateShader(GL_VERTEX_SHADER);
    const unsigned int frag = glCreateShader(GL_FRAGMENT_SHADER);

    glShaderSource(vert, 1, (const char**) &vertex_shader, NULL);
    glShaderSource(frag, 1, (const char**) &fragment_shader, NULL);

    glCompileShader(vert);
    free(vertex_shader);

    int status;
    char error[512];

    glGetShaderiv(vert, GL_COMPILE_STATUS, &status);

    if (!status) {
        glGetShaderInfoLog(vert, sizeof(error), NULL, error);
        printf("vertex shader compile error (%s):\n %s \n", vertex_shader_path, error);

        glDeleteShader(vert);
        glDeleteShader(frag);
        free(fragment_shader);

        return 0;
    }

    glCompileShader(frag);
    free(fragment_shader);

    glGetShaderiv(frag, GL_COMPILE_STATUS, &status);

    if (!status) {
        glGetShaderInfoLog(frag, sizeof(error), NULL, error);
        printf("fragment shader compile error (%s):\n %s \n", fragment_shader_path, error);

        glDeleteShader(vert);
        glDeleteShader(frag);

        return 0;
    }

    const unsigned int program = glCreateProgram();

    glAttachShader(program, vert);
    glAttachShader(program, frag);

    glLinkProgram(program);

    glDeleteShader(vert);
    glDeleteShader(frag);

    glGetProgramiv(program, GL_LINK_STATUS, &status);

    if (!status) {
        glGetProgramInfoLog(program, sizeof(error), NULL, error);
        printf("shader link error (%s, %s):\n %s \n", vertex_shader_path, fragment_shader_path, error);

        glDeleteProgram(program);
        return 0;
    }

    gl_uniform_forget_program(program); // GL may hand out the id of a deleted program again
    return program;
}

// ---- uniform cache (client.h GlUniform)

struct GlUniformEntry {
    unsigned int program;
    int location;
    unsigned char resolved;  // location looked up for the current link of `program`
    unsigned char has_value; // `value` mirrors what the program holds
    float value[17];         // ints are stored bit-for-bit; matrices carry the transpose flag last
    char name[];
};

static GlUniformEntry **g_uniform_entries;
static size_t g_uniform_entry_count, g_uniform_entry_capacity;

static GlUniformEntry *gl_uniform_entry(const unsigned int program, const char *name) {
    for (size_t i = 0; i < g_uniform_entry_count; i++) {
        GlUniformEntry *entry = g_uniform_entries[i];
        if (entry->program == program && !strcmp(entry->name, name)) return entry;
    }

    if (g_uniform_entry_count == g_uniform_entry_capacity) {
        const size_t capacity = g_uniform_entry_capacity ? g_uniform_entry_capacity * 2 : 64;
        GlUniformEntry **grown = realloc(g_uniform_entries, capacity * sizeof(*grown));
        if (!grown) return NULL;
        g_uniform_entries = grown;
        g_uniform_entry_capacity = capacity;
    }

    const size_t name_length = strlen(name);
    GlUniformEntry *entry = calloc(1, sizeof(GlUniformEntry) + name_length + 1);
    if (!entry) return NULL;

    entry->program = program;
    memcpy(entry->name, name, name_length + 1);
    g_uniform_entries[g_uniform_entry_count++] = entry;
    return entry;
}

// Entry for (program, uniform->name) with a resolved location; NULL when the uniform is
// absent (or out of memory), in which case nothing must be uploaded.
static GlUniformEntry *gl_uniform_resolve(GlUniform *uniform, const unsigned int program) {
    if (!program || !uniform->name) return NULL;
    if (uniform->program != program || !uniform->entry) {
        uniform->entry = gl_uniform_entry(program, uniform->name);
        uniform->program = uniform->entry ? program : 0;
        if (!uniform->entry) return NULL;
    }

    GlUniformEntry *entry = uniform->entry;
    if (!entry->resolved) {
        entry->location = glGetUniformLocation(program, entry->name);
        entry->resolved = 1;
        entry->has_value = 0;
    }

    return entry->location < 0 ? NULL : entry;
}

// 1 when `value` differs from the cached one (and records it); 0 when the upload can be skipped.
static int gl_uniform_changed(GlUniformEntry *entry, const float *value, const size_t count) {
    if (entry->has_value && !memcmp(entry->value, value, count * sizeof(float))) return 0;
    memcpy(entry->value, value, count * sizeof(float));
    entry->has_value = 1;
    return 1;
}

int gl_uniform_location(GlUniform *uniform, const unsigned int program) {
    const GlUniformEntry *entry = gl_uniform_resolve(uniform, program);
    return entry ? entry->location : -1;
}

void gl_uniform_1i(GlUniform *uniform, const unsigned int program, const int v) {
    GlUniformEntry *entry = gl_uniform_resolve(uniform, program);
    if (!entry) return;
    float bits;
    memcpy(&bits, &v, sizeof(bits));
    if (gl_uniform_changed(entry, &bits, 1)) glUniform1i(entry->location, v);
}

void gl_uniform_1f(GlUniform *uniform, const unsigned int program, const float x) {
    GlUniformEntry *entry = gl_uniform_resolve(uniform, program);
    if (entry && gl_uniform_changed(entry, &x, 1)) glUniform1f(entry->location, x);
}

void gl_uniform_2f(GlUniform *uniform, const unsigned int program, const float x, const float y) {
    GlUniformEntry *entry = gl_uniform_resolve(uniform, program);
    const float v[2] = {x, y};
    if (entry && gl_uniform_changed(entry, v, 2)) glUniform2f(entry->location, x, y);
}

void gl_uniform_3f(GlUniform *uniform, const unsigned int program, const float x, const float y, const float z) {
    GlUniformEntry *entry = gl_uniform_resolve(uniform, program);
    const float v[3] = {x, y, z};
    if (entry && gl_uniform_changed(entry, v, 3)) glUniform3f(entry->location, x, y, z);
}

void gl_uniform_4f(GlUniform *uniform, const unsigned int program, const float x, const float y, const float z, const float w) {
    GlUniformEntry *entry = gl_uniform_resolve(uniform, program);
    const float v[4] = {x, y, z, w};
    if (entry && gl_uniform_changed(entry, v, 4)) glUniform4f(entry->location, x, y, z, w);
}

// The transpose flag is part of the cached value (a plain and a transposed upload differ).
void gl_uniform_mat3(GlUniform *uniform, const unsigned int program, const int transpose, const float *m) {
    GlUniformEntry *entry = gl_uniform_resolve(uniform, program);
    if (!entry) return;
    float v[10];
    memcpy(v, m, 9 * sizeof(float));
    v[9] = transpose ? 1.0f : 0.0f;
    if (gl_uniform_changed(entry, v, 10)) glUniformMatrix3fv(entry->location, 1, transpose ? GL_TRUE : GL_FALSE, m);
}

void gl_uniform_mat4(GlUniform *uniform, const unsigned int program, const int transpose, const float *m) {
    GlUniformEntry *entry = gl_uniform_resolve(uniform, program);
    if (!entry) return;
    float v[17];
    memcpy(v, m, 16 * sizeof(float));
    v[16] = transpose ? 1.0f : 0.0f;
    if (gl_uniform_changed(entry, v, 17)) glUniformMatrix4fv(entry->location, 1, transpose ? GL_TRUE : GL_FALSE, m);
}

void gl_uniform_forget_program(const unsigned int program) {
    for (size_t i = 0; i < g_uniform_entry_count; i++) {
        GlUniformEntry *entry = g_uniform_entries[i];
        if (entry->program != program) continue;
        entry->resolved = 0;
        entry->has_value = 0;
    }
}

void material_update_uniforms(Material *material) {
    material->vtable->update_uniforms(material);
}

void material_fini(Material *material) {
    free(material);
}