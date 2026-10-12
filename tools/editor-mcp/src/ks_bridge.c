// KrunkScript bridge: checks the scripts a map carries (v7.1.4 "scripts": {client, server},
// base64 in editor exports) with KrunkNative's KrunkScript front end (shared/include/ks.h,
// doc/v7/krunkscript.md) when it is linked in, and degrades to a stub otherwise.
//
// The compiler is a parallel work package. Its entry points are weak references here: when the
// shared/src/ks sources are part of the build and define ks_validate / ks_decode_script, they are
// used; when they are absent (or not implemented yet) the pointers are NULL and map_validate /
// map_compile report "compiler not available" instead of failing to link.
#define _GNU_SOURCE
#include "util.h"
#include "validate.h"
#include <ks.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma weak ks_validate
#pragma weak ks_decode_script
#pragma weak ks_is_compiled_js
#pragma weak ks_diag_format

static int compiler_linked(void) { return ks_validate != NULL; }

const char *ks_bridge_status(void) {
    return compiler_linked() ? "KrunkScript front end linked (ks_validate)" : "KrunkScript compiler not linked: scripts are decoded but not checked (stub)";
}

static int looks_base64(const char *s, const size_t n) {
    if (n < 4 || n % 4) return 0;
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (!(isalnum((unsigned char) c) || c == '+' || c == '/' || c == '=')) return 0;
    }
    return 1;
}

char *ks_bridge_decode(const char *text, size_t *len) {
    const size_t n = strlen(text);
    if (ks_decode_script) return ks_decode_script(text, n, len);
    if (looks_base64(text, n)) {
        size_t out_len = 0;
        unsigned char *raw = base64_decode(text, &out_len);
        if (raw) {
            int printable = 1;
            for (size_t i = 0; i < out_len && printable; i++) printable = raw[i] >= 9 && raw[i] != 0x7f;
            if (printable) {
                if (len) *len = out_len;
                return (char *) raw;
            }
            free(raw);
        }
    }
    if (len) *len = n;
    return strdup(text);
}

static void add_diag(cJSON *diags, const char *severity, const char *code, const char *side, const KsDiag *d, const char *fallback) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "severity", severity);
    cJSON_AddStringToObject(o, "code", code);
    char msg[400];
    if (d) snprintf(msg, sizeof(msg), "KrunkScript (%s) %d:%d %s", side, d->line, d->col, d->message);
    else snprintf(msg, sizeof(msg), "%s", fallback);
    cJSON_AddStringToObject(o, "message", msg);
    cJSON_AddStringToObject(o, "side", side);
    if (d) {
        cJSON_AddNumberToObject(o, "line", d->line);
        cJSON_AddNumberToObject(o, "col", d->col);
    }
    cJSON_AddItemToArray(diags, o);
}

int ks_bridge_check_scripts(const cJSON *map_json, cJSON *diags, int *available) {
    *available = compiler_linked();
    const cJSON *scripts = js_get(map_json, "scripts");
    if (!scripts) return 0;
    if (!cJSON_IsObject(scripts)) {
        add_diag(diags, "error", "script.bad_type", "map", NULL, "\"scripts\" should be an object {client, server}");
        return 1;
    }
    int errors = 0;
    static const char *sides[2] = {"client", "server"};
    for (int s = 0; s < 2; s++) {
        const cJSON *v = js_get(scripts, sides[s]);
        if (!v) continue;
        if (!cJSON_IsString(v)) {
            add_diag(diags, "error", "script.bad_type", sides[s], NULL, "scripts entries must be strings");
            errors++;
            continue;
        }
        size_t len = 0;
        char *src = ks_bridge_decode(v->valuestring, &len);
        if (!src) continue;
        if (ks_is_compiled_js && ks_is_compiled_js(src, len)) {
            char m[200];
            snprintf(m, sizeof(m), "%s script is compiled JavaScript: KrunkNative does not run it", sides[s]);
            add_diag(diags, "warning", "script.compiled_js", sides[s], NULL, m);
        } else if (!*available) {
            char m[200];
            snprintf(m, sizeof(m), "%s script (%zu bytes) not checked: the KrunkScript compiler is not linked", sides[s], len);
            add_diag(diags, "info", "script.unchecked", sides[s], NULL, m);
        } else {
            KsDiag err = {0}, warns[16];
            int wcount = 0;
            const int rc = ks_validate(src, len, s == 0 ? KS_SIDE_CLIENT : KS_SIDE_SERVER, &err, warns, 16, &wcount);
            if (rc != 0) {
                add_diag(diags, "error", "script.compile_error", sides[s], &err, NULL);
                errors++;
            }
            for (int w = 0; w < wcount && w < 16; w++) add_diag(diags, "warning", "script.warning", sides[s], &warns[w], NULL);
        }
        free(src);
    }
    return errors;
}
