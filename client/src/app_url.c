// Krunker's page URLs -> page file + query (no client state; see app_url.h).
#include <app_url.h>

#include <stdio.h>
#include <string.h>

static void copy_query(const char *q, char *query, const size_t n) {
    const size_t qlen = strcspn(q, "#");
    snprintf(query, n, "%.*s", (int) (qlen < n ? qlen : n - 1), q);
}

int app_url_split(const char *url, const char **page, char *query, const size_t n) {
    static const char *const hosts[] = {"https://krunker.io/", "http://krunker.io/", "https://www.krunker.io/", "//krunker.io/"};
    static const char *const pages[] = {"social.html", "editor.html", "viewer.html"};
    const char *rest;
    if (page) *page = NULL;
    if (!query || !n) return 0;
    query[0] = 0;
    if (!url) return 0;
    while (*url == ' ') url++;
    for (size_t i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++) {
        const size_t len = strlen(hosts[i]);
        if (!strncmp(url, hosts[i], len)) {
            url += len;
            break;
        }
    }
    if (!strncmp(url, "./", 2)) url += 2;
    while (*url == '/') url++;
    // "/" (the hub's KRUNKER logo: openLink('/')), "/?play=<map>" and index.html are the game; its
    // parameters are the text after the '?' (the hub's Play link, testMap=1), kept as the query.
    if (!*url || *url == '?' || *url == '#') {
        rest = url;
    } else if (!strncmp(url, "index.html", 10)) {
        rest = url + 10;
    } else {
        for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
            const size_t len = strlen(pages[i]);
            if (strncmp(url, pages[i], len) != 0) continue;
            rest = url + len;
            if (*rest && *rest != '?' && *rest != '#') continue;
            if (page) *page = pages[i];
            copy_query(*rest == '?' ? rest + 1 : "", query, n);
            return 2;
        }
        return 0;
    }
    copy_query(*rest == '?' ? rest + 1 : "", query, n);
    return 1;
}
