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

// `#include "name.glsl"` at the start of a line pulls in assets/shaders/name.glsl (the shader's own
// directory, one level, no nesting: an included file's includes are left as they are and fail to
// compile, which is the signal). The ray tracer's rt_*.frag share their traversal through
// rt_common.glsl this way. Returns a new buffer (the source is freed) or the source unchanged.
static unsigned char *shader_resolve_includes(unsigned char *source, const char *shader_path) {
    static const char directive[] = "#include \"";
    const char *at = strstr((const char *) source, directive);
    if (!at) return source;

    const char *slash = strrchr(shader_path, '/');
    const size_t dir_length = slash ? (size_t) (slash - shader_path + 1) : 0;

    size_t out_cap = strlen((const char *) source) + 1, out_length = 0;
    unsigned char *out = malloc(out_cap);
    if (!out) return source;
    const char *cursor = (const char *) source;
    while (at) {
        if (at != (const char *) source && at[-1] != '\n') { // only at the start of a line
            at = strstr(at + 1, directive);
            continue;
        }
        const char *name = at + sizeof(directive) - 1;
        const char *name_end = strchr(name, '"');
        const char *line_end = strchr(at, '\n');
        if (!name_end || !line_end || name_end > line_end) break;

        char *path = malloc(dir_length + (size_t) (name_end - name) + 1);
        if (!path) break;
        memcpy(path, shader_path, dir_length);
        memcpy(path + dir_length, name, (size_t) (name_end - name));
        path[dir_length + (size_t) (name_end - name)] = 0;
        unsigned char *included = NULL;
        size_t included_length = 0;
        if (read_file(path, &included, &included_length)) printf("shader include not found: %s (in %s)\n", path, shader_path);
        free(path);

        const size_t before = (size_t) (at - cursor);
        const size_t need = out_length + before + included_length + 2;
        if (need > out_cap) {
            unsigned char *grown = realloc(out, need);
            if (!grown) { free(included); break; }
            out = grown;
            out_cap = need;
        }
        memcpy(out + out_length, cursor, before);
        out_length += before;
        if (included) {
            memcpy(out + out_length, included, included_length);
            out_length += included_length;
            free(included);
        }
        out[out_length++] = '\n';
        cursor = line_end + 1;
        at = strstr(cursor, directive);
    }
    const size_t rest = strlen(cursor);
    unsigned char *grown = realloc(out, out_length + rest + 1);
    if (!grown) { free(out); return source; }
    out = grown;
    memcpy(out + out_length, cursor, rest + 1);
    free(source);
    return out;
}

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
        // Box Office ray tracing (rt.h): compiled when rtMode is first turned on
        "rt_common.glsl", "rt_prepass.vert", "rt_prepass.frag", "rt_trace.frag", "rt_denoise.frag", "rt_debug.frag",
        "rt_reflect.frag", "rt_refl_denoise.frag", "rt_atlas.frag", // reflections (phase 4)
        "rt_taa.frag", // temporal anti-aliasing (render_target.c)
        "rt_bloom.frag", // bloom (rtBloom, render_target.c)
        "rt_upscale.frag", "rt_sharpen.frag", // edge-adaptive upscale + sharpening (rtSharpen, render_target.c)
        "rt_gi.frag", "rt_gi_denoise.frag", // one-bounce GI
        "rt_shafts.frag", "rt_shafts_denoise.frag", "rt_shafts_composite.frag", // volumetric sun shafts (rtShafts)
        "rt_sky.glsl", "rt_sky.frag", // physical sky (rtSky; basic.frag includes rt_sky.glsl too)
        "rt_grade.glsl", // Cinematic grade (scene_composite.frag / rt_sharpen.frag include it)
        "rt_mask_classify.frag", "rt_mask_filter.frag", // per-weapon part masks (rt_masks.c)
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

// Inserts `defines` (whole lines, e.g. "#define KN_RT 0\n") right after the version line of `source`, which
// must stay the first line. Returns a new buffer (the source is freed) or the source unchanged.
static unsigned char *shader_inject_defines(unsigned char *source, const char *defines) {
    if (!defines || !*defines) return source;
    const char *line_end = strchr((const char *) source, '\n');
    if (!line_end) return source;
    const size_t head = (size_t) (line_end - (const char *) source) + 1;
    const size_t length = strlen((const char *) source), extra = strlen(defines);
    unsigned char *out = malloc(length + extra + 1);
    if (!out) return source;
    memcpy(out, source, head);
    memcpy(out + head, defines, extra);
    memcpy(out + head + extra, source + head, length - head + 1);
    free(source);
    return out;
}

unsigned int shader_compile(const char *vertex_shader_path, const char *fragment_shader_path) {
    return shader_compile_defines(vertex_shader_path, fragment_shader_path, NULL);
}

// shader_compile with `defines` injected after the version line of both stages (compile-time variants of one
// source: materials/basic.c's KN_RT programs).
unsigned int shader_compile_defines(const char *vertex_shader_path, const char *fragment_shader_path, const char *defines) {
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

    vertex_shader = shader_resolve_includes(vertex_shader, vertex_shader_path);
    fragment_shader = shader_resolve_includes(fragment_shader, fragment_shader_path);
#if defined(KN_GLES) || defined(__APPLE__)
    vertex_shader = shader_source_for_webgl(vertex_shader);
    fragment_shader = shader_source_for_webgl(fragment_shader);
#endif
    vertex_shader = shader_inject_defines(vertex_shader, defines);
    fragment_shader = shader_inject_defines(fragment_shader, defines);

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

static unsigned int g_uniform_epoch;

unsigned int gl_uniform_epoch(void) {
    return g_uniform_epoch;
}

void gl_uniform_forget_program(const unsigned int program) {
    g_uniform_epoch++; // memos of what a program holds (basic.c, scene.c) start over
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