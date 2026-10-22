// The krunker.io read routes of a krunker.io account (krunker_kio_routes.h): pure string rules, no client state.
#include <krunker_kio_routes.h>

#include <stdio.h>
#include <string.h>

// <base> without its trailing '/'; the path appended with exactly one '/' between them
static size_t kio_put(char *out, const size_t n, const char *s) {
    const size_t len = strlen(s);
    if (!out || !n || len >= n) {
        if (out && n) out[0] = 0;
        return 0;
    }
    memcpy(out, s, len + 1);
    return len;
}

static size_t kio_join(char *out, const size_t n, const char *base, const char *path) {
    char b[512];
    size_t len = strlen(base ? base : "");
    if (len >= sizeof(b)) return 0;
    memcpy(b, base ? base : "", len);
    while (len && b[len - 1] == '/') len--;
    b[len] = 0;
    char joined[768];
    const int m = snprintf(joined, sizeof(joined), "%s%s", b, path);
    if (m <= 0 || (size_t) m >= sizeof(joined)) return 0;
    return kio_put(out, n, joined);
}

size_t krunker_kio_webhook_url(char *out, const size_t n, const char *api_base, const char *group, const char *query) {
    if (!group || !group[0]) return 0;
    char path[640];
    const int m = snprintf(path, sizeof(path), "/webhooks/general/%s%s%s", group, query && query[0] ? "?" : "", query ? query : "");
    if (m <= 0 || (size_t) m >= sizeof(path)) return 0;
    return kio_join(out, n, api_base, path);
}

const char *krunker_kio_webhook_group(const char *kn_path, const char *search_type) {
    if (!kn_path) return NULL;
    if (!strcmp(kn_path, "/maps")) return "maps/list";
    if (!strcmp(kn_path, "/mods")) return "mods/list";
    if (!strcmp(kn_path, "/search") && search_type && !strcmp(search_type, "map")) return "maps/search";
    if (!strcmp(kn_path, "/search") && search_type && !strcmp(search_type, "mod")) return "mods/search";
    return NULL;
}

// URLSearchParams' form encoding: ASCII letters, digits and *-._ stay; a space is '+'; any other byte is %XX
static size_t kio_form_encode(char *out, const size_t n, const char *s, size_t at) {
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *) s; *p; p++) {
        char piece[4];
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '*' || *p == '-' || *p == '.' || *p == '_') {
            piece[0] = (char) *p;
            piece[1] = 0;
        } else if (*p == ' ') {
            piece[0] = '+';
            piece[1] = 0;
        } else {
            piece[0] = '%';
            piece[1] = hex[*p >> 4];
            piece[2] = hex[*p & 15];
            piece[3] = 0;
        }
        const size_t len = strlen(piece);
        if (at + len + 1 > n) return 0;
        memcpy(out + at, piece, len);
        at += len;
    }
    out[at] = 0;
    return at;
}

size_t krunker_kio_search_query(char *out, const size_t n, const char *val, const int cat) {
    if (!out || !n) return 0;
    const char *q = "val=";
    size_t at = strlen(q);
    if (at + 1 > n) return 0;
    memcpy(out, q, at + 1);
    at = kio_form_encode(out, n, val ? val : "", at);
    if (!at) return 0;
    char tail[24];
    const int m = snprintf(tail, sizeof(tail), "&cat=%d", cat);
    if (m <= 0 || (size_t) m + at + 1 > n) return 0;
    memcpy(out + at, tail, (size_t) m + 1);
    return at + (size_t) m;
}

size_t krunker_kio_streams_url(char *out, const size_t n, const char *api_base) {
    return kio_join(out, n, api_base, "/streams/top?cb=2");
}

size_t krunker_kio_player_count_url(char *out, const size_t n, const char *social_base) {
    return kio_join(out, n, social_base, "/player-count");
}

size_t krunker_kio_gapi_leaderboard_path(char *out, const size_t n, const char *kn_path) {
    static const char prefix[] = "/api/leaderboards/";
    if (!kn_path || strncmp(kn_path, prefix, sizeof(prefix) - 1) != 0) {
        if (out && n) out[0] = 0;
        return 0;
    }
    char path[320];
    const int m = snprintf(path, sizeof(path), "/leaderboards/%s", kn_path + sizeof(prefix) - 1);
    if (m <= 0 || (size_t) m >= sizeof(path)) return 0;
    return kio_put(out, n, path);
}

size_t krunker_kio_gapi_path(char *out, const size_t n, const char *kn_path) {
    if (out && n) out[0] = 0;
    if (!kn_path || strstr(kn_path, "token=")) return 0;
    static const struct {
        const char *kn, *gapi;
    } groups[] = {{"/api/leaderboards/", "/leaderboards/"}, {"/api/territorial-wars/", "/territorial-wars/"}, {"/api/clans/by-name/", "/clans/by-name/"}};
    for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
        const size_t kl = strlen(groups[i].kn);
        if (strncmp(kn_path, groups[i].kn, kl) != 0) continue;
        if (!kn_path[kl]) return 0; // a group with no name after it
        char path[320];
        const int m = snprintf(path, sizeof(path), "%s%s", groups[i].gapi, kn_path + kl);
        if (m <= 0 || (size_t) m >= sizeof(path)) return 0;
        return kio_put(out, n, path);
    }
    static const char recruit_kn[] = "/api/clans/recruitment", recruit_gapi[] = "/clans/recruitment";
    const size_t rkl = sizeof(recruit_kn) - 1;
    if (strncmp(kn_path, recruit_kn, rkl) == 0 && (kn_path[rkl] == 0 || kn_path[rkl] == '?')) {
        char path[320];
        const int m = snprintf(path, sizeof(path), "%s%s", recruit_gapi, kn_path + rkl);
        if (m <= 0 || (size_t) m >= sizeof(path)) return 0;
        return kio_put(out, n, path);
    }
    static const char clans_kn[] = "/api/clans/";
    const size_t ckl = sizeof(clans_kn) - 1;
    if (strncmp(kn_path, clans_kn, ckl) == 0 && kn_path[ckl] >= '0' && kn_path[ckl] <= '9') {
        const char *tail = kn_path + ckl;
        while (*tail >= '0' && *tail <= '9') tail++;
        if (strncmp(tail, "/recruitment", 12) == 0 && (tail[12] == 0 || tail[12] == '?')) {
            char path[320];
            const int m = snprintf(path, sizeof(path), "/clans/%.*s%s", (int) (tail - (kn_path + ckl)), kn_path + ckl, tail);
            if (m <= 0 || (size_t) m >= sizeof(path)) return 0;
            return kio_put(out, n, path);
        }
    }
    return 0;
}
