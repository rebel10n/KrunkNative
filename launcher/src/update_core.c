// Client updates, the pure half (launcher/include/kn_update.h).
#include <kn_update.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_s(char *dst, const size_t n, const char *src) {
    snprintf(dst, n, "%s", src ? src : "");
}

static void set_err(char *err, const size_t errn, const char *msg) {
    if (err && errn) snprintf(err, errn, "%s", msg);
}

static const char *json_str(const cJSON *o, const char *key) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, key));
    return s ? s : "";
}

static int64_t json_i64(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 9.0e15) return 0;
    return (int64_t) v->valuedouble;
}

static int is_hex(const char *s, const size_t min, const size_t max) {
    size_t n = 0;
    if (!s) return 0;
    for (; s[n]; n++)
        if (!isxdigit((unsigned char) s[n])) return 0;
    return n >= min && n <= max;
}

static void lower(char *s) {
    for (; *s; s++) *s = (char) tolower((unsigned char) *s);
}

// ---------------------------------------------------------------- builds

int kn_update_parse_protocol(const char *s, int *major, int *minor) {
    *major = *minor = -1;
    if (!s || !isdigit((unsigned char) *s)) return 0;
    char *end = NULL;
    const long ma = strtol(s, &end, 10);
    if (!end || *end != '.' || !isdigit((unsigned char) end[1])) return 0;
    const long mi = strtol(end + 1, &end, 10);
    if (*end || ma > 255 || mi > 255) return 0;
    *major = (int) ma;
    *minor = (int) mi;
    return 1;
}

int kn_update_parse_build(const cJSON *o, KnUpdateBuild *out) {
    memset(out, 0, sizeof(*out));
    out->proto_major = out->proto_minor = -1;
    if (!cJSON_IsObject(o)) return 0;
    const char *commit = json_str(o, "commit");
    if (!is_hex(commit, 7, sizeof(out->commit) - 1)) return 0;
    copy_s(out->commit, sizeof(out->commit), commit);
    lower(out->commit);
    copy_s(out->describe, sizeof(out->describe), json_str(o, "describe"));
    if (!out->describe[0]) copy_s(out->describe, sizeof(out->describe), out->commit);
    out->commit_time = json_i64(o, "commit_time");
    copy_s(out->build_time, sizeof(out->build_time), json_str(o, "build_time"));
    kn_update_parse_protocol(json_str(o, "protocol"), &out->proto_major, &out->proto_minor);
    return 1;
}

int kn_update_same_commit(const char *a, const char *b) {
    if (!is_hex(a, 7, 64) || !is_hex(b, 7, 64)) return 0;
    const size_t la = strlen(a), lb = strlen(b), n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char) a[i]) != tolower((unsigned char) b[i])) return 0;
    return 1;
}

KnUpdateState kn_update_compare(const KnUpdateBuild *mine, const KnUpdateBuild *latest, const int64_t grace_s) {
    if (!latest || !latest->commit[0]) return KN_UPDATE_CURRENT; // nothing published
    if (kn_update_same_commit(mine->commit, latest->commit)) return KN_UPDATE_CURRENT;
    // a protocol the latest build moved past: this client can't talk to current servers
    const int newer_proto = latest->proto_major >= 0 && mine->proto_major >= 0 &&
                            (latest->proto_major > mine->proto_major ||
                             (latest->proto_major == mine->proto_major && latest->proto_minor > mine->proto_minor));
    if (newer_proto) return KN_UPDATE_OUTDATED;
    if (latest->commit_time <= 0) return KN_UPDATE_CURRENT;
    if (mine->commit_time <= 0) return KN_UPDATE_OUTDATED; // no build info: never matches anything
    if (mine->commit_time >= latest->commit_time) return KN_UPDATE_CURRENT; // a newer (dev) build
    return mine->commit_time < latest->commit_time - (grace_s > 0 ? grace_s : 0) ? KN_UPDATE_OUTDATED
                                                                                  : KN_UPDATE_AVAILABLE;
}

const char *kn_update_state_name(const KnUpdateState s) {
    return s == KN_UPDATE_OUTDATED ? "outdated" : s == KN_UPDATE_AVAILABLE ? "available" : "current";
}

// ---------------------------------------------------------------- URLs and paths

static int is_http_url(const char *s) {
    return !strncmp(s, "http://", 7) || !strncmp(s, "https://", 8);
}

static int url_chars_ok(const char *s) {
    for (; *s; s++)
        if ((unsigned char) *s <= 0x20 || *s == '"' || *s == '\'' || *s == '\\' || *s == '<' || *s == '>' ||
            (unsigned char) *s == 0x7f)
            return 0;
    return 1;
}

int kn_update_resolve_url(const char *base, const char *ref, char *out, const size_t n) {
    if (!out || !n) return 0;
    out[0] = 0;
    if (!ref || !*ref || !url_chars_ok(ref)) return 0;
    if (is_http_url(ref)) {
        if (strlen(ref) >= n) return 0;
        copy_s(out, n, ref);
        return 1;
    }
    if (!base || !is_http_url(base) || !url_chars_ok(base) || strstr(ref, "://")) return 0;
    const char *host = strstr(base, "://") + 3;
    const char *path = strchr(host, '/');
    const size_t origin_len = path ? (size_t) (path - base) : strlen(base);
    char buf[2048];
    if (ref[0] == '/') {
        if (origin_len + strlen(ref) >= sizeof(buf)) return 0;
        snprintf(buf, sizeof(buf), "%.*s%s", (int) origin_len, base, ref);
    } else {
        // base directory: up to the last '/' of the path (query/fragment cut first)
        size_t dir_end = origin_len;
        if (path) {
            const char *q = path + strcspn(path, "?#");
            for (const char *p = path; p < q; p++)
                if (*p == '/') dir_end = (size_t) (p - base) + 1;
        }
        if (dir_end == origin_len) { // no path: "/"
            if (origin_len + 1 + strlen(ref) >= sizeof(buf)) return 0;
            snprintf(buf, sizeof(buf), "%.*s/%s", (int) origin_len, base, ref);
        } else {
            if (dir_end + strlen(ref) >= sizeof(buf)) return 0;
            snprintf(buf, sizeof(buf), "%.*s%s", (int) dir_end, base, ref);
        }
    }
    // normalise "." and ".." segments of the path (RFC 3986 5.2.4; never above the origin)
    const char *path_start = buf + origin_len;
    const size_t path_len = strcspn(path_start, "?#");
    const char *tail = path_start + path_len;
    const char *segs[256];
    size_t lens[256], count = 0;
    int trailing_slash = path_len > 0 && path_start[path_len - 1] == '/';
    for (const char *p = path_start; p < tail;) {
        while (p < tail && *p == '/') p++;
        if (p >= tail) break;
        const char *seg = p;
        while (p < tail && *p != '/') p++;
        const size_t sl = (size_t) (p - seg);
        const int last = p >= tail;
        if (sl == 1 && seg[0] == '.') {
            if (last) trailing_slash = 1;
        } else if (sl == 2 && seg[0] == '.' && seg[1] == '.') {
            if (count) count--;
            if (last) trailing_slash = 1;
        } else {
            if (count >= 256) return 0;
            segs[count] = seg;
            lens[count++] = sl;
        }
    }
    char res[2048];
    size_t rl = origin_len;
    memcpy(res, buf, origin_len);
    for (size_t i = 0; i < count; i++) {
        if (rl + 1 + lens[i] >= sizeof(res)) return 0;
        res[rl++] = '/';
        memcpy(res + rl, segs[i], lens[i]);
        rl += lens[i];
    }
    if (!count || trailing_slash) res[rl++] = '/';
    const size_t tl = strlen(tail);
    if (rl + tl >= sizeof(res) || rl + tl >= n) return 0;
    memcpy(res + rl, tail, tl + 1);
    copy_s(out, n, res);
    return 1;
}

int kn_update_path_ok(const char *rel) {
    if (!rel || !*rel || rel[0] == '/' || strlen(rel) >= 400) return 0;
    const char *seg = rel;
    for (const char *p = rel;; p++) {
        if (*p == '/' || !*p) {
            const size_t sl = (size_t) (p - seg);
            if (!sl || (sl == 1 && seg[0] == '.') || (sl == 2 && seg[0] == '.' && seg[1] == '.')) return 0;
            if (!*p) break;
            seg = p + 1;
            continue;
        }
        if ((unsigned char) *p < 0x20 || *p == '\\' || *p == ':' || (unsigned char) *p == 0x7f) return 0;
    }
    return 1;
}

// ---------------------------------------------------------------- channel manifest

static int str_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *) a, *(const char *const *) b);
}

static int in_sorted(char *const *list, const size_t n, const char *path) {
    return list && n && path && bsearch(&path, list, n, sizeof(char *), str_cmp) != NULL;
}

int kn_update_is_packaged(const KnUpdateManifest *m, const char *path) {
    return m && in_sorted(m->assets.packaged, m->assets.packaged_count, path);
}

static int parse_file(const cJSON *o, const char *base, KnUpdateFile *out) {
    memset(out, 0, sizeof(*out));
    const char *sha = json_str(o, "sha256");
    if (!cJSON_IsObject(o) || !is_hex(sha, 64, 64)) return 0;
    if (!kn_update_resolve_url(base, json_str(o, "url"), out->url, sizeof(out->url))) return 0;
    copy_s(out->sha256, sizeof(out->sha256), sha);
    lower(out->sha256);
    out->size = json_i64(o, "size");
    return out->size > 0;
}

static int filter_add(KnPathFilter *f, const char *prefix); // origin path filters, below

int kn_update_manifest_parse(const char *json, const size_t len, const char *manifest_url, KnUpdateManifest *out,
                             char *err, const size_t errn) {
    memset(out, 0, sizeof(*out));
    out->build.proto_major = out->build.proto_minor = -1;
    cJSON *root = json ? cJSON_ParseWithLength(json, len) : NULL;
    if (!cJSON_IsObject(root)) {
        set_err(err, errn, "manifest is not a JSON object");
        cJSON_Delete(root);
        return 0;
    }
    out->format = (int) json_i64(root, "format");
    if (out->format != KN_UPDATE_FORMAT) {
        set_err(err, errn, "unsupported manifest format");
        cJSON_Delete(root);
        return 0;
    }
    copy_s(out->channel, sizeof(out->channel), json_str(root, "channel"));
    if (!kn_update_parse_build(cJSON_GetObjectItemCaseSensitive(root, "build"), &out->build)) {
        set_err(err, errn, "manifest has no build");
        cJSON_Delete(root);
        return 0;
    }
    const cJSON *grace = cJSON_GetObjectItemCaseSensitive(root, "outdated_grace_h");
    out->grace_s = cJSON_IsNumber(grace) && grace->valuedouble >= 0 && grace->valuedouble < 1e6
                       ? (int64_t) (grace->valuedouble * 3600)
                       : KN_UPDATE_GRACE_DEFAULT_S;
    if (!kn_update_resolve_url(manifest_url, json_str(root, "page"), out->page, sizeof(out->page))) out->page[0] = 0;

    const cJSON *plats = cJSON_GetObjectItemCaseSensitive(root, "platforms");
    const int count = cJSON_IsObject(plats) ? cJSON_GetArraySize(plats) : 0;
    if (count > 0) {
        out->platforms = calloc((size_t) count, sizeof(*out->platforms));
        if (!out->platforms) {
            set_err(err, errn, "out of memory");
            cJSON_Delete(root);
            return 0;
        }
    }
    const cJSON *p;
    cJSON_ArrayForEach(p, plats) {
        if (!cJSON_IsObject(p) || !p->string || !p->string[0] || strlen(p->string) >= sizeof(out->platforms[0].id))
            continue;
        KnUpdatePlatform *pl = &out->platforms[out->platform_count];
        memset(pl, 0, sizeof(*pl));
        copy_s(pl->id, sizeof(pl->id), p->string);
        pl->has_update = parse_file(cJSON_GetObjectItemCaseSensitive(p, "update"), manifest_url, &pl->update);
        const cJSON *dl;
        cJSON_ArrayForEach(dl, cJSON_GetObjectItemCaseSensitive(p, "downloads")) {
            if (pl->link_count >= KN_UPDATE_MAX_LINKS || !dl->string || strlen(dl->string) >= sizeof(pl->links[0].kind))
                continue;
            KnUpdateLink *l = &pl->links[pl->link_count];
            if (!kn_update_resolve_url(manifest_url, cJSON_GetStringValue(dl), l->url, sizeof(l->url))) continue;
            copy_s(l->kind, sizeof(l->kind), dl->string);
            pl->link_count++;
        }
        out->platform_count++;
    }

    const cJSON *a = cJSON_GetObjectItemCaseSensitive(root, "assets");
    const char *ver = json_str(a, "version");
    if (cJSON_IsObject(a) && ver[0]) {
        int ok = strlen(ver) < sizeof(out->assets.version) && kn_update_path_ok(ver) && !strchr(ver, '/');
        KnUpdateFile mf = {0};
        const char *murl = json_str(a, "manifest");
        ok = ok && kn_update_resolve_url(manifest_url, murl, mf.url, sizeof(mf.url));
        const char *msha = json_str(a, "sha256");
        if (ok && msha[0]) {
            ok = is_hex(msha, 64, 64);
            copy_s(mf.sha256, sizeof(mf.sha256), msha);
            lower(mf.sha256);
        }
        mf.size = json_i64(a, "size");
        ok = ok && kn_update_resolve_url(manifest_url, json_str(a, "objects"), out->assets.objects, sizeof(out->assets.objects));
        if (ok) {
            const size_t ol = strlen(out->assets.objects);
            if (ol && out->assets.objects[ol - 1] != '/' && ol + 1 < sizeof(out->assets.objects)) {
                out->assets.objects[ol] = '/';
                out->assets.objects[ol + 1] = 0;
            }
            copy_s(out->assets.version, sizeof(out->assets.version), ver);
            out->assets.manifest = mf;
            out->assets.files = json_i64(a, "files");
            out->assets.bytes = json_i64(a, "bytes");
            const cJSON *packaged = cJSON_GetObjectItemCaseSensitive(a, "packaged");
            const int pn = cJSON_IsArray(packaged) ? cJSON_GetArraySize(packaged) : 0;
            out->assets.packaged = pn > 0 ? calloc((size_t) pn, sizeof(char *)) : NULL;
            const cJSON *pe;
            cJSON_ArrayForEach(pe, packaged) {
                const char *path = cJSON_GetStringValue(pe);
                if (!out->assets.packaged || !kn_update_path_ok(path)) continue;
                out->assets.packaged[out->assets.packaged_count] = strdup(path);
                if (out->assets.packaged[out->assets.packaged_count]) out->assets.packaged_count++;
            }
            if (out->assets.packaged_count > 1)
                qsort(out->assets.packaged, out->assets.packaged_count, sizeof(char *), str_cmp);
            const cJSON *mirrors = cJSON_GetObjectItemCaseSensitive(a, "mirrors");
            out->assets.mirrors_set = cJSON_IsArray(mirrors);
            const cJSON *me;
            cJSON_ArrayForEach(me, mirrors) {
                if (out->assets.mirror_count >= KN_UPDATE_MAX_MIRRORS) break;
                const char *murl = cJSON_IsString(me) ? me->valuestring : json_str(me, "url");
                const char *layout = cJSON_IsObject(me) ? json_str(me, "layout") : "";
                if (layout[0] && strcmp(layout, "path") != 0 && strcmp(layout, "objects") != 0) continue;
                char *dst = out->assets.mirrors[out->assets.mirror_count].url;
                if (!murl || !murl[0] || !kn_update_resolve_url(manifest_url, murl, dst, KN_UPDATE_URL_MAX - 1)) continue;
                const size_t dl = strlen(dst);
                if (dst[dl - 1] != '/') strcat(dst, "/");
                // "only" / "skip" (by path only): one part of a split CDN. Unusable = not this mirror.
                KnPathFilter *flt = &out->assets.mirrors[out->assets.mirror_count].filter;
                memset(flt, 0, sizeof(*flt));
                const cJSON *only = cJSON_IsObject(me) ? cJSON_GetObjectItemCaseSensitive(me, "only") : NULL;
                const cJSON *skip = cJSON_IsObject(me) ? cJSON_GetObjectItemCaseSensitive(me, "skip") : NULL;
                const cJSON *list = only ? only : skip;
                int usable = !(only && skip) && (!list || (cJSON_IsArray(list) && strcmp(layout, "objects") != 0));
                if (usable && list) {
                    flt->mode = only ? KN_PATH_ONLY : KN_PATH_SKIP;
                    const cJSON *pe2;
                    cJSON_ArrayForEach(pe2, list) usable = usable && filter_add(flt, cJSON_GetStringValue(pe2));
                    if (only && !flt->list[0]) usable = 0; // serves nothing
                    if (!flt->list[0]) flt->mode = KN_PATH_ALL;
                }
                if (!usable) continue;
                out->assets.mirrors[out->assets.mirror_count].by_path = strcmp(layout, "objects") != 0;
                out->assets.mirror_count++;
            }
        } else {
            out->assets.objects[0] = 0;
        }
    }
    cJSON_Delete(root);
    return 1;
}

// ---------------------------------------------------------------- origin path filters

int kn_path_filter_ok(const KnPathFilter *f, const char *path) {
    if (!f || f->mode == KN_PATH_ALL || !path) return 1;
    int hit = 0;
    for (const char *t = f->list; *t && !hit;) {
        const char *end = strchr(t, '+');
        const size_t len = end ? (size_t) (end - t) : strlen(t);
        hit = len && !strncmp(path, t, len);
        t += len + (end ? 1 : 0);
    }
    return f->mode == KN_PATH_ONLY ? hit : !hit;
}

// A prefix of a filter: a path below assets/ with an optional trailing '/' (no '+').
static int filter_prefix_ok(const char *p) {
    if (!p || !*p || strchr(p, '+') || strchr(p, '@') || strchr(p, ' ') || strchr(p, ',')) return 0;
    char tmp[400];
    const size_t len = strlen(p);
    if (len >= sizeof(tmp)) return 0;
    memcpy(tmp, p, len + 1);
    if (tmp[len - 1] == '/') tmp[len - 1] = 0;
    return kn_update_path_ok(tmp);
}

static int filter_add(KnPathFilter *f, const char *prefix) {
    const size_t used = strlen(f->list), len = strlen(prefix);
    if (!filter_prefix_ok(prefix) || used + len + 2 > sizeof(f->list)) return 0;
    snprintf(f->list + used, sizeof(f->list) - used, "%s%s", used ? "+" : "", prefix);
    return 1;
}

int kn_origin_spec_parse(const char *spec, char *url, const size_t urln, int *by_path, KnPathFilter *f) {
    KnPathFilter none = {0};
    if (f) *f = none;
    if (by_path) *by_path = 1;
    if (!spec || !url || !urln) return 0;
    if (!strncmp(spec, "objects:", 8)) {
        if (by_path) *by_path = 0;
        spec += 8;
    } else if (!strncmp(spec, "only=", 5) || !strncmp(spec, "skip=", 5)) {
        const char *at = strchr(spec, '@');
        if (!at) return 0;
        KnPathFilter got = {.mode = spec[0] == 'o' ? KN_PATH_ONLY : KN_PATH_SKIP};
        char list[KN_PATH_FILTER_MAX];
        const size_t len = (size_t) (at - spec - 5);
        if (!len || len >= sizeof(list)) return 0;
        memcpy(list, spec + 5, len);
        list[len] = 0;
        for (char *t = list, *end; t; t = end) {
            end = strchr(t, '+');
            if (end) *end++ = 0;
            if (!filter_add(&got, t)) return 0;
        }
        if (f) *f = got;
        spec = at + 1;
    }
    if (strncmp(spec, "http://", 7) != 0 && strncmp(spec, "https://", 8) != 0) return 0;
    const size_t len = strlen(spec);
    if (len + 2 > urln) return 0;
    snprintf(url, urln, "%s%s", spec, spec[len - 1] == '/' ? "" : "/");
    return 1;
}

int kn_origin_spec_format(char *out, const size_t n, const char *url, const int by_path, const KnPathFilter *f) {
    if (!out || !n || !url) return 0;
    int w;
    if (!by_path) w = snprintf(out, n, "objects:%s", url);
    else if (f && f->mode != KN_PATH_ALL && f->list[0])
        w = snprintf(out, n, "%s=%s@%s", f->mode == KN_PATH_ONLY ? "only" : "skip", f->list, url);
    else w = snprintf(out, n, "%s", url);
    return w > 0 && (size_t) w < n;
}

void kn_update_manifest_free(KnUpdateManifest *m) {
    if (!m) return;
    for (size_t i = 0; i < m->assets.packaged_count; i++) free(m->assets.packaged[i]);
    free(m->assets.packaged);
    m->assets.packaged = NULL;
    m->assets.packaged_count = 0;
    free(m->platforms);
    m->platforms = NULL;
    m->platform_count = 0;
}

const KnUpdatePlatform *kn_update_manifest_platform(const KnUpdateManifest *m, const char *id) {
    for (int i = 0; m && id && i < m->platform_count; i++)
        if (!strcmp(m->platforms[i].id, id)) return &m->platforms[i];
    return NULL;
}

const char *kn_update_download_url(const KnUpdateManifest *m, const char *platform, const char *kind) {
    const KnUpdatePlatform *p = kn_update_manifest_platform(m, platform);
    const char *archive = kn_update_archive_kind(platform);
    for (int pass = 0; p && pass < 2; pass++) {
        const char *want = pass == 0 ? (kind && *kind ? kind : archive) : archive;
        for (int i = 0; i < p->link_count; i++)
            if (!strcmp(p->links[i].kind, want)) return p->links[i].url;
    }
    if (m && m->page[0]) return m->page;
    return "";
}

// ---------------------------------------------------------------- asset lists

static int entry_cmp(const void *a, const void *b) {
    return strcmp(((const KnAssetEntry *) a)->path, ((const KnAssetEntry *) b)->path);
}

void kn_asset_list_free(KnAssetList *l) {
    if (!l) return;
    for (size_t i = 0; i < l->count; i++) free(l->files[i].path);
    free(l->files);
    l->files = NULL;
    l->count = 0;
}

int kn_asset_list_parse(const char *json, const size_t len, KnAssetList *out, char *err, const size_t errn) {
    memset(out, 0, sizeof(*out));
    cJSON *root = json ? cJSON_ParseWithLength(json, len) : NULL;
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(root, "files");
    if (!cJSON_IsObject(root) || json_i64(root, "format") != KN_UPDATE_FORMAT || !cJSON_IsArray(files)) {
        set_err(err, errn, "not an asset list");
        cJSON_Delete(root);
        return 0;
    }
    const char *ver = json_str(root, "version");
    copy_s(out->version, sizeof(out->version), strlen(ver) < sizeof(out->version) ? ver : "");
    const int n = cJSON_GetArraySize(files);
    out->files = n > 0 ? calloc((size_t) n, sizeof(*out->files)) : NULL;
    if (n > 0 && !out->files) {
        set_err(err, errn, "out of memory");
        cJSON_Delete(root);
        return 0;
    }
    const cJSON *f;
    int ok = 1;
    cJSON_ArrayForEach(f, files) {
        const char *path = cJSON_GetStringValue(cJSON_GetArrayItem(f, 0));
        const char *sha = cJSON_GetStringValue(cJSON_GetArrayItem(f, 1));
        const cJSON *size = cJSON_GetArrayItem(f, 2);
        if (!kn_update_path_ok(path) || !is_hex(sha, 64, 64) || !cJSON_IsNumber(size) || size->valuedouble < 0 ||
            size->valuedouble > 9.0e15 || !strncmp(path, ".kn-", 4)) {
            set_err(err, errn, "bad asset entry");
            ok = 0;
            break;
        }
        KnAssetEntry *e = &out->files[out->count];
        e->path = strdup(path);
        if (!e->path) {
            set_err(err, errn, "out of memory");
            ok = 0;
            break;
        }
        copy_s(e->sha256, sizeof(e->sha256), sha);
        lower(e->sha256);
        e->size = (int64_t) size->valuedouble;
        out->count++;
    }
    cJSON_Delete(root);
    if (ok && out->count > 1) {
        qsort(out->files, out->count, sizeof(*out->files), entry_cmp);
        for (size_t i = 1; i < out->count; i++)
            if (!strcmp(out->files[i - 1].path, out->files[i].path)) {
                set_err(err, errn, "duplicate asset path");
                ok = 0;
                break;
            }
    }
    if (!ok) kn_asset_list_free(out);
    return ok;
}

char *kn_asset_list_json(const KnAssetList *l) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "format", KN_UPDATE_FORMAT);
    cJSON_AddStringToObject(root, "version", l->version);
    cJSON *files = cJSON_AddArrayToObject(root, "files");
    for (size_t i = 0; files && i < l->count; i++) {
        cJSON *e = cJSON_CreateArray();
        cJSON_AddItemToArray(e, cJSON_CreateString(l->files[i].path));
        cJSON_AddItemToArray(e, cJSON_CreateString(l->files[i].sha256));
        cJSON_AddItemToArray(e, cJSON_CreateNumber((double) l->files[i].size));
        cJSON_AddItemToArray(files, e);
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

const KnAssetEntry *kn_asset_list_find(const KnAssetList *l, const char *path) {
    if (!l || !l->count || !path) return NULL;
    const KnAssetEntry key = {.path = (char *) path};
    return bsearch(&key, l->files, l->count, sizeof(*l->files), entry_cmp);
}

int64_t kn_asset_list_bytes(const KnAssetList *l) {
    int64_t sum = 0;
    for (size_t i = 0; l && i < l->count; i++) sum += l->files[i].size;
    return sum;
}

void kn_asset_diff_free(KnAssetDiff *d) {
    if (!d) return;
    free(d->fetch);
    for (size_t i = 0; i < d->remove_count; i++) free(d->remove[i]);
    free(d->remove);
    memset(d, 0, sizeof(*d));
}

int kn_asset_diff(const KnAssetList *want, const KnAssetList *have, char *const *keep, const size_t keep_count,
                  const KnAssetStatFn stat, const KnAssetHashFn hash, void *user, KnAssetDiff *out) {
    memset(out, 0, sizeof(*out));
    if (want->count) {
        out->fetch = malloc(want->count * sizeof(*out->fetch));
        if (!out->fetch) return 0;
    }
    for (size_t i = 0; i < want->count; i++) {
        const KnAssetEntry *w = &want->files[i];
        const int64_t size = stat ? stat(user, w->path) : -1;
        if (size == w->size) {
            const KnAssetEntry *h = kn_asset_list_find(have, w->path);
            if (h && !strcmp(h->sha256, w->sha256) && h->size == w->size) {
                out->kept++;
                continue;
            }
            char hex[65];
            if (hash) {
                out->hashed++;
                if (hash(user, w->path, hex) && !strcmp(hex, w->sha256)) {
                    out->adopted++;
                    continue;
                }
            }
        }
        out->fetch[out->fetch_count++] = i;
        out->fetch_bytes += w->size;
    }
    for (size_t i = 0; have && i < have->count; i++) {
        if (kn_asset_list_find(want, have->files[i].path) || in_sorted(keep, keep_count, have->files[i].path)) continue;
        char **grown = realloc(out->remove, (out->remove_count + 1) * sizeof(*grown));
        if (!grown) {
            kn_asset_diff_free(out);
            return 0;
        }
        out->remove = grown;
        out->remove[out->remove_count] = strdup(have->files[i].path);
        if (!out->remove[out->remove_count]) {
            kn_asset_diff_free(out);
            return 0;
        }
        out->remove_count++;
    }
    return 1;
}

// ---------------------------------------------------------------- install marker, platform

int kn_install_marker_parse(const char *json, const size_t len, KnInstallMarker *out) {
    memset(out, 0, sizeof(*out));
    cJSON *root = json ? cJSON_ParseWithLength(json, len) : NULL;
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return 0;
    }
    out->format = (int) json_i64(root, "format");
    copy_s(out->platform, sizeof(out->platform), json_str(root, "platform"));
    copy_s(out->package, sizeof(out->package), json_str(root, "package"));
    copy_s(out->channel, sizeof(out->channel), json_str(root, "channel"));
    copy_s(out->root, sizeof(out->root), json_str(root, "root"));
    cJSON_Delete(root);
    if (!out->channel[0]) copy_s(out->channel, sizeof(out->channel), "latest");
    // "." or a chain of ".." only: the root is the marker's directory or one of its parents
    if (!out->root[0]) copy_s(out->root, sizeof(out->root), ".");
    for (const char *p = out->root; *p; p++)
        if (*p != '.' && *p != '/') return 0;
    if (strstr(out->root, "...")) return 0;
    for (const char *p = out->channel; *p; p++)
        if (!isalnum((unsigned char) *p) && *p != '-' && *p != '_' && *p != '.') return 0;
    return out->format >= 1;
}

int kn_install_self_updatable(const KnInstallMarker *m) {
    return m && strcmp(m->package, "deb") != 0 && strcmp(m->package, "rpm") != 0;
}

#define KN_STR2(x) #x
#define KN_STR(x) KN_STR2(x)

const char *kn_update_platform_default(void) {
#if defined(KRUNKNATIVE_PLATFORM_ID)
    return KN_STR(KRUNKNATIVE_PLATFORM_ID);
#elif defined(_WIN32)
#if defined(__aarch64__) || defined(_M_ARM64)
    return "windows-arm64";
#else
    return "windows-x64";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
    return "macos-arm64";
#else
    return "macos-x64";
#endif
#elif defined(__aarch64__)
    return "linux-arm64";
#else
    return "linux-x64";
#endif
}

// The package format of a platform: Linux is a tar.gz, Windows and macOS a zip (BoxOffice-<id>.<ext>).
const char *kn_update_archive_kind(const char *platform) {
    return platform && (!strncmp(platform, "windows-", 8) || !strncmp(platform, "macos-", 6)) ? "zip" : "tar.gz";
}
