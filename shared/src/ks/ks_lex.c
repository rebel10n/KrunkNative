// KrunkScript lexer (doc/v7/krunkscript.md 1.1, 1.2), the front-end arena and
// diagnostics, and the map-script text helpers (base64 / raw, fullwidth parens,
// compiled-JS detection).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ks_front_internal.h"

// ---------------------------------------------------------------------------
// Arena

#define KS_ARENA_ALIGN 16u
#define KS_ARENA_BLOCK (64u * 1024u)

static size_t ks_arena_header(void) {
    return (sizeof(KsArenaBlock) + KS_ARENA_ALIGN - 1) & ~(size_t) (KS_ARENA_ALIGN - 1);
}

KsArena *ks_arena_new(void) {
    return calloc(1, sizeof(KsArena));
}

void ks_arena_free(KsArena *a) {
    if (!a) return;
    KsArenaBlock *b = a->head;
    while (b) {
        KsArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    free(a);
}

void *ks_arena_alloc(KsArena *a, size_t size) {
    size = (size + KS_ARENA_ALIGN - 1) & ~(size_t) (KS_ARENA_ALIGN - 1);
    if (size == 0) size = KS_ARENA_ALIGN;
    KsArenaBlock *b = a->head;
    if (!b || b->cap - b->used < size) {
        size_t cap = size > KS_ARENA_BLOCK ? size : KS_ARENA_BLOCK;
        b = malloc(ks_arena_header() + cap);
        if (!b) {
            a->oom = 1;
            return NULL;
        }
        b->next = a->head;
        b->used = 0;
        b->cap = cap;
        a->head = b;
    }
    void *p = (char *) b + ks_arena_header() + b->used;
    b->used += size;
    memset(p, 0, size);
    return p;
}

char *ks_arena_strndup(KsArena *a, const char *s, size_t n) {
    char *p = ks_arena_alloc(a, n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

// ---------------------------------------------------------------------------
// Diagnostics

void ks_front_error(KsFront *f, int line, int col, const char *fmt, ...) {
    if (f->failed) return;
    f->failed = 1;
    KsDiag *d = f->err ? f->err : &f->err_local;
    d->line = line;
    d->col = col;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->message, sizeof d->message, fmt, ap);
    va_end(ap);
}

void ks_front_warning(KsFront *f, int line, int col, const char *fmt, ...) {
    if (f->warnings && f->warning_count < f->max_warnings) {
        KsDiag *d = &f->warnings[f->warning_count];
        d->line = line;
        d->col = col;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(d->message, sizeof d->message, fmt, ap);
        va_end(ap);
    }
    f->warning_count++;
}

int ks_diag_format(const KsDiag *d, char *buf, size_t n) {
    if (d->line > 0) return snprintf(buf, n, "%d:%d %s", d->line, d->col, d->message);
    return snprintf(buf, n, "%s", d->message);
}

// ---------------------------------------------------------------------------
// Names

static const char *const ks_keywords[] = {
    "num", "str", "bool", "obj", "action", "public", "return", "if", "else",
    "for", "while", "break", "continue", "true", "false", "addTo", "remove",
    "lengthOf", "notEmpty", "hasProp", "toNum", "toStr", "static", "dynamic",
};

static const char *const ks_restricted[] = {
    "prototype", "process", "require", "mainModule", "Module", "__compile", "constructor",
    "__defineGetter__", "__defineSetter__", "hasOwnProperty", "__lookupGetter__",
    "__lookupSetter__", "isPrototypeOf", "propertyIsEnumerable", "valueOf", "__proto__",
    "arguments", "caller", "apply", "bind", "call",
};

int ks_is_restricted_name(const char *name) {
    for (size_t i = 0; i < sizeof ks_restricted / sizeof ks_restricted[0]; i++)
        if (strcmp(name, ks_restricted[i]) == 0) return 1;
    return 0;
}

int ks_is_predeclared_name(const char *name) {
    return strcmp(name, "GAME") == 0 || strcmp(name, "UTILS") == 0 || strcmp(name, "Math") == 0;
}

const char *ks_token_kind_text(int kind) {
    static const char *const punct[] = {
        "{", "}", "(", ")", "[", "]", ";", ",", ".", ":", "?",
        "=", "+=", "-=", "*=", "/=", "%=", "**=", "<<=", ">>=", ">>>=", "&=", "|=", "^=",
        "++", "--", "+", "-", "*", "/", "%", "**",
        "==", "!=", "<", "<=", ">", ">=", "<<", ">>", ">>>",
        "&", "|", "^", "~", "!", "&&", "||",
    };
    if (kind >= TK_FIRST_KEYWORD && kind <= TK_LAST_KEYWORD) return ks_keywords[kind - TK_FIRST_KEYWORD];
    if (kind >= TK_LBRACE && kind < TK__COUNT) return punct[kind - TK_LBRACE];
    return NULL;
}

// ---------------------------------------------------------------------------
// Lexer

typedef struct {
    KsFront *f;
    const unsigned char *s;
    size_t n, i;
    int line, col;
    KsToken *toks;
    int ntok, cap;
} Lexer;

static int is_ident_start(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_ident_char(int c) { return is_ident_start(c) || is_digit(c); }
static int hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int peek_at(const Lexer *L, size_t k) { return L->i + k < L->n ? L->s[L->i + k] : -1; }

// Advance one UTF-8 code point (one column; newlines handled by the caller).
static void adv(Lexer *L) {
    if (L->i >= L->n) return;
    unsigned char c = L->s[L->i++];
    if (c >= 0xC0)
        while (L->i < L->n && (L->s[L->i] & 0xC0) == 0x80) L->i++;
    L->col++;
}

static void newline(Lexer *L) {
    // at CR or LF
    if (L->s[L->i] == '\r' && L->i + 1 < L->n && L->s[L->i + 1] == '\n') L->i++;
    L->i++;
    L->line++;
    L->col = 1;
}

static KsToken *push_tok(Lexer *L, int kind, size_t start, int line, int col) {
    if (L->ntok == L->cap) {
        int cap = L->cap ? L->cap * 2 : 256;
        KsToken *t = realloc(L->toks, (size_t) cap * sizeof *t);
        if (!t) {
            ks_front_error(L->f, line, col, "Out of memory.");
            return NULL;
        }
        L->toks = t;
        L->cap = cap;
    }
    KsToken *t = &L->toks[L->ntok++];
    memset(t, 0, sizeof *t);
    t->kind = (uint8_t) kind;
    t->line = line;
    t->col = col;
    t->start = (uint32_t) start;
    t->end = (uint32_t) L->i;
    return t;
}

// Quote a short excerpt of the source for messages.
static void excerpt(const Lexer *L, size_t start, size_t end, char *buf, size_t n) {
    size_t len = end - start;
    if (len > 40) len = 40;
    if (len >= n) len = n - 1;
    memcpy(buf, L->s + start, len);
    buf[len] = 0;
}

// UTF-8 encode a code point (surrogates as 3-byte WTF-8). Returns bytes written.
static int put_utf8(char *o, uint32_t cp) {
    if (cp < 0x80) { o[0] = (char) cp; return 1; }
    if (cp < 0x800) { o[0] = (char) (0xC0 | (cp >> 6)); o[1] = (char) (0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char) (0xE0 | (cp >> 12));
        o[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char) (0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char) (0xF0 | (cp >> 18));
    o[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char) (0x80 | (cp & 0x3F));
    return 4;
}

static int lex_string(Lexer *L, size_t start, int line, int col) {
    int quote = L->s[L->i];
    adv(L);
    // decoded output is never longer than the source text
    char *out = ks_arena_alloc(L->f->arena, L->n - L->i + 1);
    if (!out) {
        ks_front_error(L->f, line, col, "Out of memory.");
        return -1;
    }
    size_t o = 0;
    uint32_t pending_hi = 0;   // a \uD800-\uDBFF waiting for its low half
    for (;;) {
        int c = peek_at(L, 0);
        if (c < 0 || c == '\n' || c == '\r') {
            ks_front_error(L->f, line, col, "Unterminated string.");
            return -1;
        }
        if (c == quote) {
            adv(L);
            break;
        }
        if (c != '\\') {
            if (pending_hi) { o += (size_t) put_utf8(out + o, pending_hi); pending_hi = 0; }
            size_t b = L->i;
            adv(L);
            memcpy(out + o, L->s + b, L->i - b);
            o += L->i - b;
            continue;
        }
        int el = L->line, ec = L->col;
        adv(L);   // backslash
        c = peek_at(L, 0);
        uint32_t cp;
        int is_unit = 0;
        switch (c) {
        case '\'': case '"': case '\\': cp = (uint32_t) c; adv(L); break;
        case 'n': cp = '\n'; adv(L); break;
        case 'r': cp = '\r'; adv(L); break;
        case 't': cp = '\t'; adv(L); break;
        case 'b': cp = '\b'; adv(L); break;
        case 'f': cp = '\f'; adv(L); break;
        case 'v': cp = '\v'; adv(L); break;
        case '0':
            if (is_digit(peek_at(L, 1))) {
                ks_front_error(L->f, el, ec, "Invalid escape sequence.");
                return -1;
            }
            cp = 0;
            adv(L);
            break;
        case 'x': {
            int h1 = hex_val(peek_at(L, 1)), h2 = hex_val(peek_at(L, 2));
            if (h1 < 0 || h2 < 0) {
                ks_front_error(L->f, el, ec, "Invalid escape sequence.");
                return -1;
            }
            cp = (uint32_t) (h1 * 16 + h2);
            adv(L); adv(L); adv(L);
            break;
        }
        case 'u': {
            uint32_t v = 0;
            for (int k = 1; k <= 4; k++) {
                int h = hex_val(peek_at(L, (size_t) k));
                if (h < 0) {
                    ks_front_error(L->f, el, ec, "Invalid escape sequence.");
                    return -1;
                }
                v = v * 16 + (uint32_t) h;
            }
            for (int k = 0; k < 5; k++) adv(L);
            cp = v;
            is_unit = 1;
            break;
        }
        case '\r': case '\n':   // line continuation: nothing emitted
            newline(L);
            continue;
        default:
            ks_front_error(L->f, el, ec, "Invalid escape sequence.");
            return -1;
        }
        if (is_unit && cp >= 0xDC00 && cp <= 0xDFFF && pending_hi) {
            cp = 0x10000 + ((pending_hi - 0xD800) << 10) + (cp - 0xDC00);
            pending_hi = 0;
            o += (size_t) put_utf8(out + o, cp);
            continue;
        }
        if (pending_hi) { o += (size_t) put_utf8(out + o, pending_hi); pending_hi = 0; }
        if (is_unit && cp >= 0xD800 && cp <= 0xDBFF) {
            pending_hi = cp;
            continue;
        }
        o += (size_t) put_utf8(out + o, cp);
    }
    if (pending_hi) o += (size_t) put_utf8(out + o, pending_hi);
    out[o] = 0;
    KsToken *t = push_tok(L, TK_STR, start, line, col);
    if (!t) return -1;
    t->text = out;
    t->len = o;
    return 0;
}

static int lex_number(Lexer *L, size_t start, int line, int col) {
    double v = 0;
    if (peek_at(L, 0) == '0' && (peek_at(L, 1) == 'x' || peek_at(L, 1) == 'X')) {
        adv(L); adv(L);
        int any = 0;
        while (hex_val(peek_at(L, 0)) >= 0) {
            v = v * 16 + hex_val(peek_at(L, 0));
            adv(L);
            any = 1;
        }
        if (!any) goto invalid;
    } else {
        while (is_digit(peek_at(L, 0))) adv(L);
        if (peek_at(L, 0) == '.' && is_digit(peek_at(L, 1))) {
            adv(L);
            while (is_digit(peek_at(L, 0))) adv(L);
        }
        if (peek_at(L, 0) == 'e' || peek_at(L, 0) == 'E') {
            size_t k = 1;
            if (peek_at(L, 1) == '+' || peek_at(L, 1) == '-') k = 2;
            if (!is_digit(peek_at(L, k))) goto invalid;
            while (k--) adv(L);
            while (is_digit(peek_at(L, 0))) adv(L);
        }
        char buf[512];
        size_t len = L->i - start;
        if (len >= sizeof buf) goto invalid;
        memcpy(buf, L->s + start, len);
        buf[len] = 0;
        v = strtod(buf, NULL);
    }
    if (is_ident_char(peek_at(L, 0)) || (peek_at(L, 0) == '.' && is_digit(peek_at(L, 1)))) goto invalid;
    KsToken *t = push_tok(L, TK_NUM, start, line, col);
    if (!t) return -1;
    t->num = v;
    return 0;
invalid:
    while (is_ident_char(peek_at(L, 0)) || peek_at(L, 0) == '.') adv(L);
    {
        char buf[48];
        excerpt(L, start, L->i, buf, sizeof buf);
        ks_front_error(L->f, line, col, "Invalid number \"%s\".", buf);
    }
    return -1;
}

typedef struct { const char *text; uint8_t kind; } Punct;
// longest first within each leading character
static const Punct ks_puncts[] = {
    {">>>=", TK_USHR_ASSIGN}, {">>>", TK_USHR}, {">>=", TK_SHR_ASSIGN}, {"<<=", TK_SHL_ASSIGN},
    {"**=", TK_POW_ASSIGN},
    {">>", TK_SHR}, {">=", TK_GE}, {"<<", TK_SHL}, {"<=", TK_LE}, {"**", TK_POW}, {"*=", TK_MUL_ASSIGN},
    {"==", TK_EQ}, {"!=", TK_NE}, {"&&", TK_ANDAND}, {"&=", TK_AND_ASSIGN}, {"||", TK_OROR},
    {"|=", TK_OR_ASSIGN}, {"^=", TK_XOR_ASSIGN}, {"++", TK_INC}, {"+=", TK_ADD_ASSIGN},
    {"--", TK_DEC}, {"-=", TK_SUB_ASSIGN}, {"/=", TK_DIV_ASSIGN}, {"%=", TK_MOD_ASSIGN},
    {">", TK_GT}, {"<", TK_LT}, {"*", TK_STAR}, {"=", TK_ASSIGN}, {"!", TK_BANG}, {"&", TK_AMP},
    {"|", TK_PIPE}, {"^", TK_CARET}, {"+", TK_PLUS}, {"-", TK_MINUS}, {"/", TK_SLASH},
    {"%", TK_PERCENT}, {"~", TK_TILDE}, {"{", TK_LBRACE}, {"}", TK_RBRACE}, {"(", TK_LPAREN},
    {")", TK_RPAREN}, {"[", TK_LBRACKET}, {"]", TK_RBRACKET}, {";", TK_SEMI}, {",", TK_COMMA},
    {".", TK_DOT}, {":", TK_COLON}, {"?", TK_QUESTION},
};

int ks_lex(KsFront *f) {
    Lexer L = {.f = f, .s = (const unsigned char *) f->src, .n = f->len, .line = 1, .col = 1};
    while (!f->failed) {
        int c = peek_at(&L, 0);
        if (c < 0) break;
        if (c == '\n' || c == '\r') { newline(&L); continue; }
        if (c == ' ' || c == '\t' || c == '\v' || c == '\f') { adv(&L); continue; }
        if (c == 0xC2 && peek_at(&L, 1) == 0xA0) { adv(&L); continue; }                        // U+00A0
        if (c == 0xEF && peek_at(&L, 1) == 0xBB && peek_at(&L, 2) == 0xBF) { adv(&L); continue; } // U+FEFF
        if (c == '#') {
            while (peek_at(&L, 0) >= 0 && peek_at(&L, 0) != '\n' && peek_at(&L, 0) != '\r') adv(&L);
            continue;
        }
        size_t start = L.i;
        int line = L.line, col = L.col;
        if (is_ident_start(c)) {
            while (is_ident_char(peek_at(&L, 0))) adv(&L);
            int kind = TK_IDENT;
            size_t len = L.i - start;
            for (int k = TK_FIRST_KEYWORD; k <= TK_LAST_KEYWORD; k++) {
                const char *kw = ks_keywords[k - TK_FIRST_KEYWORD];
                if (strlen(kw) == len && memcmp(kw, L.s + start, len) == 0) { kind = k; break; }
            }
            KsToken *t = push_tok(&L, kind, start, line, col);
            if (!t) break;
            t->text = kind == TK_IDENT ? ks_arena_strndup(f->arena, (const char *) L.s + start, len)
                                       : ks_keywords[kind - TK_FIRST_KEYWORD];
            if (!t->text) { ks_front_error(f, line, col, "Out of memory."); break; }
            continue;
        }
        if (is_digit(c) || (c == '.' && is_digit(peek_at(&L, 1)))) {
            if (lex_number(&L, start, line, col) < 0) break;
            continue;
        }
        if (c == '"' || c == '\'') {
            if (lex_string(&L, start, line, col) < 0) break;
            continue;
        }
        int matched = 0;
        for (size_t k = 0; k < sizeof ks_puncts / sizeof ks_puncts[0]; k++) {
            size_t pl = strlen(ks_puncts[k].text);
            if (L.n - L.i >= pl && memcmp(L.s + L.i, ks_puncts[k].text, pl) == 0) {
                for (size_t j = 0; j < pl; j++) adv(&L);
                if (!push_tok(&L, ks_puncts[k].kind, start, line, col)) return -1;
                matched = 1;
                break;
            }
        }
        if (matched) continue;
        adv(&L);
        char buf[16];
        excerpt(&L, start, L.i, buf, sizeof buf);
        ks_front_error(f, line, col, "Unexpected \"%s\".", buf);
    }
    if (!f->failed) push_tok(&L, TK_EOF, L.i, L.line, L.col);
    f->toks = L.toks;
    f->ntok = L.ntok;
    return f->failed ? -1 : 0;
}

// ---------------------------------------------------------------------------
// Map script text (1.1)

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// v7 b64Test: /^([0-9a-zA-Z+/]{4})*(([0-9a-zA-Z+/]{2}==)|([0-9a-zA-Z+/]{3}=))?$/
static int b64_test(const char *s, size_t n) {
    if (n % 4) return 0;
    for (size_t i = 0; i < n; i++) {
        int c = (unsigned char) s[i];
        if (c == '=') {
            // only in the last group: "xx==" or "xxx="
            if (i < n - 2) return 0;
            if (i == n - 2 && s[n - 1] != '=') return 0;
            continue;
        }
        if (b64_val(c) < 0) return 0;
    }
    return 1;
}

// U+FF08 / U+FF09 (EF BC 88 / EF BC 89) -> ( ) in place; returns the new length.
size_t ks_map_fullwidth_parens(char *buf, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (i + 2 < n && (unsigned char) buf[i] == 0xEF && (unsigned char) buf[i + 1] == 0xBC &&
            ((unsigned char) buf[i + 2] == 0x88 || (unsigned char) buf[i + 2] == 0x89)) {
            buf[o++] = (unsigned char) buf[i + 2] == 0x88 ? '(' : ')';
            i += 2;
            continue;
        }
        buf[o++] = buf[i];
    }
    return o;
}

char *ks_decode_script(const char *text, size_t len, size_t *out_len) {
    char *raw = NULL;
    size_t n = 0;
    if (len > 0 && b64_test(text, len)) {
        raw = malloc(len / 4 * 3 + 1);
        if (!raw) return NULL;
        for (size_t i = 0; i < len; i += 4) {
            int v[4];
            for (int k = 0; k < 4; k++) v[k] = text[i + k] == '=' ? 0 : b64_val((unsigned char) text[i + k]);
            uint32_t w = (uint32_t) (v[0] << 18 | v[1] << 12 | v[2] << 6 | v[3]);
            raw[n++] = (char) (w >> 16);
            if (text[i + 2] != '=') raw[n++] = (char) (w >> 8);
            if (text[i + 3] != '=') raw[n++] = (char) w;
        }
    } else {
        raw = malloc(len + 1);
        if (!raw) return NULL;
        memcpy(raw, text, len);
        n = len;
    }
    size_t o = ks_map_fullwidth_parens(raw, n);
    raw[o] = 0;
    if (out_len) *out_len = o;
    return raw;
}

int ks_is_compiled_js(const char *source, size_t len) {
    static const char head[] = "\"use strict\";";
    static const char marker[] = "const SM = (m) =>";
    size_t i = 0;
    while (i < len && (source[i] == ' ' || source[i] == '\t' || source[i] == '\r' || source[i] == '\n')) i++;
    if (len - i < sizeof head - 1 || memcmp(source + i, head, sizeof head - 1) != 0) return 0;
    for (size_t k = i; k + sizeof marker - 1 <= len; k++)
        if (memcmp(source + k, marker, sizeof marker - 1) == 0) return 1;
    return 0;
}
