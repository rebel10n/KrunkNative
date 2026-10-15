// YouTube video ids of map links (shared/include/youtube_link.h).
#include <youtube_link.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static int id_char(const int c) { return isalnum(c) || c == '_' || c == '-'; }

// exactly KN_YOUTUBE_ID_LEN id characters at s, then the end or a non-id character
static int id_at(const char *s) {
    for (int i = 0; i < KN_YOUTUBE_ID_LEN; i++)
        if (!id_char((unsigned char) s[i])) return 0;
    return !id_char((unsigned char) s[KN_YOUTUBE_ID_LEN]);
}

static int put(const char *s, char *out, const size_t cap) {
    if (cap < KN_YOUTUBE_ID_LEN + 1) return 0;
    memcpy(out, s, KN_YOUTUBE_ID_LEN);
    out[KN_YOUTUBE_ID_LEN] = 0;
    return 1;
}

static int starts_ci(const char *s, const char *prefix) { return !strncasecmp(s, prefix, strlen(prefix)); }

int kn_youtube_id(const char *link, char *out, const size_t cap) {
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!link) return 0;
    while (*link == ' ' || *link == '\t' || *link == '\n' || *link == '\r') link++;
    size_t n = strlen(link);
    while (n && (link[n - 1] == ' ' || link[n - 1] == '\t' || link[n - 1] == '\n' || link[n - 1] == '\r')) n--;
    if (n > 512) return 0;
    char buf[520];
    memcpy(buf, link, n);
    buf[n] = 0;
    // a bare id
    if (n == KN_YOUTUBE_ID_LEN && id_at(buf)) return put(buf, out, cap);
    const char *p = buf;
    if (starts_ci(p, "https://")) p += 8;
    else if (starts_ci(p, "http://")) p += 7;
    else if (starts_ci(p, "//")) p += 2;
    // the host: [www. | m. | music.](youtube.com | youtube-nocookie.com) or youtu.be, then '/'
    const char *slash = strchr(p, '/');
    if (!slash) return 0;
    const size_t host_len = (size_t) (slash - p);
    char host[64];
    if (!host_len || host_len >= sizeof(host)) return 0;
    for (size_t i = 0; i < host_len; i++) host[i] = (char) tolower((unsigned char) p[i]);
    host[host_len] = 0;
    const char *h = host;
    if (!strncmp(h, "www.", 4)) h += 4;
    else if (!strncmp(h, "m.", 2)) h += 2;
    else if (!strncmp(h, "music.", 6)) h += 6;
    const char *path = slash + 1;
    if (!strcmp(h, "youtu.be")) return id_at(path) ? put(path, out, cap) : 0;
    if (strcmp(h, "youtube.com") && strcmp(h, "youtube-nocookie.com")) return 0;
    static const char *const k_paths[] = {"embed/", "shorts/", "live/", "v/"};
    for (size_t i = 0; i < sizeof(k_paths) / sizeof(k_paths[0]); i++) {
        const size_t l = strlen(k_paths[i]);
        if (!strncmp(path, k_paths[i], l)) return id_at(path + l) ? put(path + l, out, cap) : 0;
    }
    // watch?v=<id> (v anywhere in the query)
    if (strncmp(path, "watch", 5) != 0 || (path[5] != '?' && path[5] != '/')) return 0;
    const char *q = strchr(path, '?');
    while (q) {
        q++;
        if (!strncmp(q, "v=", 2)) return id_at(q + 2) ? put(q + 2, out, cap) : 0;
        q = strchr(q, '&');
    }
    return 0;
}

int kn_youtube_watch_url(const char *stored, char *out, const size_t cap) {
    char id[KN_YOUTUBE_ID_LEN + 1];
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!kn_youtube_id(stored, id, sizeof(id))) return 0;
    const int w = snprintf(out, cap, "https://www.youtube.com/watch?v=%s", id);
    if (w < 0 || (size_t) w >= cap) {
        out[0] = 0;
        return 0;
    }
    return 1;
}
