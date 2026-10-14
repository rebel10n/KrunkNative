/*
 * services/strike/director.c - kn_strike_director, Krunker Strike's matchmaker "director"
 * (doc/v10/strike-director.md). The same shape as FRVR's Strike director: one per region, the
 * client times GET /live, then POST /v1/director/allocate (public) or /allocate/private (a lobby
 * with a code) and connects to the game server it names with the signed ticket it hands out.
 *
 * Standalone: knsvc only (HTTP, loop, crypto), no database, no kn_brain. Fed by Strike game
 * servers' finder heartbeats (gameserver.finder_url = this director): the same
 * POST /servers/heartbeat contract as the brain's finder (services/brain/finder.c), the same
 * table (finder_table.c) and the same KNINFO UDP verification. Only servers whose verified KNINFO
 * reply says flavor "strike" are ever handed out; a base server can register but never gets a
 * player from here.
 *
 * State is in memory: slot reservations (tickets issued but not yet visible in a poll), private
 * lobbies (code -> server, room -> code) and room pins (a room's public lobby). A restart forgets
 * them; the game servers still refuse a public ticket into a private lobby (director_gate.h).
 */
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "build_info.h"
#include "director_ticket.h"
#include "finder_table.h"
#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#define MOD "director"
#define TICK_MS 250
#define POLLS_PER_TICK 256
#define SLOT_PENDING 64          /* outstanding tickets remembered per server */
#define MAX_SLOTS 1024
#define MAX_LOBBIES 1024
#define MAX_PINS 1024
#define CODE_LEN 6
#define ROOM_MAX 64
#define RATE_SLOTS 4096

/* ------------------------------------------------------------------ state */

typedef struct QueryAddr {
    struct sockaddr_storage sa;
    socklen_t len;
} QueryAddr;

typedef struct Slot {            /* director state of one game server (host:port) */
    char host[FINDER_HOST_MAX];
    int port;
    uint64_t issued_ms[SLOT_PENDING], exp_ms[SLOT_PENDING];
    int next;
} Slot;

typedef struct Lobby {           /* a private lobby: its code holds one server */
    char code[DIRECTOR_CODE_MAX];
    char room[ROOM_MAX];         /* the room / FRVR customId that opened it ("" = none) */
    int capacity;                /* FRVR's capacity (seats), 0 = the server's max */
    char host[FINDER_HOST_MAX];
    int port;
    uint64_t opened_ms, last_alloc_ms;
} Lobby;

typedef struct Pin {             /* a room's public lobby: the room's members land together */
    char room[ROOM_MAX];
    char host[FINDER_HOST_MAX];
    int port;
    uint64_t until_ms;
} Pin;

typedef struct Rate {
    char ip[64];
    uint64_t window_ms;
    int count;
} Rate;

static struct {
    kn_config *cfg;
    kn_loop *loop;
    kn_http_server *http;
    FinderTable *t;
    int fd4, fd6;
    char key[65];                /* director ticket key (director_ticket_key) */
    int ttl_s;
    int window_ms;               /* public seats stay reserved this long after a ticket (join window) */
    int private_idle_ms;
    int pin_ms;
    int allocate_per_min;
    char min_version[32];
    char default_region[32];
    const char *trusted[64];
    size_t trusted_count;
    Slot *slots[MAX_SLOTS];
    size_t slot_count;
    Lobby lobbies[MAX_LOBBIES];
    size_t lobby_count;
    Pin pins[MAX_PINS];
    Rate rates[RATE_SLOTS];
} g_d = {.fd4 = -1, .fd6 = -1};

/* ------------------------------------------------------------------ helpers */

static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t) *s) * 16777619u;
    return h;
}

/* fixed-window per-IP limit; 1 = allowed */
static int rate_ok(const char *ip, int per_min) {
    if (per_min <= 0) return 1;
    const uint64_t now = kn_now_ms();
    Rate *r = &g_d.rates[fnv1a(ip ? ip : "") & (RATE_SLOTS - 1)];
    if (strcmp(r->ip, ip ? ip : "") != 0 || now - r->window_ms >= 60000) {
        snprintf(r->ip, sizeof r->ip, "%s", ip ? ip : "");
        r->window_ms = now;
        r->count = 0;
    }
    return ++r->count <= per_min;
}

/* dotted numeric versions ("1.3", "v1.27.0"): <0, 0, >0. Missing parts count as 0. */
static int version_cmp(const char *a, const char *b) {
    if (*a == 'v' || *a == 'V') a++;
    if (*b == 'v' || *b == 'V') b++;
    for (int i = 0; i < 4; i++) {
        char *ea, *eb;
        const long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);
        if (x != y) return x < y ? -1 : 1;
        a = *ea == '.' ? ea + 1 : ea;
        b = *eb == '.' ? eb + 1 : eb;
        if (!*a && !*b) break;
    }
    return 0;
}

static int version_valid(const char *v) {
    if (*v == 'v' || *v == 'V') v++;
    return *v >= '0' && *v <= '9';
}

static Slot *slot_get(const char *host, int port, int create) {
    for (size_t i = 0; i < g_d.slot_count; i++)
        if (g_d.slots[i]->port == port && !strcmp(g_d.slots[i]->host, host)) return g_d.slots[i];
    if (!create || g_d.slot_count >= MAX_SLOTS) return NULL;
    Slot *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    snprintf(s->host, sizeof s->host, "%s", host);
    s->port = port;
    g_d.slots[g_d.slot_count++] = s;
    return s;
}

static FinderEntry *entry_get(const char *host, int port) {
    for (size_t i = 0; i < finder_table_count(g_d.t); i++) {
        FinderEntry *e = finder_table_at(g_d.t, i);
        if (e->port == port && !strcmp(e->host, host)) return e;
    }
    return NULL;
}

/* Seats held by tickets the server's player count may not show yet. Two views:
 *   PENDING_WINDOW (public, opening a lobby): every ticket of the last join_window_s, also when the
 *     player is already counted. Conservative: a Strike client loading the map for seconds is never
 *     missed, so fill-first never over-assigns; at worst a seat looks taken a few seconds too long.
 *   PENDING_SINCE_POLL (joining a private lobby by code / room): only tickets issued after the
 *     last verified poll was sent, so a friend is never turned away by a seat counted twice; the
 *     game server's max_players stays the hard cap. */
enum { PENDING_WINDOW, PENDING_SINCE_POLL };

static int slot_pending(const Slot *s, uint64_t now, int view) {
    if (!s) return 0;
    const FinderEntry *e = entry_get(s->host, s->port);
    const uint64_t polled = e && e->last_ok_ms > (uint64_t) e->ping_ms ? e->last_ok_ms - (uint64_t) e->ping_ms : 0;
    int n = 0;
    for (int i = 0; i < SLOT_PENDING; i++) {
        if (s->exp_ms[i] <= now) continue;
        if (view == PENDING_WINDOW ? now - s->issued_ms[i] < (uint64_t) g_d.window_ms : s->issued_ms[i] > polled) n++;
    }
    return n;
}

static void slot_reserve(Slot *s, uint64_t now) {
    s->issued_ms[s->next] = now;
    s->exp_ms[s->next] = now + (uint64_t) g_d.ttl_s * 1000;
    s->next = (s->next + 1) % SLOT_PENDING;
}

static Lobby *lobby_by_code(const char *code) {
    for (size_t i = 0; i < g_d.lobby_count; i++)
        if (!strcasecmp(g_d.lobbies[i].code, code)) return &g_d.lobbies[i];
    return NULL;
}

static Lobby *lobby_by_room(const char *room) {
    if (!room || !*room) return NULL;
    for (size_t i = 0; i < g_d.lobby_count; i++)
        if (!strcmp(g_d.lobbies[i].room, room)) return &g_d.lobbies[i];
    return NULL;
}

static Lobby *lobby_by_server(const char *host, int port) {
    for (size_t i = 0; i < g_d.lobby_count; i++)
        if (g_d.lobbies[i].port == port && !strcmp(g_d.lobbies[i].host, host)) return &g_d.lobbies[i];
    return NULL;
}

static void lobby_remove(Lobby *l) {
    const size_t i = (size_t) (l - g_d.lobbies);
    KN_INFO(MOD, "private lobby %s on %s:%d closed", l->code, l->host, l->port);
    g_d.lobbies[i] = g_d.lobbies[--g_d.lobby_count];
}

static Pin *pin_get(const char *room, uint64_t now, int create) {
    if (!room || !*room) return NULL;
    Pin *p = &g_d.pins[fnv1a(room) & (MAX_PINS - 1)];
    if (p->until_ms > now && !strcmp(p->room, room)) return p;
    if (!create) return NULL;
    memset(p, 0, sizeof(*p));
    snprintf(p->room, sizeof p->room, "%s", room);
    return p;
}

/* the director's game id of a server: "<region>:<8 hex of sha256(host:port)>" */
static void game_id(const char *region, const char *host, int port, char *out, size_t cap) {
    char hp[FINDER_HOST_MAX + 8], hex[2 * KN_SHA256_LEN + 1];
    uint8_t h[KN_SHA256_LEN];
    snprintf(hp, sizeof hp, "%s:%d", host, port);
    if (kn_sha256(hp, strlen(hp), h) != 0 || kn_hex_encode(h, sizeof h, hex, sizeof hex) < 0) hex[0] = 0;
    snprintf(out, cap, "%s:%.8s", region && *region ? region : g_d.default_region, hex);
}

static int new_code(char out[DIRECTOR_CODE_MAX]) {
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; /* no 0/O, 1/I */
    for (int attempt = 0; attempt < 16; attempt++) {
        uint8_t b[CODE_LEN];
        if (kn_random_bytes(b, sizeof b) != 0) return -1;
        for (int i = 0; i < CODE_LEN; i++) out[i] = alphabet[b[i] % (sizeof(alphabet) - 1)];
        out[CODE_LEN] = 0;
        if (!lobby_by_code(out)) return 0;
    }
    return -1;
}

static int has_tag(const cJSON *srv, const char *tag) {
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(srv, "tags"))
        if (cJSON_IsString(it) && !strcasecmp(it->valuestring, tag)) return 1;
    return 0;
}

static int region_ok(const char *have, const char *want) {
    return !want || !*want || (have && !strcasecmp(have, want));
}

/* ------------------------------------------------------------------ UDP (KNINFO polls) */

static int resolve(FinderEntry *e) {
    if (e->addr) return 0;
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST;
    char port[8];
    snprintf(port, sizeof(port), "%d", e->query_port);
    if (getaddrinfo(e->host, port, &hints, &res) != 0) {
        hints.ai_flags = 0;
        if (getaddrinfo(e->host, port, &hints, &res) != 0 || !res) return -1;
    }
    QueryAddr *q = calloc(1, sizeof(*q));
    if (!q) {
        freeaddrinfo(res);
        return -1;
    }
    memcpy(&q->sa, res->ai_addr, res->ai_addrlen);
    q->len = res->ai_addrlen;
    freeaddrinfo(res);
    e->addr = q;
    return 0;
}

static int same_addr(const struct sockaddr_storage *a, const struct sockaddr_storage *b) {
    if (a->ss_family != b->ss_family) return 0;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *x = (const void *) a, *y = (const void *) b;
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6 *x = (const void *) a, *y = (const void *) b;
        return x->sin6_port == y->sin6_port && !memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(x->sin6_addr));
    }
    return 0;
}

static void on_udp(kn_loop *loop, int fd, unsigned events, void *ud) {
    (void) loop;
    (void) events;
    (void) ud;
    for (int i = 0; i < 64; i++) {
        uint8_t buf[2048];
        struct sockaddr_storage from;
        socklen_t flen = sizeof(from);
        const ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *) &from, &flen);
        if (n < 0) return; /* EAGAIN */
        cJSON *info = finder_parse_reply(buf, (size_t) n);
        if (!info) continue;
        const char *nonce = kn_json_str(info, "nonce", "");
        FinderEntry *match = NULL;
        for (size_t k = 0; k < finder_table_count(g_d.t); k++) {
            FinderEntry *e = finder_table_at(g_d.t, k);
            if (e->addr && e->nonce[0] && !strcmp(e->nonce, nonce) && same_addr(&((QueryAddr *) e->addr)->sa, &from)) {
                match = e;
                break;
            }
        }
        if (!match) {
            cJSON_Delete(info);
            continue;
        }
        const int was_verified = match->info != NULL;
        if (finder_poll_reply(g_d.t, match, info, kn_now_ms(), kn_wall_ms() / 1000) && !was_verified) {
            const char *flavor = kn_json_str(match->info, "flavor", "");
            KN_INFO(MOD, "verified %s:%d (\"%s\", flavor %s)%s", match->host, match->port,
                    kn_json_str(match->info, "name", ""), flavor[0] ? flavor : "base",
                    strcasecmp(flavor, "strike") ? ": not a Strike server, never handed out" : "");
        }
    }
}

static int open_udp(int family) {
    const int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    kn_set_nonblock(fd);
    kn_set_cloexec(fd);
    if (family == AF_INET6) {
        const int one = 1;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
    }
    if (kn_loop_add_fd(g_d.loop, fd, KN_EV_READ, on_udp, NULL) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void send_poll(FinderEntry *e, uint64_t now) {
    char nonce[FINDER_NONCE_LEN + 1];
    if (kn_random_hex(nonce, sizeof(nonce), FINDER_NONCE_LEN / 2) != 0) return;
    finder_poll_stamp(g_d.t, e, nonce, now); /* a failed send times out like a lost reply */
    if (resolve(e) != 0) return;
    const QueryAddr *q = e->addr;
    const int fd = q->sa.ss_family == AF_INET6 ? g_d.fd6 : g_d.fd4;
    if (fd < 0) return;
    uint8_t pkt[64];
    const size_t len = finder_build_query(nonce, pkt, sizeof(pkt));
    if (sendto(fd, pkt, len, 0, (const struct sockaddr *) &q->sa, q->len) < 0)
        KN_DEBUG(MOD, "poll %s:%d: %s", e->host, e->query_port, strerror(errno));
}

/* ------------------------------------------------------------------ allocation */

/* The Strike servers that can take players now (verified, fresh, flavor strike, no password,
 * not draining), as finder_list entries. mode filters like the finder (name / id). */
static cJSON *strike_servers(const char *mode, uint64_t now) {
    FinderFilter f = {.flavor = "strike", .mode = mode, .online_mode = -1};
    cJSON *list = finder_list(g_d.t, &f, now);
    cJSON *out = cJSON_CreateArray();
    cJSON *it = list ? list->child : NULL;
    while (it) {
        cJSON *next = it->next;
        if (!kn_json_bool(it, "password", 0) && !has_tag(it, "draining") && !has_tag(it, "ranked")) {
            cJSON_DetachItemViaPointer(list, it);
            cJSON_AddItemToArray(out, it);
        }
        it = next;
    }
    cJSON_Delete(list);
    return out;
}

static Lobby *lobby_by_server(const char *host, int port);

static int srv_free_view(const cJSON *srv, int *players_out, uint64_t now, int view) {
    const char *host = kn_json_str(srv, "host", "");
    const int port = (int) kn_json_int(srv, "port", 0);
    const int players = (int) kn_json_int(srv, "players", 0) + slot_pending(slot_get(host, port, 0), now, view);
    if (players_out) *players_out = players;
    int max = (int) kn_json_int(srv, "max", 0);
    const Lobby *l = lobby_by_server(host, port);
    if (l && l->capacity > 0 && l->capacity < max) max = l->capacity; /* allocate/private capacity */
    return max - players;
}

static int srv_free(const cJSON *srv, int *players_out, uint64_t now) {
    return srv_free_view(srv, players_out, now, PENDING_WINDOW);
}

/* a private lobby (ours, or one the game server reports) is never handed out publicly */
static int srv_private(const cJSON *srv) {
    return has_tag(srv, "private") || lobby_by_server(kn_json_str(srv, "host", ""), (int) kn_json_int(srv, "port", 0));
}

/* Public: the fullest Strike server with room (fill first), in the asked region when there is
 * one there, else anywhere. need_empty (a new private lobby): only servers nobody plays on. */
static const cJSON *pick(const cJSON *servers, const char *region, int need_empty, int seats, uint64_t now) {
    for (int pass = 0; pass < 2; pass++) {
        const char *want = pass == 0 ? region : NULL;
        if (pass == 1 && (!region || !*region)) break;
        const cJSON *best = NULL;
        int best_players = -1;
        const cJSON *srv;
        cJSON_ArrayForEach(srv, servers) {
            if (!region_ok(kn_json_str(srv, "region", ""), want) || srv_private(srv)) continue;
            int players;
            if (srv_free(srv, &players, now) < seats) continue;
            if (need_empty && players > 0) continue;
            if (players > best_players) { /* ties: the first registered */
                best = srv;
                best_players = players;
            }
        }
        if (best) return best;
    }
    return NULL;
}

static const cJSON *find_srv(const cJSON *servers, const char *host, int port) {
    const cJSON *srv;
    cJSON_ArrayForEach(srv, servers)
        if ((int) kn_json_int(srv, "port", 0) == port && !strcmp(kn_json_str(srv, "host", ""), host)) return srv;
    return NULL;
}

/* {host, port, gameId, ticket, ...} for srv, reserving a seat; NULL when signing failed */
static cJSON *grant(const cJSON *srv, const char *code, int seats, uint64_t now) {
    const char *host = kn_json_str(srv, "host", "");
    const int port = (int) kn_json_int(srv, "port", 0);
    const char *region = kn_json_str(srv, "region", "");
    char gid[64], jti[40];
    game_id(region, host, port, gid, sizeof gid);
    char *ticket = director_ticket_issue(g_d.key, gid, port, code, g_d.ttl_s, jti, sizeof jti);
    if (!ticket) return NULL;
    Slot *s = slot_get(host, port, 1);
    for (int i = 0; s && i < seats; i++) slot_reserve(s, now);
    cJSON *o = cJSON_CreateObject();
    /* FRVR's client reads only `address` (it connects to "//" + address + "/ws") */
    char address[FINDER_HOST_MAX + 8];
    snprintf(address, sizeof address, "%s:%d", host, port);
    cJSON_AddStringToObject(o, "address", address);
    cJSON_AddStringToObject(o, "host", host);
    cJSON_AddNumberToObject(o, "port", port);
    cJSON_AddStringToObject(o, "gameId", gid);
    cJSON_AddStringToObject(o, "ticket", ticket);
    cJSON_AddStringToObject(o, "region", region[0] ? region : g_d.default_region);
    cJSON_AddStringToObject(o, "mode", kn_json_str(srv, "mode", ""));
    cJSON_AddStringToObject(o, "map", kn_json_str(srv, "map", ""));
    cJSON_AddNumberToObject(o, "expiresIn", g_d.ttl_s);
    /* FRVR's response shape (gameServer {host, port}) next to ours */
    cJSON *gs = cJSON_AddObjectToObject(o, "gameServer");
    cJSON_AddStringToObject(gs, "host", host);
    cJSON_AddNumberToObject(gs, "port", port);
    KN_INFO(MOD, "allocated %s%s%s at %s:%d (ticket %.8s...)", gid, code && *code ? " private " : "", code ? code : "", host,
            port, jti);
    free(ticket);
    return o;
}

/* ------------------------------------------------------------------ HTTP */

static void reply(kn_http_req *req, int status, cJSON *o) {
    kn_http_set_header(req, "Cache-Control", "no-store");
    kn_http_respond_json(req, status, o);
    cJSON_Delete(o);
}

static void reply_err(kn_http_req *req, int status, const char *error) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", error);
    reply(req, status, o);
}

/* The x-game-version gate (FRVR's header; gameVersion in the body for clients that cannot set
 * headers): 1 = passed, else the 426 reply is sent. FRVR's x-min-game-version is the client's copy
 * of its game-config nonLegacy version: informational, the director's own minimum decides. */
static int version_gate(kn_http_req *req, const cJSON *body) {
    const char *v = kn_http_header_get(req, "x-game-version");
    if (!v || !*v) v = kn_json_str(body, "gameVersion", "");
    if (v && version_valid(v) && version_cmp(v, g_d.min_version) >= 0) return 1;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", "GameVersionTooOld");
    cJSON_AddStringToObject(o, "minVersion", g_d.min_version);
    reply(req, 426, o);
    return 0;
}

static int allocate_preamble(kn_http_req *req, cJSON **body_out) {
    *body_out = NULL;
    if (!rate_ok(req->remote_addr, g_d.allocate_per_min)) {
        reply_err(req, 429, "TooManyRequests");
        return 0;
    }
    cJSON *body = req->body_len ? kn_http_body_json(req) : cJSON_CreateObject();
    if (!cJSON_IsObject(body)) {
        cJSON_Delete(body);
        reply_err(req, 400, "BadRequest");
        return 0;
    }
    if (!version_gate(req, body)) {
        cJSON_Delete(body);
        return 0;
    }
    *body_out = body;
    return 1;
}

/* a string field of at most cap-1 chars; 0 = present but too long / not a string */
static int body_str(const cJSON *body, const char *key, char *out, size_t cap) {
    out[0] = 0;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(body, key);
    if (!v || cJSON_IsNull(v)) return 1;
    if (cJSON_IsNumber(v)) {
        snprintf(out, cap, "%d", v->valueint);
        return 1;
    }
    if (!cJSON_IsString(v) || strlen(v->valuestring) >= cap) return 0;
    snprintf(out, cap, "%s", v->valuestring);
    return 1;
}

static void h_live(kn_http_req *req, void *ud) {
    (void) ud;
    const uint64_t now = kn_now_ms();
    cJSON *servers = strike_servers(NULL, now);
    int open = 0;
    const cJSON *srv;
    cJSON_ArrayForEach(srv, servers) {
        const int free_seats = srv_free(srv, NULL, now);
        if (!srv_private(srv) && free_seats > 0) open += free_seats;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 1);
    cJSON_AddStringToObject(o, "service", "kn-strike-director");
    cJSON_AddStringToObject(o, "region", g_d.default_region);
    cJSON_AddNumberToObject(o, "servers", cJSON_GetArraySize(servers));
    cJSON_AddNumberToObject(o, "open", open);
    cJSON_AddNumberToObject(o, "lobbies", (double) g_d.lobby_count);
    cJSON_AddStringToObject(o, "minVersion", g_d.min_version);
    cJSON_AddStringToObject(o, "version", kn_build_info()->describe);
    cJSON_Delete(servers);
    reply(req, 200, o);
}

static void h_servers(kn_http_req *req, void *ud) {
    (void) ud;
    const uint64_t now = kn_now_ms();
    cJSON *servers = strike_servers(NULL, now);
    cJSON *srv;
    cJSON_ArrayForEach(srv, servers) {
        const char *host = kn_json_str(srv, "host", "");
        const int port = (int) kn_json_int(srv, "port", 0);
        char gid[64];
        game_id(kn_json_str(srv, "region", ""), host, port, gid, sizeof gid);
        cJSON_AddStringToObject(srv, "gameId", gid);
        cJSON_AddNumberToObject(srv, "pending", slot_pending(slot_get(host, port, 0), now, PENDING_WINDOW));
        cJSON_AddBoolToObject(srv, "private", srv_private(srv)); /* the code is never listed */
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "servers", servers);
    reply(req, 200, o);
}

static void h_heartbeat(kn_http_req *req, void *ud) {
    (void) ud;
    const char *ip = req->remote_addr ? req->remote_addr : "";
    if (g_d.trusted_count) {
        int ok = 0;
        for (size_t i = 0; i < g_d.trusted_count && !ok; i++) ok = !strcmp(g_d.trusted[i], ip);
        if (!ok) {
            reply_err(req, 403, "Forbidden");
            return;
        }
    }
    if (!rate_ok(ip, 120)) {
        reply_err(req, 429, "TooManyRequests");
        return;
    }
    cJSON *body = kn_http_body_json(req);
    if (!cJSON_IsObject(body)) {
        cJSON_Delete(body);
        reply_err(req, 400, "JSON body required");
        return;
    }
    const char *host = kn_json_str(body, "host", "");
    if (!host || !*host || !strcmp(host, "0.0.0.0")) host = ip;
    const int port = (int) kn_json_int(body, "port", 0);
    if (kn_json_bool(body, "shutdown", 0)) {
        const int removed = finder_remove(g_d.t, host, port, ip);
        if (removed) KN_INFO(MOD, "%s:%d shut down", host, port);
        cJSON_Delete(body);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", 1);
        cJSON_AddBoolToObject(o, "removed", removed);
        reply(req, 200, o);
        return;
    }
    int created = 0;
    const char *err = NULL;
    FinderEntry *e = finder_heartbeat(g_d.t, host, port, (int) kn_json_int(body, "query_port", port),
                                      kn_json_str(body, "name", ""), kn_json_str(body, "community", ""),
                                      kn_json_bool(body, "online_mode", 0), ip, kn_now_ms(), &created, &err);
    const char *flavor = kn_json_str(body, "flavor", "");
    if (e && created)
        KN_INFO(MOD, "new server %s:%d (query %d, says flavor %s) from %s", e->host, e->port, e->query_port,
                flavor[0] ? flavor : "base", ip);
    cJSON_Delete(body);
    if (!e) {
        reply_err(req, 400, err ? err : "refused");
        return;
    }
    if (created) send_poll(e, kn_now_ms());
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 1);
    cJSON_AddNumberToObject(o, "interval", finder_table_config(g_d.t)->heartbeat_interval_s);
    cJSON_AddBoolToObject(o, "verified", e->info != NULL);
    reply(req, 200, o);
}

/* POST /v1/director/allocate {region?, mode?, roomId?} */
static void h_allocate(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *body;
    if (!allocate_preamble(req, &body)) return;
    char region[32], mode[32], room[ROOM_MAX];
    if (!body_str(body, "region", region, sizeof region) || !body_str(body, "mode", mode, sizeof mode) ||
        !body_str(body, "roomId", room, sizeof room)) {
        cJSON_Delete(body);
        reply_err(req, 400, "BadRequest");
        return;
    }
    /* FRVR's openSlots: seats for a party that joins together (1 = just this player). Its
     * beginner `fleet` has no counterpart here: ignored. */
    const int64_t slots = kn_json_int(body, "openSlots", 1);
    const int seats = slots < 1 ? 1 : slots > 32 ? 32 : (int) slots;
    cJSON_Delete(body);
    const uint64_t now = kn_now_ms();
    cJSON *servers = strike_servers(mode, now);
    const cJSON *srv = NULL;
    Pin *pin = pin_get(room, now, 0);
    if (pin) { /* the room's members land in the lobby its first member got, while it has room */
        srv = find_srv(servers, pin->host, pin->port);
        if (srv && (srv_private(srv) || srv_free(srv, NULL, now) < seats)) srv = NULL;
    }
    if (!srv) srv = pick(servers, region, 0, seats, now);
    if (!srv) {
        cJSON_Delete(servers);
        reply_err(req, 503, "NoAvailableServers");
        return;
    }
    cJSON *o = grant(srv, NULL, seats, now);
    if (o && room[0]) {
        Pin *p = pin_get(room, now, 1);
        snprintf(p->host, sizeof p->host, "%s", kn_json_str(srv, "host", ""));
        p->port = (int) kn_json_int(srv, "port", 0);
        p->until_ms = now + (uint64_t) g_d.pin_ms;
        cJSON_AddStringToObject(o, "roomId", room);
    }
    cJSON_Delete(servers);
    if (!o) reply_err(req, 500, "TicketFailed");
    else reply(req, 200, o);
}

/* POST /v1/director/allocate/private {customMatchId} joins the lobby with that code;
 * {roomId} (FRVR: {customId}, "<region>::<id>" or "<id>") joins the room's lobby or opens one for
 * it; {} opens a new lobby. capacity (FRVR sends 12) caps the lobby's seats. */
static void h_allocate_private(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *body;
    if (!allocate_preamble(req, &body)) return;
    char region[32], mode[32], room[ROOM_MAX], custom[ROOM_MAX], code[DIRECTOR_CODE_MAX];
    if (!body_str(body, "region", region, sizeof region) || !body_str(body, "mode", mode, sizeof mode) ||
        !body_str(body, "roomId", room, sizeof room) || !body_str(body, "customId", custom, sizeof custom) ||
        !body_str(body, "customMatchId", code, sizeof code)) {
        cJSON_Delete(body);
        reply_err(req, 400, "BadRequest");
        return;
    }
    if (!room[0] && custom[0]) { /* FRVR: the host's regioned FRVR id, "usc1a::<id>" */
        const char *sep = strstr(custom, "::");
        snprintf(room, sizeof room, "%s", sep ? sep + 2 : custom);
    }
    const int64_t capacity = kn_json_int(body, "capacity", 0);
    cJSON_Delete(body);
    const uint64_t now = kn_now_ms();
    cJSON *servers = strike_servers(NULL, now);
    Lobby *l = code[0] ? lobby_by_code(code) : lobby_by_room(room);
    if (code[0] && !l) {
        cJSON_Delete(servers);
        reply_err(req, 404, "LobbyNotFound");
        return;
    }
    const cJSON *srv = l ? find_srv(servers, l->host, l->port) : NULL;
    if (l && !srv) { /* its server went away */
        const int by_code = code[0] != 0;
        lobby_remove(l);
        l = NULL;
        if (by_code) {
            cJSON_Delete(servers);
            reply_err(req, 404, "LobbyNotFound");
            return;
        }
    }
    int created = 0;
    if (l) {
        if (srv_free_view(srv, NULL, now, PENDING_SINCE_POLL) <= 0) {
            cJSON_Delete(servers);
            reply_err(req, 409, "GameFull");
            return;
        }
    } else {
        /* a new private lobby: an empty Strike server (in the region when possible), held by a code */
        if (mode[0]) {
            cJSON_Delete(servers);
            servers = strike_servers(mode, now);
        }
        srv = pick(servers, region, 1, 1, now);
        if (!srv || g_d.lobby_count >= MAX_LOBBIES) {
            cJSON_Delete(servers);
            reply_err(req, 503, "NoAvailableServers");
            return;
        }
        l = &g_d.lobbies[g_d.lobby_count];
        memset(l, 0, sizeof(*l));
        if (new_code(l->code) != 0) {
            cJSON_Delete(servers);
            reply_err(req, 500, "CodeFailed");
            return;
        }
        g_d.lobby_count++;
        snprintf(l->room, sizeof l->room, "%s", room);
        snprintf(l->host, sizeof l->host, "%s", kn_json_str(srv, "host", ""));
        l->port = (int) kn_json_int(srv, "port", 0);
        l->opened_ms = now;
        l->capacity = capacity > 0 && capacity <= 64 ? (int) capacity : 0;
        created = 1;
        KN_INFO(MOD, "private lobby %s opened on %s:%d%s%s", l->code, l->host, l->port, room[0] ? " for room " : "", room);
    }
    l->last_alloc_ms = now;
    cJSON *o = grant(srv, l->code, 1, now);
    cJSON_Delete(servers);
    if (!o) {
        reply_err(req, 500, "TicketFailed");
        return;
    }
    cJSON_AddStringToObject(o, "code", l->code);
    cJSON_AddStringToObject(o, "customMatchId", l->code);
    if (l->room[0]) cJSON_AddStringToObject(o, "roomId", l->room);
    cJSON_AddBoolToObject(o, "created", created);
    reply(req, 200, o);
}

/* ------------------------------------------------------------------ timers */

static void tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    const uint64_t now = kn_now_ms();
    const size_t removed = finder_sweep(g_d.t, now, free);
    if (removed) KN_INFO(MOD, "%zu server(s) expired, %zu registered", removed, finder_table_count(g_d.t));
    FinderEntry *due[POLLS_PER_TICK];
    const size_t n = finder_due_polls(g_d.t, now, due, POLLS_PER_TICK);
    for (size_t i = 0; i < n; i++) send_poll(due[i], now);
    /* private lobbies end when their server is gone, or when it has been empty (verified by a
     * poll after the last ticket's join window) for private_idle_s since the last allocation */
    for (size_t i = 0; i < g_d.lobby_count;) {
        Lobby *l = &g_d.lobbies[i];
        const FinderEntry *e = entry_get(l->host, l->port);
        const int gone = !e;
        const int idle = e && e->info && now - l->last_alloc_ms > (uint64_t) g_d.private_idle_ms &&
                         e->last_ok_ms > l->last_alloc_ms + (uint64_t) g_d.window_ms &&
                         kn_json_int(e->info, "players", 0) == 0 &&
                         slot_pending(slot_get(l->host, l->port, 0), now, PENDING_WINDOW) == 0;
        if (gone || idle) lobby_remove(l);
        else i++;
    }
}

/* ------------------------------------------------------------------ main */

static int cfg_ms(const char *key, int def_s) {
    const int64_t v = kn_config_int(g_d.cfg, key, def_s);
    return (int) ((v > 0 ? v : def_s) * 1000);
}

static void usage(void) {
    printf("kn_strike_director - Krunker Strike matchmaker (doc/v10/strike-director.md)\n"
           "  --config <file>                       config/config.json (section strike_director)\n"
           "  --strike_director.port=<n>            HTTP port (default 8095, 0 = ephemeral)\n"
           "  --strike_director.ticket_secret=<s>   shared with gameserver.director_secret (required)\n"
           "  --version\n");
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version")) {
            printf("kn_strike_director %s\n", kn_build_string());
            return 0;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage();
            return 0;
        }
    }
    kn_log_init("strike-director");
    char err[256] = "";
    g_d.cfg = kn_config_load(NULL, argc, argv, err, sizeof err);
    if (!g_d.cfg) {
        KN_FATAL(MOD, "config: %s", err);
        return 2;
    }
    kn_log_configure(g_d.cfg);
    const kn_config *cfg = g_d.cfg;

    if (director_ticket_key(kn_config_str(cfg, "strike_director.ticket_secret", ""), g_d.key) != 0) {
        KN_FATAL(MOD, "strike_director.ticket_secret is required (the game servers' gameserver.director_secret)");
        kn_config_free(g_d.cfg);
        return 2;
    }
    const int64_t ttl = kn_config_int(cfg, "strike_director.ticket_ttl_s", 60);
    g_d.ttl_s = ttl < 5 ? 5 : ttl > 3600 ? 3600 : (int) ttl;
    g_d.window_ms = cfg_ms("strike_director.join_window_s", 15);
    g_d.private_idle_ms = cfg_ms("strike_director.private_idle_s", 120);
    g_d.pin_ms = cfg_ms("strike_director.room_pin_s", 120);
    g_d.allocate_per_min = (int) kn_config_int(cfg, "strike_director.allocate_per_min", 60);
    /* x-game-version below this is refused (426); default: this build's network protocol, the
     * oldest client that can play on servers built from the same tree */
    snprintf(g_d.min_version, sizeof g_d.min_version, "%s",
             kn_config_str(cfg, "strike_director.min_game_version", kn_build_protocol()));
    snprintf(g_d.default_region, sizeof g_d.default_region, "%s", kn_config_str(cfg, "strike_director.region", "local"));
    g_d.trusted_count = kn_config_str_list(cfg, "strike_director.trusted_submitters", g_d.trusted, 64);

    FinderConfig fc;
    finder_config_defaults(&fc);
    fc.poll_interval_ms = cfg_ms("strike_director.poll_interval_s", 10);
    fc.poll_timeout_ms = cfg_ms("strike_director.poll_timeout_s", 3);
    fc.expire_ms = cfg_ms("strike_director.expire_s", 180);
    fc.max_failures = (int) kn_config_int(cfg, "strike_director.max_failures", 3);
    fc.max_per_ip = (int) kn_config_int(cfg, "strike_director.max_per_ip", 64);
    fc.heartbeat_interval_s = (int) kn_config_int(cfg, "strike_director.heartbeat_interval_s", 30);
    if (fc.max_failures < 1) fc.max_failures = 1;
    g_d.t = finder_table_new(&fc);
    g_d.loop = kn_loop_new();
    if (!g_d.t || !g_d.loop) {
        KN_FATAL(MOD, "out of memory");
        return 1;
    }
    kn_loop_stop_on_signals(g_d.loop);
    g_d.fd4 = open_udp(AF_INET);
    g_d.fd6 = open_udp(AF_INET6);
    if (g_d.fd4 < 0 && g_d.fd6 < 0) {
        KN_FATAL(MOD, "no UDP socket for KNINFO polls: %s", strerror(errno));
        return 1;
    }

    kn_http_config hc = {0};
    kn_http_config_defaults(&hc);
    hc.host = kn_config_str(cfg, "strike_director.host", "0.0.0.0");
    hc.port = (int) kn_config_int(cfg, "strike_director.port", 8095);
    hc.max_body_bytes = 16 * 1024;
    hc.trust_proxy = kn_config_bool(cfg, "strike_director.trust_proxy", 0);
    hc.server_name = "kn-strike-director";
    const char *cors[16];
    size_t ncors = kn_config_str_list(cfg, "strike_director.cors_origins", cors, 16);
    if (!kn_config_has(cfg, "strike_director.cors_origins")) {
        cors[0] = "*"; /* the web build calls from its own origin */
        ncors = 1;
    }
    hc.cors_origins = cors;
    hc.cors_origin_count = ncors;
    g_d.http = kn_http_server_new(g_d.loop, &hc);
    if (!g_d.http) {
        KN_FATAL(MOD, "cannot create the HTTP server");
        return 1;
    }
    kn_http_route(g_d.http, "GET", "/live", h_live, NULL);
    kn_http_route(g_d.http, "POST", "/v1/director/allocate", h_allocate, NULL);
    kn_http_route(g_d.http, "POST", "/v1/director/allocate/private", h_allocate_private, NULL);
    kn_http_route(g_d.http, "GET", "/v1/director/servers", h_servers, NULL);
    kn_http_route(g_d.http, "POST", "/servers/heartbeat", h_heartbeat, NULL);
    kn_http_route(g_d.http, "POST", "/servers/submit", h_heartbeat, NULL);
    if (kn_http_server_listen(g_d.http) != 0) {
        KN_FATAL(MOD, "cannot listen on %s:%d", hc.host, hc.port);
        return 1;
    }
    kn_loop_add_timer(g_d.loop, TICK_MS, TICK_MS, tick, NULL);
    KN_INFO(MOD, "Krunker Strike director %s listening on http://%s:%d (region %s, min game version %s, ticket ttl %d s)",
            kn_build_info()->describe, hc.host, kn_http_server_port(g_d.http), g_d.default_region, g_d.min_version, g_d.ttl_s);

    const int rc = kn_loop_run(g_d.loop);

    kn_http_server_free(g_d.http);
    if (g_d.fd4 >= 0) close(g_d.fd4);
    if (g_d.fd6 >= 0) close(g_d.fd6);
    finder_table_free(g_d.t);
    for (size_t i = 0; i < g_d.slot_count; i++) free(g_d.slots[i]);
    kn_loop_free(g_d.loop);
    kn_config_free(g_d.cfg);
    KN_INFO(MOD, "stopped");
    return rc == 0 ? 0 : 1;
}
