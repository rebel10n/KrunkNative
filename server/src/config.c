// Game server configuration: config/config.json "gameserver" (server/include/server_config.h).
//
// Same conventions as the services' knsvc/config.h, so one config file and one set of
// environment variables drive the brain and every game server:
//   file:  --config FILE, else $KN_CONFIG, else the first config/config.json in the cwd or
//          up to 4 parent directories; a missing file means "all defaults".
//   env:   KN_GAMESERVER_<KEY> (gameserver.max_players -> KN_GAMESERVER_MAX_PLAYERS), values
//          parsed as JSON when they look like JSON; v3.1.1 aliases MM_BRAIN_URL,
//          MM_BRAIN_SECRET, MM_NODE_REGION, MM_NODE_ID, PUBLIC_DOMAIN, PUBLIC_PORT, PERMS.
//   CLI:   server_bin's own flags (--port, --mode, ...) and --gameserver.<key>=<json|text>
//          (main.c), applied last.
#include <server_config.h>
#include <build_info.h> // KN_PRODUCT_NAME
#include <flavor.h> // gameserver.flavor: the product flavor (Krunker Strike)
#include <flavor_rules.h> // the flavor's lobby defaults and mode rotation (Krunker Strike)
#include <strike_rules.h> // StrikeRules: Krunker Strike's modes and lobby cap
#include <server.h> // g_server (effective lobby size in the summary)
#include <server_log.h>
#include <map_catalog.h>
#include <published_maps.h> // gameserver.maps = published:<creator>/<name>
#include <game_config.h> // WP-E: v7 serverConfig table (game.<varN>)
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GsConfig g_gs_config;

static char g_config_root[GS_STR_LARGE]; // repository root guessed from the config path ("" = cwd)

static void gs_copy(char *dst, const size_t size, const char *src) {
    snprintf(dst, size, "%s", src ? src : "");
}

static int g_flavor_locked; // --flavor given: gameserver.flavor does not apply (gs_config_lock_flavor)

void gs_config_lock_flavor(void) { g_flavor_locked = 1; }

void gs_config_defaults(GsConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    gs_copy(cfg->region, sizeof(cfg->region), "local");
    gs_copy(cfg->node_id, sizeof(cfg->node_id), "LOC");
    gs_copy(cfg->name, sizeof(cfg->name), KN_PRODUCT_NAME); // the server browser name (hostname)
    gs_copy(cfg->listen_host, sizeof(cfg->listen_host), "0.0.0.0");
    cfg->listen_port = NET_DEFAULT_PORT;
    cfg->host_type = -1; // WP-E: a public game
    gs_copy(cfg->public_host, sizeof(cfg->public_host), "127.0.0.1");
    gs_copy(cfg->brain_secret, sizeof(cfg->brain_secret), "kn-dev-secret"); // = kn_brain's brain.internal_secret default
    cfg->tick_rate = 30;
    cfg->max_players = flavor_max_players() > 0 ? flavor_max_players() : 8; // Krunker Strike: 10
    cfg->game = *flavor_default_game_config(); // base: g_default_game_config
    cfg->custom_games = 1;
    // The server's chat lines to every joiner on their first spawn ($ = their name); none by default.
    // The map's welMsg is the menu's line (menu_ui.c #mapWelcome), so no game welcome line is sent
    // unless a config's "motd" asks for one.
    cfg->motd_count = 0;
    gs_copy(cfg->join_message, sizeof(cfg->join_message), "$ joined the game"); // config.serverMessages[0]
    gs_copy(cfg->leave_message, sizeof(cfg->leave_message), "$ left the game"); // serverMessages[1]
    cfg->idle_timeout = 60.0f;          // gameserver.ts SERVER_IDLE_TIMEOUT_MS
    cfg->idle_menu_timeout = 300.0f;    // a backstop behind the client's v7 menu kick (config.kickTimer 120 s)
    cfg->idle_move_threshold = 1.0f;    // SERVER_IDLE_MOVE_THRESHOLD
    cfg->allow_guests = 1;
    cfg->join_timeout = 5.0f;
    cfg->earn_kr = 1;
    cfg->kr_cap = 30;                   // game.js endGame: min(premiumT ? 40 : 30, ...)
    cfg->kr_cap_premium = 40;
    cfg->kr_multiplier = 1.0f;
    gs_copy(cfg->rcon_host, sizeof(cfg->rcon_host), "127.0.0.1");
    gs_copy(cfg->auth_url, sizeof(cfg->auth_url), "https://coordinator.krunk.io"); // coord-auth contract
    cfg->enforce_skins = -1;            // auto: on when linked to a coordinator
    cfg->shutdown_grace = 10.0f;
    cfg->lag_comp = 1;
    cfg->lag_comp_anchor = LAG_COMP_ANCHOR_EXACT;
    cfg->lag_comp_scale = 1.0f;         // x each player's lagComp (ENT slider): 1 = v3.1.1
    gs_copy(cfg->bot_difficulty, sizeof(cfg->bot_difficulty), "normal"); // bots / bot_quota: 0 = no bots
    cfg->bot_quota_grace = 10.0f; // bot_quota: an empty lobby keeps its bots this long
    cfg->bot_chat = 0;            // off by default: no gg, no kill comments, no AI calls
                                  // 1 = bots say gg / comment kills (templates, or kn_brain's AI with bot_chat_ai)
    cfg->bot_chat_ai = 1;
}

// ------------------------------------------------------------------ file location

static int gs_file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void gs_set_root_from(const char *config_path) {
    // root = parent of the directory holding config.json ("a/b/config/config.json" -> "a/b")
    char tmp[GS_STR_LARGE];
    gs_copy(tmp, sizeof(tmp), config_path);
    char *slash = strrchr(tmp, '/');
    if (!slash) { g_config_root[0] = 0; return; }
    *slash = 0;
    slash = strrchr(tmp, '/');
    if (!slash) { gs_copy(g_config_root, sizeof(g_config_root), "."); return; }
    *slash = 0;
    gs_copy(g_config_root, sizeof(g_config_root), tmp[0] ? tmp : "/");
}

static void gs_resolve_path(char *out, const size_t size, const char *path) {
    if (!path || !path[0]) { out[0] = 0; return; }
    if (path[0] == '/' || !g_config_root[0]) { gs_copy(out, size, path); return; }
    snprintf(out, size, "%s/%s", g_config_root, path);
}

static int gs_find_config(char *out, const size_t size) {
    const char *env = getenv("KN_CONFIG");
    if (env && env[0]) {
        gs_copy(out, size, env);
        return 2; // explicit
    }

    char candidate[GS_STR_LARGE];
    gs_copy(candidate, sizeof(candidate), GS_CONFIG_DEFAULT_PATH);
    for (int up = 0; up <= 4; up++) {
        if (gs_file_exists(candidate)) {
            gs_copy(out, size, candidate);
            return 1;
        }
        char next[GS_STR_LARGE];
        snprintf(next, sizeof(next), "../%s", candidate);
        gs_copy(candidate, sizeof(candidate), next);
    }
    return 0;
}

// ------------------------------------------------------------------ JSON helpers

static const cJSON *gs_item(const cJSON *obj, const char *key) {
    return obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
}

static void gs_read_str(const cJSON *obj, const char *key, char *dst, const size_t size) {
    const cJSON *item = gs_item(obj, key);
    if (cJSON_IsString(item)) gs_copy(dst, size, item->valuestring);
    else if (cJSON_IsNumber(item)) snprintf(dst, size, "%g", item->valuedouble);
}

static int gs_read_num(const cJSON *obj, const char *key, double *out) {
    const cJSON *item = gs_item(obj, key);
    if (cJSON_IsNumber(item)) { *out = item->valuedouble; return 1; }
    if (cJSON_IsBool(item)) { *out = cJSON_IsTrue(item) ? 1.0 : 0.0; return 1; }
    if (cJSON_IsString(item) && item->valuestring[0]) {
        char *end = NULL;
        const double v = strtod(item->valuestring, &end);
        if (end && !*end) { *out = v; return 1; }
    }
    return 0;
}

static void gs_read_int(const cJSON *obj, const char *key, int *dst, const int min, const int max) {
    double v;
    if (!gs_read_num(obj, key, &v)) return;
    if (v < min) v = min;
    if (v > max) v = max;
    *dst = (int) v;
}

static void gs_read_float(const cJSON *obj, const char *key, float *dst, const float min, const float max) {
    double v;
    if (!gs_read_num(obj, key, &v)) return;
    if (v < min) v = min;
    if (v > max) v = max;
    *dst = (float) v;
}

static void gs_read_bool(const cJSON *obj, const char *key, int *dst) {
    const cJSON *item = gs_item(obj, key);
    if (cJSON_IsBool(item)) *dst = cJSON_IsTrue(item);
    else if (cJSON_IsNumber(item)) *dst = item->valuedouble != 0.0;
    else if (cJSON_IsString(item)) {
        const char *s = item->valuestring;
        *dst = !strcmp(s, "1") || !strcasecmp(s, "true") || !strcasecmp(s, "yes") || !strcasecmp(s, "on");
    }
}

static void gs_read_port(const cJSON *obj, const char *key, unsigned short *dst) {
    int port = -1;
    gs_read_int(obj, key, &port, 0, 65535);
    if (port >= 0) *dst = (unsigned short) port;
}

// {"tdm": 12, "ctf": 16} -> table[mode id]
static void gs_read_mode_table_int(const cJSON *obj, const char *key, int *table, const int max) {
    const cJSON *item = gs_item(obj, key);
    const cJSON *entry;
    cJSON_ArrayForEach(entry, item) {
        const int id = mode_find(entry->string);
        if (id < 0) {
            server_log_error("config: gameserver.%s: unknown mode '%s'", key, entry->string);
            continue;
        }
        if (cJSON_IsNumber(entry)) table[id] = entry->valueint < 0 ? 0 : entry->valueint > max ? max : entry->valueint;
    }
}

static void gs_read_mode_table_float(const cJSON *obj, const char *key, float *table) {
    const cJSON *item = gs_item(obj, key);
    const cJSON *entry;
    cJSON_ArrayForEach(entry, item) {
        const int id = mode_find(entry->string);
        if (id < 0) {
            server_log_error("config: gameserver.%s: unknown mode '%s'", key, entry->string);
            continue;
        }
        if (cJSON_IsNumber(entry)) table[id] = entry->valuedouble < 0.0 ? 0.0f : (float) entry->valuedouble;
    }
}

// string or array of strings -> rows
static int gs_read_lines(const cJSON *obj, const char *key, char *rows, const size_t row_size, const int max) {
    const cJSON *item = gs_item(obj, key);
    if (!item) return -1;
    int count = 0;
    if (cJSON_IsString(item)) {
        if (item->valuestring[0]) gs_copy(rows, row_size, item->valuestring), count = 1;
        return count;
    }
    const cJSON *entry;
    cJSON_ArrayForEach(entry, item) {
        if (count >= max) break;
        if (cJSON_IsString(entry)) gs_copy(rows + (size_t) count++ * row_size, row_size, entry->valuestring);
        else if (cJSON_IsNumber(entry)) snprintf(rows + (size_t) count++ * row_size, row_size, "%d", entry->valueint);
    }
    return count;
}

// "#rgb" / "#rrggbb" (CSS hex, the v3.1.1 colour inputs); anything else keeps `def`.
static void gs_read_color(const cJSON *obj, const char *key, char *dst, const char *def) {
    const cJSON *item = gs_item(obj, key);
    const char *s = cJSON_IsString(item) ? item->valuestring : NULL;
    const size_t len = s ? strlen(s) : 0;
    int ok = s && s[0] == '#' && (len == 4 || len == 7);
    for (size_t i = 1; ok && i < len; i++) ok = isxdigit((unsigned char) s[i]) != 0;
    if (!ok) {
        if (s) server_log_error("config: billboard %s '%s' is not #rrggbb, using %s", key, s, def);
        gs_copy(dst, 8, def);
        return;
    }
    if (len == 4) snprintf(dst, 8, "#%c%c%c%c%c%c", s[1], s[1], s[2], s[2], s[3], s[3]);
    else gs_copy(dst, 8, s);
}

// An empty billboard ("" or an entry without text and image) is no billboard: the maps keep
// their own posters, as in v7.1.4.
static int gs_set_billboard_image(GsBillboard *bb, const char *image) {
    if (!image || !image[0]) return 0;
    if (!billboard_image_path_ok(image)) {
        server_log_error("config: billboard image '%s' is not an asset path (.png/.jpg under assets/, no URL), ignored", image);
        return 0;
    }
    gs_copy(bb->image, sizeof(bb->image), image);
    return 1;
}

static int gs_add_billboard(GsConfig *cfg, const cJSON *entry) {
    if (cfg->billboard_count >= GS_MAX_LIST) return 0;
    GsBillboard *bb = &cfg->billboards[cfg->billboard_count];
    memset(bb, 0, sizeof(*bb));
    gs_copy(bb->txt_col, sizeof(bb->txt_col), "#e3e3e3"); // game.updateBillboards defaults
    gs_copy(bb->bg_col, sizeof(bb->bg_col), "#000000");

    if (cJSON_IsString(entry)) {
        gs_copy(bb->txt, sizeof(bb->txt), entry->valuestring);
    } else if (cJSON_IsObject(entry)) {
        gs_read_str(entry, "txt", bb->txt, sizeof(bb->txt));
        if (!bb->txt[0]) gs_read_str(entry, "text", bb->txt, sizeof(bb->txt));
        gs_read_color(entry, "txtCol", bb->txt_col, "#e3e3e3");
        gs_read_color(entry, "bgCol", bb->bg_col, "#000000");
        const cJSON *image = gs_item(entry, "image");
        if (cJSON_IsString(image)) gs_set_billboard_image(bb, image->valuestring);
    } else {
        return 0;
    }

    if (!bb->txt[0] && !bb->image[0]) return 0; // unset
    cfg->billboard_count++;
    return 1;
}

// v3.1.1 billboards.json: an array of {txt, txtCol, bgCol}.
static void gs_load_billboards_file(GsConfig *cfg, const char *path) {
    char full[GS_STR_LARGE];
    gs_resolve_path(full, sizeof(full), path);
    cJSON *json = NULL;
    if (load_json_file(full, &json) || !cJSON_IsArray(json)) {
        server_log_error("config: billboards_file %s is missing or not a JSON array", full);
        cJSON_Delete(json);
        return;
    }
    const cJSON *entry;
    cJSON_ArrayForEach(entry, json) gs_add_billboard(cfg, entry);
    cJSON_Delete(json);
}

// ------------------------------------------------------------------ game.<varN> (serverConfig)

// gameserver.game / server.cfg "game.<varN>": the v7.1.4 serverConfig keys (WP-E,
// shared/src/game_config.c), only the ones present, clamped like a dedicated server's
// applyConfig (maxD). server.cfg values arrive as text ("game.canSlide 0"): each is typed by its
// option first, with this file's number / bool spellings.
#define GS_GAME_MAX_PLAYERS 31 // the lobby cap (gameserver.max_players), below v7's maxD 50

// The serverConfig option of a key in any case: server.cfg lowercases its keys (game.gametime).
static const GameConfigOption *gs_game_option(const char *key) {
    size_t count = 0;
    const GameConfigOption *options = game_config_options(GAME_CONFIG_SERVER, &count);
    for (size_t i = 0; key && i < count; i++) {
        if (!strcasecmp(options[i].var, key)) return &options[i];
    }
    return NULL;
}

// The "game" object with each known key under its varN and of its option's type.
static cJSON *gs_game_typed(const cJSON *obj) {
    cJSON *typed = cJSON_CreateObject();
    const cJSON *item;
    cJSON_ArrayForEach(item, (cJSON_IsObject(obj) ? obj : NULL)) {
        if (!typed) break;
        // the Host Game request's class / weapon / effect overrides (game_config_setup reads them from
        // the game request, as the sandbox and a hosted game get them); kept as they are
        if (!strcmp(item->string, "classes") || !strcmp(item->string, "weps") || !strcmp(item->string, "cls") || !strcmp(item->string, "prks")) {
            if (cJSON_IsArray(item) || cJSON_IsObject(item)) cJSON_AddItemToObject(typed, item->string, cJSON_Duplicate(item, 1));
            continue;
        }
        // v10 game.mimicPubs: a hosted game's request (the operator's preset, or a Host Game request)
        // may make it a pub-type game (Turf Wars, pub progress)
        if (!strcmp(item->string, "mimicPubs")) {
            if (cJSON_IsBool(item)) cJSON_AddBoolToObject(typed, "mimicPubs", cJSON_IsTrue(item));
            continue;
        }
        const GameConfigOption *o = gs_game_option(item->string);
        if (!o) continue;
        cJSON_DeleteItemFromObjectCaseSensitive(typed, o->var);
        if (o->kind == GC_INPUT) {
            if (cJSON_IsString(item)) cJSON_AddStringToObject(typed, o->var, item->valuestring);
        } else if (o->kind == GC_BOOL) {
            int on = -1;
            gs_read_bool(obj, item->string, &on);
            if (on >= 0) cJSON_AddBoolToObject(typed, o->var, on);
        } else {
            double v;
            if (gs_read_num(obj, item->string, &v)) cJSON_AddNumberToObject(typed, o->var, v);
        }
    }
    return typed;
}

void gs_game_config_from_json(GameConfig *g, const cJSON *obj) {
    if (!g || !cJSON_IsObject(obj)) return;
    cJSON *typed = gs_game_typed(obj);
    if (!typed) return;
    game_config_merge(g, typed, GAME_CONFIG_CLAMP_DEDICATED);
    cJSON_Delete(typed);
    if (g->max_players > GS_GAME_MAX_PLAYERS) g->max_players = GS_GAME_MAX_PLAYERS;
    if (g->min_players > GS_GAME_MAX_PLAYERS) g->min_players = GS_GAME_MAX_PLAYERS;
}

// ------------------------------------------------------------------ section parser

int gs_config_from_json(GsConfig *cfg, const cJSON *s, char *err, const size_t err_size) {
    if (!s) return 0;
    if (!cJSON_IsObject(s)) {
        snprintf(err, err_size, "\"gameserver\" must be an object");
        return -1;
    }

    gs_read_str(s, "region", cfg->region, sizeof(cfg->region));
    // the version toggle's key: Krunker v10.0.3 is the only version now (shared/include/ruleset.h)
    if (cJSON_GetObjectItem(s, "ruleset")) server_log_error("config: gameserver.ruleset is ignored: the server plays Krunker v10.0.3, the only version");
    // product flavor (flavor.h): "base" (default) or "strike"; --flavor overrides it (server/src/main.c)
    gs_read_str(s, "flavor", cfg->flavor, sizeof(cfg->flavor));
    if (cfg->flavor[0] && flavor_from_key(cfg->flavor, FLAVOR_COUNT) == FLAVOR_COUNT) {
        server_log_error("config: gameserver.flavor '%s' is unknown (base|strike): ignored", cfg->flavor);
        cfg->flavor[0] = 0;
    }
    // The flavor this config runs as (main.c applies gameserver.flavor after loading unless --flavor
    // was given). When it is not the one gs_config_defaults() saw, its lobby defaults go in now,
    // under the keys read below.
    const GameFlavor flavor = !g_flavor_locked && cfg->flavor[0] ? flavor_from_key(cfg->flavor, flavor_get()) : flavor_get();
    if (flavor != flavor_get() && cJSON_GetObjectItem(s, "flavor")) {
        const StrikeRules *rules = flavor_config(flavor)->rules;
        cfg->max_players = rules && rules->max_players > 0 ? rules->max_players : 8;
        cfg->game = rules ? *strike_game_config() : g_default_game_config;
    }
    gs_read_str(s, "node_id", cfg->node_id, sizeof(cfg->node_id));
    gs_read_str(s, "name", cfg->name, sizeof(cfg->name));
    gs_read_str(s, "listen_host", cfg->listen_host, sizeof(cfg->listen_host));
    gs_read_port(s, "port", &cfg->listen_port);
    gs_read_port(s, "listen_port", &cfg->listen_port);
    gs_read_str(s, "public_host", cfg->public_host, sizeof(cfg->public_host));
    gs_read_port(s, "public_port", &cfg->public_port);
    gs_read_str(s, "brain_url", cfg->brain_url, sizeof(cfg->brain_url));
    gs_read_str(s, "brain_secret", cfg->brain_secret, sizeof(cfg->brain_secret));
    gs_read_str(s, "join_secret", cfg->join_secret, sizeof(cfg->join_secret));
    gs_read_str(s, "director_secret", cfg->director_secret, sizeof(cfg->director_secret)); // Krunker Strike director (director_gate.h)
    gs_read_str(s, "web_url", cfg->web_url, sizeof(cfg->web_url));
    gs_read_str(s, "brain_http_url", cfg->brain_http_url, sizeof(cfg->brain_http_url));
    {
        int tick = cfg->tick_rate;
        gs_read_int(s, "tick_rate", &tick, 1, 240);
        cfg->tick_rate = (unsigned short) tick;
    }

    // lobby
    gs_read_int(s, "max_players", &cfg->max_players, 1, 31);
    gs_read_mode_table_int(s, "mode_max_players", cfg->mode_max_players, 31);
    gs_read_int(s, "team_size", &cfg->team_size, 0, 15);
    gs_read_mode_table_int(s, "mode_team_size", cfg->mode_team_size, 15);

    const int maps = gs_read_lines(s, "maps", &cfg->maps[0][0], sizeof(cfg->maps[0]), GS_MAX_LIST);
    if (maps >= 0) cfg->map_count = maps;

    // modes: names / aliases / ids; "rotation" (alone, as server.cfg's `modes rotation` list, or
    // among others) adds Krunker's public rotation (map.js rotationModes). Duplicates count once.
    if (gs_item(s, "modes")) {
        cfg->mode_count = 0;
        char names[GS_MAX_LIST][GS_STR_SMALL];
        const int n = gs_read_lines(s, "modes", &names[0][0], sizeof(names[0]), GS_MAX_LIST);
        const int *rotation_modes = NULL; // the flavor's public rotation (Krunker Strike: its offered modes)
        const size_t rotation_count = flavor_rotation_modes(flavor, &rotation_modes);
        for (int i = 0; i < n; i++) {
            const int rotation = !strcasecmp(names[i], "rotation");
            const int id = rotation ? -1 : mode_find(names[i]);
            if (!rotation && id < 0) {
                snprintf(err, err_size, "gameserver.modes: unknown mode '%s'", names[i]);
                return -1;
            }
            if (!rotation && flavor_config(flavor)->rules && !strike_mode(flavor_config(flavor)->rules, id)) {
                snprintf(err, err_size, "gameserver.modes: '%s' is not a %s mode", names[i], flavor_display_name(flavor));
                return -1;
            }
            const size_t add = rotation ? rotation_count : 1;
            for (size_t r = 0; r < add; r++) {
                const int mode_id = rotation ? rotation_modes[r] : id;
                int dup = 0;
                for (int k = 0; k < cfg->mode_count; k++) dup |= cfg->modes[k] == mode_id;
                if (!dup && cfg->mode_count < GS_MAX_LIST) cfg->modes[cfg->mode_count++] = mode_id;
            }
        }
    }

    const int dirs = gs_read_lines(s, "custom_map_dirs", &cfg->custom_map_dirs[0][0], sizeof(cfg->custom_map_dirs[0]), 8);
    if (dirs >= 0) cfg->custom_map_dir_count = dirs;
    gs_read_float(s, "round_seconds", &cfg->round_seconds, 0.0f, 86400.0f);
    gs_read_mode_table_float(s, "mode_round_seconds", cfg->mode_round_seconds);

    const cJSON *game = gs_item(s, "game");
    if (cJSON_IsObject(game)) {
        gs_game_config_from_json(&cfg->game, game);
        cfg->has_game = 1;
        cJSON *typed = gs_game_typed(game);
        char *text = typed ? cJSON_PrintUnformatted(typed) : NULL;
        gs_copy(cfg->game_request, sizeof(cfg->game_request), text && strlen(text) < sizeof(cfg->game_request) ? text : "");
        cJSON_free(text);
        cJSON_Delete(typed);
        // config.maxPlayers is the lobby size of a hosted game, keep them in sync
        if (gs_item(game, "maxPlayers") && !gs_item(s, "max_players")) cfg->max_players = cfg->game.max_players;
    }
    gs_read_bool(s, "custom_games", &cfg->custom_games);
    gs_read_int(s, "force_event", &cfg->force_event, 0, 1000000); // v10 forceEvent (live_events_server.c)
    // WP-E server presets: host_type by index or name ("comp", "zombies", "raid", ...)
    const cJSON *host_type = gs_item(s, "host_type");
    if (cJSON_IsString(host_type) && host_type->valuestring[0] && !isdigit((unsigned char) host_type->valuestring[0])) {
        static const char *const names[GAME_HOST_TYPE_COUNT] = {"custom", "comp", "rent", "zombies", "raid"};
        cfg->host_type = -2;
        for (int i = 0; i < GAME_HOST_TYPE_COUNT; i++) if (!strcasecmp(host_type->valuestring, names[i])) cfg->host_type = i;
        if (cfg->host_type == -2) {
            snprintf(err, err_size, "gameserver.host_type: unknown host type '%s' (custom, comp, rent, zombies, raid)", host_type->valuestring);
            return -1;
        }
    } else {
        gs_read_int(s, "host_type", &cfg->host_type, -1, GAME_HOST_TYPE_COUNT - 1);
    }
    gs_read_bool(s, "map_config", &cfg->map_config);

    // messages
    const int motd = gs_read_lines(s, "motd", &cfg->motd[0][0], sizeof(cfg->motd[0]), 8);
    if (motd >= 0) cfg->motd_count = motd;
    gs_read_str(s, "join_message", cfg->join_message, sizeof(cfg->join_message));
    gs_read_str(s, "leave_message", cfg->leave_message, sizeof(cfg->leave_message));

    const cJSON *billboard = gs_item(s, "billboard");
    const cJSON *billboards = gs_item(s, "billboards");
    const cJSON *billboards_file = gs_item(s, "billboards_file");
    if (billboard || billboards || billboards_file) cfg->billboard_count = 0;
    if (billboard) gs_add_billboard(cfg, billboard);
    const cJSON *entry;
    cJSON_ArrayForEach(entry, billboards) gs_add_billboard(cfg, entry);
    if (cJSON_IsString(billboards_file) && billboards_file->valuestring[0]) gs_load_billboards_file(cfg, billboards_file->valuestring);
    // billboard_image: the image of every entry that has none, or the only billboard
    const cJSON *billboard_image = gs_item(s, "billboard_image");
    if (cJSON_IsString(billboard_image) && billboard_image->valuestring[0]) {
        GsBillboard probe;
        memset(&probe, 0, sizeof(probe));
        if (gs_set_billboard_image(&probe, billboard_image->valuestring)) {
            for (int i = 0; i < cfg->billboard_count; i++)
                if (!cfg->billboards[i].image[0]) gs_copy(cfg->billboards[i].image, sizeof(cfg->billboards[i].image), probe.image);
            if (!cfg->billboard_count) {
                cJSON *only = cJSON_CreateObject();
                cJSON_AddStringToObject(only, "image", probe.image);
                gs_add_billboard(cfg, only);
                cJSON_Delete(only);
            }
        }
    }

    // moderation
    gs_read_float(s, "idle_kick_seconds", &cfg->idle_timeout, 0.0f, 86400.0f);
    gs_read_float(s, "idle_menu_kick_seconds", &cfg->idle_menu_timeout, 0.0f, 86400.0f);
    gs_read_float(s, "idle_move_threshold", &cfg->idle_move_threshold, 0.0f, 1000.0f);
    gs_read_bool(s, "perms", &cfg->perms);
    {
        char cheats[16] = "";
        gs_read_str(s, "cheats", cheats, sizeof(cheats)); // "auth" (default) | "all"
        if (cheats[0]) cfg->cheats_open = !strcasecmp(cheats, "all");
    }
    const int ops = gs_read_lines(s, "ops", &cfg->ops[0][0], sizeof(cfg->ops[0]), GS_MAX_LIST);
    if (ops >= 0) cfg->op_count = ops;
    {
        char path[GS_STR_LARGE] = "";
        gs_read_str(s, "bans_file", path, sizeof(path));
        if (path[0]) gs_resolve_path(cfg->bans_file, sizeof(cfg->bans_file), path);
        else if (gs_item(s, "bans_file")) cfg->bans_file[0] = 0;
    }
    gs_read_bool(s, "allow_guests", &cfg->allow_guests);
    gs_read_bool(s, "require_ticket", &cfg->require_ticket);
    gs_read_bool(s, "director_require_ticket", &cfg->director_require_ticket);
    gs_read_float(s, "join_timeout", &cfg->join_timeout, 0.5f, 120.0f);

    // rewards
    gs_read_bool(s, "earn_kr", &cfg->earn_kr);
    gs_read_int(s, "kr_cap", &cfg->kr_cap, 0, 1000000);
    gs_read_int(s, "kr_cap_premium", &cfg->kr_cap_premium, 0, 1000000);
    gs_read_float(s, "kr_multiplier", &cfg->kr_multiplier, 0.0f, 1000.0f);

    // dedicated server (server.cfg keys, doc/backend.md 8.7)
    gs_read_str(s, "password", cfg->password, sizeof(cfg->password));
    gs_read_str(s, "rcon_password", cfg->rcon_password, sizeof(cfg->rcon_password));
    gs_read_str(s, "rcon_host", cfg->rcon_host, sizeof(cfg->rcon_host));
    gs_read_port(s, "rcon_port", &cfg->rcon_port);
    gs_read_port(s, "query_port", &cfg->query_port);
    gs_read_bool(s, "online_mode", &cfg->online_mode);
    gs_read_str(s, "auth_url", cfg->auth_url, sizeof(cfg->auth_url));
    gs_read_str(s, "finder_url", cfg->finder_url, sizeof(cfg->finder_url));
    gs_read_str(s, "community", cfg->community, sizeof(cfg->community));
    {
        char tags[GS_MAX_LIST][GS_STR_SMALL];
        const int n = gs_read_lines(s, "tags", &tags[0][0], sizeof(tags[0]), 16);
        if (n >= 0) {
            cfg->tags[0] = 0;
            for (int i = 0; i < n; i++) {
                const size_t used = strlen(cfg->tags);
                snprintf(cfg->tags + used, sizeof(cfg->tags) - used, "%s%s", used ? "," : "", tags[i]);
            }
        }
    }
    {
        const cJSON *item = gs_item(s, "enforce_skins");
        if (cJSON_IsString(item) && !strcasecmp(item->valuestring, "auto")) cfg->enforce_skins = -1;
        else if (item) gs_read_bool(s, "enforce_skins", &cfg->enforce_skins);
    }
    gs_read_float(s, "shutdown_grace", &cfg->shutdown_grace, 0.0f, 3600.0f);
    gs_read_str(s, "shutdown_reason_file", cfg->shutdown_reason_file, sizeof(cfg->shutdown_reason_file));
    gs_read_bool(s, "lag_comp", &cfg->lag_comp);
    {
        const cJSON *item = gs_item(s, "lag_comp_anchor"); // true / false (older configs), 0 / 1 / 2, or a mode name
        int anchor = -1;
        if (cJSON_IsBool(item)) anchor = cJSON_IsTrue(item) ? LAG_COMP_ANCHOR_EXACT : LAG_COMP_ANCHOR_V3;
        else if (cJSON_IsNumber(item)) anchor = item->valueint >= 0 && item->valueint <= 2 ? item->valueint : -1;
        else if (cJSON_IsString(item)) anchor = lag_comp_anchor_parse(item->valuestring);
        if (anchor >= 0) cfg->lag_comp_anchor = anchor;
        else if (item) server_log_error("config: gameserver.lag_comp_anchor: expected 0 (v3.1.1), 1 (exact) or 2 (arrival)");
    }
    gs_read_float(s, "lag_comp_scale", &cfg->lag_comp_scale, 0.1f, 1.0f); // the range of the per-player lagComp
    gs_read_int(s, "bots", &cfg->bots, 0, 16);            // config.botLimit
    gs_read_int(s, "bot_quota", &cfg->bot_quota, 0, 16);
    gs_read_float(s, "bot_quota_grace", &cfg->bot_quota_grace, 0.0f, 600.0f);
    gs_read_bool(s, "bot_chat", &cfg->bot_chat);
    gs_read_bool(s, "bot_chat_ai", &cfg->bot_chat_ai);
    {
        const cJSON *item = gs_item(s, "bot_difficulty"); // "hard" or 1.8
        if (cJSON_IsNumber(item)) snprintf(cfg->bot_difficulty, sizeof(cfg->bot_difficulty), "%g", item->valuedouble);
        else gs_read_str(s, "bot_difficulty", cfg->bot_difficulty, sizeof(cfg->bot_difficulty));
    }
    {
        char path[GS_STR_LARGE] = "";
        gs_read_str(s, "log_file", path, sizeof(path));
        if (path[0]) gs_copy(cfg->log_file, sizeof(cfg->log_file), path);
    }
    return 0;
}

// ------------------------------------------------------------------ env overrides

// JSON when it looks like JSON (knsvc/config.h rule), else a string.
static cJSON *gs_env_value(const char *text) {
    if (!text) return NULL;
    const char c = text[0];
    if (c == '{' || c == '[' || c == '"' || c == '-' || isdigit((unsigned char) c) || !strcmp(text, "true") || !strcmp(text, "false") ||
        !strcmp(text, "null")) {
        // Whole value only: plain cJSON_Parse accepts a prefix ("2.56.222.45" -> 2.56).
        cJSON *v = cJSON_ParseWithOpts(text, NULL, 1);
        if (v) return v;
    }
    return cJSON_CreateString(text);
}

static void gs_override(cJSON *section, const char *key, const char *value) {
    cJSON *v = gs_env_value(value);
    if (!v) return;
    if (cJSON_GetObjectItemCaseSensitive(section, key)) cJSON_ReplaceItemInObjectCaseSensitive(section, key, v);
    else cJSON_AddItemToObject(section, key, v);
}

extern char **environ;

static int gs_env_to_json(cJSON *section) {
    int count = 0;

    // v3.1.1 main/constants.js names (lower priority than KN_GAMESERVER_*)
    static const char *const aliases[][2] = {
        {"MM_BRAIN_URL", "brain_url"},     {"MM_BRAIN_SECRET", "brain_secret"}, {"MM_NODE_REGION", "region"},
        {"MM_NODE_ID", "node_id"},         {"PUBLIC_DOMAIN", "public_host"},    {"PUBLIC_PORT", "public_port"},
        {"PERMS", "perms"},
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        const char *value = getenv(aliases[i][0]);
        if (!value || !value[0]) continue;
        gs_override(section, aliases[i][1], value);
        count++;
    }

    static const char prefix[] = "KN_GAMESERVER_";
    for (char **env = environ; env && *env; env++) {
        if (strncmp(*env, prefix, sizeof(prefix) - 1)) continue;
        const char *eq = strchr(*env, '=');
        if (!eq) continue;
        char key[GS_STR_SMALL];
        size_t n = 0;
        for (const char *p = *env + sizeof(prefix) - 1; p < eq && n + 1 < sizeof(key); p++) key[n++] = (char) tolower((unsigned char) *p);
        key[n] = 0;
        if (!n || !strcmp(key, "index") || !strcmp(key, "brain_url_fallback")) continue; // gs_apply_index / gs_resolve_brain_url
        gs_override(section, key, eq + 1);
        count++;
    }
    return count;
}

// KN_GAMESERVER_INDEX=i (services/scripts/backend-up.sh starts N servers from one config):
// server i listens on port + i (public_port + i when set), its node id gets the index
// appended ("LOC" -> "LOC0", "LOC1", ...) and its name " #<i+1>".
static void gs_apply_index(GsConfig *cfg) {
    const char *env = getenv("KN_GAMESERVER_INDEX");
    char *end = NULL;
    const long index = env && env[0] ? strtol(env, &end, 10) : -1;
    if (index < 0 || index > 999 || !end || *end) return;
    cfg->listen_port = (unsigned short) (cfg->listen_port + index);
    if (cfg->public_port) cfg->public_port = (unsigned short) (cfg->public_port + index);
    char buf[GS_STR_SMALL + 8];
    snprintf(buf, sizeof(buf), "%s%ld", cfg->node_id, index);
    snprintf(cfg->node_id, sizeof(cfg->node_id), "%s", buf);
    snprintf(buf, sizeof(buf), "%s #%ld", cfg->name, index + 1);
    snprintf(cfg->name, sizeof(cfg->name), "%s", buf);
}

// brain_url "auto" = this config's own brain: ws://<brain.host>:<brain.port>/mm/internal/ws
// (defaults 127.0.0.1:5050; a 0.0.0.0 bind is dialled on 127.0.0.1).
// KN_GAMESERVER_BRAIN_URL_FALLBACK (set by services/scripts/backend-up.sh) applies only
// when no brain_url came from the file, the environment or the command line.
static void gs_resolve_brain_url(GsConfig *cfg, const cJSON *brain) {
    const char *fallback = getenv("KN_GAMESERVER_BRAIN_URL_FALLBACK");
    if (!cfg->brain_url[0] && fallback && fallback[0]) gs_copy(cfg->brain_url, sizeof(cfg->brain_url), fallback);
    if (strcmp(cfg->brain_url, "auto")) return;

    char host[GS_STR_SMALL] = "127.0.0.1";
    int port = 5050;
    gs_read_str(brain, "host", host, sizeof(host));
    gs_read_int(brain, "port", &port, 1, 65535);
    const char *env_host = getenv("KN_BRAIN_HOST");
    const char *env_port = getenv("KN_BRAIN_PORT");
    if (env_host && env_host[0]) gs_copy(host, sizeof(host), env_host);
    if (env_port && atoi(env_port) > 0) port = atoi(env_port);
    if (!strcmp(host, "0.0.0.0") || !host[0]) gs_copy(host, sizeof(host), "127.0.0.1");
    snprintf(cfg->brain_url, sizeof(cfg->brain_url), "ws://%s:%d/mm/internal/ws", host, port);
}

// ------------------------------------------------------------------ server.cfg
//
// CS:S-style dedicated server config: one "key value" per line, // or # comments, values
// optionally "quoted" (\" and \\ escapes). Keys are the gameserver JSON keys or the classic
// aliases below; "game.<var> value" sets a host config var. List keys (maps, modes, ops,
// tags) take "a,b,c" or repeated lines; motd / billboard lines repeat. Values parse as
// JSON when they look like JSON (numbers, true/false, [..], {..}), else as text.

static const char *const k_cfg_aliases[][2] = {
    {"hostname", "name"},           {"sv_password", "password"},        {"maxplayers", "max_players"},
    {"sv_maxplayers", "max_players"}, {"mapcycle", "maps"},             {"map", "maps"},
    {"mode", "modes"},              {"sv_region", "region"},            {"game_version", "ruleset"},            {"region_id", "region"},
    {"ip", "listen_host"},          {"public_ip", "public_host"},       {"rcon_ip", "rcon_host"},
    {"coordinator_url", "brain_url"}, {"coordinator", "brain_url"},     {"coordinator_secret", "brain_secret"},
    {"coordinator_http_url", "brain_http_url"}, {"sv_tags", "tags"},   {"online-mode", "online_mode"},
    {"sv_lan", "lan"},              {"tickrate", "tick_rate"},          {"sv_tickrate", "tick_rate"},
    {"logfile", "log_file"},        {"log", "log_file"},
};

static int gs_cfg_is_list(const char *key, int *split_commas) {
    static const char *const split[] = {"maps", "modes", "ops", "tags", "custom_map_dirs"};
    static const char *const repeat[] = {"motd", "billboards"};
    for (size_t i = 0; i < sizeof(split) / sizeof(split[0]); i++)
        if (!strcmp(key, split[i])) return *split_commas = 1, 1;
    for (size_t i = 0; i < sizeof(repeat) / sizeof(repeat[0]); i++)
        if (!strcmp(key, repeat[i])) return *split_commas = 0, 1;
    return 0;
}

// Reads one token (quoted or bare) at *p; returns 0 at end of line / comment.
static int gs_cfg_token(const char **p, char *out, const size_t size, const int rest_of_line) {
    const char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || *s == '#' || (s[0] == '/' && s[1] == '/')) return 0;
    size_t n = 0;
    if (*s == '"') {
        for (s++; *s && *s != '"'; s++) {
            if (*s == '\\' && (s[1] == '"' || s[1] == '\\')) s++;
            if (n + 1 < size) out[n++] = *s;
        }
        if (*s == '"') s++;
    } else {
        // the value is the rest of the line (minus a trailing comment) so "say hi there" works
        // a comment starts with # or // after whitespace (so ws://host/path values survive)
        while (*s && !(rest_of_line ? ((*s == '#' || (s[0] == '/' && s[1] == '/')) && n && (out[n - 1] == ' ' || out[n - 1] == '\t'))
                                    : (*s == ' ' || *s == '\t'))) {
            if (n + 1 < size) out[n++] = *s;
            s++;
        }
        while (rest_of_line && n && (out[n - 1] == ' ' || out[n - 1] == '\t')) n--;
    }
    out[n] = 0;
    *p = s;
    return 1;
}

static void gs_cfg_put(cJSON *section, const char *key, const char *value) {
    int split = 0;
    if (!strncmp(key, "game.", 5) && key[5]) {
        cJSON *game = cJSON_GetObjectItemCaseSensitive(section, "game");
        if (!cJSON_IsObject(game)) {
            cJSON_DeleteItemFromObjectCaseSensitive(section, "game");
            game = cJSON_AddObjectToObject(section, "game");
        }
        gs_override(game, key + 5, value);
        return;
    }
    if (!strcmp(key, "lan")) { // sv_lan 1: not listed, guests only
        if (strcmp(value, "1") && strcasecmp(value, "true")) return;
        gs_override(section, "finder_url", "\"\"");
        gs_override(section, "online_mode", "false");
        return;
    }
    if (!gs_cfg_is_list(key, &split) || value[0] == '[') {
        gs_override(section, key, value);
        return;
    }
    cJSON *list = cJSON_GetObjectItemCaseSensitive(section, key);
    if (!cJSON_IsArray(list)) {
        cJSON_DeleteItemFromObjectCaseSensitive(section, key);
        list = cJSON_AddArrayToObject(section, key);
    }
    if (!split) {
        cJSON_AddItemToArray(list, cJSON_CreateString(value));
        return;
    }
    char item[GS_STR_LARGE];
    for (const char *p = value; *p;) {
        const size_t len = strcspn(p, ",");
        snprintf(item, sizeof(item), "%.*s", (int) len, p);
        char *a = item, *b = item + strlen(item);
        while (*a == ' ') a++;
        while (b > a && b[-1] == ' ') *--b = 0;
        if (*a) cJSON_AddItemToArray(list, cJSON_CreateString(a));
        p += len + (p[len] == ',');
    }
}

cJSON *gs_cfg_parse(const char *text, char *err, const size_t err_size) {
    cJSON *section = cJSON_CreateObject();
    int line_no = 0;
    // list keys given in the file replace (not extend) the JSON config's lists
    while (section && text && *text) {
        line_no++;
        const size_t len = strcspn(text, "\r\n");
        char line[1024];
        snprintf(line, sizeof(line), "%.*s", (int) (len < sizeof(line) - 1 ? len : sizeof(line) - 1), text);
        text += len;
        while (*text == '\r' || *text == '\n') {
            if (*text == '\n') { text++; break; }
            text++;
        }
        const char *p = line;
        char key[GS_STR_SMALL], value[GS_STR_LARGE * 2] = "";
        if (!gs_cfg_token(&p, key, sizeof(key), 0)) continue;
        if (!gs_cfg_token(&p, value, sizeof(value), 1)) {
            snprintf(err, err_size, "line %d: '%s' needs a value", line_no, key);
            cJSON_Delete(section);
            return NULL;
        }
        for (char *k = key; *k; k++) *k = (char) tolower((unsigned char) *k);
        const char *mapped = key;
        for (size_t i = 0; i < sizeof(k_cfg_aliases) / sizeof(k_cfg_aliases[0]); i++)
            if (!strcmp(key, k_cfg_aliases[i][0])) mapped = k_cfg_aliases[i][1];
        gs_cfg_put(section, mapped, value);
    }
    return section;
}

static char g_cfg_path_arg[GS_STR_LARGE]; // --cfg FILE ("" = search)
static int g_cfg_path_set;

void gs_config_set_cfg_path(const char *path) {
    gs_copy(g_cfg_path_arg, sizeof(g_cfg_path_arg), path);
    g_cfg_path_set = path != NULL;
}

// server.cfg over the JSON section: --cfg FILE (must exist), else $KN_SERVER_CFG, else
// ./server.cfg, else ./cfg/server.cfg. Returns -1 on error.
static int gs_apply_server_cfg(GsConfig *cfg, cJSON *section, char *err, const size_t err_size) {
    char path[GS_STR_LARGE] = "";
    int must_exist = 0;
    const char *env = getenv("KN_SERVER_CFG");
    if (g_cfg_path_set) gs_copy(path, sizeof(path), g_cfg_path_arg), must_exist = 1;
    else if (env && env[0]) gs_copy(path, sizeof(path), env), must_exist = 1;
    else if (gs_file_exists("server.cfg")) gs_copy(path, sizeof(path), "server.cfg");
    else if (gs_file_exists("cfg/server.cfg")) gs_copy(path, sizeof(path), "cfg/server.cfg");
    if (!path[0] || (g_cfg_path_set && !strcmp(path, "none"))) return 0;

    FILE *f = fopen(path, "rb");
    if (!f) {
        if (!must_exist) return 0;
        snprintf(err, err_size, "server.cfg %s: %s", path, strerror(errno));
        return -1;
    }
    char *text = NULL;
    size_t len = 0, cap = 0;
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (len + got + 1 > cap) {
            cap = (len + got + 1) * 2;
            char *grown = realloc(text, cap);
            if (!grown) break;
            text = grown;
        }
        memcpy(text + len, chunk, got);
        len += got;
    }
    fclose(f);
    if (text) text[len] = 0;

    char perr[160] = "";
    cJSON *parsed = gs_cfg_parse(text ? text : "", perr, sizeof(perr));
    free(text);
    if (!parsed) {
        snprintf(err, err_size, "%s: %s", path, perr);
        return -1;
    }
    cJSON *item = parsed->child;
    while (item) {
        cJSON *next = item->next;
        cJSON_DetachItemViaPointer(parsed, item);
        const cJSON *old = cJSON_GetObjectItemCaseSensitive(section, item->string);
        if (old && cJSON_IsObject(old) && cJSON_IsObject(item)) { // game.* merges into "game"
            cJSON *dst = cJSON_GetObjectItemCaseSensitive(section, item->string);
            cJSON *sub = item->child;
            while (sub) {
                cJSON *sub_next = sub->next;
                cJSON_DetachItemViaPointer(item, sub);
                if (cJSON_GetObjectItemCaseSensitive(dst, sub->string)) cJSON_ReplaceItemInObjectCaseSensitive(dst, sub->string, sub);
                else cJSON_AddItemToObject(dst, sub->string, sub);
                sub = sub_next;
            }
            cJSON_Delete(item);
        } else if (old) {
            cJSON_ReplaceItemInObjectCaseSensitive(section, item->string, item);
        } else {
            cJSON_AddItemToObject(section, item->string, item);
        }
        item = next;
    }
    cJSON_Delete(parsed);
    gs_copy(cfg->cfg_path, sizeof(cfg->cfg_path), path);
    return 0;
}

// ------------------------------------------------------------------ load

static cJSON *g_doc;          // the loaded file (kept for the "game" host config pointers)
static cJSON *g_cli_section;  // --gameserver.<key>=<value> overrides (main.c)

int gs_config_set_cli(const char *key, const char *value) {
    if (!g_cli_section) g_cli_section = cJSON_CreateObject();
    if (!g_cli_section || !key || !key[0]) return -1;
    // --set gameserver.game.<var>=<value>: the game object's <var> (as server.cfg's `game.<var> value`)
    if (!strncmp(key, "game.", 5) && key[5]) {
        cJSON *game = cJSON_GetObjectItemCaseSensitive(g_cli_section, "game");
        if (!cJSON_IsObject(game)) {
            cJSON_DeleteItemFromObjectCaseSensitive(g_cli_section, "game");
            game = cJSON_AddObjectToObject(g_cli_section, "game");
        }
        if (!game) return -1;
        gs_override(game, key + 5, value ? value : "");
        return 0;
    }
    gs_override(g_cli_section, key, value ? value : "");
    return 0;
}

int gs_config_load(GsConfig *cfg, const char *path, const int explicit_path, char *err, const size_t err_size) {
    char found[GS_STR_LARGE] = "";
    int is_explicit = explicit_path && path;

    if (path) gs_copy(found, sizeof(found), path);
    else {
        const int how = gs_find_config(found, sizeof(found));
        if (how == 2) is_explicit = 1;
    }

    cJSON_Delete(g_doc);
    g_doc = NULL;

    if (found[0]) {
        if (!gs_file_exists(found)) {
            if (is_explicit) {
                snprintf(err, err_size, "config file %s: %s", found, strerror(errno));
                return -1;
            }
        } else if (load_json_file(found, &g_doc) || !cJSON_IsObject(g_doc)) {
            snprintf(err, err_size, "config file %s is not a valid JSON object", found);
            cJSON_Delete(g_doc);
            g_doc = NULL;
            return -1;
        } else {
            gs_copy(cfg->path, sizeof(cfg->path), found);
            gs_set_root_from(found);
        }
    }

    if (!g_doc) g_doc = cJSON_CreateObject();
    cJSON *section = cJSON_GetObjectItemCaseSensitive(g_doc, "gameserver");
    if (!section) section = cJSON_AddObjectToObject(g_doc, "gameserver");
    if (!cJSON_IsObject(section)) {
        snprintf(err, err_size, "\"gameserver\" in %s must be an object", found);
        return -1;
    }

    if (gs_apply_server_cfg(cfg, section, err, err_size)) return -1;
    gs_env_to_json(section);

    const cJSON *cli;
    cJSON_ArrayForEach(cli, g_cli_section) {
        cJSON *old = cJSON_GetObjectItemCaseSensitive(section, cli->string);
        if (cJSON_IsObject(old) && cJSON_IsObject(cli)) { // game.<var> overrides keep the file's other game keys
            const cJSON *sub;
            cJSON_ArrayForEach(sub, cli) {
                cJSON *dup = cJSON_Duplicate(sub, 1);
                if (!dup) continue;
                if (cJSON_GetObjectItemCaseSensitive(old, sub->string)) cJSON_ReplaceItemInObjectCaseSensitive(old, sub->string, dup);
                else cJSON_AddItemToObject(old, sub->string, dup);
            }
            continue;
        }
        cJSON *copy = cJSON_Duplicate(cli, 1);
        if (!copy) continue;
        if (cJSON_GetObjectItemCaseSensitive(section, cli->string)) cJSON_ReplaceItemInObjectCaseSensitive(section, cli->string, copy);
        else cJSON_AddItemToObject(section, cli->string, copy);
    }

    if (gs_config_from_json(cfg, section, err, err_size)) return -1;
    gs_apply_index(cfg);
    gs_resolve_brain_url(cfg, cJSON_GetObjectItemCaseSensitive(g_doc, "brain"));
    if (strchr(cfg->node_id, ':')) {
        snprintf(err, err_size, "gameserver.node_id '%s' must not contain ':'", cfg->node_id);
        return -1;
    }
    return 0;
}

int gs_config_apply_env(GsConfig *cfg) {
    cJSON *section = cJSON_CreateObject();
    const int count = gs_env_to_json(section);
    char err[128];
    gs_config_from_json(cfg, section, err, sizeof(err));
    cJSON_Delete(section);
    return count;
}

// ------------------------------------------------------------------ derived values

int gs_lobby_size(const GsConfig *cfg, const int mode_id) {
    int size = cfg->max_players > 0 ? cfg->max_players : 8;
    const GameModeConfig *mode = mode_config(mode_id);

    if (mode && mode->teams) {
        const int team = mode_id >= 0 && mode_id < MODE_COUNT && cfg->mode_team_size[mode_id] ? cfg->mode_team_size[mode_id] : cfg->team_size;
        if (team > 0) size = team * 2;
    }
    if (mode_id >= 0 && mode_id < MODE_COUNT && cfg->mode_max_players[mode_id] > 0) size = cfg->mode_max_players[mode_id];
    return size < 1 ? 1 : size > 31 ? 31 : size; // player numbers are bits of a uint32
}

float gs_round_seconds(const GsConfig *cfg, const int mode_id) {
    if (mode_id >= 0 && mode_id < MODE_COUNT && cfg->mode_round_seconds[mode_id] > 0.0f) return cfg->mode_round_seconds[mode_id];
    return cfg->round_seconds;
}

static int gs_has_suffix(const char *s, const char *suffix) {
    const size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcasecmp(s + a - b, suffix);
}

static int gs_cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *) a, *(char *const *) b);
}

// Every *.json map in `dir` (sorted by file name), appended to list. JSON is kept for
// the process lifetime.
static void gs_append_dir_maps(const char *dir, const cJSON ***list, size_t *count) {
    char full[GS_STR_LARGE];
    gs_resolve_path(full, sizeof(full), dir);
    DIR *d = opendir(full);
    if (!d) {
        server_log_error("config: custom map dir %s: %s", full, strerror(errno));
        return;
    }

    char *names[256];
    size_t name_count = 0;
    struct dirent *e;
    while ((e = readdir(d)) && name_count < 256) {
        if (e->d_name[0] == '.' || !gs_has_suffix(e->d_name, ".json")) continue;
        // catalogue maps (assets/maps/custom/<key>.json) are picked by "maps", not twice here
        char stem[GS_STR_LARGE];
        snprintf(stem, sizeof(stem), "%.*s", (int) (strlen(e->d_name) - 5), e->d_name);
        if (map_catalog_index_of_key(stem) >= 0) continue;
        names[name_count] = strdup(e->d_name);
        if (names[name_count]) name_count++;
    }
    closedir(d);
    qsort(names, name_count, sizeof(names[0]), gs_cmp_str);

    for (size_t i = 0; i < name_count; i++) {
        char path[GS_STR_LARGE * 2];
        snprintf(path, sizeof(path), "%s/%s", full, names[i]);
        cJSON *map = NULL;
        if (load_json_file(path, &map) || !cJSON_IsObject(map) || !cJSON_IsArray(cJSON_GetObjectItem(map, "objects"))) {
            server_log_error("config: skipping %s (not a Krunker map JSON)", path);
            cJSON_Delete(map);
        } else {
            map_sanitize_json(map);
            const cJSON **grown = realloc(*list, (*count + 1) * sizeof(cJSON *));
            if (grown) {
                *list = grown;
                (*list)[(*count)++] = map;
            } else {
                cJSON_Delete(map);
            }
        }
        free(names[i]);
    }
}

const cJSON **gs_build_map_list(const GsConfig *cfg, size_t *count) {
    *count = 0;
    if (!cfg->map_count && !cfg->custom_map_dir_count) return NULL;

    const cJSON **list = NULL;
    if (cfg->map_count) {
        // catalogue maps by name / key, and published maps (published:<creator>/<name>: fetched
        // from the brain, checked like v10, cached; published_maps.h), in the list's order
        int indices[GS_MAX_LIST];
        const cJSON *published[GS_MAX_LIST];
        size_t n = 0, catalogue = 0;
        for (int i = 0; i < cfg->map_count; i++) {
            char name[96], creator[64];
            published[n] = NULL;
            if (published_map_parse(cfg->maps[i], name, sizeof(name), creator, sizeof(creator))) {
                char err[160];
                published[n] = published_map_load(cfg, name, creator, 10000, err, sizeof(err));
                if (!published[n]) {
                    server_log_error("config: gameserver.maps: published map '%s' skipped (%s)", cfg->maps[i], err);
                    continue;
                }
                indices[n++] = -1;
                continue;
            }
            const int index = map_catalog_find(cfg->maps[i]);
            if (index < 0) server_log_error("config: gameserver.maps: unknown map '%s' (skipped)", cfg->maps[i]);
            else {
                indices[n++] = index;
                catalogue++;
            }
        }
        if (n) {
            size_t loaded = 0;
            free(catalogue ? (void *) map_catalog_list(indices, n, &loaded) : NULL); // loads the catalogue maps
            list = calloc(n, sizeof(cJSON *));
            for (size_t i = 0; list && i < n; i++) {
                const cJSON *json = indices[i] >= 0 ? map_catalog_json((size_t) indices[i]) : published[i];
                if (json) list[(*count)++] = json;
            }
        }
    } else {
        // only custom dirs: they join Krunker's rotation
        int indices[GS_MAX_LIST];
        const size_t n = map_catalog_rotation(indices, GS_MAX_LIST);
        list = map_catalog_list(indices, n, count);
    }

    for (int i = 0; i < cfg->custom_map_dir_count; i++) gs_append_dir_maps(cfg->custom_map_dirs[i], &list, count);
    if (!*count) {
        free(list);
        return NULL;
    }
    return list;
}

// A mode this server's lobby can play: its minPlayers fit the mode's lobby size (gs_lobby_size:
// max_players, mode_max_players, mode_team_size) and team modes have a player per team.
static int gs_mode_fits_lobby(const GsConfig *cfg, const int mode_id) {
    const GameModeConfig *mode = mode_config(mode_id);
    if (!mode) return 0;
    const int size = gs_lobby_size(cfg, mode_id);
    const int teams = mode->teams ? (mode->multi_team ? mode->multi_team : 2) : 1;
    return mode->min_players <= size && teams <= size;
}

// The game's mode list. KN (decisions.md "Public mode rotation"): a rotation of several modes
// drops the ones the lobby cannot play (gs_mode_fits_lobby); a single mode is kept as configured.
int *gs_build_mode_list(const GsConfig *cfg, size_t *count) {
    *count = 0;
    if (!cfg->mode_count) return NULL;
    int *modes = calloc((size_t) cfg->mode_count, sizeof(int));
    if (!modes) return NULL;
    for (int i = 0; i < cfg->mode_count; i++) {
        if (cfg->mode_count > 1 && !gs_mode_fits_lobby(cfg, cfg->modes[i])) {
            server_log_error("config: mode %s left out of the rotation (lobby of %d, needs %d players)", mode_name(cfg->modes[i]),
                             gs_lobby_size(cfg, cfg->modes[i]), mode_config(cfg->modes[i]) ? mode_config(cfg->modes[i])->min_players : 0);
            continue;
        }
        modes[(*count)++] = cfg->modes[i];
    }
    if (!*count) { // nothing fits: play the list as configured rather than nothing
        memcpy(modes, cfg->modes, (size_t) cfg->mode_count * sizeof(int));
        *count = (size_t) cfg->mode_count;
    }
    return modes;
}

const GsBillboard *gs_billboard_for_cycle(const GsConfig *cfg, const unsigned int cycle) {
    return cfg->billboard_count ? &cfg->billboards[cycle % (unsigned int) cfg->billboard_count] : NULL;
}

int gs_is_op(const GsConfig *cfg, const int account_id, const char *name) {
    for (int i = 0; i < cfg->op_count; i++) {
        const char *op = cfg->ops[i];
        char *end = NULL;
        const long id = strtol(op, &end, 10);
        if (end && !*end && end != op) {
            if (account_id > 0 && id == account_id) return 1;
        } else if (name && name[0] && account_id > 0 && !strcasecmp(op, name)) {
            return 1; // names only identify accounts (a guest could pick any nickname)
        }
    }
    return 0;
}

void gs_config_log_summary(const GsConfig *cfg) {
    server_log("Config: %s | server '%s' region %s node %s | public %s:%u | lobby %d | tick %u Hz | brain %s",
               cfg->path[0] ? cfg->path : "(none, defaults)", cfg->name, cfg->region, cfg->node_id, cfg->public_host,
               (unsigned int) (cfg->public_port ? cfg->public_port : cfg->listen_port), (int) g_server->config.max_players, (unsigned int) cfg->tick_rate,
               cfg->brain_url[0] ? cfg->brain_url : "none (standalone)");
    server_log("Server: %s%s | %s | query udp %u | rcon %s | finder %s%s%s",
               cfg->cfg_path[0] ? "server.cfg " : "no server.cfg", cfg->cfg_path,
               cfg->online_mode ? "online mode (accounts verified, guests welcome)" : "offline mode (coordinator accounts, guests)",
               (unsigned int) (cfg->query_port ? cfg->query_port : cfg->listen_port),
               cfg->rcon_port && cfg->rcon_password[0] ? "local tcp" : cfg->brain_url[0] ? "coordinator only" : "off",
               cfg->finder_url[0] ? cfg->finder_url : "off (unlisted)", cfg->password[0] ? " | password protected" : "",
               cfg->community[0] ? " | community set" : "");
    if (!cfg->lag_comp) server_log("Lag compensation: off");
    else server_log("Lag compensation: %s, lagComp scale %.2f (x each player's slider), %.0f ms history",
                    cfg->lag_comp_anchor == LAG_COMP_ANCHOR_EXACT ? "exact (client input time + ping)" :
                    cfg->lag_comp_anchor == LAG_COMP_ANCHOR_ARRIVAL ? "arrival (input arrival + ping)" : "v3.1.1 (ping * 2)",
                    cfg->lag_comp_scale, GAME_STATE_HISTORY * 1000.0f);
}
