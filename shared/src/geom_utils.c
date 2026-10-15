// Pure geometry and colour helpers (shared/include/shared.h): hex colours, angles and the
// ray / box test. No asset or file access, so every target (client, server, brain) links them.
// They used to sit in utils.c next to the asset loaders, which kn_brain cannot link whole.

#include <shared.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
