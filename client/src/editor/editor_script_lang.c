// KrunkScript code window, language side: the krunkscript CodeMirror mode, the hint list,
// brackets / indentation / comments and the script store (header comment: editor_script_lang.h).
#include "editor_script_lang.h"
#include "editor_model.h"

#include <ks.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_style_names[EKS__COUNT] = {"", "comment", "keyword", "def", "type", "atom", "variable", "number", "string"};

const char *ed_ks_style_name(const int style) { return style > 0 && style < EKS__COUNT ? k_style_names[style] : ""; }

// ---------------------------------------------------------------------------
// the krunkscript simple mode

static const char *const k_keywords[] = {"public", "static", "action", "break", "continue", "return", "if", "else", "for", "in",
                                         "while", "addTo", "remove", "lengthOf", "notEmpty", "hasProp", "toStr", "toNum", NULL};
static const char *const k_types[] = {"bool", "num", "str", "obj", NULL};
static const char *const k_atoms[] = {"GAME", "true", "false", NULL}; // initCodeMirror(["GAME"]) + true / false

static int is_word(const unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
static int is_ident_start(const unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_digit(const unsigned char c) { return c >= '0' && c <= '9'; }
static int is_space(const unsigned char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r'; }

static int in_list(const char *const *list, const char *s, const size_t n) {
    for (int i = 0; list[i]; i++) {
        if (strlen(list[i]) == n && !memcmp(list[i], s, n)) return 1;
    }
    return 0;
}

// One rule match at s[p] of the line [p, e). Returns the matched length (0 = no rule: one
// character of untokenized text) and the style of its first group; *paren = 1 when the match
// is an identifier + "(" (the def rule), *bracket = the bracket char of the bare bracket rules.
static int match_rule(const char *s, const int p, const int e, int *style, int *ident_len, char *bracket) {
    *style = EKS_NONE;
    *ident_len = 0;
    *bracket = 0;
    // comment: \s*#[^\n\r]*
    int q = p;
    while (q < e && is_space((unsigned char) s[q])) q++;
    if (q < e && s[q] == '#') {
        while (q < e && s[q] != '\r') q++;
        *style = EKS_COMMENT;
        return q - p;
    }
    const unsigned char c = (unsigned char) s[p];
    if (is_ident_start(c)) {
        int w = p;
        while (w < e && is_word((unsigned char) s[w])) w++;
        const size_t n = (size_t) (w - p);
        if (in_list(k_keywords, s + p, n)) *style = EKS_KEYWORD;
        else if (w < e && s[w] == '(') {
            *style = EKS_DEF;
            *ident_len = (int) n;
            return (int) n + 1; // the "(" is the rule's null group
        } else if (in_list(k_types, s + p, n)) *style = EKS_TYPE;
        else if (in_list(k_atoms, s + p, n)) *style = EKS_ATOM;
        else *style = EKS_VARIABLE;
        return (int) n;
    }
    if (is_digit(c)) {
        int w = p;
        while (w < e && is_digit((unsigned char) s[w])) w++;
        if (w < e && s[w] == '.') {
            w++;
            while (w < e && is_digit((unsigned char) s[w])) w++;
        }
        *style = EKS_NUMBER;
        return w - p;
    }
    if (c == '"' || c == '\'') {
        int w = p + 1;
        while (w < e && s[w] != (char) c && s[w] != '\r') w++;
        if (w < e && s[w] == (char) c) {
            *style = EKS_STRING;
            return w + 1 - p;
        }
    }
    if (strchr("{[(}])", c) && c) {
        *bracket = (char) c;
        return 1;
    }
    return 0;
}

static int utf8_step(const char *s, const int p, const int e) {
    int q = p + 1;
    while (q < e && ((unsigned char) s[q] & 0xC0) == 0x80) q++;
    return q;
}

// Walks the text with the mode. tok (may be NULL) receives styled tokens; brk (may be NULL)
// is called for every bracket the indentation stack sees.
typedef void (*BracketFn)(void *user, int pos, char ch, int line_start);

static int walk(const char *src, const size_t len, const int stop, EdKsToken *out, const int max, BracketFn brk, void *user) {
    int count = 0, ls = 0;
    const int end = stop >= 0 && stop < (int) len ? stop : (int) len;
    while (ls <= end) {
        int le = ls;
        while (le < (int) len && src[le] != '\n') le++;
        const int lim = le < end ? le : end;
        for (int p = ls; p < lim;) {
            int style, ident, n;
            char bracket;
            n = match_rule(src, p, le, &style, &ident, &bracket);
            if (n > 0 && p + n > lim && style != EKS_NONE && stop >= 0) n = lim - p; // cut at the stop
            if (n == 0) {
                p = utf8_step(src, p, lim);
                continue;
            }
            if (style != EKS_NONE) {
                if (out && count < max) {
                    out[count].start = p;
                    out[count].end = p + (style == EKS_DEF ? ident : n);
                    out[count].style = style;
                }
                count++;
            }
            // intentional difference from v7.1.4: the def rule's "(" counts as an opening
            // bracket, so the ")" of a call no longer dedents the following line
            if (brk && style == EKS_DEF && p + ident < lim) brk(user, p + ident, '(', ls);
            if (brk && bracket) brk(user, p, bracket, ls);
            p += n;
        }
        if (le >= end) break;
        ls = le + 1;
    }
    return count;
}

int ed_ks_tokenize(const char *src, const size_t len, EdKsToken *out, const int max) {
    return src ? walk(src, len, -1, out, max, NULL, NULL) : 0;
}

int ed_ks_style_at(const EdKsToken *toks, const int n, const int off) {
    int lo = 0, hi = n;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (toks[mid].end <= off) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && toks[lo].start <= off && off < toks[lo].end ? toks[lo].style : EKS_NONE;
}

// ---------------------------------------------------------------------------
// matchBrackets (addon/edit/matchbrackets.js findMatchingBracket / scanForBracket)

static int bracket_dir(const char c) { return c == '(' || c == '[' || c == '{' ? 1 : c == ')' || c == ']' || c == '}' ? -1 : 0; }
static char bracket_partner(const char c) {
    switch (c) {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '{': return '}';
    case '}': return '{';
    default: return 0;
    }
}

int ed_ks_match_bracket(const char *src, const size_t len, const EdKsToken *toks, const int n, const int caret, int *a, int *b) {
    int pos = -1;
    if (caret > 0 && caret <= (int) len && bracket_dir(src[caret - 1])) pos = caret - 1;
    else if (caret >= 0 && caret < (int) len && bracket_dir(src[caret])) pos = caret;
    if (pos < 0) return 0;
    const int dir = bracket_dir(src[pos]);
    const int style = ed_ks_style_at(toks, n, pos);
    int depth = 0, scanned = 0;
    for (int i = pos + dir; i >= 0 && i < (int) len; i += dir) {
        if (++scanned > 10000) return 0; // CodeMirror gives up (no marks)
        const char c = src[i];
        if (!bracket_dir(c) || ed_ks_style_at(toks, n, i) != style) continue;
        if (bracket_dir(c) == dir) depth++;
        else if (depth) depth--;
        else {
            *a = pos;
            *b = i;
            return c == bracket_partner(src[pos]) ? 1 : -1;
        }
    }
    *a = pos;
    *b = -1;
    return -1;
}

// ---------------------------------------------------------------------------
// hints (the hint function of scripting.html's initCodeMirror)

static int lower(const int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int starts_with_ci(const char *s, const char *prefix, const size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!s[i] || lower((unsigned char) s[i]) != lower((unsigned char) prefix[i])) return 0;
    }
    return 1;
}

// new RegExp(q + word + q).test(line): the word's "." matches any character
static int quoted_in_line(const char *line, const size_t len, const char *word, const size_t wn, const char q) {
    for (size_t i = 0; i + wn + 2 <= len; i++) {
        if (line[i] != q || line[i + wn + 1] != q) continue;
        size_t k = 0;
        while (k < wn && (word[k] == '.' ? line[i + 1 + k] != '\n' : line[i + 1 + k] == word[k])) k++;
        if (k == wn) return 1;
    }
    return 0;
}

int ed_ks_hints(const char *line, const size_t len, int col, const cJSON *entries, const cJSON *alias, EdKsHint *out, const int max, int *from) {
    if (col > (int) len) col = (int) len;
    if (col < 0) col = 0;
    int start = col;
    while (start > 0 && (is_word((unsigned char) line[start - 1]) || line[start - 1] == '.')) start--;
    *from = start;
    const size_t wn = (size_t) (col - start);
    if (!wn || quoted_in_line(line, len, line + start, wn, '"') || quoted_in_line(line, len, line + start, wn, '\'')) return 0;
    const char *word = line + start;
    int count = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, entries) {
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "text"));
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "type"));
        if (!text) continue;
        char shown[192];
        int ok = starts_with_ci(text, word, wn);
        snprintf(shown, sizeof(shown), "%s", text);
        if (!ok) {
            // an alias (UTILS -> GAME.UTILS, Math -> GAME.UTILS.math) typed instead of its expansion.
            // Intentional difference from v7.1.4: v7 rewrote every listed entry with the last
            // alias that matched (and kept the rewrite in the shared table); here only the
            // entries that matched through an alias show it.
            const cJSON *a;
            cJSON_ArrayForEach(a, alias) {
                const char *exp = cJSON_GetStringValue(a);
                const size_t xn = exp ? strlen(exp) : 0;
                if (!exp || !starts_with_ci(text, exp, xn)) continue;
                char cand[192];
                snprintf(cand, sizeof(cand), "%s%s", a->string, text + xn);
                if (starts_with_ci(cand, word, wn)) {
                    snprintf(shown, sizeof(shown), "%s", cand);
                    ok = 1;
                    break;
                }
            }
        }
        if (!ok) continue;
        if (count < max) {
            snprintf(out[count].text, sizeof(out[count].text), "%s", shown);
            snprintf(out[count].type, sizeof(out[count].type), "%s", type ? type : "");
        }
        if (++count >= max || count >= ED_KS_HINT_MAX) break;
    }
    return count < max ? count : max;
}

// ---------------------------------------------------------------------------
// indentation (simple mode indent stack, indentUnit 4, tabSize 4)

static int col_width(const char *s, const int a, const int b) {
    int cols = 0;
    for (int i = a; i < b; i++) cols = s[i] == '\t' ? (cols / ED_KS_TAB_SIZE + 1) * ED_KS_TAB_SIZE : cols + 1;
    return cols;
}

int ed_ks_line_indent(const char *src, const size_t len, const int line_start, int *bytes) {
    int e = line_start;
    while (e < (int) len && (src[e] == ' ' || src[e] == '\t')) e++;
    if (bytes) *bytes = e - line_start;
    return col_width(src, line_start, e);
}

typedef struct {
    int stack[256];
    int n;
    const char *src;
    size_t len;
} IndentState;

static void indent_bracket(void *user, const int pos, const char ch, const int line_start) {
    (void) pos;
    IndentState *st = user;
    if (bracket_dir(ch) > 0) {
        const int cols = ed_ks_line_indent(st->src, st->len, line_start, NULL) + ED_KS_TAB_SIZE;
        if (st->n < 256) st->stack[st->n] = cols;
        st->n++;
    } else if (st->n > 0) {
        st->n--;
    }
}

int ed_ks_indent_at(const char *src, const size_t len, const int pos) {
    IndentState st = {.n = 0, .src = src, .len = len};
    walk(src, len, pos, NULL, 0, indent_bracket, &st);
    // textAfter: the rest of the line after its leading whitespace; each leading closer dedents
    int p = pos;
    while (p < (int) len && (src[p] == ' ' || src[p] == '\t')) p++;
    int top = st.n - 1;
    while (p < (int) len && bracket_dir(src[p]) < 0) {
        top--;
        p++;
    }
    if (top < 0) return 0;
    return st.stack[top < 256 ? top : 255];
}

int ed_ks_indent_string(const int cols, char *out, const size_t n) {
    int k = 0;
    for (int i = 0; i < cols / ED_KS_TAB_SIZE && (size_t) k + 1 < n; i++) out[k++] = '\t';
    for (int i = 0; i < cols % ED_KS_TAB_SIZE && (size_t) k + 1 < n; i++) out[k++] = ' ';
    if (n) out[k] = 0;
    return k;
}

// ---------------------------------------------------------------------------
// closebrackets (addon/edit/closebrackets.js, pairs ()[]{}''"", closeBefore )]}'":;>)

char ed_ks_closer(const char open) {
    switch (open) {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    case '\'': return '\'';
    case '"': return '"';
    default: return 0;
    }
}

static int cm_word_char(const unsigned char c) { return is_word(c) || c >= 0x80; }

int ed_ks_close_action(const char *src, const size_t len, const int pos, const char ch) {
    const int identical = ch == '\'' || ch == '"';
    const int opening = identical || ch == '(' || ch == '[' || ch == '{';
    const int closing = ch == ')' || ch == ']' || ch == '}';
    if (!opening && !closing) return 0;
    const char next = pos < (int) len ? src[pos] : 0;
    if ((identical || closing) && next == ch) return 2;
    if (identical) {
        const char prev = pos > 0 ? src[pos - 1] : ' ';
        return !cm_word_char((unsigned char) next) && prev != ch && !cm_word_char((unsigned char) prev) ? 1 : 0;
    }
    if (opening) return !next || next == ' ' || next == '\t' || next == '\n' || next == '\r' || strchr(")]}'\":;>", next) ? 1 : 0;
    return 0;
}

// ---------------------------------------------------------------------------
// toggleComment (addon/comment/comment.js lineComment "#", padding " ")

char *ed_ks_toggle_comment(const char *src, const size_t len, int a, int b, int *ra, int *rb) {
    if (a > b) {
        const int t = a;
        a = b;
        b = t;
    }
    // CodeMirror leaves out a last line the selection only touches at column 0
    if (b > a && b > 0 && src[b - 1] == '\n') b--;
    int ls = a;
    while (ls > 0 && src[ls - 1] != '\n') ls--;
    int le = b;
    while (le < (int) len && src[le] != '\n') le++;
    // all non-blank lines commented? / the smallest indentation of the non-blank ones
    int all = 1, any = 0, base = 1 << 30;
    for (int p = ls; p <= le;) {
        int e = p;
        while (e < le && src[e] != '\n') e++;
        int ib;
        const int cols = ed_ks_line_indent(src, len, p, &ib);
        if (p + ib < e) {
            any = 1;
            if (src[p + ib] != '#') all = 0;
            if (cols < base) base = cols;
        }
        p = e + 1;
    }
    if (!any) return NULL;
    char *out = malloc((size_t) (le - ls) * 2 + 16);
    if (!out) return NULL;
    size_t k = 0;
    for (int p = ls; p <= le;) {
        int e = p;
        while (e < le && src[e] != '\n') e++;
        int ib;
        ed_ks_line_indent(src, len, p, &ib);
        if (p + ib >= e) {
            memcpy(out + k, src + p, (size_t) (e - p));
            k += (size_t) (e - p);
        } else if (all) {
            // uncomment: drop "#" and one following space
            const int h = p + ib;
            memcpy(out + k, src + p, (size_t) ib);
            k += (size_t) ib;
            const int rest = h + 1 + (h + 1 < e && src[h + 1] == ' ');
            memcpy(out + k, src + rest, (size_t) (e - rest));
            k += (size_t) (e - rest);
        } else {
            // comment at the base column (inside the indentation when it is wider)
            int i = p, cols = 0;
            while (i < e && cols < base && (src[i] == ' ' || src[i] == '\t')) {
                cols = src[i] == '\t' ? (cols / ED_KS_TAB_SIZE + 1) * ED_KS_TAB_SIZE : cols + 1;
                i++;
            }
            memcpy(out + k, src + p, (size_t) (i - p));
            k += (size_t) (i - p);
            memcpy(out + k, "# ", 2);
            k += 2;
            memcpy(out + k, src + i, (size_t) (e - i));
            k += (size_t) (e - i);
        }
        if (e < le) out[k++] = '\n';
        p = e + 1;
    }
    out[k] = 0;
    *ra = ls;
    *rb = le;
    return out;
}

// ---------------------------------------------------------------------------
// the script store

static const char *side_key(const int server) { return server ? "server" : "client"; }

const char *ed_ks_script_text(const cJSON *scripts, const int server) {
    const char *code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(scripts, side_key(server)));
    return code && code[0] ? code : ed_model_script_template(side_key(server)); // code || templates[key]
}

void ed_ks_script_store(cJSON *scripts, const int server, const char *code) {
    if (!scripts) return;
    cJSON *v = cJSON_CreateString(code ? code : "");
    if (cJSON_GetObjectItemCaseSensitive(scripts, side_key(server))) cJSON_ReplaceItemInObjectCaseSensitive(scripts, side_key(server), v);
    else cJSON_AddItemToObject(scripts, side_key(server), v);
}

// ---------------------------------------------------------------------------
// diagnostics

int ed_ks_check(const char *code, const int server, EdKsDiag *out, const int max) {
    if (max <= 0) return 0;
    KsDiag err, warns[32];
    int nw = 0, n = 0;
    memset(&err, 0, sizeof(err));
    const size_t len = code ? strlen(code) : 0;
    const int rc = ks_validate(code ? code : "", len, server ? KS_SIDE_SERVER : KS_SIDE_CLIENT, &err, warns, 32, &nw);
    if (rc != 0) {
        out[n].line = err.line;
        out[n].col = err.col;
        out[n].error = 1;
        snprintf(out[n].message, sizeof(out[n].message), "%s", err.message);
        n++;
    }
    for (int i = 0; i < nw && i < 32 && n < max; i++, n++) {
        out[n].line = warns[i].line;
        out[n].col = warns[i].col;
        out[n].error = 0;
        snprintf(out[n].message, sizeof(out[n].message), "%s", warns[i].message);
    }
    return n;
}
