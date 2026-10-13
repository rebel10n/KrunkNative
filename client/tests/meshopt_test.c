// meshopt_test: the EXT_meshopt_compression decoder (client/lib/meshopt/meshopt_decode.h through
// client/src/gltf_meshopt.c) against the meshoptimizer npm decoder (client/tests/data/meshopt_golden.json,
// tools/meshopt_golden/gen_golden.mjs):
//   - "vectors": encoded buffers (vertex codec v0 / v1, index buffer / sequence, the oct / quat / exp
//     filters) decode to the reference bytes;
//   - "assets": every compressed bufferView of the v7 AI / map .glb user assets, read with cgltf and
//     decoded by gltf_meshopt_decode, hashes like the reference (skipped when assets/ lacks the file);
//   - malformed input (truncated, bad header, trailing bytes, bad stride) is refused.
//   meshopt_test <repo root>
#define CGLTF_IMPLEMENTATION
#include "../lib/cgltf/cgltf.h"
#include <gltf_meshopt.h>
#include "../lib/meshopt/meshopt_decode.h"
#include <cJSON.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t fnv64(const unsigned char *p, const size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static unsigned char *from_hex(const char *hex, size_t *n) {
    const size_t len = strlen(hex);
    unsigned char *out = malloc(len / 2 + 1);
    if (!out) return NULL;
    for (size_t i = 0; i + 1 < len; i += 2) {
        unsigned int v = 0;
        sscanf(hex + i, "%2x", &v);
        out[i / 2] = (unsigned char) v;
    }
    *n = len / 2;
    return out;
}

static int mode_of(const char *s) {
    return !strcmp(s, "ATTRIBUTES") ? MESHOPT_MODE_ATTRIBUTES : !strcmp(s, "TRIANGLES") ? MESHOPT_MODE_TRIANGLES : !strcmp(s, "INDICES") ? MESHOPT_MODE_INDICES : -1;
}

static int filter_of(const char *s) {
    return !strcmp(s, "NONE") ? MESHOPT_FILTER_NONE : !strcmp(s, "OCTAHEDRAL") ? MESHOPT_FILTER_OCTAHEDRAL : !strcmp(s, "QUATERNION") ? MESHOPT_FILTER_QUATERNION
         : !strcmp(s, "EXPONENTIAL") ? MESHOPT_FILTER_EXPONENTIAL : -1;
}

static const char *str(const cJSON *o, const char *k) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, k));
    return s ? s : "";
}

static size_t num(const cJSON *o, const char *k) {
    return (size_t) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, k));
}

static void check_vectors(const cJSON *vectors) {
    const cJSON *v;
    cJSON_ArrayForEach(v, vectors) {
        const char *name = str(v, "name");
        const size_t count = num(v, "count"), stride = num(v, "stride");
        size_t encoded_size = 0;
        unsigned char *encoded = from_hex(str(v, "encoded"), &encoded_size);
        unsigned char *out = calloc(count * stride + 1, 1);
        const int result = meshopt_decode_gltf_buffer(out, count, stride, encoded, encoded_size, mode_of(str(v, "mode")), filter_of(str(v, "filter")));
        CHECK(result == 0, "%s: decode returned %d", name, result);
        char hash[17];
        snprintf(hash, sizeof(hash), "%016llx", (unsigned long long) fnv64(out, count * stride));
        CHECK(!strcmp(hash, str(v, "fnv64")), "%s: decoded hash %s, reference %s", name, hash, str(v, "fnv64"));
        const cJSON *decoded = cJSON_GetObjectItemCaseSensitive(v, "decoded");
        if (cJSON_IsString(decoded)) {
            size_t n = 0;
            unsigned char *expect = from_hex(decoded->valuestring, &n);
            CHECK(n == count * stride && !memcmp(expect, out, n), "%s: decoded bytes differ", name);
            free(expect);
        }

        // malformed copies are refused, never read out of bounds
        if (encoded_size > 1) {
            CHECK(meshopt_decode_gltf_buffer(out, count, stride, encoded, encoded_size - 1, mode_of(str(v, "mode")), filter_of(str(v, "filter"))) < 0,
                  "%s: a truncated buffer decodes", name);
            unsigned char *longer = malloc(encoded_size + 1);
            memcpy(longer, encoded, encoded_size);
            longer[encoded_size] = 0;
            // (the vertex codec's tail is at the end: an extra byte shifts it, so the decode fails or reads another tail)
            if (mode_of(str(v, "mode")) != MESHOPT_MODE_ATTRIBUTES)
                CHECK(meshopt_decode_gltf_buffer(out, count, stride, longer, encoded_size + 1, mode_of(str(v, "mode")), filter_of(str(v, "filter"))) < 0,
                      "%s: a buffer with a trailing byte decodes", name);
            longer[0] ^= 0x40; // header
            CHECK(meshopt_decode_gltf_buffer(out, count, stride, longer, encoded_size, mode_of(str(v, "mode")), filter_of(str(v, "filter"))) == -1,
                  "%s: a bad header decodes", name);
            free(longer);
        }
        free(out);
        free(encoded);
    }
}

static void check_assets(const char *root, const cJSON *assets) {
    int decoded = 0, skipped = 0;
    const char *loaded_file = NULL;
    cgltf_data *data = NULL;
    const cJSON *a;
    cJSON_ArrayForEach(a, assets) {
        const char *file = str(a, "file");
        if (!loaded_file || strcmp(loaded_file, file)) {
            if (data) cgltf_free(data);
            data = NULL;
            loaded_file = file;
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", root, file);
            cgltf_options options = {0};
            if (cgltf_parse_file(&options, path, &data) != cgltf_result_success) {
                data = NULL;
                printf("skip: %s (not in assets/: make assets-import-live)\n", file);
            } else {
                CHECK(cgltf_load_buffers(&options, data, path) == cgltf_result_success, "%s: buffers", file);
                char err[128];
                CHECK(gltf_meshopt_decode(data, err, sizeof(err)), "%s: gltf_meshopt_decode: %s", file, err);
                // every accessor of the model now reads (the fallback buffer has no data of its own)
                for (size_t i = 0; i < data->accessors_count; i++) {
                    const cgltf_accessor *accessor = &data->accessors[i];
                    if (!accessor->buffer_view || !accessor->count) continue;
                    float value[16];
                    CHECK(cgltf_accessor_read_float(accessor, accessor->count - 1, value, 16) || accessor->component_type == cgltf_component_type_invalid,
                          "%s: accessor %zu does not read", file, i);
                }
            }
        }
        if (!data) {
            skipped++;
            continue;
        }
        const size_t view = num(a, "view");
        CHECK(view < data->buffer_views_count && data->buffer_views[view].has_meshopt_compression, "%s: view %zu is not compressed", file, view);
        if (view >= data->buffer_views_count) continue;
        const cgltf_buffer_view *bv = &data->buffer_views[view];
        const size_t bytes = num(a, "count") * num(a, "stride");
        CHECK(bv->data != NULL && bv->meshopt_compression.count == num(a, "count") && bv->meshopt_compression.stride == num(a, "stride"),
              "%s: view %zu shape", file, view);
        if (!bv->data) continue;
        char hash[17];
        snprintf(hash, sizeof(hash), "%016llx", (unsigned long long) fnv64(bv->data, bytes));
        CHECK(!strcmp(hash, str(a, "fnv64")), "%s view %zu (%s %s): decoded hash %s, reference %s", file, view, str(a, "mode"), str(a, "filter"), hash, str(a, "fnv64"));
        decoded++;
    }
    if (data) cgltf_free(data);
    printf("assets: %d compressed views matched the reference decoder, %d skipped (missing files)\n", decoded, skipped);
}

static void check_errors(void) {
    unsigned char out[64], src[64];
    memset(src, 0, sizeof(src));
    src[0] = 0xa0;
    CHECK(meshopt_decode_gltf_buffer(out, 4, 6, src, sizeof(src), MESHOPT_MODE_ATTRIBUTES, MESHOPT_FILTER_NONE) == -4, "stride 6 accepted");
    CHECK(meshopt_decode_gltf_buffer(out, 4, 4, src, sizeof(src), MESHOPT_MODE_ATTRIBUTES, MESHOPT_FILTER_QUATERNION) == -4, "quaternion stride 4 accepted");
    CHECK(meshopt_decode_gltf_buffer(out, 4, 2, src, sizeof(src), MESHOPT_MODE_TRIANGLES, MESHOPT_FILTER_NONE) == -4, "4 triangle indices accepted");
    CHECK(meshopt_decode_gltf_buffer(out, 3, 2, src, sizeof(src), MESHOPT_MODE_TRIANGLES, MESHOPT_FILTER_OCTAHEDRAL) == -4, "filtered indices accepted");
    src[0] = 0xa2; // vertex codec version 2 does not exist
    CHECK(meshopt_decode_vertex_buffer(out, 4, 4, src, sizeof(src)) == -1, "vertex version 2 accepted");
    CHECK(meshopt_decode_vertex_buffer(out, 4, 4, src, 0) == -2, "empty buffer accepted");
}

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : ".";
    char path[1024];
    snprintf(path, sizeof(path), "%s/client/tests/data/meshopt_golden.json", root);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("FAIL: cannot open %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t) size + 1);
    const size_t got = text ? fread(text, 1, (size_t) size, f) : 0;
    fclose(f);
    if (!text || got != (size_t) size) return 1;
    text[size] = 0;
    cJSON *golden = cJSON_Parse(text);
    free(text);
    if (!golden) {
        printf("FAIL: %s does not parse\n", path);
        return 1;
    }
    check_vectors(cJSON_GetObjectItemCaseSensitive(golden, "vectors"));
    check_assets(root, cJSON_GetObjectItemCaseSensitive(golden, "assets"));
    check_errors();
    cJSON_Delete(golden);
    printf("meshopt_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
