// Game ids and their short link form: see server_names.h.
#include <server_names.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void copy_out(char *out, const size_t n, const char *s, const size_t len) {
    if (!out || !n) return;
    const size_t k = len < n ? len : n - 1;
    memcpy(out, s, k);
    out[k] = 0;
}

void kn_node_short_id(const char *node_id, const char *region, char *out, const size_t n) {
    if (!out || !n) return;
    out[0] = 0;
    if (!node_id) return;
    const size_t len = strcspn(node_id, ":");
    size_t stem = len;
    while (stem > 0 && isdigit((unsigned char) node_id[stem - 1])) stem--;
    const char *digits = node_id + stem;
    const size_t digit_len = len - stem;
    char buf[KN_SHORT_NODE_MAX];
    size_t k = 0;
    if (region && strlen(region) == 2 && isalpha((unsigned char) region[0]) && isalpha((unsigned char) region[1]) && stem > 2) {
        // an edge server: CHITOWN2 in CH -> CH2
        buf[k++] = (char) toupper((unsigned char) region[0]);
        buf[k++] = (char) toupper((unsigned char) region[1]);
    } else if (stem > 4) {
        // a long community node id: its first 3 letters / digits
        for (size_t i = 0; i < stem && k < 3; i++) {
            if (isalnum((unsigned char) node_id[i])) buf[k++] = (char) toupper((unsigned char) node_id[i]);
        }
    }
    if (!k) { // already short (or nothing usable to shorten): the node id itself
        copy_out(out, n, node_id, len);
        return;
    }
    for (size_t i = 0; i < digit_len && k + 1 < sizeof(buf); i++) buf[k++] = digits[i];
    copy_out(out, n, buf, k);
}

void kn_game_link_id(const char *game_id, const char *region, char *out, const size_t n) {
    if (!out || !n) return;
    const char *colon = game_id ? strchr(game_id, ':') : NULL;
    if (!colon) {
        copy_out(out, n, game_id ? game_id : "", game_id ? strlen(game_id) : 0);
        return;
    }
    char node[KN_SHORT_NODE_MAX];
    kn_node_short_id(game_id, region, node, sizeof(node));
    snprintf(out, n, "%s%s", node, colon);
}

int kn_game_id_matches(const char *id, const char *game_id, const char *region) {
    if (!id || !game_id || !id[0]) return 0;
    if (!strcmp(id, game_id)) return 1;
    const char *a = strchr(id, ':'), *b = strchr(game_id, ':');
    if (!a || !b || strcmp(a + 1, b + 1) != 0) return 0; // the lobby part must be the same
    char short_node[KN_SHORT_NODE_MAX];
    kn_node_short_id(game_id, region, short_node, sizeof(short_node));
    const size_t len = (size_t) (a - id);
    return len == strlen(short_node) && !strncasecmp(id, short_node, len);
}

int kn_game_link_parse(const char *text, char *id, const size_t n) {
    char buf[512];
    if (!id || !n) return 0;
    id[0] = 0;
    while (text && (*text == ' ' || *text == '\t')) text++;
    snprintf(buf, sizeof(buf), "%s", text ? text : "");
    size_t len = strlen(buf);
    while (len && (buf[len - 1] == ' ' || buf[len - 1] == '\t' || buf[len - 1] == '\r' || buf[len - 1] == '\n')) buf[--len] = 0;
    const char *q = strstr(buf, "/?");
    const char *dot = strchr(buf, '.'), *sep = strchr(buf, ':');
    // (a '.' / ':' only after the query is not a link; "/?game=" and "localhost/?game=" are)
    if (q && (strncmp(buf, "http", 4) == 0 || !dot || dot < q || !sep || sep < q)) {
        // a link: its query's game= (the page's ?game= reader decodes %XX / '+')
        const char *g = strstr(q + 1, "game=");
        while (g && g[-1] != '?' && g[-1] != '&') g = strstr(g + 1, "game="); // not ?mygame=
        if (!g) return 0;
        size_t k = 0;
        for (g += 5; *g && *g != '&' && *g != '#' && k + 1 < n; g++) {
            if (*g == '%' && isxdigit((unsigned char) g[1]) && isxdigit((unsigned char) g[2])) {
                const char hex[3] = {g[1], g[2], 0};
                id[k++] = (char) strtol(hex, NULL, 16);
                g += 2;
            } else {
                id[k++] = *g == '+' ? ' ' : *g;
            }
        }
        id[k] = 0;
        return id[0] != 0;
    }
    // /^([A-Z]+):(\w+)$/ - KrunkNative node ids carry digits (CHITOWN2:kz862, CH2:kz862)
    const char *colon = strchr(buf, ':');
    int ok = colon && colon > buf && colon[1];
    for (const char *p = buf; ok && p < colon; p++) ok = isupper((unsigned char) *p) || (p > buf && isdigit((unsigned char) *p));
    for (const char *p = colon ? colon + 1 : buf; ok && *p; p++) ok = isalnum((unsigned char) *p) || *p == '_';
    if (ok) snprintf(id, n, "%s", buf);
    return ok;
}
