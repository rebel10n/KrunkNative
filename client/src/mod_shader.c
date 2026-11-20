// Mod post-processing passes (client/include/mod_shader.h): Krunker v10.0.7's insertShaderPass /
// applyShaderInput / resetShaderPasses (deob-10.0.7.js:205848-205949), setShaderInput (deob:216668-216673) and
// the postfx chain they feed (deob:195864-196010): each enabled pass draws a full-screen triangle into one of two
// RGBA8 ping-pong targets, sampling the previous one (the scene first). A pass whose GLSL does not compile or
// link is skipped, as postfx disables a pass whose build throws and copies its input instead (deob:195964-195966).
#include <gl.h>
#include <mod_shader.h>
#include <client.h>
#include <cJSON.h>
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    ModShaderProgram prog; // the translated graph (fragment source and uniforms)
    unsigned int program;  // 0 until first drawn
    int failed;            // compile / link refused: the pass draws nothing (Krunker's copy fallback)
    int loc_input, loc_resolution, loc_time, loc_delta, loc_dpr;
    int loc_u[MOD_SHADER_MAX_UNIFORMS];
    int enabled;
    ModShaderInput inputs[MOD_SHADER_MAX_INPUTS];
    int input_count;
} ModShaderPass;

static struct {
    ModShaderPass *passes[MOD_SHADER_MAX_PASSES];
    int count;
    unsigned int vao;
    unsigned int tex[2], fbo[2];
    int width, height;
    double last_time;
} g_ms;

#ifdef KN_GLES
static const char k_header[] = "#version 300 es\nprecision highp float; precision highp int; precision highp sampler2D;\n";
#elif defined(__APPLE__)
static const char k_header[] = "#version 410 core\n";
#else
static const char k_header[] = "#version 450 core\n";
#endif

// the full-screen triangle from gl_VertexID (as scene_composite.vert): no vertex buffers
static const char k_vertex[] =
    "void main() {\n"
    "  vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

// ---------------------------------------------------------------- the pass list

// applyShaderInput (deob:205932-205941): row 0 switches the pass, the others set the uniform of their name
static void apply_input(ModShaderPass *p, const int index) {
    const ModShaderInput *in = &p->inputs[index];
    if (index == 0) {
        p->enabled = in->val != 0.0f;
        return;
    }
    const float v = in->toggle ? (in->val != 0.0f ? 1.0f : 0.0f) : in->val;
    for (int u = 0; u < p->prog.uniform_count; u++)
        if (!strcmp(p->prog.uniforms[u].name, in->name)) p->prog.uniforms[u].value[0] = v;
    // (a param the output does not reach has no uniform: Krunker parks the value in `pending`, which no build uses)
}

int mod_shader_insert(const cJSON *json) {
    if (g_ms.count >= MOD_SHADER_MAX_PASSES || !cJSON_IsObject(json)) return 0;
    // {graph, shaderArgs} or a bare post graph
    const cJSON *graph = cJSON_GetObjectItemCaseSensitive(json, "graph");
    if (!graph || cJSON_IsNull(graph) || cJSON_IsFalse(graph)) graph = json;
    if (!cJSON_IsObject(graph)) return 0;
    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(graph, "kind"));
    if (!kind || strcmp(kind, "post")) return 0;
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(json, "shaderArgs");
    const cJSON *params = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive(args, "params") : NULL;

    ModShaderPass *p = calloc(1, sizeof(*p));
    if (!p) return 0;
    char err[256] = "";
    if (!mod_shader_graph_compile(graph, params, &p->prog, err, sizeof(err))) {
        printf("[mod] shader pass refused: %s\n", err[0] ? err : "invalid graph");
        free(p);
        return 0;
    }
    // shaderInputs: the switch, then a slider per float param (a "bool" type would be a switch; vecN / color: none)
    ModShaderInput *row = &p->inputs[p->input_count++];
    snprintf(row->name, sizeof(row->name), "Shader Mod # %d", g_ms.count + 1);
    row->toggle = 1;
    row->val = 1.0f;
    for (int i = 0; i < p->prog.param_count && p->input_count < MOD_SHADER_MAX_INPUTS; i++) {
        const ModShaderParam *mp = &p->prog.params[i];
        if (!strcmp(mp->type, "bool")) {
            row = &p->inputs[p->input_count++];
            memset(row, 0, sizeof(*row));
            snprintf(row->name, sizeof(row->name), "%s", mp->name);
            row->toggle = 1;
            row->val = mp->has_default && mp->def != 0.0 ? 1.0f : 0.0f;
        } else if (!strcmp(mp->type, "float") || !strcmp(mp->type, "number")) {
            row = &p->inputs[p->input_count++];
            memset(row, 0, sizeof(*row));
            snprintf(row->name, sizeof(row->name), "%s", mp->name);
            row->min = mp->has_min ? (float) mp->min : 0.0f;  // parseFloat(min) || 0
            row->max = mp->has_max && mp->max != 0.0 ? (float) mp->max : 1.0f; // parseFloat(max) || 1
            row->step = 0.01f;
            row->val = mp->has_default ? (float) mp->def : 0.0f;
        }
    }
    g_ms.passes[g_ms.count++] = p;
    for (int i = 0; i < p->input_count; i++) apply_input(p, i); // (the rows' values win over shaderArgs)
    printf("[mod] shader pass %d added (%d uniforms, %d inputs)\n", g_ms.count, p->prog.uniform_count, p->input_count);
    return 1;
}

int mod_shader_insert_json(const char *text, const size_t len) {
    if (!text) return 0;
    cJSON *json = cJSON_ParseWithLength(text, len);
    if (!json) {
        printf("[mod] shader entry is not JSON\n");
        return 0;
    }
    const int ok = mod_shader_insert(json);
    cJSON_Delete(json);
    return ok;
}

static void pass_free(ModShaderPass *p) {
    if (!p) return;
    if (p->program) {
        if (g_active_shader == p->program) g_active_shader = 0;
        glDeleteProgram(p->program);
    }
    mod_shader_program_free(&p->prog);
    free(p);
}

void mod_shader_reset(void) {
    for (int i = 0; i < g_ms.count; i++) pass_free(g_ms.passes[i]);
    if (g_ms.count) printf("[mod] shader passes reset\n");
    g_ms.count = 0;
}

int mod_shader_count(void) { return g_ms.count; }

int mod_shader_input_count(const int pass) {
    return pass >= 0 && pass < g_ms.count ? g_ms.passes[pass]->input_count : 0;
}

const ModShaderInput *mod_shader_input(const int pass, const int index) {
    if (pass < 0 || pass >= g_ms.count || index < 0 || index >= g_ms.passes[pass]->input_count) return NULL;
    return &g_ms.passes[pass]->inputs[index];
}

// setShaderInput (deob:216668-216673)
void mod_shader_set_input(const int pass, const int index, const char *value) {
    if (pass < 0 || pass >= g_ms.count) return;
    ModShaderPass *p = g_ms.passes[pass];
    if (index < 0 || index >= p->input_count) return;
    ModShaderInput *in = &p->inputs[index];
    const char *v = value ? value : "";
    float f;
    if (in->toggle) f = strcmp(v, "false") && *v ? 1.0f : 0.0f; // ~~!!v ("false" -> false; any other text is truthy)
    else if (!strcmp(v, "true")) f = 1.0f;
    else if (!strcmp(v, "false")) f = 0.0f;
    else {
        char *end;
        const double d = strtod(v, &end);
        f = end != v ? (float) d : 0.0f; // (Number("") is 0; other text, NaN there, reads 0 here)
    }
    if (!in->toggle && (in->min != 0.0f || in->max != 0.0f)) {
        if (f < in->min) f = in->min;
        if (f > in->max) f = in->max;
    }
    in->val = f;
    apply_input(p, index);
}

int mod_shader_active(void) {
    for (int i = 0; i < g_ms.count; i++)
        if (g_ms.passes[i]->enabled && !g_ms.passes[i]->failed) return 1;
    return 0;
}

// ---------------------------------------------------------------- GL

static unsigned int compile_stage(const unsigned int type, const char *body) {
    const unsigned int shader = glCreateShader(type);
    const char *parts[2] = {k_header, body};
    glShaderSource(shader, 2, parts, NULL);
    glCompileShader(shader);
    int status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        printf("[mod] shader pass compile error:\n%s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static int pass_program(ModShaderPass *p) {
    if (p->program) return 1;
    if (p->failed) return 0;
    const unsigned int vert = compile_stage(GL_VERTEX_SHADER, k_vertex);
    const unsigned int frag = vert ? compile_stage(GL_FRAGMENT_SHADER, p->prog.fragment) : 0;
    if (vert && frag) {
        p->program = glCreateProgram();
        glAttachShader(p->program, vert);
        glAttachShader(p->program, frag);
        glLinkProgram(p->program);
        int status = 0;
        glGetProgramiv(p->program, GL_LINK_STATUS, &status);
        if (!status) {
            char log[1024];
            glGetProgramInfoLog(p->program, sizeof(log), NULL, log);
            printf("[mod] shader pass link error:\n%s\n", log);
            glDeleteProgram(p->program);
            p->program = 0;
        }
    }
    if (vert) glDeleteShader(vert);
    if (frag) glDeleteShader(frag);
    if (!p->program) {
        p->failed = 1;
        return 0;
    }
    p->loc_input = glGetUniformLocation(p->program, "kn_input");
    p->loc_resolution = glGetUniformLocation(p->program, "kn_resolution");
    p->loc_time = glGetUniformLocation(p->program, "kn_time");
    p->loc_delta = glGetUniformLocation(p->program, "kn_delta");
    p->loc_dpr = glGetUniformLocation(p->program, "kn_dpr");
    for (int u = 0; u < p->prog.uniform_count; u++) {
        char name[16];
        snprintf(name, sizeof(name), "u_p%d", u);
        p->loc_u[u] = glGetUniformLocation(p->program, name);
    }
    return 1;
}

static void delete_targets(void) {
    for (int i = 0; i < 2; i++) {
        if (g_ms.fbo[i]) glDeleteFramebuffers(1, &g_ms.fbo[i]);
        if (g_ms.tex[i]) {
            gl_release_texture(g_ms.tex[i]);
            glDeleteTextures(1, &g_ms.tex[i]);
        }
        g_ms.fbo[i] = g_ms.tex[i] = 0;
    }
    g_ms.width = g_ms.height = 0;
}

// the two ping-pong targets at the scene's size (postfx.targets, setSize per frame: deob:195896)
static int allocate(const int width, const int height) {
    if (g_ms.tex[0] && g_ms.width == width && g_ms.height == height) return 1;
    delete_targets();
    for (int i = 0; i < 2; i++) {
        glGenTextures(1, &g_ms.tex[i]);
        glBindTexture(GL_TEXTURE_2D, g_ms.tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &g_ms.fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, g_ms.fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_ms.tex[i], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "[mod] %dx%d shader pass target incomplete, passes skipped\n", width, height);
            glBindTexture(GL_TEXTURE_2D, 0);
            delete_targets();
            return 0;
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    g_ms.width = width;
    g_ms.height = height;
    return 1;
}

unsigned int mod_shader_render(const unsigned int scene_tex, const int width, const int height) {
    if (!mod_shader_active() || width <= 0 || height <= 0) {
        if (g_ms.tex[0] && !g_ms.count) delete_targets(); // no passes left: release the buffers
        return scene_tex;
    }
    if (!allocate(width, height)) return scene_tex;
    if (!g_ms.vao) glGenVertexArrays(1, &g_ms.vao);
    const double now = glfwGetTime();
    const float delta = g_ms.last_time > 0.0 && now > g_ms.last_time ? (float) (now - g_ms.last_time) : 0.0f;
    g_ms.last_time = now;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(g_ms.vao);
    glActiveTexture(GL_TEXTURE0);
    unsigned int source = scene_tex;
    int target = 0;
    for (int i = 0; i < g_ms.count; i++) {
        ModShaderPass *p = g_ms.passes[i];
        if (!p->enabled || !pass_program(p)) continue;
        glBindTexture(GL_TEXTURE_2D, 0); // the target must not be bound for sampling while it is drawn into
        glBindFramebuffer(GL_FRAMEBUFFER, g_ms.fbo[target]);
        glViewport(0, 0, width, height);
        glUseProgram(p->program);
        g_active_shader = p->program;
        glBindTexture(GL_TEXTURE_2D, source);
        glUniform1i(p->loc_input, 0);
        glUniform2f(p->loc_resolution, (float) width, (float) height);
        glUniform1f(p->loc_time, (float) now);
        glUniform1f(p->loc_delta, delta);
        glUniform1f(p->loc_dpr, 1.0f);
        for (int u = 0; u < p->prog.uniform_count; u++) {
            const ModShaderUniform *mu = &p->prog.uniforms[u];
            switch (mu->width) {
                case 1: glUniform1f(p->loc_u[u], mu->value[0]); break;
                case 2: glUniform2fv(p->loc_u[u], 1, mu->value); break;
                case 3: glUniform3fv(p->loc_u[u], 1, mu->value); break;
                default: glUniform4fv(p->loc_u[u], 1, mu->value); break;
            }
        }
        glDrawArrays(GL_TRIANGLES, 0, 3);
        source = g_ms.tex[target];
        target ^= 1;
    }
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    g_active_texture = 0;
    return source;
}

void mod_shader_fini(void) {
    for (int i = 0; i < g_ms.count; i++) {
        ModShaderPass *p = g_ms.passes[i];
        if (p->program) glDeleteProgram(p->program);
        p->program = 0;
        p->failed = 0;
    }
    delete_targets();
    if (g_ms.vao) glDeleteVertexArrays(1, &g_ms.vao);
    g_ms.vao = 0;
    g_active_shader = 0;
}
