// mod_shader_test: the shader-graph layer of mod post passes and skin surface graphs (client/src/mod_shader_graph.c, no GL).
//   - the validator's refusals (deob-10.0.7.js:94463-94792) and a valid greyscale / invert graph;
//   - every node type of the table, unwired, feeding the output from each of its ports, translates;
//   - a surface graph's output ports and settings (deob:94197-94241, the material :95813-95847), its two stages sharing
//     a param's uniform, and every node type allowed in a surface graph feeding its colour;
//   - `--dump <dir>` writes those shaders (with a GLSL ES 3.00 header): <dir>/*.frag the post passes,
//     <dir>/surface/*.vert / *.frag the surface ones, for tools/modshader/check_glsl.sh and check_gl.py.
#include "../src/mod_shader_graph.c"

static int g_failures;

#define CHECK(cond, ...)                                                                                                    \
    do {                                                                                                                    \
        if (!(cond)) {                                                                                                      \
            g_failures++;                                                                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                     \
            printf(__VA_ARGS__);                                                                                            \
            printf("\n");                                                                                                   \
        }                                                                                                                   \
    } while (0)

static int valid(const char *json, char *err, const size_t n) {
    cJSON *g = cJSON_Parse(json);
    if (!g) {
        snprintf(err, n, "(test JSON does not parse)");
        return -1;
    }
    const int ok = mod_shader_graph_validate(g, "post", err, n);
    cJSON_Delete(g);
    return ok;
}

static void expect_refused(const char *json, const char *needle) {
    char err[256] = "";
    const int ok = valid(json, err, sizeof(err));
    CHECK(ok == 0, "accepted: %s", json);
    CHECK(!needle || strstr(err, needle), "error \"%s\" lacks \"%s\" for %s", err, needle, json);
}

static const char k_grey[] =
    "{\"v\":1,\"kind\":\"post\",\"nodes\":["
    "{\"id\":\"s\",\"type\":\"sceneColor\"},"
    "{\"id\":\"g\",\"type\":\"grayscale\",\"inputs\":{\"c\":{\"node\":\"s\"}}},"
    "{\"id\":\"amt\",\"type\":\"param\",\"params\":{\"name\":\"amount\",\"default\":2,\"min\":0,\"max\":1}},"
    "{\"id\":\"m\",\"type\":\"mix\",\"inputs\":{\"a\":{\"node\":\"s\"},\"b\":{\"node\":\"g\"},\"t\":{\"node\":\"amt\"}}},"
    "{\"id\":\"out\",\"type\":\"output\",\"inputs\":{\"color\":{\"node\":\"m\"}}}],\"output\":\"out\"}";

static void test_validation(void) {
    char err[256];
    CHECK(valid(k_grey, err, sizeof(err)) == 1, "greyscale refused: %s", err);
    expect_refused("[]", "object");
    expect_refused("{\"v\":2,\"kind\":\"post\",\"nodes\":[{\"id\":\"o\",\"type\":\"output\"}],\"output\":\"o\"}", "v must be 1");
    expect_refused("{\"v\":1,\"kind\":\"surface\",\"nodes\":[{\"id\":\"o\",\"type\":\"output\"}],\"output\":\"o\"}", "kind must be post");
    expect_refused("{\"v\":1,\"kind\":\"post\",\"nodes\":[],\"output\":\"o\"}", "no nodes");
    expect_refused("{\"v\":1,\"kind\":\"post\",\"extra\":1,\"nodes\":[{\"id\":\"o\",\"type\":\"output\"}],\"output\":\"o\"}", "unknown graph key");
    expect_refused("{\"v\":1,\"kind\":\"post\",\"nodes\":[{\"id\":\"o\",\"type\":\"nope\"},{\"id\":\"p\",\"type\":\"output\"}],\"output\":\"p\"}",
                   "unknown node type");
    expect_refused("{\"v\":1,\"kind\":\"post\",\"nodes\":[{\"id\":\"a b\",\"type\":\"output\"}],\"output\":\"a b\"}", "id must be");
    expect_refused("{\"v\":1,\"kind\":\"post\",\"nodes\":[{\"id\":\"o\",\"type\":\"output\"},{\"id\":\"o\",\"type\":\"output\"}],\"output\":\"o\"}",
                   "duplicate node id");
    expect_refused("{\"v\":1,\"kind\":\"post\",\"nodes\":[{\"id\":\"o\",\"type\":\"output\"},{\"id\":\"p\",\"type\":\"output\"}],\"output\":\"o\"}",
                   "exactly one output");
    expect_refused("{\"v\":1,\"kind\":\"post\",\"nodes\":[{\"id\":\"f\",\"type\":\"float\"}],\"output\":\"f\"}", "exactly one output");
    expect_refused("{\"v\":1,\"kind\":\"post\",\"nodes\":["
                   "{\"id\":\"a\",\"type\":\"abs\",\"inputs\":{\"x\":{\"node\":\"b\"}}},{\"id\":\"b\",\"type\":\"abs\",\"inputs\":{\"x\":{\"node\":\"a\"}}},"
                   "{\"id\":\"o\",\"type\":\"output\",\"inputs\":{\"color\":{\"node\":\"a\"}}}],\"output\":\"o\"}",
                   "cycle");
    expect_refused("{\"v\":1,\"kind\":\"post\",\"nodes\":[{\"id\":\"a\",\"type\":\"abs\",\"inputs\":{\"x\":{\"node\":\"a\"}}},"
                   "{\"id\":\"o\",\"type\":\"output\"}],\"output\":\"o\"}",
                   "itself");
    expect_refused("{\"v\":1,\"kind\":\"post\",\"nodes\":[{\"id\":\"a\",\"type\":\"abs\",\"inputs\":{\"x\":{\"value\":1,\"port\":\"out\"}}},"
                   "{\"id\":\"o\",\"type\":\"output\"}],\"output\":\"o\"}",
                   "cannot have a port");
    expect_refused("{\"v\":1,\"kind\":\"post\",\"nodes\":[{\"id\":\"a\",\"type\":\"abs\",\"inputs\":{\"y\":{\"value\":1}}},"
                   "{\"id\":\"o\",\"type\":\"output\"}],\"output\":\"o\"}",
                   "unknown input port");
    expect_refused("{\"v\":1,\"kind\":\"post\",\"nodes\":[{\"id\":\"a\",\"type\":\"abs\"},"
                   "{\"id\":\"o\",\"type\":\"output\",\"inputs\":{\"color\":{\"node\":\"a\",\"port\":\"rgb\"}}}],\"output\":\"o\"}",
                   "unknown output port");
    expect_refused("{\"v\":1,\"kind\":\"post\",\"nodes\":[{\"id\":\"p\",\"type\":\"param\",\"params\":{\"name\":\"9x\"}},"
                   "{\"id\":\"o\",\"type\":\"output\"}],\"output\":\"o\"}",
                   "not a valid name");
    expect_refused("{\"v\":1,\"kind\":\"post\",\"nodes\":[{\"id\":\"p\",\"type\":\"param\",\"params\":{\"name\":\"constructor\"}},"
                   "{\"id\":\"o\",\"type\":\"output\"}],\"output\":\"o\"}",
                   "reserved");
    expect_refused("{\"v\":1,\"kind\":\"post\",\"nodes\":[{\"id\":\"p\",\"type\":\"param\",\"params\":{\"name\":\"a\",\"min\":2,\"max\":1}},"
                   "{\"id\":\"o\",\"type\":\"output\"}],\"output\":\"o\"}",
                   "min must be <= max");
    expect_refused("{\"v\":1,\"kind\":\"post\",\"nodes\":[{\"id\":\"n\",\"type\":\"fbm\",\"params\":{\"octaves\":99}},"
                   "{\"id\":\"o\",\"type\":\"output\"}],\"output\":\"o\"}",
                   "octaves");
    expect_refused("{\"v\":1,\"kind\":\"post\",\"nodes\":[{\"id\":\"s\",\"type\":\"swizzle\",\"params\":{\"mask\":\"rgb\"}},"
                   "{\"id\":\"o\",\"type\":\"output\"}],\"output\":\"o\"}",
                   NULL);
    expect_refused("{\"v\":1,\"kind\":\"post\",\"nodes\":[{\"id\":\"o\",\"type\":\"output\",\"x\":\"left\"}],\"output\":\"o\"}", "x must be");
}

static int valid_kind(const char *json, const char *kind, char *err, const size_t n) {
    cJSON *g = cJSON_Parse(json);
    const int ok = g ? mod_shader_graph_validate(g, kind, err, n) : -1;
    cJSON_Delete(g);
    return ok;
}

// the output node per kind (iíîíîií, deob:94230-94253; the validator's port / param lists, :94307 and :94381)
static void test_surface_validation(void) {
    char err[256] = "";
    static const char lit[] = "{\"v\":1,\"kind\":\"surface\",\"nodes\":[{\"id\":\"c\",\"type\":\"color\",\"params\":{\"value\":\"#ff0000\"}},"
                              "{\"id\":\"o\",\"type\":\"output\",\"params\":{\"lit\":true,\"side\":\"front\",\"blend\":\"additive\","
                              "\"depthWrite\":false,\"transparent\":false},\"inputs\":{\"color\":{\"node\":\"c\"},\"emissive\":{\"node\":\"c\"},"
                              "\"alpha\":{\"value\":0.5}}}],\"output\":\"o\"}";
    CHECK(valid_kind(lit, "surface", err, sizeof(err)) == 1, "a lit surface graph refused: %s", err);
    CHECK(valid_kind(lit, "post", err, sizeof(err)) == 0 && strstr(err, "kind must be post"), "a surface graph passed as post: %s", err);
    err[0] = 0;
    CHECK(valid_kind("{\"v\":1,\"kind\":\"post\",\"nodes\":[{\"id\":\"o\",\"type\":\"output\",\"inputs\":{\"alpha\":{\"value\":1}}}],"
                     "\"output\":\"o\"}", "post", err, sizeof(err)) == 0 && strstr(err, "unknown input port"),
          "a post output with alpha: %s", err);
    err[0] = 0;
    CHECK(valid_kind("{\"v\":1,\"kind\":\"post\",\"nodes\":[{\"id\":\"o\",\"type\":\"output\",\"params\":{\"lit\":true}}],"
                     "\"output\":\"o\"}", "post", err, sizeof(err)) == 0 && strstr(err, "unknown param"),
          "a post output with lit: %s", err);
    err[0] = 0;
    CHECK(valid_kind("{\"v\":1,\"kind\":\"paint\",\"nodes\":[{\"id\":\"o\",\"type\":\"output\",\"inputs\":{\"emissive\":{\"value\":1}}}],"
                     "\"output\":\"o\"}", "paint", err, sizeof(err)) == 0 && strstr(err, "unknown input port"),
          "a paint output with emissive: %s", err);
    err[0] = 0;
    CHECK(valid_kind("{\"v\":1,\"kind\":\"surface\",\"nodes\":[{\"id\":\"s\",\"type\":\"sceneColor\"},"
                     "{\"id\":\"o\",\"type\":\"output\",\"inputs\":{\"color\":{\"node\":\"s\"}}}],\"output\":\"o\"}", "surface", err,
                     sizeof(err)) == 0 && strstr(err, "only allowed in post"),
          "sceneColor in a surface graph: %s", err);
    err[0] = 0;
    CHECK(valid_kind("{\"v\":1,\"kind\":\"surface\",\"nodes\":[{\"id\":\"o\",\"type\":\"output\",\"params\":{\"side\":\"back\"}}],"
                     "\"output\":\"o\"}", "surface", err, sizeof(err)) == 0 && strstr(err, "param \"side\""),
          "side back: %s", err);
}

// the material a surface graph makes (deob:95813-95847)
static void test_surface_compile(void) {
    char err[256] = "";
    ModShaderProgram prog;
    // v10's default surface graph shape: the map times a colour param, a wobble on positionOffset driven by the same param
    // node as the colour's strength, emissive into an unlit colour
    cJSON *g = cJSON_Parse(
        "{\"v\":1,\"kind\":\"surface\",\"nodes\":["
        "{\"id\":\"tex\",\"type\":\"texture\"},"
        "{\"id\":\"tint\",\"type\":\"param\",\"params\":{\"name\":\"tint\",\"type\":\"color\",\"default\":\"#80ff00\"}},"
        "{\"id\":\"amp\",\"type\":\"param\",\"params\":{\"name\":\"amp\",\"default\":0.1,\"min\":0,\"max\":1}},"
        "{\"id\":\"m\",\"type\":\"mul\",\"inputs\":{\"a\":{\"node\":\"tex\",\"port\":\"rgb\"},\"b\":{\"node\":\"tint\"}}},"
        "{\"id\":\"t\",\"type\":\"time\"},{\"id\":\"s\",\"type\":\"sin\",\"inputs\":{\"x\":{\"node\":\"t\"}}},"
        "{\"id\":\"w\",\"type\":\"mul\",\"inputs\":{\"a\":{\"node\":\"s\"},\"b\":{\"node\":\"amp\"}}},"
        "{\"id\":\"p\",\"type\":\"position\"},{\"id\":\"po\",\"type\":\"add\",\"inputs\":{\"a\":{\"node\":\"p\"},\"b\":{\"node\":\"w\"}}},"
        "{\"id\":\"e\",\"type\":\"mul\",\"inputs\":{\"a\":{\"node\":\"tint\"},\"b\":{\"node\":\"amp\"}}},"
        "{\"id\":\"o\",\"type\":\"output\",\"inputs\":{\"color\":{\"node\":\"m\"},\"emissive\":{\"node\":\"e\"},"
        "\"positionOffset\":{\"node\":\"po\"}}}],\"output\":\"o\"}");
    cJSON *args = cJSON_Parse("{\"tint\":\"#0000ff\",\"amp\":5}");
    CHECK(mod_shader_graph_compile_surface(g, args, &prog, err, sizeof(err)), "surface graph did not compile: %s", err);
    CHECK(prog.vertex && prog.fragment, "a surface graph has both stages");
    CHECK(!prog.lit && prog.double_sided && !prog.additive && prog.depth_write && prog.transparent,
          "the output defaults: unlit, double, normal, depthWrite, transparent (%d %d %d %d %d)", prog.lit, prog.double_sided,
          prog.additive, prog.depth_write, prog.transparent);
    CHECK(prog.uniform_count == 2, "one uniform per param node across both stages: %d", prog.uniform_count);
    CHECK(prog.param_count == 2, "the param list: %d", prog.param_count);
    for (int i = 0; i < prog.uniform_count; i++) {
        if (!strcmp(prog.uniforms[i].name, "tint"))
            CHECK(prog.uniforms[i].width == 3 && prog.uniforms[i].value[2] == 1.0f && prog.uniforms[i].value[0] == 0.0f, "tint is #0000ff");
        if (!strcmp(prog.uniforms[i].name, "amp")) CHECK(prog.uniforms[i].value[0] == 1.0f, "amp 5 clamps to 1: %g", prog.uniforms[i].value[0]);
    }
    CHECK(prog.vertex && strstr(prog.vertex, "vec3 kn_position = ") && !strstr(prog.vertex, "kn_position = kn_plocal;"),
          "positionOffset is the vertex position");
    CHECK(prog.fragment && strstr(prog.fragment, "kn_map_at(kn_uv)"), "the map slot samples the mesh's texture at its uv");
    CHECK(prog.fragment && strstr(prog.fragment, "kn_rgb += "), "an unlit emissive adds to the colour (:95838)");
    CHECK(prog.fragment && !strstr(prog.fragment, "kn_ambient"), "an unlit graph is not lit");
    mod_shader_program_free(&prog);
    cJSON_Delete(args);
    cJSON_Delete(g);

    // lit, front, additive; no colour: black (:95590); no positionOffset: the mesh's position
    g = cJSON_Parse("{\"v\":1,\"kind\":\"surface\",\"nodes\":[{\"id\":\"o\",\"type\":\"output\",\"params\":{\"lit\":true,"
                    "\"side\":\"front\",\"blend\":\"additive\",\"transparent\":false}}],\"output\":\"o\"}");
    CHECK(mod_shader_graph_compile_surface(g, NULL, &prog, err, sizeof(err)), "lit graph did not compile: %s", err);
    CHECK(prog.lit && !prog.double_sided && prog.additive && prog.depth_write && !prog.transparent, "lit front additive opaque");
    CHECK(prog.fragment && strstr(prog.fragment, "vec3 kn_color = vec3(0.0);") && strstr(prog.fragment, "kn_ambient"),
          "a lit graph without colour: black, lit");
    CHECK(prog.vertex && strstr(prog.vertex, "vec3 kn_position = kn_plocal;"), "no positionOffset: the mesh's own position");
    mod_shader_program_free(&prog);
    cJSON_Delete(g);
}

static void test_compile(void) {
    cJSON *g = cJSON_Parse(k_grey);
    ModShaderProgram prog;
    char err[256] = "";
    CHECK(mod_shader_graph_compile(g, NULL, &prog, err, sizeof(err)), "greyscale did not compile: %s", err);
    CHECK(prog.fragment && strstr(prog.fragment, "kn_lum("), "no luminance in the greyscale shader");
    CHECK(prog.fragment && !strstr(prog.fragment, "kn_perlin"), "the noise library is emitted without a noise node");
    CHECK(prog.uniform_count == 1 && prog.uniforms[0].width == 1, "one float uniform expected, %d", prog.uniform_count);
    CHECK(prog.uniform_count == 1 && prog.uniforms[0].value[0] == 1.0f, "default 2 should clamp to max 1: %g", prog.uniforms[0].value[0]);
    CHECK(prog.param_count == 1 && !strcmp(prog.params[0].name, "amount") && prog.params[0].has_max, "the param list");
    mod_shader_program_free(&prog);
    // shaderArgs.params override the default (and clamp)
    cJSON *args = cJSON_Parse("{\"amount\":0.25}");
    CHECK(mod_shader_graph_compile(g, args, &prog, err, sizeof(err)) && prog.uniforms[0].value[0] == 0.25f, "instance param");
    mod_shader_program_free(&prog);
    cJSON_Delete(args);
    cJSON_Delete(g);

    // invert: 1 - sceneColor, alpha from a combine
    g = cJSON_Parse("{\"v\":1,\"kind\":\"post\",\"nodes\":[{\"id\":\"s\",\"type\":\"sceneColor\"},"
                    "{\"id\":\"i\",\"type\":\"oneMinus\",\"inputs\":{\"x\":{\"node\":\"s\"}}},"
                    "{\"id\":\"sp\",\"type\":\"split\",\"inputs\":{\"v\":{\"node\":\"i\"}}},"
                    "{\"id\":\"c\",\"type\":\"combine\",\"inputs\":{\"x\":{\"node\":\"sp\",\"port\":\"x\"},\"y\":{\"node\":\"sp\",\"port\":\"y\"},"
                    "\"z\":{\"node\":\"sp\",\"port\":\"z\"},\"w\":{\"value\":1}}},"
                    "{\"id\":\"o\",\"type\":\"output\",\"inputs\":{\"color\":{\"node\":\"c\"}}}],\"output\":\"o\"}");
    CHECK(mod_shader_graph_compile(g, NULL, &prog, err, sizeof(err)), "invert did not compile: %s", err);
    CHECK(prog.fragment && strstr(prog.fragment, "1.0 - ("), "no oneMinus in the invert shader");
    mod_shader_program_free(&prog);
    cJSON_Delete(g);

    // no colour input: black (deob:95527-95531)
    g = cJSON_Parse("{\"v\":1,\"kind\":\"post\",\"nodes\":[{\"id\":\"o\",\"type\":\"output\"}],\"output\":\"o\"}");
    CHECK(mod_shader_graph_compile(g, NULL, &prog, err, sizeof(err)), "empty output did not compile: %s", err);
    CHECK(prog.fragment && strstr(prog.fragment, "kn_frag = vec4(0.0, 0.0, 0.0, 1.0);"), "an unwired output is black");
    mod_shader_program_free(&prog);
    cJSON_Delete(g);
}

static void write_stage(const char *dir, const char *name, const char *ext, const char *text) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.%s", dir, name, ext);
    FILE *f = fopen(path, "wb");
    if (!f) {
        CHECK(0, "cannot write %s", path);
        return;
    }
    fputs("#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n", f);
    fputs(text, f);
    fclose(f);
}

static void write_file(const char *dir, const char *name, const char *text) { write_stage(dir, name, "frag", text); }

// every node type a surface graph may hold (not the post-only ones) feeding its colour from each port; the dump goes
// to <dir>/surface
static void test_every_surface_node(const char *dump) {
    int compiled = 0;
    char dir[512] = "";
    if (dump) {
        snprintf(dir, sizeof(dir), "%s/surface", dump);
        char cmd[600];
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
        if (system(cmd) != 0) CHECK(0, "cannot make %s", dir);
    }
    for (int i = 0; i < MSG_NODE_COUNT; i++) {
        const MsgNodeDef *d = &MSG_NODES[i];
        if (!strcmp(d->type, "output") || d->kinds == 1) continue;
        for (int o = 0; o < d->nout; o++) {
            for (int port = 0; port < 2; port++) { // the colour, and the vertex stage's positionOffset
                char json[1024];
                snprintf(json, sizeof(json),
                         "{\"v\":1,\"kind\":\"surface\",\"nodes\":[{\"id\":\"n\",\"type\":\"%s\"%s},"
                         "{\"id\":\"o\",\"type\":\"output\",\"params\":{\"lit\":%s},\"inputs\":{\"%s\":{\"node\":\"n\",\"port\":\"%s\"}}}],"
                         "\"output\":\"o\"}",
                         d->type, !strcmp(d->type, "param") ? ",\"params\":{\"name\":\"p\"}" : "", i % 2 ? "true" : "false",
                         port ? "positionOffset" : "color", d->out[o].name);
                cJSON *g = cJSON_Parse(json);
                ModShaderProgram prog;
                char err[256] = "";
                const int ok = mod_shader_graph_compile_surface(g, NULL, &prog, err, sizeof(err));
                CHECK(ok, "surface %s.%s (%s) did not compile: %s", d->type, d->out[o].name, port ? "positionOffset" : "color", err);
                if (ok) {
                    compiled++;
                    if (dump) {
                        char name[96];
                        snprintf(name, sizeof(name), "%s_%s_%s", d->type, d->out[o].name, port ? "pos" : "col");
                        write_stage(dir, name, "vert", prog.vertex);
                        write_stage(dir, name, "frag", prog.fragment);
                    }
                }
                mod_shader_program_free(&prog);
                cJSON_Delete(g);
            }
        }
    }
    printf("mod_shader_test: %d surface node outputs translate\n", compiled);
}

// every node type alone (its inputs at their defaults) feeding the output from each port
static void test_every_node(const char *dump) {
    int compiled = 0;
    for (int i = 0; i < MSG_NODE_COUNT; i++) {
        const MsgNodeDef *d = &MSG_NODES[i];
        if (!strcmp(d->type, "output")) continue;
        for (int o = 0; o < d->nout; o++) {
            char json[1024];
            snprintf(json, sizeof(json),
                     "{\"v\":1,\"kind\":\"post\",\"nodes\":[{\"id\":\"n\",\"type\":\"%s\"%s},"
                     "{\"id\":\"o\",\"type\":\"output\",\"inputs\":{\"color\":{\"node\":\"n\",\"port\":\"%s\"}}}],\"output\":\"o\"}",
                     d->type, !strcmp(d->type, "param") ? ",\"params\":{\"name\":\"p\"}" : "", d->out[o].name);
            cJSON *g = cJSON_Parse(json);
            ModShaderProgram prog;
            char err[256] = "";
            const int ok = mod_shader_graph_compile(g, NULL, &prog, err, sizeof(err));
            CHECK(ok, "%s.%s did not compile: %s", d->type, d->out[o].name, err);
            if (ok) {
                compiled++;
                if (dump) {
                    char name[96];
                    snprintf(name, sizeof(name), "%s_%s", d->type, d->out[o].name);
                    write_file(dump, name, prog.fragment);
                }
            }
            mod_shader_program_free(&prog);
            cJSON_Delete(g);
        }
    }
    printf("mod_shader_test: %d node outputs translate\n", compiled);
}

int main(const int argc, char **argv) {
    const char *dump = argc > 2 && !strcmp(argv[1], "--dump") ? argv[2] : NULL;
    test_validation();
    test_compile();
    test_every_node(dump);
    test_surface_validation();
    test_surface_compile();
    test_every_surface_node(dump);
    if (dump) { // the two sample graphs too
        const char *samples[2] = {k_grey, NULL};
        for (int i = 0; samples[i]; i++) {
            cJSON *g = cJSON_Parse(samples[i]);
            ModShaderProgram prog;
            char err[256];
            if (mod_shader_graph_compile(g, NULL, &prog, err, sizeof(err))) write_file(dump, "sample_grey", prog.fragment);
            mod_shader_program_free(&prog);
            cJSON_Delete(g);
        }
    }
    if (g_failures) {
        printf("mod_shader_test: %d failures\n", g_failures);
        return 1;
    }
    printf("mod_shader_test: ok\n");
    return 0;
}
