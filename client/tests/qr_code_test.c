// qr_code_test: client/src/ui/qr_code.c (the 2FA popup's QR code) against matrices made by
// python-qrcode (client/tests/data/qr_golden.json: text, ECC level, forced mask -> rows), plus the
// automatic mask choice and the SVG writer.
//   qr_code_test <repo root>
#include "qr_code.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t) n + 1);
    if (s && fread(s, 1, (size_t) n, f) != (size_t) n) {
        free(s);
        s = NULL;
    }
    if (s) s[n] = 0;
    fclose(f);
    return s;
}

int main(int argc, char **argv) {
    char path[1024];
    snprintf(path, sizeof path, "%s/client/tests/data/qr_golden.json", argc > 1 ? argv[1] : ".");
    char *text = slurp(path);
    cJSON *cases = text ? cJSON_Parse(text) : NULL;
    CHECK(cJSON_IsArray(cases), "cannot read %s", path);
    const cJSON *c;
    static QrCode qr;
    cJSON_ArrayForEach(c, cases) {
        const char *data = cJSON_GetStringValue(cJSON_GetObjectItem(c, "text"));
        const QrEcc ecc = !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(c, "ecc")), "L") ? QR_ECC_L : QR_ECC_M;
        const int mask = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(c, "mask"));
        const cJSON *rows = cJSON_GetObjectItem(c, "rows");
        CHECK(qr_encode((const unsigned char *) data, strlen(data), ecc, mask, &qr) == 0, "encode %.20s", data);
        CHECK(qr.version == (int) cJSON_GetNumberValue(cJSON_GetObjectItem(c, "version")), "version of %.20s: %d", data, qr.version);
        CHECK(qr.size == cJSON_GetArraySize(rows), "size of %.20s", data);
        int diff = 0;
        for (int y = 0; y < qr.size && y < cJSON_GetArraySize(rows); y++) {
            const char *row = cJSON_GetStringValue(cJSON_GetArrayItem(rows, y));
            for (int x = 0; x < qr.size && row && row[x]; x++) diff += (row[x] == '1') != qr.dark[y][x];
        }
        CHECK(diff == 0, "%.30s (ecc %d mask %d): %d modules differ from python-qrcode", data, ecc, mask, diff);
    }
    // automatic mask: some mask, same version
    const char *uri = "otpauth://totp/Krunker:player?secret=JBSWY3DPEHPK3PXP&issuer=Krunker";
    CHECK(qr_encode((const unsigned char *) uri, strlen(uri), QR_ECC_M, -1, &qr) == 0 && qr.mask >= 0 && qr.mask <= 7, "auto mask");
    char svg[64 * 1024];
    const int n = qr_svg(&qr, svg, sizeof svg);
    CHECK(n > 0 && strstr(svg, "<svg") == svg && strstr(svg, "</svg>"), "svg");
    CHECK(qr_svg(&qr, svg, 64) == -1, "svg: small buffer refused");
    char big[400];
    memset(big, 'a', sizeof big);
    CHECK(qr_encode((const unsigned char *) big, sizeof big, QR_ECC_M, -1, &qr) == -1, "too long for version 10");
    cJSON_Delete(cases);
    free(text);
    printf("qr_code_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
