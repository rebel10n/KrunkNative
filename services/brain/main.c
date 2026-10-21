/*
 * services/brain/main.c - kn_brain: the KrunkNative matchmaker "brain"
 * (Krunker v3.1.1 matchmaker/index.ts) in C.
 *
 * One HTTP listener (nginx terminates TLS in deployment) serving:
 *   - public matchmaker routes /mm/... (+ prefix-less aliases) public_routes.c
 *   - the internal WebSocket for game servers /mm/internal/ws   internal_ws.c
 *   - the social WebSocket + accounts routes                    (svc-accounts)
 * and a database-backed registry shared by every brain        federation.c
 *
 * Usage: kn_brain [--config FILE] [--migrate] [--migrate-only] [--no-db]
 *                 [--brain.port=N ...any config override] (doc/backend.md)
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "brain_internal.h"
#include "accounts.h"
#include "social_ws.h"
#include "roles.h"
#include "finder.h"
#include "hot.h"
#include "releases.h"
#include "ai.h"
#include "streams.h"
#include "user_assets.h"
#include "v10_api.h"
#include "v10_empty.h"
#include "v10_market.h"
#include "avatar.h"
#include "clan_logo.h"
#include "push.h"
#include "strike_econ.h"
#include "build_info.h"
#include "ruleset.h"
#include "knsvc/config.h"
#include "knsvc/db.h"
#include "knsvc/http.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"
#include "knsvc/migrate.h"

static Brain g_brain;

Brain *brain_get(void) {
    return &g_brain;
}

static int registry_module_init(Brain *b) {
    b->registry = registry_new();
    return b->registry ? 0 : -1;
}

/* ---- module table: init in order, shutdown in reverse. Other owners add ONE
 * line each here (anchored edit) plus their sources in CMakeLists.txt. */
static const BrainModule k_brain_modules[] = {
    {"registry", registry_module_init, NULL},
    {"hot", hot_init, hot_shutdown}, /* optional Redis hot tier (hot.h): before anything listens */
    {"releases", releases_init, releases_shutdown}, /* latest server build (releases.h) */
    {"federation", federation_init, federation_shutdown},
    /* svc-accounts modules go here (accounts, social WS) */
    {"accounts", accounts_init, accounts_shutdown},
    {"social-ws", social_ws_init, social_ws_shutdown},
    {"creations", creations_init, NULL},
    {"public-routes", public_routes_init, NULL},
    {"ranked", ranked_init, ranked_shutdown},
    {"turf", turf_init, turf_shutdown},
    {"lottery", lottery_init, lottery_shutdown}, /* v10 lottery windows and draws (lottery.c) */
    {"shop", shop_init, shop_shutdown},          /* the Junk-priced shop (shop.c) */
    {"guest", guest_init, guest_shutdown},       /* the signed-out guest's balance: /guest/profile, /guest/claim (guest.c) */
    {"community", community_init, community_shutdown}, /* v10 events / news feed (community.c) */
    {"leaderboards-http", leaderboards_http_init, NULL}, /* v10's /api/leaderboards/... (leaderboards.c) */
    {"internal-ws", internal_ws_init, internal_ws_shutdown},
    /* coord-auth: central auth + server finder (roles.h decides what runs) */
    {"auth", auth_init, auth_shutdown},
    {"finder", finder_init, finder_shutdown},
    /* editor-ai: the map editor's AI proxy (ai.h; community role) */
    {"ai", ai_init, ai_shutdown},
    {"ai-search", ai_search_init, ai_search_shutdown}, /* the AI's web search (ai_search.c) */
    {"streams", streams_init, streams_shutdown}, /* v10 Live Streams: /streams/top (streams.c) */
    {"avatar", avatar_init, avatar_shutdown}, /* profile pictures: upload, AI review, serving (avatar.c) */
    {"clan-logo", clan_logo_init, clan_logo_shutdown}, /* clan logos: upload, AI review, serving (clan_logo.c) */
    {"push", push_init, NULL}, /* Web Push subscriptions: vapid-key, subscribe, unsubscribe (push.c) */
    {"stream-thumbs", stream_thumbs_init, stream_thumbs_shutdown}, /* the cards' previews: /streams/thumb */
    {"strike-econ", strike_econ_init, strike_econ_shutdown}, /* Krunker Strike's economy (strike_econ.h) */
    {"user-assets", user_assets_init, user_assets_shutdown}, /* community maps' Krunker user assets (user_assets.c) */
    {"v10-api", v10_api_init, NULL}, /* v10 REST at the root: maps, titles, packed skins and spins (v10_api.c) */
    {"v10-empty", v10_empty_init, NULL}, /* v10 REST routes with no KrunkNative data (v10_empty.c) */
    {"v10-market", v10_market_init, NULL}, /* v10 market, items, trades, moderation, mods, KPD (v10_market.c) */
};
#define BRAIN_MODULE_COUNT (sizeof(k_brain_modules) / sizeof(k_brain_modules[0]))

static size_t g_inited;      /* modules whose init succeeded */
static int g_shutdown_done;

static void modules_shutdown(Brain *b) {
    if (g_shutdown_done) return;
    g_shutdown_done = 1;
    while (g_inited > 0) {
        const BrainModule *m = &k_brain_modules[--g_inited];
        if (m->shutdown && brain_module_enabled(m->name)) m->shutdown(b);
    }
}

static void stop_now(kn_loop *loop, void *ud) {
    (void) ud;
    kn_loop_stop(loop);
}

static void on_signal(kn_loop *loop, int signo, void *ud) {
    Brain *b = ud;
    if (b->draining) {
        KN_WARN("brain", "signal %d again: exiting now", signo);
        kn_loop_stop(loop);
        return;
    }
    KN_INFO("brain", "signal %d received, shutting down", signo);
    b->draining = 1;
    modules_shutdown(b);
    /* let close frames and the last writes go out */
    kn_loop_add_timer(loop, 400, 0, stop_now, NULL);
}

static void usage(const char *argv0) {
    printf("Usage: %s [--config FILE] [--migrate] [--migrate-only] [--no-db] [--<section.key>=VALUE ...]\n"
           "  --migrate        apply pending services/db/migrations before starting\n"
           "  --migrate-only   apply migrations and exit (same as kn_migrate)\n"
           "  --no-db          run without a database (registry is local to this brain)\n"
           "  --role R[,R]     auth, community, finder (brain.roles; default all three)\n"
           "  --brain.port=N   any config key can be overridden like this (or KN_BRAIN_PORT=N)\n"
           "Config: config/config.json (\"brain\" section, doc/backend.md).\n",
           argv0);
}

static int run_migrations(const kn_config *cfg) {
    kn_db_config dc;
    kn_db_config_from(cfg, &dc);
    char err[512] = "";
    kn_dbconn *c = kn_dbconn_open(&dc, err, sizeof(err));
    if (!c) {
        KN_ERROR("brain", "migrate: cannot connect: %s", err);
        return -1;
    }
    kn_migrate_opts mo = {0};
    char dir[1024];
    if (kn_migrate_resolve_dir(cfg, dir, sizeof(dir))) mo.dir = dir;
    int n = kn_migrate_run(c, &mo, err, sizeof(err));
    kn_dbconn_close(c);
    if (n < 0) {
        KN_ERROR("brain", "migrate: %s", err);
        return -1;
    }
    KN_INFO("brain", "migrate: %d migration(s) applied", n);
    return 0;
}

int main(int argc, char **argv) {
    kn_log_init("brain");
    int migrate = 0, migrate_only = 0, no_db = 0;
    char err[512] = "";
    kn_config *cfg = kn_config_load(NULL, argc, argv, err, sizeof(err));
    if (!cfg) {
        fprintf(stderr, "kn_brain: config: %s\n", err);
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        if (kn_config_arg_consumed(cfg, i)) continue;
        if (!strcmp(argv[i], "--migrate")) migrate = 1;
        else if (!strcmp(argv[i], "--migrate-only")) migrate_only = 1;
        else if (!strcmp(argv[i], "--no-db")) no_db = 1;
        else if (!strcmp(argv[i], "--role") && i + 1 < argc) kn_config_set(cfg, "brain.roles", argv[++i]);
        else if (!strncmp(argv[i], "--role=", 7)) kn_config_set(cfg, "brain.roles", argv[i] + 7);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            kn_config_free(cfg);
            return 0;
        } else if (!strcmp(argv[i], "--version")) {
            printf(KN_PRODUCT_NAME " kn_brain, Krunker v%s\nbuild %s\n", ruleset_current()->version, kn_build_string());
            kn_config_free(cfg);
            return 0;
        } else {
            fprintf(stderr, "kn_brain: unknown argument %s\n", argv[i]);
            usage(argv[0]);
            kn_config_free(cfg);
            return 2;
        }
    }
    kn_log_configure(cfg);
    if (!brain_roles_load(cfg)) {
        fprintf(stderr, "kn_brain: brain.roles names no known role (auth, community, finder)\n");
        kn_config_free(cfg);
        return 2;
    }
    KN_INFO("brain", "kn_brain, Krunker v%s, build %s, config %s", ruleset_current()->version, kn_build_commit_id(),
            kn_config_path(cfg)[0] ? kn_config_path(cfg) : "(none, defaults)");
    /* database.auto_migrate (default: on for the zero-config sqlite file) applies
     * pending migrations at every start, like --migrate (coord-store) */
    if (!migrate && !no_db) {
        kn_db_config adc;
        kn_db_config_from(cfg, &adc);
        migrate = kn_config_bool(cfg, "database.auto_migrate", adc.driver && !strcmp(adc.driver, "sqlite"));
    }
    if ((migrate || migrate_only) && !no_db) {
        if (run_migrations(cfg)) {
            kn_config_free(cfg);
            return 1;
        }
        if (migrate_only) {
            kn_config_free(cfg);
            return 0;
        }
    }

    Brain *b = &g_brain;
    b->cfg = cfg;
    b->started_ms = kn_wall_ms();
    snprintf(b->version, sizeof(b->version), "%s", kn_build_commit_id()); // the brain's build: its commit (status, brainVersion)
    snprintf(b->region, sizeof(b->region), "%s", kn_config_str(cfg, "brain.region", "local"));
    const char *host = kn_config_str(cfg, "brain.host", "127.0.0.1");
    int port = (int) kn_config_int(cfg, "brain.port", 5050);
    const char *id = kn_config_str(cfg, "brain.id", "");
    if (id && *id) {
        snprintf(b->id, sizeof(b->id), "%s", id);
    } else {
        char hn[48] = "brain";
        gethostname(hn, sizeof(hn) - 1);
        snprintf(b->id, sizeof(b->id), "%s-%d", hn, port);
    }

    b->loop = kn_loop_new();
    if (!b->loop) {
        KN_FATAL("brain", "event loop");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    kn_loop_on_signal(b->loop, SIGINT, on_signal, b);
    kn_loop_on_signal(b->loop, SIGTERM, on_signal, b);

    if (!no_db) {
        kn_db_config dc;
        kn_db_config_from(cfg, &dc);
        if (!dc.application_name || !strcmp(dc.application_name, "knsvc")) dc.application_name = "kn_brain";
        b->db = kn_db_new(b->loop, &dc);
        if (!b->db) KN_ERROR("brain", "database pool could not be created; running without DB");
        else if (kn_db_wait_ready(b->db, 5000))
            KN_WARN("brain", "database not reachable yet (pool keeps retrying)");
    }
    mm_settings_load(b);

    kn_http_config hc;
    memset(&hc, 0, sizeof(hc));
    kn_http_config_defaults(&hc);
    hc.host = host;
    hc.port = port;
    hc.trust_proxy = kn_config_bool(cfg, "brain.trust_proxy", 0);
    hc.server_name = "kn_brain";
    hc.max_body_bytes = (size_t) kn_config_int(cfg, "brain.max_body_bytes", 1 << 20);
    hc.max_header_bytes = 32 * 1024; /* GET /mm/host-game?config=<json> */
    const char *origins[32];
    size_t norigins = kn_config_has(cfg, "brain.cors_origins")
                          ? kn_config_str_list(cfg, "brain.cors_origins", origins, 32)
                          : 0;
    if (!kn_config_has(cfg, "brain.cors_origins")) {
        origins[0] = "*";
        norigins = 1;
    }
    if (norigins > 32) norigins = 32;
    hc.cors_origins = origins;
    hc.cors_origin_count = norigins;
    b->http = kn_http_server_new(b->loop, &hc);
    if (!b->http) {
        KN_FATAL("brain", "http server");
        return 1;
    }

    int rc = 0;
    for (size_t i = 0; i < BRAIN_MODULE_COUNT; i++) {
        const BrainModule *m = &k_brain_modules[i];
        if (!brain_module_enabled(m->name)) {
            g_inited = i + 1;
            continue;
        }
        if (m->init && m->init(b)) {
            KN_FATAL("brain", "module %s failed to start", m->name);
            rc = 1;
            break;
        }
        g_inited = i + 1;
    }
    if (!rc && kn_http_server_listen(b->http)) {
        KN_FATAL("brain", "cannot listen on %s:%d", host, port);
        rc = 1;
    }
    if (!rc) {
        /* tests parse "listening on http://H:PORT (ws" from this line (brain.port=0): keep the format */
        KN_INFO("brain", "roles: %s", brain_roles_text());
        KN_INFO("brain", "brain \"%s\" (region %s) listening on http://%s:%d (ws /mm/internal/ws)%s", b->id,
                b->region, host, kn_http_server_port(b->http), b->db ? "" : " [no database]");
        rc = kn_loop_run(b->loop) ? 1 : 0;
    }
    b->draining = 1;
    modules_shutdown(b);
    kn_http_server_free(b->http);
    b->http = NULL;
    if (b->db) kn_db_free(b->db);
    b->db = NULL;
    registry_free(b->registry);
    b->registry = NULL;
    kn_loop_free(b->loop);
    kn_config_free(cfg);
    KN_INFO("brain", "bye");
    return rc;
}
