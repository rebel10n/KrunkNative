#include <shared.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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