// Player target selectors: see shared/include/target_select.h.
#include <target_select.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// normalisePlayerQuery (chat_commands.c): lowercase, [^a-z0-9] removed; `keep_glob` keeps * and ?
static void ts_normalise(const char *in, char *out, const size_t n, const int keep_glob) {
    size_t len = 0;
    for (; in && *in && len + 1 < n; in++) {
        const unsigned char ch = (unsigned char) *in;
        if (isalnum(ch)) out[len++] = (char) tolower(ch);
        else if (keep_glob && (ch == '*' || ch == '?')) out[len++] = (char) ch;
    }
    out[len] = 0;
}

int target_glob_match(const char *p, const char *t) {
    const char *star = NULL, *resume = NULL;
    while (*t) {
        if (*p == '*') {
            star = p++;
            resume = t;
        } else if (*p == '?' || (*p && tolower((unsigned char) *p) == tolower((unsigned char) *t))) {
            p++;
            t++;
        } else if (star) {
            p = star + 1;
            t = ++resume;
        } else {
            return 0;
        }
    }
    while (*p == '*') p++;
    return !*p;
}

typedef enum {
    SEL_NONE, SEL_ALL, SEL_SELF, SEL_OTHERS, SEL_BOTS, SEL_HUMANS, SEL_TEAM, SEL_ENEMIES, SEL_ALIVE, SEL_DEAD,
} Selector;

static Selector ts_selector(const char *q) {
    static const struct { const char *word; Selector sel; } WORDS[] = {
        {"*", SEL_ALL}, {"@all", SEL_ALL}, {"@a", SEL_ALL}, {"@everyone", SEL_ALL},
        {"@me", SEL_SELF}, {"@s", SEL_SELF}, {"@self", SEL_SELF}, {"!", SEL_SELF},
        {"@others", SEL_OTHERS}, {"@bots", SEL_BOTS}, {"@bot", SEL_BOTS}, {"@humans", SEL_HUMANS}, {"@human", SEL_HUMANS},
        {"@team", SEL_TEAM}, {"@friends", SEL_TEAM}, {"@enemies", SEL_ENEMIES}, {"@enemy", SEL_ENEMIES},
        {"@alive", SEL_ALIVE}, {"@dead", SEL_DEAD},
    };
    for (size_t i = 0; i < sizeof(WORDS) / sizeof(WORDS[0]); i++) {
        if (!strcasecmp(q, WORDS[i].word)) return WORDS[i].sel;
    }
    return SEL_NONE;
}

static void ts_trim(const char *in, char *out, const size_t n) {
    while (*in == ' ' || *in == '\t') in++;
    snprintf(out, n, "%s", in);
    for (size_t len = strlen(out); len && (out[len - 1] == ' ' || out[len - 1] == '\t'); len--) out[len - 1] = 0;
}

int target_is_selector(const char *query) {
    char q[128];
    ts_trim(query ? query : "", q, sizeof(q));
    return ts_selector(q) != SEL_NONE || strchr(q, '*') || strchr(q, '?') || q[0] == '@';
}

static int ts_keep(const TargetCandidate *c, const Selector sel, const TargetCandidate *self) {
    switch (sel) {
        case SEL_ALL: return 1;
        case SEL_SELF: return c->self;
        case SEL_OTHERS: return !c->self;
        case SEL_BOTS: return c->bot;
        case SEL_HUMANS: return !c->bot;
        case SEL_TEAM: return c->self || (self->team && c->team == self->team);
        case SEL_ENEMIES: return !c->self && (!self->team || c->team != self->team);
        case SEL_ALIVE: return c->alive;
        case SEL_DEAD: return !c->alive;
        default: return 0;
    }
}

static int ts_finish(TargetResult *r, const TargetStatus status, const int count, const int selector) {
    r->status = status;
    r->count = count;
    r->selector = selector;
    return count;
}

int target_select(const TargetCandidate *cands, const int n, const char *query, const int flags, int *out, const int max,
                  TargetResult *result) {
    TargetResult local;
    TargetResult *r = result ? result : &local;
    char q[128];
    ts_trim(query ? query : "", q, sizeof(q));
    int count = 0;
#define TS_PUSH(i) do { if (count < max && out) out[count] = (i); count++; } while (0)

    const Selector sel = ts_selector(q);
    if (sel != SEL_NONE) {
        const TargetCandidate *self = NULL;
        for (int i = 0; i < n; i++) if (cands[i].self) self = &cands[i];
        const int needs_self = sel == SEL_SELF || sel == SEL_TEAM || sel == SEL_ENEMIES;
        if (needs_self && !self) return ts_finish(r, TARGET_NO_SELF, 0, 1);
        for (int i = 0; i < n; i++) if (ts_keep(&cands[i], sel, self)) TS_PUSH(i);
        return ts_finish(r, count ? TARGET_OK : TARGET_NO_MATCH, count, 1);
    }
    if (q[0] == '@') return ts_finish(r, TARGET_NO_MATCH, 0, 1); // an unknown selector names nobody

    if (strchr(q, '*') || strchr(q, '?')) {
        // a glob over the name as typed; a pattern of letters, digits and spaces also matches the
        // normalised name (bluebot* finds "Blue Bot"), one with punctuation (joel_*) only as typed
        int plain = 1;
        for (const char *p = q; *p; p++) {
            if (!isalnum((unsigned char) *p) && *p != '*' && *p != '?' && *p != ' ') plain = 0;
        }
        char pattern[128];
        ts_normalise(q, pattern, sizeof(pattern), 1);
        if (!plain) pattern[0] = 0;
        for (int i = 0; i < n; i++) {
            const char *name = cands[i].name ? cands[i].name : "";
            char norm[128];
            ts_normalise(name, norm, sizeof(norm), 0);
            if (name[0] && (target_glob_match(q, name) || (pattern[0] && target_glob_match(pattern, norm)))) TS_PUSH(i);
        }
        return ts_finish(r, count ? TARGET_OK : TARGET_NO_MATCH, count, 1);
    }

    // resolveOnlinePlayerByQuery: exact (normalised) name / id, else a unique prefix
    char nq[128];
    ts_normalise(q, nq, sizeof(nq), 0);
    if (!nq[0]) return ts_finish(r, TARGET_EMPTY, 0, 0);
    const size_t qlen = strlen(nq);
    int exact = -1, exact_n = 0, prefix = -1, prefix_n = 0;
    for (int i = 0; i < n; i++) {
        char name[128], id[16], nid[16];
        ts_normalise(cands[i].name, name, sizeof(name), 0);
        snprintf(id, sizeof(id), "%d", cands[i].id);
        ts_normalise(id, nid, sizeof(nid), 0);
        if (!strcmp(name, nq) || !strcmp(nid, nq)) {
            if (exact < 0) exact = i;
            exact_n++;
        } else if ((name[0] && !strncmp(name, nq, qlen)) || !strncmp(nid, nq, qlen)) {
            if (prefix < 0) prefix = i;
            prefix_n++;
        }
    }
    if (exact_n == 1) {
        TS_PUSH(exact);
        return ts_finish(r, TARGET_OK, 1, 0);
    }
    if (exact_n > 1) return ts_finish(r, TARGET_AMBIGUOUS, 0, 0);
    if (flags & TARGET_EXACT_ONLY) return ts_finish(r, TARGET_NOT_FOUND, 0, 0);
    if (prefix_n == 1) {
        TS_PUSH(prefix);
        return ts_finish(r, TARGET_OK, 1, 0);
    }
    return ts_finish(r, prefix_n > 1 ? TARGET_AMBIGUOUS : TARGET_NOT_FOUND, 0, 0);
#undef TS_PUSH
}
