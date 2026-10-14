// Server-info query responder and finder heartbeat (server/include/info_query.h).
#include <ruleset.h>
#include <bots.h>
#include <build_info.h>
#include <info_query.h>
#include <http_client.h>
#include <server_cli.h>
#include <server_config.h>
#include <server_accounts.h>
#include <cJSON.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef WIN32
#include <poll.h>
#include <unistd.h>
#endif

volatile int g_info_query_external;

static struct {
    pthread_mutex_t lock;
    ServerInfo info;           // snapshot (nonce empty)
    float next_update;
    int fd;
    pthread_t udp_thread, finder_thread;
    int udp_started, finder_started;
    volatile int stopping;
    pthread_mutex_t wake_lock;
    pthread_cond_t wake;
} g_iq = {.lock = PTHREAD_MUTEX_INITIALIZER, .fd = -1, .wake_lock = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER};

size_t server_info_handle_datagram(const uint8_t *in, const size_t len, uint8_t *out, const size_t cap) {
    char nonce[SERVER_INFO_NONCE_MAX + 1];
    if (!server_info_parse_request(in, len, nonce)) return 0;
    ServerInfo info;
    pthread_mutex_lock(&g_iq.lock);
    info = g_iq.info;
    pthread_mutex_unlock(&g_iq.lock);
    snprintf(info.nonce, sizeof(info.nonce), "%s", nonce);
    return server_info_build_reply(out, cap, &info);
}

static void iq_add_tag(ServerInfo *info, const char *tag) {
    if (!tag || !tag[0] || info->tag_count >= SERVER_INFO_TAGS_MAX) return;
    for (int i = 0; i < info->tag_count; i++)
        if (!strcmp(info->tags[i], tag)) return;
    snprintf(info->tags[info->tag_count++], sizeof(info->tags[0]), "%s", tag);
}

void info_query_update(const float now) {
    if (now < g_iq.next_update && g_iq.next_update - now < 2.0f) return;
    g_iq.next_update = now + 0.25f;
    const GsConfig *cfg = &g_gs_config;
    const Game *game = &g_server->game;
    ServerInfo info;
    memset(&info, 0, sizeof(info));
    snprintf(info.name, sizeof(info.name), "%s", cfg->name);
    const cJSON *map = game->ready && game->current_map_index >= 0 && (size_t) game->current_map_index < game->map_count
                           ? game->maps[game->current_map_index] : NULL;
    const char *map_name = cJSON_GetStringValue(cJSON_GetObjectItem(map, "name"));
    snprintf(info.map, sizeof(info.map), "%s", map_name ? map_name : "");
    info.mode_id = game_mode_id(game);
    snprintf(info.mode, sizeof(info.mode), "%s", info.mode_id >= 0 ? mode_name(info.mode_id) : "");
    info.players = accounts_player_count();
    info.max_players = g_server->config.max_players;
    info.bots = game_bots_count(game); // v7.1.4 bot players: not in `players` (connections only)
    // online_mode on the wire (finder listing, KNINFO) is "signed-in players only": what server_bin
    // builds that refused guests sent, and what older clients label "Account required" (or hide
    // in their offline mode). Every server admits guests now, so it is always false; online_mode
    // in server.cfg only adds hasJoined account verification (accounts.c acc_online_join).
    info.online_mode = 0;
    info.password = cfg->password[0] != 0;
    snprintf(info.community, sizeof(info.community), "%s", accounts_community_name());
    snprintf(info.region, sizeof(info.region), "%s", cfg->region);
    // build identity (build_info.h): the finder flags servers older than the latest release
    const KnBuildInfo *build = kn_build_info();
    snprintf(info.version, sizeof(info.version), "%s", build->describe);
    snprintf(info.commit, sizeof(info.commit), "%s", build->commit);
    info.commit_time = build->commit_time;
    snprintf(info.build_time, sizeof(info.build_time), "%s", build->build_time);
    snprintf(info.protocol, sizeof(info.protocol), "%s", kn_build_protocol());
    snprintf(info.ruleset, sizeof(info.ruleset), "%s", ruleset_current()->key);
    info.port = cfg->public_port ? cfg->public_port : cfg->listen_port;
    if (cfg->password[0]) iq_add_tag(&info, "password");
    if (game_bots_autofill(game, NULL)) iq_add_tag(&info, "autofill"); // KrunkNative `autofill`: bots fill the lobby
    char dynamic[4][24];
    const int n = accounts_info_tags(dynamic, 4); // ranked / custom / draining / community
    for (int i = 0; i < n; i++) iq_add_tag(&info, dynamic[i]);
    char extra[sizeof(cfg->tags)];
    snprintf(extra, sizeof(extra), "%s", cfg->tags);
    for (char *save = NULL, *t = strtok_r(extra, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        while (*t == ' ') t++;
        iq_add_tag(&info, t);
    }
    pthread_mutex_lock(&g_iq.lock);
    g_iq.info = info;
    pthread_mutex_unlock(&g_iq.lock);
}

// ------------------------------------------------------------------ UDP responder

#ifndef WIN32
static void *iq_udp_main(void *arg) {
    (void) arg;
    uint8_t in[256], out[SERVER_INFO_REPLY_MAX + 64];
    while (!g_iq.stopping) {
        struct pollfd pfd = {.fd = g_iq.fd, .events = POLLIN};
        if (poll(&pfd, 1, 200) <= 0) continue;
        struct sockaddr_storage from;
        socklen_t from_len = sizeof(from);
        const ssize_t got = recvfrom(g_iq.fd, in, sizeof(in), 0, (struct sockaddr *) &from, &from_len);
        if (got <= 0) continue;
        const size_t reply = server_info_handle_datagram(in, (size_t) got, out, sizeof(out));
        if (reply) sendto(g_iq.fd, out, reply, 0, (struct sockaddr *) &from, from_len);
    }
    return NULL;
}

static int iq_open_socket(const unsigned short port) {
    const int fd = (int) socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (!g_gs_config.listen_host[0] || inet_pton(AF_INET, g_gs_config.listen_host, &addr.sin_addr) != 1) addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr))) {
        close(fd);
        return -1;
    }
    return fd;
}
#endif

// ------------------------------------------------------------------ finder heartbeat

static int iq_wait(const int seconds) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += seconds;
    pthread_mutex_lock(&g_iq.wake_lock);
    while (!g_iq.stopping) {
        if (pthread_cond_timedwait(&g_iq.wake, &g_iq.wake_lock, &until)) break;
    }
    const int stopping = g_iq.stopping;
    pthread_mutex_unlock(&g_iq.wake_lock);
    return !stopping;
}

static int iq_heartbeat(const int shutdown) {
    const GsConfig *cfg = &g_gs_config;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "v", 1);
    // public_host 127.0.0.1 (the default) is useless to a remote finder: "" = use the source IP
    const int loopback = !strcmp(cfg->public_host, "127.0.0.1") || !strcmp(cfg->public_host, "localhost");
    cJSON_AddStringToObject(body, "host", loopback && !strstr(cfg->finder_url, "127.0.0.1") ? "" : cfg->public_host);
    cJSON_AddNumberToObject(body, "port", cfg->public_port ? cfg->public_port : cfg->listen_port);
    cJSON_AddNumberToObject(body, "query_port", cfg->query_port ? cfg->query_port : cfg->listen_port);
    cJSON_AddStringToObject(body, "name", cfg->name);
    cJSON_AddStringToObject(body, "community", accounts_community_name());
    cJSON_AddBoolToObject(body, "online_mode", 0); // "guests refused", never (iq_update)
    const KnBuildInfo *build = kn_build_info(); // informational: the listing uses the verified KNINFO reply
    cJSON_AddStringToObject(body, "version", build->describe);
    cJSON_AddStringToObject(body, "commit", build->commit);
    cJSON_AddNumberToObject(body, "commit_time", (double) build->commit_time);
    cJSON_AddStringToObject(body, "build_time", build->build_time);
    cJSON_AddStringToObject(body, "protocol", kn_build_protocol());
    if (shutdown) cJSON_AddBoolToObject(body, "shutdown", 1);
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    char url[GS_STR_LARGE + 32];
    snprintf(url, sizeof(url), "%s/servers/heartbeat", cfg->finder_url);
    HttpResult r;
    const int rc = text ? http_request("POST", url, "application/json", text, shutdown ? 2000 : 8000, 4096, &r) : -1;
    cJSON_free(text);
    int interval = 60;
    if (rc) {
        server_log_error("[finder] heartbeat to %s failed: %s", url, r.error);
    } else if (r.status != 200) {
        server_log_error("[finder] heartbeat to %s: HTTP %d %.120s", url, r.status, r.body ? r.body : "");
    } else {
        cJSON *reply = r.body ? cJSON_Parse(r.body) : NULL;
        const cJSON *iv = cJSON_GetObjectItem(reply, "interval");
        if (cJSON_IsNumber(iv) && iv->valueint >= 10 && iv->valueint <= 3600) interval = iv->valueint;
        cJSON_Delete(reply);
    }
    http_result_free(&r);
    return interval;
}

static void *iq_finder_main(void *arg) {
    (void) arg;
    server_log("[finder] listing this server at %s (opt-in, gameserver.finder_url)", g_gs_config.finder_url);
    int interval = 1;
    int first = 1;
    while (iq_wait(interval)) {
        interval = iq_heartbeat(0);
        if (first) server_log("[finder] registered, heartbeat every %d s", interval);
        first = 0;
    }
    iq_heartbeat(1);
    return NULL;
}

// ------------------------------------------------------------------ lifecycle

int info_query_start(void) {
    pthread_mutex_lock(&g_server->lock);
    g_iq.next_update = 0.0f;
    info_query_update(g_server->now);
    pthread_mutex_unlock(&g_server->lock);
    const unsigned short port = g_gs_config.query_port ? g_gs_config.query_port : g_gs_config.listen_port;
#if defined(SERVER_NET_OWNS_INFO_QUERY)
    g_info_query_external = 1;
#endif
#ifndef WIN32
    if (!g_info_query_external) {
        g_iq.fd = iq_open_socket(port);
        if (g_iq.fd < 0) server_log_error("[query] cannot bind UDP port %u: server-info queries disabled", (unsigned int) port);
        else if (!pthread_create(&g_iq.udp_thread, NULL, iq_udp_main, NULL)) g_iq.udp_started = 1;
        if (g_iq.udp_started) server_log("[query] answering server-info queries on UDP %u", (unsigned int) port);
    }
#endif
    if (g_gs_config.finder_url[0] && !pthread_create(&g_iq.finder_thread, NULL, iq_finder_main, NULL)) g_iq.finder_started = 1;
    return 0;
}

void info_query_stop(void) {
    pthread_mutex_lock(&g_iq.wake_lock);
    g_iq.stopping = 1;
    pthread_cond_broadcast(&g_iq.wake);
    pthread_mutex_unlock(&g_iq.wake_lock);
    if (g_iq.finder_started) pthread_join(g_iq.finder_thread, NULL);
    if (g_iq.udp_started) pthread_join(g_iq.udp_thread, NULL);
    g_iq.finder_started = g_iq.udp_started = 0;
#ifndef WIN32
    if (g_iq.fd >= 0) close(g_iq.fd);
#endif
    g_iq.fd = -1;
}
