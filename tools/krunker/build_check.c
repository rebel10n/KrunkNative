/* The client's own verdict on a Krunker build document (shared/src/krunker_build_live.c), for the
 * "Krunker build" workflow and tools/krunker/test_extract.py: the same parse and shape checks an official
 * join runs before it uses a published build.json, so the publisher refuses exactly what clients would.
 *
 *   cc -std=c11 -Ishared/include -Ishared/lib/cJSON tools/krunker/build_check.c shared/src/krunker_build_live.c \
 *      shared/src/auth_join.c shared/lib/cJSON/cJSON.c -o build_check
 *   ./build_check DOC.json [MANIFEST.json]
 *
 * Prints "ok <clientVersion> <buildVersion> ..." (exit 0), or "refused: <why>" (exit 2: a build this
 * client cannot follow, the shape changed) / "invalid: <why>" (exit 1: not a document, or not the one
 * the manifest names). */
#include "krunker_build_live.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc((4u << 20) + 1);
    const size_t n = buf ? fread(buf, 1, 4u << 20, f) : 0;
    fclose(f);
    if (!buf) return NULL;
    buf[n] = 0;
    *len = n;
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s DOC.json [MANIFEST.json]\n", argv[0]);
        return 64;
    }
    size_t len = 0, mlen = 0;
    char *doc = slurp(argv[1], &len);
    if (!doc) {
        printf("invalid: cannot read %s\n", argv[1]);
        return 1;
    }
    KrunkerBuild *b = NULL;
    char why[300] = "";
    int rc;
    if (argc > 2) {
        char *mtext = slurp(argv[2], &mlen);
        KrunkerBuildManifest m;
        rc = mtext ? krunker_build_manifest_parse(mtext, mlen, &m, why, sizeof why) : KRUNKER_BUILD_EJSON;
        if (!mtext) snprintf(why, sizeof why, "cannot read %s", argv[2]);
        if (rc == KRUNKER_BUILD_OK) rc = krunker_build_accept(&m, doc, len, &b, why, sizeof why);
        free(mtext);
    } else {
        rc = krunker_build_parse(doc, len, &b, why, sizeof why);
    }
    free(doc);
    if (rc != KRUNKER_BUILD_OK) {
        printf("%s: %s\n", rc == KRUNKER_BUILD_ESHAPE ? "refused" : "invalid", why);
        return rc == KRUNKER_BUILD_ESHAPE ? 2 : 1;
    }
    const KrunkerBuild *c = krunker_build_compiled();
    int c2s = 0, s2c = 0;
    for (int i = 0; i < KRUNKER_C2S_COUNT; i++) c2s += b->c2s_wire[i] != NULL;
    for (int i = 0; i < KRUNKER_S2C_COUNT; i++) s2c += b->s2c_wire[i] != NULL;
    printf("ok %s %s AHKEY 0x%llx \"0\" %d/player \"k\" %d/player, %d/%d c2s and %d/%d s2c packets known%s\n", b->client_version,
           b->build_version, (unsigned long long) b->ahkey, b->add_stride, b->k_stride, c2s, KRUNKER_C2S_COUNT, s2c,
           KRUNKER_S2C_COUNT, strcmp(b->build_version, c->build_version) ? " (not the compiled build)" : " (the compiled build)");
    krunker_build_free(b);
    return 0;
}
