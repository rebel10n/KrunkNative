/*
 * services/brain/release_info.c - latest server release + the outdated rule (release_info.h).
 */
#include "release_info.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/json.h"

static void copy_s(char *dst, size_t n, const char *src) {
    snprintf(dst, n, "%s", src ? src : "");
}

int release_parse_protocol(const char *s, int *major, int *minor) {
    *major = *minor = -1;
    if (!s || !isdigit((unsigned char) *s)) return 0;
    char *end = NULL;
    const long ma = strtol(s, &end, 10);
    if (!end || *end != '.' || !isdigit((unsigned char) end[1])) return 0;
    const long mi = strtol(end + 1, &end, 10);
    if (*end || ma < 0 || ma > 255 || mi < 0 || mi > 255) return 0;
    *major = (int) ma;
    *minor = (int) mi;
    return 1;
}

static int is_hex(const char *s) {
    if (!s || !*s) return 0;
    for (; *s; s++)
        if (!isxdigit((unsigned char) *s)) return 0;
    return 1;
}

int release_parse_body(const char *body, ReleaseInfo *out) {
    static const char tag[] = "<!-- kn-build ";
    const char *p = body ? strstr(body, tag) : NULL;
    if (!p) return 0;
    p += sizeof(tag) - 1;
    const char *end = strstr(p, "-->");
    if (!end || end - p > 2048) return 0;
    cJSON *o = cJSON_ParseWithLength(p, (size_t) (end - p));
    if (!cJSON_IsObject(o)) {
        cJSON_Delete(o);
        return 0;
    }
    const char *commit = kn_json_str(o, "commit", "");
    int ok = is_hex(commit) && strlen(commit) >= 7 && strlen(commit) < sizeof(out->commit);
    if (ok) {
        copy_s(out->describe, sizeof(out->describe), kn_json_str(o, "describe", commit));
        copy_s(out->commit, sizeof(out->commit), commit);
        for (char *c = out->commit; *c; c++) *c = (char) tolower((unsigned char) *c);
        const double ct = kn_json_num(o, "commit_time", 0);
        out->commit_time = ct > 0 ? (int64_t) ct : 0;
        copy_s(out->build_time, sizeof(out->build_time), kn_json_str(o, "build_time", ""));
        release_parse_protocol(kn_json_str(o, "protocol", ""), &out->proto_major, &out->proto_minor);
    }
    cJSON_Delete(o);
    return ok;
}

static int server_tag(const char *t) {
    return t && (!strcmp(t, "server-latest") || !strncmp(t, "server-v", 8));
}

static int ends_with(const char *s, const char *suffix) {
    const size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcmp(s + a - b, suffix);
}

int release_parse_release(const cJSON *rel, ReleaseInfo *out) {
    memset(out, 0, sizeof(*out));
    out->proto_major = out->proto_minor = -1;
    const char *t = kn_json_str(rel, "tag_name", "");
    if (!cJSON_IsObject(rel) || !server_tag(t) || kn_json_bool(rel, "draft", 0)) return 0;
    if (!release_parse_body(kn_json_str(rel, "body", ""), out)) return 0;
    copy_s(out->tag, sizeof(out->tag), t);
    copy_s(out->html_url, sizeof(out->html_url), kn_json_str(rel, "html_url", ""));
    const cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(rel, "assets")) {
        const char *name = kn_json_str(a, "name", ""), *url = kn_json_str(a, "browser_download_url", "");
        if (strncmp(url, "https://", 8) != 0) continue;
        if (!strcmp(name, "krunknative-server-linux-x86_64.tar.gz")) copy_s(out->dl_linux, sizeof(out->dl_linux), url);
        else if (!strcmp(name, "krunknative-server-linux-x86_64-bullseye.tar.gz"))
            copy_s(out->dl_linux_bullseye, sizeof(out->dl_linux_bullseye), url);
        else if (!strcmp(name, "krunknative-server-windows-x86_64.zip")) copy_s(out->dl_windows, sizeof(out->dl_windows), url);
        else if (!strncmp(name, "krunknative-server-macos-", 25) && ends_with(name, ".tar.gz"))
            copy_s(out->dl_macos, sizeof(out->dl_macos), url);
    }
    out->known = 1;
    return 1;
}

int release_pick_latest(const cJSON *releases, ReleaseInfo *out) {
    int found = 0;
    const cJSON *rel;
    cJSON_ArrayForEach(rel, releases) {
        ReleaseInfo r;
        if (!release_parse_release(rel, &r)) continue;
        const int newer = !found || r.commit_time > out->commit_time ||
                          (r.commit_time == out->commit_time && strncmp(r.tag, "server-v", 8) == 0 &&
                           strncmp(out->tag, "server-v", 8) != 0);
        if (newer) {
            *out = r;
            found = 1;
        }
    }
    return found;
}

void release_apply_overrides(ReleaseInfo *r, const char *latest_version, const char *download_url) {
    if (latest_version && *latest_version) {
        memset(r, 0, sizeof(*r));
        r->proto_major = r->proto_minor = -1;
        r->known = 1;
        r->from_config = 1;
        copy_s(r->tag, sizeof(r->tag), "config");
        copy_s(r->describe, sizeof(r->describe), latest_version);
        if (is_hex(latest_version) && strlen(latest_version) >= 7 && strlen(latest_version) < sizeof(r->commit)) {
            copy_s(r->commit, sizeof(r->commit), latest_version);
            for (char *c = r->commit; *c; c++) *c = (char) tolower((unsigned char) *c);
        }
    }
    if (download_url && *download_url) copy_s(r->html_url, sizeof(r->html_url), download_url);
}

int release_same_commit(const char *a, const char *b) {
    if (!is_hex(a) || !is_hex(b)) return 0;
    const size_t la = strlen(a), lb = strlen(b), n = la < lb ? la : lb;
    if (n < 7) return 0;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char) a[i]) != tolower((unsigned char) b[i])) return 0;
    return 1;
}

int release_server_outdated(const ReleaseInfo *latest, const ServerBuild *s, int64_t grace_s, int my_major,
                            int my_minor) {
    /* (a) older protocol than this coordinator */
    if (s->proto_major >= 0 && (s->proto_major < my_major || (s->proto_major == my_major && s->proto_minor < my_minor)))
        return 1;
    if (!latest || !latest->known) return 0;
    const int has_build = (s->commit && *s->commit) || (s->describe && *s->describe);
    /* (c) no build info at all: a server_bin from before the build fields */
    if (!has_build) return 1;
    if (release_same_commit(s->commit, latest->commit)) return 0;
    if (latest->from_config) /* (d) explicit version: anything else is outdated */
        return !(s->describe && !strcmp(s->describe, latest->describe));
    /* (b) older than the latest release by more than the grace period */
    if (latest->commit_time <= 0) return 0;
    if (s->commit_time <= 0) return 1;
    return s->commit_time < latest->commit_time - (grace_s > 0 ? grace_s : 0);
}

int release_protocol_incompatible(int client_major, int server_major) {
    return client_major >= 0 && server_major >= 0 && client_major != server_major;
}

static int64_t json_i64(const cJSON *o, const char *key) {
    const double v = kn_json_num(o, key, 0);
    return v > 0 ? (int64_t) v : 0;
}

void release_server_build_from_finder(const cJSON *entry, ServerBuild *out) {
    out->describe = kn_json_str(entry, "version", "");
    out->commit = kn_json_str(entry, "commit", "");
    out->commit_time = json_i64(entry, "commit_time");
    release_parse_protocol(kn_json_str(entry, "protocol", ""), &out->proto_major, &out->proto_minor);
    /* the pre-build-info server_bin reported "1.0.0" as version: that is no build info */
    if (!out->commit[0] && out->proto_major < 0) out->describe = "";
}

void release_server_build_from_meta(const cJSON *meta, ServerBuild *out) {
    out->describe = kn_json_str(meta, "build", "");
    out->commit = kn_json_str(meta, "commit", "");
    out->commit_time = json_i64(meta, "commitTime");
    release_parse_protocol(kn_json_str(meta, "proto", ""), &out->proto_major, &out->proto_minor);
}

void release_add_json(const ReleaseInfo *r, cJSON *o) {
    if (r && r->known) {
        cJSON *l = cJSON_AddObjectToObject(o, "latest");
        cJSON_AddStringToObject(l, "version", r->describe);
        cJSON_AddStringToObject(l, "commit", r->commit);
        cJSON_AddNumberToObject(l, "commit_time", (double) r->commit_time);
        cJSON_AddStringToObject(l, "build_time", r->build_time);
        char proto[16] = "";
        if (r->proto_major >= 0) snprintf(proto, sizeof(proto), "%d.%d", r->proto_major, r->proto_minor);
        cJSON_AddStringToObject(l, "protocol", proto);
        cJSON_AddStringToObject(l, "tag", r->tag);
    }
    cJSON_AddStringToObject(o, "download_url", r && r->html_url[0] ? r->html_url : RELEASE_PAGE_DEFAULT);
    cJSON *d = cJSON_AddObjectToObject(o, "downloads");
    if (r && r->dl_linux[0]) cJSON_AddStringToObject(d, "linux", r->dl_linux);
    if (r && r->dl_linux_bullseye[0]) cJSON_AddStringToObject(d, "linux_bullseye", r->dl_linux_bullseye);
    if (r && r->dl_windows[0]) cJSON_AddStringToObject(d, "windows", r->dl_windows);
    if (r && r->dl_macos[0]) cJSON_AddStringToObject(d, "macos", r->dl_macos);
}
