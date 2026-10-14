// Localisation: Krunker v10.0.2's i18n object in C (client/include/i18n.h has the contract).
// v10 reference: live bundle game.deob.js:18773 (langNames), 197726-197788 (the i18n object:
// locale / defaultLocale / supported / translations / parseText / getTranslations / saveLocale /
// setLocale / setLocaleFromStorage / get = t / has = h / replacePlaceholders), 203431-203448
// (settings.list.lang).
#include <i18n.h>
#include <cJSON.h>
#include <html_dom.h>
#include <shared.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// game.deob.js:18773 config.langNames, in its order (the Language select lists them so)
static const I18nLang LANGS[I18N_LANG_COUNT] = {
    {"en", "English", "en"},          {"es", "Español", "es"},      {"de", "Deutsch", "de"},
    {"nl", "Nederlands", "nl"},       {"kr", "한국어", "ko"},       {"pr", "Português", "pt"},
    {"jp", "日本語", "ja"},           {"fr", "Français", "fr"},     {"fi", "Suomi", "fi"},
    {"zh", "简体中文", "zh-CN"},      {"zhtr", "繁體中文", "zh-TW"}, {"pl", "Polski", "pl"},
    {"hi", "हिन्दी", "hi"},            {"ru", "Pусский", "ru"},      {"it", "Italiano", "it"},
    {"tg", "Tagalog", "tl"},          {"tr", "Türkçe", "tr"},
};

const I18nLang *i18n_lang(const int index) { return index >= 0 && index < I18N_LANG_COUNT ? &LANGS[index] : NULL; }

const I18nLang *i18n_lang_find(const char *code) {
    for (int i = 0; code && i < I18N_LANG_COUNT; i++)
        if (!strcmp(LANGS[i].code, code)) return &LANGS[i];
    return NULL;
}

static int lang_index(const char *code) {
    const I18nLang *l = i18n_lang_find(code);
    return l ? (int) (l - LANGS) : -1;
}

// ---------------------------------------------------------------- tables

typedef struct {
    char *key, *val;
} Entry;

// Reverse map entry (DOM translation): a normalised text -> the entry it came from; seg -1 = the
// whole value, else its seg-th text segment of nseg (values with markup).
typedef struct {
    char *text;
    int entry, seg, nseg;
    int next; // the next entry with the same text (-1 none): one English text, several keys
} RevEntry;

typedef struct {
    int state;          // 0 not asked, 1 loaded, -1 failed (lookups fall back to en), 2 fetching (web)
    int pending;        // web: fetches still out (table + kn overlay)
    Entry *e;
    int n, cap;
    int32_t *hash;      // key -> entry index, open addressing, -1 empty
    int hcap;
    RevEntry *rev;      // built on first DOM use
    int rn;
    int32_t *rhash;
    int rhcap;
} Table;

static Table g_tables[I18N_LANG_COUNT];
static int g_init;
static int g_locale;         // active (index into LANGS)
static int g_wanted = -1;    // web: asked for, table on its way
static int g_dom_locale;     // the language the document's markup currently shows
static unsigned g_generation = 1;

#define MAX_LISTENERS 32
static struct {
    I18nListener fn;
    void *user;
} g_listeners[MAX_LISTENERS];
static int g_listener_count;

static uint32_t hash_str(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char) *s) * 16777619u;
    return h;
}

static char *dup_str(const char *s) {
    const size_t n = strlen(s);
    char *d = malloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

static void table_rehash(Table *t) {
    free(t->hash);
    t->hcap = 64;
    while (t->hcap < t->n * 2 + 2) t->hcap *= 2;
    t->hash = malloc(sizeof(int32_t) * (size_t) t->hcap);
    if (!t->hash) { t->hcap = 0; return; }
    for (int i = 0; i < t->hcap; i++) t->hash[i] = -1;
    for (int i = 0; i < t->n; i++) {
        uint32_t h = hash_str(t->e[i].key) & (uint32_t) (t->hcap - 1);
        while (t->hash[h] >= 0) h = (h + 1) & (uint32_t) (t->hcap - 1);
        t->hash[h] = i;
    }
}

static int table_find(const Table *t, const char *key) {
    if (!t->hcap || !key) return -1;
    uint32_t h = hash_str(key) & (uint32_t) (t->hcap - 1);
    while (t->hash[h] >= 0) {
        if (!strcmp(t->e[t->hash[h]].key, key)) return t->hash[h];
        h = (h + 1) & (uint32_t) (t->hcap - 1);
    }
    return -1;
}

// parseText assigns into an array: a repeated key keeps its first position and its last value
static void table_put(Table *t, const char *key, const char *val) {
    for (int i = 0; i < t->n; i++) { // only while building (n small per merge); hash after
        if (!strcmp(t->e[i].key, key)) {
            char *v = dup_str(val);
            if (v) { free(t->e[i].val); t->e[i].val = v; }
            return;
        }
    }
    if (t->n == t->cap) {
        const int cap = t->cap ? t->cap * 2 : 1024;
        Entry *e = realloc(t->e, sizeof(Entry) * (size_t) cap);
        if (!e) return;
        t->e = e;
        t->cap = cap;
    }
    t->e[t->n].key = dup_str(key);
    t->e[t->n].val = dup_str(val);
    if (t->e[t->n].key && t->e[t->n].val) t->n++;
    else { free(t->e[t->n].key); free(t->e[t->n].val); }
}

static void table_free_rev(Table *t) {
    for (int i = 0; i < t->rn; i++) free(t->rev[i].text);
    free(t->rev);
    free(t->rhash);
    t->rev = NULL;
    t->rhash = NULL;
    t->rn = t->rhcap = 0;
}

static void table_free(Table *t) {
    for (int i = 0; i < t->n; i++) { free(t->e[i].key); free(t->e[i].val); }
    free(t->e);
    free(t->hash);
    table_free_rev(t);
    memset(t, 0, sizeof(*t));
}

// One JSON object of strings into the table. Keys already present are replaced (the kn overlay
// never shadows a Krunker key: its keys are kn.*).
static int table_merge_json(Table *t, const char *text, const size_t len) {
    cJSON *root = cJSON_ParseWithLength(text, len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return 0;
    }
    // fast path for the big tables: no duplicate scan when the table is empty
    const int fresh = t->n == 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (!item->string || !cJSON_IsString(item) || item->string[0] == '_') continue; // "_comment" etc.
        if (fresh) {
            if (t->n == t->cap) {
                const int cap = t->cap ? t->cap * 2 : 2048;
                Entry *e = realloc(t->e, sizeof(Entry) * (size_t) cap);
                if (!e) break;
                t->e = e;
                t->cap = cap;
            }
            t->e[t->n].key = dup_str(item->string);
            t->e[t->n].val = dup_str(item->valuestring);
            if (t->e[t->n].key && t->e[t->n].val) t->n++;
        } else {
            table_put(t, item->string, item->valuestring);
        }
    }
    cJSON_Delete(root);
    table_rehash(t);
    table_free_rev(t);
    return 1;
}

// ---------------------------------------------------------------- loading

static void apply_locale(int index); // forward

static char *ui_file_path(const char *rel) {
#ifdef __EMSCRIPTEN__
    // en is preloaded with the window templates (--preload-file ui/i18n/en.json, ui/i18n/kn/en.json):
    // v10 has it inline in its bundle, and the first frame already draws the C UI's strings
    char *path = malloc(strlen(rel) + 5);
    if (path) sprintf(path, "/ui/%s", rel);
    return path;
#else
    const char *dir = getenv("KRUNKNATIVE_UI_DIR"); // main.c's page tree override
    if (dir && *dir) {
        char *own = malloc(strlen(dir) + strlen(rel) + 2);
        if (own) {
            sprintf(own, "%s/%s", dir, rel);
            FILE *f = fopen(own, "rb");
            if (f) { fclose(f); return own; }
            free(own);
        }
    }
    const char *base = ui_default_path();
    char *path = malloc(strlen(base) + strlen(rel) + 1);
    if (path) sprintf(path, "%s%s", base, rel);
    return path;
#endif
}

static int load_file_into(Table *t, const char *rel, const int required) {
    char *path = ui_file_path(rel);
    unsigned char *data = NULL;
    size_t len = 0;
    int ok = 0;
    if (path && read_file(path, &data, &len) == 0) ok = table_merge_json(t, (const char *) data, len);
    if (!ok && required) fprintf(stderr, "[i18n] %s missing or unreadable: falling back to en\n", path ? path : rel);
    free(data);
    free(path);
    return ok;
}

#ifdef __EMSCRIPTEN__
static void web_fetch(int index, const char *url, int required);
#endif

static void table_load(const int index) {
    Table *t = &g_tables[index];
    if (t->state) return;
    char rel[64], kn[64];
    snprintf(rel, sizeof(rel), "i18n/%s.json", LANGS[index].code);
    snprintf(kn, sizeof(kn), "i18n/kn/%s.json", LANGS[index].code);
#ifdef __EMSCRIPTEN__
    if (index > 0) {
        t->state = 2;
        t->pending = 2;
        web_fetch(index, rel, 1);
        web_fetch(index, kn, 0);
        return;
    }
#endif
    const int ok = load_file_into(t, rel, 1);
    load_file_into(t, kn, 0);
    t->state = ok || t->n ? 1 : -1;
}

#ifdef __EMSCRIPTEN__
// emscripten_async_wget_data: page-relative URLs (dist/web holds ui/, so i18n/<code>.json)
typedef struct {
    int index, required;
    char url[64];
} Fetch;

static void web_settled(Fetch *f) {
    Table *t = &g_tables[f->index];
    if (--t->pending <= 0) {
        t->state = t->n ? 1 : -1;
        if (t->state < 0) fprintf(stderr, "[i18n] %s: no table, falling back to en\n", LANGS[f->index].code);
        if (f->index == g_wanted) { // the language picked last (an earlier pick that arrives late is dropped)
            g_wanted = -1;
            apply_locale(f->index);
        }
    }
    free(f);
}

static void web_onload(void *arg, void *data, int size) {
    Fetch *f = arg;
    if (!table_merge_json(&g_tables[f->index], (const char *) data, (size_t) size) && f->required)
        fprintf(stderr, "[i18n] %s: not a JSON object\n", f->url);
    web_settled(f);
}

static void web_onerror(void *arg) {
    Fetch *f = arg;
    if (f->required) fprintf(stderr, "[i18n] %s: fetch failed\n", f->url);
    web_settled(f);
}

static void web_fetch(const int index, const char *url, const int required) {
    Fetch *f = calloc(1, sizeof(*f));
    if (!f) { g_tables[index].pending--; return; }
    f->index = index;
    f->required = required;
    snprintf(f->url, sizeof(f->url), "%s", url);
    emscripten_async_wget_data(f->url, f, web_onload, web_onerror);
}
#endif

void i18n_init(void) {
    if (g_init) return;
    g_init = 1;
    g_locale = 0;
    g_dom_locale = 0;
    table_load(0); // translations.en: always there (inline in v10)
    // setLocaleFromStorage(): krk_lang if supported, else defaultLocale. KRUNKNATIVE_LANG (tests,
    // tools) overrides it natively.
    char saved[32] = "";
    const char *env = getenv("KRUNKNATIVE_LANG");
#ifndef __EMSCRIPTEN__
    if (env && *env) snprintf(saved, sizeof(saved), "%s", env);
    else
#endif
        if (html_storage_get(I18N_STORAGE_KEY, saved, sizeof(saved)) < 0) saved[0] = 0;
    (void) env;
    const int want = lang_index(saved);
    if (want > 0) i18n_set_locale(saved);
}

const char *i18n_locale(void) {
    i18n_init();
    return LANGS[g_locale].code;
}

unsigned i18n_generation(void) { return g_generation; }

void i18n_on_change(const I18nListener fn, void *user) {
    if (!fn || g_listener_count == MAX_LISTENERS) return;
    for (int i = 0; i < g_listener_count; i++)
        if (g_listeners[i].fn == fn && g_listeners[i].user == user) return;
    g_listeners[g_listener_count].fn = fn;
    g_listeners[g_listener_count].user = user;
    g_listener_count++;
}

static void apply_locale(const int index) {
    const int changed = index != g_locale;
    g_locale = index;
    g_generation++;
    html_dom_set_lang(LANGS[index].bcp47);
    if (html_dom_root()) i18n_translate_tree(0); // the page as it is now, in the new language
    if (!changed && index != 0) return;
    for (int i = 0; i < g_listener_count; i++) g_listeners[i].fn(g_listeners[i].user);
}

int i18n_set_locale(const char *code) {
    i18n_init();
    const int index = lang_index(code);
    if (index < 0) return 0; // setLocale: supported.indexOf(code) > -1 only
    html_storage_set(I18N_STORAGE_KEY, LANGS[index].code); // saveLocale()
    if (index == g_locale && g_wanted < 0) return 0;
    table_load(index);
    if (g_tables[index].state == 2) { // web: switch when it arrives
        g_wanted = index;
        return 0;
    }
    g_wanted = -1;
    apply_locale(index);
    return 1;
}

int i18n_table_size(const char *code) {
    i18n_init();
    const int i = lang_index(code);
    return i < 0 || g_tables[i].state != 1 ? -1 : g_tables[i].n;
}

int i18n_ready(const char *code) {
    i18n_init();
    const int i = lang_index(code);
    return i >= 0 && (g_tables[i].state == 1 || g_tables[i].state == -1);
}

void i18n_shutdown(void) {
    for (int i = 0; i < I18N_LANG_COUNT; i++) table_free(&g_tables[i]);
    g_init = 0;
    g_listener_count = 0;
}

// ---------------------------------------------------------------- lookups

static const char *lookup(const int table, const char *key) {
    const Table *t = &g_tables[table];
    const int i = t->state == 1 ? table_find(t, key) : -1;
    return i >= 0 && t->e[i].val[0] ? t->e[i].val : NULL; // "" is falsy in v10's `||` chain
}

const char *i18n_has(const char *key) {
    i18n_init();
    if (!key) return NULL;
    const char *v = g_locale ? lookup(g_locale, key) : NULL;
    return v ? v : lookup(0, key);
}

const char *i18n_t(const char *key) {
    const char *v = i18n_has(key);
    return v ? v : I18N_UNLOCALIZED;
}

const char *i18n_tr(const char *key, const char *english) {
    const char *v = i18n_has(key);
    return v ? v : english ? english : I18N_UNLOCALIZED;
}

const char *i18n_trf(char *out, const size_t n, const char *key, const char *english, const int argc, const char *const *argv) {
    return i18n_replace(out, n, i18n_tr(key, english), argc, argv);
}

const char *i18n_replace(char *out, const size_t n, const char *text, const int argc, const char *const *argv) {
    if (!out || !n) return out;
    size_t o = 0;
    for (const char *p = text ? text : ""; *p && o + 1 < n;) {
        if (p[0] == '{' && p[1] >= '0' && p[1] <= '9' && p[2] == '}') { // /\{(\d)\}/gm
            const int d = p[1] - '0';
            const char *arg = d < argc && argv ? argv[d] : NULL;
            for (const char *a = arg ? arg : ""; *a && o + 1 < n; a++) out[o++] = *a;
            p += 3;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    return out;
}

const char *i18n_format(char *out, const size_t n, const char *key, const int argc, const char *const *argv) {
    return i18n_replace(out, n, i18n_t(key), argc, argv);
}

// ---------------------------------------------------------------- DOM translation

// HTML entities of the tables (&amp; &lt; &gt; &quot; &#39; &nbsp; &#N; &#xH;) -> UTF-8; a bare
// '&' stays (innerHTML keeps it).
static size_t put_utf8(char *o, const uint32_t cp) {
    if (cp < 0x80) { o[0] = (char) cp; return 1; }
    if (cp < 0x800) { o[0] = (char) (0xC0 | (cp >> 6)); o[1] = (char) (0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char) (0xE0 | (cp >> 12)); o[1] = (char) (0x80 | ((cp >> 6) & 63)); o[2] = (char) (0x80 | (cp & 63));
        return 3;
    }
    o[0] = (char) (0xF0 | (cp >> 18)); o[1] = (char) (0x80 | ((cp >> 12) & 63));
    o[2] = (char) (0x80 | ((cp >> 6) & 63)); o[3] = (char) (0x80 | (cp & 63));
    return 4;
}

// `src[0..len)` decoded, white space runs folded to one space, trimmed: the matching form.
static char *normalise(const char *src, const size_t len) {
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t o = 0;
    int space = 0;
    for (size_t i = 0; i < len;) {
        char buf[8];
        size_t bl = 0;
        if (src[i] == '&') {
            static const struct { const char *name; uint32_t cp; } ENT[] = {
                {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&#39;", '\''}, {"&apos;", '\''}, {"&nbsp;", 0xA0},
            };
            size_t k;
            for (k = 0; k < sizeof(ENT) / sizeof(ENT[0]); k++) {
                const size_t el = strlen(ENT[k].name);
                if (i + el <= len && !memcmp(src + i, ENT[k].name, el)) { bl = put_utf8(buf, ENT[k].cp); i += el; break; }
            }
            if (k == sizeof(ENT) / sizeof(ENT[0]) && i + 2 < len && src[i + 1] == '#') {
                const int hex = src[i + 2] == 'x' || src[i + 2] == 'X';
                char *end;
                const unsigned long cp = strtoul(src + i + 2 + hex, &end, hex ? 16 : 10);
                if (end > src + i + 2 + hex && (size_t) (end - src) < len && *end == ';' && cp && cp < 0x110000) {
                    bl = put_utf8(buf, (uint32_t) cp);
                    i = (size_t) (end - src) + 1;
                }
            }
            if (!bl) { buf[0] = '&'; bl = 1; i++; }
        } else {
            buf[0] = src[i++];
            bl = 1;
        }
        const int ws = bl == 1 && (buf[0] == ' ' || buf[0] == '\n' || buf[0] == '\t' || buf[0] == '\r' || buf[0] == '\f');
        const int nbsp = bl == 2 && (unsigned char) buf[0] == 0xC2 && (unsigned char) buf[1] == 0xA0;
        if (ws || nbsp) {
            space = o > 0;
            continue;
        }
        if (space) { out[o++] = ' '; space = 0; }
        memcpy(out + o, buf, bl);
        o += bl;
    }
    out[o] = 0;
    return out;
}

// Text segments of a value: the text between tags. Returns the count (all of them, empty ones
// included, so both languages split alike); seg i = [starts[i], ends[i]).
#define MAX_SEGS 16
static int split_segments(const char *v, size_t *starts, size_t *ends) {
    int n = 0;
    size_t i = 0, s = 0;
    const size_t len = strlen(v);
    while (i <= len) {
        if (i == len || (v[i] == '<' && i + 1 < len && (isalpha((unsigned char) v[i + 1]) || v[i + 1] == '/' || v[i + 1] == '!'))) {
            if (n < MAX_SEGS) { starts[n] = s; ends[n] = i; }
            n++;
            if (i == len) break;
            const char *close = strchr(v + i, '>');
            if (!close) break;
            i = (size_t) (close - v) + 1;
            s = i;
            continue;
        }
        i++;
    }
    return n;
}

static int has_placeholder(const char *v) {
    for (; *v; v++)
        if (v[0] == '{' && v[1] >= '0' && v[1] <= '9' && v[2] == '}') return 1;
    return 0;
}

static int has_letter(const char *s) {
    for (; *s; s++)
        if (isalpha((unsigned char) *s) || (unsigned char) *s >= 0x80) return 1;
    return 0;
}

static void rev_add(Table *t, char *text, const int entry, const int seg, const int nseg) {
    if (!text || !text[0] || !has_letter(text)) { free(text); return; }
    RevEntry *r = realloc(t->rev, sizeof(RevEntry) * (size_t) (t->rn + 1));
    if (!r) { free(text); return; }
    t->rev = r;
    t->rev[t->rn++] = (RevEntry){text, entry, seg, nseg, -1};
}

static void table_build_rev(Table *t) {
    if (t->rev || t->state != 1) return;
    for (int i = 0; i < t->n; i++) {
        const char *v = t->e[i].val;
        if (!v[0] || has_placeholder(v)) continue;
        size_t st[MAX_SEGS], en[MAX_SEGS];
        const int nseg = split_segments(v, st, en);
        if (nseg <= 1) {
            rev_add(t, normalise(v, strlen(v)), i, -1, 1);
            continue;
        }
        if (nseg > MAX_SEGS) continue;
        for (int k = 0; k < nseg; k++) rev_add(t, normalise(v + st[k], en[k] - st[k]), i, k, nseg);
    }
    t->rhcap = 64;
    while (t->rhcap < t->rn * 2 + 2) t->rhcap *= 2;
    t->rhash = malloc(sizeof(int32_t) * (size_t) t->rhcap);
    if (!t->rhash) { t->rhcap = 0; return; }
    for (int i = 0; i < t->rhcap; i++) t->rhash[i] = -1;
    for (int i = 0; i < t->rn; i++) {
        // one chain per text, in key order (menu, generic, ... come first); whole values before
        // segments of other values
        uint32_t h = hash_str(t->rev[i].text) & (uint32_t) (t->rhcap - 1);
        int dup = 0;
        while (t->rhash[h] >= 0) {
            RevEntry *o = &t->rev[t->rhash[h]];
            if (!strcmp(o->text, t->rev[i].text)) {
                if (o->seg >= 0 && t->rev[i].seg < 0) {
                    t->rev[i].next = t->rhash[h];
                    t->rhash[h] = i;
                } else {
                    int last = t->rhash[h];
                    while (t->rev[last].next >= 0 && (t->rev[i].seg >= 0 || t->rev[t->rev[last].next].seg < 0)) last = t->rev[last].next;
                    t->rev[i].next = t->rev[last].next;
                    t->rev[last].next = i;
                }
                dup = 1;
                break;
            }
            h = (h + 1) & (uint32_t) (t->rhcap - 1);
        }
        if (!dup) t->rhash[h] = i;
    }
}

static const RevEntry *rev_find(Table *t, const char *text) {
    table_build_rev(t);
    if (!t->rhcap) return NULL;
    uint32_t h = hash_str(text) & (uint32_t) (t->rhcap - 1);
    while (t->rhash[h] >= 0) {
        if (!strcmp(t->rev[t->rhash[h]].text, text)) return &t->rev[t->rhash[h]];
        h = (h + 1) & (uint32_t) (t->rhcap - 1);
    }
    return NULL;
}

typedef struct {
    int from;          // the language the markup shows (then en)
    char out[4096];
} MapCtx;

// Plain text of value `v` (or of its segment `seg`, when it splits into `nseg` like the source):
// entities decoded, tags dropped. NULL when the segments do not line up.
static char *target_text(const char *v, const int seg, const int nseg) {
    size_t st[MAX_SEGS], en[MAX_SEGS];
    const int n = split_segments(v, st, en);
    if (seg < 0) {
        if (n <= 1) return normalise(v, strlen(v));
        size_t total = 0;
        char *joined = malloc(strlen(v) + 1);
        if (!joined) return NULL;
        for (int k = 0; k < n && k < MAX_SEGS; k++) { memcpy(joined + total, v + st[k], en[k] - st[k]); total += en[k] - st[k]; }
        joined[total] = 0;
        char *r = normalise(joined, total);
        free(joined);
        return r;
    }
    if (n != nseg || n > MAX_SEGS) return NULL;
    return normalise(v + st[seg], en[seg] - st[seg]);
}

// The key each translated string came from is kept on its owner element, data-kn-i18n =
// ";<slot>|<key>|<seg>|<nseg>" per text node / attribute (the v10 menu's Svelte markup knew its keys;
// the captured page does not). A later switch reads it back instead of mapping the text again, so
// two keys that share a translation in one language do not get mixed up on the way to the next.
// A record is only trusted while the text still is that key's string (the C UI may have written
// something else into the element since).
#define I18N_ATTR "data-kn-i18n"

typedef struct {
    int found;
    char key[96];
    int seg, nseg;
} Record;

static void record_get(const HtmlElem owner, const int slot, Record *r, char *attr, const size_t an) {
    r->found = 0;
    attr[0] = 0;
    if (html_dom_get_attr(owner, I18N_ATTR, attr, an) < 0) { attr[0] = 0; return; }
    char want[16];
    snprintf(want, sizeof(want), ";%d|", slot);
    const char *p = strstr(attr, want);
    if (!p) return;
    p += strlen(want);
    const char *bar = strchr(p, '|');
    if (!bar || (size_t) (bar - p) >= sizeof(r->key)) return;
    memcpy(r->key, p, (size_t) (bar - p));
    r->key[bar - p] = 0;
    if (sscanf(bar + 1, "%d|%d", &r->seg, &r->nseg) == 2) r->found = 1;
}

static void record_set(const HtmlElem owner, const int slot, const char *key, const int seg, const int nseg, const char *attr) {
    char want[16], out[1024];
    snprintf(want, sizeof(want), ";%d|", slot);
    size_t o = 0;
    const char *p = attr;
    while (*p) { // every record but this slot's
        const char *next = strchr(p + 1, ';');
        const size_t len = next ? (size_t) (next - p) : strlen(p);
        if (strncmp(p, want, strlen(want)) && o + len < sizeof(out)) { memcpy(out + o, p, len); o += len; }
        p += len;
    }
    const int n = snprintf(out + o, sizeof(out) - o, ";%d|%s|%d|%d", slot, key, seg, nseg);
    if (n < 0 || (size_t) n >= sizeof(out) - o) return;
    html_dom_set_attr(owner, I18N_ATTR, out);
}

static const char *value_in(const int lang, const char *key) {
    const char *v = lang > 0 ? lookup(lang, key) : NULL;
    return v ? v : lookup(0, key);
}

static int text_is(const char *norm, const char *value, const int seg, const int nseg) {
    if (!value) return 0;
    char *plain = target_text(value, seg, nseg);
    const int same = plain && !strcmp(plain, norm);
    free(plain);
    return same;
}

static const char *map_text(void *user, const HtmlElem owner, const int slot, const char *text) {
    MapCtx *c = user;
    size_t len = strlen(text), lead = 0, trail = 0;
    if (!len || len > 2000) return NULL;
    while (lead < len && isspace((unsigned char) text[lead])) lead++;
    if (lead == len) return NULL;
    while (trail < len - lead && isspace((unsigned char) text[len - 1 - trail])) trail++;
    char *norm = normalise(text, len);
    if (!norm) return NULL;
    char attr[1024];
    Record rec;
    record_get(owner, slot, &rec, attr, sizeof(attr));
    const char *key = NULL;
    int seg = -1, nseg = 1;
    if (rec.found && (text_is(norm, value_in(c->from, rec.key), rec.seg, rec.nseg) ||
                      text_is(norm, value_in(0, rec.key), rec.seg, rec.nseg))) {
        key = rec.key;
        seg = rec.seg;
        nseg = rec.nseg;
    } else {
        const RevEntry *r = NULL;
        int from = c->from;
        if (from >= 0 && g_tables[from].state == 1) r = rev_find(&g_tables[from], norm);
        if (!r && from != 0) { from = 0; r = rev_find(&g_tables[0], norm); }
        if (r && g_locale) { // several keys with this text: the first one the active language translates
            for (const RevEntry *c2 = r; c2; c2 = c2->next >= 0 ? &g_tables[from].rev[c2->next] : NULL) {
                if (lookup(g_locale, g_tables[from].e[c2->entry].key)) { r = c2; break; }
            }
        }
        if (r) {
            key = g_tables[from].e[r->entry].key;
            seg = r->seg;
            nseg = r->nseg;
        }
    }
    free(norm);
    if (!key) return NULL;
    char *plain = target_text(i18n_t(key), seg, nseg);
    if (!plain) return NULL;
    if (owner && (!rec.found || strcmp(rec.key, key) || rec.seg != seg)) record_set(owner, slot, key, seg, nseg, attr);
    const size_t pl = strlen(plain);
    if (lead + pl + trail + 1 > sizeof(c->out)) { free(plain); return NULL; }
    memcpy(c->out, text, lead);
    memcpy(c->out + lead, plain, pl);
    memcpy(c->out + lead + pl, text + len - trail, trail);
    c->out[lead + pl + trail] = 0;
    free(plain);
    return c->out;
}

// Never translated: icon-font ligature names, markup that opts out, and the containers of
// player-made text (chat lines, kill feed, names, scoreboards, server and map lists).
static const char *SKIP_SELECTOR =
    "[translate=\"no\"], .notranslate, .material-icons, .material-icons-outlined, .material-symbols-outlined, "
    "#chatList, #killFeed, #leaderContainer, #leaderContainerD, #leaderContainer0, #leaderContainer1, "
    "#compPlListL, #compPlListR, #specNames, #specNamesF, #mapInfo, #voteKickName, #knDevConsole";

int i18n_translate_tree(const HtmlElem root) {
    i18n_init();
    if (g_tables[0].state != 1) return 0;
    if (!root) {
        // the whole page: from what it shows now
        if (g_dom_locale == g_locale && g_locale == 0) return 0;
    } else if (g_locale == 0 && g_dom_locale == 0) {
        return 0; // fresh English markup in an English page
    }
    MapCtx *c = malloc(sizeof(*c));
    if (!c) return 0;
    c->from = root ? g_locale : g_dom_locale;
    const int n = html_dom_map_text(root, SKIP_SELECTOR, map_text, c);
    free(c);
    if (!root) g_dom_locale = g_locale;
    return n;
}
