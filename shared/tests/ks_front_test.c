// ks_front_test: KrunkScript front end (WP1) conformance suite.
//
//   ks_front_test <dir>   (dir = shared/tests/ks)
//
// <dir>/corpus/*.krnk  v7.1.4 reference scripts: must compile with no errors and no
//                      warnings. "server" in the file name selects the server side.
// <dir>/ok/*.krnk      targeted accept tests; optional <name>.warn lists the expected
//                      warnings ("<line>:<col> <message>", one per line).
// <dir>/err/*.krnk     negative tests; <name>.err holds the expected first error
//                      ("<line>:<col> <message>").
// <dir>/js/*.js        compiled-JS scripts (WP7 importer, ks_import_js.h): the import must
//                      equal <name>.krnk byte for byte and pass the checker with no warnings.
// A first line "# side: server" selects the server side for ok/ and err/ tests.
// Plus unit checks of the AST shape, the GAME.* table and the script text helpers.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ks.h"
#include "ks_api.h"
#include "ks_ast.h"
#include "ks_import_js.h"

static int g_fail, g_pass;

#define CHECK(cond, ...) do { \
    if (cond) g_pass++; \
    else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static char *slurp(const char *path, size_t *len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc((size_t) n + 1);
    if (buf && fread(buf, 1, (size_t) n, fp) != (size_t) n) {
        free(buf);
        buf = NULL;
    }
    fclose(fp);
    if (!buf) return NULL;
    buf[n] = 0;
    if (len) *len = (size_t) n;
    return buf;
}

static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *) a, *(char *const *) b); }

// Sorted *.krnk names in dir (caller frees).
static int list_krnk(const char *dir, char ***out) {
    DIR *d = opendir(dir);
    *out = NULL;
    if (!d) return 0;
    int n = 0, cap = 0;
    char **names = NULL;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 6 || strcmp(e->d_name + l - 5, ".krnk") != 0) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            names = realloc(names, (size_t) cap * sizeof *names);
        }
        names[n] = malloc(l + 1);
        memcpy(names[n++], e->d_name, l + 1);
    }
    closedir(d);
    qsort(names, (size_t) n, sizeof *names, cmp_str);
    *out = names;
    return n;
}

static KsSide side_of(const char *name, const char *src) {
    if (strncmp(src, "# side: server", 14) == 0) return KS_SIDE_SERVER;
    if (strncmp(src, "# side: client", 14) == 0) return KS_SIDE_CLIENT;
    return strstr(name, "server") ? KS_SIDE_SERVER : KS_SIDE_CLIENT;
}

#define MAXW 64

static void run_dir(const char *root, const char *sub, int mode /* 0 corpus, 1 ok, 2 err */, int *count) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/%s", root, sub);
    char **names;
    int n = list_krnk(dir, &names);
    *count = n;
    for (int i = 0; i < n; i++) {
        char path[1200], exp_path[1200];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        size_t len;
        char *src = slurp(path, &len);
        CHECK(src != NULL, "cannot read %s", path);
        if (!src) continue;
        KsDiag err, warns[MAXW];
        int nw = 0;
        KsSide side = side_of(names[i], src);
        int rc = ks_validate(src, len, side, &err, warns, MAXW, &nw);
        char got[300];
        ks_diag_format(&err, got, sizeof got);
        if (mode == 2) {
            snprintf(exp_path, sizeof exp_path, "%s/%.*s.err", dir, (int) (strlen(names[i]) - 5), names[i]);
            char *exp = slurp(exp_path, NULL);
            CHECK(exp != NULL, "%s: missing %s", names[i], exp_path);
            if (exp) {
                rstrip(exp);
                CHECK(rc != 0 && strcmp(got, exp) == 0, "%s/%s: expected \"%s\", got %s\"%s\"", sub, names[i], exp,
                      rc == 0 ? "success " : "", rc == 0 ? "" : got);
                free(exp);
            }
        } else {
            CHECK(rc == 0, "%s/%s: %s", sub, names[i], got);
            snprintf(exp_path, sizeof exp_path, "%s/%.*s.warn", dir, (int) (strlen(names[i]) - 5), names[i]);
            char *exp = mode == 1 ? slurp(exp_path, NULL) : NULL;
            char gotw[4096] = "";
            for (int w = 0; w < nw && w < MAXW; w++) {
                char line[300];
                ks_diag_format(&warns[w], line, sizeof line);
                strncat(gotw, line, sizeof gotw - strlen(gotw) - 2);
                strcat(gotw, "\n");
            }
            rstrip(gotw);
            if (exp) rstrip(exp);
            CHECK(strcmp(gotw, exp ? exp : "") == 0, "%s/%s: warnings:\n%s\nexpected:\n%s", sub, names[i], gotw,
                  exp ? exp : "(none)");
            free(exp);
        }
        free(src);
        free(names[i]);
    }
    free(names);
}

// ---------------------------------------------------------------------------
// Unit checks

static KsAstProgram *front(const char *src, KsSide side) {
    KsDiag err;
    KsAstProgram *p = ks_front(src, strlen(src), side, &err, NULL, 0, NULL);
    if (!p) {
        char buf[300];
        ks_diag_format(&err, buf, sizeof buf);
        printf("  front error: %s\n", buf);
    }
    return p;
}

static void test_ast_shape(void) {
    const char *src =
        "num g = 1;\n"
        "obj o = {a: 1, \"b\": 2,};\n"
        "num action twice(num x) { return x * 2; }\n"
        "action helper(action cb, str s) { cb(s); }\n"
        "public action update(num delta) {\n"
        "    num y = twice(g);\n"
        "    for (num i = 0; i < 3; i++) { y += i; }\n"
        "    for (obj e : o) { GAME.log(e.key, e.value); }\n"
        "    helper(twice, \"q\");\n"
        "    o.f(1);\n"
        "    num z = (num) o.a + Math.PI;\n"
        "    num w = o.a;\n"
        "}\n";
    KsDiag warns[4];
    int nw = 0;
    KsDiag err;
    KsAstProgram *p = ks_front(src, strlen(src), KS_SIDE_CLIENT, &err, warns, 4, &nw);
    CHECK(p != NULL, "ast program compiles: %s", err.message);
    if (!p) return;
    CHECK(p->global_count == 2 && p->action_count == 3, "globals %d actions %d", p->global_count, p->action_count);
    CHECK(nw == 1 && warns[0].line == 12, "one implicit-cast warning on line 12 (got %d)", nw);
    CHECK(ks_ast_find_action(p, "update") == 2, "find_action");
    KsAstAction *up = &p->actions[2];
    CHECK(up->is_public && up->hook_params == 1, "update is a 1-param hook");
    CHECK(p->actions[0].hook_params == -1, "twice is not a hook");
    CHECK(p->actions[1].params[0].type.base == KS_T_ACTION, "action-typed parameter");
    // locals: delta, y, i, e, z, w
    CHECK(up->local_count == 6, "update local_count %d", up->local_count);
    KsNodeList *b = &up->body->u.block;
    CHECK(b->count == 7, "statement count %d", b->count);
    KsNode *y = b->items[0];
    CHECK(y->kind == KS_S_VARDECL && y->u.var.scope == KS_SCOPE_LOCAL && y->u.var.slot == 1, "y local slot 1");
    KsNode *call = y->u.var.init;
    CHECK(call->kind == KS_E_CALL && call->u.call.call_kind == KS_CALL_ACTION && call->u.call.action_index == 0,
          "direct action call");
    CHECK(call->type.base == KS_T_NUM, "typed action result");
    KsNode *arg = call->u.call.args.items[0];
    CHECK(arg->kind == KS_E_IDENT && arg->u.ident.scope == KS_SCOPE_GLOBAL && arg->u.ident.slot == 0, "global ident");
    KsNode *loop = b->items[1];
    CHECK(loop->kind == KS_S_FOR && loop->line == 7 && loop->col == 5, "for at 7:5");
    KsNode *each = b->items[2];
    CHECK(each->kind == KS_S_FOREACH && each->u.foreach.slot == 3, "foreach slot %d", each->u.foreach.slot);
    KsNode *log = each->u.foreach.body->u.block.items[0]->u.expr.expr;
    CHECK(log->u.call.call_kind == KS_CALL_GAME_API && log->u.call.api_id == KS_API_LOG, "GAME.log api call");
    CHECK(log->u.call.callee->kind == KS_E_GAMEREF, "callee rewritten to GAMEREF");
    KsNode *hcall = b->items[3]->u.expr.expr;
    CHECK(hcall->u.call.args.items[0]->kind == KS_E_ACTIONREF, "action value argument");
    KsNode *cbcall = p->actions[1].body->u.block.items[0]->u.expr.expr;
    CHECK(cbcall->u.call.call_kind == KS_CALL_ACTION_VALUE, "action-value call");
    KsNode *m = b->items[4]->u.expr.expr;
    CHECK(m->u.call.call_kind == KS_CALL_METHOD && strcmp(m->u.call.callee_text, "o.f") == 0, "method call text");
    CHECK(m->u.call.callee->u.member.name_line == 10 && m->u.call.callee->u.member.name_col == 7, "method name pos");
    KsNode *z = b->items[5]->u.var.init;
    CHECK(z->kind == KS_E_BINARY && z->u.binary.l->kind == KS_E_CAST && !z->u.binary.l->u.cast.implicit, "explicit cast");
    CHECK(z->u.binary.r->kind == KS_E_GAMEREF && z->u.binary.r->u.gameref.api_id == KS_API_UTILS_MATH_PI, "Math.PI");
    KsNode *w = b->items[6]->u.var.init;
    CHECK(w->kind == KS_E_CAST && w->u.cast.implicit && w->type.base == KS_T_NUM, "implicit cast inserted");
    ks_ast_free(p);
}

static void test_strings(void) {
    KsAstProgram *p = front("str s = \"a\\x41\\u00e9\\uD83D\\uDE00\\uD800!\\\n\";", KS_SIDE_CLIENT);
    CHECK(p != NULL, "escapes compile");
    if (!p) return;
    KsNode *s = p->globals[0]->u.var.init;
    static const char want[] = "aA\xC3\xA9\xF0\x9F\x98\x80\xED\xA0\x80!";
    CHECK(s->u.str.len == sizeof want - 1 && memcmp(s->u.str.s, want, sizeof want - 1) == 0, "escape decoding");
    ks_ast_free(p);
    p = front("num a = 0xFF; num b = 1e+3; num c = .5; num d = 2.5e-1;", KS_SIDE_CLIENT);
    CHECK(p && p->globals[0]->u.var.init->u.num == 255 && p->globals[1]->u.var.init->u.num == 1000 &&
          p->globals[2]->u.var.init->u.num == 0.5 && p->globals[3]->u.var.init->u.num == 0.25, "number literals");
    ks_ast_free(p);
}

static void test_helpers(void) {
    // base64 of "num x = 1;" ; raw text passes through; fullwidth parens map
    size_t n;
    char *s = ks_decode_script("bnVtIHggPSAxOw==", 16, &n);
    CHECK(s && n == 10 && strcmp(s, "num x = 1;") == 0, "base64 decode");
    free(s);
    s = ks_decode_script("num x = 1;", 10, &n);
    CHECK(s && strcmp(s, "num x = 1;") == 0, "raw passthrough");
    free(s);
    const char fw[] = "GAME.log\xEF\xBC\x88\"a\"\xEF\xBC\x89";
    s = ks_decode_script(fw, sizeof fw - 1, &n);
    CHECK(s && strcmp(s, "GAME.log(\"a\")") == 0, "fullwidth parens");
    free(s);
    const char js[] = "\"use strict\";\nconst process = {}\nconst SM = (m) => m;";
    CHECK(ks_is_compiled_js(js, sizeof js - 1) == 1, "compiled js detected");
    CHECK(ks_is_compiled_js("num x = 1;", 10) == 0, "source is not js");
    KsDiag d = {3, 7, "Oops."};
    char buf[64];
    ks_diag_format(&d, buf, sizeof buf);
    CHECK(strcmp(buf, "3:7 Oops.") == 0, "diag format");
    CHECK(ks_hook_param_count(KS_SIDE_SERVER, "onCustomTrigger") == 3, "server hook");
    CHECK(ks_hook_param_count(KS_SIDE_CLIENT, "onCustomTrigger") == -1, "not a client hook");
    CHECK(ks_hook_param_count(KS_SIDE_CLIENT, "render") == 1, "client render hook");
    // too large
    size_t big = KS_MAX_SOURCE_BYTES + 1;
    char *src = malloc(big);
    memset(src, ' ', big);
    KsDiag err;
    CHECK(ks_validate(src, big, KS_SIDE_CLIENT, &err, NULL, 0, NULL) != 0 && strcmp(err.message, "Script too large.") == 0,
          "script too large");
    free(src);
    // recursion bounds: deep parens / blocks / long chains fail cleanly; long-ish chains pass
    src = malloc(64 * 1024);
    strcpy(src, "num x = ");
    for (int i = 0; i < 300; i++) strcat(src, "(");
    strcat(src, "1");
    for (int i = 0; i < 300; i++) strcat(src, ")");
    strcat(src, ";");
    CHECK(ks_validate(src, strlen(src), KS_SIDE_CLIENT, &err, NULL, 0, NULL) != 0 &&
          strcmp(err.message, "Script is too deeply nested.") == 0, "deep parens");
    strcpy(src, "public action start() ");
    for (int i = 0; i < 300; i++) strcat(src, "{");
    for (int i = 0; i < 300; i++) strcat(src, "}");
    CHECK(ks_validate(src, strlen(src), KS_SIDE_CLIENT, &err, NULL, 0, NULL) != 0 &&
          strcmp(err.message, "Script is too deeply nested.") == 0, "deep blocks");
    strcpy(src, "num x = 1");
    for (int i = 0; i < 2000; i++) strcat(src, "+1");
    strcat(src, ";");
    CHECK(ks_validate(src, strlen(src), KS_SIDE_CLIENT, &err, NULL, 0, NULL) != 0 &&
          strcmp(err.message, "Script is too deeply nested.") == 0, "long chain");
    strcpy(src, "str s = \"a\"");
    for (int i = 0; i < 500; i++) strcat(src, " + \"b\"");
    strcat(src, ";");
    CHECK(ks_validate(src, strlen(src), KS_SIDE_CLIENT, &err, NULL, 0, NULL) == 0, "500-term chain ok");
    free(src);
    // fullwidth parens in direct source
    const char fws[] = "public action start\xEF\xBC\x88\xEF\xBC\x89 { GAME.log(1); }";
    CHECK(ks_validate(fws, sizeof fws - 1, KS_SIDE_CLIENT, &err, NULL, 0, NULL) == 0, "fullwidth parens in source");
}

static void test_api_table(void) {
    int bad = 0;
    for (int i = 0; i < KS_API_COUNT; i++) {
        const KsApiEntry *e = ks_api_entry(i);
        if (ks_api_find(e->path) != i) bad++;
        if (ks_api_child(e->parent, e->name) != i) bad++;
        if (e->parent >= i) bad++;
        if (e->parent >= 0 && ks_api_entry(e->parent)->kind != KS_API_KIND_NS) bad++;
    }
    CHECK(bad == 0, "api table consistency (%d bad)", bad);
    CHECK(ks_api_find("GAME.SCENE.addCube") == KS_API_SCENE_ADDCUBE, "find with GAME. prefix");
    CHECK(ks_api_entry(KS_API_PLAYERS_LIST)->ret.base == KS_T_OBJ && ks_api_entry(KS_API_PLAYERS_LIST)->ret.dims == 1,
          "PLAYERS.list obj[]");
    CHECK(ks_api_entry(KS_API_SETTINGS_LIST)->ret.base == KS_T_STR, "SETTINGS.list typed str (TYP quirk)");
    CHECK(ks_api_entry(KS_API_COUNT) == NULL, "out of range");
}

// ---------------------------------------------------------------------------
// Compiled-JS importer (WP7)

static char *import_js(const char *js, size_t len, KsDiag *err) {
    size_t n = 0;
    char *out = ks_import_js(js, len, KS_SIDE_CLIENT, &n, err);
    CHECK(!out || n == strlen(out), "import length");
    return out;
}

static int test_import_dir(const char *root) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/js", root);
    DIR *d = opendir(dir);
    if (!d) return 0;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const size_t l = strlen(e->d_name);
        if (l < 4 || strcmp(e->d_name + l - 3, ".js") != 0) continue;
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        size_t len = 0;
        char *js = slurp(path, &len);
        snprintf(path, sizeof path, "%s/%.*s.krnk", dir, (int) (l - 3), e->d_name);
        char *want = slurp(path, NULL);
        CHECK(js && want, "%s: fixture and golden", e->d_name);
        if (!js || !want) {
            free(js);
            free(want);
            continue;
        }
        count++;
        CHECK(ks_is_compiled_js(js, len), "%s: detected as compiled JS", e->d_name);
        KsDiag err = {0};
        char *got = import_js(js, len, &err);
        CHECK(got != NULL, "%s: import: %d:%d %s", e->d_name, err.line, err.col, err.message);
        if (got) {
            CHECK(strcmp(got, want) == 0, "%s: import differs from %.*s.krnk", e->d_name, (int) (l - 3), e->d_name);
            KsDiag warns[8];
            int nwarn = 0;
            const int rc = ks_validate(got, strlen(got), KS_SIDE_CLIENT, &err, warns, 8, &nwarn);
            CHECK(rc == 0, "%s: imported script type-checks: %d:%d %s", e->d_name, err.line, err.col, err.message);
            CHECK(nwarn == 0, "%s: %d warnings (first %d:%d %s)", e->d_name, nwarn, warns[0].line, warns[0].col,
                  warns[0].message);
            // CRLF line endings import the same
            char *crlf = malloc(len * 2 + 1);
            size_t m = 0;
            for (size_t i = 0; i < len; i++) {
                if (js[i] == '\n') crlf[m++] = '\r';
                crlf[m++] = js[i];
            }
            char *got2 = import_js(crlf, m, &err);
            CHECK(got2 && strcmp(got2, want) == 0, "%s: CRLF import", e->d_name);
            free(got2);
            free(crlf);
        }
        free(got);
        free(js);
        free(want);
    }
    closedir(d);
    return count;
}

// Unrecognised shapes fail with a diagnostic at the JS position.
static void test_import_errors(const char *root) {
    char path[1024];
    snprintf(path, sizeof path, "%s/js/SniperRoyale2_client.js", root);
    char *js = slurp(path, NULL);
    CHECK(js != NULL, "SniperRoyale2 fixture");
    if (!js) return;
    char *end = strstr(js, "let LC = 0;\n");
    CHECK(end != NULL, "prelude end");
    if (!end) {
        free(js);
        return;
    }
    end[strlen("let LC = 0;\n")] = 0;
    const size_t plen = strlen(js); // 23 prelude lines: bodies start on line 24
    static const char epi[] = "return {start: function(){}};\n";
    static const struct { const char *body; int line; const char *msg; } cases[] = {
        {"function V_start() {\ntry {\nLC = 0;\nif (V_a == 1) {\n};\n} catch (e) {EH(e.message)};\n}\n", 27, "Loose equality"},
        {"let V_a = typeof V_b;\n", 24, "Unrecognised identifier \"typeof\""},
        {"let V_a = [];\n", 24, "Cannot recover the type of \"a\""},
        {"let V_a = `x`;\n", 24, "Template literals"},
        {"function V_f(V_x) {\n}\n", 24, "Cannot recover the parameter types of action \"f\""},
        {"let V_a = [1];\nlet V_b = [2];\nfunction V_start() {\nthis.GAME.V_log.bind(this)(V_a[BC(V_b,0,1,1)]);\n}\n", 27,
         "BC() checks a different array"},
        {"let V_a = foo;\n", 24, "Unrecognised identifier \"foo\""},
        {"let V_a = N(1);\n", 24, "Helper N() outside of its compiled pattern"},
        {"let V_a = 'x\\q';\n", 24, "Unsupported escape sequence"},
        {"let V_a = 1;\nfunction V_start() {\nV_a = this.GAME.V_PLAYERS.V_list.bind(this)().V_x;\nwith (V_a) {}\n}\n", 27,
         "\"with\" statement"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const size_t bl = strlen(cases[i].body);
        char *src = malloc(plen + bl + sizeof epi);
        memcpy(src, js, plen);
        memcpy(src + plen, cases[i].body, bl);
        memcpy(src + plen + bl, epi, sizeof epi);
        KsDiag err = {0};
        char *out = import_js(src, strlen(src), &err);
        CHECK(out == NULL, "import error case %zu rejected", i);
        CHECK(err.line == cases[i].line && strstr(err.message, cases[i].msg),
              "import error case %zu: got %d:%d %s, want %d: %s", i, err.line, err.col, err.message, cases[i].line,
              cases[i].msg);
        free(out);
        free(src);
    }
    {
        const char body[] = "let V_a = 1;\nlet V_b = 2;\n"; // no epilogue
        char *src = malloc(plen + sizeof body);
        memcpy(src, js, plen);
        memcpy(src + plen, body, sizeof body);
        KsDiag err = {0};
        char *out = import_js(src, strlen(src), &err);
        CHECK(out == NULL && err.line == 26 && strstr(err.message, "Missing the module epilogue"), "no epilogue: %d %s",
              err.line, err.message);
        free(out);
        free(src);
    }
    // a changed prelude line is not the v7 compiler's output
    char *bad = strstr(js, "const L = (x) => (x.length||0)");
    if (bad) {
        bad[17] = 'X';
        KsDiag err = {0};
        char *out = import_js(js, strlen(js), &err);
        CHECK(out == NULL && err.line == 8 && strstr(err.message, "prelude"), "prelude mismatch: %d %s", err.line,
              err.message);
        free(out);
    }
    free(js);
}

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : "shared/tests/ks";
    int nc, nok, nerr;
    run_dir(root, "corpus", 0, &nc);
    run_dir(root, "ok", 1, &nok);
    run_dir(root, "err", 2, &nerr);
    CHECK(nc >= 28, "corpus scripts found: %d", nc);
    CHECK(nerr > 0 && nok > 0, "ok/err tests found");
    test_ast_shape();
    test_strings();
    test_helpers();
    test_api_table();
    const int njs = test_import_dir(root);
    CHECK(njs >= 2, "compiled-JS fixtures found: %d", njs);
    test_import_errors(root);
    printf("ks_front_test: %d corpus, %d ok, %d err scripts; %d checks passed, %d failed\n", nc, nok, nerr, g_pass,
           g_fail);
    return g_fail ? 1 : 0;
}
