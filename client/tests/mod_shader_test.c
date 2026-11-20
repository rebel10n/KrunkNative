// mod_shader_test: the shader-graph layer of mod post passes (client/src/mod_shader_graph.c, no GL).
//   - the validator's refusals (deob-10.0.7.js:94463-94792) and a valid greyscale / invert graph;
//   - every node type of the table, unwired, feeding the output from each of its ports, translates;
//   - `--dump <dir>` writes those fragments (with a GLSL ES 3.00 header) for tools/modshader/check_glsl.sh.
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

static void write_file(const char *dir, const char *name, const char *text) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.frag", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        CHECK(0, "cannot write %s", path);
        return;
    }
    fputs("#version 300 es\n", f);
    fputs(text, f);
    fclose(f);
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
