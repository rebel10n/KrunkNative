#include <shared.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

char *concat(const char *a, const char *b) {
    const size_t len_a = strlen(a);
    const size_t len_b = strlen(b);

    char *result = malloc(len_a + len_b + 1);
    if (!result) return NULL;

    memcpy(result, a, len_a);
    memcpy(result + len_a, b, len_b);

    result[len_a + len_b] = 0;

    return result;
}

const char *assets_path() {
    char path[256];
    if (!getcwd(path, sizeof(path))) return NULL;

#ifdef WIN32
    const char slash = '\\';
    const char *cmake_debug_suffix = "\\cmake-build-debug";
    const char *cmake_release_suffix = "\\cmake-build-release";
#else
    const char slash = '/';
    const char *cmake_debug_suffix = "/cmake-build-debug";
    const char *cmake_release_suffix = "/cmake-build-release";
#endif

    const size_t path_length = strlen(path);
    const size_t debug_length = strlen(cmake_debug_suffix);
    const size_t release_length = strlen(cmake_release_suffix);

    int devel = 0;

    if (path_length >= debug_length && !strcmp(path + path_length - debug_length, cmake_debug_suffix)) devel = 1;
    if (path_length > debug_length && !strcmp(path + path_length - debug_length - 1, cmake_debug_suffix) && path[path_length-1] == slash) devel = 1;

    if (path_length >= release_length && !strcmp(path + path_length - release_length, cmake_release_suffix)) devel = 1;
    if (path_length > release_length && !strcmp(path + path_length - release_length - 1, cmake_release_suffix) && path[path_length-1] == slash) devel = 1;

    const char *devel_assets = "../assets/";
    const char *assets = "./assets/";

    return devel ? devel_assets : assets;
}

int read_file(const char *path, unsigned char **buf, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file) return -1;

    fseek(file, 0, SEEK_END);
    *length = ftell(file);

    fseek(file, 0, SEEK_SET);
    *buf = malloc(*length + 1);

    if (!*buf) {
        fclose(file);
        return -1;
    }

    if (fread(*buf, 1, *length, file) != *length) {
        fclose(file);
        free(*buf);

        *buf = NULL;
        return -1;
    }

    fclose(file);
    (*buf)[*length] = 0;

    return 0;
}

int parse_hex_color(const char *hex_str, vec4 *out) {
    const int hex_len = (int) strlen(hex_str) - 1;

    if (hex_len < 0 || hex_str[0] != '#') return -1;
    hex_str++;

    // #RGA, #RGBA, #RRGGBB, #RRGGBBAA
    if (hex_len != 3 && hex_len != 4 && hex_len != 6 && hex_len != 8) return -1;

    unsigned char parsed;
    unsigned char channels[4] = {0};

    for (int i = 0; i < hex_len; i++) {
        const char value = hex_str[i];

        if (value >= 'A' && value <= 'F') parsed = value - 'A' + 10;
        else if (value >= 'a' && value <= 'f') parsed = value - 'a' + 10;
        else if (value >= '0' && value <= '9') parsed = value - '0';
        else return -1;

        if (hex_len < 6) {
            if (i > 3) i = 3; // SILENCE, LINTER!
            channels[i] = parsed << 4 | parsed;
        } else {
            channels[i / 2] |= parsed << (i % 2 ? 0 : 4);
        }
    }

    out->x = (float) channels[0] / 255.0f;
    out->y = (float) channels[1] / 255.0f;
    out->z = (float) channels[2] / 255.0f;
    out->w = hex_len == 4 || hex_len == 8 ? (float) channels[3] / 255.0f : 1.0f;

    return 0;
}

vec4 hex_to_vec(const int hex) {
    return (vec4) {
        (float) (hex >> 16 & 0xff) / 255.0f,
        (float) (hex >> 8 & 0xff) / 255.0f,
        (float) (hex & 0xff) / 255.0f,
        1.0f,
    };
}

float normalize_angle(float angle) {
    angle = fmodf(angle + (float) M_PI, (float) M_PI * 2.0f);

    if (angle < 0) return angle + (float) M_PI;
    return angle - (float) M_PI;
}

float progress_on_line(const vec2 line_start, const vec2 line_end, const vec2 point) {
    const vec2 dir = {line_end.x - line_start.x, line_end.y - line_start.y};
    return (dir.x * (point.x - line_start.x) + (point.y - line_start.y) * dir.y) / (dir.x*dir.x + dir.y*dir.y);
}

float line_in_rect(const vec3 origin, const vec3 direction, const vec3 obj_origin, const vec3 obj_scale) {
    if (
        direction.x > 0.0f && obj_origin.x + obj_scale.x * 0.5f < origin.x ||
        direction.x < 0.0f && obj_origin.x - obj_scale.x * 0.5f > origin.x ||
        direction.y > 0.0f && obj_origin.y + obj_scale.y < origin.y ||
        direction.y < 0.0f && obj_origin.y > origin.y ||
        direction.z > 0.0f && obj_origin.z + obj_scale.z * 0.5f < origin.z ||
        direction.z < 0.0f && obj_origin.z - obj_scale.z * 0.5f > origin.z
    ) return -1.0f;

    const float tx1 = (obj_origin.x - obj_scale.x * 0.5f - origin.x) / direction.x;
    const float tx2 = (obj_origin.x + obj_scale.x * 0.5f - origin.x) / direction.x;
    const float ty1 = (obj_origin.y - origin.y) / direction.y;
    const float ty2 = (obj_origin.y + obj_scale.y - origin.y) / direction.y;
    const float tz1 = (obj_origin.z - obj_scale.z * 0.5f - origin.z) / direction.z;
    const float tz2 = (obj_origin.z + obj_scale.z * 0.5f - origin.z) / direction.z;

    const float tmin = MAX(MAX(MIN(tx1, tx2), MIN(ty1, ty2)), MIN(tz1, tz2));
    const float tmax = MIN(MIN(MAX(tx1, tx2), MAX(ty1, ty2)), MAX(tz1, tz2));

    return tmin <= tmax && tmin > 0 ? tmin : -1.0f;
}

void angles_from_sides(const float a, const float b, const float c, float *out) {
    out[0] = acosf((b*b + c*c - a*a) / (2.0f * b * c));
    out[1] = acosf((a*a + c*c - b*b) / (2.0f * a * c));
    out[2] = acosf((a*a + b*b - c*c) / (2.0f * a * b));
}
