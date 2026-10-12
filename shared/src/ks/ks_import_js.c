// KrunkScript importer for compiled-JS map scripts (doc/v7/krunkscript.md WP7): the v7
// compiler's JavaScript lowering (section 1.16) translated back to KrunkScript source,
// which the normal front end then compiles. See ks_import_js.h.
//
// Pipeline: match the fixed prelude line by line, lex the rest as a small JavaScript subset,
// parse it into a tree that already undoes the helper patterns (BC, A, S, B, O, F, L, NE, HP,
// FC, DE, OE, loop guards, the try/LC=0/catch wrapper, `.bind(this)`), then emit KrunkScript
// with types recovered from literals, hook signatures (4.1) and the GAME.* table (ks_api.h).
// Every construct outside that shape is an error with the JS line:col; this is not a
// JavaScript implementation.
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ks_api.h"
#include "ks_import_js.h"

// The prelude every compiled script starts with, verbatim from SniperRoyale2 / Verb_Arena (1.16).
// Its helpers define the semantics the translation relies on, so it must match exactly
// (trailing whitespace and CR aside).
static const char *const g_prelude[] = {
    "\"use strict\";",
    "const process = {}",
    "const require = {}",
    "const SM = (m) => m.replace(/\\bthis\\.GAME\\b/g, \"GAME\").replace(/V_(\\w+)/g, \"$1\")"
    ".replace(/\\bdelete\\b/g, \"remove\").replace(/of #<Object>/g, \"\");",
    "const EH = (m) => {throw new Error(\"RUNTIME ERROR: \"+SM(m).replace(/^RUNTIME ERROR: /g, \"\"))};",
    "const RE = (l,c,m) => {throw new Error(`${l}:${c} ${m}`)};",
    "const RP = (s) => s.replace(/V_(\\w+)/g, \"$1\");",
    "const L = (x) => (x.length||0)",
    "const NE = (x) => !!Object.keys(x||{}).length",
    "const B = (x) => !!x",
    "const N = (x) => (parseInt(x)||0);",
    "const F = (x) => (parseFloat(x)||0);",
    "const S = (x) => (x+\"\");",
    "const O = (x) => (typeof x===\"object\"&&!Array.isArray(x)?x:{});",
    "const A = (x,f) => (Array.isArray(x)?(x.forEach((v,i)=>x[i]=A(v,f)),x):f(x));",
    "const HP = (x,p) => x.hasOwnProperty(p);",
    "const BC = (a,i,l,c) => (a.hasOwnProperty(N(i))?N(i):RE(l,c,\"Index out of range.\"));",
    "const FC = (f,p,l,c) => (typeof f===\"function\"?f:RE(l,c,`Property \"${p}\" is not an an \"action\".`));",
    "const DE = (x,i) => (Array.isArray(x)?x.splice(i,1):0);",
    "const OE = (x) => Object.entries(x||{}).map(p=>{return{V_key:RP(p[0]),V_value:p[1]}});",
    "let V_UTILS = this.GAME.V_UTILS;",
    "let V_Math = this.GAME.V_UTILS.V_math;",
    "let LC = 0;",
};

#define MAX_DEPTH 200

// ---------------------------------------------------------------------------
// Tokens

enum { TK_EOF, TK_ID, TK_NUM, TK_STR, TK_PUNCT };

typedef struct {
    int kind;
    const char *s;
    int len;
    int line, col;
} Tok;

// ---------------------------------------------------------------------------
// Tree (expressions E_*, statements S_*)

enum {
    E_NUM, E_STR, E_BOOL, E_ID, E_GAME, E_MEMBER, E_INDEX, E_CALL, E_UNARY, E_BIN, E_LOGIC, E_TERN,
    E_ARR, E_OBJ, E_PAREN, E_CAST, E_LEN, E_NE, E_HP, E_PUSH, E_DE,
    S_VAR, S_IF, S_FOR, S_FOREACH, S_WHILE, S_RET, S_BREAK, S_CONT, S_EXPR, S_ASSIGN, S_INCDEC,
    S_ADDTO, S_REMOVE, S_BLOCK, S_EMPTY, S_GUARD
};

typedef struct Nd Nd;
struct Nd {
    int k;
    const char *op;      // E_UNARY/E_BIN/E_LOGIC/S_ASSIGN/S_INCDEC operator (KrunkScript spelling)
    const char *s;       // literal text (E_NUM/E_STR), name (E_ID/E_MEMBER/E_HP/S_VAR/S_FOREACH)
    int len;             // literal length
    Nd *a, *b, *c, *d;
    Nd **items;          // E_CALL args, E_ARR items, E_OBJ values, S_BLOCK statements
    const char **keys;   // E_OBJ keys (un-prefixed)
    int n;
    int prefix;          // S_INCDEC: ++x
    uint8_t cast;        // E_CAST: KS_T_NUM/STR/BOOL/OBJ
    int from_a;          // E_CAST: lowered through A(x, f) (element-wise on arrays)
    int line, col;       // JS position of the first token
    int hint;            // statements: original KrunkScript line (from BC/RE/FC), 0 = unknown
};

typedef struct {
    const char *name;
    const char **params;
    int nparams;
    Nd *body;            // S_BLOCK
    int line, col;
    int is_public;
    KsType ret;
    int ret_set, done;
} Fn;

typedef struct {
    char *p;
    size_t len, cap;
    int line;            // 1 + newlines written
} Buf;

typedef struct {
    const char *name;
    KsType t;
    int depth;
} Var;

typedef struct {
    const char *src;
    size_t len;
    KsSide side;
    KsDiag *err;
    jmp_buf jb;

    void **allocs;
    size_t nallocs, cap_allocs;

    Tok *toks;
    int ntok, cap_tok, pos;
    int hint;            // smallest original line seen in the current statement head
    int depth;

    Nd **globals;
    int nglobals, cap_globals;
    Fn *fns;
    int nfns, cap_fns;
    int have_epilogue;

    Var *vars;
    int nvars, cap_vars, scope;
    Fn *cur_fn;
    Buf out, body;       // the script, the action being emitted (in Imp: they survive longjmp)
} Imp;

static const KsType T_ANY = {KS_T_ANY, 0}, T_VOID = {KS_T_VOID, 0}, T_NUM = {KS_T_NUM, 0},
                    T_STR = {KS_T_STR, 0}, T_BOOL = {KS_T_BOOL, 0}, T_OBJ = {KS_T_OBJ, 0};

// ---------------------------------------------------------------------------
// Errors and memory

static void fail(Imp *I, int line, int col, const char *fmt, ...) {
    if (I->err) {
        I->err->line = line;
        I->err->col = col;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(I->err->message, sizeof I->err->message, fmt, ap);
        va_end(ap);
    }
    longjmp(I->jb, 1);
}

static void *alloc(Imp *I, size_t n) {
    if (I->nallocs == I->cap_allocs) {
        size_t cap = I->cap_allocs ? I->cap_allocs * 2 : 256;
        void **a = realloc(I->allocs, cap * sizeof *a);
        if (!a) fail(I, 0, 0, "Out of memory.");
        I->allocs = a;
        I->cap_allocs = cap;
    }
    void *p = calloc(1, n ? n : 1);
    if (!p) fail(I, 0, 0, "Out of memory.");
    I->allocs[I->nallocs++] = p;
    return p;
}

static char *dup_n(Imp *I, const char *s, size_t n) {
    char *d = alloc(I, n + 1);
    memcpy(d, s, n);
    return d;
}

// Grow an arena array (the old copy stays in the arena until the end).
static void *agrow(Imp *I, void *p, int *cap, int need, size_t size) {
    if (need <= *cap) return p;
    int c = *cap ? *cap * 2 : 8;
    while (c < need) c *= 2;
    void *q = alloc(I, (size_t) c * size);
    if (p) memcpy(q, p, (size_t) *cap * size);
    *cap = c;
    return q;
}

// Grow a malloc'd array of `size`-byte elements held in Imp (freed at the end).
static void *grow(Imp *I, void *p, int *cap, int need, size_t size) {
    if (need <= *cap) return p;
    int c = *cap ? *cap * 2 : 16;
    while (c < need) c *= 2;
    void *q = realloc(p, (size_t) c * size);
    if (!q) fail(I, 0, 0, "Out of memory.");
    *cap = c;
    return q;
}

// ---------------------------------------------------------------------------
// Output buffer

static void bput(Imp *I, Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p) fail(I, 0, 0, "Out of memory.");
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] == '\n') b->line++;
}

static void bputs(Imp *I, Buf *b, const char *s) { bput(I, b, s, strlen(s)); }

static void indent(Imp *I, Buf *b, int ind) {
    for (int i = 0; i < ind; i++) bputs(I, b, "    ");
}

// ---------------------------------------------------------------------------
// Lexer (the body after the prelude)

static int is_ident_start(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$'; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_hex(int c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int is_ident_char(int c) { return is_ident_start(c) || is_digit(c); }

static void push_tok(Imp *I, int kind, const char *s, int len, int line, int col) {
    I->toks = grow(I, I->toks, &I->cap_tok, I->ntok + 1, sizeof *I->toks);
    I->toks[I->ntok++] = (Tok) {kind, s, len, line, col};
}

// A KrunkScript number literal (1.2): the compiler copied numbers verbatim.
static int ks_number_ok(const char *s, int n) {
    int i = 0;
    if (n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for (i = 2; i < n; i++)
            if (!is_hex((unsigned char) s[i])) return 0;
        return 1;
    }
    int digits = 0;
    while (i < n && is_digit((unsigned char) s[i])) i++, digits++;
    if (i < n && s[i] == '.') {
        i++;
        int frac = 0;
        while (i < n && is_digit((unsigned char) s[i])) i++, frac++;
        if (!frac) return 0;
        digits += frac;
    }
    if (!digits) return 0;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) i++;
        int e = 0;
        while (i < n && is_digit((unsigned char) s[i])) i++, e++;
        if (!e) return 0;
    }
    return i == n;
}

static void lex(Imp *I, size_t i, int line) {
    const char *src = I->src;
    const size_t n = I->len;
    int col = 1;
    static const char *const punct[] = {
        ">>>=", "===", "!==", "**=", "<<=", ">>=", ">>>", "=>", "==", "!=", "<=", ">=", "&&", "||", "++", "--",
        "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "**", "<<", ">>",
        "{", "}", "(", ")", "[", "]", ";", ",", ".", ":", "?", "=", "+", "-", "*", "/", "%", "<", ">", "&", "|",
        "^", "~", "!",
    };
    for (;;) {
        // whitespace and comments
        while (i < n) {
            const int c = (unsigned char) src[i];
            if (c == '\n') {
                line++;
                col = 1;
                i++;
            } else if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') {
                i++;
                col++;
            } else if (c == '/' && i + 1 < n && src[i + 1] == '/') {
                while (i < n && src[i] != '\n') i++;
            } else if (c == '/' && i + 1 < n && src[i + 1] == '*') {
                const int l0 = line, c0 = col;
                i += 2;
                col += 2;
                while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) {
                    if (src[i] == '\n') line++, col = 1;
                    else col++;
                    i++;
                }
                if (i + 1 >= n) fail(I, l0, c0, "Unterminated comment.");
                i += 2;
                col += 2;
            } else {
                break;
            }
        }
        if (i >= n) {
            push_tok(I, TK_EOF, src + n, 0, line, col);
            return;
        }
        const int c = (unsigned char) src[i];
        const size_t st = i;
        if (is_ident_start(c)) {
            while (i < n && is_ident_char((unsigned char) src[i])) i++;
            push_tok(I, TK_ID, src + st, (int) (i - st), line, col);
        } else if (is_digit(c) || (c == '.' && i + 1 < n && is_digit((unsigned char) src[i + 1]))) {
            const int hex = c == '0' && i + 1 < n && (src[i + 1] == 'x' || src[i + 1] == 'X');
            while (i < n) {
                const int d = (unsigned char) src[i];
                if (is_ident_char(d) || d == '.') i++;
                else if (!hex && (d == '+' || d == '-') && (src[i - 1] == 'e' || src[i - 1] == 'E')) i++;
                else break;
            }
            if (!ks_number_ok(src + st, (int) (i - st)))
                fail(I, line, col, "Unsupported number literal \"%.*s\".", (int) (i - st), src + st);
            push_tok(I, TK_NUM, src + st, (int) (i - st), line, col);
        } else if (c == '"' || c == '\'') {
            // Copied verbatim into KrunkScript, so every escape must be one KrunkScript has (1.2).
            i++;
            while (i < n && (unsigned char) src[i] != c) {
                if (src[i] == '\n' || src[i] == '\r') fail(I, line, col, "Unterminated string.");
                if (src[i] == '\\') {
                    if (i + 1 >= n) break;
                    const int e = (unsigned char) src[i + 1];
                    int ok = 1, skip = 2;
                    if (strchr("'\"\\nrtbfv", e)) {
                    } else if (e == '0') {
                        ok = !(i + 2 < n && is_digit((unsigned char) src[i + 2]));
                    } else if (e == 'x') {
                        ok = i + 3 < n && is_hex((unsigned char) src[i + 2]) && is_hex((unsigned char) src[i + 3]);
                        skip = 4;
                    } else if (e == 'u') {
                        ok = i + 5 < n;
                        for (int k = 2; ok && k < 6; k++) ok = is_hex((unsigned char) src[i + k]);
                        skip = 6;
                    } else {
                        ok = 0; // line continuations, \u{...}, identity escapes
                    }
                    if (!ok) fail(I, line, col, "Unsupported escape sequence in a string literal.");
                    i += (size_t) skip;
                    continue;
                }
                i++;
            }
            if (i >= n) fail(I, line, col, "Unterminated string.");
            i++;
            push_tok(I, TK_STR, src + st, (int) (i - st), line, col);
        } else {
            int len = 0;
            for (size_t k = 0; k < sizeof punct / sizeof punct[0]; k++) {
                const size_t pl = strlen(punct[k]);
                if (i + pl <= n && memcmp(src + i, punct[k], pl) == 0) {
                    len = (int) pl;
                    break;
                }
            }
            if (!len) {
                if (c == '`') fail(I, line, col, "Template literals are not part of the compiled KrunkScript shape.");
                fail(I, line, col, "Unexpected character \"%c\".", c >= 0x20 && c < 0x7f ? c : '?');
            }
            i += (size_t) len;
            push_tok(I, TK_PUNCT, src + st, len, line, col);
        }
        col += (int) (i - st);
    }
}

// ---------------------------------------------------------------------------
// Parser helpers

static Tok *cur(Imp *I) { return &I->toks[I->pos]; }
static Tok *peekn(Imp *I, int k) {
    const int i = I->pos + k;
    return &I->toks[i < I->ntok ? i : I->ntok - 1];
}
static int is_p(const Tok *t, const char *p) {
    return t->kind == TK_PUNCT && (size_t) t->len == strlen(p) && memcmp(t->s, p, (size_t) t->len) == 0;
}
static int is_id(const Tok *t, const char *p) {
    return t->kind == TK_ID && (size_t) t->len == strlen(p) && memcmp(t->s, p, (size_t) t->len) == 0;
}
static int acc(Imp *I, const char *p) {
    if (!is_p(cur(I), p)) return 0;
    I->pos++;
    return 1;
}

static void unexpected(Imp *I, const Tok *t, const char *want) {
    if (t->kind == TK_EOF) fail(I, t->line, t->col, "Unexpected end of the compiled script.");
    const int len = t->len > 40 ? 40 : t->len;
    if (want) fail(I, t->line, t->col, "Unrecognised compiled JavaScript: expected \"%s\", found \"%.*s\".", want, len, t->s);
    fail(I, t->line, t->col, "Unrecognised compiled JavaScript at \"%.*s\".", len, t->s);
}

static void expect_p(Imp *I, const char *p) {
    if (!acc(I, p)) unexpected(I, cur(I), p);
}
static void expect_id(Imp *I, const char *name) {
    if (!is_id(cur(I), name)) unexpected(I, cur(I), name);
    I->pos++;
}

static Nd *mk(Imp *I, int k, const Tok *at) {
    Nd *n = alloc(I, sizeof *n);
    n->k = k;
    n->line = at->line;
    n->col = at->col;
    return n;
}

// A compiled user name: V_<name> -> <name>.
static const char *uname(Imp *I, const Tok *t) {
    if (t->kind != TK_ID) unexpected(I, t, "a name");
    if (t->len < 3 || memcmp(t->s, "V_", 2) != 0)
        fail(I, t->line, t->col, "Unrecognised identifier \"%.*s\" (compiled KrunkScript names start with V_).",
             t->len > 40 ? 40 : t->len, t->s);
    return dup_n(I, t->s + 2, (size_t) t->len - 2);
}

static int int_lit(Imp *I) {
    Tok *t = cur(I);
    if (t->kind != TK_NUM) unexpected(I, t, "a line number");
    I->pos++;
    return (int) strtol(t->s, NULL, 10);
}

static void note_hint(Imp *I, int line) {
    if (line > 0 && (I->hint == 0 || line < I->hint)) I->hint = line;
}

static void push_item(Imp *I, Nd *list, Nd *item, int *cap) {
    list->items = agrow(I, list->items, cap, list->n + 1, sizeof *list->items);
    list->items[list->n++] = item;
}

// Structural equality (BC's array operand must be the indexed expression).
static int same(const Nd *a, const Nd *b) {
    if (!a || !b) return a == b;
    if (a->k != b->k || a->n != b->n || a->len != b->len || a->cast != b->cast) return 0;
    if ((a->op || b->op) && (!a->op || !b->op || strcmp(a->op, b->op) != 0)) return 0;
    if (a->s || b->s) {
        if (!a->s || !b->s) return 0;
        if (a->len ? memcmp(a->s, b->s, (size_t) a->len) != 0 : strcmp(a->s, b->s) != 0) return 0;
    }
    if (!same(a->a, b->a) || !same(a->b, b->b) || !same(a->c, b->c)) return 0;
    for (int i = 0; i < a->n; i++)
        if (!same(a->items[i], b->items[i])) return 0;
    return 1;
}

// ---------------------------------------------------------------------------
// Expressions

static Nd *expr(Imp *I);

static void call_args(Imp *I, Nd *call) {
    int cap = 0;
    if (acc(I, ")")) return;
    for (;;) {
        push_item(I, call, expr(I), &cap);
        if (acc(I, ")")) return;
        expect_p(I, ",");
        if (acc(I, ")")) return; // trailing comma
    }
}

// The helper call `name(` at the cursor: returns its argument count after parsing them into out.
static int helper_args(Imp *I, Nd **out, int max) {
    I->pos += 2; // name (
    int n = 0;
    if (acc(I, ")")) return 0;
    for (;;) {
        Nd *e = expr(I);
        if (n < max) out[n] = e;
        n++;
        if (acc(I, ")")) return n;
        expect_p(I, ",");
    }
}

static uint8_t cast_of(const Tok *t) {
    if (is_id(t, "F")) return KS_T_NUM;
    if (is_id(t, "S")) return KS_T_STR;
    if (is_id(t, "B")) return KS_T_BOOL;
    if (is_id(t, "O")) return KS_T_OBJ;
    return 0xff;
}

static Nd *primary(Imp *I) {
    Tok *t = cur(I);
    Nd *n;
    if (t->kind == TK_NUM || t->kind == TK_STR) {
        n = mk(I, t->kind == TK_NUM ? E_NUM : E_STR, t);
        n->s = t->s;
        n->len = t->len;
        I->pos++;
        return n;
    }
    if (is_p(t, "(")) {
        I->pos++;
        n = mk(I, E_PAREN, t);
        n->a = expr(I);
        expect_p(I, ")");
        return n;
    }
    if (is_p(t, "[")) {
        I->pos++;
        n = mk(I, E_ARR, t);
        int cap = 0;
        while (!acc(I, "]")) {
            push_item(I, n, expr(I), &cap);
            if (acc(I, "]")) break;
            expect_p(I, ",");
        }
        return n;
    }
    if (is_p(t, "{")) {
        I->pos++;
        n = mk(I, E_OBJ, t);
        int cap = 0, kcap = 0;
        while (!acc(I, "}")) {
            Tok *k = cur(I);
            const char *key;
            if (k->kind == TK_STR) {
                if (k->len < 5 || memcmp(k->s + 1, "V_", 2) != 0)
                    fail(I, k->line, k->col, "Unrecognised object key %.*s.", k->len > 40 ? 40 : k->len, k->s);
                for (int i = 1; i < k->len - 1; i++)
                    if (k->s[i] == '\\') fail(I, k->line, k->col, "Unsupported escaped object key.");
                key = dup_n(I, k->s + 3, (size_t) k->len - 4);
            } else {
                key = uname(I, k);
            }
            I->pos++;
            expect_p(I, ":");
            n->keys = agrow(I, n->keys, &kcap, n->n + 1, sizeof *n->keys);
            n->keys[n->n] = key;
            push_item(I, n, expr(I), &cap);
            if (acc(I, "}")) break;
            expect_p(I, ",");
        }
        return n;
    }
    if (t->kind != TK_ID) unexpected(I, t, NULL);

    if (is_id(t, "true") || is_id(t, "false")) {
        n = mk(I, E_BOOL, t);
        n->len = is_id(t, "true");
        I->pos++;
        return n;
    }
    if (is_id(t, "this")) {
        if (!is_p(peekn(I, 1), ".") || !is_id(peekn(I, 2), "GAME"))
            fail(I, t->line, t->col, "\"this\" is only recognised as this.GAME.");
        I->pos += 3;
        return mk(I, E_GAME, t);
    }
    if (is_p(peekn(I, 1), "(")) {
        Nd *args[4];
        if (is_id(t, "L") || is_id(t, "NE") || is_id(t, "S") || is_id(t, "B") || is_id(t, "O") || is_id(t, "F")) {
            const int is_len = is_id(t, "L"), is_ne = is_id(t, "NE");
            const uint8_t cast = cast_of(t);
            if (helper_args(I, args, 4) != 1) fail(I, t->line, t->col, "%.*s() takes one argument.", t->len, t->s);
            n = mk(I, is_len ? E_LEN : is_ne ? E_NE : E_CAST, t);
            n->cast = is_len || is_ne ? 0 : cast;
            n->a = args[0];
            return n;
        }
        if (is_id(t, "A")) {
            // A(x, f): a cast, element-wise on arrays; f is one of the scalar helpers
            I->pos += 2;
            Nd *x = expr(I);
            expect_p(I, ",");
            const uint8_t cast = cast_of(cur(I));
            if (cast == 0xff) unexpected(I, cur(I), "F, S, B or O");
            I->pos++;
            expect_p(I, ")");
            n = mk(I, E_CAST, t);
            n->cast = cast;
            n->from_a = 1;
            n->a = x;
            return n;
        }
        if (is_id(t, "HP")) {
            // HP(o, "V_k") = hasProp o.k
            I->pos += 2;
            Nd *o = expr(I);
            expect_p(I, ",");
            Tok *k = cur(I);
            if (k->kind != TK_STR || k->len < 5 || memcmp(k->s + 1, "V_", 2) != 0) unexpected(I, k, "\"V_<name>\"");
            for (int i = 3; i < k->len - 1; i++)
                if (!is_ident_char((unsigned char) k->s[i])) fail(I, k->line, k->col, "Unrecognised property name in HP().");
            I->pos++;
            expect_p(I, ")");
            n = mk(I, E_HP, t);
            n->a = o;
            n->s = dup_n(I, k->s + 3, (size_t) k->len - 4);
            return n;
        }
        if (is_id(t, "FC")) {
            // FC(o.V_f, "f", line, col)(args) = o.f(args), a checked method call
            I->pos += 2;
            Nd *m = expr(I);
            expect_p(I, ",");
            Tok *k = cur(I);
            if (k->kind != TK_STR) unexpected(I, k, "the property name");
            I->pos++;
            expect_p(I, ",");
            note_hint(I, int_lit(I));
            expect_p(I, ",");
            int_lit(I);
            expect_p(I, ")");
            if (m->k != E_MEMBER || strlen(m->s) != (size_t) k->len - 2 || memcmp(m->s, k->s + 1, (size_t) k->len - 2) != 0)
                fail(I, t->line, t->col, "FC() does not check the member it names.");
            if (!is_p(cur(I), "(")) unexpected(I, cur(I), "(");
            return m;
        }
        if (is_id(t, "DE")) {
            // DE(a, i) = remove a[i] (statements only)
            if (helper_args(I, args, 4) != 2) fail(I, t->line, t->col, "DE() takes two arguments.");
            n = mk(I, E_DE, t);
            n->a = args[0];
            n->b = args[1];
            return n;
        }
        if (t->len <= 2 && t->s[0] >= 'A' && t->s[0] <= 'Z')
            fail(I, t->line, t->col, "Helper %.*s() outside of its compiled pattern.", t->len, t->s);
    }
    n = mk(I, E_ID, t);
    n->s = uname(I, t);
    I->pos++;
    return n;
}

static Nd *postfix(Imp *I) {
    Nd *e = primary(I);
    for (;;) {
        Tok *t = cur(I);
        if (is_p(t, ".")) {
            Tok *nm = peekn(I, 1);
            if (is_id(nm, "bind") && is_p(peekn(I, 2), "(") && is_id(peekn(I, 3), "this") && is_p(peekn(I, 4), ")")) {
                I->pos += 5; // x.bind(this): calls through it are plain calls, alone it is an action value
                continue;
            }
            if (is_id(nm, "push") && is_p(peekn(I, 2), "(")) {
                // a.push(v) = addTo a v (statements only)
                I->pos += 3;
                Nd *n = mk(I, E_PUSH, t);
                n->a = e;
                n->b = expr(I);
                expect_p(I, ")");
                e = n;
                continue;
            }
            Nd *n = mk(I, E_MEMBER, nm);
            n->a = e;
            n->s = uname(I, nm);
            I->pos += 2;
            e = n;
            continue;
        }
        if (is_p(t, "[")) {
            I->pos++;
            Nd *idx;
            if (is_id(cur(I), "BC") && is_p(peekn(I, 1), "(")) {
                // a[BC(a, i, line, col)] = a[i] (bounds-checked)
                I->pos += 2;
                Nd *arr = expr(I);
                expect_p(I, ",");
                idx = expr(I);
                expect_p(I, ",");
                note_hint(I, int_lit(I));
                expect_p(I, ",");
                int_lit(I);
                expect_p(I, ")");
                if (!same(arr, e)) fail(I, t->line, t->col, "BC() checks a different array than the one indexed.");
            } else {
                idx = expr(I);
            }
            expect_p(I, "]");
            Nd *n = mk(I, E_INDEX, t);
            n->a = e;
            n->b = idx;
            e = n;
            continue;
        }
        if (is_p(t, "(")) {
            I->pos++;
            Nd *n = mk(I, E_CALL, t);
            n->a = e;
            call_args(I, n);
            e = n;
            continue;
        }
        return e;
    }
}

static Nd *unary(Imp *I) {
    Tok *t = cur(I);
    if (is_p(t, "!") || is_p(t, "-") || is_p(t, "+") || is_p(t, "~")) {
        I->pos++;
        Nd *n = mk(I, E_UNARY, t);
        n->op = is_p(t, "!") ? "!" : is_p(t, "-") ? "-" : is_p(t, "+") ? "+" : "~";
        if (++I->depth > MAX_DEPTH) fail(I, t->line, t->col, "Expression nested too deeply.");
        n->a = unary(I);
        I->depth--;
        return n;
    }
    if (is_p(t, "++") || is_p(t, "--")) fail(I, t->line, t->col, "\"%.*s\" inside an expression.", t->len, t->s);
    return postfix(I);
}

static int bin_prec(const Tok *t, const char **op) {
    static const struct { const char *js, *ks; int prec; } ops[] = {
        {"||", "||", 1}, {"&&", "&&", 2}, {"|", "|", 3}, {"^", "^", 4}, {"&", "&", 5},
        {"===", "==", 6}, {"!==", "!=", 6}, {"==", NULL, 6}, {"!=", NULL, 6},
        {"<", "<", 7}, {"<=", "<=", 7}, {">", ">", 7}, {">=", ">=", 7},
        {"<<", "<<", 8}, {">>", ">>", 8}, {">>>", ">>>", 8}, {"+", "+", 9}, {"-", "-", 9},
        {"*", "*", 10}, {"/", "/", 10}, {"%", "%", 10}, {"**", "**", 11},
    };
    if (t->kind != TK_PUNCT) return 0;
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        if (is_p(t, ops[i].js)) {
            *op = ops[i].ks;
            return ops[i].prec;
        }
    }
    return 0;
}

static Nd *binary(Imp *I, int min_prec) {
    Nd *l = unary(I);
    for (;;) {
        Tok *t = cur(I);
        const char *op = NULL;
        const int p = bin_prec(t, &op);
        if (!p || p < min_prec) return l;
        // KrunkScript's == is strict: the compiler only ever emits === / !== (1.2)
        if (!op) fail(I, t->line, t->col, "Loose equality \"%.*s\" is not compiled KrunkScript (expected === or !==).", t->len, t->s);
        I->pos++;
        Nd *n = mk(I, p <= 2 ? E_LOGIC : E_BIN, t);
        n->op = op;
        n->a = l;
        n->b = binary(I, p == 11 ? p : p + 1); // ** is right-associative
        l = n;
    }
}

static Nd *expr(Imp *I) {
    Tok *t = cur(I);
    if (++I->depth > MAX_DEPTH) fail(I, t->line, t->col, "Expression nested too deeply.");
    Nd *c = binary(I, 1);
    if (is_p(cur(I), "?")) {
        Tok *q = cur(I);
        I->pos++;
        Nd *n = mk(I, E_TERN, q);
        n->a = c;
        n->b = expr(I);
        expect_p(I, ":");
        n->c = expr(I);
        c = n;
    }
    I->depth--;
    return c;
}

// ---------------------------------------------------------------------------
// Statements

static Nd *stmt(Imp *I);

static Nd *strip_paren(Nd *e) {
    while (e && e->k == E_PAREN) e = e->a;
    return e;
}

static Nd *lvalue(Imp *I, Nd *e) {
    Nd *t = strip_paren(e);
    if (t->k != E_ID && t->k != E_MEMBER && t->k != E_INDEX)
        fail(I, e->line, e->col, "Unrecognised assignment target.");
    return t;
}

static int is_assign_op(const Tok *t) {
    static const char *const ops[] = {"=", "+=", "-=", "*=", "/=", "%=", "**=", "<<=", ">>=", ">>>=", "&=", "|=", "^="};
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
        if (is_p(t, ops[i])) return 1;
    return 0;
}

// Assignment, ++/--, a call, addTo (push) or remove (DE); no trailing ";".
static Nd *simple(Imp *I) {
    Tok *t = cur(I);
    if (is_p(t, "++") || is_p(t, "--")) {
        I->pos++;
        Nd *n = mk(I, S_INCDEC, t);
        n->op = is_p(t, "++") ? "++" : "--";
        n->prefix = 1;
        n->a = lvalue(I, postfix(I));
        return n;
    }
    Nd *e = expr(I);
    Tok *o = cur(I);
    if (is_assign_op(o)) {
        I->pos++;
        Nd *n = mk(I, S_ASSIGN, t);
        n->op = dup_n(I, o->s, (size_t) o->len);
        n->a = lvalue(I, e);
        n->b = expr(I);
        return n;
    }
    if (is_p(o, "++") || is_p(o, "--")) {
        I->pos++;
        Nd *n = mk(I, S_INCDEC, t);
        n->op = is_p(o, "++") ? "++" : "--";
        n->a = lvalue(I, e);
        return n;
    }
    Nd *n;
    switch (e->k) {
    case E_PUSH:
        n = mk(I, S_ADDTO, t);
        n->a = e->a;
        n->b = e->b;
        return n;
    case E_DE: {
        n = mk(I, S_REMOVE, t);
        Nd *ix = mk(I, E_INDEX, t);
        ix->a = e->a;
        ix->b = e->b;
        n->a = ix;
        return n;
    }
    case E_CALL:
        n = mk(I, S_EXPR, t);
        n->a = e;
        return n;
    default:
        fail(I, t->line, t->col, "Unrecognised expression statement (not a call or an assignment).");
    }
    return NULL;
}

static Nd *block_until_brace(Imp *I, const Tok *at) {
    Nd *b = mk(I, S_BLOCK, at);
    int cap = 0;
    while (!acc(I, "}")) {
        if (cur(I)->kind == TK_EOF) unexpected(I, cur(I), "}");
        push_item(I, b, stmt(I), &cap);
    }
    return b;
}

static Nd *let_decl(Imp *I) {
    Tok *t = cur(I);
    I->pos++; // let / const
    Nd *n = mk(I, S_VAR, t);
    n->s = uname(I, cur(I));
    I->pos++;
    if (!acc(I, "=")) fail(I, t->line, t->col, "Declaration of \"%s\" without an initialiser (its type cannot be recovered).", n->s);
    n->a = expr(I);
    return n;
}

// A loop body: drops the leading `if (LC++ > 1e+6) RE(l,c,"Loop timed out.");` guard and
// returns its line (the loop keyword's line in the original source).
static int take_guard(Nd *body) {
    if (!body || body->k != S_BLOCK || body->n == 0 || body->items[0]->k != S_GUARD) return 0;
    const int line = body->items[0]->hint;
    body->items[0]->k = S_EMPTY;
    return line;
}

static Nd *stmt_inner(Imp *I) {
    Tok *t = cur(I);
    Nd *n;
    if (is_p(t, "{")) {
        I->pos++;
        return block_until_brace(I, t);
    }
    if (is_p(t, ";")) {
        I->pos++;
        return mk(I, S_EMPTY, t);
    }
    if (t->kind != TK_ID) {
        n = simple(I);
        expect_p(I, ";");
        return n;
    }
    if (is_id(t, "let") || is_id(t, "const")) {
        n = let_decl(I);
        expect_p(I, ";");
        return n;
    }
    if (is_id(t, "if")) {
        if (is_p(peekn(I, 1), "(") && is_id(peekn(I, 2), "LC")) {
            // loop guard
            I->pos += 3;
            expect_p(I, "++");
            expect_p(I, ">");
            if (cur(I)->kind != TK_NUM) unexpected(I, cur(I), "1e+6");
            I->pos++;
            expect_p(I, ")");
            expect_id(I, "RE");
            expect_p(I, "(");
            n = mk(I, S_GUARD, t);
            n->hint = int_lit(I);
            expect_p(I, ",");
            int_lit(I);
            expect_p(I, ",");
            if (cur(I)->kind != TK_STR) unexpected(I, cur(I), "\"Loop timed out.\"");
            I->pos++;
            expect_p(I, ")");
            expect_p(I, ";");
            return n;
        }
        I->pos++;
        n = mk(I, S_IF, t);
        expect_p(I, "(");
        n->a = expr(I);
        expect_p(I, ")");
        n->b = stmt(I);
        if (is_id(cur(I), "else")) {
            I->pos++;
            n->c = stmt(I);
        }
        return n;
    }
    if (is_id(t, "for")) {
        I->pos++;
        expect_p(I, "(");
        if ((is_id(cur(I), "let") || is_id(cur(I), "const")) && is_id(peekn(I, 2), "of")) {
            // for (let V_e of OE(x)) = for (obj e : x)
            n = mk(I, S_FOREACH, t);
            I->pos++;
            n->s = uname(I, cur(I));
            I->pos += 2;
            expect_id(I, "OE");
            expect_p(I, "(");
            n->a = expr(I);
            expect_p(I, ")");
            expect_p(I, ")");
        } else {
            n = mk(I, S_FOR, t);
            if (!acc(I, ";")) {
                n->a = is_id(cur(I), "let") ? let_decl(I) : simple(I);
                expect_p(I, ";");
            }
            if (!acc(I, ";")) {
                n->b = expr(I);
                expect_p(I, ";");
            }
            if (!acc(I, ")")) {
                n->c = simple(I);
                expect_p(I, ")");
            }
        }
        n->d = stmt(I);
        note_hint(I, take_guard(n->d));
        return n;
    }
    if (is_id(t, "while")) {
        I->pos++;
        n = mk(I, S_WHILE, t);
        expect_p(I, "(");
        n->a = expr(I);
        expect_p(I, ")");
        n->d = stmt(I);
        note_hint(I, take_guard(n->d));
        return n;
    }
    if (is_id(t, "return")) {
        I->pos++;
        n = mk(I, S_RET, t);
        if (!acc(I, ";")) {
            n->a = expr(I);
            expect_p(I, ";");
        }
        return n;
    }
    if (is_id(t, "break") || is_id(t, "continue")) {
        I->pos++;
        expect_p(I, ";");
        return mk(I, is_id(t, "break") ? S_BREAK : S_CONT, t);
    }
    if (is_id(t, "delete")) {
        // delete o.V_k = remove o.k
        I->pos++;
        n = mk(I, S_REMOVE, t);
        n->a = strip_paren(postfix(I));
        if (n->a->k != E_MEMBER) fail(I, t->line, t->col, "Unrecognised delete target.");
        expect_p(I, ";");
        return n;
    }
    static const char *const unsupported[] = {"var", "function", "try", "catch", "throw", "switch", "do", "class",
                                              "new", "typeof", "void", "yield", "await", "with", "import", "export"};
    for (size_t i = 0; i < sizeof unsupported / sizeof unsupported[0]; i++)
        if (is_id(t, unsupported[i]))
            fail(I, t->line, t->col, "Unrecognised compiled JavaScript: \"%s\" statement.", unsupported[i]);
    n = simple(I);
    expect_p(I, ";");
    return n;
}

// Statements record the smallest original line seen in their own head (nested statements
// restore the outer value), the emitter pads the output up to that line.
static Nd *stmt(Imp *I) {
    Tok *t = cur(I);
    if (++I->depth > MAX_DEPTH) fail(I, t->line, t->col, "Statements nested too deeply.");
    const int outer = I->hint;
    I->hint = 0;
    Nd *n = stmt_inner(I);
    if (n->k != S_GUARD) n->hint = I->hint;
    I->hint = outer;
    I->depth--;
    return n;
}

// ---------------------------------------------------------------------------
// Top level: globals, functions, the module epilogue

static Fn *find_fn(Imp *I, const char *name) {
    for (int i = 0; i < I->nfns; i++)
        if (strcmp(I->fns[i].name, name) == 0) return &I->fns[i];
    return NULL;
}

static void parse_function(Imp *I) {
    Tok *ft = cur(I);
    I->pos++; // function
    Tok *nt = cur(I);
    const char *name = uname(I, nt);
    I->pos++;
    if (find_fn(I, name)) fail(I, nt->line, nt->col, "Action \"%s\" is defined twice.", name);
    I->fns = grow(I, I->fns, &I->cap_fns, I->nfns + 1, sizeof *I->fns);
    Fn *f = &I->fns[I->nfns++];
    memset(f, 0, sizeof *f);
    f->name = name;
    f->line = ft->line;
    f->col = ft->col;
    expect_p(I, "(");
    int cap = 0;
    while (!acc(I, ")")) {
        if (f->nparams) expect_p(I, ",");
        f->params = agrow(I, f->params, &cap, f->nparams + 1, sizeof *f->params);
        f->params[f->nparams++] = uname(I, cur(I));
        I->pos++;
    }
    Tok *bt = cur(I);
    expect_p(I, "{");
    if (is_id(cur(I), "try") && is_p(peekn(I, 1), "{") && is_id(peekn(I, 2), "LC") && is_p(peekn(I, 3), "=") &&
        peekn(I, 4)->kind == TK_NUM && peekn(I, 4)->len == 1 && peekn(I, 4)->s[0] == '0' && is_p(peekn(I, 5), ";")) {
        // try { LC = 0; <body> } catch (e) {EH(e.message)}; (public actions)
        I->pos += 6;
        f->body = block_until_brace(I, bt);
        expect_id(I, "catch");
        expect_p(I, "(");
        Tok *ev = cur(I);
        if (ev->kind != TK_ID) unexpected(I, ev, "the catch variable");
        I->pos++;
        expect_p(I, ")");
        expect_p(I, "{");
        expect_id(I, "EH");
        expect_p(I, "(");
        if (cur(I)->kind != TK_ID || cur(I)->len != ev->len || memcmp(cur(I)->s, ev->s, (size_t) ev->len) != 0)
            unexpected(I, cur(I), "the catch variable");
        I->pos++;
        expect_p(I, ".");
        expect_id(I, "message");
        expect_p(I, ")");
        acc(I, ";");
        expect_p(I, "}");
        acc(I, ";");
        expect_p(I, "}");
    } else {
        f->body = block_until_brace(I, bt);
    }
}

// return {hook: V_x.bind(this), other: function(){}, ...};
static void parse_epilogue(Imp *I) {
    I->pos += 2; // return {
    while (!acc(I, "}")) {
        Tok *k = cur(I);
        if (k->kind != TK_ID) unexpected(I, k, "an exported name");
        I->pos++;
        expect_p(I, ":");
        if (is_id(cur(I), "function")) {
            I->pos++;
            expect_p(I, "(");
            expect_p(I, ")");
            expect_p(I, "{");
            expect_p(I, "}");
        } else {
            Tok *v = cur(I);
            const char *name = uname(I, v);
            I->pos++;
            expect_p(I, ".");
            expect_id(I, "bind");
            expect_p(I, "(");
            expect_id(I, "this");
            expect_p(I, ")");
            if (strlen(name) != (size_t) k->len || memcmp(name, k->s, (size_t) k->len) != 0)
                fail(I, k->line, k->col, "Export \"%.*s\" does not name its own action.", k->len, k->s);
            Fn *f = find_fn(I, name);
            if (!f) fail(I, v->line, v->col, "Export of an undefined action \"%s\".", name);
            f->is_public = 1;
        }
        if (acc(I, "}")) break;
        expect_p(I, ",");
    }
    acc(I, ";");
    I->have_epilogue = 1;
    if (cur(I)->kind != TK_EOF) unexpected(I, cur(I), "the end of the script");
}

static void parse_program(Imp *I) {
    while (cur(I)->kind != TK_EOF) {
        Tok *t = cur(I);
        if (acc(I, ";")) continue;
        if (is_id(t, "let") || is_id(t, "const")) {
            const int outer = I->hint;
            I->hint = 0;
            Nd *n = let_decl(I);
            expect_p(I, ";");
            n->hint = I->hint;
            I->hint = outer;
            I->globals = grow(I, I->globals, &I->cap_globals, I->nglobals + 1, sizeof *I->globals);
            I->globals[I->nglobals++] = n;
        } else if (is_id(t, "function")) {
            parse_function(I);
        } else if (is_id(t, "return") && is_p(peekn(I, 1), "{")) {
            parse_epilogue(I);
            return;
        } else {
            unexpected(I, t, NULL);
        }
    }
    fail(I, cur(I)->line, cur(I)->col, "Missing the module epilogue (return {...}).");
}

// ---------------------------------------------------------------------------
// Types

typedef struct { const char *name; const char *sig; } HookSig; // sig: n num, s str, b bool, o obj

static const HookSig g_client_hooks[] = {
    {"start", ""}, {"update", "n"}, {"render", "n"}, {"onPlayerSpawn", "s"}, {"onPlayerDeath", "ss"},
    {"onPlayerUpdate", "sno"}, {"onKeyPress", "sn"}, {"onKeyUp", "sn"}, {"onKeyHeld", "sn"},
    {"onControllerPress", "sn"}, {"onControllerUp", "sn"}, {"onControllerHeld", "sn"},
    {"onMouseClick", "nnn"}, {"onMouseUp", "nnn"}, {"onMouseScroll", "n"}, {"onDIVClicked", "s"},
    {"onNetworkMessage", "so"},
};
static const HookSig g_server_hooks[] = {
    {"start", ""}, {"update", "n"}, {"onPlayerSpawn", "s"}, {"onPlayerDeath", "ss"}, {"onPlayerDamage", "ssn"},
    {"onPlayerUpdate", "sno"}, {"onCustomTrigger", "ssn"}, {"onNetworkMessage", "sos"}, {"onChatMessage", "ss"},
    {"onPlayerLeave", "s"}, {"onGameEnd", ""}, {"onAdFinished", "sb"}, {"onServerClosed", ""},
};

static const char *hook_sig(KsSide side, const char *name) {
    const HookSig *h = side == KS_SIDE_SERVER ? g_server_hooks : g_client_hooks;
    const size_t n = side == KS_SIDE_SERVER ? sizeof g_server_hooks / sizeof *h : sizeof g_client_hooks / sizeof *h;
    for (size_t i = 0; i < n; i++)
        if (strcmp(h[i].name, name) == 0) return h[i].sig;
    return NULL;
}

static KsType sig_type(char c) {
    return c == 'n' ? T_NUM : c == 's' ? T_STR : c == 'b' ? T_BOOL : T_OBJ;
}

static int known(KsType t) { return t.base != KS_T_ANY && t.base != KS_T_VOID; }
static int teq(KsType a, KsType b) { return a.base == b.base && a.dims == b.dims; }

static Var *lookup(Imp *I, const char *name) {
    for (int i = I->nvars - 1; i >= 0; i--)
        if (strcmp(I->vars[i].name, name) == 0) return &I->vars[i];
    return NULL;
}

static void declare(Imp *I, const char *name, KsType t) {
    I->vars = grow(I, I->vars, &I->cap_vars, I->nvars + 1, sizeof *I->vars);
    I->vars[I->nvars++] = (Var) {name, t, I->scope};
}

static void push_scope(Imp *I) { I->scope++; }
static void pop_scope(Imp *I) {
    while (I->nvars > 0 && I->vars[I->nvars - 1].depth == I->scope) I->nvars--;
    I->scope--;
}

// GAME.* path of a member chain rooted at this.GAME, UTILS or Math ("" for GAME itself).
static int api_path(const Nd *n, char *buf, size_t cap) {
    if (n->k == E_GAME) {
        buf[0] = 0;
        return 1;
    }
    if (n->k == E_ID && (strcmp(n->s, "UTILS") == 0 || strcmp(n->s, "Math") == 0)) {
        snprintf(buf, cap, "%s", strcmp(n->s, "UTILS") == 0 ? "UTILS" : "UTILS.math");
        return 1;
    }
    if (n->k != E_MEMBER || !api_path(n->a, buf, cap)) return 0;
    const size_t l = strlen(buf);
    snprintf(buf + l, cap - l, "%s%s", l ? "." : "", n->s);
    return 1;
}

static const KsApiEntry *api_of(const Nd *n) {
    char path[256];
    if (!api_path(n, path, sizeof path) || !path[0]) return NULL;
    const int id = ks_api_find(path);
    return id >= 0 ? ks_api_entry(id) : NULL;
}

static KsType infer(Imp *I, const Nd *n);

static KsType cast_type(Imp *I, const Nd *n) {
    KsType t = {n->cast, 0};
    if (n->from_a) {
        // A(x, f) maps arrays element-wise: (num[]) x for an array operand
        const KsType x = infer(I, n->a);
        if (x.dims > 0) t.dims = x.dims;
    }
    return t;
}

static KsType infer(Imp *I, const Nd *n) {
    switch (n->k) {
    case E_NUM: return T_NUM;
    case E_STR: return T_STR;
    case E_BOOL: return T_BOOL;
    case E_ID: {
        const Var *v = lookup(I, n->s);
        return v ? v->t : T_ANY;
    }
    case E_MEMBER: {
        const KsApiEntry *e = api_of(n);
        return e && e->kind == KS_API_KIND_PROP ? e->ret : T_ANY;
    }
    case E_INDEX: {
        KsType t = infer(I, n->a);
        if (t.dims == 0) return T_ANY;
        t.dims--;
        return t;
    }
    case E_CALL: {
        const KsApiEntry *e = api_of(n->a);
        if (e) return e->kind == KS_API_KIND_FN ? e->ret : T_ANY;
        if (n->a->k == E_ID && !lookup(I, n->a->s)) {
            const Fn *f = find_fn(I, n->a->s);
            if (f && f->done) return f->ret_set ? f->ret : T_VOID;
        }
        return T_ANY;
    }
    case E_UNARY: return n->op[0] == '!' ? T_BOOL : T_NUM;
    case E_BIN: {
        const char *op = n->op;
        if (!strcmp(op, "==") || !strcmp(op, "!=") || op[0] == '<' || op[0] == '>') {
            if (strcmp(op, "<<") && strcmp(op, ">>") && strcmp(op, ">>>")) return T_BOOL;
        }
        if (!strcmp(op, "+")) {
            const KsType a = infer(I, n->a), b = infer(I, n->b);
            if (teq(a, T_STR) || teq(b, T_STR)) return T_STR;
            if (teq(a, T_NUM) && teq(b, T_NUM)) return T_NUM;
            return T_ANY;
        }
        return T_NUM;
    }
    case E_LOGIC:
    case E_TERN: {
        const KsType a = infer(I, n->k == E_TERN ? n->b : n->a), b = infer(I, n->k == E_TERN ? n->c : n->b);
        return teq(a, b) ? a : T_ANY;
    }
    case E_ARR: {
        if (n->n == 0) return T_ANY;
        KsType t = infer(I, n->items[0]);
        for (int i = 1; i < n->n; i++)
            if (!teq(t, infer(I, n->items[i]))) return T_ANY;
        if (!known(t) || t.base == KS_T_ACTION) return T_ANY;
        t.dims++;
        return t;
    }
    case E_OBJ: return T_OBJ;
    case E_PAREN: return infer(I, n->a);
    case E_CAST: return cast_type(I, n);
    case E_LEN: return T_NUM;
    case E_NE:
    case E_HP: return T_BOOL;
    default: return T_ANY;
    }
}

// ---------------------------------------------------------------------------
// Emitter

static void emit_expr(Imp *I, Buf *b, const Nd *n, KsType expect);

static int is_postfixish(const Nd *n) {
    switch (n->k) {
    case E_NUM: case E_STR: case E_BOOL: case E_ID: case E_GAME: case E_MEMBER: case E_INDEX: case E_CALL:
    case E_PAREN: case E_ARR: case E_OBJ:
        return 1;
    default:
        return 0;
    }
}

// Operand of a cast/keyword or the object of . [] (): parenthesised unless it is a postfix form.
static void emit_tight(Imp *I, Buf *b, const Nd *n) {
    const int paren = !is_postfixish(n);
    if (paren) bputs(I, b, "(");
    emit_expr(I, b, n, T_ANY);
    if (paren) bputs(I, b, ")");
}

static void emit_type(Imp *I, Buf *b, KsType t) {
    char name[64];
    bputs(I, b, ks_type_name(t, name, sizeof name));
}

static int is_ks_ident(const char *s) {
    static const char *const kw[] = {"num", "str", "bool", "obj", "action", "public", "return", "if", "else", "for",
                                     "while", "break", "continue", "true", "false", "addTo", "remove", "lengthOf",
                                     "notEmpty", "hasProp", "toNum", "toStr", "static", "dynamic"};
    if (!is_ident_start((unsigned char) s[0]) || s[0] == '$') return 0;
    for (const char *p = s; *p; p++)
        if (!is_ident_char((unsigned char) *p) || *p == '$') return 0;
    for (size_t i = 0; i < sizeof kw / sizeof kw[0]; i++)
        if (strcmp(s, kw[i]) == 0) return 0;
    return 1;
}

static void emit_name(Imp *I, Buf *b, const Nd *at, const char *name) {
    if (!is_ks_ident(name)) fail(I, at->line, at->col, "\"%s\" is not a KrunkScript name.", name);
    bputs(I, b, name);
}

static void emit_expr(Imp *I, Buf *b, const Nd *n, KsType expect) {
    switch (n->k) {
    case E_NUM:
    case E_STR:
        bput(I, b, n->s, (size_t) n->len);
        return;
    case E_BOOL:
        bputs(I, b, n->len ? "true" : "false");
        return;
    case E_ID:
        emit_name(I, b, n, n->s);
        return;
    case E_GAME:
        bputs(I, b, "GAME");
        return;
    case E_MEMBER:
        if (n->a->k == E_GAME && n->s[0] && !api_of(n))
            fail(I, n->line, n->col, "Unknown GAME member \"%s\".", n->s);
        emit_tight(I, b, n->a);
        bputs(I, b, ".");
        emit_name(I, b, n, n->s);
        return;
    case E_INDEX:
        emit_tight(I, b, n->a);
        bputs(I, b, "[");
        emit_expr(I, b, n->b, T_ANY);
        bputs(I, b, "]");
        return;
    case E_CALL:
        emit_tight(I, b, n->a);
        bputs(I, b, "(");
        for (int i = 0; i < n->n; i++) {
            if (i) bputs(I, b, ", ");
            emit_expr(I, b, n->items[i], T_ANY);
        }
        bputs(I, b, ")");
        return;
    case E_UNARY:
        bputs(I, b, n->op);
        // "- -x" must not become the "--" token
        if ((n->op[0] == '-' || n->op[0] == '+') && n->a->k == E_UNARY && n->a->op[0] == n->op[0]) bputs(I, b, " ");
        emit_expr(I, b, n->a, T_ANY);
        return;
    case E_BIN:
    case E_LOGIC:
        // JS and KrunkScript share binary precedence and the compiler kept the parentheses;
        // only a cast/keyword left of ** needs its own (a unary left operand of ** is an error).
        if (!strcmp(n->op, "**")) emit_tight(I, b, n->a);
        else emit_expr(I, b, n->a, T_ANY);
        bputs(I, b, " ");
        bputs(I, b, n->op);
        bputs(I, b, " ");
        emit_expr(I, b, n->b, T_ANY);
        return;
    case E_TERN:
        emit_expr(I, b, n->a, T_ANY);
        bputs(I, b, " ? ");
        emit_expr(I, b, n->b, expect);
        bputs(I, b, " : ");
        emit_expr(I, b, n->c, expect);
        return;
    case E_ARR: {
        // KrunkScript array literals carry their element type: str["a", "b"], num[][num[1]]
        KsType t = infer(I, n);
        if (!known(t)) t = expect;
        if (!known(t) || t.dims == 0) fail(I, n->line, n->col, "Cannot recover the element type of this array literal.");
        KsType elem = t;
        elem.dims--;
        emit_type(I, b, elem);
        bputs(I, b, "[");
        for (int i = 0; i < n->n; i++) {
            if (i) bputs(I, b, ", ");
            emit_expr(I, b, n->items[i], elem);
        }
        bputs(I, b, "]");
        return;
    }
    case E_OBJ:
        bputs(I, b, "{");
        for (int i = 0; i < n->n; i++) {
            if (i) bputs(I, b, ", ");
            if (is_ks_ident(n->keys[i])) {
                bputs(I, b, n->keys[i]);
            } else {
                for (const char *p = n->keys[i]; *p; p++)
                    if (!is_ident_char((unsigned char) *p)) fail(I, n->line, n->col, "Unrecognised object key.");
                bputs(I, b, "\"");
                bputs(I, b, n->keys[i]);
                bputs(I, b, "\"");
            }
            bputs(I, b, ": ");
            emit_expr(I, b, n->items[i], T_ANY);
        }
        bputs(I, b, "}");
        return;
    case E_PAREN:
        bputs(I, b, "(");
        emit_expr(I, b, n->a, expect);
        bputs(I, b, ")");
        return;
    case E_CAST:
        bputs(I, b, "(");
        emit_type(I, b, cast_type(I, n));
        bputs(I, b, ") ");
        emit_tight(I, b, n->a);
        return;
    case E_LEN:
    case E_NE:
        bputs(I, b, n->k == E_LEN ? "lengthOf " : "notEmpty ");
        emit_tight(I, b, n->a);
        return;
    case E_HP:
        bputs(I, b, "hasProp ");
        emit_tight(I, b, n->a);
        bputs(I, b, ".");
        emit_name(I, b, n, n->s);
        return;
    case E_PUSH:
    case E_DE:
        fail(I, n->line, n->col, "addTo / remove used inside an expression.");
        return;
    default:
        fail(I, n->line, n->col, "Unrecognised expression.");
    }
}

static void emit_stmt(Imp *I, Buf *b, const Nd *s, int ind);

static void emit_body(Imp *I, Buf *b, const Nd *body, int ind) {
    bputs(I, b, "{\n");
    push_scope(I);
    if (body->k == S_BLOCK) {
        for (int i = 0; i < body->n; i++) emit_stmt(I, b, body->items[i], ind + 1);
    } else {
        emit_stmt(I, b, body, ind + 1);
    }
    pop_scope(I);
    indent(I, b, ind);
    bputs(I, b, "}");
}

// `T name = init` (no ";"), declared in the current scope.
static void emit_var(Imp *I, Buf *b, const Nd *s) {
    const KsType t = infer(I, s->a);
    if (!known(t) || t.base == KS_T_ACTION)
        fail(I, s->line, s->col, "Cannot recover the type of \"%s\" (its initialiser has no static type).", s->s);
    emit_type(I, b, t);
    bputs(I, b, " ");
    emit_name(I, b, s, s->s);
    bputs(I, b, " = ");
    emit_expr(I, b, s->a, t);
    declare(I, s->s, t);
}

static void emit_simple(Imp *I, Buf *b, const Nd *s) {
    switch (s->k) {
    case S_VAR:
        emit_var(I, b, s);
        return;
    case S_ASSIGN: {
        const KsType t = infer(I, s->a);
        // a compound assignment to a dynamic l-value needs its static type: (num) o.x += 1
        if (strcmp(s->op, "=") != 0 && !known(t)) {
            bputs(I, b, !strcmp(s->op, "+=") && teq(infer(I, s->b), T_STR) ? "(str) " : "(num) ");
        }
        emit_expr(I, b, s->a, T_ANY);
        bputs(I, b, " ");
        bputs(I, b, s->op);
        bputs(I, b, " ");
        emit_expr(I, b, s->b, t);
        return;
    }
    case S_INCDEC:
        if (s->prefix) bputs(I, b, s->op);
        if (!known(infer(I, s->a))) bputs(I, b, "(num) ");
        emit_expr(I, b, s->a, T_ANY);
        if (!s->prefix) bputs(I, b, s->op);
        return;
    case S_EXPR:
        emit_expr(I, b, s->a, T_ANY);
        return;
    default:
        fail(I, s->line, s->col, "Unrecognised statement in a for header.");
    }
}

static void emit_if(Imp *I, Buf *b, const Nd *s, int ind) {
    bputs(I, b, "if (");
    emit_expr(I, b, s->a, T_ANY);
    bputs(I, b, ") ");
    emit_body(I, b, s->b, ind);
    if (s->c) {
        bputs(I, b, " else ");
        if (s->c->k == S_IF) emit_if(I, b, s->c, ind);
        else emit_body(I, b, s->c, ind);
    }
}

static void emit_stmt(Imp *I, Buf *b, const Nd *s, int ind) {
    if (s->k == S_EMPTY) return;
    if (s->k == S_GUARD) fail(I, s->line, s->col, "Loop guard outside of a loop body.");
    while (s->hint > b->line) bputs(I, b, "\n");
    indent(I, b, ind);
    switch (s->k) {
    case S_BLOCK:
        emit_body(I, b, s, ind);
        break;
    case S_IF:
        emit_if(I, b, s, ind);
        break;
    case S_FOR:
        push_scope(I);
        bputs(I, b, "for (");
        if (s->a) emit_simple(I, b, s->a);
        bputs(I, b, "; ");
        if (s->b) emit_expr(I, b, s->b, T_ANY);
        bputs(I, b, "; ");
        if (s->c) emit_simple(I, b, s->c);
        bputs(I, b, ") ");
        emit_body(I, b, s->d, ind);
        pop_scope(I);
        break;
    case S_FOREACH:
        push_scope(I);
        bputs(I, b, "for (obj ");
        emit_name(I, b, s, s->s);
        bputs(I, b, " : ");
        emit_expr(I, b, s->a, T_ANY);
        bputs(I, b, ") ");
        declare(I, s->s, T_OBJ);
        emit_body(I, b, s->d, ind);
        pop_scope(I);
        break;
    case S_WHILE:
        bputs(I, b, "while (");
        emit_expr(I, b, s->a, T_ANY);
        bputs(I, b, ") ");
        emit_body(I, b, s->d, ind);
        break;
    case S_RET:
        bputs(I, b, "return");
        if (s->a) {
            const KsType t = infer(I, s->a);
            Fn *f = I->cur_fn;
            if (!known(t) || t.base == KS_T_ACTION)
                fail(I, s->line, s->col, "Cannot recover the return type of action \"%s\".", f->name);
            if (f->ret_set && !teq(f->ret, t))
                fail(I, s->line, s->col, "Action \"%s\" returns values of different types.", f->name);
            f->ret = t;
            f->ret_set = 1;
            bputs(I, b, " ");
            emit_expr(I, b, s->a, t);
        }
        bputs(I, b, ";");
        break;
    case S_BREAK:
        bputs(I, b, "break;");
        break;
    case S_CONT:
        bputs(I, b, "continue;");
        break;
    case S_ADDTO: {
        bputs(I, b, "addTo ");
        emit_tight(I, b, s->a);
        bputs(I, b, " ");
        // `addTo a (x)` would parse as the call a(x)
        const Nd *v = s->b;
        if (v->k == E_PAREN || v->k == E_CAST || (v->k == E_BIN && v->a->k == E_PAREN))
            fail(I, s->line, s->col, "addTo with a parenthesised value cannot be written in KrunkScript.");
        KsType elem = infer(I, s->a);
        if (elem.dims) elem.dims--;
        emit_expr(I, b, v, elem);
        bputs(I, b, ";");
        break;
    }
    case S_REMOVE:
        bputs(I, b, "remove ");
        emit_expr(I, b, s->a, T_ANY);
        bputs(I, b, ";");
        break;
    default:
        emit_simple(I, b, s);
        bputs(I, b, ";");
        break;
    }
    bputs(I, b, "\n");
}

static void emit_function(Imp *I, Buf *out, Fn *f) {
    const char *sig = f->is_public ? hook_sig(I->side, f->name) : NULL;
    if (f->nparams && !sig)
        fail(I, f->line, f->col, "Cannot recover the parameter types of action \"%s\" (not a %s hook).", f->name,
             I->side == KS_SIDE_SERVER ? "server" : "client");
    if (sig && f->nparams > (int) strlen(sig))
        fail(I, f->line, f->col, "Hook \"%s\" takes at most %d parameters.", f->name, (int) strlen(sig));

    // body first (it decides the return type), then the header line in front of it
    Buf *body = &I->body;
    body->len = 0;
    body->line = out->line + 1;
    I->cur_fn = f;
    push_scope(I);
    for (int i = 0; i < f->nparams; i++) declare(I, f->params[i], sig_type(sig[i]));
    for (int i = 0; i < f->body->n; i++) emit_stmt(I, body, f->body->items[i], 1);
    pop_scope(I);
    f->done = 1;
    I->cur_fn = NULL;

    if (f->is_public) bputs(I, out, "public ");
    if (f->ret_set) {
        emit_type(I, out, f->ret);
        bputs(I, out, " ");
    }
    const Nd at = {.line = f->line, .col = f->col};
    bputs(I, out, "action ");
    emit_name(I, out, &at, f->name);
    bputs(I, out, "(");
    for (int i = 0; i < f->nparams; i++) {
        if (i) bputs(I, out, ", ");
        emit_type(I, out, sig_type(sig[i]));
        bputs(I, out, " ");
        emit_name(I, out, &at, f->params[i]);
    }
    bputs(I, out, ") {\n");
    if (body->len) bput(I, out, body->p, body->len);
    bputs(I, out, "}\n");
}

// ---------------------------------------------------------------------------
// Entry point

// Matches the prelude; returns the byte offset of the body and its first line number.
static size_t match_prelude(Imp *I, int *body_line) {
    size_t i = 0;
    int line = 1;
    // leading blank lines
    for (;;) {
        size_t k = i;
        while (k < I->len && (I->src[k] == ' ' || I->src[k] == '\t' || I->src[k] == '\r')) k++;
        if (k < I->len && I->src[k] == '\n') {
            i = k + 1;
            line++;
            continue;
        }
        break;
    }
    for (size_t p = 0; p < sizeof g_prelude / sizeof g_prelude[0]; p++) {
        size_t e = i;
        while (e < I->len && I->src[e] != '\n') e++;
        size_t l = e - i;
        while (l > 0 && (I->src[i + l - 1] == ' ' || I->src[i + l - 1] == '\t' || I->src[i + l - 1] == '\r')) l--;
        size_t s = 0;
        while (s < l && (I->src[i + s] == ' ' || I->src[i + s] == '\t')) s++;
        const char *want = g_prelude[p];
        if (l - s != strlen(want) || memcmp(I->src + i + s, want, l - s) != 0)
            fail(I, line, 1, "Unrecognised compiled JavaScript prelude (line %d differs from the v7 compiler's).", line);
        i = e < I->len ? e + 1 : e;
        line++;
    }
    *body_line = line;
    return i;
}

char *ks_import_js(const char *js, size_t len, KsSide side, size_t *out_len, KsDiag *err) {
    if (err) memset(err, 0, sizeof *err);
    if (out_len) *out_len = 0;
    if (!js) return NULL;
    Imp *volatile I = calloc(1, sizeof *I);
    if (!I) {
        if (err) snprintf(err->message, sizeof err->message, "Out of memory.");
        return NULL;
    }
    I->src = js;
    I->len = len;
    I->side = side;
    I->err = err;
    I->out.line = 1;
    char *volatile result = NULL;
    if (setjmp(I->jb) == 0) {
        if (len > 4u * KS_MAX_SOURCE_BYTES) fail(I, 0, 0, "Script too large.");
        int body_line = 1;
        const size_t body = match_prelude(I, &body_line);
        lex(I, body, body_line);
        parse_program(I);

        Buf *out = &I->out;
        bputs(I, out, "# KrunkScript recovered from the v7 compiler's JavaScript output (KrunkNative ks_import_js).\n");
        for (int g = 0; g < I->nglobals; g++) {
            const Nd *s = I->globals[g];
            while (s->hint > out->line) bputs(I, out, "\n");
            emit_var(I, out, s);
            bputs(I, out, ";\n");
        }
        for (int f = 0; f < I->nfns; f++) emit_function(I, out, &I->fns[f]);
        result = I->out.p;
        if (out_len) *out_len = I->out.len;
        I->out.p = NULL;
    }
    free(I->out.p);
    free(I->body.p);
    for (size_t i = 0; i < I->nallocs; i++) free(I->allocs[i]);
    free(I->allocs);
    free(I->toks);
    free(I->globals);
    free(I->fns);
    free(I->vars);
    free(I);
    return result;
}
