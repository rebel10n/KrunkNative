/* knsvc/config.h implementation: JSON document + CLI/env overrides. */
#include "knsvc/config.h"
#include "knsvc/buf.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

extern char **environ;

#define KN_PATH_CAP 4096

typedef struct kn_override {
    char *path;
    char *value;
} kn_override;

struct kn_config {
    cJSON *doc;
    char path[KN_PATH_CAP];
    char root[KN_PATH_CAP];
    kn_override *ovr;     /* CLI + kn_config_set, later entries win */
    size_t novr;
    unsigned char *consumed;
    int argc;
    char **cache;         /* strings handed out by getters (owned) */
    size_t ncache;
};

/* ------------------------------------------------------------------ helpers */

static void set_err(char *err, size_t errlen, const char *fmt, const char *a, const char *b) {
    if (err && errlen) snprintf(err, errlen, fmt, a ? a : "", b ? b : "");
}

static const char *cache_str(const kn_config *ccfg, const char *s, size_t len) {
    kn_config *cfg = (kn_config *)ccfg;
    char **n = realloc(cfg->cache, (cfg->ncache + 1) * sizeof *n);
    if (!n) return NULL;
    cfg->cache = n;
    char *c = malloc(len + 1);
    if (!c) return NULL;
    memcpy(c, s, len);
    c[len] = 0;
    cfg->cache[cfg->ncache++] = c;
    return c;
}

static int file_exists(const char *p) {
    return access(p, R_OK) == 0;
}

static void dirname_into(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash == out) out[1] = 0;
    else if (slash) *slash = 0;
    else snprintf(out, cap, ".");
}

static int find_default_config(char *out, size_t cap) {
    char dir[KN_PATH_CAP];
    if (!getcwd(dir, sizeof dir)) return -1;
    for (int up = 0; up <= 4; up++) {
        char cand[KN_PATH_CAP + 32];
        snprintf(cand, sizeof cand, "%s/config/config.json", dir);
        if (file_exists(cand)) {
            snprintf(out, cap, "%s", cand);
            return 0;
        }
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir) break;
        *slash = 0;
    }
    return -1;
}

static char *read_whole_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    kn_buf b = {0};
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) kn_buf_append(&b, tmp, n);
    fclose(f);
    return kn_buf_detach(&b, len);
}

/* Value text -> JSON node: JSON literal when it parses, else a string. */
static cJSON *value_to_json(const char *v) {
    while (isspace((unsigned char)*v)) v++;
    int looks = *v == '{' || *v == '[' || *v == '"' || *v == '-' || isdigit((unsigned char)*v) ||
                strcmp(v, "true") == 0 || strcmp(v, "false") == 0 || strcmp(v, "null") == 0;
    if (looks) {
        const char *end = NULL;
        cJSON *j = cJSON_ParseWithOpts(v, &end, 0);
        if (j) {
            while (end && isspace((unsigned char)*end)) end++;
            if (end && *end == 0) return j;
            cJSON_Delete(j);
        }
    }
    return cJSON_CreateString(v);
}

static int is_index(const char *s, size_t len) {
    if (!len) return 0;
    for (size_t i = 0; i < len; i++)
        if (!isdigit((unsigned char)s[i])) return 0;
    return 1;
}

/* Find the child of node named by the path segment [seg, seg+len). */
static cJSON *child(const cJSON *node, const char *seg, size_t len) {
    if (cJSON_IsArray(node) && is_index(seg, len)) return cJSON_GetArrayItem(node, atoi(seg));
    if (!cJSON_IsObject(node)) return NULL;
    for (cJSON *c = node->child; c; c = c->next)
        if (c->string && strlen(c->string) == len && memcmp(c->string, seg, len) == 0) return c;
    return NULL;
}

static cJSON *doc_lookup(const cJSON *doc, const char *path) {
    const cJSON *node = doc;
    const char *p = path;
    while (node && *p) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        node = child(node, p, len);
        if (!dot) break;
        p = dot + 1;
    }
    return (cJSON *)node;
}

/* Set path in doc to value (creating intermediate objects). */
static int doc_set(cJSON *doc, const char *path, cJSON *value) {
    cJSON *node = doc;
    const char *p = path;
    for (;;) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        char key[256];
        if (len == 0 || len >= sizeof key) break;
        memcpy(key, p, len);
        key[len] = 0;
        cJSON *c = child(node, p, len);
        if (!dot) {
            if (cJSON_IsArray(node)) {
                if (!c) break;
                return cJSON_ReplaceItemInArray(node, atoi(key), value) ? 0 : -1;
            }
            if (!cJSON_IsObject(node)) break;
            if (c) return cJSON_ReplaceItemInObjectCaseSensitive(node, key, value) ? 0 : -1;
            return cJSON_AddItemToObject(node, key, value) ? 0 : -1;
        }
        if (!c) {
            if (!cJSON_IsObject(node)) break;
            c = cJSON_AddObjectToObject(node, key);
            if (!c) break;
        }
        node = c;
        p = dot + 1;
    }
    cJSON_Delete(value);
    return -1;
}

static void env_name(const char *path, char *out, size_t cap) {
    size_t o = (size_t)snprintf(out, cap, "KN_");
    for (const char *p = path; *p && o + 1 < cap; p++) {
        char c = *p;
        out[o++] = (c == '.' || c == '-') ? '_' : (char)toupper((unsigned char)c);
    }
    out[o] = 0;
}

/* Resolve env tokens (upper-cased, '_' separated) against the existing document:
 * greedy longest key match per level. Returns 0 and the dot path in out. */
static int env_resolve(const cJSON *node, const char *rest, kn_buf *out) {
    if (!*rest) return 0;
    if (!cJSON_IsObject(node) && !cJSON_IsArray(node)) return -1;
    const cJSON *best = NULL;
    size_t best_len = 0;
    int idx = 0;
    for (const cJSON *c = node->child; c; c = c->next, idx++) {
        char key[256];
        if (c->string) {
            env_name(c->string, key, sizeof key);
            memmove(key, key + 3, strlen(key + 3) + 1); /* drop KN_ */
        } else {
            snprintf(key, sizeof key, "%d", idx);
        }
        size_t kl = strlen(key);
        if (kl > best_len && strncmp(rest, key, kl) == 0 && (rest[kl] == 0 || rest[kl] == '_')) {
            /* make sure the remainder resolves below this child */
            kn_buf sub = {0};
            const char *after = rest[kl] ? rest + kl + 1 : rest + kl;
            if (env_resolve(c, after, &sub) == 0) {
                best = c;
                best_len = kl;
                kn_buf_reset(out);
                if (c->string) kn_buf_append_str(out, c->string);
                else kn_buf_appendf(out, "%d", idx);
                if (sub.len) {
                    kn_buf_append_byte(out, '.');
                    kn_buf_append(out, sub.data, sub.len);
                }
            }
            kn_buf_free(&sub);
        }
    }
    return best ? 0 : -1;
}

static void merge_env(kn_config *cfg) {
    for (char **e = environ; e && *e; e++) {
        if (strncmp(*e, "KN_", 3) != 0) continue;
        const char *eq = strchr(*e, '=');
        if (!eq) continue;
        char name[256];
        size_t nl = (size_t)(eq - *e) - 3;
        if (nl == 0 || nl >= sizeof name) continue;
        memcpy(name, *e + 3, nl);
        name[nl] = 0;
        kn_buf path = {0};
        if (env_resolve(cfg->doc, name, &path) == 0 && path.len) doc_set(cfg->doc, path.data, value_to_json(eq + 1));
        kn_buf_free(&path);
    }
}

static int add_override(kn_config *cfg, const char *path, size_t plen, const char *value) {
    kn_override *n = realloc(cfg->ovr, (cfg->novr + 1) * sizeof *n);
    if (!n) return -1;
    cfg->ovr = n;
    char *p = strndup(path, plen);
    char *v = strdup(value);
    if (!p || !v) {
        free(p);
        free(v);
        return -1;
    }
    cfg->ovr[cfg->novr].path = p;
    cfg->ovr[cfg->novr].value = v;
    cfg->novr++;
    return 0;
}

/* --set a.b=v, --set=a.b=v, --a.b=v. Marks consumed args. */
static int parse_args(kn_config *cfg, int argc, char **argv, const char **config_path) {
    cfg->argc = argc;
    cfg->consumed = calloc((size_t)(argc > 0 ? argc : 1), 1);
    if (!cfg->consumed) return -1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *assign = NULL;
        if (strcmp(a, "--config") == 0 && i + 1 < argc) {
            *config_path = argv[i + 1];
            cfg->consumed[i] = cfg->consumed[i + 1] = 1;
            i++;
            continue;
        }
        if (strncmp(a, "--config=", 9) == 0) {
            *config_path = a + 9;
            cfg->consumed[i] = 1;
            continue;
        }
        if (strcmp(a, "--set") == 0 && i + 1 < argc) {
            cfg->consumed[i] = cfg->consumed[i + 1] = 1;
            assign = argv[++i];
        } else if (strncmp(a, "--set=", 6) == 0) {
            cfg->consumed[i] = 1;
            assign = a + 6;
        } else if (strncmp(a, "--", 2) == 0) {
            const char *eq = strchr(a + 2, '=');
            const char *dot = strchr(a + 2, '.');
            if (eq && dot && dot < eq) {
                cfg->consumed[i] = 1;
                assign = a + 2;
            }
        }
        if (!assign) continue;
        const char *eq = strchr(assign, '=');
        if (!eq || eq == assign) continue;
        if (add_override(cfg, assign, (size_t)(eq - assign), eq + 1) != 0) return -1;
    }
    return 0;
}

static void apply_overrides(kn_config *cfg) {
    for (size_t i = 0; i < cfg->novr; i++) doc_set(cfg->doc, cfg->ovr[i].path, value_to_json(cfg->ovr[i].value));
}

/* ------------------------------------------------------------------ load */

static kn_config *config_new(void) {
    kn_config *cfg = calloc(1, sizeof *cfg);
    return cfg;
}

kn_config *kn_config_load(const char *path, int argc, char **argv, char *err, size_t errlen) {
    kn_config *cfg = config_new();
    if (!cfg) return NULL;
    const char *cli_path = NULL;
    if (parse_args(cfg, argc, argv, &cli_path) != 0) goto oom;
    if (!path) path = cli_path;
    if (!path) {
        const char *env = getenv("KN_CONFIG");
        if (env && *env) path = env;
    }
    char found[KN_PATH_CAP];
    if (!path && find_default_config(found, sizeof found) == 0) path = found;

    if (path) {
        size_t len = 0;
        char *text = read_whole_file(path, &len);
        if (!text) {
            set_err(err, errlen, "cannot read config %s: %s", path, strerror(errno));
            kn_config_free(cfg);
            return NULL;
        }
        const char *end = NULL;
        cfg->doc = cJSON_ParseWithLengthOpts(text, len, &end, 0);
        if (!cfg->doc || !cJSON_IsObject(cfg->doc)) {
            char where[64];
            snprintf(where, sizeof where, "near byte %ld", end ? (long)(end - text) : 0L);
            free(text);
            set_err(err, errlen, "invalid JSON in config %s (%s)", path, where);
            kn_config_free(cfg);
            return NULL;
        }
        free(text);
        char abs[KN_PATH_CAP];
        if (!realpath(path, abs)) snprintf(abs, sizeof abs, "%s", path);
        snprintf(cfg->path, sizeof cfg->path, "%s", abs);
        char dir[KN_PATH_CAP];
        dirname_into(abs, dir, sizeof dir);
        dirname_into(dir, cfg->root, sizeof cfg->root);
    } else {
        cfg->doc = cJSON_CreateObject();
        if (!getcwd(cfg->root, sizeof cfg->root)) snprintf(cfg->root, sizeof cfg->root, ".");
    }
    if (!cfg->doc) goto oom;
    merge_env(cfg);
    apply_overrides(cfg);
    return cfg;
oom:
    set_err(err, errlen, "out of memory%s%s", NULL, NULL);
    kn_config_free(cfg);
    return NULL;
}

kn_config *kn_config_from_string(const char *json, char *err, size_t errlen) {
    kn_config *cfg = config_new();
    if (!cfg) return NULL;
    cfg->doc = cJSON_Parse(json ? json : "{}");
    if (!cfg->doc || !cJSON_IsObject(cfg->doc)) {
        set_err(err, errlen, "invalid JSON config%s%s", NULL, NULL);
        kn_config_free(cfg);
        return NULL;
    }
    if (!getcwd(cfg->root, sizeof cfg->root)) snprintf(cfg->root, sizeof cfg->root, ".");
    return cfg;
}

void kn_config_free(kn_config *cfg) {
    if (!cfg) return;
    cJSON_Delete(cfg->doc);
    for (size_t i = 0; i < cfg->novr; i++) {
        free(cfg->ovr[i].path);
        free(cfg->ovr[i].value);
    }
    free(cfg->ovr);
    for (size_t i = 0; i < cfg->ncache; i++) free(cfg->cache[i]);
    free(cfg->cache);
    free(cfg->consumed);
    free(cfg);
}

const char *kn_config_path(const kn_config *cfg) { return cfg->path; }
const char *kn_config_root(const kn_config *cfg) { return cfg->root; }

char *kn_config_resolve_path(const kn_config *cfg, const char *rel, char *out, size_t cap) {
    int n = rel[0] == '/' ? snprintf(out, cap, "%s", rel) : snprintf(out, cap, "%s/%s", cfg->root, rel);
    return (n < 0 || (size_t)n >= cap) ? NULL : out;
}

int kn_config_set(kn_config *cfg, const char *path, const char *value) {
    if (add_override(cfg, path, strlen(path), value) != 0) return -1;
    doc_set(cfg->doc, path, value_to_json(value));
    return 0;
}

int kn_config_arg_consumed(const kn_config *cfg, int i) {
    return cfg->consumed && i >= 0 && i < cfg->argc && cfg->consumed[i];
}

/* ------------------------------------------------------------------ getters */

/* Override text for path (CLI/set first, then env), or NULL. */
static const char *override_text(const kn_config *cfg, const char *path) {
    for (size_t i = cfg->novr; i-- > 0;)
        if (strcmp(cfg->ovr[i].path, path) == 0) return cfg->ovr[i].value;
    char name[512];
    env_name(path, name, sizeof name);
    return getenv(name);
}

const cJSON *kn_config_get(const kn_config *cfg, const char *path) {
    return doc_lookup(cfg->doc, path);
}

int kn_config_has(const kn_config *cfg, const char *path) {
    return override_text(cfg, path) != NULL || doc_lookup(cfg->doc, path) != NULL;
}

/* Scalar view of path: override text, or a document node. */
static const cJSON *scalar(const kn_config *cfg, const char *path, const char **text) {
    *text = override_text(cfg, path);
    return *text ? NULL : doc_lookup(cfg->doc, path);
}

const char *kn_config_str(const kn_config *cfg, const char *path, const char *def) {
    const char *text;
    const cJSON *n = scalar(cfg, path, &text);
    if (text) {
        size_t len = strlen(text);
        if (len >= 2 && text[0] == '"' && text[len - 1] == '"') {
            cJSON *j = cJSON_Parse(text);
            const char *r = cJSON_IsString(j) ? cache_str(cfg, j->valuestring, strlen(j->valuestring)) : text;
            cJSON_Delete(j);
            return r;
        }
        return text;
    }
    if (cJSON_IsString(n)) return n->valuestring;
    if (cJSON_IsNumber(n)) {
        char tmp[64];
        double d = n->valuedouble;
        if (d == (double)(long long)d) snprintf(tmp, sizeof tmp, "%lld", (long long)d);
        else snprintf(tmp, sizeof tmp, "%.17g", d);
        return cache_str(cfg, tmp, strlen(tmp));
    }
    if (cJSON_IsBool(n)) return cJSON_IsTrue(n) ? "true" : "false";
    return def;
}

static int parse_bool_text(const char *s, int def) {
    if (!s) return def;
    if (!strcasecmp(s, "true") || !strcasecmp(s, "1") || !strcasecmp(s, "yes") || !strcasecmp(s, "on")) return 1;
    if (!strcasecmp(s, "false") || !strcasecmp(s, "0") || !strcasecmp(s, "no") || !strcasecmp(s, "off")) return 0;
    return def;
}

double kn_config_num(const kn_config *cfg, const char *path, double def) {
    const char *text;
    const cJSON *n = scalar(cfg, path, &text);
    if (!text && cJSON_IsString(n)) text = n->valuestring;
    if (text) {
        char *end;
        errno = 0;
        double d = strtod(text, &end);
        if (end != text && *end == 0 && !errno) return d;
        int b = parse_bool_text(text, -1);
        return b >= 0 ? b : def;
    }
    if (cJSON_IsNumber(n)) return n->valuedouble;
    if (cJSON_IsBool(n)) return cJSON_IsTrue(n);
    return def;
}

int64_t kn_config_int(const kn_config *cfg, const char *path, int64_t def) {
    const char *text;
    const cJSON *n = scalar(cfg, path, &text);
    if (!text && cJSON_IsString(n)) text = n->valuestring;
    if (text) {
        char *end;
        errno = 0;
        long long v = strtoll(text, &end, 0);
        if (end != text && *end == 0 && !errno) return v;
        double d = strtod(text, &end);
        if (end != text && *end == 0) return (int64_t)d;
        int b = parse_bool_text(text, -1);
        return b >= 0 ? b : def;
    }
    if (cJSON_IsNumber(n)) return (int64_t)n->valuedouble;
    if (cJSON_IsBool(n)) return cJSON_IsTrue(n);
    return def;
}

int kn_config_bool(const kn_config *cfg, const char *path, int def) {
    const char *text;
    const cJSON *n = scalar(cfg, path, &text);
    if (!text && cJSON_IsString(n)) text = n->valuestring;
    if (text) return parse_bool_text(text, def);
    if (cJSON_IsBool(n)) return cJSON_IsTrue(n);
    if (cJSON_IsNumber(n)) return n->valuedouble != 0;
    return def;
}

size_t kn_config_str_list(const kn_config *cfg, const char *path, const char **out, size_t max) {
    const char *text = override_text(cfg, path);
    const cJSON *n = doc_lookup(cfg->doc, path);
    size_t count = 0;
    if (!text && cJSON_IsArray(n)) {
        for (const cJSON *c = n->child; c; c = c->next) {
            if (!cJSON_IsString(c)) continue;
            if (count < max) out[count] = c->valuestring;
            count++;
        }
        return count;
    }
    if (!text && cJSON_IsString(n)) text = n->valuestring;
    if (!text) return 0;
    const char *p = text;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        while (len && p[len - 1] == ' ') len--;
        if (count < max) out[count] = cache_str(cfg, p, len);
        count++;
        p += len;
    }
    return count;
}

/* ------------------------------------------------------------------ dump */

static int secret_key(const char *k) {
    static const char *const words[] = {"password", "secret", "token", "key"};
    for (size_t i = 0; i < sizeof words / sizeof *words; i++)
        if (strcasestr(k, words[i])) return 1;
    return 0;
}

static void redact(cJSON *node) {
    for (cJSON *c = node ? node->child : NULL; c; c = c->next) {
        if (c->string && secret_key(c->string) && !cJSON_IsObject(c) && !cJSON_IsArray(c)) {
            char *key = strdup(c->string);
            cJSON *r = cJSON_CreateString("***");
            if (!key || !r || !cJSON_ReplaceItemInObjectCaseSensitive(node, key, r)) {
                free(key);
                cJSON_Delete(r);
                return;
            }
            free(key);
            c = r;
            continue;
        }
        redact(c);
    }
}

char *kn_config_dump_redacted(const kn_config *cfg) {
    cJSON *copy = cJSON_Duplicate(cfg->doc, 1);
    if (!copy) return NULL;
    redact(copy);
    char *s = cJSON_Print(copy);
    cJSON_Delete(copy);
    return s;
}
