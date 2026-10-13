// Markdown-ish rendering of the AI panel's messages (editor_ai.c) into the panel's HTML. Pure C, no
// DOM: fenced code blocks (``` with a language label), headings (# .. ####), bullet and numbered
// lists, block quotes, simple pipe tables, horizontal rules, paragraphs (single newlines kept as
// line breaks) and inline `code`, **bold**, *italic* / _italic_, ~~strike~~ and [text](url) links
// (shown as text + URL, never followed: the panel opens nothing on its own). Everything else is
// escaped text. An unterminated code fence (a reply still streaming) renders as code so far.
#include "editor_ai.h"
#include <ctype.h>
#include <string.h>

static void esc_n(EdBuf *b, const char *s, const size_t n) {
    size_t run = 0;
    for (size_t i = 0; i <= n; i++) {
        const char *rep = NULL;
        if (i < n) {
            switch (s[i]) {
                case '&': rep = "&amp;"; break;
                case '<': rep = "&lt;"; break;
                case '>': rep = "&gt;"; break;
                case '"': rep = "&quot;"; break;
                case '\'': rep = "&#39;"; break;
                default: break;
            }
        }
        if (i == n || rep) {
            if (i > run) edb_addf(b, "%.*s", (int) (i - run), s + run);
            if (rep) edb_add(b, rep);
            run = i + 1;
        }
    }
}

// a closing marker for an inline span starting at s (same line); NULL when none
static const char *find_close(const char *s, const char *end, const char *mark) {
    const size_t m = strlen(mark);
    for (const char *p = s; p + m <= end; p++) {
        if (!strncmp(p, mark, m) && p > s) return p;
    }
    return NULL;
}

static void inline_md(EdBuf *b, const char *s, size_t n) {
    const char *p = s, *end = s + n;
    const char *run = p;
    while (p < end) {
        const char c = *p;
        const char *close = NULL;
        if (c == '`') {
            close = memchr(p + 1, '`', (size_t) (end - p - 1));
            if (close) {
                esc_n(b, run, (size_t) (p - run));
                edb_add(b, "<code class='knAiInl'>");
                esc_n(b, p + 1, (size_t) (close - p - 1));
                edb_add(b, "</code>");
                p = run = close + 1;
                continue;
            }
        } else if ((c == '*' || c == '_') && p + 1 < end && p[1] == c && (close = find_close(p + 2, end, c == '*' ? "**" : "__"))) {
            esc_n(b, run, (size_t) (p - run));
            edb_add(b, "<b>");
            inline_md(b, p + 2, (size_t) (close - p - 2));
            edb_add(b, "</b>");
            p = run = close + 2;
            continue;
        } else if (c == '~' && p + 1 < end && p[1] == '~' && (close = find_close(p + 2, end, "~~"))) {
            esc_n(b, run, (size_t) (p - run));
            edb_add(b, "<s>");
            inline_md(b, p + 2, (size_t) (close - p - 2));
            edb_add(b, "</s>");
            p = run = close + 2;
            continue;
        } else if ((c == '*' || c == '_') && p + 1 < end && !isspace((unsigned char) p[1]) &&
                   (p == s || !isalnum((unsigned char) p[-1])) && (close = find_close(p + 1, end, c == '*' ? "*" : "_")) &&
                   !isspace((unsigned char) close[-1]) && (close + 1 >= end || !isalnum((unsigned char) close[1]))) {
            esc_n(b, run, (size_t) (p - run));
            edb_add(b, "<i>");
            inline_md(b, p + 1, (size_t) (close - p - 1));
            edb_add(b, "</i>");
            p = run = close + 1;
            continue;
        } else if (c == '[') {
            const char *rb = memchr(p, ']', (size_t) (end - p));
            if (rb && rb + 1 < end && rb[1] == '(') {
                const char *rp = memchr(rb, ')', (size_t) (end - rb));
                if (rp) {
                    esc_n(b, run, (size_t) (p - run));
                    edb_add(b, "<span class='knAiLink'>");
                    inline_md(b, p + 1, (size_t) (rb - p - 1));
                    edb_add(b, "</span><span class='knAiUrl'> (");
                    esc_n(b, rb + 2, (size_t) (rp - rb - 2));
                    edb_add(b, ")</span>");
                    p = run = rp + 1;
                    continue;
                }
            }
        }
        p++;
    }
    esc_n(b, run, (size_t) (end - run));
}

static int is_rule(const char *l, const size_t n) {
    int count = 0;
    char ch = 0;
    for (size_t i = 0; i < n; i++) {
        if (l[i] == ' ') continue;
        if (l[i] != '-' && l[i] != '*' && l[i] != '_') return 0;
        if (ch && l[i] != ch) return 0;
        ch = l[i];
        count++;
    }
    return count >= 3;
}

// list item: returns the marker length (0 = not an item); *ordered set
static size_t list_marker(const char *l, const size_t n, int *ordered) {
    size_t i = 0;
    while (i < n && l[i] == ' ') i++;
    if (i + 1 < n && (l[i] == '-' || l[i] == '*' || l[i] == '+') && l[i + 1] == ' ') {
        *ordered = 0;
        return i + 2;
    }
    size_t d = i;
    while (d < n && isdigit((unsigned char) l[d])) d++;
    if (d > i && d + 1 < n && (l[d] == '.' || l[d] == ')') && l[d + 1] == ' ') {
        *ordered = 1;
        return d + 2;
    }
    return 0;
}

static int table_row(const char *l, const size_t n) {
    size_t i = 0;
    while (i < n && l[i] == ' ') i++;
    return i < n && l[i] == '|';
}

static int table_sep(const char *l, const size_t n) {
    if (!table_row(l, n)) return 0;
    for (size_t i = 0; i < n; i++) {
        if (!strchr("|-: ", l[i])) return 0;
    }
    return 1;
}

static void table_cells(EdBuf *b, const char *l, size_t n, const char *tag) {
    while (n && (l[n - 1] == ' ' || l[n - 1] == '|')) n--;
    size_t i = 0;
    while (i < n && (l[i] == ' ' || l[i] == '|')) i++;
    edb_add(b, "<tr>");
    while (i <= n) {
        size_t j = i;
        while (j < n && l[j] != '|') j++;
        size_t a = i, z = j;
        while (a < z && l[a] == ' ') a++;
        while (z > a && l[z - 1] == ' ') z--;
        edb_addf(b, "<%s>", tag);
        inline_md(b, l + a, z - a);
        edb_addf(b, "</%s>", tag);
        if (j >= n) break;
        i = j + 1;
    }
    edb_add(b, "</tr>");
}

void ai_md_html(const char *md, EdBuf *out) {
    if (!md) return;
    const char *p = md;
    int in_list = 0; // 0 none, 1 ul, 2 ol
    int in_para = 0;
    int in_table = 0;
    while (*p) {
        const char *nl = strchr(p, '\n');
        const size_t n = nl ? (size_t) (nl - p) : strlen(p);
        const char *next = nl ? nl + 1 : p + n;
        size_t ind = 0;
        while (ind < n && (p[ind] == ' ' || p[ind] == '\t')) ind++;
        const char *l = p + ind;
        const size_t ln = n - ind;
        // fenced code
        if (ln >= 3 && !strncmp(l, "```", 3)) {
            if (in_para) edb_add(out, "</div>"), in_para = 0;
            if (in_list) edb_add(out, in_list == 1 ? "</ul>" : "</ol>"), in_list = 0;
            if (in_table) edb_add(out, "</table>"), in_table = 0;
            char lang[32] = "";
            size_t k = 0;
            for (size_t i = 3; i < ln && k + 1 < sizeof(lang) && !isspace((unsigned char) l[i]); i++) lang[k++] = l[i];
            lang[k] = 0;
            edb_add(out, "<div class='knAiCode'><div class='knAiCodeHead'>");
            esc_n(out, lang[0] ? lang : "code", strlen(lang[0] ? lang : "code"));
            edb_add(out, "</div><pre class='knAiPre'>");
            const char *q = next;
            int first = 1;
            while (*q) {
                const char *qnl = strchr(q, '\n');
                const size_t qn = qnl ? (size_t) (qnl - q) : strlen(q);
                size_t qi = 0;
                while (qi < qn && q[qi] == ' ') qi++;
                if (qn - qi >= 3 && !strncmp(q + qi, "```", 3)) {
                    q = qnl ? qnl + 1 : q + qn;
                    goto fence_done;
                }
                if (!first) edb_add(out, "\n");
                first = 0;
                esc_n(out, q, qn);
                q = qnl ? qnl + 1 : q + qn;
            }
        fence_done:
            edb_add(out, "</pre></div>");
            p = q;
            continue;
        }
        if (!ln) { // blank line
            if (in_para) edb_add(out, "</div>"), in_para = 0;
            if (in_list) edb_add(out, in_list == 1 ? "</ul>" : "</ol>"), in_list = 0;
            if (in_table) edb_add(out, "</table>"), in_table = 0;
            p = next;
            continue;
        }
        if (table_row(l, ln)) {
            if (in_para) edb_add(out, "</div>"), in_para = 0;
            if (in_list) edb_add(out, in_list == 1 ? "</ul>" : "</ol>"), in_list = 0;
            if (!table_sep(l, ln)) {
                const char *nn = strchr(next, '\n');
                const size_t nlen = nn ? (size_t) (nn - next) : strlen(next);
                const int header = !in_table && table_sep(next, nlen);
                if (!in_table) edb_add(out, "<table class='knAiTable'>"), in_table = 1;
                table_cells(out, l, ln, header ? "th" : "td");
            }
            p = next;
            continue;
        }
        if (in_table) edb_add(out, "</table>"), in_table = 0;
        int hashes = 0;
        while ((size_t) hashes < ln && l[hashes] == '#') hashes++;
        if (hashes >= 1 && hashes <= 6 && (size_t) hashes < ln && l[hashes] == ' ') {
            if (in_para) edb_add(out, "</div>"), in_para = 0;
            if (in_list) edb_add(out, in_list == 1 ? "</ul>" : "</ol>"), in_list = 0;
            edb_addf(out, "<div class='knAiH%d'>", hashes > 4 ? 4 : hashes);
            inline_md(out, l + hashes + 1, ln - (size_t) hashes - 1);
            edb_add(out, "</div>");
            p = next;
            continue;
        }
        if (is_rule(l, ln)) {
            if (in_para) edb_add(out, "</div>"), in_para = 0;
            if (in_list) edb_add(out, in_list == 1 ? "</ul>" : "</ol>"), in_list = 0;
            edb_add(out, "<div class='knAiHr'></div>");
            p = next;
            continue;
        }
        if (l[0] == '>') {
            if (in_para) edb_add(out, "</div>"), in_para = 0;
            if (in_list) edb_add(out, in_list == 1 ? "</ul>" : "</ol>"), in_list = 0;
            const size_t skip = ln > 1 && l[1] == ' ' ? 2 : 1;
            edb_add(out, "<div class='knAiQuote'>");
            inline_md(out, l + skip, ln - skip);
            edb_add(out, "</div>");
            p = next;
            continue;
        }
        int ordered = 0;
        const size_t mk = list_marker(p, n, &ordered);
        if (mk) {
            if (in_para) edb_add(out, "</div>"), in_para = 0;
            const int want = ordered ? 2 : 1;
            if (in_list && in_list != want) edb_add(out, in_list == 1 ? "</ul>" : "</ol>"), in_list = 0;
            if (!in_list) edb_add(out, ordered ? "<ol class='knAiList'>" : "<ul class='knAiList'>"), in_list = want;
            edb_add(out, ind >= 2 ? "<li class='knAiSub'>" : "<li>");
            inline_md(out, p + mk, n - mk);
            edb_add(out, "</li>");
            p = next;
            continue;
        }
        if (in_list && ind >= 2) { // a continuation line of the last item
            edb_add(out, "<div class='knAiCont'>");
            inline_md(out, l, ln);
            edb_add(out, "</div>");
            p = next;
            continue;
        }
        if (in_list) edb_add(out, in_list == 1 ? "</ul>" : "</ol>"), in_list = 0;
        if (in_para) edb_add(out, "<br>");
        else edb_add(out, "<div class='knAiP'>"), in_para = 1;
        inline_md(out, l, ln);
        p = next;
    }
    if (in_para) edb_add(out, "</div>");
    if (in_list) edb_add(out, in_list == 1 ? "</ul>" : "</ol>");
    if (in_table) edb_add(out, "</table>");
}

void ai_html_escape(EdBuf *out, const char *s) { esc_n(out, s, s ? strlen(s) : 0); }
