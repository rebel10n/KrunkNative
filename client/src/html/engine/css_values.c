/*
 * css_values.c - tokenizer, declaration splitting, value parsers, shorthand
 * expansion, var() substitution and CssComputed helpers.  See css_values.h.
 */
#include "css_values.h"
#include "util.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Tokens per declaration, whitespace included (the value is tokenized before var() substitution and
 * a longer one is cut short, i.e. dropped as invalid). Krunker Strike's stylesheet outlines its pixel
 * text with 9 to 26 text-shadows of calc(n * var(--base-scale)) offsets: up to ~700 tokens (v10's
 * longest, a 12-layer text-shadow, needs ~250). */
#define CSS_MAX_TOKENS 1024

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ========================================================================= */
/* Property table                                                            */
/* ========================================================================= */

typedef struct PropName {
    const char *name;
    int id;
} PropName;

static const PropName g_prop_names[] = {
#define X(id, name, inh, init) { name, CSS_PROP_##id },
    CSS_PROP_LIST(X)
#undef X
#define X(id, name) { name, CSS_PROP_##id },
    CSS_SHORTHAND_LIST(X)
#undef X
#define X(name, id) { name, CSS_PROP_##id },
    CSS_ALIAS_LIST(X)
#undef X
};

static const char *g_longhand_names[CSS_PROP_LONGHAND_COUNT] = {
#define X(id, name, inh, init) name,
    CSS_PROP_LIST(X)
#undef X
};

static const char *g_shorthand_names[CSS_PROP_COUNT - CSS_PROP_LONGHAND_COUNT] = {
#define X(id, name) name,
    CSS_SHORTHAND_LIST(X)
#undef X
};

static const unsigned char g_prop_inherited[CSS_PROP_LONGHAND_COUNT] = {
#define X(id, name, inh, init) inh,
    CSS_PROP_LIST(X)
#undef X
};

static const char *g_prop_initial[CSS_PROP_LONGHAND_COUNT] = {
#define X(id, name, inh, init) init,
    CSS_PROP_LIST(X)
#undef X
};

#define PROP_HASH_CAP 1024
static int16_t g_prop_hash[PROP_HASH_CAP];   /* index into g_prop_names + 1 */
static int g_prop_hash_ready;

static const char *g_custom_names[CSS_PROP_CUSTOM_MAX - CSS_PROP_CUSTOM_BASE];
static int g_custom_count;

static void prop_hash_build(void)
{
    size_t n = sizeof(g_prop_names) / sizeof(g_prop_names[0]);
    memset(g_prop_hash, 0, sizeof(g_prop_hash));
    for (size_t i = 0; i < n; i++) {
        uint64_t h = kn_hash_str_ci(g_prop_names[i].name, strlen(g_prop_names[i].name));
        size_t j = h & (PROP_HASH_CAP - 1);
        while (g_prop_hash[j]) j = (j + 1) & (PROP_HASH_CAP - 1);
        g_prop_hash[j] = (int16_t)(i + 1);
    }
    g_prop_hash_ready = 1;
}

int css_prop_lookup(const char *name, size_t len)
{
    name = kn_str_trim(name, &len);
    if (len == 0) return CSS_PROP_INVALID;
    if (len >= 2 && name[0] == '-' && name[1] == '-') {
        const char *atom = kn_atom(name, len);
        for (int i = 0; i < g_custom_count; i++)
            if (g_custom_names[i] == atom) return CSS_PROP_CUSTOM_BASE + i;
        if (g_custom_count >= (int)(sizeof(g_custom_names) / sizeof(g_custom_names[0])))
            return CSS_PROP_INVALID;
        g_custom_names[g_custom_count] = atom;
        return CSS_PROP_CUSTOM_BASE + g_custom_count++;
    }
    if (!g_prop_hash_ready) prop_hash_build();
    uint64_t h = kn_hash_str_ci(name, len);
    size_t j = h & (PROP_HASH_CAP - 1);
    while (g_prop_hash[j]) {
        const PropName *p = &g_prop_names[g_prop_hash[j] - 1];
        if (kn_str_ieq(name, len, p->name)) return p->id;
        j = (j + 1) & (PROP_HASH_CAP - 1);
    }
    return CSS_PROP_INVALID;
}

const char *css_prop_name(int id)
{
    if (CSS_PROP_IS_LONGHAND(id)) return g_longhand_names[id];
    if (CSS_PROP_IS_SHORTHAND(id)) return g_shorthand_names[id - CSS_PROP_LONGHAND_COUNT];
    if (id == CSS_PROP_ALL) return "all";
    if (CSS_PROP_IS_CUSTOM(id) && id - CSS_PROP_CUSTOM_BASE < g_custom_count)
        return g_custom_names[id - CSS_PROP_CUSTOM_BASE];
    return NULL;
}

/* @property registrations, indexed by custom id - CSS_PROP_CUSTOM_BASE
 * (name == NULL: unregistered).  See css_property_register(). */
static CssRegisteredProperty g_registered[CSS_PROP_CUSTOM_MAX - CSS_PROP_CUSTOM_BASE];

int css_prop_is_inherited(int id)
{
    if (CSS_PROP_IS_LONGHAND(id)) return g_prop_inherited[id];
    if (!CSS_PROP_IS_CUSTOM(id)) return 0;
    const CssRegisteredProperty *r = &g_registered[id - CSS_PROP_CUSTOM_BASE];
    return r->name ? r->inherits : 1;
}

const char *css_prop_initial_text(int id)
{
    return CSS_PROP_IS_LONGHAND(id) ? g_prop_initial[id] : NULL;
}

/* ========================================================================= */
/* Tokenizer                                                                 */
/* ========================================================================= */

static int is_name_start(int c) { return kn_is_alpha(c) || c == '_' || c >= 0x80; }
static int is_name_char(int c) { return is_name_start(c) || kn_is_digit(c) || c == '-'; }

static int starts_ident(const char *p, const char *end)
{
    if (p >= end) return 0;
    if (p[0] == '-') {
        if (p + 1 < end && (is_name_start((unsigned char)p[1]) || p[1] == '-')) return 1;
        if (p + 2 < end && p[1] == '\\' && !kn_is_space((unsigned char)p[2])) return 1;
        return 0;
    }
    if (is_name_start((unsigned char)p[0])) return 1;
    if (p[0] == '\\' && p + 1 < end && p[1] != '\n') return 1;
    return 0;
}

static int starts_number(const char *p, const char *end)
{
    if (p >= end) return 0;
    if (p[0] == '+' || p[0] == '-') {
        if (p + 1 < end && kn_is_digit((unsigned char)p[1])) return 1;
        if (p + 2 < end && p[1] == '.' && kn_is_digit((unsigned char)p[2])) return 1;
        return 0;
    }
    if (p[0] == '.') return p + 1 < end && kn_is_digit((unsigned char)p[1]);
    return kn_is_digit((unsigned char)p[0]);
}

static const char *consume_name(const char *p, const char *end)
{
    while (p < end) {
        if (is_name_char((unsigned char)*p)) p++;
        else if (*p == '\\' && p + 1 < end && p[1] != '\n') p += 2;
        else break;
    }
    return p;
}

static CssUnit unit_lookup(const char *s, size_t len)
{
    static const struct { const char *n; CssUnit u; } units[] = {
        { "px", CSS_UNIT_PX }, { "em", CSS_UNIT_EM }, { "rem", CSS_UNIT_REM },
        { "s", CSS_UNIT_S }, { "ms", CSS_UNIT_MS }, { "deg", CSS_UNIT_DEG },
        { "rad", CSS_UNIT_RAD }, { "grad", CSS_UNIT_GRAD }, { "turn", CSS_UNIT_TURN },
        { "vw", CSS_UNIT_VW }, { "vh", CSS_UNIT_VH }, { "vmin", CSS_UNIT_VMIN }, { "vmax", CSS_UNIT_VMAX }, { "pt", CSS_UNIT_PT },
        { "ch", CSS_UNIT_CH }, { "ex", CSS_UNIT_EX }, { "fr", CSS_UNIT_FR },
    };
    for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++)
        if (kn_str_ieq(s, len, units[i].n)) return units[i].u;
    return CSS_UNIT_UNKNOWN;
}

static const char *consume_number(const char *p, const char *end, CssToken *t)
{
    const char *start = p;
    int is_int = 1;
    if (*p == '+' || *p == '-') { t->flags |= CSS_TOKF_SIGNED; p++; }
    while (p < end && kn_is_digit((unsigned char)*p)) p++;
    if (p + 1 < end && *p == '.' && kn_is_digit((unsigned char)p[1])) {
        is_int = 0;
        p++;
        while (p < end && kn_is_digit((unsigned char)*p)) p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        const char *q = p + 1;
        if (q < end && (*q == '+' || *q == '-')) q++;
        if (q < end && kn_is_digit((unsigned char)*q)) {
            is_int = 0;
            p = q;
            while (p < end && kn_is_digit((unsigned char)*p)) p++;
        }
    }
    char buf[64];
    size_t n = (size_t)(p - start);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, start, n);
    buf[n] = '\0';
    t->num = strtod(buf, NULL);
    if (is_int) t->flags |= CSS_TOKF_INTEGER;
    return p;
}

int css_tokenize(const char *text, size_t len, CssToken *out, int max)
{
    const char *p = text, *end = text + len;
    int n = 0;
    while (p < end && n < max) {
        CssToken *t = &out[n];
        memset(t, 0, sizeof(*t));
        t->text = p;
        t->len = 1;
        unsigned char c = (unsigned char)*p;
        if (kn_is_space(c)) {
            while (p < end && kn_is_space((unsigned char)*p)) p++;
            t->type = CSS_TOK_WHITESPACE;
            t->len = (size_t)(p - t->text);
            n++;
            continue;
        }
        if (c == '/' && p + 1 < end && p[1] == '*') {
            const char *q = p + 2;
            while (q + 1 < end && !(q[0] == '*' && q[1] == '/')) q++;
            p = q + 1 < end ? q + 2 : end;
            continue;
        }
        if (c == '"' || c == '\'') {
            const char *q = p + 1;
            t->text = q;
            while (q < end && *q != (char)c && *q != '\n') {
                if (*q == '\\' && q + 1 < end) q++;
                q++;
            }
            t->len = (size_t)(q - t->text);
            if (q < end && *q == (char)c) { t->type = CSS_TOK_STRING; p = q + 1; }
            else { t->type = CSS_TOK_BAD; p = q; }
            n++;
            continue;
        }
        if (c == '#' && p + 1 < end && (is_name_char((unsigned char)p[1]) || p[1] == '\\')) {
            const char *q = consume_name(p + 1, end);
            t->type = CSS_TOK_HASH;
            t->text = p + 1;
            t->len = (size_t)(q - t->text);
            if (starts_ident(p + 1, end)) t->flags |= CSS_TOKF_ID_HASH;
            p = q;
            n++;
            continue;
        }
        if (starts_number(p, end)) {
            p = consume_number(p, end, t);
            if (starts_ident(p, end)) {
                const char *q = consume_name(p, end);
                t->type = CSS_TOK_DIMENSION;
                t->text = p;
                t->len = (size_t)(q - p);
                t->unit = (uint8_t)unit_lookup(p, t->len);
                p = q;
            } else if (p < end && *p == '%') {
                t->type = CSS_TOK_PERCENTAGE;
                t->unit = CSS_UNIT_PERCENT;
                t->len = (size_t)(p + 1 - t->text);
                p++;
            } else {
                t->type = CSS_TOK_NUMBER;
                t->len = (size_t)(p - t->text);
            }
            n++;
            continue;
        }
        if (starts_ident(p, end)) {
            const char *q = consume_name(p, end);
            t->len = (size_t)(q - p);
            if (q < end && *q == '(') {
                if (kn_str_ieq(p, t->len, "url")) {
                    const char *r = q + 1;
                    while (r < end && kn_is_space((unsigned char)*r)) r++;
                    if (r < end && *r != '"' && *r != '\'') {
                        const char *s = r;
                        while (s < end && *s != ')' && !kn_is_space((unsigned char)*s)) s++;
                        t->type = CSS_TOK_URL;
                        t->text = r;
                        t->len = (size_t)(s - r);
                        while (s < end && kn_is_space((unsigned char)*s)) s++;
                        if (s < end && *s == ')') s++;
                        p = s;
                        n++;
                        continue;
                    }
                }
                t->type = CSS_TOK_FUNCTION;
                p = q + 1;
            } else {
                t->type = CSS_TOK_IDENT;
                p = q;
            }
            n++;
            continue;
        }
        switch (c) {
        case ',': t->type = CSS_TOK_COMMA; break;
        case ':': t->type = CSS_TOK_COLON; break;
        case ';': t->type = CSS_TOK_SEMICOLON; break;
        case '(': t->type = CSS_TOK_LPAREN; break;
        case ')': t->type = CSS_TOK_RPAREN; break;
        case '[': t->type = CSS_TOK_LBRACKET; break;
        case ']': t->type = CSS_TOK_RBRACKET; break;
        case '{': t->type = CSS_TOK_LBRACE; break;
        case '}': t->type = CSS_TOK_RBRACE; break;
        default:  t->type = CSS_TOK_DELIM; break;
        }
        p++;
        n++;
    }
    return n;
}

int css_tokens_strip_ws(CssToken *tokens, int n)
{
    int w = 0;
    for (int i = 0; i < n; i++)
        if (tokens[i].type != CSS_TOK_WHITESPACE) tokens[w++] = tokens[i];
    return w;
}

int css_component_len(const CssToken *tokens, int n, int i)
{
    if (i >= n) return 0;
    uint8_t t = tokens[i].type;
    if (t != CSS_TOK_FUNCTION && t != CSS_TOK_LPAREN && t != CSS_TOK_LBRACKET && t != CSS_TOK_LBRACE)
        return 1;
    int depth = 0;
    for (int j = i; j < n; j++) {
        uint8_t k = tokens[j].type;
        if (k == CSS_TOK_FUNCTION || k == CSS_TOK_LPAREN || k == CSS_TOK_LBRACKET || k == CSS_TOK_LBRACE) depth++;
        else if (k == CSS_TOK_RPAREN || k == CSS_TOK_RBRACKET || k == CSS_TOK_RBRACE) {
            depth--;
            if (depth == 0) return j - i + 1;
        }
    }
    return n - i; /* unterminated: take the rest */
}

size_t css_tokens_to_text(const CssToken *tokens, int n, char *out, size_t cap)
{
    size_t w = 0;
#define PUT(s, l) do { size_t l_ = (l); if (w + l_ < cap) memcpy(out + w, (s), l_); w += l_; } while (0)
    for (int i = 0; i < n; i++) {
        const CssToken *t = &tokens[i];
        char tmp[48];
        switch (t->type) {
        case CSS_TOK_FUNCTION: PUT(t->text, t->len); PUT("(", 1); break;
        case CSS_TOK_HASH: PUT("#", 1); PUT(t->text, t->len); break;
        case CSS_TOK_STRING: PUT("\"", 1); PUT(t->text, t->len); PUT("\"", 1); break;
        case CSS_TOK_URL: PUT("url(", 4); PUT(t->text, t->len); PUT(")", 1); break;
        case CSS_TOK_NUMBER: case CSS_TOK_PERCENTAGE: case CSS_TOK_DIMENSION:
            if (t->type == CSS_TOK_DIMENSION) {
                snprintf(tmp, sizeof(tmp), "%g", t->num);
                PUT(tmp, strlen(tmp));
                PUT(t->text, t->len);
            } else {
                snprintf(tmp, sizeof(tmp), t->type == CSS_TOK_PERCENTAGE ? "%g%%" : "%g", t->num);
                PUT(tmp, strlen(tmp));
            }
            break;
        case CSS_TOK_WHITESPACE: PUT(" ", 1); break;
        case CSS_TOK_COMMA: PUT(",", 1); break;
        case CSS_TOK_COLON: PUT(":", 1); break;
        case CSS_TOK_SEMICOLON: PUT(";", 1); break;
        case CSS_TOK_LPAREN: PUT("(", 1); break;
        case CSS_TOK_RPAREN: PUT(")", 1); break;
        case CSS_TOK_LBRACKET: PUT("[", 1); break;
        case CSS_TOK_RBRACKET: PUT("]", 1); break;
        case CSS_TOK_LBRACE: PUT("{", 1); break;
        case CSS_TOK_RBRACE: PUT("}", 1); break;
        case CSS_TOK_SHORTHAND_REF: break;
        default: PUT(t->text, t->len); break;
        }
    }
#undef PUT
    if (cap) out[w < cap ? w : cap - 1] = '\0';
    return w;
}

/* ========================================================================= */
/* Small token helpers                                                       */
/* ========================================================================= */

static int tok_is_ident(const CssToken *t, const char *name)
{
    return t->type == CSS_TOK_IDENT && kn_str_ieq(t->text, t->len, name);
}

static int tok_is_func(const CssToken *t, const char *name)
{
    return t->type == CSS_TOK_FUNCTION && kn_str_ieq(t->text, t->len, name);
}

static int tok_is_delim(const CssToken *t, char c)
{
    return t->type == CSS_TOK_DELIM && t->text[0] == c;
}

static int tok_is_global_kw(const CssToken *t)
{
    return tok_is_ident(t, "inherit") || tok_is_ident(t, "initial") || tok_is_ident(t, "unset") ||
           tok_is_ident(t, "revert");
}

/* Copies the slice into a whitespace-free local array.  Returns count. */
static int tokens_copy_nows(const CssToken *in, int n, CssToken *out, int max)
{
    int w = 0;
    for (int i = 0; i < n && w < max; i++)
        if (in[i].type != CSS_TOK_WHITESPACE) out[w++] = in[i];
    return w;
}

/* Splits a whitespace-free slice at top-level commas.  Fills starts/lens. */
static int split_commas(const CssToken *t, int n, int *starts, int *lens, int max)
{
    int count = 0, i = 0;
    if (n == 0) return 0;
    while (i <= n && count < max) {
        int s = i;
        while (i < n && t[i].type != CSS_TOK_COMMA) i += css_component_len(t, n, i);
        starts[count] = s;
        lens[count] = i - s;
        count++;
        if (i >= n) break;
        i++; /* skip comma */
        if (i == n) { /* trailing comma: empty item */
            if (count < max) { starts[count] = i; lens[count] = 0; count++; }
            break;
        }
    }
    return count;
}

/* Literal tokens for synthesized defaults (text points at static storage). */
static int tok_lit(const char *lit, CssToken *out, int max)
{
    int n = css_tokenize(lit, strlen(lit), out, max);
    return css_tokens_strip_ws(out, n);
}

/* ========================================================================= */
/* Declaration blocks                                                        */
/* ========================================================================= */

/* `all` (CSS Cascade 4 3.2): only a CSS-wide keyword (inherit / initial / unset / revert), which
 * stands for every longhand except direction and unicode-bidi; custom properties are untouched.
 * v10's .io-button { all: unset; ... } relies on it to drop the UA button style (box-sizing:
 * border-box, Arial 13.33px, outset border, display inline-block). */
static int parse_all(const char *value, size_t value_len, int important, CssDeclCb cb, void *user)
{
    value = kn_str_trim(value, &value_len);
    CssToken tokens[8];
    int n = css_tokens_strip_ws(tokens, css_tokenize(value, value_len, tokens, 8));
    if (n != 1 || !tok_is_global_kw(&tokens[0])) return 0;
    for (int id = 0; id < CSS_PROP_LONGHAND_COUNT; id++)
        if (id != CSS_PROP_DIRECTION) cb(user, id, tokens, 1, important);
    return 1;
}

int css_parse_declaration(const char *name, size_t name_len,
                          const char *value, size_t value_len,
                          int important, CssDeclCb cb, void *user)
{
    int id = css_prop_lookup(name, name_len);
    if (id == CSS_PROP_INVALID) {
        size_t nl = name_len;
        const char *nm = kn_str_trim(name, &nl);
        return kn_str_ieq(nm, nl, "all") ? parse_all(value, value_len, important, cb, user) : 0;
    }
    value = kn_str_trim(value, &value_len);
    CssToken tokens[CSS_MAX_TOKENS];
    int n = css_tokenize(value, value_len, tokens, CSS_MAX_TOKENS);
    /* strip leading/trailing whitespace tokens */
    int s = 0;
    while (s < n && tokens[s].type == CSS_TOK_WHITESPACE) s++;
    while (n > s && tokens[n - 1].type == CSS_TOK_WHITESPACE) n--;
    if (n - s <= 0 && !CSS_PROP_IS_CUSTOM(id)) return 0;
    if (CSS_PROP_IS_SHORTHAND(id))
        return css_expand_shorthand(id, tokens + s, n - s, important, cb, user) > 0;
    cb(user, id, tokens + s, n - s, important);
    return 1;
}

int css_parse_declaration_block(const char *text, size_t len, CssDeclCb cb, void *user)
{
    const char *p = text, *end = text + len;
    int count = 0;
    while (p < end) {
        /* skip whitespace / stray semicolons / comments */
        while (p < end) {
            if (kn_is_space((unsigned char)*p) || *p == ';') p++;
            else if (*p == '/' && p + 1 < end && p[1] == '*') {
                const char *q = p + 2;
                while (q + 1 < end && !(q[0] == '*' && q[1] == '/')) q++;
                p = q + 1 < end ? q + 2 : end;
            } else break;
        }
        if (p >= end) break;
        const char *name = p;
        while (p < end && *p != ':' && *p != ';' && *p != '{') p++;
        if (p >= end || *p != ':') {
            /* garbage: skip to next ';' */
            while (p < end && *p != ';') p++;
            continue;
        }
        size_t name_len = (size_t)(p - name);
        p++; /* ':' */
        const char *value = p;
        int depth = 0;
        const char *bang = NULL;
        while (p < end) {
            char c = *p;
            if (c == '"' || c == '\'') {
                const char *q = p + 1;
                while (q < end && *q != c && *q != '\n') { if (*q == '\\' && q + 1 < end) q++; q++; }
                p = q < end ? q + 1 : end;
                continue;
            }
            if (c == '/' && p + 1 < end && p[1] == '*') {
                const char *q = p + 2;
                while (q + 1 < end && !(q[0] == '*' && q[1] == '/')) q++;
                p = q + 1 < end ? q + 2 : end;
                continue;
            }
            if (c == '(' || c == '[' || c == '{') depth++;
            else if (c == ')' || c == ']' || c == '}') { if (depth > 0) depth--; }
            else if (c == ';' && depth == 0) break;
            else if (c == '!' && depth == 0) bang = p;
            p++;
        }
        size_t value_len = (size_t)(p - value);
        int important = 0;
        if (bang) {
            const char *q = bang + 1;
            while (q < p && kn_is_space((unsigned char)*q)) q++;
            if (p - q >= 9 && kn_str_ieq(q, 9, "important")) {
                const char *r = q + 9;
                while (r < p && kn_is_space((unsigned char)*r)) r++;
                if (r == p) { important = 1; value_len = (size_t)(bang - value); }
            }
        }
        if (css_parse_declaration(name, name_len, value, value_len, important, cb, user)) count++;
        if (p < end) p++; /* ';' */
    }
    return count;
}

/* ========================================================================= */
/* Lengths and calc()                                                        */
/* ========================================================================= */

typedef struct CalcVal {
    double px, pct, em;
    double num;        /* pure number component                               */
    int is_number;     /* 1 when the value is a plain number (no units)       */
} CalcVal;

static int calc_sum(const CssToken *t, int n, int *i, CalcVal *out, int depth);

/* Viewport for vw/vh/vmin/vmax: absolute lengths, resolved to px when the
 * value is computed (css_values_set_viewport). */
static float g_viewport_w = 1280.0f, g_viewport_h = 720.0f;
static int g_viewport_units_used;

void css_values_set_viewport(float w, float h)
{
    if (w > 0.0f) g_viewport_w = w;
    if (h > 0.0f) g_viewport_h = h;
}

int css_values_viewport_units_used(void) { return g_viewport_units_used; }

/* min()/max()/clamp(): the arguments are resolved here, so they must be
 * comparable without layout: all absolute (px, incl. vw/vh), all percentages
 * or all em.  Mixed arguments (min(50%, 300px)) need a used-value calc tree
 * that CssLength cannot hold; such declarations are rejected. */
/* 0 zero length, 1 px, 2 %, 3 em, 4 number; -1 mixed (not comparable). */
static int calc_kind(const CalcVal *v)
{
    if (v->is_number) return 4;
    const int px = v->px != 0.0, pct = v->pct != 0.0, em = v->em != 0.0;
    if (px + pct + em > 1) return -1;
    return px ? 1 : pct ? 2 : em ? 3 : 0;
}

static double calc_scalar(const CalcVal *v, int kind)
{
    return kind == 2 ? v->pct : kind == 3 ? v->em : kind == 4 ? v->num : v->px;
}

/* fn: 0 min, 1 max, 2 clamp(MIN, VAL, MAX) = max(MIN, min(VAL, MAX)). */
static int calc_minmax(const CssToken *t, int n, int *i, CalcVal *out, int depth, int fn)
{
    double v[8];
    CalcVal a;
    int count = 0, kind = 0;
    for (;;) {
        if (count == 8 || !calc_sum(t, n, i, &a, depth + 1)) return 0;
        const int k = calc_kind(&a);
        if (k < 0 || (k && kind && k != kind) || (k == 4 && count && !kind)) return 0;
        if (k == 0 && kind == 4) return 0;
        if (k) kind = k;
        v[count++] = calc_scalar(&a, k ? k : 1);
        if (*i < n && t[*i].type == CSS_TOK_COMMA) { (*i)++; continue; }
        break;
    }
    if (fn == 2 && count != 3) return 0;
    double r = v[0];
    if (fn == 2) {
        r = v[1] < v[2] ? v[1] : v[2];
        if (v[0] > r) r = v[0];
    } else {
        for (int k = 1; k < count; k++) r = fn == 0 ? (v[k] < r ? v[k] : r) : (v[k] > r ? v[k] : r);
    }
    memset(out, 0, sizeof(*out));
    if (kind == 2) out->pct = r;
    else if (kind == 3) out->em = r;
    else if (kind == 4) { out->num = r; out->is_number = 1; }
    else out->px = r;
    return 1;
}

static int calc_primary(const CssToken *t, int n, int *i, CalcVal *out, int depth)
{
    if (*i >= n || depth > 16) return 0;
    const CssToken *k = &t[*i];
    memset(out, 0, sizeof(*out));
    if (k->type == CSS_TOK_NUMBER) { out->num = k->num; out->is_number = 1; (*i)++; return 1; }
    if (k->type == CSS_TOK_PERCENTAGE) { out->pct = k->num; (*i)++; return 1; }
    if (k->type == CSS_TOK_DIMENSION) {
        switch (k->unit) {
        case CSS_UNIT_PX: out->px = k->num; break;
        case CSS_UNIT_PT: out->px = k->num * 4.0 / 3.0; break;
        case CSS_UNIT_EM: out->em = k->num; break;
        case CSS_UNIT_REM: out->px = k->num * 16.0; break;
        case CSS_UNIT_VW: out->px = k->num * 0.01 * g_viewport_w; g_viewport_units_used = 1; break;
        case CSS_UNIT_VH: out->px = k->num * 0.01 * g_viewport_h; g_viewport_units_used = 1; break;
        case CSS_UNIT_VMIN: out->px = k->num * 0.01 * fminf(g_viewport_w, g_viewport_h); g_viewport_units_used = 1; break;
        case CSS_UNIT_VMAX: out->px = k->num * 0.01 * fmaxf(g_viewport_w, g_viewport_h); g_viewport_units_used = 1; break;
        default: return 0;
        }
        (*i)++;
        return 1;
    }
    if (tok_is_func(k, "min") || tok_is_func(k, "max") || tok_is_func(k, "clamp")) {
        const int fn = tok_is_func(k, "min") ? 0 : tok_is_func(k, "max") ? 1 : 2;
        (*i)++;
        if (!calc_minmax(t, n, i, out, depth, fn)) return 0;
        if (*i >= n || t[*i].type != CSS_TOK_RPAREN) return 0;
        (*i)++;
        return 1;
    }
    if (k->type == CSS_TOK_LPAREN || tok_is_func(k, "calc") ||
        tok_is_func(k, "-webkit-calc")) {   /* Blink: no -moz-/-o-calc */
        (*i)++;
        if (!calc_sum(t, n, i, out, depth + 1)) return 0;
        if (*i >= n || t[*i].type != CSS_TOK_RPAREN) return 0;
        (*i)++;
        return 1;
    }
    return 0;
}

static int calc_product(const CssToken *t, int n, int *i, CalcVal *out, int depth)
{
    if (!calc_primary(t, n, i, out, depth)) return 0;
    while (*i < n && (tok_is_delim(&t[*i], '*') || tok_is_delim(&t[*i], '/'))) {
        int mul = tok_is_delim(&t[*i], '*');
        (*i)++;
        CalcVal r;
        if (!calc_primary(t, n, i, &r, depth)) return 0;
        if (mul) {
            if (r.is_number) {
                out->px *= r.num; out->pct *= r.num; out->em *= r.num; out->num *= r.num;
            } else if (out->is_number) {
                double f = out->num;
                *out = r;
                out->px *= f; out->pct *= f; out->em *= f; out->num *= f;
            } else return 0;
        } else {
            if (!r.is_number || r.num == 0.0) return 0;
            out->px /= r.num; out->pct /= r.num; out->em /= r.num; out->num /= r.num;
        }
    }
    return 1;
}

static int calc_sum(const CssToken *t, int n, int *i, CalcVal *out, int depth)
{
    if (!calc_product(t, n, i, out, depth)) return 0;
    for (;;) {
        if (*i >= n) break;
        int sign;
        if (tok_is_delim(&t[*i], '+')) { sign = 1; (*i)++; }
        else if (tok_is_delim(&t[*i], '-')) { sign = -1; (*i)++; }
        else if ((t[*i].type == CSS_TOK_NUMBER || t[*i].type == CSS_TOK_PERCENTAGE ||
                  t[*i].type == CSS_TOK_DIMENSION) && (t[*i].flags & CSS_TOKF_SIGNED)) {
            sign = 1; /* lenient: `50% -5px` */
        } else break;
        CalcVal r;
        if (!calc_product(t, n, i, &r, depth)) return 0;
        if (out->is_number != r.is_number) {
            /* number + length is invalid unless the number is zero-ish usage; reject */
            return 0;
        }
        out->px += sign * r.px; out->pct += sign * r.pct; out->em += sign * r.em; out->num += sign * r.num;
    }
    return 1;
}

static int length_from_calcval(const CalcVal *v, unsigned flags, int is_calc, CssLength *out)
{
    CssLength r;
    memset(&r, 0, sizeof(r));
    if (v->is_number) {
        if (flags & CSS_LENF_NUMBER) { r.kind = CSS_LEN_NUMBER; r.px = (float)v->num; }
        else if (v->num == 0.0 || (flags & CSS_LENF_QUIRKY)) { r.kind = CSS_LEN_VALUE; r.px = (float)v->num; }
        else return 0;
    } else {
        if (v->pct != 0.0 && !(flags & CSS_LENF_PCT)) return 0;
        r.kind = CSS_LEN_VALUE;
        r.px = (float)v->px;
        r.pct = (float)v->pct;
        r.em = (float)v->em;
    }
    /* calc() results are clamped at used-value time, never rejected */
    if (!is_calc && (flags & CSS_LENF_NONNEG) && (r.px < 0 || r.pct < 0 || r.em < 0) && r.kind == CSS_LEN_VALUE)
        return 0;
    *out = r;
    return 1;
}

int css_parse_length_item(const CssToken *tokens, int n, unsigned flags, CssLength *out, int *consumed)
{
    if (n <= 0) return 0;
    const CssToken *t = &tokens[0];
    CssLength r;
    memset(&r, 0, sizeof(r));
    if (t->type == CSS_TOK_IDENT) {
        if ((flags & CSS_LENF_AUTO) && tok_is_ident(t, "auto")) r.kind = CSS_LEN_AUTO;
        else if ((flags & CSS_LENF_NONE) && tok_is_ident(t, "none")) r.kind = CSS_LEN_NONE;
        else if ((flags & CSS_LENF_NORMAL) && tok_is_ident(t, "normal")) r.kind = CSS_LEN_NORMAL;
        else if ((flags & CSS_LENF_CONTENT) && tok_is_ident(t, "min-content")) r.kind = CSS_LEN_MIN_CONTENT;
        else if ((flags & CSS_LENF_CONTENT) && tok_is_ident(t, "max-content")) r.kind = CSS_LEN_MAX_CONTENT;
        else if ((flags & CSS_LENF_CONTENT) && tok_is_ident(t, "fit-content")) r.kind = CSS_LEN_FIT_CONTENT;
        else return 0;
        *out = r;
        if (consumed) *consumed = 1;
        return 1;
    }
    if (t->type == CSS_TOK_NUMBER || t->type == CSS_TOK_PERCENTAGE || t->type == CSS_TOK_DIMENSION) {
        CalcVal v;
        int i = 0;
        if (!calc_primary(tokens, 1, &i, &v, 0)) return 0;
        if (!length_from_calcval(&v, flags, 0, &r)) return 0;
        r.pct_term = t->type == CSS_TOK_PERCENTAGE;
        *out = r;
        if (consumed) *consumed = 1;
        return 1;
    }
    if (t->type == CSS_TOK_FUNCTION) {
        int len = css_component_len(tokens, n, 0);
        int i = 0;
        CalcVal v;
        if (!calc_primary(tokens, len, &i, &v, 0) || i != len) return 0;
        if (!length_from_calcval(&v, flags, 1, &r)) return 0;
        *out = r;
        if (consumed) *consumed = len;
        return 1;
    }
    return 0;
}

int css_parse_length(const CssToken *tokens, int n, unsigned flags, CssLength *out)
{
    CssToken t[CSS_MAX_TOKENS];
    int m = tokens_copy_nows(tokens, n, t, CSS_MAX_TOKENS);
    int used = 0;
    CssLength r;
    if (!css_parse_length_item(t, m, flags, &r, &used) || used != m) return 0;
    *out = r;
    return 1;
}

void css_length_resolve_em(CssLength *len, float font_px)
{
    if (len->kind == CSS_LEN_VALUE && len->em != 0.0f) {
        len->px += len->em * font_px;
        len->em = 0.0f;
    }
}

float css_length_resolve(const CssLength *len, float reference_px)
{
    if (len->kind != CSS_LEN_VALUE) return 0.0f;
    return len->px + len->pct * 0.01f * reference_px;
}

/* ========================================================================= */
/* Numbers, angles, times                                                    */
/* ========================================================================= */

static int single_nows(const CssToken *tokens, int n, CssToken *out)
{
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (tokens[i].type == CSS_TOK_WHITESPACE) continue;
        if (found) return 0;
        *out = tokens[i];
        found = 1;
    }
    return found;
}

int css_parse_number(const CssToken *tokens, int n, float *out)
{
    CssToken t;
    if (!single_nows(tokens, n, &t)) {
        /* calc() of numbers */
        CssToken buf[CSS_MAX_TOKENS];
        int m = tokens_copy_nows(tokens, n, buf, CSS_MAX_TOKENS);
        int i = 0;
        CalcVal v;
        if (m == 0 || !calc_primary(buf, m, &i, &v, 0) || i != m || !v.is_number) return 0;
        *out = (float)v.num;
        return 1;
    }
    if (t.type != CSS_TOK_NUMBER) return 0;
    *out = (float)t.num;
    return 1;
}

int css_parse_integer(const CssToken *tokens, int n, int *out)
{
    CssToken t;
    if (!single_nows(tokens, n, &t) || t.type != CSS_TOK_NUMBER || !(t.flags & CSS_TOKF_INTEGER)) return 0;
    double v = t.num;
    if (v > INT_MAX) v = INT_MAX;
    if (v < INT_MIN) v = INT_MIN;
    *out = (int)v;
    return 1;
}

int css_parse_z_index(const CssToken *tokens, int n, int32_t *out, int *is_auto)
{
    CssToken t;
    if (!single_nows(tokens, n, &t)) return 0;
    if (tok_is_ident(&t, "auto")) { *is_auto = 1; *out = 0; return 1; }
    if (t.type != CSS_TOK_NUMBER || !(t.flags & CSS_TOKF_INTEGER)) return 0;
    double v = t.num;
    if (v > 2147483647.0) v = 2147483647.0;
    if (v < -2147483648.0) v = -2147483648.0;
    *out = (int32_t)v;
    *is_auto = 0;
    return 1;
}

int css_parse_opacity(const CssToken *tokens, int n, float *out)
{
    CssToken t;
    float v;
    if (single_nows(tokens, n, &t) && t.type == CSS_TOK_PERCENTAGE) v = (float)(t.num / 100.0);
    else if (!css_parse_number(tokens, n, &v)) return 0;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    *out = v;
    return 1;
}

static int angle_from_token(const CssToken *t, float *out)
{
    if (t->type == CSS_TOK_NUMBER && t->num == 0.0) { *out = 0; return 1; }
    if (t->type != CSS_TOK_DIMENSION) return 0;
    switch (t->unit) {
    case CSS_UNIT_DEG: *out = (float)t->num; return 1;
    case CSS_UNIT_RAD: *out = (float)(t->num * 180.0 / M_PI); return 1;
    case CSS_UNIT_GRAD: *out = (float)(t->num * 0.9); return 1;
    case CSS_UNIT_TURN: *out = (float)(t->num * 360.0); return 1;
    default: return 0;
    }
}

int css_parse_angle_deg(const CssToken *tokens, int n, float *out)
{
    CssToken t;
    if (!single_nows(tokens, n, &t)) return 0;
    return angle_from_token(&t, out);
}

static int time_from_token(const CssToken *t, float *out)
{
    if (t->type == CSS_TOK_NUMBER && t->num == 0.0) { *out = 0; return 1; }
    if (t->type != CSS_TOK_DIMENSION) return 0;
    if (t->unit == CSS_UNIT_S) { *out = (float)t->num; return 1; }
    if (t->unit == CSS_UNIT_MS) { *out = (float)(t->num / 1000.0); return 1; }
    return 0;
}

/* A <time> at t[0]: a dimension, or a math function (calc(), min(), max(), clamp()) of times
 * and numbers (`calc(var(--total-maps) * 5s)`, v10's ranked map carousel).  The calc evaluator
 * works on lengths, so a copy with s / ms as px (seconds) is evaluated; any length or percentage
 * makes it invalid, and so does a plain number (calc(5) is no time).  *used: tokens taken. */
static int time_from_tokens(const CssToken *t, int n, int *used, float *out)
{
    if (n <= 0) return 0;
    if (time_from_token(&t[0], out)) { *used = 1; return 1; }
    if (!(tok_is_func(&t[0], "calc") || tok_is_func(&t[0], "-webkit-calc") || tok_is_func(&t[0], "min") ||
          tok_is_func(&t[0], "max") || tok_is_func(&t[0], "clamp")))
        return 0;
    const int len = css_component_len(t, n, 0);
    CssToken buf[128];
    if (len <= 0 || len > (int)(sizeof buf / sizeof buf[0])) return 0;
    for (int i = 0; i < len; i++) {
        buf[i] = t[i];
        if (t[i].type == CSS_TOK_PERCENTAGE) return 0;
        if (t[i].type == CSS_TOK_DIMENSION) {
            if (t[i].unit == CSS_UNIT_S) buf[i].num = t[i].num;
            else if (t[i].unit == CSS_UNIT_MS) buf[i].num = t[i].num / 1000.0;
            else return 0;
            buf[i].unit = CSS_UNIT_PX;
        }
    }
    int j = 0;
    CalcVal v;
    if (!calc_primary(buf, len, &j, &v, 0) || j != len || v.is_number || v.pct != 0.0 || v.em != 0.0) return 0;
    *out = (float)v.px;
    *used = len;
    return 1;
}

int css_parse_time_s(const CssToken *tokens, int n, float *out)
{
    CssToken t;
    if (!single_nows(tokens, n, &t)) return 0;
    return time_from_token(&t, out);
}

/* ========================================================================= */
/* Keywords                                                                  */
/* ========================================================================= */

int css_keyword_lookup(const CssToken *tok, const CssKeyword *table, int *out)
{
    if (tok->type != CSS_TOK_IDENT) return 0;
    for (const CssKeyword *k = table; k->name; k++) {
        if (kn_str_ieq(tok->text, tok->len, k->name)) { *out = k->value; return 1; }
    }
    return 0;
}

int css_parse_keyword(const CssToken *tokens, int n, const CssKeyword *table, int *out)
{
    CssToken t;
    if (!single_nows(tokens, n, &t)) return 0;
    return css_keyword_lookup(&t, table, out);
}

const CssKeyword css_kw_display[] = {
    { "inline", CSS_DISPLAY_INLINE }, { "block", CSS_DISPLAY_BLOCK }, { "none", CSS_DISPLAY_NONE },
    { "inline-block", CSS_DISPLAY_INLINE_BLOCK }, { "flex", CSS_DISPLAY_FLEX }, { "inline-flex", CSS_DISPLAY_INLINE_FLEX },
    { "grid", CSS_DISPLAY_GRID }, { "inline-grid", CSS_DISPLAY_INLINE_GRID }, { "table", CSS_DISPLAY_TABLE },
    { "inline-table", CSS_DISPLAY_INLINE_TABLE }, { "table-row", CSS_DISPLAY_TABLE_ROW }, { "table-cell", CSS_DISPLAY_TABLE_CELL },
    { "table-row-group", CSS_DISPLAY_TABLE_ROW_GROUP }, { "list-item", CSS_DISPLAY_LIST_ITEM }, { "contents", CSS_DISPLAY_CONTENTS },
    { "flow-root", CSS_DISPLAY_FLOW_ROOT },
    { "-webkit-box", CSS_DISPLAY_FLEX }, { "-webkit-flex", CSS_DISPLAY_FLEX }, { "-webkit-inline-box", CSS_DISPLAY_INLINE_FLEX },
    { NULL, 0 }
};
const CssKeyword css_kw_position[] = {
    { "static", CSS_POSITION_STATIC }, { "relative", CSS_POSITION_RELATIVE }, { "absolute", CSS_POSITION_ABSOLUTE },
    { "fixed", CSS_POSITION_FIXED }, { "sticky", CSS_POSITION_STICKY }, /* no -webkit-sticky: Blink rejects it */
    { NULL, 0 }
};
const CssKeyword css_kw_overflow[] = {
    { "visible", CSS_OVERFLOW_VISIBLE }, { "hidden", CSS_OVERFLOW_HIDDEN }, { "scroll", CSS_OVERFLOW_SCROLL },
    { "auto", CSS_OVERFLOW_AUTO }, { "clip", CSS_OVERFLOW_CLIP }, { "overlay", CSS_OVERFLOW_AUTO }, { NULL, 0 }
};
const CssKeyword css_kw_cursor[] = {
    { "auto", CSS_CURSOR_AUTO }, { "default", CSS_CURSOR_DEFAULT }, { "pointer", CSS_CURSOR_POINTER },
    { "none", CSS_CURSOR_NONE }, { "crosshair", CSS_CURSOR_CROSSHAIR }, { "text", CSS_CURSOR_TEXT },
    { "move", CSS_CURSOR_MOVE }, { "not-allowed", CSS_CURSOR_NOT_ALLOWED }, { "grab", CSS_CURSOR_GRAB },
    { "grabbing", CSS_CURSOR_GRABBING }, { "wait", CSS_CURSOR_WAIT }, { "help", CSS_CURSOR_HELP },
    { "hand", CSS_CURSOR_POINTER },
    { "ew-resize", CSS_CURSOR_EW_RESIZE }, { "col-resize", CSS_CURSOR_EW_RESIZE }, { "e-resize", CSS_CURSOR_EW_RESIZE },
    { "w-resize", CSS_CURSOR_EW_RESIZE }, { "ns-resize", CSS_CURSOR_NS_RESIZE }, { "row-resize", CSS_CURSOR_NS_RESIZE },
    { "n-resize", CSS_CURSOR_NS_RESIZE }, { "s-resize", CSS_CURSOR_NS_RESIZE }, { "nwse-resize", CSS_CURSOR_NWSE_RESIZE },
    { "nw-resize", CSS_CURSOR_NWSE_RESIZE }, { "se-resize", CSS_CURSOR_NWSE_RESIZE }, { "nesw-resize", CSS_CURSOR_NESW_RESIZE },
    { "ne-resize", CSS_CURSOR_NESW_RESIZE }, { "sw-resize", CSS_CURSOR_NESW_RESIZE }, { NULL, 0 }
};
const CssKeyword css_kw_border_style[] = {
    { "none", CSS_BORDER_STYLE_NONE }, { "hidden", CSS_BORDER_STYLE_HIDDEN }, { "solid", CSS_BORDER_STYLE_SOLID },
    { "dashed", CSS_BORDER_STYLE_DASHED }, { "dotted", CSS_BORDER_STYLE_DOTTED }, { "double", CSS_BORDER_STYLE_DOUBLE },
    { "groove", CSS_BORDER_STYLE_GROOVE }, { "ridge", CSS_BORDER_STYLE_RIDGE }, { "inset", CSS_BORDER_STYLE_INSET },
    { "outset", CSS_BORDER_STYLE_OUTSET }, { NULL, 0 }
};
const CssKeyword css_kw_timing[] = {
    { "ease", CSS_TIMING_EASE }, { "linear", CSS_TIMING_LINEAR }, { "ease-in", CSS_TIMING_EASE_IN },
    { "ease-out", CSS_TIMING_EASE_OUT }, { "ease-in-out", CSS_TIMING_EASE_IN_OUT },
    { "step-start", CSS_TIMING_STEPS }, { "step-end", CSS_TIMING_STEPS }, { NULL, 0 }
};

static const CssKeyword kw_float[] = { { "none", CSS_FLOAT_NONE }, { "left", CSS_FLOAT_LEFT }, { "right", CSS_FLOAT_RIGHT }, { NULL, 0 } };
static const CssKeyword kw_clear[] = { { "none", CSS_CLEAR_NONE }, { "left", CSS_CLEAR_LEFT }, { "right", CSS_CLEAR_RIGHT }, { "both", CSS_CLEAR_BOTH }, { NULL, 0 } };
static const CssKeyword kw_visibility[] = { { "visible", CSS_VISIBILITY_VISIBLE }, { "hidden", CSS_VISIBILITY_HIDDEN }, { "collapse", CSS_VISIBILITY_COLLAPSE }, { NULL, 0 } };
static const CssKeyword kw_transform_style[] = { { "flat", CSS_TRANSFORM_STYLE_FLAT }, { "preserve-3d", CSS_TRANSFORM_STYLE_PRESERVE_3D }, { NULL, 0 } };
static const CssKeyword kw_backface[] = { { "visible", CSS_BACKFACE_VISIBLE }, { "hidden", CSS_BACKFACE_HIDDEN }, { NULL, 0 } };
static const CssKeyword kw_bg_box[] = { { "border-box", CSS_BG_BOX_BORDER }, { "padding-box", CSS_BG_BOX_PADDING },
                                        { "content-box", CSS_BG_BOX_CONTENT }, { "text", CSS_BG_BOX_TEXT }, { NULL, 0 } };
static const CssKeyword kw_mask_mode[] = { { "match-source", CSS_MASK_MODE_MATCH_SOURCE }, { "alpha", CSS_MASK_MODE_ALPHA },
                                           { "luminance", CSS_MASK_MODE_LUMINANCE }, { NULL, 0 } };
static const CssKeyword kw_box_orient[] = { { "horizontal", CSS_BOX_ORIENT_HORIZONTAL }, { "inline-axis", CSS_BOX_ORIENT_HORIZONTAL },
                                            { "vertical", CSS_BOX_ORIENT_VERTICAL }, { "block-axis", CSS_BOX_ORIENT_VERTICAL }, { NULL, 0 } };
static const CssKeyword kw_pointer_events[] = { { "auto", CSS_POINTER_EVENTS_AUTO }, { "none", CSS_POINTER_EVENTS_NONE }, { "all", CSS_POINTER_EVENTS_ALL }, { "visible", CSS_POINTER_EVENTS_AUTO }, { "painted", CSS_POINTER_EVENTS_AUTO }, { NULL, 0 } };
static const CssKeyword kw_mix_blend_mode[] = {
    { "normal", CSS_BLEND_NORMAL }, { "multiply", CSS_BLEND_MULTIPLY }, { "screen", CSS_BLEND_SCREEN },
    { "overlay", CSS_BLEND_OVERLAY }, { "darken", CSS_BLEND_DARKEN }, { "lighten", CSS_BLEND_LIGHTEN },
    { "color-dodge", CSS_BLEND_COLOR_DODGE }, { "color-burn", CSS_BLEND_COLOR_BURN }, { "hard-light", CSS_BLEND_HARD_LIGHT },
    { "soft-light", CSS_BLEND_SOFT_LIGHT }, { "difference", CSS_BLEND_DIFFERENCE }, { "exclusion", CSS_BLEND_EXCLUSION },
    { "hue", CSS_BLEND_HUE }, { "saturation", CSS_BLEND_SATURATION }, { "color", CSS_BLEND_COLOR },
    { "luminosity", CSS_BLEND_LUMINOSITY }, { "plus-lighter", CSS_BLEND_NORMAL }, { NULL, 0 }
};
static const CssKeyword kw_user_select[] = { { "auto", CSS_USER_SELECT_AUTO }, { "none", CSS_USER_SELECT_NONE }, { "text", CSS_USER_SELECT_TEXT }, { "all", CSS_USER_SELECT_ALL }, { "contain", CSS_USER_SELECT_AUTO }, { NULL, 0 } };
static const CssKeyword kw_direction[] = { { "ltr", CSS_DIRECTION_LTR }, { "rtl", CSS_DIRECTION_RTL }, { NULL, 0 } };
static const CssKeyword kw_appearance[] = { { "auto", CSS_APPEARANCE_AUTO }, { "none", CSS_APPEARANCE_NONE }, { "textfield", CSS_APPEARANCE_TEXTFIELD }, { "menulist", CSS_APPEARANCE_MENULIST }, { "menulist-button", CSS_APPEARANCE_MENULIST }, { "button", CSS_APPEARANCE_BUTTON }, { "checkbox", CSS_APPEARANCE_AUTO }, { "radio", CSS_APPEARANCE_AUTO }, { "slider-horizontal", CSS_APPEARANCE_AUTO }, { NULL, 0 } };
static const CssKeyword kw_box_sizing[] = { { "content-box", CSS_BOX_SIZING_CONTENT_BOX }, { "border-box", CSS_BOX_SIZING_BORDER_BOX }, { NULL, 0 } };
static const CssKeyword kw_table_layout[] = { { "auto", CSS_TABLE_LAYOUT_AUTO }, { "fixed", CSS_TABLE_LAYOUT_FIXED }, { NULL, 0 } };
static const CssKeyword kw_text_align[] = { { "start", CSS_TEXT_ALIGN_START }, { "left", CSS_TEXT_ALIGN_LEFT }, { "right", CSS_TEXT_ALIGN_RIGHT }, { "center", CSS_TEXT_ALIGN_CENTER }, { "justify", CSS_TEXT_ALIGN_JUSTIFY }, { "end", CSS_TEXT_ALIGN_END }, { "-webkit-center", CSS_TEXT_ALIGN_CENTER }, { NULL, 0 } };
static const CssKeyword kw_text_transform[] = { { "none", CSS_TEXT_TRANSFORM_NONE }, { "uppercase", CSS_TEXT_TRANSFORM_UPPERCASE }, { "lowercase", CSS_TEXT_TRANSFORM_LOWERCASE }, { "capitalize", CSS_TEXT_TRANSFORM_CAPITALIZE }, { NULL, 0 } };
/* text-wrap: 0 = wrap (the initial value and every wrap keyword: auto, balance, pretty, stable), 1 = nowrap */
static const CssKeyword kw_text_wrap[] = { { "normal", 0 }, { "wrap", 0 }, { "auto", 0 }, { "balance", 0 }, { "pretty", 0 },
                                           { "stable", 0 }, { "nowrap", 1 }, { NULL, 0 } };
static const CssKeyword kw_white_space[] = { { "normal", CSS_WHITE_SPACE_NORMAL }, { "nowrap", CSS_WHITE_SPACE_NOWRAP }, { "pre", CSS_WHITE_SPACE_PRE }, { "pre-wrap", CSS_WHITE_SPACE_PRE_WRAP }, { "pre-line", CSS_WHITE_SPACE_PRE_LINE }, { "break-spaces", CSS_WHITE_SPACE_PRE_WRAP }, { NULL, 0 } };
static const CssKeyword kw_overflow_wrap[] = { { "normal", CSS_OVERFLOW_WRAP_NORMAL }, { "break-word", CSS_OVERFLOW_WRAP_BREAK_WORD }, { "anywhere", CSS_OVERFLOW_WRAP_ANYWHERE }, { NULL, 0 } };
static const CssKeyword kw_word_break[] = { { "normal", CSS_WORD_BREAK_NORMAL }, { "break-all", CSS_WORD_BREAK_BREAK_ALL }, { "keep-all", CSS_WORD_BREAK_KEEP_ALL }, { "break-word", CSS_WORD_BREAK_BREAK_WORD }, { NULL, 0 } };
static const CssKeyword kw_vertical_align[] = { { "baseline", CSS_VERTICAL_ALIGN_BASELINE }, { "top", CSS_VERTICAL_ALIGN_TOP }, { "middle", CSS_VERTICAL_ALIGN_MIDDLE }, { "bottom", CSS_VERTICAL_ALIGN_BOTTOM }, { "sub", CSS_VERTICAL_ALIGN_SUB }, { "super", CSS_VERTICAL_ALIGN_SUPER }, { "text-top", CSS_VERTICAL_ALIGN_TEXT_TOP }, { "text-bottom", CSS_VERTICAL_ALIGN_TEXT_BOTTOM }, { NULL, 0 } };
static const CssKeyword kw_image_rendering[] = { { "auto", CSS_IMAGE_RENDERING_AUTO }, { "pixelated", CSS_IMAGE_RENDERING_PIXELATED }, { "crisp-edges", CSS_IMAGE_RENDERING_CRISP_EDGES }, { "-webkit-optimize-contrast", CSS_IMAGE_RENDERING_CRISP_EDGES }, { NULL, 0 } };
/* ^ only the keywords Blink accepts: -moz-/-webkit-crisp-edges, optimizeSpeed/Quality,
 * smooth and high-quality are invalid there, so they must not override an earlier value. */
static const CssKeyword kw_object_fit[] = { { "fill", CSS_OBJECT_FIT_FILL }, { "contain", CSS_OBJECT_FIT_CONTAIN }, { "cover", CSS_OBJECT_FIT_COVER }, { "none", CSS_OBJECT_FIT_NONE }, { "scale-down", CSS_OBJECT_FIT_SCALE_DOWN }, { NULL, 0 } };
static const CssKeyword kw_flex_direction[] = { { "row", CSS_FLEX_DIRECTION_ROW }, { "row-reverse", CSS_FLEX_DIRECTION_ROW_REVERSE }, { "column", CSS_FLEX_DIRECTION_COLUMN }, { "column-reverse", CSS_FLEX_DIRECTION_COLUMN_REVERSE }, { NULL, 0 } };
static const CssKeyword kw_flex_wrap[] = { { "nowrap", CSS_FLEX_WRAP_NOWRAP }, { "wrap", CSS_FLEX_WRAP_WRAP }, { "wrap-reverse", CSS_FLEX_WRAP_WRAP_REVERSE }, { NULL, 0 } };
static const CssKeyword kw_justify_content[] = { { "flex-start", CSS_JUSTIFY_CONTENT_FLEX_START }, { "flex-end", CSS_JUSTIFY_CONTENT_FLEX_END }, { "center", CSS_JUSTIFY_CONTENT_CENTER }, { "space-between", CSS_JUSTIFY_CONTENT_SPACE_BETWEEN }, { "space-around", CSS_JUSTIFY_CONTENT_SPACE_AROUND }, { "space-evenly", CSS_JUSTIFY_CONTENT_SPACE_EVENLY }, { "start", CSS_JUSTIFY_CONTENT_FLEX_START }, { "end", CSS_JUSTIFY_CONTENT_FLEX_END }, { "left", CSS_JUSTIFY_CONTENT_FLEX_START }, { "right", CSS_JUSTIFY_CONTENT_FLEX_END }, { "normal", CSS_JUSTIFY_CONTENT_NORMAL }, { "stretch", CSS_JUSTIFY_CONTENT_NORMAL }, { NULL, 0 } };
static const CssKeyword kw_align_items[] = { { "stretch", CSS_ALIGN_ITEMS_STRETCH }, { "flex-start", CSS_ALIGN_ITEMS_FLEX_START }, { "flex-end", CSS_ALIGN_ITEMS_FLEX_END }, { "center", CSS_ALIGN_ITEMS_CENTER }, { "baseline", CSS_ALIGN_ITEMS_BASELINE }, { "start", CSS_ALIGN_ITEMS_FLEX_START }, { "end", CSS_ALIGN_ITEMS_FLEX_END }, { "self-start", CSS_ALIGN_ITEMS_FLEX_START }, { "self-end", CSS_ALIGN_ITEMS_FLEX_END }, { "normal", CSS_ALIGN_ITEMS_STRETCH }, { NULL, 0 } };
/* align-self / justify-self (auto: the container's align-items / justify-items) and justify-items */
static const CssKeyword kw_self_align[] = { { "auto", CSS_ALIGN_ITEMS_AUTO }, { "normal", CSS_ALIGN_ITEMS_STRETCH }, { "stretch", CSS_ALIGN_ITEMS_STRETCH }, { "start", CSS_ALIGN_ITEMS_FLEX_START }, { "end", CSS_ALIGN_ITEMS_FLEX_END }, { "flex-start", CSS_ALIGN_ITEMS_FLEX_START }, { "flex-end", CSS_ALIGN_ITEMS_FLEX_END }, { "self-start", CSS_ALIGN_ITEMS_FLEX_START }, { "self-end", CSS_ALIGN_ITEMS_FLEX_END }, { "left", CSS_ALIGN_ITEMS_FLEX_START }, { "right", CSS_ALIGN_ITEMS_FLEX_END }, { "center", CSS_ALIGN_ITEMS_CENTER }, { "baseline", CSS_ALIGN_ITEMS_BASELINE }, { NULL, 0 } };
static const CssKeyword kw_justify_items[] = { { "legacy", CSS_ALIGN_ITEMS_STRETCH }, { "normal", CSS_ALIGN_ITEMS_STRETCH }, { "stretch", CSS_ALIGN_ITEMS_STRETCH }, { "start", CSS_ALIGN_ITEMS_FLEX_START }, { "end", CSS_ALIGN_ITEMS_FLEX_END }, { "flex-start", CSS_ALIGN_ITEMS_FLEX_START }, { "flex-end", CSS_ALIGN_ITEMS_FLEX_END }, { "self-start", CSS_ALIGN_ITEMS_FLEX_START }, { "self-end", CSS_ALIGN_ITEMS_FLEX_END }, { "left", CSS_ALIGN_ITEMS_FLEX_START }, { "right", CSS_ALIGN_ITEMS_FLEX_END }, { "center", CSS_ALIGN_ITEMS_CENTER }, { "baseline", CSS_ALIGN_ITEMS_BASELINE }, { NULL, 0 } };
static const CssKeyword kw_border_collapse[] = { { "separate", CSS_BORDER_COLLAPSE_SEPARATE }, { "collapse", CSS_BORDER_COLLAPSE_COLLAPSE }, { NULL, 0 } };
static const CssKeyword kw_font_style[] = { { "normal", CSS_FONT_STYLE_NORMAL }, { "italic", CSS_FONT_STYLE_ITALIC }, { "oblique", CSS_FONT_STYLE_OBLIQUE }, { NULL, 0 } };
static const CssKeyword kw_list_style[] = { { "disc", CSS_LIST_STYLE_DISC }, { "none", CSS_LIST_STYLE_NONE }, { "circle", CSS_LIST_STYLE_CIRCLE }, { "square", CSS_LIST_STYLE_SQUARE }, { "decimal", CSS_LIST_STYLE_DECIMAL }, { NULL, 0 } };
static const CssKeyword kw_bg_repeat[] = { { "repeat", CSS_BG_REPEAT_REPEAT }, { "no-repeat", CSS_BG_REPEAT_NO_REPEAT }, { "round", CSS_BG_REPEAT_ROUND }, { "space", CSS_BG_REPEAT_SPACE }, { NULL, 0 } };
static const CssKeyword kw_anim_direction[] = { { "normal", CSS_ANIM_DIRECTION_NORMAL }, { "reverse", CSS_ANIM_DIRECTION_REVERSE }, { "alternate", CSS_ANIM_DIRECTION_ALTERNATE }, { "alternate-reverse", CSS_ANIM_DIRECTION_ALTERNATE_REVERSE }, { NULL, 0 } };
static const CssKeyword kw_anim_fill[] = { { "none", CSS_ANIM_FILL_NONE }, { "forwards", CSS_ANIM_FILL_FORWARDS }, { "backwards", CSS_ANIM_FILL_BACKWARDS }, { "both", CSS_ANIM_FILL_BOTH }, { NULL, 0 } };
static const CssKeyword kw_anim_play[] = { { "running", CSS_ANIM_PLAY_RUNNING }, { "paused", CSS_ANIM_PLAY_PAUSED }, { NULL, 0 } };
static const CssKeyword kw_text_decoration[] = { { "none", CSS_TEXT_DECORATION_NONE }, { "underline", CSS_TEXT_DECORATION_UNDERLINE }, { "overline", CSS_TEXT_DECORATION_OVERLINE }, { "line-through", CSS_TEXT_DECORATION_LINE_THROUGH }, { NULL, 0 } };

/* ========================================================================= */
/* Colors                                                                    */
/* ========================================================================= */

typedef struct NamedColor { const char *name; uint32_t rgb; } NamedColor;

static const NamedColor g_named_colors[] = {
    {"aliceblue",0xf0f8ff},{"antiquewhite",0xfaebd7},{"aqua",0x00ffff},{"aquamarine",0x7fffd4},{"azure",0xf0ffff},
    {"beige",0xf5f5dc},{"bisque",0xffe4c4},{"black",0x000000},{"blanchedalmond",0xffebcd},{"blue",0x0000ff},
    {"blueviolet",0x8a2be2},{"brown",0xa52a2a},{"burlywood",0xdeb887},{"cadetblue",0x5f9ea0},{"chartreuse",0x7fff00},
    {"chocolate",0xd2691e},{"coral",0xff7f50},{"cornflowerblue",0x6495ed},{"cornsilk",0xfff8dc},{"crimson",0xdc143c},
    {"cyan",0x00ffff},{"darkblue",0x00008b},{"darkcyan",0x008b8b},{"darkgoldenrod",0xb8860b},{"darkgray",0xa9a9a9},
    {"darkgreen",0x006400},{"darkgrey",0xa9a9a9},{"darkkhaki",0xbdb76b},{"darkmagenta",0x8b008b},{"darkolivegreen",0x556b2f},
    {"darkorange",0xff8c00},{"darkorchid",0x9932cc},{"darkred",0x8b0000},{"darksalmon",0xe9967a},{"darkseagreen",0x8fbc8f},
    {"darkslateblue",0x483d8b},{"darkslategray",0x2f4f4f},{"darkslategrey",0x2f4f4f},{"darkturquoise",0x00ced1},{"darkviolet",0x9400d3},
    {"deeppink",0xff1493},{"deepskyblue",0x00bfff},{"dimgray",0x696969},{"dimgrey",0x696969},{"dodgerblue",0x1e90ff},
    {"firebrick",0xb22222},{"floralwhite",0xfffaf0},{"forestgreen",0x228b22},{"fuchsia",0xff00ff},{"gainsboro",0xdcdcdc},
    {"ghostwhite",0xf8f8ff},{"gold",0xffd700},{"goldenrod",0xdaa520},{"gray",0x808080},{"green",0x008000},
    {"greenyellow",0xadff2f},{"grey",0x808080},{"honeydew",0xf0fff0},{"hotpink",0xff69b4},{"indianred",0xcd5c5c},
    {"indigo",0x4b0082},{"ivory",0xfffff0},{"khaki",0xf0e68c},{"lavender",0xe6e6fa},{"lavenderblush",0xfff0f5},
    {"lawngreen",0x7cfc00},{"lemonchiffon",0xfffacd},{"lightblue",0xadd8e6},{"lightcoral",0xf08080},{"lightcyan",0xe0ffff},
    {"lightgoldenrodyellow",0xfafad2},{"lightgray",0xd3d3d3},{"lightgreen",0x90ee90},{"lightgrey",0xd3d3d3},{"lightpink",0xffb6c1},
    {"lightsalmon",0xffa07a},{"lightseagreen",0x20b2aa},{"lightskyblue",0x87cefa},{"lightslategray",0x778899},{"lightslategrey",0x778899},
    {"lightsteelblue",0xb0c4de},{"lightyellow",0xffffe0},{"lime",0x00ff00},{"limegreen",0x32cd32},{"linen",0xfaf0e6},
    {"magenta",0xff00ff},{"maroon",0x800000},{"mediumaquamarine",0x66cdaa},{"mediumblue",0x0000cd},{"mediumorchid",0xba55d3},
    {"mediumpurple",0x9370db},{"mediumseagreen",0x3cb371},{"mediumslateblue",0x7b68ee},{"mediumspringgreen",0x00fa9a},{"mediumturquoise",0x48d1cc},
    {"mediumvioletred",0xc71585},{"midnightblue",0x191970},{"mintcream",0xf5fffa},{"mistyrose",0xffe4e1},{"moccasin",0xffe4b5},
    {"navajowhite",0xffdead},{"navy",0x000080},{"oldlace",0xfdf5e6},{"olive",0x808000},{"olivedrab",0x6b8e23},
    {"orange",0xffa500},{"orangered",0xff4500},{"orchid",0xda70d6},{"palegoldenrod",0xeee8aa},{"palegreen",0x98fb98},
    {"paleturquoise",0xafeeee},{"palevioletred",0xdb7093},{"papayawhip",0xffefd5},{"peachpuff",0xffdab9},{"peru",0xcd853f},
    {"pink",0xffc0cb},{"plum",0xdda0dd},{"powderblue",0xb0e0e6},{"purple",0x800080},{"rebeccapurple",0x663399},
    {"red",0xff0000},{"rosybrown",0xbc8f8f},{"royalblue",0x4169e1},{"saddlebrown",0x8b4513},{"salmon",0xfa8072},
    {"sandybrown",0xf4a460},{"seagreen",0x2e8b57},{"seashell",0xfff5ee},{"sienna",0xa0522d},{"silver",0xc0c0c0},
    {"skyblue",0x87ceeb},{"slateblue",0x6a5acd},{"slategray",0x708090},{"slategrey",0x708090},{"snow",0xfffafa},
    {"springgreen",0x00ff7f},{"steelblue",0x4682b4},{"tan",0xd2b48c},{"teal",0x008080},{"thistle",0xd8bfd8},
    {"tomato",0xff6347},{"turquoise",0x40e0d0},{"violet",0xee82ee},{"wheat",0xf5deb3},{"white",0xffffff},
    {"whitesmoke",0xf5f5f5},{"yellow",0xffff00},{"yellowgreen",0x9acd32},
    {NULL,0}
};

static uint8_t clamp_u8_fwd(double v)
{
    if (!(v > 0)) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)(v + 0.5);
}

CssColor css_color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    CssColor c = { r, g, b, a, CSS_COLOR_RGBA };
    return c;
}

CssColor css_color_resolve(CssColor c, CssColor current)
{
    if (c.kind == CSS_COLOR_CURRENT) return current;
    if (c.kind == CSS_COLOR_CURRENT_MIX) {
        CssColor other = css_color_rgba(c.r, c.g, c.b, c.a);
        if (current.kind != CSS_COLOR_RGBA) current = css_color_rgba(current.r, current.g, current.b, current.a);
        return css_color_mix(current, other, c.mix / 200.0f, 1.0f, c.mix_space);
    }
    return c;
}

/* sRGB <-> OKLab (Björn Ottosson, CSS Color 4 §17). */
static double srgb_to_linear(double v) { return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
static double linear_to_srgb(double v) { return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055; }

static void rgb_to_oklab(const double rgb[3], double lab[3])
{
    const double r = srgb_to_linear(rgb[0]), g = srgb_to_linear(rgb[1]), b = srgb_to_linear(rgb[2]);
    const double l = cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    lab[0] = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    lab[1] = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    lab[2] = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
}

static void oklab_to_rgb(const double lab[3], double rgb[3])
{
    double l = lab[0] + 0.3963377774 * lab[1] + 0.2158037573 * lab[2];
    double m = lab[0] - 0.1055613458 * lab[1] - 0.0638541728 * lab[2];
    double s = lab[0] - 0.0894841775 * lab[1] - 1.2914855480 * lab[2];
    l = l * l * l; m = m * m * m; s = s * s * s;
    rgb[0] = linear_to_srgb(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s);
    rgb[1] = linear_to_srgb(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s);
    rgb[2] = linear_to_srgb(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s);
}

CssColor css_color_mix(CssColor c1, CssColor c2, float w1, float alpha_mul, int space)
{
    const double a1 = c1.a / 255.0, a2 = c2.a / 255.0, k1 = w1, k2 = 1.0 - w1;
    double v1[3] = { c1.r / 255.0, c1.g / 255.0, c1.b / 255.0 };
    double v2[3] = { c2.r / 255.0, c2.g / 255.0, c2.b / 255.0 };
    if (space == CSS_COLOR_SPACE_OKLAB) {
        double t[3];
        rgb_to_oklab(v1, t); memcpy(v1, t, sizeof t);
        rgb_to_oklab(v2, t); memcpy(v2, t, sizeof t);
    }
    const double a = a1 * k1 + a2 * k2;
    CssColor res;
    if (a <= 1e-9) {
        res = css_color_rgba(0, 0, 0, 0);
    } else {
        double o[3];
        for (int i = 0; i < 3; i++) o[i] = (v1[i] * a1 * k1 + v2[i] * a2 * k2) / a;   /* premultiplied interpolation */
        if (space == CSS_COLOR_SPACE_OKLAB) {
            double t[3];
            oklab_to_rgb(o, t);
            memcpy(o, t, sizeof t);
        }
        res = css_color_rgba(clamp_u8_fwd(o[0] * 255.0), clamp_u8_fwd(o[1] * 255.0), clamp_u8_fwd(o[2] * 255.0),
                             clamp_u8_fwd(a * alpha_mul * 255.0));
    }
    res.nonlegacy = 1;   /* a color-mix() result is non-legacy whatever the space (Chrome serialises color(srgb ...)) */
    return res;
}

static uint8_t clamp_u8(double v)
{
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)(v + 0.5);
}

static int parse_hex_color(const CssToken *t, CssColor *out)
{
    const char *s = t->text;
    size_t n = t->len;
    for (size_t i = 0; i < n; i++) if (!kn_is_hex((unsigned char)s[i])) return 0;
    unsigned v[8];
    for (size_t i = 0; i < n && i < 8; i++) v[i] = (unsigned)kn_hex_val((unsigned char)s[i]);
    switch (n) {
    case 3: *out = css_color_rgba((uint8_t)(v[0] * 17), (uint8_t)(v[1] * 17), (uint8_t)(v[2] * 17), 255); return 1;
    case 4: *out = css_color_rgba((uint8_t)(v[0] * 17), (uint8_t)(v[1] * 17), (uint8_t)(v[2] * 17), (uint8_t)(v[3] * 17)); return 1;
    case 6: *out = css_color_rgba((uint8_t)(v[0] * 16 + v[1]), (uint8_t)(v[2] * 16 + v[3]), (uint8_t)(v[4] * 16 + v[5]), 255); return 1;
    case 8: *out = css_color_rgba((uint8_t)(v[0] * 16 + v[1]), (uint8_t)(v[2] * 16 + v[3]), (uint8_t)(v[4] * 16 + v[5]), (uint8_t)(v[6] * 16 + v[7])); return 1;
    default: return 0;
    }
}

/* Collects up to 4 numeric args of an rgb()/hsl() body, accepting commas,
 * spaces and the `/ alpha` separator.  args[i] receives value, is_pct[i]
 * flags percentages. */
static int color_args(const CssToken *t, int n, double *args, int *is_pct, int *count)
{
    int c = 0;
    for (int i = 0; i < n; i++) {
        const CssToken *k = &t[i];
        if (k->type == CSS_TOK_COMMA || tok_is_delim(k, '/')) continue;
        if (k->type == CSS_TOK_NUMBER || k->type == CSS_TOK_PERCENTAGE) {
            if (c >= 4) return 0;
            args[c] = k->num;
            is_pct[c] = k->type == CSS_TOK_PERCENTAGE;
            c++;
        } else if (k->type == CSS_TOK_DIMENSION && c == 0) {
            float deg;
            if (!angle_from_token(k, &deg)) return 0;
            args[c] = deg;
            is_pct[c] = 0;
            c++;
        } else if (tok_is_ident(k, "none")) {
            if (c >= 4) return 0;
            args[c] = 0; is_pct[c] = 0; c++;
        } else return 0;
    }
    *count = c;
    return c == 3 || c == 4;
}

static double hue_to_rgb(double p, double q, double t)
{
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 1.0 / 2) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

/* color-mix(in <space>, <color> <pct>?, <color> <pct>?) (CSS Color 5 §2): srgb and oklab,
 * the spaces v10's sheets use (main.css .chalRewTile / .titleRow: `in srgb, currentColor 30%,
 * transparent`).  A mix with currentColor stays CSS_COLOR_CURRENT_MIX until used.  Known
 * gap: a percentage sum below 100% (alpha multiplier) is ignored for currentColor mixes. */
static int parse_color_mix(const CssToken *body, int n, CssColor *out)
{
    CssToken t[64];
    int m = tokens_copy_nows(body, n, t, 64);
    int st[4], ln[4];
    const int parts = split_commas(t, m, st, ln, 4);
    /* CSS Color 5 §2: the interpolation method is optional and defaults to oklab
     * (Chrome: color-mix(red, blue) == color-mix(in oklab, red, blue)) */
    int space = CSS_COLOR_SPACE_OKLAB, first = 0;
    if (parts == 3) {
        const CssToken *sp = t + st[0];
        if (ln[0] != 2 || !tok_is_ident(&sp[0], "in")) return 0;
        if (tok_is_ident(&sp[1], "srgb")) space = CSS_COLOR_SPACE_SRGB;
        else if (tok_is_ident(&sp[1], "oklab")) space = CSS_COLOR_SPACE_OKLAB;
        else return 0;
        first = 1;
    } else if (parts != 2) {
        return 0;
    }
    CssColor c[2];
    float p[2];
    int has[2] = { 0, 0 };
    for (int k = 0; k < 2; k++) {
        const CssToken *q = t + st[k + first];
        int ql = ln[k + first], used = 0, i = 0;
        if (ql > 0 && q[0].type == CSS_TOK_PERCENTAGE) { p[k] = q[0].num; has[k] = 1; i = 1; }
        if (!css_parse_color_item(q + i, ql - i, &c[k], &used) || used <= 0) return 0;
        i += used;
        if (i < ql && !has[k] && q[i].type == CSS_TOK_PERCENTAGE) { p[k] = q[i].num; has[k] = 1; i++; }
        if (i != ql) return 0;
        if (has[k] && (p[k] < 0 || p[k] > 100)) return 0;
        if (c[k].kind == CSS_COLOR_CURRENT_MIX) return 0;
    }
    if (!has[0] && !has[1]) { p[0] = p[1] = 50; }
    else if (!has[0]) p[0] = 100 - p[1];
    else if (!has[1]) p[1] = 100 - p[0];
    const float sum = p[0] + p[1];
    if (sum <= 0) return 0;
    const float w1 = p[0] / sum, amul = sum < 100 ? sum / 100.0f : 1.0f;
    const int cur0 = c[0].kind == CSS_COLOR_CURRENT, cur1 = c[1].kind == CSS_COLOR_CURRENT;
    if (cur0 && cur1) { *out = c[0]; return 1; }
    if (cur0 || cur1) {
        const CssColor other = cur0 ? c[1] : c[0];
        const float wc = cur0 ? w1 : 1.0f - w1;
        CssColor r = other;
        r.kind = CSS_COLOR_CURRENT_MIX;
        r.mix = clamp_u8(wc * 200.0);
        r.mix_space = (uint8_t)space;
        r.nonlegacy = 1;
        *out = r;
        return 1;
    }
    *out = css_color_mix(c[0], c[1], w1, amul, space);
    return 1;
}

int css_parse_color_item(const CssToken *tokens, int n, CssColor *out, int *consumed)
{
    if (n <= 0) return 0;
    const CssToken *t = &tokens[0];
    if (t->type == CSS_TOK_HASH) {
        if (!parse_hex_color(t, out)) return 0;
        if (consumed) *consumed = 1;
        return 1;
    }
    if (t->type == CSS_TOK_IDENT) {
        if (tok_is_ident(t, "transparent")) { *out = css_color_rgba(0, 0, 0, 0); }
        else if (tok_is_ident(t, "currentcolor")) { CssColor c = { 0, 0, 0, 0, CSS_COLOR_CURRENT }; *out = c; }
        else {
            const NamedColor *nc = g_named_colors;
            for (; nc->name; nc++) if (kn_str_ieq(t->text, t->len, nc->name)) break;
            if (!nc->name) return 0;
            *out = css_color_rgba((uint8_t)(nc->rgb >> 16), (uint8_t)(nc->rgb >> 8), (uint8_t)nc->rgb, 255);
        }
        if (consumed) *consumed = 1;
        return 1;
    }
    if (t->type == CSS_TOK_FUNCTION && tok_is_func(t, "color-mix")) {
        int len = css_component_len(tokens, n, 0);
        if (len < 2 || tokens[len - 1].type != CSS_TOK_RPAREN) return 0;
        if (!parse_color_mix(tokens + 1, len - 2, out)) return 0;
        if (consumed) *consumed = len;
        return 1;
    }
    if (t->type == CSS_TOK_FUNCTION) {
        int len = css_component_len(tokens, n, 0);
        if (len < 2 || tokens[len - 1].type != CSS_TOK_RPAREN) return 0;
        double a[4]; int pct[4]; int cnt;
        int is_rgb = tok_is_func(t, "rgb") || tok_is_func(t, "rgba");
        int is_hsl = tok_is_func(t, "hsl") || tok_is_func(t, "hsla");
        if (!is_rgb && !is_hsl) return 0;
        if (!color_args(tokens + 1, len - 2, a, pct, &cnt)) return 0;
        double alpha = 1.0;
        if (cnt == 4) alpha = pct[3] ? a[3] / 100.0 : a[3];
        if (is_rgb) {
            double r = pct[0] ? a[0] * 2.55 : a[0];
            double g = pct[1] ? a[1] * 2.55 : a[1];
            double b = pct[2] ? a[2] * 2.55 : a[2];
            *out = css_color_rgba(clamp_u8(r), clamp_u8(g), clamp_u8(b), clamp_u8(alpha * 255.0));
        } else {
            double h = fmod(a[0], 360.0); if (h < 0) h += 360.0;
            double s = a[1] / 100.0, l = a[2] / 100.0;
            s = s < 0 ? 0 : s > 1 ? 1 : s;
            l = l < 0 ? 0 : l > 1 ? 1 : l;
            double r, g, b;
            if (s == 0) r = g = b = l;
            else {
                double q = l < 0.5 ? l * (1 + s) : l + s - l * s;
                double p = 2 * l - q;
                double hk = h / 360.0;
                r = hue_to_rgb(p, q, hk + 1.0 / 3); g = hue_to_rgb(p, q, hk); b = hue_to_rgb(p, q, hk - 1.0 / 3);
            }
            *out = css_color_rgba(clamp_u8(r * 255), clamp_u8(g * 255), clamp_u8(b * 255), clamp_u8(alpha * 255.0));
        }
        if (consumed) *consumed = len;
        return 1;
    }
    return 0;
}

int css_parse_color(const CssToken *tokens, int n, CssColor *out)
{
    CssToken t[64];
    int m = tokens_copy_nows(tokens, n, t, 64);
    int used = 0;
    CssColor c;
    if (!css_parse_color_item(t, m, &c, &used) || used != m) return 0;
    *out = c;
    return 1;
}

/* ========================================================================= */
/* Transforms                                                                */
/* ========================================================================= */

/* Parses function args (whitespace-free, inside the parens) into numbers,
 * accepting lengths for translate.  Returns arg count. */
static int func_args(const CssToken *t, int n, const CssToken **starts, int *lens, int max)
{
    int s[16], l[16];
    int c = split_commas(t, n, s, l, 16);
    if (c > max) return -1;
    for (int i = 0; i < c; i++) { starts[i] = t + s[i]; lens[i] = l[i]; }
    return c;
}

int css_parse_transform(const CssToken *tokens, int n, CssTransform *out)
{
    CssToken t[CSS_MAX_TOKENS];
    int m = tokens_copy_nows(tokens, n, t, CSS_MAX_TOKENS);
    CssTransform r;
    memset(&r, 0, sizeof(r));
    if (m == 1 && tok_is_ident(&t[0], "none")) { *out = r; return 1; }
    int i = 0;
    while (i < m) {
        if (t[i].type != CSS_TOK_FUNCTION) return 0;
        int len = css_component_len(t, m, i);
        if (len < 2 || t[i + len - 1].type != CSS_TOK_RPAREN) return 0;
        const CssToken *as[6]; int al[6];
        int argc = func_args(t + i + 1, len - 2, as, al, 6);
        if (argc < 0) return 0;
        const CssToken *f = &t[i];
        CssTransformOp op;
        memset(&op, 0, sizeof(op));
        int keep = 1;
        unsigned lf = CSS_LENF_PCT;
        if (tok_is_func(f, "translate")) {
            if (argc < 1 || argc > 2) return 0;
            op.kind = CSS_TRANSFORM_TRANSLATE;
            if (!css_parse_length(as[0], al[0], lf, &op.tx)) return 0;
            if (argc == 2 && !css_parse_length(as[1], al[1], lf, &op.ty)) return 0;
        } else if (tok_is_func(f, "translatex")) {
            if (argc != 1) return 0;
            op.kind = CSS_TRANSFORM_TRANSLATE;
            if (!css_parse_length(as[0], al[0], lf, &op.tx)) return 0;
        } else if (tok_is_func(f, "translatey")) {
            if (argc != 1) return 0;
            op.kind = CSS_TRANSFORM_TRANSLATE;
            if (!css_parse_length(as[0], al[0], lf, &op.ty)) return 0;
        } else if (tok_is_func(f, "translatez")) {
            /* translateZ(0) (the compositing hint all over main.css) stays an identity
             * that only sets has_3d; a non-zero z is kept for perspective / preserve-3d */
            if (argc != 1) return 0;
            CssLength z;
            if (!css_parse_length(as[0], al[0], 0, &z)) return 0;
            r.has_3d = 1;
            op.kind = CSS_TRANSFORM_TRANSLATE;
            op.tz = z.px + z.em * 16.0f;
            keep = op.tz != 0.0f;
        } else if (tok_is_func(f, "translate3d")) {
            if (argc != 3) return 0;
            op.kind = CSS_TRANSFORM_TRANSLATE;
            CssLength z;
            if (!css_parse_length(as[0], al[0], lf, &op.tx) || !css_parse_length(as[1], al[1], lf, &op.ty) ||
                !css_parse_length(as[2], al[2], 0, &z)) return 0;
            op.tz = z.px + z.em * 16.0f;
            r.has_3d = 1;
        } else if (tok_is_func(f, "scale") || tok_is_func(f, "scale3d")) {
            const int s3 = tok_is_func(f, "scale3d");
            if (s3 ? argc != 3 : (argc < 1 || argc > 2)) return 0;
            if (s3) r.has_3d = 1;
            op.kind = CSS_TRANSFORM_SCALE;
            op.sz = 1;
            if (!css_parse_number(as[0], al[0], &op.sx)) return 0;
            op.sy = op.sx;
            if (argc >= 2 && !css_parse_number(as[1], al[1], &op.sy)) return 0;
            if (s3 && !css_parse_number(as[2], al[2], &op.sz)) return 0;
        } else if (tok_is_func(f, "scalex")) {
            if (argc != 1) return 0;
            op.kind = CSS_TRANSFORM_SCALE; op.sy = 1; op.sz = 1;
            if (!css_parse_number(as[0], al[0], &op.sx)) return 0;
        } else if (tok_is_func(f, "scaley")) {
            if (argc != 1) return 0;
            op.kind = CSS_TRANSFORM_SCALE; op.sx = 1; op.sz = 1;
            if (!css_parse_number(as[0], al[0], &op.sy)) return 0;
        } else if (tok_is_func(f, "scalez")) {
            if (argc != 1) return 0;
            keep = 0;
            r.has_3d = 1;
        } else if (tok_is_func(f, "rotate") || tok_is_func(f, "rotatez")) {
            if (argc != 1) return 0;
            op.kind = CSS_TRANSFORM_ROTATE;
            if (!css_parse_angle_deg(as[0], al[0], &op.angle_deg)) return 0;
        } else if (tok_is_func(f, "rotatex") || tok_is_func(f, "rotatey")) {
            if (argc != 1) return 0;
            op.kind = CSS_TRANSFORM_ROTATE_3D;
            if (!css_parse_angle_deg(as[0], al[0], &op.angle_deg)) return 0;
            if (tok_is_func(f, "rotatex")) op.axis[0] = 1; else op.axis[1] = 1;
            r.has_3d = 1;
        } else if (tok_is_func(f, "rotate3d")) {
            if (argc != 4) return 0;
            op.kind = CSS_TRANSFORM_ROTATE_3D;
            float v[3];
            for (int k = 0; k < 3; k++) if (!css_parse_number(as[k], al[k], &v[k])) return 0;
            if (!css_parse_angle_deg(as[3], al[3], &op.angle_deg)) return 0;
            const float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (len > 1e-12f) { op.axis[0] = v[0] / len; op.axis[1] = v[1] / len; op.axis[2] = v[2] / len; }
            else { op.axis[2] = 1; op.angle_deg = 0; }   /* rotate3d(0, 0, 0, a): identity */
            if (op.axis[0] == 0 && op.axis[1] == 0) {
                /* about z: an ordinary 2D rotation (sign of the axis flips the angle) */
                op.kind = CSS_TRANSFORM_ROTATE;
                if (op.axis[2] < 0) op.angle_deg = -op.angle_deg;
                op.axis[2] = 0;
            } else {
                r.has_3d = 1;
            }
        } else if (tok_is_func(f, "skew")) {
            if (argc < 1 || argc > 2) return 0;
            op.kind = CSS_TRANSFORM_SKEW;
            if (!css_parse_angle_deg(as[0], al[0], &op.angle_deg)) return 0;
            if (argc == 2 && !css_parse_angle_deg(as[1], al[1], &op.angle2_deg)) return 0;
        } else if (tok_is_func(f, "skewx")) {
            if (argc != 1) return 0;
            op.kind = CSS_TRANSFORM_SKEW;
            if (!css_parse_angle_deg(as[0], al[0], &op.angle_deg)) return 0;
        } else if (tok_is_func(f, "skewy")) {
            if (argc != 1) return 0;
            op.kind = CSS_TRANSFORM_SKEW;
            if (!css_parse_angle_deg(as[0], al[0], &op.angle2_deg)) return 0;
        } else if (tok_is_func(f, "matrix")) {
            if (argc != 6) return 0;
            op.kind = CSS_TRANSFORM_MATRIX;
            float *dst[6] = { &op.m.a, &op.m.b, &op.m.c, &op.m.d, &op.m.e, &op.m.f };
            for (int k = 0; k < 6; k++) if (!css_parse_number(as[k], al[k], dst[k])) return 0;
        } else if (tok_is_func(f, "perspective")) {
            if (argc != 1) return 0;
            op.kind = CSS_TRANSFORM_PERSPECTIVE;
            if (al[0] == 1 && tok_is_ident(as[0], "none")) op.persp = 0;
            else {
                CssLength d;
                if (!css_parse_length(as[0], al[0], CSS_LENF_NONNEG, &d)) return 0;
                op.persp = d.px + d.em * 16.0f;
                if (op.persp < 1.0f && op.persp >= 0.0f && (d.px != 0 || d.em != 0)) op.persp = 1.0f;  /* clamped to 1px (Chrome) */
                if (d.px == 0 && d.em == 0) op.persp = 1.0f;   /* perspective(0) acts like 1px */
            }
            r.has_3d = 1;
        } else return 0;
        if (keep) {
            if (r.count >= CSS_MAX_TRANSFORM_OPS) return 0;
            r.ops[r.count++] = op;
        }
        i += len;
    }
    *out = r;
    return 1;
}

/* ---- transform helpers -------------------------------------------------- */

static float op_sz(const CssTransformOp *op) { return op->sz == 0.0f ? 1.0f : op->sz; }   /* 0: written before sz existed (2D scale) */

void css_transform_op_identity(CssTransformOp *op, const CssTransformOp *like)
{
    memset(op, 0, sizeof *op);
    op->kind = like->kind;
    switch (like->kind) {
    case CSS_TRANSFORM_SCALE: op->sx = op->sy = op->sz = 1.0f; break;
    case CSS_TRANSFORM_MATRIX: op->m.a = op->m.d = 1.0f; break;
    case CSS_TRANSFORM_ROTATE_3D: memcpy(op->axis, like->axis, sizeof op->axis); break;
    default: break;   /* translate 0 0 0, rotate 0, skew 0 0, perspective none */
    }
}

static int len_eq(const CssLength *a, const CssLength *b)
{
    return a->kind == b->kind && a->px == b->px && a->pct == b->pct && a->em == b->em;
}

int css_transform_op_equal(const CssTransformOp *a, const CssTransformOp *b)
{
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
    case CSS_TRANSFORM_TRANSLATE: return len_eq(&a->tx, &b->tx) && len_eq(&a->ty, &b->ty) && a->tz == b->tz;
    case CSS_TRANSFORM_SCALE: return a->sx == b->sx && a->sy == b->sy && op_sz(a) == op_sz(b);
    case CSS_TRANSFORM_ROTATE: return a->angle_deg == b->angle_deg;
    case CSS_TRANSFORM_SKEW: return a->angle_deg == b->angle_deg && a->angle2_deg == b->angle2_deg;
    case CSS_TRANSFORM_ROTATE_3D:
        return a->angle_deg == b->angle_deg && a->axis[0] == b->axis[0] && a->axis[1] == b->axis[1] && a->axis[2] == b->axis[2];
    case CSS_TRANSFORM_PERSPECTIVE: return a->persp == b->persp;
    default: return memcmp(&a->m, &b->m, sizeof a->m) == 0;
    }
}

static float lerp1(float a, float b, float t) { return a + (b - a) * t; }

static void len_lerp(const CssLength *a, const CssLength *b, float t, CssLength *o)
{
    *o = *a;
    o->px = lerp1(a->px, b->px, t);
    o->pct = lerp1(a->pct, b->pct, t);
    o->em = lerp1(a->em, b->em, t);
    o->pct_term = a->pct_term | b->pct_term;
}

int css_transform_op_lerp(const CssTransformOp *a, const CssTransformOp *b, float t, CssTransformOp *out)
{
    if (a->kind != b->kind) return 0;
    memset(out, 0, sizeof *out);
    out->kind = a->kind;
    switch (a->kind) {
    case CSS_TRANSFORM_TRANSLATE:
        len_lerp(&a->tx, &b->tx, t, &out->tx);
        len_lerp(&a->ty, &b->ty, t, &out->ty);
        out->tz = lerp1(a->tz, b->tz, t);
        return 1;
    case CSS_TRANSFORM_SCALE:
        out->sx = lerp1(a->sx, b->sx, t);
        out->sy = lerp1(a->sy, b->sy, t);
        out->sz = lerp1(op_sz(a), op_sz(b), t);
        return 1;
    case CSS_TRANSFORM_ROTATE:
        out->angle_deg = lerp1(a->angle_deg, b->angle_deg, t);
        return 1;
    case CSS_TRANSFORM_SKEW:
        out->angle_deg = lerp1(a->angle_deg, b->angle_deg, t);
        out->angle2_deg = lerp1(a->angle2_deg, b->angle2_deg, t);
        return 1;
    case CSS_TRANSFORM_ROTATE_3D:
        /* same axis (CSS Transforms 2 §16.4: rotate3d with equal normalized axes interpolates the
         * angle); different axes would need quaternion slerp of the decomposed matrices */
        if (fabsf(a->axis[0] - b->axis[0]) > 1e-5f || fabsf(a->axis[1] - b->axis[1]) > 1e-5f ||
            fabsf(a->axis[2] - b->axis[2]) > 1e-5f) {
            if (a->angle_deg != 0.0f && b->angle_deg != 0.0f) return 0;
        }
        memcpy(out->axis, a->angle_deg != 0.0f ? a->axis : b->axis, sizeof out->axis);
        out->angle_deg = lerp1(a->angle_deg, b->angle_deg, t);
        return 1;
    case CSS_TRANSFORM_PERSPECTIVE: {
        /* interpolates the inverse distance (none == infinity == 0) like Chrome */
        const float ia = a->persp > 0 ? 1.0f / a->persp : 0.0f, ib = b->persp > 0 ? 1.0f / b->persp : 0.0f;
        const float i = lerp1(ia, ib, t);
        out->persp = i > 1e-9f ? 1.0f / i : 0.0f;
        return 1;
    }
    default:
        out->m.a = lerp1(a->m.a, b->m.a, t); out->m.b = lerp1(a->m.b, b->m.b, t);
        out->m.c = lerp1(a->m.c, b->m.c, t); out->m.d = lerp1(a->m.d, b->m.d, t);
        out->m.e = lerp1(a->m.e, b->m.e, t); out->m.f = lerp1(a->m.f, b->m.f, t);
        return 1;
    }
}

int css_transform_is_3d(const CssTransform *t)
{
    for (int i = 0; i < t->count && i < CSS_MAX_TRANSFORM_OPS; i++) {
        const CssTransformOp *op = &t->ops[i];
        if (op->kind == CSS_TRANSFORM_ROTATE_3D || op->kind == CSS_TRANSFORM_PERSPECTIVE) return 1;
        if (op->kind == CSS_TRANSFORM_TRANSLATE && op->tz != 0.0f) return 1;
        if (op->kind == CSS_TRANSFORM_SCALE && op_sz(op) != 1.0f) return 1;
    }
    return 0;
}

/* column-major: m[col * 4 + row] */
static void m4_identity(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void m4_mul(const float a[16], const float b[16], float out[16])
{
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int rw = 0; rw < 4; rw++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += a[k * 4 + rw] * b[c * 4 + k];
            r[c * 4 + rw] = v;
        }
    memcpy(out, r, sizeof r);
}

static float len_res(const CssLength *l, float ref)
{
    return l->kind == CSS_LEN_VALUE ? l->px + l->pct * 0.01f * ref : 0.0f;
}

void css_transform_to_mat4(const CssTransform *t, float w, float h, float out[16])
{
    const float d2r = 3.14159265358979f / 180.0f;
    m4_identity(out);
    for (int i = 0; i < t->count && i < CSS_MAX_TRANSFORM_OPS; i++) {
        const CssTransformOp *op = &t->ops[i];
        float k[16];
        m4_identity(k);
        switch (op->kind) {
        case CSS_TRANSFORM_TRANSLATE:
            k[12] = len_res(&op->tx, w); k[13] = len_res(&op->ty, h); k[14] = op->tz;
            break;
        case CSS_TRANSFORM_SCALE: k[0] = op->sx; k[5] = op->sy; k[10] = op_sz(op); break;
        case CSS_TRANSFORM_ROTATE: {
            const float r = op->angle_deg * d2r, c = cosf(r), s = sinf(r);
            k[0] = c; k[1] = s; k[4] = -s; k[5] = c;
            break;
        }
        case CSS_TRANSFORM_SKEW:
            k[4] = tanf(op->angle_deg * d2r);
            k[1] = tanf(op->angle2_deg * d2r);
            break;
        case CSS_TRANSFORM_MATRIX:
            k[0] = op->m.a; k[1] = op->m.b; k[4] = op->m.c; k[5] = op->m.d; k[12] = op->m.e; k[13] = op->m.f;
            break;
        case CSS_TRANSFORM_ROTATE_3D: {
            /* CSS Transforms 2 §12 rotate3d() matrix (row R, column C -> k[C * 4 + R]) */
            const float x = op->axis[0], y = op->axis[1], z = op->axis[2];
            const float a = op->angle_deg * d2r, sc = sinf(a * 0.5f) * cosf(a * 0.5f), sq = sinf(a * 0.5f) * sinf(a * 0.5f);
            k[0] = 1 - 2 * (y * y + z * z) * sq;  k[4] = 2 * (x * y * sq - z * sc);    k[8] = 2 * (x * z * sq + y * sc);
            k[1] = 2 * (x * y * sq + z * sc);     k[5] = 1 - 2 * (x * x + z * z) * sq; k[9] = 2 * (y * z * sq - x * sc);
            k[2] = 2 * (x * z * sq - y * sc);     k[6] = 2 * (y * z * sq + x * sc);    k[10] = 1 - 2 * (x * x + y * y) * sq;
            break;
        }
        case CSS_TRANSFORM_PERSPECTIVE:
            if (op->persp > 0) k[11] = -1.0f / op->persp;
            break;
        default: break;
        }
        m4_mul(out, k, out);
    }
}

/* ========================================================================= */
/* Positions (transform-origin, background-position)                         */
/* ========================================================================= */

typedef enum { POS_NONE, POS_LEFT, POS_RIGHT, POS_TOP, POS_BOTTOM, POS_CENTER } PosKw;

static PosKw pos_kw(const CssToken *t)
{
    if (t->type != CSS_TOK_IDENT) return POS_NONE;
    if (tok_is_ident(t, "left")) return POS_LEFT;
    if (tok_is_ident(t, "right")) return POS_RIGHT;
    if (tok_is_ident(t, "top")) return POS_TOP;
    if (tok_is_ident(t, "bottom")) return POS_BOTTOM;
    if (tok_is_ident(t, "center")) return POS_CENTER;
    return POS_NONE;
}

static CssLength len_pct(float pct)
{
    CssLength l; memset(&l, 0, sizeof(l)); l.pct = pct; return l;
}

static int pos_is_x(PosKw k) { return k == POS_LEFT || k == POS_RIGHT || k == POS_CENTER; }
static int pos_is_y(PosKw k) { return k == POS_TOP || k == POS_BOTTOM || k == POS_CENTER; }

/* keyword [offset] -> length */
static CssLength pos_kw_len(PosKw k, const CssLength *offset)
{
    CssLength l = len_pct(k == POS_RIGHT || k == POS_BOTTOM ? 100.0f : k == POS_CENTER ? 50.0f : 0.0f);
    if (offset) {
        float sign = (k == POS_RIGHT || k == POS_BOTTOM) ? -1.0f : 1.0f;
        l.px += sign * offset->px; l.pct += sign * offset->pct; l.em += sign * offset->em;
    }
    return l;
}

/* Parses a <position> from whitespace-free tokens; sets *consumed. */
static int parse_position(const CssToken *t, int n, CssLength *x, CssLength *y, int *consumed)
{
    /* collect up to 4 components: keyword or length */
    PosKw kw[4]; CssLength len[4]; int is_len[4];
    int c = 0, i = 0;
    while (i < n && c < 4) {
        PosKw k = pos_kw(&t[i]);
        int used = 0;
        if (k != POS_NONE) { kw[c] = k; is_len[c] = 0; used = 1; }
        else if (css_parse_length_item(t + i, n - i, CSS_LENF_PCT, &len[c], &used)) { kw[c] = POS_NONE; is_len[c] = 1; }
        else break;
        c++;
        i += used;
    }
    if (c == 0) return 0;
    CssLength rx = len_pct(50), ry = len_pct(50);
    if (c == 1) {
        if (is_len[0]) rx = len[0];
        else if (pos_is_x(kw[0])) rx = pos_kw_len(kw[0], NULL);
        else ry = pos_kw_len(kw[0], NULL);
        if (kw[0] == POS_CENTER) { rx = len_pct(50); ry = len_pct(50); }
    } else if (c == 2) {
        int a_y_only = !is_len[0] && (kw[0] == POS_TOP || kw[0] == POS_BOTTOM);
        int b_x_only = !is_len[1] && (kw[1] == POS_LEFT || kw[1] == POS_RIGHT);
        int swap = a_y_only || (b_x_only && !is_len[0] && kw[0] == POS_CENTER);
        int ia = swap ? 1 : 0, ib = swap ? 0 : 1;
        if (is_len[ia]) rx = len[ia];
        else if (pos_is_x(kw[ia])) rx = pos_kw_len(kw[ia], NULL);
        else return 0;
        if (is_len[ib]) ry = len[ib];
        else if (pos_is_y(kw[ib])) ry = pos_kw_len(kw[ib], NULL);
        else return 0;
    } else {
        /* 3-4 values: keyword [offset] keyword [offset]; `center` takes
         * whichever axis the other keyword leaves free. */
        int j = 0, got_x = 0, got_y = 0, centers = 0;
        while (j < c) {
            if (is_len[j]) return 0;
            PosKw k = kw[j++];
            const CssLength *off = NULL;
            if (j < c && is_len[j]) {
                if (k == POS_CENTER) return 0;
                off = &len[j++];
            }
            if (k == POS_CENTER) centers++;
            else if (k == POS_LEFT || k == POS_RIGHT) { if (got_x) return 0; rx = pos_kw_len(k, off); got_x = 1; }
            else { if (got_y) return 0; ry = pos_kw_len(k, off); got_y = 1; }
        }
        if (got_x + got_y + centers != 2) return 0;
        if (c == 3 && !is_len[1] && !is_len[2]) return 0; /* three keywords is invalid */
    }
    *x = rx; *y = ry;
    *consumed = i;
    return 1;
}

int css_parse_position_xy(const CssToken *tokens, int n, CssLength *x, CssLength *y)
{
    CssToken t[32];
    int m = tokens_copy_nows(tokens, n, t, 32);
    int used = 0;
    CssLength rx, ry;
    if (!parse_position(t, m, &rx, &ry, &used) || used != m) return 0;
    *x = rx; *y = ry;
    return 1;
}

int css_parse_transform_origin(const CssToken *tokens, int n, CssLength *x, CssLength *y)
{
    CssToken t[32];
    int m = tokens_copy_nows(tokens, n, t, 32);
    int used = 0;
    CssLength rx, ry, z;
    if (!parse_position(t, m, &rx, &ry, &used)) return 0;
    if (used < m) { /* optional z offset */
        int u2 = 0;
        if (!css_parse_length_item(t + used, m - used, 0, &z, &u2) || used + u2 != m) return 0;
    }
    *x = rx; *y = ry;
    return 1;
}

/* ========================================================================= */
/* Shadows                                                                   */
/* ========================================================================= */

/* em in shadow lengths: CssShadow holds px, so the parser folds em with the
 * font size css_computed_apply() sets here (16 for validation-only parses). */
static float g_shadow_em_px = 16.0f;

int css_parse_shadow_item(const CssToken *tokens, int n, int allow_inset_spread, CssShadow *out, int *consumed)
{
    CssShadow s;
    memset(&s, 0, sizeof(s));
    s.color.kind = CSS_COLOR_CURRENT;
    int i = 0, got_color = 0, lengths = 0, run_closed = 0;
    float v[4] = { 0, 0, 0, 0 };
    while (i < n && tokens[i].type != CSS_TOK_COMMA) {
        int used = 0;
        CssLength l;
        if (allow_inset_spread && tok_is_ident(&tokens[i], "inset")) {
            if (s.inset) return 0;
            s.inset = 1;
            if (lengths) run_closed = 1;
            i++;
            continue;
        }
        if (css_parse_length_item(tokens + i, n - i, 0, &l, &used)) {
            if (run_closed) return 0;                  /* lengths must be contiguous */
            if (lengths >= (allow_inset_spread ? 4 : 3)) return 0;
            if (lengths == 2 && (l.px < 0 || l.em < 0)) return 0;    /* negative blur */
            v[lengths++] = l.px + l.em * g_shadow_em_px;
            i += used;
            continue;
        }
        if (css_parse_color_item(tokens + i, n - i, &s.color, &used)) {
            if (got_color) return 0;
            got_color = 1;
            if (lengths) run_closed = 1;
            i += used;
            continue;
        }
        return 0;
    }
    if (lengths < 2) return 0;
    s.x = v[0]; s.y = v[1]; s.blur = v[2]; s.spread = v[3];
    *out = s;
    *consumed = i;
    return 1;
}

int css_parse_box_shadow_list(const CssToken *tokens, int n, CssShadow *out, int max, int *count)
{
    CssToken t[CSS_MAX_TOKENS];
    int m = tokens_copy_nows(tokens, n, t, CSS_MAX_TOKENS);
    if (m == 1 && tok_is_ident(&t[0], "none")) { *count = 0; return 1; }
    int i = 0, k = 0;
    while (i < m) {
        CssShadow s; int used = 0;
        if (!css_parse_shadow_item(t + i, m - i, 1, &s, &used)) return 0;
        if (k < max) out[k++] = s;
        i += used;
        if (i < m) { if (t[i].type != CSS_TOK_COMMA) return 0; i++; if (i == m) return 0; }
    }
    if (k == 0) return 0;
    *count = k;
    return 1;
}

int css_parse_box_shadow(const CssToken *tokens, int n, CssShadow *out, int *count)
{
    CssToken t[CSS_MAX_TOKENS];
    int m = tokens_copy_nows(tokens, n, t, CSS_MAX_TOKENS);
    if (m == 1 && tok_is_ident(&t[0], "none")) { *count = 0; return 1; }
    int i = 0, k = 0;
    CssShadow first;
    while (i < m) {
        CssShadow s; int used = 0;
        if (!css_parse_shadow_item(t + i, m - i, 1, &s, &used)) return 0;
        if (k == 0) first = s;
        k++;
        i += used;
        if (i < m) { if (t[i].type != CSS_TOK_COMMA) return 0; i++; if (i == m) return 0; }
    }
    if (k == 0) return 0;
    *out = first;
    *count = 1;
    return 1;
}

int css_parse_text_shadow(const CssToken *tokens, int n, CssShadowList *out)
{
    CssToken t[CSS_MAX_TOKENS];
    int m = tokens_copy_nows(tokens, n, t, CSS_MAX_TOKENS);
    CssShadowList r;
    memset(&r, 0, sizeof(r));
    if (m == 1 && tok_is_ident(&t[0], "none")) { *out = r; return 1; }
    int i = 0;
    while (i < m) {
        CssShadow s; int used = 0;
        if (!css_parse_shadow_item(t + i, m - i, 0, &s, &used)) return 0;
        if (r.count < CSS_MAX_TEXT_SHADOWS) r.items[r.count++] = s;
        i += used;
        if (i < m) { if (t[i].type != CSS_TOK_COMMA) return 0; i++; if (i == m) return 0; }
    }
    if (r.count == 0) return 0;
    *out = r;
    return 1;
}

/* ========================================================================= */
/* Gradients and images                                                      */
/* ========================================================================= */

static int side_angle(PosKw a, PosKw b, float *angle, int legacy)
{
    /* `to <side>`: top=0 right=90 bottom=180 left=270; legacy form names the start side */
    float ax = 0, ay = 0;
    PosKw ks[2] = { a, b };
    int gx = 0, gy = 0;
    for (int i = 0; i < 2; i++) {
        if (ks[i] == POS_NONE) continue;
        if (ks[i] == POS_LEFT) { if (gx) return 0; ax = -1; gx = 1; }
        else if (ks[i] == POS_RIGHT) { if (gx) return 0; ax = 1; gx = 1; }
        else if (ks[i] == POS_TOP) { if (gy) return 0; ay = -1; gy = 1; }
        else if (ks[i] == POS_BOTTOM) { if (gy) return 0; ay = 1; gy = 1; }
        else return 0;
    }
    if (!gx && !gy) return 0;
    if (legacy) { ax = -ax; ay = -ay; }
    /* angle: 0deg = to top (0,-1), 90deg = to right (1,0) */
    *angle = (float)(atan2((double)ax, (double)-ay) * 180.0 / M_PI);
    if (*angle < 0) *angle += 360.0f;
    return 1;
}

static int parse_color_stops(const CssToken *t, int n, CssGradient *g)
{
    int s[CSS_MAX_GRADIENT_STOPS * 2 + 2], l[CSS_MAX_GRADIENT_STOPS * 2 + 2];
    int c = split_commas(t, n, s, l, CSS_MAX_GRADIENT_STOPS * 2 + 2);
    g->stop_count = 0;
    for (int i = 0; i < c; i++) {
        const CssToken *it = t + s[i];
        int len = l[i], used = 0;
        CssGradientStop st;
        memset(&st, 0, sizeof(st));
        if (len == 0) return 0;
        if (!css_parse_color_item(it, len, &st.color, &used)) {
            /* a bare position is a transition hint (CSS Images 4 §3.5.3): it bends the
             * interpolation between the stops around it (`#fbc02d, 94%, white`) */
            CssLength hint;
            if (css_parse_length_item(it, len, CSS_LENF_PCT, &hint, &used) && used == len && g->stop_count > 0) {
                g->stops[g->stop_count - 1].has_hint = 1;
                g->stops[g->stop_count - 1].hint = hint;
                continue;
            }
            return 0;
        }
        int j = used;
        int npos = 0;
        CssLength pos[2];
        while (j < len && npos < 2) {
            if (!css_parse_length_item(it + j, len - j, CSS_LENF_PCT, &pos[npos], &used)) return 0;
            npos++;
            j += used;
        }
        if (j != len) return 0;
        for (int k = 0; k < (npos ? npos : 1); k++) {
            if (g->stop_count >= CSS_MAX_GRADIENT_STOPS) return 0;
            CssGradientStop out = st;
            if (npos) { out.pos = pos[k]; out.has_pos = 1; }
            g->stops[g->stop_count++] = out;
        }
    }
    return g->stop_count >= 2;
}

int css_parse_gradient(const CssToken *tokens, int n, CssGradient *out, int *consumed)
{
    if (n < 2 || tokens[0].type != CSS_TOK_FUNCTION) return 0;
    const CssToken *f = &tokens[0];
    int len = css_component_len(tokens, n, 0);
    if (len < 2 || tokens[len - 1].type != CSS_TOK_RPAREN) return 0;
    const CssToken *body = tokens + 1;
    int bn = len - 2;
    CssGradient g;
    memset(&g, 0, sizeof(g));
    int legacy = 0, linear;
    if (tok_is_func(f, "linear-gradient")) linear = 1;
    else if (tok_is_func(f, "-webkit-linear-gradient")) { linear = 1; legacy = 1; }   /* Blink: no -moz-/-o- */
    else if (tok_is_func(f, "radial-gradient")) linear = 0;
    else if (tok_is_func(f, "-webkit-radial-gradient")) { linear = 0; legacy = 1; }
    else if (tok_is_func(f, "repeating-linear-gradient")) linear = 1;
    else if (tok_is_func(f, "repeating-radial-gradient")) linear = 0;
    else return 0;
    /* first comma-separated item may be the direction / shape */
    int first_len = 0;
    while (first_len < bn && body[first_len].type != CSS_TOK_COMMA) first_len += css_component_len(body, bn, first_len);
    int stops_start = 0;
    if (linear) {
        g.kind = CSS_GRADIENT_LINEAR;
        g.angle_deg = 180.0f;
        float ang;
        if (first_len == 1 && angle_from_token(&body[0], &ang)) { g.angle_deg = ang; stops_start = first_len + 1; }
        else if (first_len >= 2 && tok_is_ident(&body[0], "to")) {
            PosKw a = pos_kw(&body[1]), b = first_len == 3 ? pos_kw(&body[2]) : POS_NONE;
            if (first_len > 3 || !side_angle(a, b, &g.angle_deg, 0)) return 0;
            stops_start = first_len + 1;
        } else if (legacy && first_len >= 1 && pos_kw(&body[0]) != POS_NONE && pos_kw(&body[0]) != POS_CENTER) {
            PosKw a = pos_kw(&body[0]), b = first_len == 2 ? pos_kw(&body[1]) : POS_NONE;
            if (first_len > 2 || !side_angle(a, b, &g.angle_deg, 1)) return 0;
            stops_start = first_len + 1;
        }
    } else {
        g.kind = CSS_GRADIENT_RADIAL;
        g.cx = len_pct(50); g.cy = len_pct(50);
        /* [shape || extent] [at position] */
        int i = 0, is_prelude = 0;
        while (i < first_len) {
            if (tok_is_ident(&body[i], "ellipse")) { g.shape = CSS_RADIAL_ELLIPSE; i++; is_prelude = 1; }
            else if (tok_is_ident(&body[i], "circle")) { g.shape = CSS_RADIAL_CIRCLE; i++; is_prelude = 1; }
            else if (tok_is_ident(&body[i], "farthest-corner")) { g.extent = CSS_RADIAL_FARTHEST_CORNER; i++; is_prelude = 1; }
            else if (tok_is_ident(&body[i], "closest-side")) { g.extent = CSS_RADIAL_CLOSEST_SIDE; i++; is_prelude = 1; }
            else if (tok_is_ident(&body[i], "closest-corner")) { g.extent = CSS_RADIAL_CLOSEST_CORNER; i++; is_prelude = 1; }
            else if (tok_is_ident(&body[i], "farthest-side")) { g.extent = CSS_RADIAL_FARTHEST_SIDE; i++; is_prelude = 1; }
            else if (tok_is_ident(&body[i], "at")) {
                int used = 0;
                if (!parse_position(body + i + 1, first_len - i - 1, &g.cx, &g.cy, &used)) return 0;
                i += 1 + used;
                is_prelude = 1;
            } else if (legacy && !is_prelude && i == 0) {
                /* legacy: position first (`center`, `50% 50%`) */
                int used = 0;
                if (!parse_position(body, first_len, &g.cx, &g.cy, &used) || used != first_len) break;
                i = first_len;
                is_prelude = 1;
            } else break;
        }
        if (is_prelude) {
            if (i != first_len) return 0;
            stops_start = first_len + 1;
        }
    }
    if (stops_start > bn) return 0;
    if (!parse_color_stops(body + stops_start, bn - stops_start, &g)) return 0;
    *out = g;
    if (consumed) *consumed = len;
    return 1;
}

static const char *g_url_base;   /* see css_values_set_url_base() */

void css_values_set_url_base(const char *base_atom) { g_url_base = base_atom; }

int css_parse_image(const CssToken *tokens, int n, CssImage *out, int *consumed)
{
    if (n <= 0) return 0;
    CssImage img;
    memset(&img, 0, sizeof(img));
    const CssToken *t = &tokens[0];
    if (tok_is_ident(t, "none")) { img.kind = CSS_IMAGE_NONE; *out = img; if (consumed) *consumed = 1; return 1; }
    if (t->type == CSS_TOK_URL) {
        img.kind = CSS_IMAGE_URL;
        img.url = kn_atom(t->text, t->len);
        img.base = g_url_base;
        *out = img;
        if (consumed) *consumed = 1;
        return 1;
    }
    if (tok_is_func(t, "url")) {
        int len = css_component_len(tokens, n, 0);
        if (len != 3 || tokens[1].type != CSS_TOK_STRING || tokens[2].type != CSS_TOK_RPAREN) return 0;
        img.kind = CSS_IMAGE_URL;
        img.url = kn_atom(tokens[1].text, tokens[1].len);
        img.base = g_url_base;
        *out = img;
        if (consumed) *consumed = len;
        return 1;
    }
    int used = 0;
    if (css_parse_gradient(tokens, n, &img.gradient, &used)) {
        img.kind = CSS_IMAGE_GRADIENT;
        *out = img;
        if (consumed) *consumed = used;
        return 1;
    }
    return 0;
}

int css_parse_bg_image_list(const CssToken *tokens, int n, CssImage *out, int max, int *count)
{
    CssToken t[CSS_MAX_TOKENS];
    int m = tokens_copy_nows(tokens, n, t, CSS_MAX_TOKENS);
    int s[16], l[16];
    int c = split_commas(t, m, s, l, 16);
    if (c == 0) return 0;
    CssImage tmp[16];
    for (int i = 0; i < c; i++) {
        int used = 0;
        if (!css_parse_image(t + s[i], l[i], &tmp[i], &used) || used != l[i]) return 0;
    }
    int k = c < max ? c : max;
    for (int i = 0; i < k; i++) out[i] = tmp[i];
    *count = c;
    return 1;
}

/* Marks the first token of every comma-separated item as a full <position>
 * (set by shorthand expansion so the per-axis longhands parse both axes). */
static void flag_full_position(CssToken *t, int n)
{
    int at_start = 1;
    for (int i = 0; i < n; i++) {
        if (t[i].type == CSS_TOK_WHITESPACE) continue;
        if (t[i].type == CSS_TOK_COMMA) { at_start = 1; continue; }
        if (at_start) t[i].flags |= CSS_TOKF_FULL_POSITION;
        at_start = 0;
    }
}

/* Per-axis value of background-position-x/-y: `center`, `<length>`,
 * `<side>`, `<side> <offset>` (CSS Backgrounds 4).  Items flagged
 * CSS_TOKF_FULL_POSITION (from the shorthands) are full <position>s. */
static int parse_bg_axis_item(const CssToken *it, int n, int axis, CssLength *out)
{
    CssLength x, y;
    int used = 0;
    if (n <= 0) return 0;
    if (it[0].flags & CSS_TOKF_FULL_POSITION) {
        if (!parse_position(it, n, &x, &y, &used) || used != n) return 0;
        *out = axis == 0 ? x : y;
        return 1;
    }
    PosKw k = pos_kw(&it[0]);
    if (k == POS_NONE) {
        if (!css_parse_length_item(it, n, CSS_LENF_PCT, &x, &used) || used != n) return 0;
        *out = x;
        return 1;
    }
    if (!(axis == 0 ? pos_is_x(k) : pos_is_y(k))) return 0;
    if (n == 1) { *out = pos_kw_len(k, NULL); return 1; }
    if (k == POS_CENTER) return 0;
    CssLength off;
    if (!css_parse_length_item(it + 1, n - 1, CSS_LENF_PCT, &off, &used) || 1 + used != n) return 0;
    *out = pos_kw_len(k, &off);
    return 1;
}

int css_parse_bg_position_list(const CssToken *tokens, int n, int axis, CssLength *out, int max, int *count)
{
    CssToken t[CSS_MAX_TOKENS];
    int m = tokens_copy_nows(tokens, n, t, CSS_MAX_TOKENS);
    int s[16], l[16];
    int c = split_commas(t, m, s, l, 16);
    if (c == 0) return 0;
    CssLength tmp[16];
    for (int i = 0; i < c; i++)
        if (!parse_bg_axis_item(t + s[i], l[i], axis, &tmp[i])) return 0;
    int k = c < max ? c : max;
    for (int i = 0; i < k; i++) out[i] = tmp[i];
    *count = c;
    return 1;
}

int css_parse_bg_size_item(const CssToken *tokens, int n, CssBgSize *out, int *consumed)
{
    if (n <= 0) return 0;
    CssBgSize s;
    memset(&s, 0, sizeof(s));
    if (tok_is_ident(&tokens[0], "cover")) { s.kind = CSS_BG_SIZE_COVER; *out = s; *consumed = 1; return 1; }
    if (tok_is_ident(&tokens[0], "contain")) { s.kind = CSS_BG_SIZE_CONTAIN; *out = s; *consumed = 1; return 1; }
    int used = 0;
    unsigned fl = CSS_LENF_PCT | CSS_LENF_AUTO | CSS_LENF_NONNEG;
    if (!css_parse_length_item(tokens, n, fl, &s.w, &used)) return 0;
    int i = used;
    s.h.kind = CSS_LEN_AUTO;
    if (i < n && tokens[i].type != CSS_TOK_COMMA && css_parse_length_item(tokens + i, n - i, fl, &s.h, &used)) i += used;
    *out = s;
    *consumed = i;
    return 1;
}

int css_parse_bg_size_list(const CssToken *tokens, int n, CssBgSize *out, int max, int *count)
{
    CssToken t[CSS_MAX_TOKENS];
    int m = tokens_copy_nows(tokens, n, t, CSS_MAX_TOKENS);
    int s[16], l[16];
    int c = split_commas(t, m, s, l, 16);
    if (c == 0) return 0;
    CssBgSize tmp[16];
    for (int i = 0; i < c; i++) {
        int used = 0;
        if (!css_parse_bg_size_item(t + s[i], l[i], &tmp[i], &used) || used != l[i]) return 0;
    }
    int k = c < max ? c : max;
    for (int i = 0; i < k; i++) out[i] = tmp[i];
    *count = c;
    return 1;
}

int css_parse_bg_repeat_item(const CssToken *tokens, int n, uint8_t *rx, uint8_t *ry, int *consumed)
{
    if (n <= 0) return 0;
    int a, b;
    if (tok_is_ident(&tokens[0], "repeat-x")) { *rx = CSS_BG_REPEAT_REPEAT; *ry = CSS_BG_REPEAT_NO_REPEAT; *consumed = 1; return 1; }
    if (tok_is_ident(&tokens[0], "repeat-y")) { *rx = CSS_BG_REPEAT_NO_REPEAT; *ry = CSS_BG_REPEAT_REPEAT; *consumed = 1; return 1; }
    if (!css_keyword_lookup(&tokens[0], kw_bg_repeat, &a)) return 0;
    b = a;
    int i = 1;
    if (n > 1 && css_keyword_lookup(&tokens[1], kw_bg_repeat, &b)) i = 2;
    *rx = (uint8_t)a; *ry = (uint8_t)b; *consumed = i;
    return 1;
}

int css_parse_bg_repeat_list(const CssToken *tokens, int n, uint8_t *rx, uint8_t *ry, int max, int *count)
{
    CssToken t[CSS_MAX_TOKENS];
    int m = tokens_copy_nows(tokens, n, t, CSS_MAX_TOKENS);
    int s[16], l[16];
    int c = split_commas(t, m, s, l, 16);
    if (c == 0) return 0;
    uint8_t tx[16], ty[16];
    for (int i = 0; i < c; i++) {
        int used = 0;
        if (!css_parse_bg_repeat_item(t + s[i], l[i], &tx[i], &ty[i], &used) || used != l[i]) return 0;
    }
    int k = c < max ? c : max;
    for (int i = 0; i < k; i++) { rx[i] = tx[i]; ry[i] = ty[i]; }
    *count = c;
    return 1;
}

/* ========================================================================= */
/* Filters                                                                   */
/* ========================================================================= */

static int parse_amount(const CssToken *t, int n, float *out)
{
    if (n == 1 && t[0].type == CSS_TOK_PERCENTAGE) { *out = (float)(t[0].num / 100.0); return 1; }
    return css_parse_number(t, n, out);
}

int css_parse_filter(const CssToken *tokens, int n, CssFilterOp *out, int max, int *count)
{
    CssToken t[CSS_MAX_TOKENS];
    int m = tokens_copy_nows(tokens, n, t, CSS_MAX_TOKENS);
    if (m == 1 && tok_is_ident(&t[0], "none")) { *count = 0; return 1; }
    CssFilterOp tmp[CSS_MAX_FILTER_OPS * 2];
    int k = 0, i = 0;
    while (i < m) {
        if (t[i].type != CSS_TOK_FUNCTION) return 0;
        int len = css_component_len(t, m, i);
        if (len < 2 || t[i + len - 1].type != CSS_TOK_RPAREN) return 0;
        const CssToken *f = &t[i];
        const CssToken *body = t + i + 1;
        int bn = len - 2;
        CssFilterOp op;
        memset(&op, 0, sizeof(op));
        if (tok_is_func(f, "grayscale")) { op.kind = CSS_FILTER_GRAYSCALE; op.amount = 1; if (bn && !parse_amount(body, bn, &op.amount)) return 0; }
        else if (tok_is_func(f, "brightness")) { op.kind = CSS_FILTER_BRIGHTNESS; op.amount = 1; if (bn && !parse_amount(body, bn, &op.amount)) return 0; }
        else if (tok_is_func(f, "contrast")) { op.kind = CSS_FILTER_CONTRAST; op.amount = 1; if (bn && !parse_amount(body, bn, &op.amount)) return 0; }
        else if (tok_is_func(f, "saturate")) { op.kind = CSS_FILTER_SATURATE; op.amount = 1; if (bn && !parse_amount(body, bn, &op.amount)) return 0; }
        else if (tok_is_func(f, "opacity")) { op.kind = CSS_FILTER_OPACITY; op.amount = 1; if (bn && !parse_amount(body, bn, &op.amount)) return 0; }
        else if (tok_is_func(f, "invert")) { op.kind = CSS_FILTER_INVERT; op.amount = 1; if (bn && !parse_amount(body, bn, &op.amount)) return 0; }
        else if (tok_is_func(f, "sepia")) { op.kind = CSS_FILTER_SEPIA; op.amount = 1; if (bn && !parse_amount(body, bn, &op.amount)) return 0; }
        else if (tok_is_func(f, "hue-rotate")) { op.kind = CSS_FILTER_HUE_ROTATE; if (bn && !css_parse_angle_deg(body, bn, &op.amount)) return 0; }
        else if (tok_is_func(f, "blur")) {
            op.kind = CSS_FILTER_BLUR;
            CssLength l; memset(&l, 0, sizeof(l));
            if (bn && !css_parse_length(body, bn, CSS_LENF_NONNEG, &l)) return 0;
            op.amount = l.px;
        } else if (tok_is_func(f, "drop-shadow")) {
            op.kind = CSS_FILTER_DROP_SHADOW;
            int used = 0;
            if (!css_parse_shadow_item(body, bn, 0, &op.shadow, &used) || used != bn) return 0;
        } else return 0;
        if (op.amount < 0 && op.kind != CSS_FILTER_HUE_ROTATE) return 0;
        if (k >= (int)(sizeof(tmp) / sizeof(tmp[0]))) return 0;
        tmp[k++] = op;
        i += len;
    }
    if (k == 0) return 0;
    int w = k < max ? k : max;
    for (int j = 0; j < w; j++) out[j] = tmp[j];
    *count = w;
    return 1;
}

/* ========================================================================= */
/* clip-path                                                                 */
/* ========================================================================= */

int css_parse_clip_path(const CssToken *tokens, int n, CssClipPath *out)
{
    CssToken t[CSS_MAX_TOKENS];
    int m = tokens_copy_nows(tokens, n, t, CSS_MAX_TOKENS);
    CssClipPath r;
    memset(&r, 0, sizeof(r));
    if (m == 1 && tok_is_ident(&t[0], "none")) { *out = r; return 1; }
    if (m < 2 || !tok_is_func(&t[0], "polygon")) return 0;
    int len = css_component_len(t, m, 0);
    if (len != m || t[m - 1].type != CSS_TOK_RPAREN) return 0;
    int s[CSS_MAX_CLIP_POINTS + 2], l[CSS_MAX_CLIP_POINTS + 2];
    int c = split_commas(t + 1, m - 2, s, l, CSS_MAX_CLIP_POINTS + 2);
    int first = 0;
    if (c > 0 && l[0] == 1 && (tok_is_ident(&t[1 + s[0]], "nonzero") || tok_is_ident(&t[1 + s[0]], "evenodd"))) {
        first = 1;
        r.evenodd = (uint8_t) tok_is_ident(&t[1 + s[0]], "evenodd");
    }
    if (c - first < 3 || c - first > CSS_MAX_CLIP_POINTS) return 0;
    for (int i = first; i < c; i++) {
        const CssToken *it = t + 1 + s[i];
        int used = 0, j = 0;
        CssLength x, y;
        if (!css_parse_length_item(it, l[i], CSS_LENF_PCT, &x, &used)) return 0;
        j += used;
        if (!css_parse_length_item(it + j, l[i] - j, CSS_LENF_PCT, &y, &used)) return 0;
        j += used;
        if (j != l[i]) return 0;
        r.x[r.point_count] = x; r.y[r.point_count] = y; r.point_count++;
    }
    r.kind = CSS_CLIP_PATH_POLYGON;
    *out = r;
    return 1;
}

/* ========================================================================= */
/* Timing functions                                                          */
/* ========================================================================= */

static void timing_preset(CssTiming *t, int kind)
{
    memset(t, 0, sizeof(*t));
    t->kind = (uint8_t)kind;
    switch (kind) {
    case CSS_TIMING_EASE: t->x1 = 0.25f; t->y1 = 0.1f; t->x2 = 0.25f; t->y2 = 1.0f; break;
    case CSS_TIMING_LINEAR: t->x1 = 0; t->y1 = 0; t->x2 = 1; t->y2 = 1; break;
    case CSS_TIMING_EASE_IN: t->x1 = 0.42f; t->y1 = 0; t->x2 = 1; t->y2 = 1; break;
    case CSS_TIMING_EASE_OUT: t->x1 = 0; t->y1 = 0; t->x2 = 0.58f; t->y2 = 1; break;
    case CSS_TIMING_EASE_IN_OUT: t->x1 = 0.42f; t->y1 = 0; t->x2 = 0.58f; t->y2 = 1; break;
    default: break;
    }
}

int css_parse_timing(const CssToken *tokens, int n, CssTiming *out, int *consumed)
{
    if (n <= 0) return 0;
    const CssToken *t = &tokens[0];
    CssTiming r;
    if (t->type == CSS_TOK_IDENT) {
        int kind;
        if (!css_keyword_lookup(t, css_kw_timing, &kind)) return 0;
        timing_preset(&r, kind);
        if (tok_is_ident(t, "step-start")) { r.steps = 1; r.step_pos = CSS_STEP_START; }
        if (tok_is_ident(t, "step-end")) { r.steps = 1; r.step_pos = CSS_STEP_END; }
        *out = r;
        *consumed = 1;
        return 1;
    }
    if (t->type != CSS_TOK_FUNCTION) return 0;
    int len = css_component_len(tokens, n, 0);
    if (len < 2 || tokens[len - 1].type != CSS_TOK_RPAREN) return 0;
    const CssToken *as[4]; int al[4];
    int argc = func_args(tokens + 1, len - 2, as, al, 4);
    if (tok_is_func(t, "cubic-bezier")) {
        if (argc != 4) return 0;
        timing_preset(&r, CSS_TIMING_CUBIC_BEZIER);
        float *dst[4] = { &r.x1, &r.y1, &r.x2, &r.y2 };
        for (int i = 0; i < 4; i++) if (!css_parse_number(as[i], al[i], dst[i])) return 0;
        if (r.x1 < 0 || r.x1 > 1 || r.x2 < 0 || r.x2 > 1) return 0;
    } else if (tok_is_func(t, "steps")) {
        if (argc < 1 || argc > 2) return 0;
        timing_preset(&r, CSS_TIMING_STEPS);
        if (!css_parse_integer(as[0], al[0], &r.steps) || r.steps <= 0) return 0;
        r.step_pos = CSS_STEP_END;
        if (argc == 2) {
            if (al[1] != 1) return 0;
            if (tok_is_ident(as[1], "start") || tok_is_ident(as[1], "jump-start")) r.step_pos = CSS_STEP_START;
            else if (tok_is_ident(as[1], "end") || tok_is_ident(as[1], "jump-end")) r.step_pos = CSS_STEP_END;
            else if (tok_is_ident(as[1], "jump-none")) r.step_pos = CSS_STEP_JUMP_NONE;
            else if (tok_is_ident(as[1], "jump-both")) r.step_pos = CSS_STEP_JUMP_BOTH;
            else return 0;
        }
    } else return 0;
    *out = r;
    *consumed = len;
    return 1;
}

/* ========================================================================= */
/* animation / transition items                                              */
/* ========================================================================= */

/* Per-component slice record used by shorthand expansion. */
typedef struct AnimParts {
    int name_s, name_l, dur_s, dur_l, timing_s, timing_l, delay_s, delay_l;
    int iter_s, iter_l, dir_s, dir_l, fill_s, fill_l, play_s, play_l;
} AnimParts;

static int parse_animation_item_parts(const CssToken *t, int n, CssAnimation *out, AnimParts *parts, int *consumed)
{
    CssAnimation a;
    memset(&a, 0, sizeof(a));
    timing_preset(&a.timing, CSS_TIMING_EASE);
    a.iterations = 1;
    AnimParts p;
    memset(&p, -1, sizeof(p));
    int times = 0, got_timing = 0, got_iter = 0, got_dir = 0, got_fill = 0, got_play = 0, got_name = 0;
    int i = 0;
    while (i < n && t[i].type != CSS_TOK_COMMA) {
        const CssToken *k = &t[i];
        float f; int used = 0, kw;
        if (time_from_tokens(t + i, n - i, &used, &f)) {
            if (times == 0) { a.duration_s = f; p.dur_s = i; p.dur_l = used; }
            else if (times == 1) { a.delay_s = f; p.delay_s = i; p.delay_l = used; }
            else return 0;
            times++; i += used; continue;
        }
        if (!got_timing && css_parse_timing(t + i, n - i, &a.timing, &used)) { got_timing = 1; p.timing_s = i; p.timing_l = used; i += used; continue; }
        if (!got_iter && (k->type == CSS_TOK_NUMBER || tok_is_ident(k, "infinite"))) {
            if (k->type == CSS_TOK_NUMBER) { if (k->num < 0) return 0; a.iterations = (float)k->num; }
            else a.iterations = INFINITY;
            got_iter = 1; p.iter_s = i; p.iter_l = 1; i++; continue;
        }
        if (!got_dir && css_keyword_lookup(k, kw_anim_direction, &kw)) { a.direction = (uint8_t)kw; got_dir = 1; p.dir_s = i; p.dir_l = 1; i++; continue; }
        if (!got_fill && css_keyword_lookup(k, kw_anim_fill, &kw)) { a.fill = (uint8_t)kw; got_fill = 1; p.fill_s = i; p.fill_l = 1; i++; continue; }
        if (!got_play && css_keyword_lookup(k, kw_anim_play, &kw)) { a.play_state = (uint8_t)kw; got_play = 1; p.play_s = i; p.play_l = 1; i++; continue; }
        if (!got_name && (k->type == CSS_TOK_IDENT || k->type == CSS_TOK_STRING)) {
            if (tok_is_global_kw(k)) return 0;
            a.name = tok_is_ident(k, "none") ? NULL : kn_atom(k->text, k->len);
            got_name = 1; p.name_s = i; p.name_l = 1; i++; continue;
        }
        return 0;
    }
    if (i == 0) return 0;
    *out = a;
    if (parts) *parts = p;
    *consumed = i;
    return 1;
}

int css_parse_animation_item(const CssToken *tokens, int n, CssAnimation *out, int *consumed)
{
    return parse_animation_item_parts(tokens, n, out, NULL, consumed);
}

typedef struct TransParts { int prop_s, prop_l, dur_s, dur_l, timing_s, timing_l, delay_s, delay_l; } TransParts;

static int parse_transition_item_parts(const CssToken *t, int n, CssTransition *out, TransParts *parts, int *consumed)
{
    CssTransition tr;
    memset(&tr, 0, sizeof(tr));
    tr.prop = CSS_PROP_ALL;
    timing_preset(&tr.timing, CSS_TIMING_EASE);
    TransParts p;
    memset(&p, -1, sizeof(p));
    int times = 0, got_timing = 0, got_prop = 0, i = 0;
    while (i < n && t[i].type != CSS_TOK_COMMA) {
        const CssToken *k = &t[i];
        float f; int used = 0;
        if (time_from_tokens(t + i, n - i, &used, &f)) {
            if (times == 0) { tr.duration_s = f; p.dur_s = i; p.dur_l = used; }
            else if (times == 1) { tr.delay_s = f; p.delay_s = i; p.delay_l = used; }
            else return 0;
            times++; i += used; continue;
        }
        if (!got_timing && css_parse_timing(t + i, n - i, &tr.timing, &used)) { got_timing = 1; p.timing_s = i; p.timing_l = used; i += used; continue; }
        if (!got_prop && k->type == CSS_TOK_IDENT) {
            if (tok_is_global_kw(k)) return 0;
            if (tok_is_ident(k, "all")) tr.prop = CSS_PROP_ALL;
            else if (tok_is_ident(k, "none")) tr.prop = -1;
            else {
                int id = css_prop_lookup(k->text, k->len);
                tr.prop = (int16_t)(id == CSS_PROP_INVALID ? -2 /* unknown: never matches */ : id);
            }
            got_prop = 1; p.prop_s = i; p.prop_l = 1; i++; continue;
        }
        return 0;
    }
    if (i == 0) return 0;
    *out = tr;
    if (parts) *parts = p;
    *consumed = i;
    return 1;
}

int css_parse_transition_item(const CssToken *tokens, int n, CssTransition *out, int *consumed)
{
    return parse_transition_item_parts(tokens, n, out, NULL, consumed);
}

/* comma lists for the longhands ------------------------------------------ */

#define LIST_PROLOGUE \
    CssToken t[CSS_MAX_TOKENS]; \
    int m = tokens_copy_nows(tokens, n, t, CSS_MAX_TOKENS); \
    int s[16], l[16]; \
    int c = split_commas(t, m, s, l, 16); \
    if (c == 0) return 0;

int css_parse_animation_names(const CssToken *tokens, int n, const char **out, int max, int *count)
{
    LIST_PROLOGUE
    const char *tmp[16];
    for (int i = 0; i < c; i++) {
        if (l[i] != 1 || (t[s[i]].type != CSS_TOK_IDENT && t[s[i]].type != CSS_TOK_STRING) || tok_is_global_kw(&t[s[i]])) return 0;
        tmp[i] = tok_is_ident(&t[s[i]], "none") ? NULL : kn_atom(t[s[i]].text, t[s[i]].len);
    }
    int k = c < max ? c : max;
    for (int i = 0; i < k; i++) out[i] = tmp[i];
    *count = c;
    return 1;
}

int css_parse_time_list(const CssToken *tokens, int n, float *out, int max, int *count)
{
    LIST_PROLOGUE
    float tmp[16];
    for (int i = 0; i < c; i++) {
        int used = 0;
        if (!time_from_tokens(&t[s[i]], l[i], &used, &tmp[i]) || used != l[i]) return 0;
    }
    int k = c < max ? c : max;
    for (int i = 0; i < k; i++) out[i] = tmp[i];
    *count = c;
    return 1;
}

int css_parse_timing_list(const CssToken *tokens, int n, CssTiming *out, int max, int *count)
{
    LIST_PROLOGUE
    CssTiming tmp[16];
    for (int i = 0; i < c; i++) { int used = 0; if (!css_parse_timing(t + s[i], l[i], &tmp[i], &used) || used != l[i]) return 0; }
    int k = c < max ? c : max;
    for (int i = 0; i < k; i++) out[i] = tmp[i];
    *count = c;
    return 1;
}

int css_parse_iteration_list(const CssToken *tokens, int n, float *out, int max, int *count)
{
    LIST_PROLOGUE
    float tmp[16];
    for (int i = 0; i < c; i++) {
        if (l[i] != 1) return 0;
        if (tok_is_ident(&t[s[i]], "infinite")) tmp[i] = INFINITY;
        else if (t[s[i]].type == CSS_TOK_NUMBER && t[s[i]].num >= 0) tmp[i] = (float)t[s[i]].num;
        else return 0;
    }
    int k = c < max ? c : max;
    for (int i = 0; i < k; i++) out[i] = tmp[i];
    *count = c;
    return 1;
}

int css_parse_keyword_list(const CssToken *tokens, int n, const CssKeyword *table, uint8_t *out, int max, int *count)
{
    LIST_PROLOGUE
    uint8_t tmp[16];
    for (int i = 0; i < c; i++) { int v; if (l[i] != 1 || !css_keyword_lookup(&t[s[i]], table, &v)) return 0; tmp[i] = (uint8_t)v; }
    int k = c < max ? c : max;
    for (int i = 0; i < k; i++) out[i] = tmp[i];
    *count = c;
    return 1;
}

int css_parse_transition_props(const CssToken *tokens, int n, int16_t *out, int max, int *count)
{
    LIST_PROLOGUE
    int16_t tmp[16];
    for (int i = 0; i < c; i++) {
        const CssToken *k = &t[s[i]];
        if (l[i] != 1 || k->type != CSS_TOK_IDENT || tok_is_global_kw(k)) return 0;
        if (tok_is_ident(k, "all")) tmp[i] = CSS_PROP_ALL;
        else if (tok_is_ident(k, "none")) tmp[i] = -1;
        else { int id = css_prop_lookup(k->text, k->len); tmp[i] = (int16_t)(id == CSS_PROP_INVALID ? -2 : id); }
    }
    int k = c < max ? c : max;
    for (int i = 0; i < k; i++) out[i] = tmp[i];
    *count = c;
    return 1;
}

/* ========================================================================= */
/* Grid, border width, fonts, misc                                           */
/* ========================================================================= */

/* One <track-size> at t[0..]: <length-percentage> | auto | min-content |
 * max-content | <flex> | fit-content(<length-percentage>) |
 * minmax(<inflexible-breadth>, <track-breadth>).  em stays unresolved. */
static int parse_track_size(const CssToken *t, int n, CssTrack *out, int *used)
{
    const unsigned fl = CSS_LENF_PCT | CSS_LENF_AUTO | CSS_LENF_CONTENT | CSS_LENF_NONNEG;
    memset(out, 0, sizeof(*out));
    if (n <= 0) return 0;
    if (t[0].type == CSS_TOK_DIMENSION && t[0].unit == CSS_UNIT_FR) {
        if (t[0].num < 0) return 0;
        out->kind = CSS_TRACK_FR; out->fr = (float)t[0].num; out->size.kind = CSS_LEN_AUTO;
        *used = 1;
        return 1;
    }
    if (t[0].type == CSS_TOK_FUNCTION && (tok_is_func(&t[0], "fit-content") || tok_is_func(&t[0], "minmax"))) {
        int len = css_component_len(t, n, 0), u = 0;
        const CssToken *a = t + 1;
        int an = len - 2;                      /* between the function token and ')' */
        if (an <= 0 || t[len - 1].type != CSS_TOK_RPAREN) return 0;
        if (tok_is_func(&t[0], "fit-content")) {
            if (!css_parse_length_item(a, an, CSS_LENF_PCT | CSS_LENF_NONNEG, &out->size, &u) || u != an) return 0;
            out->kind = CSS_TRACK_FIT_CONTENT;
        } else {
            if (!css_parse_length_item(a, an, fl, &out->size, &u) || u >= an || a[u].type != CSS_TOK_COMMA) return 0;
            const CssToken *b = a + u + 1;
            int bn = an - u - 1, u2 = 0;
            if (bn == 1 && b[0].type == CSS_TOK_DIMENSION && b[0].unit == CSS_UNIT_FR && b[0].num >= 0) {
                out->kind = CSS_TRACK_MINMAX_FR; out->fr = (float)b[0].num;
            } else if (css_parse_length_item(b, bn, fl, &out->max, &u2) && u2 == bn) {
                out->kind = CSS_TRACK_MINMAX;
            } else return 0;
        }
        *used = len;
        return 1;
    }
    out->kind = CSS_TRACK_SIZE;
    return css_parse_length_item(t, n, fl, &out->size, used);
}

/* Joins two space-separated line-name atoms (either may be NULL). */
static const char *grid_names_join(const char *a, const char *b)
{
    if (!a) return b;
    if (!b) return a;
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "%s %s", a, b);
    if (n < 0 || n >= (int)sizeof(buf)) return a;
    return kn_atom(buf, (size_t)n);
}

/* <custom-ident> usable as a grid line / area name (not a grid keyword). */
static int grid_ident_ok(const CssToken *t)
{
    return t->type == CSS_TOK_IDENT && !tok_is_ident(t, "span") && !tok_is_ident(t, "auto") && !tok_is_global_kw(t)
        && !tok_is_ident(t, "default");
}

/* <line-names>: '[' <custom-ident>* ']' at t[0]; *out = space-separated atom or NULL. */
static int parse_line_names(const CssToken *t, int n, const char **out, int *used)
{
    int len = css_component_len(t, n, 0);
    if (t[0].type != CSS_TOK_LBRACKET || len < 2 || t[len - 1].type != CSS_TOK_RBRACKET) return 0;
    char buf[256];
    size_t w = 0;
    for (int i = 1; i < len - 1; i++) {
        if (!grid_ident_ok(&t[i])) return 0;
        if (w + t[i].len + 2 >= sizeof(buf)) return 0;
        if (w) buf[w++] = ' ';
        memcpy(buf + w, t[i].text, t[i].len);
        w += t[i].len;
    }
    *out = w ? kn_atom(buf, w) : NULL;
    *used = len;
    return 1;
}

/* A <fixed-size> track (auto-repeat requires every track to be one). */
static int track_is_fixed(const CssTrack *tr)
{
    int min_fixed = tr->size.kind == CSS_LEN_VALUE;
    switch (tr->kind) {
    case CSS_TRACK_SIZE: return min_fixed;
    case CSS_TRACK_MINMAX: return min_fixed || tr->max.kind == CSS_LEN_VALUE;
    case CSS_TRACK_MINMAX_FR: return min_fixed;
    default: return 0;
    }
}

/* The inside of repeat(): [<line-names>? <track-size>]+ <line-names>?.
 * tracks[] / inner_names[0..count] receive the group. */
static int parse_repeat_group(const CssToken *t, int n, CssTrack *tracks, const char **inner_names, int max, int *count)
{
    int k = 0, i = 0, used = 0;
    inner_names[0] = NULL;
    while (i < n) {
        if (t[i].type == CSS_TOK_LBRACKET) {
            const char *nm;
            if (!parse_line_names(t + i, n - i, &nm, &used)) return 0;
            inner_names[k] = grid_names_join(inner_names[k], nm);
            i += used;
            continue;
        }
        if (k >= max || !parse_track_size(t + i, n - i, &tracks[k], &used)) return 0;
        k++;
        inner_names[k] = NULL;
        i += used;
    }
    if (k == 0) return 0;
    *count = k;
    return 1;
}

/* <track-list> | <auto-track-list> (whitespace stripped) with line names.
 * names may be NULL: names are then accepted and dropped and an auto repeat
 * is rejected. */
static int parse_grid_track_list(const CssToken *t, int m, CssTrack *out, int max, int *count, CssGridNames *names)
{
    CssTrack tmp[32];
    const char *ln[33];
    memset(ln, 0, sizeof(ln));
    CssGridNames nm;
    memset(&nm, 0, sizeof(nm));
    int k = 0, i = 0, used = 0;
    while (i < m) {
        if (t[i].type == CSS_TOK_LBRACKET) {
            const char *s;
            if (!parse_line_names(t + i, m - i, &s, &used)) return 0;
            ln[k] = grid_names_join(ln[k], s);
            i += used;
            continue;
        }
        if (k >= 32) return 0;
        if (t[i].type == CSS_TOK_FUNCTION && tok_is_func(&t[i], "repeat")) {
            /* repeat(<integer> | auto-fill | auto-fit, <group>) */
            int len = css_component_len(t + i, m - i, 0);
            int j = i + 1, end = i + len - 1;
            if (len < 5 || t[end].type != CSS_TOK_RPAREN || t[j + 1].type != CSS_TOK_COMMA) return 0;
            int reps = 0, autorep = CSS_GRID_REPEAT_NONE;
            if (t[j].type == CSS_TOK_NUMBER && (t[j].flags & CSS_TOKF_INTEGER) && t[j].num >= 1 && t[j].num <= 1000) reps = (int)t[j].num;
            else if (tok_is_ident(&t[j], "auto-fill")) autorep = CSS_GRID_REPEAT_AUTO_FILL;
            else if (tok_is_ident(&t[j], "auto-fit")) autorep = CSS_GRID_REPEAT_AUTO_FIT;
            else return 0;
            CssTrack grp[32];
            const char *gn[33];
            int per = 0;
            if (!parse_repeat_group(t + j + 2, end - j - 2, grp, gn, 32, &per)) return 0;
            if (autorep) {
                if (!names || nm.repeat) return 0;
                if (k + per > 32) return 0;
                nm.repeat = (uint8_t)autorep; nm.repeat_at = (uint8_t)k; nm.repeat_n = (uint8_t)per;
                nm.rep_first = gn[0]; nm.rep_last = gn[per];
                for (int q = 0; q < per; q++) {
                    tmp[k + q] = grp[q];
                    if (q) ln[k + q] = gn[q];
                }
                k += per;
            } else {
                for (int r = 0; r < reps; r++) {
                    ln[k] = grid_names_join(ln[k], gn[0]);
                    for (int q = 0; q < per; q++) {
                        if (k >= 32) return 0;
                        tmp[k++] = grp[q];
                        ln[k] = gn[q + 1];
                    }
                }
            }
            i += len;
            continue;
        }
        if (!parse_track_size(t + i, m - i, &tmp[k], &used)) return 0;
        k++;
        i += used;
    }
    if (k == 0 || k > max) return 0;
    if (nm.repeat)
        for (int j = 0; j < k; j++) if (!track_is_fixed(&tmp[j])) return 0;   /* <auto-track-list> */
    for (int j = 0; j < k; j++) out[j] = tmp[j];
    for (int j = 0; j <= k; j++) nm.line[j] = ln[j];
    *count = k;
    if (names) *names = nm;
    return 1;
}

int css_parse_grid_track_list(const CssToken *tokens, int n, CssTrack *out, int max, int *count, CssGridNames *names)
{
    CssToken t[CSS_MAX_TOKENS];
    int m = tokens_copy_nows(tokens, n, t, CSS_MAX_TOKENS);
    if (m == 1 && tok_is_ident(&t[0], "none")) {
        *count = 0;
        if (names) memset(names, 0, sizeof(*names));
        return 1;
    }
    return parse_grid_track_list(t, m, out, max, count, names);
}

int css_parse_grid_tracks(const CssToken *tokens, int n, CssTrack *out, int max, int *count)
{
    return css_parse_grid_track_list(tokens, n, out, max, count, NULL);
}

/* grid-auto-rows / grid-auto-columns: <track-size>+ (no names, no repeat). */
static int parse_grid_auto_tracks(const CssToken *t, int m, CssTrack *out, int *count)
{
    int k = 0, i = 0, used = 0;
    while (i < m) {
        if (k >= CSS_MAX_GRID_AUTO_TRACKS || !parse_track_size(t + i, m - i, &out[k], &used)) return 0;
        k++;
        i += used;
    }
    if (!k) return 0;
    *count = k;
    return 1;
}

/* grid-auto-flow: [row | column] || dense */
static int parse_grid_auto_flow(const CssToken *t, int m, uint8_t *out)
{
    int dir = -1, dense = 0;
    for (int i = 0; i < m; i++) {
        if (dir < 0 && tok_is_ident(&t[i], "row")) dir = 0;
        else if (dir < 0 && tok_is_ident(&t[i], "column")) dir = CSS_GRID_FLOW_COLUMN;
        else if (!dense && tok_is_ident(&t[i], "dense")) dense = CSS_GRID_FLOW_DENSE;
        else return 0;
    }
    if (m < 1) return 0;
    *out = (uint8_t)((dir < 0 ? 0 : dir) | dense);
    return 1;
}

static int is_area_name_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c >= 0x80;
}

/* grid-template-areas: none | <string>+.  Every named area must be a filled
 * rectangle.  *out = atom of the normalized cells (see CssComputed). */
static int parse_grid_areas(const CssToken *t, int m, const char **out, uint8_t *rows, uint8_t *cols)
{
    if (m == 1 && tok_is_ident(&t[0], "none")) { *out = NULL; *rows = *cols = 0; return 1; }
    enum { MAXR = 32, MAXC = 32 };
    const char *cell[MAXR][MAXC];
    int nr = 0, nc = -1;
    for (int r = 0; r < m; r++) {
        if (t[r].type != CSS_TOK_STRING || nr >= MAXR) return 0;
        const char *s = t[r].text;
        size_t len = t[r].len, p = 0;
        int c = 0;
        while (p < len) {
            unsigned char ch = (unsigned char)s[p];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f') { p++; continue; }
            size_t q = p;
            if (ch == '.') while (q < len && s[q] == '.') q++;
            else if (is_area_name_char(ch)) while (q < len && is_area_name_char((unsigned char)s[q])) q++;
            else return 0;                                  /* trash token */
            if (c >= MAXC) return 0;
            cell[nr][c++] = ch == '.' ? NULL : kn_atom(s + p, q - p);
            p = q;
        }
        if (c == 0 || (nc >= 0 && c != nc)) return 0;
        nc = c;
        nr++;
    }
    if (nr == 0) return 0;
    /* rectangles: each name's cells fill its bounding box */
    for (int r = 0; r < nr; r++)
        for (int c = 0; c < nc; c++) {
            const char *a = cell[r][c];
            if (!a) continue;
            int r0 = nr, r1 = -1, c0 = nc, c1 = -1, n = 0;
            for (int y = 0; y < nr; y++)
                for (int x = 0; x < nc; x++)
                    if (cell[y][x] == a) { n++; if (y < r0) r0 = y; if (y > r1) r1 = y; if (x < c0) c0 = x; if (x > c1) c1 = x; }
            if (n != (r1 - r0 + 1) * (c1 - c0 + 1)) return 0;
        }
    char buf[2048];
    size_t w = 0;
    for (int r = 0; r < nr; r++)
        for (int c = 0; c < nc; c++) {
            const char *a = cell[r][c] ? cell[r][c] : ".";
            size_t l = strlen(a);
            if (w + l + 2 >= sizeof(buf)) return 0;
            if (c) buf[w++] = ' ';
            else if (r) buf[w++] = '\n';
            memcpy(buf + w, a, l);
            w += l;
        }
    *out = kn_atom(buf, w);
    *rows = (uint8_t)nr;
    *cols = (uint8_t)nc;
    return 1;
}

/* <grid-line>: auto | <custom-ident> | [<integer> && <custom-ident>?] |
 * [span && [<integer> || <custom-ident>]] (whitespace stripped).  Line 0 and
 * span < 1 are invalid. */
static int parse_grid_line(const CssToken *t, int m, CssGridLine *out)
{
    memset(out, 0, sizeof(*out));
    if (m == 1 && tok_is_ident(&t[0], "auto")) return 1;
    int span = 0, num = -1, name = -1;
    for (int i = 0; i < m; i++) {
        if (!span && tok_is_ident(&t[i], "span")) {
            if (i != 0 && i != m - 1) return 0;           /* span first or last */
            span = 1;
        }
        else if (num < 0 && t[i].type == CSS_TOK_NUMBER && (t[i].flags & CSS_TOKF_INTEGER)) num = i;
        else if (name < 0 && grid_ident_ok(&t[i])) name = i;
        else return 0;
    }
    if (num < 0 && name < 0) return 0;
    if (num >= 0 && (t[num].num == 0 || fabs(t[num].num) > 10000 || (span && t[num].num < 1))) return 0;
    out->line = (int16_t)(num >= 0 ? t[num].num : span ? 1 : 0);
    out->span = (uint8_t)span;
    out->name = name >= 0 ? kn_atom(t[name].text, t[name].len) : NULL;
    return 1;
}

/* Split grid-row / grid-column / grid-area values on '/' into at most `max`
 * parts; each part must be a valid <grid-line>. Returns the part count or 0. */
static int split_grid_lines(const CssToken *t, int m, int max, int *s, int *l)
{
    int c = 0, start = 0;
    for (int i = 0; i <= m; i++) {
        if (i < m && !tok_is_delim(&t[i], '/')) continue;
        if (c >= max) return 0;
        CssGridLine g;
        if (!parse_grid_line(t + start, i - start, &g)) return 0;
        s[c] = start; l[c] = i - start; c++;
        start = i + 1;
    }
    return c;
}

int css_parse_border_width(const CssToken *tokens, int n, CssLength *out, int *consumed)
{
    if (n <= 0) return 0;
    CssLength l;
    memset(&l, 0, sizeof(l));
    if (tok_is_ident(&tokens[0], "thin")) { l.px = 1; *out = l; *consumed = 1; return 1; }
    if (tok_is_ident(&tokens[0], "medium")) { l.px = 3; *out = l; *consumed = 1; return 1; }
    if (tok_is_ident(&tokens[0], "thick")) { l.px = 5; *out = l; *consumed = 1; return 1; }
    return css_parse_length_item(tokens, n, CSS_LENF_NONNEG, out, consumed);
}

int css_parse_font_family(const CssToken *tokens, int n, const char **families, int max, int *count)
{
    CssToken t[CSS_MAX_TOKENS];
    int m = tokens_copy_nows(tokens, n, t, CSS_MAX_TOKENS);
    int s[16], l[16];
    int c = split_commas(t, m, s, l, 16);
    if (c == 0) return 0;
    const char *tmp[16];
    char buf[256];
    for (int i = 0; i < c; i++) {
        if (l[i] == 0) return 0;
        const CssToken *it = t + s[i];
        if (l[i] == 1 && it[0].type == CSS_TOK_STRING) {
            tmp[i] = kn_atom_lower(it[0].text, it[0].len);
            continue;
        }
        size_t w = 0;
        for (int j = 0; j < l[i]; j++) {
            if (it[j].type != CSS_TOK_IDENT || tok_is_global_kw(&it[j])) return 0;
            if (j && w < sizeof(buf)) buf[w++] = ' ';
            size_t take = it[j].len;
            if (w + take > sizeof(buf)) take = sizeof(buf) - w;
            memcpy(buf + w, it[j].text, take);
            w += take;
        }
        tmp[i] = kn_atom_lower(buf, w);
    }
    int k = c < max ? c : max;
    for (int i = 0; i < k; i++) families[i] = tmp[i];
    *count = k;
    return 1;
}

int css_parse_font_weight(const CssToken *tokens, int n, int *out)
{
    CssToken t;
    if (!single_nows(tokens, n, &t)) return 0;
    if (tok_is_ident(&t, "normal")) { *out = 400; return 1; }
    if (tok_is_ident(&t, "bold")) { *out = 700; return 1; }
    if (tok_is_ident(&t, "bolder")) { *out = 700; return 1; }
    if (tok_is_ident(&t, "lighter")) { *out = 100; return 1; }
    if (t.type != CSS_TOK_NUMBER || t.num < 1 || t.num > 1000) return 0;
    *out = (int)t.num;
    return 1;
}

int css_parse_line_height(const CssToken *tokens, int n, CssLength *out)
{
    return css_parse_length(tokens, n, CSS_LENF_PCT | CSS_LENF_NORMAL | CSS_LENF_NUMBER | CSS_LENF_NONNEG, out);
}

int css_parse_vertical_align(const CssToken *tokens, int n, uint8_t *kw, CssLength *len)
{
    int v;
    if (css_parse_keyword(tokens, n, kw_vertical_align, &v)) { *kw = (uint8_t)v; return 1; }
    CssLength l;
    if (!css_parse_length(tokens, n, CSS_LENF_PCT, &l)) return 0;
    *kw = CSS_VERTICAL_ALIGN_LENGTH;
    *len = l;
    return 1;
}

int css_parse_content(const CssToken *tokens, int n, CssContent *out)
{
    CssToken t[16];
    int m = tokens_copy_nows(tokens, n, t, 16);
    CssContent c;
    memset(&c, 0, sizeof(c));
    if (m == 1 && tok_is_ident(&t[0], "none")) { c.kind = CSS_CONTENT_NONE; *out = c; return 1; }
    if (m == 1 && tok_is_ident(&t[0], "normal")) { c.kind = CSS_CONTENT_NORMAL; *out = c; return 1; }
    if (m == 1 && t[0].type == CSS_TOK_STRING) { c.kind = CSS_CONTENT_STRING; c.str = kn_atom(t[0].text, t[0].len); *out = c; return 1; }
    if (m == 3 && tok_is_func(&t[0], "attr") && t[1].type == CSS_TOK_IDENT && t[2].type == CSS_TOK_RPAREN) {
        c.kind = CSS_CONTENT_ATTR; c.str = kn_atom_lower(t[1].text, t[1].len); *out = c; return 1;
    }
    CssImage img;
    int used = 0;
    if (css_parse_image(t, m, &img, &used) && used == m && img.kind == CSS_IMAGE_URL) {
        c.kind = CSS_CONTENT_URL; c.str = img.url; c.base = img.base; *out = c; return 1;
    }
    return 0;
}

int css_parse_text_decoration(const CssToken *tokens, int n, uint8_t *bits)
{
    CssToken t[16];
    int m = tokens_copy_nows(tokens, n, t, 16);
    if (m == 0) return 0;
    uint8_t b = 0;
    int got_line = 0;
    for (int i = 0; i < m; i++) {
        int v, used = 0;
        CssColor col;
        if (css_keyword_lookup(&t[i], kw_text_decoration, &v)) {
            if (v == CSS_TEXT_DECORATION_NONE && (got_line || m != 1)) return 0;
            b |= (uint8_t)v; got_line = 1; continue;
        }
        if (tok_is_ident(&t[i], "solid") || tok_is_ident(&t[i], "double") || tok_is_ident(&t[i], "dotted") ||
            tok_is_ident(&t[i], "dashed") || tok_is_ident(&t[i], "wavy")) continue;
        if (css_parse_color_item(t + i, m - i, &col, &used)) { i += used - 1; continue; }
        return 0;
    }
    *bits = b;
    return 1;
}

int css_parse_will_change(const CssToken *tokens, int n, uint8_t *bits)
{
    CssToken t[32];
    int m = tokens_copy_nows(tokens, n, t, 32);
    if (m == 0) return 0;
    uint8_t b = 0;
    for (int i = 0; i < m; i++) {
        if (t[i].type == CSS_TOK_COMMA) continue;
        if (t[i].type != CSS_TOK_IDENT) return 0;
        if (tok_is_ident(&t[i], "auto")) { if (m != 1) return 0; }
        else if (tok_is_ident(&t[i], "transform")) b |= CSS_WILL_CHANGE_TRANSFORM;
        else if (tok_is_ident(&t[i], "opacity")) b |= CSS_WILL_CHANGE_OPACITY;
        else b |= CSS_WILL_CHANGE_OTHER;
    }
    *bits = b;
    return 1;
}

/* contain: none | strict | content | [ [size | inline-size] || layout || style || paint ] */
int css_parse_contain(const CssToken *tokens, int n, uint8_t *bits)
{
    CssToken t[8];
    int m = tokens_copy_nows(tokens, n, t, 8);
    if (m == 0 || m > 4) return 0;
    uint8_t b = 0;
    for (int i = 0; i < m; i++) {
        uint8_t bit;
        if (t[i].type != CSS_TOK_IDENT) return 0;
        if (m == 1 && tok_is_ident(&t[i], "none")) bit = 0;
        else if (m == 1 && tok_is_ident(&t[i], "strict")) bit = CSS_CONTAIN_SIZE | CSS_CONTAIN_LAYOUT | CSS_CONTAIN_PAINT | CSS_CONTAIN_STYLE;
        else if (m == 1 && tok_is_ident(&t[i], "content")) bit = CSS_CONTAIN_LAYOUT | CSS_CONTAIN_PAINT | CSS_CONTAIN_STYLE;
        else if (tok_is_ident(&t[i], "size")) bit = CSS_CONTAIN_SIZE;
        else if (tok_is_ident(&t[i], "inline-size")) bit = CSS_CONTAIN_INLINE_SIZE;
        else if (tok_is_ident(&t[i], "layout")) bit = CSS_CONTAIN_LAYOUT;
        else if (tok_is_ident(&t[i], "style")) bit = CSS_CONTAIN_STYLE;
        else if (tok_is_ident(&t[i], "paint")) bit = CSS_CONTAIN_PAINT;
        else return 0;
        if (b & bit) return 0;   /* each keyword once */
        b |= bit;
    }
    if ((b & CSS_CONTAIN_SIZE) && (b & CSS_CONTAIN_INLINE_SIZE)) return 0;
    *bits = b;
    return 1;
}

/* ========================================================================= */
/* var()                                                                     */
/* ========================================================================= */

int css_tokens_have_var(const CssToken *tokens, int n)
{
    for (int i = 0; i < n; i++)
        if (tok_is_func(&tokens[i], "var")) return 1;
    return 0;
}

static int subst_rec(const CssToken *in, int n, CssToken *out, int max, CssVarResolver resolve, void *user, int depth)
{
    int w = 0;
    if (depth > 8) return -1;
    for (int i = 0; i < n;) {
        if (!tok_is_func(&in[i], "var")) {
            if (w >= max) return -1;
            out[w++] = in[i++];
            continue;
        }
        int len = css_component_len(in, n, i);
        if (len < 3 || in[i + len - 1].type != CSS_TOK_RPAREN) return -1;
        /* body: [ws] ident [ws] [, fallback...] */
        int j = i + 1, end = i + len - 1;
        while (j < end && in[j].type == CSS_TOK_WHITESPACE) j++;
        if (j >= end || in[j].type != CSS_TOK_IDENT || in[j].len < 3 || in[j].text[0] != '-' || in[j].text[1] != '-') return -1;
        const CssToken *name = &in[j++];
        while (j < end && in[j].type == CSS_TOK_WHITESPACE) j++;
        const CssToken *fb = NULL; int fbn = 0;
        if (j < end) {
            if (in[j].type != CSS_TOK_COMMA) return -1;
            fb = &in[j + 1]; fbn = end - (j + 1);
        }
        const char *text = resolve(user, name->text, name->len);
        int k;
        if (text) {
            CssToken tmp[CSS_MAX_TOKENS];
            int tn = css_tokenize(text, strlen(text), tmp, CSS_MAX_TOKENS);
            int s = 0;
            while (s < tn && tmp[s].type == CSS_TOK_WHITESPACE) s++;
            while (tn > s && tmp[tn - 1].type == CSS_TOK_WHITESPACE) tn--;
            k = subst_rec(tmp + s, tn - s, out + w, max - w, resolve, user, depth + 1);
        } else if (fb) {
            k = subst_rec(fb, fbn, out + w, max - w, resolve, user, depth + 1);
        } else return -1;
        if (k < 0) return -1;
        w += k;
        i += len;
    }
    return w;
}

int css_substitute_vars(const CssToken *in, int n, CssToken *out, int max, CssVarResolver resolve, void *user)
{
    return subst_rec(in, n, out, max, resolve, user, 0);
}

/* ========================================================================= */
/* Shorthand expansion                                                       */
/* ========================================================================= */

static const CssToken g_comma_tok = { CSS_TOK_COMMA, 0, 0, ",", 1, 0, 0 };

typedef struct Emit {
    CssDeclCb cb;
    void *user;
    int important;
    int count;
} Emit;

static void emit(Emit *e, int prop, const CssToken *t, int n)
{
    e->cb(e->user, prop, t, n, e->important);
    e->count++;
}

static void emit_lit(Emit *e, int prop, const char *lit)
{
    CssToken buf[16];
    int n = tok_lit(lit, buf, 16);
    emit(e, prop, buf, n);
}

/* list builder: appends slices separated by commas */
#define LISTBUF_TOKENS 96
typedef struct ListBuf { CssToken t[LISTBUF_TOKENS]; int n; int items; } ListBuf;

static void lb_add(ListBuf *lb, const CssToken *t, int n)
{
    if (lb->items && lb->n < LISTBUF_TOKENS) lb->t[lb->n++] = g_comma_tok;
    for (int i = 0; i < n && lb->n < LISTBUF_TOKENS; i++) lb->t[lb->n++] = t[i];
    lb->items++;
}

static void lb_add_lit(ListBuf *lb, const char *lit)
{
    CssToken buf[16];
    int n = tok_lit(lit, buf, 16);
    lb_add(lb, buf, n);
}

static int shorthand_longhands(int id, int *out)
{
    int n = 0;
#define P(x) out[n++] = CSS_PROP_##x
    switch (id) {
    case CSS_PROP_MARGIN: P(MARGIN_TOP); P(MARGIN_RIGHT); P(MARGIN_BOTTOM); P(MARGIN_LEFT); break;
    case CSS_PROP_PADDING: P(PADDING_TOP); P(PADDING_RIGHT); P(PADDING_BOTTOM); P(PADDING_LEFT); break;
    case CSS_PROP_INSET: P(TOP); P(RIGHT); P(BOTTOM); P(LEFT); break;
    case CSS_PROP_BORDER:
        P(BORDER_TOP_WIDTH); P(BORDER_RIGHT_WIDTH); P(BORDER_BOTTOM_WIDTH); P(BORDER_LEFT_WIDTH);
        P(BORDER_TOP_STYLE); P(BORDER_RIGHT_STYLE); P(BORDER_BOTTOM_STYLE); P(BORDER_LEFT_STYLE);
        P(BORDER_TOP_COLOR); P(BORDER_RIGHT_COLOR); P(BORDER_BOTTOM_COLOR); P(BORDER_LEFT_COLOR); break;
    case CSS_PROP_BORDER_TOP: P(BORDER_TOP_WIDTH); P(BORDER_TOP_STYLE); P(BORDER_TOP_COLOR); break;
    case CSS_PROP_BORDER_RIGHT: P(BORDER_RIGHT_WIDTH); P(BORDER_RIGHT_STYLE); P(BORDER_RIGHT_COLOR); break;
    case CSS_PROP_BORDER_BOTTOM: P(BORDER_BOTTOM_WIDTH); P(BORDER_BOTTOM_STYLE); P(BORDER_BOTTOM_COLOR); break;
    case CSS_PROP_BORDER_LEFT: P(BORDER_LEFT_WIDTH); P(BORDER_LEFT_STYLE); P(BORDER_LEFT_COLOR); break;
    case CSS_PROP_BORDER_COLOR: P(BORDER_TOP_COLOR); P(BORDER_RIGHT_COLOR); P(BORDER_BOTTOM_COLOR); P(BORDER_LEFT_COLOR); break;
    case CSS_PROP_BORDER_WIDTH: P(BORDER_TOP_WIDTH); P(BORDER_RIGHT_WIDTH); P(BORDER_BOTTOM_WIDTH); P(BORDER_LEFT_WIDTH); break;
    case CSS_PROP_BORDER_STYLE: P(BORDER_TOP_STYLE); P(BORDER_RIGHT_STYLE); P(BORDER_BOTTOM_STYLE); P(BORDER_LEFT_STYLE); break;
    case CSS_PROP_BORDER_RADIUS: P(BORDER_TOP_LEFT_RADIUS); P(BORDER_TOP_RIGHT_RADIUS); P(BORDER_BOTTOM_RIGHT_RADIUS); P(BORDER_BOTTOM_LEFT_RADIUS); break;
    case CSS_PROP_OUTLINE: P(OUTLINE_WIDTH); P(OUTLINE_STYLE); P(OUTLINE_COLOR); break;
    case CSS_PROP_BACKGROUND: P(BACKGROUND_COLOR); P(BACKGROUND_IMAGE); P(BACKGROUND_POSITION_X); P(BACKGROUND_POSITION_Y); P(BACKGROUND_SIZE); P(BACKGROUND_REPEAT);
        P(BACKGROUND_CLIP); P(BACKGROUND_ORIGIN); break;
    case CSS_PROP_MASK: P(MASK_IMAGE); P(MASK_POSITION); P(MASK_SIZE); P(MASK_REPEAT); P(MASK_MODE); break;
    case CSS_PROP_BACKGROUND_POSITION: P(BACKGROUND_POSITION_X); P(BACKGROUND_POSITION_Y); break;
    case CSS_PROP_ANIMATION: P(ANIMATION_NAME); P(ANIMATION_DURATION); P(ANIMATION_TIMING_FUNCTION); P(ANIMATION_DELAY); P(ANIMATION_ITERATION_COUNT); P(ANIMATION_DIRECTION); P(ANIMATION_FILL_MODE); P(ANIMATION_PLAY_STATE); break;
    case CSS_PROP_TRANSITION: P(TRANSITION_PROPERTY); P(TRANSITION_DURATION); P(TRANSITION_TIMING_FUNCTION); P(TRANSITION_DELAY); break;
    case CSS_PROP_OVERFLOW: P(OVERFLOW_X); P(OVERFLOW_Y); break;
    case CSS_PROP_FLEX_FLOW: P(FLEX_DIRECTION); P(FLEX_WRAP); break;
    case CSS_PROP_FLEX: P(FLEX_GROW); P(FLEX_SHRINK); P(FLEX_BASIS); break;
    case CSS_PROP_GAP: P(ROW_GAP); P(COLUMN_GAP); break;
    case CSS_PROP_GRID_ROW: P(GRID_ROW_START); P(GRID_ROW_END); break;
    case CSS_PROP_GRID_COLUMN: P(GRID_COLUMN_START); P(GRID_COLUMN_END); break;
    case CSS_PROP_GRID_AREA: P(GRID_ROW_START); P(GRID_COLUMN_START); P(GRID_ROW_END); P(GRID_COLUMN_END); break;
    case CSS_PROP_PLACE_SELF: P(ALIGN_SELF); P(JUSTIFY_SELF); break;
    case CSS_PROP_GRID_TEMPLATE: P(GRID_TEMPLATE_ROWS); P(GRID_TEMPLATE_COLUMNS); P(GRID_TEMPLATE_AREAS); break;
    case CSS_PROP_GRID: P(GRID_TEMPLATE_ROWS); P(GRID_TEMPLATE_COLUMNS); P(GRID_TEMPLATE_AREAS);
        P(GRID_AUTO_ROWS); P(GRID_AUTO_COLUMNS); P(GRID_AUTO_FLOW); break;
    case CSS_PROP_LIST_STYLE: P(LIST_STYLE_TYPE); break;
    case CSS_PROP_FONT: P(FONT_STYLE); P(FONT_WEIGHT); P(FONT_SIZE); P(LINE_HEIGHT); P(FONT_FAMILY); break;
    default: break;
    }
#undef P
    return n;
}

typedef int (*ItemValidator)(const CssToken *t, int n);

static int valid_margin(const CssToken *t, int n) { CssLength l; return css_parse_length(t, n, CSS_LENF_PCT | CSS_LENF_AUTO, &l); }
static int valid_padding(const CssToken *t, int n) { CssLength l; return css_parse_length(t, n, CSS_LENF_PCT | CSS_LENF_NONNEG, &l); }
static int valid_radius(const CssToken *t, int n) { CssLength l; return css_parse_length(t, n, CSS_LENF_PCT | CSS_LENF_NONNEG, &l); }
static int valid_border_width(const CssToken *t, int n) { CssLength l; int u = 0; return css_parse_border_width(t, n, &l, &u) && u == n; }
static int valid_border_style(const CssToken *t, int n) { int v; return css_parse_keyword(t, n, css_kw_border_style, &v); }
static int valid_color(const CssToken *t, int n) { CssColor c; return css_parse_color(t, n, &c); }
static int valid_gap(const CssToken *t, int n) { CssLength l; return css_parse_length(t, n, CSS_LENF_PCT | CSS_LENF_NORMAL | CSS_LENF_NONNEG, &l); }

/* 1-4 component values -> top/right/bottom/left (or tl/tr/br/bl) */
static int expand_sides(Emit *e, const int *props, const CssToken *t, int m, ItemValidator valid)
{
    int s[4], l[4], c = 0, i = 0;
    while (i < m) {
        if (c >= 4) return 0;
        int len = css_component_len(t, m, i);
        s[c] = i; l[c] = len; c++;
        i += len;
    }
    if (c == 0) return 0;
    for (int k = 0; k < c; k++) if (!valid(t + s[k], l[k])) return 0;
    int map[4];
    switch (c) {
    case 1: map[0] = map[1] = map[2] = map[3] = 0; break;
    case 2: map[0] = map[2] = 0; map[1] = map[3] = 1; break;
    case 3: map[0] = 0; map[1] = map[3] = 1; map[2] = 2; break;
    default: map[0] = 0; map[1] = 1; map[2] = 2; map[3] = 3; break;
    }
    for (int k = 0; k < 4; k++) emit(e, props[k], t + s[map[k]], l[map[k]]);
    return 1;
}

/* border / border-<side> / outline: width || style || color in any order */
static int expand_border_like(Emit *e, const int *lh, int sides, const CssToken *t, int m, int is_outline)
{
    int ws = -1, wl = 0, ss = -1, sl = 0, cs = -1, cl = 0, i = 0;
    while (i < m) {
        int len = css_component_len(t, m, i);
        int v; CssLength l; CssColor c; int u = 0;
        if (ws < 0 && css_parse_border_width(t + i, len, &l, &u) && u == len) { ws = i; wl = len; }
        else if (ss < 0 && (css_parse_keyword(t + i, len, css_kw_border_style, &v) || (is_outline && tok_is_ident(&t[i], "auto")))) { ss = i; sl = len; }
        else if (cs < 0 && (css_parse_color(t + i, len, &c) || (is_outline && tok_is_ident(&t[i], "invert")))) { cs = i; cl = len; }
        else return 0;
        i += len;
    }
    for (int k = 0; k < sides; k++) {
        if (ws >= 0) emit(e, lh[k], t + ws, wl); else emit_lit(e, lh[k], "medium");
        if (ss >= 0) emit(e, lh[sides + k], t + ss, sl); else emit_lit(e, lh[sides + k], "none");
        if (cs >= 0) emit(e, lh[2 * sides + k], t + cs, cl); else emit_lit(e, lh[2 * sides + k], "currentcolor");
    }
    return 1;
}

static int expand_background(Emit *e, const CssToken *t, int m)
{
    int s[8], l[8];
    int c = split_commas(t, m, s, l, 8);
    if (c == 0) return 0;
    ListBuf img, posx, size, rep;
    memset(&img, 0, sizeof(img)); memset(&posx, 0, sizeof(posx)); memset(&size, 0, sizeof(size)); memset(&rep, 0, sizeof(rep));
    int col_s = -1, col_l = 0, bx_o = -1, bx_c = -1;
    for (int li = 0; li < c; li++) {
        const CssToken *it = t + s[li];
        int n = l[li], i = 0;
        int gi = -1, gil = 0, gp = -1, gpl = 0, gs = -1, gsl = 0, gr = -1, grl = 0, nbox = 0, box[2] = { -1, -1 };
        if (n == 0) return 0;
        while (i < n) {
            int used = 0;
            CssImage im; CssLength x, y; CssBgSize bs; uint8_t rx, ry; CssColor col;
            if (gi < 0 && css_parse_image(it + i, n - i, &im, &used)) { gi = i; gil = used; i += used; continue; }
            if (gp < 0 && parse_position(it + i, n - i, &x, &y, &used)) {
                gp = i; gpl = used; i += used;
                if (i < n && tok_is_delim(&it[i], '/')) {
                    i++;
                    if (gs >= 0 || !css_parse_bg_size_item(it + i, n - i, &bs, &used)) return 0;
                    gs = i; gsl = used; i += used;
                }
                continue;
            }
            if (gr < 0 && css_parse_bg_repeat_item(it + i, n - i, &rx, &ry, &used)) { gr = i; grl = used; i += used; continue; }
            if (col_s < 0 && li == c - 1 && css_parse_color_item(it + i, n - i, &col, &used)) { col_s = s[li] + i; col_l = used; i += used; continue; }
            if (tok_is_ident(&it[i], "scroll") || tok_is_ident(&it[i], "fixed") || tok_is_ident(&it[i], "local")) { i++; continue; }
            if (nbox < 2 && (tok_is_ident(&it[i], "border-box") || tok_is_ident(&it[i], "padding-box") ||
                             tok_is_ident(&it[i], "content-box") || (nbox == 1 && tok_is_ident(&it[i], "text")))) { box[nbox++] = i; i++; continue; }
            return 0;
        }
        /* one <box>: origin and clip; two: origin then clip (the last layer's count) */
        if (li == c - 1) { bx_o = nbox ? box[0] + s[li] : -1; bx_c = nbox ? box[nbox - 1] + s[li] : -1; }
        if (gi >= 0) lb_add(&img, it + gi, gil); else lb_add_lit(&img, "none");
        if (gp >= 0) lb_add(&posx, it + gp, gpl); else lb_add_lit(&posx, "0% 0%");
        if (gs >= 0) lb_add(&size, it + gs, gsl); else lb_add_lit(&size, "auto");
        if (gr >= 0) lb_add(&rep, it + gr, grl); else lb_add_lit(&rep, "repeat");
    }
    if (col_s >= 0) emit(e, CSS_PROP_BACKGROUND_COLOR, t + col_s, col_l); else emit_lit(e, CSS_PROP_BACKGROUND_COLOR, "transparent");
    emit(e, CSS_PROP_BACKGROUND_IMAGE, img.t, img.n);
    flag_full_position(posx.t, posx.n);
    emit(e, CSS_PROP_BACKGROUND_POSITION_X, posx.t, posx.n);
    emit(e, CSS_PROP_BACKGROUND_POSITION_Y, posx.t, posx.n);
    emit(e, CSS_PROP_BACKGROUND_SIZE, size.t, size.n);
    emit(e, CSS_PROP_BACKGROUND_REPEAT, rep.t, rep.n);
    if (bx_c >= 0) emit(e, CSS_PROP_BACKGROUND_CLIP, t + bx_c, 1); else emit_lit(e, CSS_PROP_BACKGROUND_CLIP, "border-box");
    if (bx_o >= 0 && !tok_is_ident(&t[bx_o], "text")) emit(e, CSS_PROP_BACKGROUND_ORIGIN, t + bx_o, 1);
    else emit_lit(e, CSS_PROP_BACKGROUND_ORIGIN, "padding-box");
    return 1;
}

/* mask: <mask-layer># (CSS Masking 1 §7.8): image || position [/ size]? || repeat ||
 * geometry boxes || composite || mode.  Boxes and composite are accepted and dropped
 * (mask-clip / mask-origin / mask-composite keep their initial values). */
static int expand_mask(Emit *e, const CssToken *t, int m)
{
    int s[8], l[8];
    int c = split_commas(t, m, s, l, 8);
    if (c == 0) return 0;
    ListBuf img, pos, size, rep, mode;
    memset(&img, 0, sizeof(img)); memset(&pos, 0, sizeof(pos)); memset(&size, 0, sizeof(size));
    memset(&rep, 0, sizeof(rep)); memset(&mode, 0, sizeof(mode));
    static const char *boxes[] = { "border-box", "padding-box", "content-box", "fill-box", "stroke-box", "view-box",
                                   "no-clip", "add", "subtract", "intersect", "exclude", NULL };
    for (int li = 0; li < c; li++) {
        const CssToken *it = t + s[li];
        int n = l[li], i = 0;
        int gi = -1, gil = 0, gp = -1, gpl = 0, gs = -1, gsl = 0, gr = -1, grl = 0, gm = -1;
        if (n == 0) return 0;
        while (i < n) {
            int used = 0, known = 0;
            CssImage im; CssLength x, y; CssBgSize bs; uint8_t rx, ry;
            if (gi < 0 && css_parse_image(it + i, n - i, &im, &used)) { gi = i; gil = used; i += used; continue; }
            if (gp < 0 && parse_position(it + i, n - i, &x, &y, &used)) {
                gp = i; gpl = used; i += used;
                if (i < n && tok_is_delim(&it[i], '/')) {
                    i++;
                    if (!css_parse_bg_size_item(it + i, n - i, &bs, &used)) return 0;
                    gs = i; gsl = used; i += used;
                }
                continue;
            }
            if (gr < 0 && css_parse_bg_repeat_item(it + i, n - i, &rx, &ry, &used)) { gr = i; grl = used; i += used; continue; }
            if (gm < 0 && (tok_is_ident(&it[i], "alpha") || tok_is_ident(&it[i], "luminance") || tok_is_ident(&it[i], "match-source"))) { gm = i; i++; continue; }
            for (int k = 0; boxes[k]; k++) if (tok_is_ident(&it[i], boxes[k])) known = 1;
            if (known) { i++; continue; }
            return 0;
        }
        if (gi >= 0) lb_add(&img, it + gi, gil); else lb_add_lit(&img, "none");
        if (gp >= 0) lb_add(&pos, it + gp, gpl); else lb_add_lit(&pos, "0% 0%");
        if (gs >= 0) lb_add(&size, it + gs, gsl); else lb_add_lit(&size, "auto");
        if (gr >= 0) lb_add(&rep, it + gr, grl); else lb_add_lit(&rep, "repeat");
        if (gm >= 0) lb_add(&mode, it + gm, 1); else lb_add_lit(&mode, "match-source");
    }
    emit(e, CSS_PROP_MASK_IMAGE, img.t, img.n);
    emit(e, CSS_PROP_MASK_POSITION, pos.t, pos.n);
    emit(e, CSS_PROP_MASK_SIZE, size.t, size.n);
    emit(e, CSS_PROP_MASK_REPEAT, rep.t, rep.n);
    emit(e, CSS_PROP_MASK_MODE, mode.t, mode.n);
    return 1;
}

static int expand_animation(Emit *e, const CssToken *t, int m)
{
    ListBuf lb[8];
    memset(lb, 0, sizeof(lb));
    static const char *defaults[8] = { "none", "0s", "ease", "0s", "1", "normal", "none", "running" };
    int i = 0, items = 0;
    while (i < m) {
        CssAnimation a; AnimParts p; int used = 0;
        if (!parse_animation_item_parts(t + i, m - i, &a, &p, &used)) return 0;
        int ps[8] = { p.name_s, p.dur_s, p.timing_s, p.delay_s, p.iter_s, p.dir_s, p.fill_s, p.play_s };
        int pl[8] = { p.name_l, p.dur_l, p.timing_l, p.delay_l, p.iter_l, p.dir_l, p.fill_l, p.play_l };
        for (int k = 0; k < 8; k++) {
            if (ps[k] >= 0) lb_add(&lb[k], t + i + ps[k], pl[k]); else lb_add_lit(&lb[k], defaults[k]);
        }
        items++;
        i += used;
        if (i < m) { if (t[i].type != CSS_TOK_COMMA) return 0; i++; if (i == m) return 0; }
    }
    if (!items) return 0;
    int props[8] = { CSS_PROP_ANIMATION_NAME, CSS_PROP_ANIMATION_DURATION, CSS_PROP_ANIMATION_TIMING_FUNCTION, CSS_PROP_ANIMATION_DELAY,
                     CSS_PROP_ANIMATION_ITERATION_COUNT, CSS_PROP_ANIMATION_DIRECTION, CSS_PROP_ANIMATION_FILL_MODE, CSS_PROP_ANIMATION_PLAY_STATE };
    for (int k = 0; k < 8; k++) emit(e, props[k], lb[k].t, lb[k].n);
    return 1;
}

static int expand_transition(Emit *e, const CssToken *t, int m)
{
    ListBuf lb[4];
    memset(lb, 0, sizeof(lb));
    static const char *defaults[4] = { "all", "0s", "ease", "0s" };
    int i = 0, items = 0;
    while (i < m) {
        CssTransition tr; TransParts p; int used = 0;
        if (!parse_transition_item_parts(t + i, m - i, &tr, &p, &used)) return 0;
        int ps[4] = { p.prop_s, p.dur_s, p.timing_s, p.delay_s };
        int pl[4] = { p.prop_l, p.dur_l, p.timing_l, p.delay_l };
        for (int k = 0; k < 4; k++) {
            if (ps[k] >= 0) lb_add(&lb[k], t + i + ps[k], pl[k]); else lb_add_lit(&lb[k], defaults[k]);
        }
        items++;
        i += used;
        if (i < m) { if (t[i].type != CSS_TOK_COMMA) return 0; i++; if (i == m) return 0; }
    }
    if (!items) return 0;
    int props[4] = { CSS_PROP_TRANSITION_PROPERTY, CSS_PROP_TRANSITION_DURATION, CSS_PROP_TRANSITION_TIMING_FUNCTION, CSS_PROP_TRANSITION_DELAY };
    for (int k = 0; k < 4; k++) emit(e, props[k], lb[k].t, lb[k].n);
    return 1;
}

/* font: [ <font-style> || <font-variant-css2> || <font-weight> || <font-stretch> ]?
 *       <font-size> [ / <line-height> ]? <font-family>
 * Omitted style/weight/line-height reset to normal (variant/stretch are
 * accepted and ignored).  System font keywords (caption, menu, ...) are not
 * supported: the declaration is dropped. */
static int font_size_valid(const CssToken *t, int n)
{
    static const char *const kw[] = { "xx-small", "x-small", "small", "medium", "large", "x-large", "xx-large",
                                      "xxx-large", "larger", "smaller", NULL };
    CssLength l;
    if (n == 1 && t[0].type == CSS_TOK_IDENT) {
        for (int i = 0; kw[i]; i++) if (tok_is_ident(&t[0], kw[i])) return 1;
        return 0;
    }
    return css_parse_length(t, n, CSS_LENF_PCT | CSS_LENF_NONNEG, &l);
}

static int expand_font(Emit *e, const CssToken *t, int m)
{
    static const char *const stretch[] = { "ultra-condensed", "extra-condensed", "condensed", "semi-condensed",
                                           "semi-expanded", "expanded", "extra-expanded", "ultra-expanded", NULL };
    int i = 0, style = -1, weight = -1, seen = 0;
    for (; i < m && seen < 4; i++, seen++) {
        const CssToken *k = &t[i];
        int is_stretch = 0;
        for (int s = 0; stretch[s]; s++) is_stretch |= tok_is_ident(k, stretch[s]);
        if (tok_is_ident(k, "normal") || tok_is_ident(k, "small-caps") || is_stretch) continue;
        if (style < 0 && (tok_is_ident(k, "italic") || tok_is_ident(k, "oblique"))) { style = i; continue; }
        if (weight < 0 && (tok_is_ident(k, "bold") || tok_is_ident(k, "bolder") || tok_is_ident(k, "lighter") ||
                           (k->type == CSS_TOK_NUMBER && k->num >= 1 && k->num <= 1000))) { weight = i; continue; }
        break;
    }
    if (i >= m) return 0;
    const int size_i = i, size_n = css_component_len(t, m, i);
    if (!font_size_valid(t + size_i, size_n)) return 0;
    i += size_n;
    int lh_i = -1, lh_n = 0;
    if (i < m && tok_is_delim(&t[i], '/')) {
        CssLength l;
        lh_i = ++i;
        lh_n = css_component_len(t, m, i);
        if (lh_n <= 0 || !css_parse_line_height(t + lh_i, lh_n, &l)) return 0;
        i += lh_n;
    }
    const char *fam[CSS_MAX_FONT_FAMILIES];
    int nf;
    if (i >= m || !css_parse_font_family(t + i, m - i, fam, CSS_MAX_FONT_FAMILIES, &nf)) return 0;
    if (style >= 0) emit(e, CSS_PROP_FONT_STYLE, t + style, 1); else emit_lit(e, CSS_PROP_FONT_STYLE, "normal");
    if (weight >= 0) emit(e, CSS_PROP_FONT_WEIGHT, t + weight, 1); else emit_lit(e, CSS_PROP_FONT_WEIGHT, "normal");
    emit(e, CSS_PROP_FONT_SIZE, t + size_i, size_n);
    if (lh_i >= 0) emit(e, CSS_PROP_LINE_HEIGHT, t + lh_i, lh_n); else emit_lit(e, CSS_PROP_LINE_HEIGHT, "normal");
    emit(e, CSS_PROP_FONT_FAMILY, t + i, m - i);
    return 1;
}

static int expand_flex(Emit *e, const CssToken *t, int m)
{
    if (m == 1 && tok_is_ident(&t[0], "none")) { emit_lit(e, CSS_PROP_FLEX_GROW, "0"); emit_lit(e, CSS_PROP_FLEX_SHRINK, "0"); emit_lit(e, CSS_PROP_FLEX_BASIS, "auto"); return 1; }
    if (m == 1 && tok_is_ident(&t[0], "auto")) { emit_lit(e, CSS_PROP_FLEX_GROW, "1"); emit_lit(e, CSS_PROP_FLEX_SHRINK, "1"); emit_lit(e, CSS_PROP_FLEX_BASIS, "auto"); return 1; }
    int gs = -1, ss = -1, bs = -1, bl = 0, i = 0;
    while (i < m) {
        int len = css_component_len(t, m, i);
        float f; CssLength l;
        if (len == 1 && t[i].type == CSS_TOK_NUMBER && gs < 0) { if (t[i].num < 0) return 0; gs = i; }
        else if (len == 1 && t[i].type == CSS_TOK_NUMBER && ss < 0) { if (t[i].num < 0) return 0; ss = i; }
        else if (bs < 0 && css_parse_length(t + i, len, CSS_LENF_PCT | CSS_LENF_AUTO | CSS_LENF_CONTENT | CSS_LENF_NONNEG, &l) &&
                 !(len == 1 && t[i].type == CSS_TOK_NUMBER && t[i].num != 0)) { bs = i; bl = len; }
        else return 0;
        (void)f;
        i += len;
    }
    if (gs < 0 && bs < 0) return 0;
    if (gs >= 0) emit(e, CSS_PROP_FLEX_GROW, t + gs, 1); else emit_lit(e, CSS_PROP_FLEX_GROW, "1");
    if (ss >= 0) emit(e, CSS_PROP_FLEX_SHRINK, t + ss, 1); else emit_lit(e, CSS_PROP_FLEX_SHRINK, "1");
    if (bs >= 0) emit(e, CSS_PROP_FLEX_BASIS, t + bs, bl); else emit_lit(e, CSS_PROP_FLEX_BASIS, "0%");
    return 1;
}

/* Index of the top-level '/' in t[0..m) or -1. */
static int grid_find_slash(const CssToken *t, int m, int from)
{
    for (int i = from; i < m; i += css_component_len(t, m, i))
        if (tok_is_delim(&t[i], '/')) return i;
    return -1;
}

static int valid_track_list(const CssToken *t, int m, int allow_auto_repeat)
{
    CssTrack tr[CSS_MAX_GRID_TRACKS];
    CssGridNames nm;
    int c;
    if (m == 1 && tok_is_ident(&t[0], "none")) return 1;
    return m > 0 && parse_grid_track_list(t, m, tr, CSS_MAX_GRID_TRACKS, &c, allow_auto_repeat ? &nm : NULL);
}

/* grid-template: none | <rows> / <columns> |
 * [<line-names>? <string> <track-size>? <line-names>?]+ [/ <explicit-track-list>]?
 * lh = rows, columns, areas. */
static int expand_grid_template(Emit *e, const int *lh, const CssToken *t, int m)
{
    if (m == 1 && tok_is_ident(&t[0], "none")) {
        for (int k = 0; k < 3; k++) emit_lit(e, lh[k], "none");
        return 1;
    }
    int has_string = 0;
    for (int i = 0; i < m; i++) has_string |= t[i].type == CSS_TOK_STRING;
    int slash = grid_find_slash(t, m, 0);
    if (!has_string) {
        if (slash < 0 || !valid_track_list(t, slash, 1) || !valid_track_list(t + slash + 1, m - slash - 1, 1)) return 0;
        emit(e, lh[0], t, slash);
        emit(e, lh[1], t + slash + 1, m - slash - 1);
        emit_lit(e, lh[2], "none");
        return 1;
    }
    /* areas form: the rows are the strings' track sizes (auto when omitted) */
    int rows_end = slash < 0 ? m : slash;
    CssToken rows[CSS_MAX_TOKENS], strs[64];
    CssToken auto_tok[2];
    tok_lit("auto", auto_tok, 2);
    int nr = 0, ns = 0, i = 0;
    while (i < rows_end) {
        int len = css_component_len(t, rows_end, i);
        if (t[i].type == CSS_TOK_LBRACKET) {
            if (nr + len > CSS_MAX_TOKENS) return 0;
            memcpy(rows + nr, t + i, (size_t)len * sizeof(CssToken));
            nr += len; i += len;
            continue;
        }
        if (t[i].type != CSS_TOK_STRING || ns >= 64) return 0;
        strs[ns++] = t[i++];
        /* optional <track-size> */
        if (i < rows_end && t[i].type != CSS_TOK_STRING && t[i].type != CSS_TOK_LBRACKET) {
            CssTrack tr;
            int used = 0;
            if (!parse_track_size(t + i, rows_end - i, &tr, &used) || nr + used > CSS_MAX_TOKENS) return 0;
            memcpy(rows + nr, t + i, (size_t)used * sizeof(CssToken));
            nr += used; i += used;
        } else {
            if (nr >= CSS_MAX_TOKENS) return 0;
            rows[nr++] = auto_tok[0];
        }
    }
    const char *areas; uint8_t ar, ac;
    if (!ns || !parse_grid_areas(strs, ns, &areas, &ar, &ac) || !valid_track_list(rows, nr, 0)) return 0;
    if (slash >= 0 && !valid_track_list(t + slash + 1, m - slash - 1, 0)) return 0;
    emit(e, lh[0], rows, nr);
    if (slash >= 0) emit(e, lh[1], t + slash + 1, m - slash - 1); else emit_lit(e, lh[1], "none");
    emit(e, lh[2], strs, ns);
    return 1;
}

/* auto-flow && dense? followed by optional auto tracks: t[0..m). */
static int grid_parse_auto_flow_part(const CssToken *t, int m, int *dense, int *tracks_at)
{
    int seen_flow = 0, i = 0;
    *dense = 0;
    while (i < m && i < 2) {
        if (!seen_flow && tok_is_ident(&t[i], "auto-flow")) { seen_flow = 1; i++; }
        else if (!*dense && tok_is_ident(&t[i], "dense")) { *dense = 1; i++; }
        else break;
    }
    if (!seen_flow) return 0;
    *tracks_at = i;
    return 1;
}

/* grid: <grid-template> | <rows> / [auto-flow && dense?] <auto-columns>? |
 * [auto-flow && dense?] <auto-rows>? / <columns>.  lh = rows, columns,
 * areas, auto rows, auto columns, auto flow. */
static int expand_grid(Emit *e, const int *lh, const CssToken *t, int m)
{
    int slash = grid_find_slash(t, m, 0);
    if (slash >= 0) {
        int dense = 0, at = 0;
        CssTrack tr[CSS_MAX_GRID_AUTO_TRACKS];
        int c;
        if (grid_parse_auto_flow_part(t + slash + 1, m - slash - 1, &dense, &at)) {
            const CssToken *a = t + slash + 1 + at;
            int an = m - slash - 1 - at;
            if (!valid_track_list(t, slash, 1) || (an && !parse_grid_auto_tracks(a, an, tr, &c))) return 0;
            emit(e, lh[0], t, slash);
            emit_lit(e, lh[1], "none");
            emit_lit(e, lh[2], "none");
            emit_lit(e, lh[3], "auto");
            if (an) emit(e, lh[4], a, an); else emit_lit(e, lh[4], "auto");
            emit_lit(e, lh[5], dense ? "column dense" : "column");
            return 1;
        }
        if (grid_parse_auto_flow_part(t, slash, &dense, &at)) {
            int an = slash - at;
            if (!valid_track_list(t + slash + 1, m - slash - 1, 1) || (an && !parse_grid_auto_tracks(t + at, an, tr, &c))) return 0;
            emit_lit(e, lh[0], "none");
            emit(e, lh[1], t + slash + 1, m - slash - 1);
            emit_lit(e, lh[2], "none");
            if (an) emit(e, lh[3], t + at, an); else emit_lit(e, lh[3], "auto");
            emit_lit(e, lh[4], "auto");
            emit_lit(e, lh[5], dense ? "row dense" : "row");
            return 1;
        }
    }
    Emit sub = *e;
    sub.count = 0;
    if (!expand_grid_template(&sub, lh, t, m)) return 0;
    e->count += sub.count;
    emit_lit(e, lh[3], "auto");
    emit_lit(e, lh[4], "auto");
    emit_lit(e, lh[5], "row");
    return 1;
}

int css_expand_shorthand(int id, const CssToken *tokens, int n, int important, CssDeclCb cb, void *user)
{
    if (!CSS_PROP_IS_SHORTHAND(id)) return 0;
    CssToken t[CSS_MAX_TOKENS];
    int m = tokens_copy_nows(tokens, n, t, CSS_MAX_TOKENS);
    if (m == 0) return 0;
    Emit e = { cb, user, important, 0 };
    int lh[12];
    int lc = shorthand_longhands(id, lh);
    if (css_tokens_have_var(t, m)) {
        CssToken buf[CSS_MAX_TOKENS + 1];
        memset(&buf[0], 0, sizeof(buf[0]));
        buf[0].type = CSS_TOK_SHORTHAND_REF;
        buf[0].ival = id;
        buf[0].text = "";
        memcpy(buf + 1, t, (size_t)m * sizeof(CssToken));
        for (int k = 0; k < lc; k++) emit(&e, lh[k], buf, m + 1);
        return e.count;
    }
    if (m == 1 && tok_is_global_kw(&t[0])) {
        for (int k = 0; k < lc; k++) emit(&e, lh[k], t, 1);
        return e.count;
    }
    int ok = 0;
    switch (id) {
    case CSS_PROP_MARGIN: ok = expand_sides(&e, lh, t, m, valid_margin); break;
    case CSS_PROP_INSET: ok = expand_sides(&e, lh, t, m, valid_margin); break;
    case CSS_PROP_PADDING: ok = expand_sides(&e, lh, t, m, valid_padding); break;
    case CSS_PROP_BORDER_RADIUS: ok = expand_sides(&e, lh, t, m, valid_radius); break;
    case CSS_PROP_BORDER_WIDTH: ok = expand_sides(&e, lh, t, m, valid_border_width); break;
    case CSS_PROP_BORDER_STYLE: ok = expand_sides(&e, lh, t, m, valid_border_style); break;
    case CSS_PROP_BORDER_COLOR: ok = expand_sides(&e, lh, t, m, valid_color); break;
    case CSS_PROP_BORDER: ok = expand_border_like(&e, lh, 4, t, m, 0); break;
    case CSS_PROP_BORDER_TOP: case CSS_PROP_BORDER_RIGHT: case CSS_PROP_BORDER_BOTTOM: case CSS_PROP_BORDER_LEFT:
        ok = expand_border_like(&e, lh, 1, t, m, 0); break;
    case CSS_PROP_OUTLINE: ok = expand_border_like(&e, lh, 1, t, m, 1); break;
    case CSS_PROP_BACKGROUND: ok = expand_background(&e, t, m); break;
    case CSS_PROP_MASK: ok = expand_mask(&e, t, m); break;
    case CSS_PROP_BACKGROUND_POSITION: {
        CssLength x[CSS_MAX_BG_LAYERS], y[CSS_MAX_BG_LAYERS]; int cx, cy;
        flag_full_position(t, m);
        if (css_parse_bg_position_list(t, m, 0, x, CSS_MAX_BG_LAYERS, &cx) && css_parse_bg_position_list(t, m, 1, y, CSS_MAX_BG_LAYERS, &cy)) {
            emit(&e, CSS_PROP_BACKGROUND_POSITION_X, t, m);
            emit(&e, CSS_PROP_BACKGROUND_POSITION_Y, t, m);
            ok = 1;
        }
        break;
    }
    case CSS_PROP_ANIMATION: ok = expand_animation(&e, t, m); break;
    case CSS_PROP_TRANSITION: ok = expand_transition(&e, t, m); break;
    case CSS_PROP_OVERFLOW: {
        int a, b;
        if (m == 1 && css_keyword_lookup(&t[0], css_kw_overflow, &a)) { emit(&e, lh[0], t, 1); emit(&e, lh[1], t, 1); ok = 1; }
        else if (m == 2 && css_keyword_lookup(&t[0], css_kw_overflow, &a) && css_keyword_lookup(&t[1], css_kw_overflow, &b)) { emit(&e, lh[0], t, 1); emit(&e, lh[1], t + 1, 1); ok = 1; }
        break;
    }
    case CSS_PROP_FLEX_FLOW: {
        int ds = -1, ws = -1, v;
        for (int i = 0; i < m; i++) {
            if (ds < 0 && css_keyword_lookup(&t[i], kw_flex_direction, &v)) ds = i;
            else if (ws < 0 && css_keyword_lookup(&t[i], kw_flex_wrap, &v)) ws = i;
            else { ds = -2; break; }
        }
        if (ds != -2 && m >= 1 && m <= 2) {
            if (ds >= 0) emit(&e, lh[0], t + ds, 1); else emit_lit(&e, lh[0], "row");
            if (ws >= 0) emit(&e, lh[1], t + ws, 1); else emit_lit(&e, lh[1], "nowrap");
            ok = 1;
        }
        break;
    }
    case CSS_PROP_FLEX: ok = expand_flex(&e, t, m); break;
    case CSS_PROP_FONT: ok = expand_font(&e, t, m); break;
    case CSS_PROP_GRID_ROW: case CSS_PROP_GRID_COLUMN: case CSS_PROP_GRID_AREA: {
        /* an omitted part copies its counterpart (grid-area: column-start from
         * row-start, the ends from the starts) when that is a sole
         * <custom-ident>, else it is auto */
        int ps[4], pl[4];
        int c = split_grid_lines(t, m, lc, ps, pl);
        if (c) {
            for (int k = c; k < lc; k++) {
                int src = lc == 4 ? (k == 1 ? 0 : k - 2) : 0;
                if (pl[src] == 1 && grid_ident_ok(&t[ps[src]])) { ps[k] = ps[src]; pl[k] = 1; }
                else pl[k] = 0;
            }
            for (int k = 0; k < lc; k++) {
                if (pl[k]) emit(&e, lh[k], t + ps[k], pl[k]); else emit_lit(&e, lh[k], "auto");
            }
            ok = 1;
        }
        break;
    }
    case CSS_PROP_GRID_TEMPLATE: ok = expand_grid_template(&e, lh, t, m); break;
    case CSS_PROP_GRID: ok = expand_grid(&e, lh, t, m); break;
    case CSS_PROP_PLACE_SELF: {
        int v0, v1;
        if (m == 1 && css_keyword_lookup(&t[0], kw_self_align, &v0)) { emit(&e, lh[0], t, 1); emit(&e, lh[1], t, 1); ok = 1; }
        else if (m == 2 && css_keyword_lookup(&t[0], kw_self_align, &v0) && css_keyword_lookup(&t[1], kw_self_align, &v1)) { emit(&e, lh[0], t, 1); emit(&e, lh[1], t + 1, 1); ok = 1; }
        break;
    }
    case CSS_PROP_GAP: {
        int l0 = css_component_len(t, m, 0);
        if (l0 == m && valid_gap(t, m)) { emit(&e, lh[0], t, m); emit(&e, lh[1], t, m); ok = 1; }
        else if (l0 < m && valid_gap(t, l0) && valid_gap(t + l0, m - l0)) { emit(&e, lh[0], t, l0); emit(&e, lh[1], t + l0, m - l0); ok = 1; }
        break;
    }
    case CSS_PROP_LIST_STYLE: {
        int ts = -1, v, bad = 0;
        for (int i = 0; i < m; i++) {
            if (ts < 0 && css_keyword_lookup(&t[i], kw_list_style, &v)) ts = i;
            else if (tok_is_ident(&t[i], "inside") || tok_is_ident(&t[i], "outside") || t[i].type == CSS_TOK_URL) continue;
            else { bad = 1; break; }
        }
        if (!bad) { if (ts >= 0) emit(&e, lh[0], t + ts, 1); else emit_lit(&e, lh[0], "disc"); ok = 1; }
        break;
    }
    default: break;
    }
    return ok ? e.count : 0;
}

/* ========================================================================= */
/* Computed style                                                            */
/* ========================================================================= */

static CssComputed g_initial;
static int g_initial_state; /* 0 = not built, 1 = building, 2 = ready */

static void build_initial(void)
{
    g_initial_state = 1;
    memset(&g_initial, 0, sizeof(g_initial));
    for (int id = 0; id < CSS_PROP_LONGHAND_COUNT; id++) {
        const char *txt = g_prop_initial[id];
        CssToken t[32];
        int n = css_tokenize(txt, strlen(txt), t, 32);
        css_computed_apply(&g_initial, id, t, n, NULL, 16.0f);
    }
    css_computed_hash(&g_initial);
    g_initial_state = 2;
}

const CssComputed *css_computed_initial(void)
{
    if (g_initial_state == 0) build_initial();
    return &g_initial;
}

void css_computed_init_defaults(CssComputed *c)
{
    memcpy(c, css_computed_initial(), sizeof(*c));
}

void css_computed_inherit(CssComputed *dst, const CssComputed *parent)
{
    memcpy((char *)dst + CSS_COMPUTED_INHERITED_BEGIN, (const char *)parent + CSS_COMPUTED_INHERITED_BEGIN,
           CSS_COMPUTED_INHERITED_END - CSS_COMPUTED_INHERITED_BEGIN);
}

void css_computed_hash(CssComputed *c)
{
    c->hash_inherited = kn_hash_mem(KN_FNV64_INIT, (const char *)c + CSS_COMPUTED_INHERITED_BEGIN,
                                      CSS_COMPUTED_INHERITED_END - CSS_COMPUTED_INHERITED_BEGIN);
    c->hash_non_inherited = kn_hash_mem(KN_FNV64_INIT, (const char *)c + CSS_COMPUTED_NON_INHERITED_BEGIN,
                                          CSS_COMPUTED_NON_INHERITED_END - CSS_COMPUTED_NON_INHERITED_BEGIN);
}

static void zoom_len(CssLength *l, float k)
{
    if (l->kind == CSS_LEN_VALUE) { l->px *= k; l->em *= k; }
}

static void zoom_shadow(CssShadow *s, float k)
{
    s->x *= k; s->y *= k; s->blur *= k; s->spread *= k;
}

static void zoom_image(CssImage *im, float k)
{
    if (im->kind != CSS_IMAGE_GRADIENT) return;
    zoom_len(&im->gradient.cx, k);
    zoom_len(&im->gradient.cy, k);
    for (int i = 0; i < CSS_MAX_GRADIENT_STOPS; i++) zoom_len(&im->gradient.stops[i].pos, k);
}

static void zoom_non_inherited(CssComputed *c, float k)
{
    for (int i = 0; i < 4; i++) {
        zoom_len(&c->inset[i], k);
        zoom_len(&c->margin[i], k);
        zoom_len(&c->padding[i], k);
        zoom_len(&c->border_radius[i], k);
        c->border_width[i] *= k;
    }
    zoom_len(&c->width, k); zoom_len(&c->height, k);
    zoom_len(&c->min_width, k); zoom_len(&c->min_height, k);
    zoom_len(&c->max_width, k); zoom_len(&c->max_height, k);
    c->outline_width *= k;
    for (int i = 0; i < CSS_MAX_BG_LAYERS; i++) {
        CssBgLayer *b = &c->bg_layers[i];
        zoom_image(&b->image, k);
        zoom_len(&b->x, k); zoom_len(&b->y, k);
        zoom_len(&b->size.w, k); zoom_len(&b->size.h, k);
    }
    for (int i = 0; i < c->transform.count && i < CSS_MAX_TRANSFORM_OPS; i++) {
        CssTransformOp *op = &c->transform.ops[i];
        if (op->kind == CSS_TRANSFORM_TRANSLATE) { zoom_len(&op->tx, k); zoom_len(&op->ty, k); }
        else if (op->kind == CSS_TRANSFORM_MATRIX) { op->m.e *= k; op->m.f *= k; }
        if (op->kind == CSS_TRANSFORM_TRANSLATE) op->tz *= k;
        else if (op->kind == CSS_TRANSFORM_PERSPECTIVE) op->persp *= k;
    }
    zoom_len(&c->perspective, k);
    zoom_len(&c->perspective_origin_x, k); zoom_len(&c->perspective_origin_y, k);
    for (int i = 0; i < c->backdrop_filter_count && i < CSS_MAX_FILTER_OPS; i++) {
        if (c->backdrop_filters[i].kind == CSS_FILTER_BLUR) c->backdrop_filters[i].amount *= k;
        else if (c->backdrop_filters[i].kind == CSS_FILTER_DROP_SHADOW) zoom_shadow(&c->backdrop_filters[i].shadow, k);
    }
    for (int i = 0; i < CSS_MAX_MASK_LAYERS; i++) {
        CssBgLayer *b = &c->mask_layers[i];
        zoom_image(&b->image, k);
        zoom_len(&b->x, k); zoom_len(&b->y, k);
        zoom_len(&b->size.w, k); zoom_len(&b->size.h, k);
    }
    c->outline_offset *= k;
    zoom_len(&c->transform_origin_x, k); zoom_len(&c->transform_origin_y, k);
    zoom_shadow(&c->box_shadow, k);
    for (int i = 0; i < CSS_MAX_BOX_SHADOWS - 1; i++) zoom_shadow(&c->box_shadow_more[i], k);
    for (int i = 0; i < c->filter_count && i < CSS_MAX_FILTER_OPS; i++) {
        if (c->filters[i].kind == CSS_FILTER_BLUR) c->filters[i].amount *= k;
        else if (c->filters[i].kind == CSS_FILTER_DROP_SHADOW) zoom_shadow(&c->filters[i].shadow, k);
    }
    for (int i = 0; i < c->clip_path.point_count && i < CSS_MAX_CLIP_POINTS; i++) {
        zoom_len(&c->clip_path.x[i], k); zoom_len(&c->clip_path.y[i], k);
    }
    zoom_len(&c->vertical_align_length, k);
    zoom_len(&c->flex_basis, k);
    for (int i = 0; i < CSS_MAX_GRID_TRACKS; i++) { zoom_len(&c->grid_columns[i].size, k); zoom_len(&c->grid_columns[i].max, k); }
    for (int i = 0; i < CSS_MAX_GRID_TRACKS; i++) { zoom_len(&c->grid_rows[i].size, k); zoom_len(&c->grid_rows[i].max, k); }
    for (int a = 0; a < 2; a++)
        for (int i = 0; i < CSS_MAX_GRID_AUTO_TRACKS; i++) { zoom_len(&c->grid_auto_tracks[a][i].size, k); zoom_len(&c->grid_auto_tracks[a][i].max, k); }
    zoom_len(&c->row_gap, k); zoom_len(&c->column_gap, k);
}

void css_computed_zoom(CssComputed *c, float k, int inherited_only)
{
    if (k == 1.0f) return;
    c->font_size *= k;
    zoom_len(&c->line_height, k);          /* NUMBER / NORMAL untouched */
    zoom_len(&c->letter_spacing, k);
    zoom_len(&c->border_spacing_x, k); zoom_len(&c->border_spacing_y, k);
    for (int i = 0; i < c->text_shadow.count && i < CSS_MAX_TEXT_SHADOWS; i++) zoom_shadow(&c->text_shadow.items[i], k);
    if (!inherited_only) zoom_non_inherited(c, k);
}

float css_snap_border_width(float w)
{
    if (w <= 0.0f) return 0.0f;
    return w < 1.0f ? 1.0f : floorf(w + 1e-4f);
}

const char *css_computed_custom_get(const CssComputed *c, const char *name, size_t len)
{
    const char *atom = kn_atom(name, len);
    for (int i = 0; i < c->custom_count; i++)
        if (c->custom[i].name == atom) return c->custom[i].value;
    return NULL;
}

static void custom_set(CssComputed *c, const char *name, const char *value)
{
    for (int i = 0; i < c->custom_count; i++) {
        if (c->custom[i].name == name) {
            if (value) c->custom[i].value = value;
            else { c->custom[i] = c->custom[c->custom_count - 1]; memset(&c->custom[c->custom_count - 1], 0, sizeof(CssCustomProp)); c->custom_count--; }
            return;
        }
    }
    if (!value) return;
    if (c->custom_count >= CSS_MAX_CUSTOM_PROPS) {
        static int warned;
        if (!warned++) fprintf(stderr, "[css] more than %d custom properties on one element: %s dropped\n", CSS_MAX_CUSTOM_PROPS, name);
        return;
    }
    c->custom[c->custom_count].name = name;
    c->custom[c->custom_count].value = value;
    c->custom_count++;
}

void css_computed_copy_prop(CssComputed *d, const CssComputed *s, int id)
{
#define CP(f) d->f = s->f
#define CPA(f) memcpy(d->f, s->f, sizeof(d->f))
    if (CSS_PROP_IS_CUSTOM(id)) {
        const char *name = css_prop_name(id);
        if (name) custom_set(d, name, css_computed_custom_get(s, name, strlen(name)));
        return;
    }
    switch (id) {
    case CSS_PROP_COLOR: CP(color); break;
    case CSS_PROP_FONT_FAMILY: CPA(font_families); CP(font_family_count); break;
    case CSS_PROP_FONT_SIZE: CP(font_size); break;
    case CSS_PROP_FONT_WEIGHT: CP(font_weight); break;
    case CSS_PROP_FONT_STYLE: CP(font_style); break;
    case CSS_PROP_LINE_HEIGHT: CP(line_height); break;
    case CSS_PROP_LETTER_SPACING: CP(letter_spacing); break;
    case CSS_PROP_TEXT_ALIGN: CP(text_align); break;
    case CSS_PROP_TEXT_TRANSFORM: CP(text_transform); break;
    case CSS_PROP_WHITE_SPACE: CP(white_space); break;
    case CSS_PROP_TEXT_WRAP: CP(white_space); break; /* text-wrap maps onto the wrap part of white-space */
    case CSS_PROP_OVERFLOW_WRAP: CP(overflow_wrap); break;
    case CSS_PROP_WORD_BREAK: CP(word_break); break;
    case CSS_PROP_VISIBILITY: CP(visibility); break;
    case CSS_PROP_CURSOR: CP(cursor); break;
    case CSS_PROP_POINTER_EVENTS: CP(pointer_events); break;
    case CSS_PROP_USER_SELECT: CP(user_select); break;
    case CSS_PROP_DIRECTION: CP(direction); break;
    case CSS_PROP_IMAGE_RENDERING: CP(image_rendering); break;
    case CSS_PROP_BORDER_COLLAPSE: CP(border_collapse); break;
    case CSS_PROP_LIST_STYLE_TYPE: CP(list_style_type); break;
    case CSS_PROP_BORDER_SPACING: CP(border_spacing_x); CP(border_spacing_y); break;
    case CSS_PROP_TEXT_SHADOW: CP(text_shadow); break;
    case CSS_PROP_DISPLAY: CP(display); CP(display_legacy_box); break;
    case CSS_PROP_POSITION: CP(position); break;
    case CSS_PROP_FLOAT: CP(float_); break;
    case CSS_PROP_CLEAR: CP(clear); break;
    case CSS_PROP_BOX_SIZING: CP(box_sizing); break;
    case CSS_PROP_TABLE_LAYOUT: CP(table_layout); break;
    case CSS_PROP_OVERFLOW_X: CP(overflow_x); break;
    case CSS_PROP_OVERFLOW_Y: CP(overflow_y); break;
    case CSS_PROP_APPEARANCE: CP(appearance); break;
    case CSS_PROP_WILL_CHANGE: CP(will_change); break;
    case CSS_PROP_CONTAIN: CP(contain); break;
    case CSS_PROP_MIX_BLEND_MODE: CP(mix_blend_mode); break;
    case CSS_PROP_Z_INDEX: CP(z_index); CP(z_index_auto); break;
    case CSS_PROP_OPACITY: CP(opacity); break;
    case CSS_PROP_TOP: case CSS_PROP_RIGHT: case CSS_PROP_BOTTOM: case CSS_PROP_LEFT: CP(inset[id - CSS_PROP_TOP]); break;
    case CSS_PROP_WIDTH: CP(width); break;
    case CSS_PROP_HEIGHT: CP(height); break;
    case CSS_PROP_MIN_WIDTH: CP(min_width); break;
    case CSS_PROP_MIN_HEIGHT: CP(min_height); break;
    case CSS_PROP_MAX_WIDTH: CP(max_width); break;
    case CSS_PROP_MAX_HEIGHT: CP(max_height); break;
    case CSS_PROP_MARGIN_TOP: case CSS_PROP_MARGIN_RIGHT: case CSS_PROP_MARGIN_BOTTOM: case CSS_PROP_MARGIN_LEFT: CP(margin[id - CSS_PROP_MARGIN_TOP]); break;
    case CSS_PROP_PADDING_TOP: case CSS_PROP_PADDING_RIGHT: case CSS_PROP_PADDING_BOTTOM: case CSS_PROP_PADDING_LEFT: CP(padding[id - CSS_PROP_PADDING_TOP]); break;
    case CSS_PROP_BORDER_TOP_WIDTH: case CSS_PROP_BORDER_RIGHT_WIDTH: case CSS_PROP_BORDER_BOTTOM_WIDTH: case CSS_PROP_BORDER_LEFT_WIDTH: CP(border_width[id - CSS_PROP_BORDER_TOP_WIDTH]); break;
    case CSS_PROP_BORDER_TOP_STYLE: case CSS_PROP_BORDER_RIGHT_STYLE: case CSS_PROP_BORDER_BOTTOM_STYLE: case CSS_PROP_BORDER_LEFT_STYLE: CP(border_style[id - CSS_PROP_BORDER_TOP_STYLE]); break;
    case CSS_PROP_BORDER_TOP_COLOR: case CSS_PROP_BORDER_RIGHT_COLOR: case CSS_PROP_BORDER_BOTTOM_COLOR: case CSS_PROP_BORDER_LEFT_COLOR: CP(border_color[id - CSS_PROP_BORDER_TOP_COLOR]); break;
    case CSS_PROP_BORDER_TOP_LEFT_RADIUS: case CSS_PROP_BORDER_TOP_RIGHT_RADIUS: case CSS_PROP_BORDER_BOTTOM_RIGHT_RADIUS: case CSS_PROP_BORDER_BOTTOM_LEFT_RADIUS: CP(border_radius[id - CSS_PROP_BORDER_TOP_LEFT_RADIUS]); break;
    case CSS_PROP_OUTLINE_WIDTH: CP(outline_width); break;
    case CSS_PROP_OUTLINE_STYLE: CP(outline_style); break;
    case CSS_PROP_OUTLINE_COLOR: CP(outline_color); break;
    case CSS_PROP_BACKGROUND_COLOR: CP(background_color); break;
    case CSS_PROP_BACKGROUND_IMAGE: for (int i = 0; i < CSS_MAX_BG_LAYERS; i++) CP(bg_layers[i].image); CP(bg_layer_count); break;
    case CSS_PROP_BACKGROUND_POSITION_X: for (int i = 0; i < CSS_MAX_BG_LAYERS; i++) CP(bg_layers[i].x); break;
    case CSS_PROP_BACKGROUND_POSITION_Y: for (int i = 0; i < CSS_MAX_BG_LAYERS; i++) CP(bg_layers[i].y); break;
    case CSS_PROP_BACKGROUND_SIZE: for (int i = 0; i < CSS_MAX_BG_LAYERS; i++) CP(bg_layers[i].size); break;
    case CSS_PROP_BACKGROUND_REPEAT: for (int i = 0; i < CSS_MAX_BG_LAYERS; i++) { CP(bg_layers[i].repeat_x); CP(bg_layers[i].repeat_y); } break;
    case CSS_PROP_TRANSFORM: CP(transform); break;
    case CSS_PROP_TRANSFORM_ORIGIN: CP(transform_origin_x); CP(transform_origin_y); break;
    case CSS_PROP_BOX_SHADOW: CP(box_shadow); CPA(box_shadow_more); CP(box_shadow_count); break;
    case CSS_PROP_FILTER: CPA(filters); CP(filter_count); break;
    case CSS_PROP_CLIP_PATH: CP(clip_path); break;
    case CSS_PROP_OBJECT_FIT: CP(object_fit); break;
    case CSS_PROP_CONTENT: CP(content); break;
    case CSS_PROP_TEXT_DECORATION: CP(text_decoration); break;
    case CSS_PROP_TEXT_OVERFLOW: CP(text_overflow); break;
    case CSS_PROP_VERTICAL_ALIGN: CP(vertical_align); CP(vertical_align_length); break;
    case CSS_PROP_FLEX_DIRECTION: CP(flex_direction); break;
    case CSS_PROP_FLEX_WRAP: CP(flex_wrap); break;
    case CSS_PROP_JUSTIFY_CONTENT: CP(justify_content); break;
    case CSS_PROP_ALIGN_CONTENT: CP(align_content); break;
    case CSS_PROP_ALIGN_ITEMS: CP(align_items); break;
    case CSS_PROP_FLEX_GROW: CP(flex_grow); break;
    case CSS_PROP_FLEX_SHRINK: CP(flex_shrink); break;
    case CSS_PROP_FLEX_BASIS: CP(flex_basis); break;
    case CSS_PROP_GRID_TEMPLATE_COLUMNS: CPA(grid_columns); CP(grid_column_count); CP(grid_names[CSS_GRID_AXIS_COLUMN]); break;
    case CSS_PROP_GRID_TEMPLATE_ROWS: CPA(grid_rows); CP(grid_row_count); CP(grid_names[CSS_GRID_AXIS_ROW]); break;
    case CSS_PROP_GRID_TEMPLATE_AREAS: CP(grid_areas); CP(grid_area_rows); CP(grid_area_cols); break;
    case CSS_PROP_GRID_AUTO_ROWS: CPA(grid_auto_tracks[CSS_GRID_AXIS_ROW]); CP(grid_auto_count[CSS_GRID_AXIS_ROW]); break;
    case CSS_PROP_GRID_AUTO_COLUMNS: CPA(grid_auto_tracks[CSS_GRID_AXIS_COLUMN]); CP(grid_auto_count[CSS_GRID_AXIS_COLUMN]); break;
    case CSS_PROP_GRID_AUTO_FLOW: CP(grid_auto_flow); break;
    case CSS_PROP_GRID_ROW_START: case CSS_PROP_GRID_COLUMN_START: case CSS_PROP_GRID_ROW_END: case CSS_PROP_GRID_COLUMN_END:
        CP(grid_lines[id - CSS_PROP_GRID_ROW_START]); break;
    case CSS_PROP_JUSTIFY_ITEMS: CP(justify_items); break;
    case CSS_PROP_JUSTIFY_SELF: CP(justify_self); break;
    case CSS_PROP_ALIGN_SELF: CP(align_self); break;
    case CSS_PROP_ROW_GAP: CP(row_gap); break;
    case CSS_PROP_COLUMN_GAP: CP(column_gap); break;
    case CSS_PROP_ZOOM: CP(zoom); break;
    case CSS_PROP_ANIMATION_NAME: for (int i = 0; i < CSS_MAX_ANIMATIONS; i++) CP(animations[i].name); CP(animation_count); break;
    case CSS_PROP_ANIMATION_DURATION: for (int i = 0; i < CSS_MAX_ANIMATIONS; i++) CP(animations[i].duration_s); break;
    case CSS_PROP_ANIMATION_TIMING_FUNCTION: for (int i = 0; i < CSS_MAX_ANIMATIONS; i++) CP(animations[i].timing); break;
    case CSS_PROP_ANIMATION_DELAY: for (int i = 0; i < CSS_MAX_ANIMATIONS; i++) CP(animations[i].delay_s); break;
    case CSS_PROP_ANIMATION_ITERATION_COUNT: for (int i = 0; i < CSS_MAX_ANIMATIONS; i++) CP(animations[i].iterations); break;
    case CSS_PROP_ANIMATION_DIRECTION: for (int i = 0; i < CSS_MAX_ANIMATIONS; i++) CP(animations[i].direction); break;
    case CSS_PROP_ANIMATION_FILL_MODE: for (int i = 0; i < CSS_MAX_ANIMATIONS; i++) CP(animations[i].fill); break;
    case CSS_PROP_ANIMATION_PLAY_STATE: for (int i = 0; i < CSS_MAX_ANIMATIONS; i++) CP(animations[i].play_state); break;
    case CSS_PROP_TRANSITION_PROPERTY: for (int i = 0; i < CSS_MAX_TRANSITIONS; i++) CP(transitions[i].prop); CP(transition_count); break;
    case CSS_PROP_TRANSITION_DURATION: for (int i = 0; i < CSS_MAX_TRANSITIONS; i++) CP(transitions[i].duration_s); break;
    case CSS_PROP_TRANSITION_TIMING_FUNCTION: for (int i = 0; i < CSS_MAX_TRANSITIONS; i++) CP(transitions[i].timing); break;
    case CSS_PROP_TRANSITION_DELAY: for (int i = 0; i < CSS_MAX_TRANSITIONS; i++) CP(transitions[i].delay_s); break;
    case CSS_PROP_TEXT_FILL_COLOR: CP(text_fill_color); break;
    case CSS_PROP_ACCENT_COLOR: CP(accent_color); CP(accent_color_auto); break;
    case CSS_PROP_BACKDROP_FILTER: CPA(backdrop_filters); CP(backdrop_filter_count); break;
    case CSS_PROP_PERSPECTIVE: CP(perspective); break;
    case CSS_PROP_PERSPECTIVE_ORIGIN: CP(perspective_origin_x); CP(perspective_origin_y); break;
    case CSS_PROP_TRANSFORM_STYLE: CP(transform_style); break;
    case CSS_PROP_BACKFACE_VISIBILITY: CP(backface_visibility); break;
    case CSS_PROP_MASK_IMAGE: for (int i = 0; i < CSS_MAX_MASK_LAYERS; i++) CP(mask_layers[i].image); CP(mask_layer_count); break;
    case CSS_PROP_MASK_POSITION: for (int i = 0; i < CSS_MAX_MASK_LAYERS; i++) { CP(mask_layers[i].x); CP(mask_layers[i].y); } break;
    case CSS_PROP_MASK_SIZE: for (int i = 0; i < CSS_MAX_MASK_LAYERS; i++) CP(mask_layers[i].size); break;
    case CSS_PROP_MASK_REPEAT: for (int i = 0; i < CSS_MAX_MASK_LAYERS; i++) { CP(mask_layers[i].repeat_x); CP(mask_layers[i].repeat_y); } break;
    case CSS_PROP_MASK_MODE: CP(mask_mode); break;
    case CSS_PROP_BACKGROUND_CLIP: CP(background_clip); break;
    case CSS_PROP_BACKGROUND_ORIGIN: CP(background_origin); break;
    case CSS_PROP_OUTLINE_OFFSET: CP(outline_offset); break;
    case CSS_PROP_ASPECT_RATIO: CP(aspect_ratio); CP(aspect_ratio_auto); break;
    case CSS_PROP_LINE_CLAMP: CP(line_clamp); break;
    case CSS_PROP_BOX_ORIENT: CP(box_orient); break;
    default: break; /* ignored properties carry no state */
    }
#undef CP
#undef CPA
}

typedef struct PendingApply {
    CssComputed *dst;
    int prop;
    const CssComputed *parent;
    float font_px;
    int ok;
} PendingApply;

static void pending_cb(void *user, int prop_id, const CssToken *tokens, int n, int important)
{
    PendingApply *p = user;
    (void)important;
    if (prop_id == p->prop) p->ok = css_computed_apply(p->dst, prop_id, tokens, n, p->parent, p->font_px);
}

static void resolve_em_len(CssLength *l, float font_px) { css_length_resolve_em(l, font_px); }

#define SET_KW(table, field) do { int v_; if (!css_parse_keyword(t, m, table, &v_)) return 0; d->field = (uint8_t)v_; return 1; } while (0)
#define SET_LEN(flags, field) do { CssLength l_; if (!css_parse_length(t, m, flags, &l_)) return 0; resolve_em_len(&l_, font_px); d->field = l_; return 1; } while (0)

int css_computed_apply(CssComputed *d, int id, const CssToken *tokens, int n,
                       const CssComputed *parent, float font_px)
{
    CssToken t[CSS_MAX_TOKENS];
    int m = tokens_copy_nows(tokens, n, t, CSS_MAX_TOKENS);
    g_shadow_em_px = font_px > 0 ? font_px : 16.0f;
    if (m > 0 && t[0].type == CSS_TOK_SHORTHAND_REF) {
        /* the cascade must substitute var() first; otherwise expansion
         * would re-emit the pending marker forever */
        if (css_tokens_have_var(t + 1, m - 1)) return 0;
        PendingApply p = { d, id, parent, font_px, 0 };
        css_expand_shorthand(t[0].ival, t + 1, m - 1, 0, pending_cb, &p);
        return p.ok;
    }
    if (!CSS_PROP_IS_LONGHAND(id) && !CSS_PROP_IS_CUSTOM(id)) return 0;
    const CssComputed *initial = g_initial_state == 1 ? &g_initial : css_computed_initial();
    if (m == 1 && t[0].type == CSS_TOK_IDENT) {
        int inherit = tok_is_ident(&t[0], "inherit");
        int initial_kw = tok_is_ident(&t[0], "initial");
        int unset = tok_is_ident(&t[0], "unset") || tok_is_ident(&t[0], "revert");
        if (inherit || initial_kw || unset) {
            int use_parent = inherit || (unset && css_prop_is_inherited(id));
            if (use_parent && parent) css_computed_copy_prop(d, parent, id);
            else if (CSS_PROP_IS_CUSTOM(id)) custom_set(d, css_prop_name(id), NULL);
            else css_computed_copy_prop(d, initial, id);
            return 1;
        }
    }
    if (CSS_PROP_IS_CUSTOM(id)) {
        char buf[1024];
        size_t w = css_tokens_to_text(tokens, n, buf, sizeof(buf));
        if (w >= sizeof(buf)) w = sizeof(buf) - 1;
        size_t len = w;
        const char *txt = kn_str_trim(buf, &len);
        custom_set(d, css_prop_name(id), kn_atom(txt, len));
        return 1;
    }
    if (m == 0) return 0;
    switch (id) {
    case CSS_PROP_COLOR: {
        CssColor c;
        if (!css_parse_color(t, m, &c)) return 0;
        if (c.kind != CSS_COLOR_RGBA) c = css_color_resolve(c, parent ? parent->color : initial->color);
        d->color = c;
        return 1;
    }
    case CSS_PROP_FONT_FAMILY: {
        const char *fam[CSS_MAX_FONT_FAMILIES]; int c;
        if (!css_parse_font_family(t, m, fam, CSS_MAX_FONT_FAMILIES, &c)) return 0;
        memset(d->font_families, 0, sizeof(d->font_families));
        for (int i = 0; i < c; i++) d->font_families[i] = fam[i];
        d->font_family_count = (uint8_t)c;
        return 1;
    }
    case CSS_PROP_FONT_SIZE: {
        float base = parent ? parent->font_size : 16.0f;
        static const CssKeyword kw[] = { { "xx-small", 9 }, { "x-small", 10 }, { "small", 13 }, { "medium", 16 }, { "large", 18 }, { "x-large", 24 }, { "xx-large", 32 }, { "xxx-large", 48 }, { NULL, 0 } };
        int v;
        if (css_parse_keyword(t, m, kw, &v)) { d->font_size = (float)v; return 1; }
        if (m == 1 && tok_is_ident(&t[0], "larger")) { d->font_size = base * 1.2f; return 1; }
        if (m == 1 && tok_is_ident(&t[0], "smaller")) { d->font_size = base / 1.2f; return 1; }
        CssLength l;
        if (!css_parse_length(t, m, CSS_LENF_PCT | CSS_LENF_NONNEG, &l)) return 0;
        d->font_size = l.px + l.em * base + l.pct * 0.01f * base;
        return 1;
    }
    case CSS_PROP_FONT_WEIGHT: {
        int w;
        if (!css_parse_font_weight(t, m, &w)) return 0;
        if (m == 1 && tok_is_ident(&t[0], "bolder")) w = (parent ? parent->font_weight : 400) >= 600 ? 900 : 700;
        if (m == 1 && tok_is_ident(&t[0], "lighter")) w = (parent ? parent->font_weight : 400) >= 700 ? 400 : 100;
        d->font_weight = (int16_t)w;
        return 1;
    }
    case CSS_PROP_FONT_STYLE: SET_KW(kw_font_style, font_style);
    case CSS_PROP_LINE_HEIGHT: {
        CssLength l;
        if (!css_parse_line_height(t, m, &l)) return 0;
        resolve_em_len(&l, font_px);
        if (l.kind == CSS_LEN_VALUE && l.pct != 0) { l.px += l.pct * 0.01f * font_px; l.pct = 0; }
        d->line_height = l;
        return 1;
    }
    case CSS_PROP_LETTER_SPACING: SET_LEN(CSS_LENF_NORMAL, letter_spacing);
    case CSS_PROP_TEXT_ALIGN:
        /* Chrome's UA `th { text-align: -internal-center }`: center, unless the parent's
         * text-align is not start (then inherited, e.g. #endTable's left) */
        if (m == 1 && tok_is_ident(&t[0], "-internal-center")) {
            d->text_align = parent && parent->text_align != CSS_TEXT_ALIGN_START ? parent->text_align : CSS_TEXT_ALIGN_CENTER;
            return 1;
        }
        SET_KW(kw_text_align, text_align);
    case CSS_PROP_TEXT_TRANSFORM: SET_KW(kw_text_transform, text_transform);
    case CSS_PROP_WHITE_SPACE: SET_KW(kw_white_space, white_space);
    case CSS_PROP_TEXT_WRAP: {
        /* text-wrap (css-text-4): the wrap mode is what white-space's nowrap / pre decide. A shorthand
         * value resets the mode (text-wrap: auto is wrap, as on v10's .lockedClassText, which sits in a
         * nowrap card), so only nowrap and a wrap keyword change white_space here; the other white-space
         * longhand (white-space-collapse) keeps its value. */
        int v_;
        if (!css_parse_keyword(t, m, kw_text_wrap, &v_)) return 0;
        if (v_) d->white_space = CSS_WHITE_SPACE_NOWRAP;
        else if (d->white_space == CSS_WHITE_SPACE_NOWRAP) d->white_space = CSS_WHITE_SPACE_NORMAL;
        else if (d->white_space == CSS_WHITE_SPACE_PRE) d->white_space = CSS_WHITE_SPACE_PRE_WRAP;
        return 1;
    }
    case CSS_PROP_OVERFLOW_WRAP: SET_KW(kw_overflow_wrap, overflow_wrap);
    case CSS_PROP_WORD_BREAK: SET_KW(kw_word_break, word_break);
    case CSS_PROP_VISIBILITY: SET_KW(kw_visibility, visibility);
    case CSS_PROP_CURSOR: {
        /* cursor: [url(...),]* keyword */
        int v;
        for (int i = m - 1; i >= 0; i--) { if (t[i].type == CSS_TOK_COMMA) continue; }
        if (!css_keyword_lookup(&t[m - 1], css_kw_cursor, &v)) return 0;
        for (int i = 0; i < m - 1; i++) if (t[i].type != CSS_TOK_COMMA && t[i].type != CSS_TOK_URL && t[i].type != CSS_TOK_NUMBER) return 0;
        d->cursor = (uint8_t)v;
        return 1;
    }
    case CSS_PROP_POINTER_EVENTS: SET_KW(kw_pointer_events, pointer_events);
    case CSS_PROP_USER_SELECT: SET_KW(kw_user_select, user_select);
    case CSS_PROP_DIRECTION: SET_KW(kw_direction, direction);
    case CSS_PROP_IMAGE_RENDERING: SET_KW(kw_image_rendering, image_rendering);
    case CSS_PROP_BORDER_COLLAPSE: SET_KW(kw_border_collapse, border_collapse);
    case CSS_PROP_LIST_STYLE_TYPE: SET_KW(kw_list_style, list_style_type);
    case CSS_PROP_BORDER_SPACING: {
        CssLength a, b; int u = 0;
        if (!css_parse_length_item(t, m, CSS_LENF_NONNEG, &a, &u)) return 0;
        b = a;
        if (u < m) { int u2 = 0; if (!css_parse_length_item(t + u, m - u, CSS_LENF_NONNEG, &b, &u2) || u + u2 != m) return 0; }
        resolve_em_len(&a, font_px); resolve_em_len(&b, font_px);
        d->border_spacing_x = a; d->border_spacing_y = b;
        return 1;
    }
    case CSS_PROP_TEXT_SHADOW: {
        CssShadowList s;
        if (!css_parse_text_shadow(t, m, &s)) return 0;
        d->text_shadow = s;
        return 1;
    }
    case CSS_PROP_DISPLAY: {
        int v;
        if (!css_parse_keyword(t, m, css_kw_display, &v)) return 0;
        d->display = (uint8_t)v;
        d->display_legacy_box = (uint8_t)(m == 1 && (tok_is_ident(&t[0], "-webkit-box") || tok_is_ident(&t[0], "-webkit-inline-box")));
        return 1;
    }
    case CSS_PROP_POSITION: SET_KW(css_kw_position, position);
    case CSS_PROP_FLOAT: SET_KW(kw_float, float_);
    case CSS_PROP_CLEAR: SET_KW(kw_clear, clear);
    case CSS_PROP_BOX_SIZING: SET_KW(kw_box_sizing, box_sizing);
    case CSS_PROP_TABLE_LAYOUT: SET_KW(kw_table_layout, table_layout);
    case CSS_PROP_OVERFLOW_X: SET_KW(css_kw_overflow, overflow_x);
    case CSS_PROP_OVERFLOW_Y: SET_KW(css_kw_overflow, overflow_y);
    case CSS_PROP_APPEARANCE: SET_KW(kw_appearance, appearance);
    case CSS_PROP_WILL_CHANGE: { uint8_t b; if (!css_parse_will_change(t, m, &b)) return 0; d->will_change = b; return 1; }
    case CSS_PROP_CONTAIN: { uint8_t b; if (!css_parse_contain(t, m, &b)) return 0; d->contain = b; return 1; }
    case CSS_PROP_MIX_BLEND_MODE: SET_KW(kw_mix_blend_mode, mix_blend_mode);
    case CSS_PROP_Z_INDEX: {
        int32_t z; int a;
        if (!css_parse_z_index(t, m, &z, &a)) return 0;
        d->z_index = z; d->z_index_auto = (uint8_t)a;
        return 1;
    }
    case CSS_PROP_OPACITY: { float o; if (!css_parse_opacity(t, m, &o)) return 0; d->opacity = o; return 1; }
    case CSS_PROP_TOP: case CSS_PROP_RIGHT: case CSS_PROP_BOTTOM: case CSS_PROP_LEFT:
        SET_LEN(CSS_LENF_PCT | CSS_LENF_AUTO, inset[id - CSS_PROP_TOP]);
    case CSS_PROP_WIDTH: SET_LEN(CSS_LENF_PCT | CSS_LENF_AUTO | CSS_LENF_CONTENT | CSS_LENF_NONNEG, width);
    case CSS_PROP_HEIGHT: SET_LEN(CSS_LENF_PCT | CSS_LENF_AUTO | CSS_LENF_CONTENT | CSS_LENF_NONNEG, height);
    case CSS_PROP_MIN_WIDTH: SET_LEN(CSS_LENF_PCT | CSS_LENF_AUTO | CSS_LENF_CONTENT | CSS_LENF_NONNEG, min_width);
    case CSS_PROP_MIN_HEIGHT: SET_LEN(CSS_LENF_PCT | CSS_LENF_AUTO | CSS_LENF_CONTENT | CSS_LENF_NONNEG, min_height);
    case CSS_PROP_MAX_WIDTH: SET_LEN(CSS_LENF_PCT | CSS_LENF_NONE | CSS_LENF_CONTENT | CSS_LENF_NONNEG, max_width);
    case CSS_PROP_MAX_HEIGHT: SET_LEN(CSS_LENF_PCT | CSS_LENF_NONE | CSS_LENF_CONTENT | CSS_LENF_NONNEG, max_height);
    case CSS_PROP_MARGIN_TOP: case CSS_PROP_MARGIN_RIGHT: case CSS_PROP_MARGIN_BOTTOM: case CSS_PROP_MARGIN_LEFT:
        SET_LEN(CSS_LENF_PCT | CSS_LENF_AUTO, margin[id - CSS_PROP_MARGIN_TOP]);
    case CSS_PROP_PADDING_TOP: case CSS_PROP_PADDING_RIGHT: case CSS_PROP_PADDING_BOTTOM: case CSS_PROP_PADDING_LEFT:
        SET_LEN(CSS_LENF_PCT | CSS_LENF_NONNEG, padding[id - CSS_PROP_PADDING_TOP]);
    case CSS_PROP_BORDER_TOP_WIDTH: case CSS_PROP_BORDER_RIGHT_WIDTH: case CSS_PROP_BORDER_BOTTOM_WIDTH: case CSS_PROP_BORDER_LEFT_WIDTH: {
        CssLength l; int u = 0;
        if (!css_parse_border_width(t, m, &l, &u) || u != m) return 0;
        resolve_em_len(&l, font_px);
        d->border_width[id - CSS_PROP_BORDER_TOP_WIDTH] = l.px;
        return 1;
    }
    case CSS_PROP_BORDER_TOP_STYLE: case CSS_PROP_BORDER_RIGHT_STYLE: case CSS_PROP_BORDER_BOTTOM_STYLE: case CSS_PROP_BORDER_LEFT_STYLE:
        SET_KW(css_kw_border_style, border_style[id - CSS_PROP_BORDER_TOP_STYLE]);
    case CSS_PROP_BORDER_TOP_COLOR: case CSS_PROP_BORDER_RIGHT_COLOR: case CSS_PROP_BORDER_BOTTOM_COLOR: case CSS_PROP_BORDER_LEFT_COLOR: {
        CssColor c; if (!css_parse_color(t, m, &c)) return 0; d->border_color[id - CSS_PROP_BORDER_TOP_COLOR] = c; return 1;
    }
    case CSS_PROP_BORDER_TOP_LEFT_RADIUS: case CSS_PROP_BORDER_TOP_RIGHT_RADIUS: case CSS_PROP_BORDER_BOTTOM_RIGHT_RADIUS: case CSS_PROP_BORDER_BOTTOM_LEFT_RADIUS: {
        /* accept `a` or `a b` (horizontal vertical); keep the horizontal radius */
        CssLength a, b; int u = 0;
        if (!css_parse_length_item(t, m, CSS_LENF_PCT | CSS_LENF_NONNEG, &a, &u)) return 0;
        if (u < m) { int u2 = 0; if (!css_parse_length_item(t + u, m - u, CSS_LENF_PCT | CSS_LENF_NONNEG, &b, &u2) || u + u2 != m) return 0; }
        resolve_em_len(&a, font_px);
        d->border_radius[id - CSS_PROP_BORDER_TOP_LEFT_RADIUS] = a;
        return 1;
    }
    case CSS_PROP_OUTLINE_WIDTH: {
        CssLength l; int u = 0;
        if (!css_parse_border_width(t, m, &l, &u) || u != m) return 0;
        resolve_em_len(&l, font_px);
        d->outline_width = l.px;
        return 1;
    }
    case CSS_PROP_OUTLINE_STYLE: {
        int v;
        if (m == 1 && tok_is_ident(&t[0], "auto")) v = CSS_BORDER_STYLE_AUTO;
        else if (!css_parse_keyword(t, m, css_kw_border_style, &v)) return 0;
        d->outline_style = (uint8_t)v;
        return 1;
    }
    case CSS_PROP_OUTLINE_COLOR: {
        CssColor c;
        if (m == 1 && tok_is_ident(&t[0], "invert")) c.kind = CSS_COLOR_CURRENT, c.r = c.g = c.b = c.a = 0;
        else if (!css_parse_color(t, m, &c)) return 0;
        d->outline_color = c;
        return 1;
    }
    case CSS_PROP_BACKGROUND_COLOR: { CssColor c; if (!css_parse_color(t, m, &c)) return 0; d->background_color = c; return 1; }
    case CSS_PROP_BACKGROUND_IMAGE: {
        CssImage img[CSS_MAX_BG_LAYERS]; int c;
        if (!css_parse_bg_image_list(t, m, img, CSS_MAX_BG_LAYERS, &c)) return 0;
        if (c > CSS_MAX_BG_LAYERS) c = CSS_MAX_BG_LAYERS;
        for (int i = 0; i < c; i++) d->bg_layers[i].image = img[i];
        for (int i = c; i < CSS_MAX_BG_LAYERS; i++) memset(&d->bg_layers[i].image, 0, sizeof(CssImage));
        d->bg_layer_count = (uint8_t)((c == 1 && img[0].kind == CSS_IMAGE_NONE) ? 0 : c);
        return 1;
    }
    case CSS_PROP_BACKGROUND_POSITION_X: case CSS_PROP_BACKGROUND_POSITION_Y: {
        CssLength l[CSS_MAX_BG_LAYERS]; int c;
        int axis = id == CSS_PROP_BACKGROUND_POSITION_Y;
        if (!css_parse_bg_position_list(t, m, axis, l, CSS_MAX_BG_LAYERS, &c)) return 0;
        if (c > CSS_MAX_BG_LAYERS) c = CSS_MAX_BG_LAYERS;
        for (int i = 0; i < CSS_MAX_BG_LAYERS; i++) {
            CssLength v = l[i % c];
            resolve_em_len(&v, font_px);
            if (axis) d->bg_layers[i].y = v; else d->bg_layers[i].x = v;
        }
        return 1;
    }
    case CSS_PROP_BACKGROUND_SIZE: {
        CssBgSize s[CSS_MAX_BG_LAYERS]; int c;
        if (!css_parse_bg_size_list(t, m, s, CSS_MAX_BG_LAYERS, &c)) return 0;
        if (c > CSS_MAX_BG_LAYERS) c = CSS_MAX_BG_LAYERS;
        for (int i = 0; i < CSS_MAX_BG_LAYERS; i++) {
            CssBgSize v = s[i % c];
            resolve_em_len(&v.w, font_px); resolve_em_len(&v.h, font_px);
            d->bg_layers[i].size = v;
        }
        return 1;
    }
    case CSS_PROP_BACKGROUND_REPEAT: {
        uint8_t rx[CSS_MAX_BG_LAYERS], ry[CSS_MAX_BG_LAYERS]; int c;
        if (!css_parse_bg_repeat_list(t, m, rx, ry, CSS_MAX_BG_LAYERS, &c)) return 0;
        if (c > CSS_MAX_BG_LAYERS) c = CSS_MAX_BG_LAYERS;
        for (int i = 0; i < CSS_MAX_BG_LAYERS; i++) { d->bg_layers[i].repeat_x = rx[i % c]; d->bg_layers[i].repeat_y = ry[i % c]; }
        return 1;
    }
    case CSS_PROP_TRANSFORM: {
        CssTransform tr;
        if (!css_parse_transform(t, m, &tr)) return 0;
        for (int i = 0; i < tr.count; i++) { resolve_em_len(&tr.ops[i].tx, font_px); resolve_em_len(&tr.ops[i].ty, font_px); }
        d->transform = tr;
        return 1;
    }
    case CSS_PROP_TRANSFORM_ORIGIN: {
        CssLength x, y;
        if (!css_parse_transform_origin(t, m, &x, &y)) return 0;
        resolve_em_len(&x, font_px); resolve_em_len(&y, font_px);
        d->transform_origin_x = x; d->transform_origin_y = y;
        return 1;
    }
    case CSS_PROP_BOX_SHADOW: {
        CssShadow s[CSS_MAX_BOX_SHADOWS]; int c;
        if (!css_parse_box_shadow_list(t, m, s, CSS_MAX_BOX_SHADOWS, &c)) return 0;
        memset(&d->box_shadow, 0, sizeof(d->box_shadow));
        memset(d->box_shadow_more, 0, sizeof(d->box_shadow_more));
        if (c) d->box_shadow = s[0];
        for (int i = 1; i < c; i++) d->box_shadow_more[i - 1] = s[i];
        d->box_shadow_count = (uint8_t)c;
        return 1;
    }
    case CSS_PROP_FILTER: {
        CssFilterOp f[CSS_MAX_FILTER_OPS]; int c;
        if (!css_parse_filter(t, m, f, CSS_MAX_FILTER_OPS, &c)) return 0;
        memset(d->filters, 0, sizeof(d->filters));
        for (int i = 0; i < c; i++) d->filters[i] = f[i];
        d->filter_count = (uint8_t)c;
        return 1;
    }
    case CSS_PROP_CLIP_PATH: {
        CssClipPath cp;
        if (!css_parse_clip_path(t, m, &cp)) return 0;
        for (int i = 0; i < cp.point_count; i++) { resolve_em_len(&cp.x[i], font_px); resolve_em_len(&cp.y[i], font_px); }
        d->clip_path = cp;
        return 1;
    }
    case CSS_PROP_OBJECT_FIT: SET_KW(kw_object_fit, object_fit);
    case CSS_PROP_CONTENT: { CssContent c; if (!css_parse_content(t, m, &c)) return 0; d->content = c; return 1; }
    case CSS_PROP_TEXT_DECORATION: { uint8_t b; if (!css_parse_text_decoration(t, m, &b)) return 0; d->text_decoration = b; return 1; }
    case CSS_PROP_TEXT_OVERFLOW: {
        static const CssKeyword kw[] = { { "clip", CSS_TEXT_OVERFLOW_CLIP }, { "ellipsis", CSS_TEXT_OVERFLOW_ELLIPSIS }, { NULL, 0 } };
        int v;
        if (m == 2 && css_keyword_lookup(&t[0], kw, &v)) { int v2; if (!css_keyword_lookup(&t[1], kw, &v2)) return 0; d->text_overflow = (uint8_t)v2; return 1; }
        SET_KW(kw, text_overflow);
    }
    case CSS_PROP_VERTICAL_ALIGN: {
        uint8_t kw = 0; CssLength l; memset(&l, 0, sizeof(l));
        if (!css_parse_vertical_align(t, m, &kw, &l)) return 0;
        resolve_em_len(&l, font_px);
        d->vertical_align = kw; d->vertical_align_length = l;
        return 1;
    }
    case CSS_PROP_FLEX_DIRECTION: SET_KW(kw_flex_direction, flex_direction);
    case CSS_PROP_FLEX_WRAP: SET_KW(kw_flex_wrap, flex_wrap);
    case CSS_PROP_JUSTIFY_CONTENT: SET_KW(kw_justify_content, justify_content);
    case CSS_PROP_ALIGN_CONTENT: SET_KW(kw_justify_content, align_content);
    case CSS_PROP_ALIGN_ITEMS: SET_KW(kw_align_items, align_items);
    case CSS_PROP_FLEX_GROW: { float f; if (!css_parse_number(t, m, &f) || f < 0) return 0; d->flex_grow = f; return 1; }
    case CSS_PROP_FLEX_SHRINK: { float f; if (!css_parse_number(t, m, &f) || f < 0) return 0; d->flex_shrink = f; return 1; }
    case CSS_PROP_FLEX_BASIS: SET_LEN(CSS_LENF_PCT | CSS_LENF_AUTO | CSS_LENF_CONTENT | CSS_LENF_NONNEG, flex_basis);
    case CSS_PROP_GRID_TEMPLATE_COLUMNS: {
        CssTrack tr[CSS_MAX_GRID_TRACKS]; int c; CssGridNames nm;
        if (!css_parse_grid_track_list(t, m, tr, CSS_MAX_GRID_TRACKS, &c, &nm)) return 0;
        memset(d->grid_columns, 0, sizeof(d->grid_columns));
        for (int i = 0; i < c; i++) { resolve_em_len(&tr[i].size, font_px); resolve_em_len(&tr[i].max, font_px); d->grid_columns[i] = tr[i]; }
        d->grid_column_count = (uint8_t)c;
        d->grid_names[CSS_GRID_AXIS_COLUMN] = nm;
        return 1;
    }
    case CSS_PROP_GRID_TEMPLATE_ROWS: {
        CssTrack tr[CSS_MAX_GRID_TRACKS]; int c; CssGridNames nm;
        if (!css_parse_grid_track_list(t, m, tr, CSS_MAX_GRID_TRACKS, &c, &nm)) return 0;
        memset(d->grid_rows, 0, sizeof(d->grid_rows));
        for (int i = 0; i < c; i++) { resolve_em_len(&tr[i].size, font_px); resolve_em_len(&tr[i].max, font_px); d->grid_rows[i] = tr[i]; }
        d->grid_row_count = (uint8_t)c;
        d->grid_names[CSS_GRID_AXIS_ROW] = nm;
        return 1;
    }
    case CSS_PROP_GRID_TEMPLATE_AREAS: {
        const char *a; uint8_t r, c;
        if (!parse_grid_areas(t, m, &a, &r, &c)) return 0;
        d->grid_areas = a; d->grid_area_rows = r; d->grid_area_cols = c;
        return 1;
    }
    case CSS_PROP_GRID_AUTO_ROWS: case CSS_PROP_GRID_AUTO_COLUMNS: {
        CssTrack tr[CSS_MAX_GRID_AUTO_TRACKS]; int c;
        int ax = id == CSS_PROP_GRID_AUTO_ROWS ? CSS_GRID_AXIS_ROW : CSS_GRID_AXIS_COLUMN;
        memset(tr, 0, sizeof(tr));
        if (!parse_grid_auto_tracks(t, m, tr, &c)) return 0;
        for (int i = 0; i < c; i++) { resolve_em_len(&tr[i].size, font_px); resolve_em_len(&tr[i].max, font_px); }
        memcpy(d->grid_auto_tracks[ax], tr, sizeof(tr));
        d->grid_auto_count[ax] = (uint8_t)c;
        return 1;
    }
    case CSS_PROP_GRID_AUTO_FLOW: { uint8_t f; if (!parse_grid_auto_flow(t, m, &f)) return 0; d->grid_auto_flow = f; return 1; }
    case CSS_PROP_GRID_ROW_START: case CSS_PROP_GRID_COLUMN_START: case CSS_PROP_GRID_ROW_END: case CSS_PROP_GRID_COLUMN_END: {
        CssGridLine g;
        if (!parse_grid_line(t, m, &g)) return 0;
        d->grid_lines[id - CSS_PROP_GRID_ROW_START] = g;
        return 1;
    }
    case CSS_PROP_JUSTIFY_ITEMS: SET_KW(kw_justify_items, justify_items);
    case CSS_PROP_JUSTIFY_SELF: SET_KW(kw_self_align, justify_self);
    case CSS_PROP_ALIGN_SELF: SET_KW(kw_self_align, align_self);
    case CSS_PROP_ROW_GAP: SET_LEN(CSS_LENF_PCT | CSS_LENF_NORMAL | CSS_LENF_NONNEG, row_gap);
    case CSS_PROP_COLUMN_GAP: SET_LEN(CSS_LENF_PCT | CSS_LENF_NORMAL | CSS_LENF_NONNEG, column_gap);
    case CSS_PROP_ZOOM: {
        /* <number> | <percentage> | normal | reset (0 behaves as 1, like Chrome);
         * stores the element's own zoom, the cascade turns it into the effective one */
        float z;
        if (m != 1) return 0;
        if (tok_is_ident(&t[0], "normal") || tok_is_ident(&t[0], "reset")) z = 1.0f;
        else if (t[0].type == CSS_TOK_NUMBER) z = (float)t[0].num;
        else if (t[0].type == CSS_TOK_PERCENTAGE) z = (float)t[0].num * 0.01f;
        else return 0;
        if (z < 0.0f) return 0;
        d->zoom = z == 0.0f ? 1.0f : z;
        return 1;
    }
    case CSS_PROP_ANIMATION_NAME: {
        const char *names[CSS_MAX_ANIMATIONS]; int c;
        if (!css_parse_animation_names(t, m, names, CSS_MAX_ANIMATIONS, &c)) return 0;
        if (c > CSS_MAX_ANIMATIONS) c = CSS_MAX_ANIMATIONS;
        for (int i = 0; i < CSS_MAX_ANIMATIONS; i++) d->animations[i].name = i < c ? names[i] : NULL;
        d->animation_count = (uint8_t)c;
        return 1;
    }
    case CSS_PROP_ANIMATION_DURATION: case CSS_PROP_ANIMATION_DELAY: {
        float v[CSS_MAX_ANIMATIONS]; int c;
        if (!css_parse_time_list(t, m, v, CSS_MAX_ANIMATIONS, &c)) return 0;
        if (c > CSS_MAX_ANIMATIONS) c = CSS_MAX_ANIMATIONS;
        for (int i = 0; i < CSS_MAX_ANIMATIONS; i++) {
            if (id == CSS_PROP_ANIMATION_DURATION) { if (v[i % c] < 0) return 0; d->animations[i].duration_s = v[i % c]; }
            else d->animations[i].delay_s = v[i % c];
        }
        return 1;
    }
    case CSS_PROP_ANIMATION_TIMING_FUNCTION: {
        CssTiming v[CSS_MAX_ANIMATIONS]; int c;
        if (!css_parse_timing_list(t, m, v, CSS_MAX_ANIMATIONS, &c)) return 0;
        if (c > CSS_MAX_ANIMATIONS) c = CSS_MAX_ANIMATIONS;
        for (int i = 0; i < CSS_MAX_ANIMATIONS; i++) d->animations[i].timing = v[i % c];
        return 1;
    }
    case CSS_PROP_ANIMATION_ITERATION_COUNT: {
        float v[CSS_MAX_ANIMATIONS]; int c;
        if (!css_parse_iteration_list(t, m, v, CSS_MAX_ANIMATIONS, &c)) return 0;
        if (c > CSS_MAX_ANIMATIONS) c = CSS_MAX_ANIMATIONS;
        for (int i = 0; i < CSS_MAX_ANIMATIONS; i++) d->animations[i].iterations = v[i % c];
        return 1;
    }
    case CSS_PROP_ANIMATION_DIRECTION: case CSS_PROP_ANIMATION_FILL_MODE: case CSS_PROP_ANIMATION_PLAY_STATE: {
        uint8_t v[CSS_MAX_ANIMATIONS]; int c;
        const CssKeyword *tab = id == CSS_PROP_ANIMATION_DIRECTION ? kw_anim_direction : id == CSS_PROP_ANIMATION_FILL_MODE ? kw_anim_fill : kw_anim_play;
        if (!css_parse_keyword_list(t, m, tab, v, CSS_MAX_ANIMATIONS, &c)) return 0;
        if (c > CSS_MAX_ANIMATIONS) c = CSS_MAX_ANIMATIONS;
        for (int i = 0; i < CSS_MAX_ANIMATIONS; i++) {
            if (id == CSS_PROP_ANIMATION_DIRECTION) d->animations[i].direction = v[i % c];
            else if (id == CSS_PROP_ANIMATION_FILL_MODE) d->animations[i].fill = v[i % c];
            else d->animations[i].play_state = v[i % c];
        }
        return 1;
    }
    case CSS_PROP_TRANSITION_PROPERTY: {
        int16_t v[CSS_MAX_TRANSITIONS]; int c;
        if (!css_parse_transition_props(t, m, v, CSS_MAX_TRANSITIONS, &c)) return 0;
        if (c > CSS_MAX_TRANSITIONS) c = CSS_MAX_TRANSITIONS;
        for (int i = 0; i < CSS_MAX_TRANSITIONS; i++) d->transitions[i].prop = i < c ? v[i] : -1;
        d->transition_count = (uint8_t)((c == 1 && v[0] == -1) ? 0 : c);
        return 1;
    }
    case CSS_PROP_TRANSITION_DURATION: case CSS_PROP_TRANSITION_DELAY: {
        float v[CSS_MAX_TRANSITIONS]; int c;
        if (!css_parse_time_list(t, m, v, CSS_MAX_TRANSITIONS, &c)) return 0;
        if (c > CSS_MAX_TRANSITIONS) c = CSS_MAX_TRANSITIONS;
        for (int i = 0; i < CSS_MAX_TRANSITIONS; i++) {
            if (id == CSS_PROP_TRANSITION_DURATION) { if (v[i % c] < 0) return 0; d->transitions[i].duration_s = v[i % c]; }
            else d->transitions[i].delay_s = v[i % c];
        }
        return 1;
    }
    case CSS_PROP_TRANSITION_TIMING_FUNCTION: {
        CssTiming v[CSS_MAX_TRANSITIONS]; int c;
        if (!css_parse_timing_list(t, m, v, CSS_MAX_TRANSITIONS, &c)) return 0;
        if (c > CSS_MAX_TRANSITIONS) c = CSS_MAX_TRANSITIONS;
        for (int i = 0; i < CSS_MAX_TRANSITIONS; i++) d->transitions[i].timing = v[i % c];
        return 1;
    }
    case CSS_PROP_TEXT_FILL_COLOR: { CssColor c; if (!css_parse_color(t, m, &c)) return 0; d->text_fill_color = c; return 1; }
    case CSS_PROP_ACCENT_COLOR: {
        CssColor c;
        if (m == 1 && tok_is_ident(&t[0], "auto")) { memset(&d->accent_color, 0, sizeof d->accent_color); d->accent_color_auto = 1; return 1; }
        if (!css_parse_color(t, m, &c)) return 0;
        d->accent_color = c; d->accent_color_auto = 0;
        return 1;
    }
    case CSS_PROP_BACKDROP_FILTER: {
        CssFilterOp f[CSS_MAX_FILTER_OPS]; int c;
        if (!css_parse_filter(t, m, f, CSS_MAX_FILTER_OPS, &c)) return 0;
        memset(d->backdrop_filters, 0, sizeof(d->backdrop_filters));
        for (int i = 0; i < c; i++) d->backdrop_filters[i] = f[i];
        d->backdrop_filter_count = (uint8_t)c;
        return 1;
    }
    case CSS_PROP_PERSPECTIVE: SET_LEN(CSS_LENF_NONE | CSS_LENF_NONNEG, perspective);
    case CSS_PROP_PERSPECTIVE_ORIGIN: {
        CssLength x, y;
        if (!css_parse_position_xy(t, m, &x, &y)) return 0;
        resolve_em_len(&x, font_px); resolve_em_len(&y, font_px);
        d->perspective_origin_x = x; d->perspective_origin_y = y;
        return 1;
    }
    case CSS_PROP_TRANSFORM_STYLE: SET_KW(kw_transform_style, transform_style);
    case CSS_PROP_BACKFACE_VISIBILITY: SET_KW(kw_backface, backface_visibility);
    case CSS_PROP_MASK_IMAGE: {
        CssImage img[CSS_MAX_MASK_LAYERS]; int c;
        if (!css_parse_bg_image_list(t, m, img, CSS_MAX_MASK_LAYERS, &c)) return 0;
        if (c > CSS_MAX_MASK_LAYERS) c = CSS_MAX_MASK_LAYERS;
        for (int i = 0; i < c; i++) d->mask_layers[i].image = img[i];
        for (int i = c; i < CSS_MAX_MASK_LAYERS; i++) memset(&d->mask_layers[i].image, 0, sizeof(CssImage));
        int any = 0;
        for (int i = 0; i < c; i++) any |= img[i].kind != CSS_IMAGE_NONE;
        d->mask_layer_count = (uint8_t)(any ? c : 0);
        return 1;
    }
    case CSS_PROP_MASK_POSITION: {
        int st[16], ln[16];
        int c = split_commas(t, m, st, ln, 16);
        if (c <= 0) return 0;
        CssLength x[CSS_MAX_MASK_LAYERS], y[CSS_MAX_MASK_LAYERS];
        for (int i = 0; i < c; i++) {
            CssLength px, py; int used = 0;
            if (!parse_position(t + st[i], ln[i], &px, &py, &used) || used != ln[i]) return 0;
            if (i < CSS_MAX_MASK_LAYERS) { resolve_em_len(&px, font_px); resolve_em_len(&py, font_px); x[i] = px; y[i] = py; }
        }
        if (c > CSS_MAX_MASK_LAYERS) c = CSS_MAX_MASK_LAYERS;
        for (int i = 0; i < CSS_MAX_MASK_LAYERS; i++) { d->mask_layers[i].x = x[i % c]; d->mask_layers[i].y = y[i % c]; }
        return 1;
    }
    case CSS_PROP_MASK_SIZE: {
        CssBgSize sz[CSS_MAX_MASK_LAYERS]; int c;
        if (!css_parse_bg_size_list(t, m, sz, CSS_MAX_MASK_LAYERS, &c)) return 0;
        if (c > CSS_MAX_MASK_LAYERS) c = CSS_MAX_MASK_LAYERS;
        for (int i = 0; i < CSS_MAX_MASK_LAYERS; i++) {
            CssBgSize v = sz[i % c];
            resolve_em_len(&v.w, font_px); resolve_em_len(&v.h, font_px);
            d->mask_layers[i].size = v;
        }
        return 1;
    }
    case CSS_PROP_MASK_REPEAT: {
        uint8_t rx[CSS_MAX_MASK_LAYERS], ry[CSS_MAX_MASK_LAYERS]; int c;
        if (!css_parse_bg_repeat_list(t, m, rx, ry, CSS_MAX_MASK_LAYERS, &c)) return 0;
        if (c > CSS_MAX_MASK_LAYERS) c = CSS_MAX_MASK_LAYERS;
        for (int i = 0; i < CSS_MAX_MASK_LAYERS; i++) { d->mask_layers[i].repeat_x = rx[i % c]; d->mask_layers[i].repeat_y = ry[i % c]; }
        return 1;
    }
    case CSS_PROP_MASK_MODE: {
        uint8_t v[8]; int c;
        if (!css_parse_keyword_list(t, m, kw_mask_mode, v, 8, &c) || c <= 0) return 0;
        d->mask_mode = v[0];
        return 1;
    }
    case CSS_PROP_BACKGROUND_CLIP: case CSS_PROP_BACKGROUND_ORIGIN: {
        uint8_t v[8]; int c;
        if (!css_parse_keyword_list(t, m, kw_bg_box, v, 8, &c) || c <= 0) return 0;
        if (id == CSS_PROP_BACKGROUND_ORIGIN) { if (v[0] == CSS_BG_BOX_TEXT) return 0; d->background_origin = v[0]; }
        else d->background_clip = v[0];   /* the engine paints one clip for all layers: the first */
        return 1;
    }
    case CSS_PROP_OUTLINE_OFFSET: {
        CssLength l;
        if (!css_parse_length(t, m, 0, &l)) return 0;
        resolve_em_len(&l, font_px);
        d->outline_offset = l.px;
        return 1;
    }
    case CSS_PROP_ASPECT_RATIO: {
        /* auto | <ratio> | auto && <ratio>; <ratio> = <number [0,inf]> [ / <number [0,inf]> ]? */
        int i = 0, has_auto = 0, has_ratio = 0;
        float ratio = 0;
        while (i < m) {
            if (!has_auto && tok_is_ident(&t[i], "auto")) { has_auto = 1; i++; continue; }
            if (!has_ratio && t[i].type == CSS_TOK_NUMBER && t[i].num >= 0) {
                float w = t[i].num, h = 1;
                i++;
                if (i < m && tok_is_delim(&t[i], '/')) {
                    if (i + 1 >= m || t[i + 1].type != CSS_TOK_NUMBER || t[i + 1].num < 0) return 0;
                    h = t[i + 1].num;
                    i += 2;
                }
                ratio = (w > 0 && h > 0) ? w / h : 0;   /* a degenerate ratio behaves as auto */
                has_ratio = 1;
                continue;
            }
            return 0;
        }
        if (!has_auto && !has_ratio) return 0;
        d->aspect_ratio = ratio;
        d->aspect_ratio_auto = (uint8_t)(has_auto || ratio == 0);
        return 1;
    }
    case CSS_PROP_LINE_CLAMP: {
        if (m == 1 && tok_is_ident(&t[0], "none")) { d->line_clamp = 0; return 1; }
        int v;
        if (!css_parse_integer(t, m, &v) || v <= 0) return 0;
        d->line_clamp = (uint16_t)(v > 65535 ? 65535 : v);
        return 1;
    }
    case CSS_PROP_BOX_ORIENT: SET_KW(kw_box_orient, box_orient);
    case CSS_PROP_FONT_FEATURE_SETTINGS: case CSS_PROP_FONT_SMOOTHING: case CSS_PROP_TOUCH_CALLOUT:
    case CSS_PROP_USER_DRAG: case CSS_PROP_TEXT_RENDERING:
        return 1; /* accepted, no state */
    default:
        return 0;
    }
}

#undef SET_KW
#undef SET_LEN

/* ========================================================================= */
/* @property registrations (CSS Properties and Values API 1)                 */
/* ========================================================================= */

int css_parse_syntax(const char *text, size_t len)
{
    const char *s = kn_str_trim(text, &len);
    if (len >= 2 && (s[0] == '\'' || s[0] == '"') && s[len - 1] == s[0]) { s++; len -= 2; s = kn_str_trim(s, &len); }
    static const struct { const char *name; int syntax; } k[] = {
        { "*", CSS_SYNTAX_UNIVERSAL }, { "<number>", CSS_SYNTAX_NUMBER }, { "<integer>", CSS_SYNTAX_INTEGER },
        { "<length>", CSS_SYNTAX_LENGTH }, { "<percentage>", CSS_SYNTAX_PERCENTAGE },
        { "<length-percentage>", CSS_SYNTAX_LENGTH_PERCENTAGE }, { "<color>", CSS_SYNTAX_COLOR },
        { "<angle>", CSS_SYNTAX_ANGLE }, { "<time>", CSS_SYNTAX_TIME },
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        if (kn_str_ieq(s, len, k[i].name)) return k[i].syntax;
    return -1;
}

int css_typed_value_parse(int syntax, const char *text, size_t len, float font_px, CssTypedValue *out)
{
    CssToken t[CSS_MAX_TOKENS];
    int n = css_tokens_strip_ws(t, css_tokenize(text, len, t, CSS_MAX_TOKENS));
    CssTypedValue v;
    memset(&v, 0, sizeof v);
    v.syntax = (uint8_t)syntax;
    if (n <= 0) return 0;
    switch (syntax) {
    case CSS_SYNTAX_NUMBER: if (!css_parse_number(t, n, &v.num)) return 0; break;
    case CSS_SYNTAX_INTEGER: { int i; if (!css_parse_integer(t, n, &i)) return 0; v.num = (float)i; break; }
    case CSS_SYNTAX_LENGTH: case CSS_SYNTAX_LENGTH_PERCENTAGE: case CSS_SYNTAX_PERCENTAGE: {
        CssLength l;
        if (!css_parse_length(t, n, syntax == CSS_SYNTAX_LENGTH ? 0 : CSS_LENF_PCT, &l) || l.kind != CSS_LEN_VALUE) return 0;
        if (syntax == CSS_SYNTAX_PERCENTAGE) {
            if (l.px != 0 || l.em != 0) return 0;
            v.num = l.pct;
        }
        css_length_resolve_em(&l, font_px > 0 ? font_px : 16.0f);
        v.len = l;
        break;
    }
    case CSS_SYNTAX_COLOR: if (!css_parse_color(t, n, &v.color)) return 0; break;
    case CSS_SYNTAX_ANGLE: if (!css_parse_angle_deg(t, n, &v.num)) return 0; break;
    case CSS_SYNTAX_TIME: if (!css_parse_time_s(t, n, &v.num)) return 0; break;
    default: return 0;
    }
    *out = v;
    return 1;
}

void css_typed_value_lerp(const CssTypedValue *a, const CssTypedValue *b, float t, CssTypedValue *out)
{
    CssTypedValue r = *a;
    if (a->syntax != b->syntax) { *out = t < 0.5f ? *a : *b; return; }
    r.num = lerp1(a->num, b->num, t);
    if (a->syntax == CSS_SYNTAX_INTEGER) r.num = roundf(r.num);
    len_lerp(&a->len, &b->len, t, &r.len);
    if (a->syntax == CSS_SYNTAX_COLOR) {
        if (a->color.kind == CSS_COLOR_RGBA && b->color.kind == CSS_COLOR_RGBA) {
            /* a non-legacy endpoint (color-mix()) makes the pair interpolate in Oklab */
            const int ool = a->color.nonlegacy || b->color.nonlegacy;
            r.color = css_color_mix(a->color, b->color, 1.0f - t, 1.0f, ool ? CSS_COLOR_SPACE_OKLAB : CSS_COLOR_SPACE_SRGB);
            r.color.nonlegacy = (uint8_t) ool;
        }
        else r.color = t < 0.5f ? a->color : b->color;
    }
    *out = r;
}

size_t css_typed_value_format(const CssTypedValue *v, char *out, size_t cap)
{
    int w = 0;
    switch (v->syntax) {
    case CSS_SYNTAX_NUMBER: case CSS_SYNTAX_INTEGER: w = snprintf(out, cap, "%g", (double)v->num); break;
    case CSS_SYNTAX_PERCENTAGE: w = snprintf(out, cap, "%g%%", (double)v->num); break;
    case CSS_SYNTAX_LENGTH: case CSS_SYNTAX_LENGTH_PERCENTAGE:
        if (v->len.pct != 0 && v->len.px != 0) w = snprintf(out, cap, "calc(%gpx + %g%%)", (double)v->len.px, (double)v->len.pct);
        else if (v->len.pct != 0) w = snprintf(out, cap, "%g%%", (double)v->len.pct);
        else w = snprintf(out, cap, "%gpx", (double)v->len.px);
        break;
    case CSS_SYNTAX_COLOR:
        if (v->color.kind == CSS_COLOR_CURRENT) w = snprintf(out, cap, "currentcolor");
        else w = snprintf(out, cap, "rgba(%u, %u, %u, %g)", v->color.r, v->color.g, v->color.b, v->color.a / 255.0);
        break;
    case CSS_SYNTAX_ANGLE: w = snprintf(out, cap, "%gdeg", (double)v->num); break;
    case CSS_SYNTAX_TIME: w = snprintf(out, cap, "%gs", (double)v->num); break;
    default: if (cap) out[0] = 0; return 0;
    }
    return w < 0 ? 0 : (size_t)w;
}

int css_property_register(const char *name, size_t name_len, const char *syntax, size_t syntax_len,
                          const char *inherits, size_t inherits_len, const char *initial, size_t initial_len)
{
    name = kn_str_trim(name, &name_len);
    if (name_len < 3 || name[0] != '-' || name[1] != '-' || !syntax) return CSS_PROP_INVALID;
    const int syn = css_parse_syntax(syntax, syntax_len);
    if (syn < 0 || !inherits) return CSS_PROP_INVALID;
    inherits = kn_str_trim(inherits, &inherits_len);
    int inh;
    if (kn_str_ieq(inherits, inherits_len, "true")) inh = 1;
    else if (kn_str_ieq(inherits, inherits_len, "false")) inh = 0;
    else return CSS_PROP_INVALID;
    const char *init_atom = NULL;
    if (initial) {
        initial = kn_str_trim(initial, &initial_len);
        if (initial_len) init_atom = kn_atom(initial, initial_len);
    }
    if (syn != CSS_SYNTAX_UNIVERSAL) {
        CssTypedValue v;
        /* the initial value must be computationally independent: no em / var() */
        if (!init_atom || !css_typed_value_parse(syn, init_atom, strlen(init_atom), 16.0f, &v)) return CSS_PROP_INVALID;
        if (memchr(init_atom, 'e', strlen(init_atom)) && strstr(init_atom, "em")) return CSS_PROP_INVALID;
    }
    const int id = css_prop_lookup(name, name_len);
    if (!CSS_PROP_IS_CUSTOM(id)) return CSS_PROP_INVALID;
    CssRegisteredProperty *r = &g_registered[id - CSS_PROP_CUSTOM_BASE];
    r->name = css_prop_name(id);
    r->syntax = (uint8_t)syn;
    r->inherits = (uint8_t)inh;
    r->initial = init_atom;
    return id;
}

const CssRegisteredProperty *css_property_registration(int id)
{
    if (!CSS_PROP_IS_CUSTOM(id)) return NULL;
    const CssRegisteredProperty *r = &g_registered[id - CSS_PROP_CUSTOM_BASE];
    return r->name ? r : NULL;
}

void css_property_registry_clear(void)
{
    memset(g_registered, 0, sizeof g_registered);
}
