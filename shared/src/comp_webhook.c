// The comp host's match-result webhook: the URL rule, the address rule and the body (shared/include/comp_webhook.h).
#include <comp_webhook.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------- addresses

int comp_webhook_ipv4_public(const uint32_t a) {
    const uint32_t b0 = a >> 24, b1 = (a >> 16) & 0xff, b2 = (a >> 8) & 0xff;
    if (b0 == 0 || b0 == 10 || b0 == 127 || b0 >= 224) return 0;            // this network, private, loopback, multicast, reserved
    if (b0 == 100 && (b1 & 0xc0) == 64) return 0;                           // 100.64/10 shared (CGNAT)
    if (b0 == 169 && b1 == 254) return 0;                                   // link-local (cloud metadata)
    if (b0 == 172 && (b1 & 0xf0) == 16) return 0;                           // 172.16/12
    if (b0 == 192 && b1 == 168) return 0;                                   // 192.168/16
    if (b0 == 192 && b1 == 0 && (b2 == 0 || b2 == 2)) return 0;             // 192.0.0/24 protocol, 192.0.2/24 TEST-NET-1
    if (b0 == 198 && (b1 == 18 || b1 == 19)) return 0;                      // 198.18/15 benchmarking
    if (b0 == 198 && b1 == 51 && b2 == 100) return 0;                       // TEST-NET-2
    if (b0 == 203 && b1 == 0 && b2 == 113) return 0;                        // TEST-NET-3
    return 1;
}

static uint32_t v4_at(const uint8_t *p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

int comp_webhook_ipv6_public(const uint8_t a[16]) {
    static const uint8_t zero[12] = {0};
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    static const uint8_t nat64[12] = {0x00, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0};
    if (!memcmp(a, zero, 12)) return 0; // ::, ::1 and the deprecated IPv4-compatible ::a.b.c.d
    if (!memcmp(a, mapped, 12) || !memcmp(a, nat64, 12)) return comp_webhook_ipv4_public(v4_at(a + 12));
    if (a[0] == 0x01 && a[1] == 0x00 && !memcmp(a + 2, zero, 6)) return 0;   // 100::/64 discard
    if (a[0] == 0x20 && a[1] == 0x01 && a[2] == 0x0d && a[3] == 0xb8) return 0; // 2001:db8::/32 documentation
    if ((a[0] & 0xfe) == 0xfc) return 0;                                    // fc00::/7 unique local
    if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) return 0;                    // fe80::/10 link-local
    if (a[0] == 0xfe && (a[1] & 0xc0) == 0xc0) return 0;                    // fec0::/10 site-local
    if (a[0] == 0xff) return 0;                                             // multicast
    return 1;
}

// ---------------------------------------------------------------- the URL

static int ends_with(const char *s, const size_t n, const char *suffix) {
    const size_t k = strlen(suffix);
    return n >= k && !strncasecmp(s + n - k, suffix, k);
}

// A canonical dotted quad (four decimal parts 0..255, no leading zeros): 1 and *out set; anything else 0.
static int parse_dotted_quad(const char *s, const size_t n, uint32_t *out) {
    uint32_t v = 0;
    size_t i = 0;
    for (int part = 0; part < 4; part++) {
        if (part) {
            if (i >= n || s[i] != '.') return 0;
            i++;
        }
        const size_t start = i;
        unsigned int x = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9' && i - start < 3) x = x * 10 + (unsigned int) (s[i++] - '0');
        if (i == start || x > 255 || (i - start > 1 && s[start] == '0')) return 0;
        v = (v << 8) | x;
    }
    if (i != n) return 0;
    *out = v;
    return 1;
}

int comp_webhook_url_ok(const char *url) {
    if (!url) return 0;
    const size_t len = strlen(url);
    if (len > COMP_WEBHOOK_URL_MAX || strncasecmp(url, "https://", 8) != 0) return 0;
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char) url[i];
        if (c <= 0x20 || c >= 0x7f || c == '\\') return 0; // printable ASCII, no spaces
    }
    const char *host = url + 8;
    size_t n = strcspn(host, "/?#"); // the authority
    if (memchr(host, '@', n)) return 0; // user info
    const char *colon = memchr(host, ':', n);
    if (colon) { // a port: 1..65535
        const size_t digits = n - (size_t) (colon - host) - 1;
        if (!digits || digits > 5) return 0;
        long port = 0;
        for (const char *p = colon + 1; p < host + n; p++) {
            if (*p < '0' || *p > '9') return 0;
            port = port * 10 + (*p - '0');
        }
        if (port < 1 || port > 65535) return 0;
        n = (size_t) (colon - host);
    }
    if (n == 0 || n > 253 || host[0] == '[') return 0; // no IPv6 literal
    const size_t bare = host[n - 1] == '.' ? n - 1 : n; // a trailing root dot
    if (!bare) return 0;
    // labels: letters, digits and '-', none empty. A label is a number form when curl would read it as an
    // IPv4 part (decimal, or 0x + hex digits; octal is decimal digits)
    int labels = 0, numeric = 0, last_numeric = 0;
    for (size_t i = 0; i < bare;) {
        const size_t start = i;
        while (i < bare && host[i] != '.') {
            const char c = host[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return 0;
            i++;
        }
        const size_t k = i - start;
        if (!k) return 0;
        int num = 1;
        const int hex = k >= 2 && host[start] == '0' && (host[start + 1] == 'x' || host[start + 1] == 'X');
        for (size_t j = start + (hex ? 2 : 0); j < i; j++) {
            const char c = host[j];
            if (!((c >= '0' && c <= '9') || (hex && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))))) num = 0;
        }
        labels++;
        numeric += num;
        last_numeric = num;
        if (i < bare) i++; // the dot
    }
    if (numeric == labels) { // an address: a public canonical dotted quad only (no 127.1, 2130706433, 0x7f.0.0.1 ...)
        uint32_t v4;
        return parse_dotted_quad(host, bare, &v4) && comp_webhook_ipv4_public(v4);
    }
    if (last_numeric) return 0; // a numeric top-level label: not a DNS name
    if (labels < 2) return 0;   // a bare name ("metadata", "brain"): a local one
    static const char *const local[] = {".localhost", ".local", ".internal", ".lan", ".home.arpa", ".localdomain"};
    for (size_t i = 0; i < sizeof(local) / sizeof(local[0]); i++) {
        if (ends_with(host, bare, local[i])) return 0;
    }
    return 1;
}

// ---------------------------------------------------------------- the body

int comp_webhook_winner(const CompWebhookMatch *match) {
    const int alpha = match ? match->round_wins[0] : 0, bravo = match ? match->round_wins[1] : 0;
    return alpha >= bravo ? 1 : 2; // a tie: ON_DRAW_FAVOUR_TEAM, Alpha
}

static int by_score(const void *pa, const void *pb) {
    const CompWebhookPlayer *a = *(const CompWebhookPlayer *const *) pa, *b = *(const CompWebhookPlayer *const *) pb;
    return (b->score > a->score) - (b->score < a->score);
}

cJSON *comp_webhook_payload(const CompWebhookMatch *match) {
    if (!match) return NULL;
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    const int winner = comp_webhook_winner(match);
    cJSON_AddStringToObject(o, "type", "match_end");
    cJSON_AddStringToObject(o, "map", match->map ? match->map : "Unknown"); // matchInfo map_name's fallback
    cJSON_AddNumberToObject(o, "mode", match->mode);
    cJSON_AddNumberToObject(o, "winner", winner);
    cJSON *teams = cJSON_AddArrayToObject(o, "teams");
    for (int t = 0; t < 2; t++) {
        cJSON *team = cJSON_CreateObject();
        cJSON_AddNumberToObject(team, "team", t + 1);
        cJSON_AddStringToObject(team, "name", match->team_names[t] ? match->team_names[t] : t ? "Bravo" : "Alpha");
        cJSON_AddNumberToObject(team, "score", match->round_wins[t]);
        cJSON_AddItemToArray(teams, team);
    }
    cJSON *players = cJSON_AddArrayToObject(o, "players");
    const CompWebhookPlayer **order = match->player_count ? calloc(match->player_count, sizeof(*order)) : NULL;
    size_t count = 0;
    for (size_t i = 0; order && i < match->player_count; i++) {
        const CompWebhookPlayer *p = &match->players[i];
        if (p->team == 1 || p->team == 2) order[count++] = p;
    }
    if (count > 1) qsort(order, count, sizeof(*order), by_score);
    for (size_t i = 0; i < count; i++) {
        const CompWebhookPlayer *p = order[i];
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "team", p->team);
        cJSON_AddStringToObject(e, "name", p->name ? p->name : "Guest");
        cJSON_AddNumberToObject(e, "player_id", p->player_id);
        cJSON_AddNumberToObject(e, "score", p->score);
        cJSON_AddNumberToObject(e, "kills", p->kills);
        cJSON_AddNumberToObject(e, "deaths", p->deaths);
        cJSON_AddNumberToObject(e, "assists", p->assists);
        cJSON_AddNumberToObject(e, "accuracy", p->shots > 0 ? (double) p->hits / (double) p->shots * 100.0 : 0.0);
        cJSON_AddNumberToObject(e, "headshots", p->headshots);
        cJSON_AddNumberToObject(e, "damage_done", p->damage_done);
        cJSON_AddNumberToObject(e, "objective_score", p->objective_score);
        cJSON_AddNumberToObject(e, "rounds_won", match->round_wins[p->team - 1]);
        cJSON_AddNumberToObject(e, "victory", p->team == winner ? 1 : 0);
        cJSON_AddNumberToObject(e, "kr", p->kr);
        cJSON_AddItemToArray(players, e);
    }
    free(order);
    return o;
}
