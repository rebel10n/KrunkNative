// F5 / console `reload`: the full reload (client/include/game_reload.h, doc/console.md "F5: full
// reload"). Krunker's F5 is the browser's page reload: everything (page, settings, textures,
// models, sounds, shaders) comes from the server / disk again and the address bar brings the
// player back into the same game. The web build keeps exactly that (app_reload). Natively the
// client reloads in place what can be swapped under a running game, and re-executes itself like
// F4 (app_reload, the window kept) only for what a running process cannot take again.
// The same restart serves the updates (game_reload.h "auto-restart for updates").
#include <game_reload.h>
#include <app_mode.h>        // app_reload, app_reload_prepare, app_exec_args
#include <audio.h>
#include <backend.h>         // backend_available: a lobby id needs the matchmaker
#include <backend_ui.h>      // servers_ui_session_target, servers_ui_resume_rejoin
#include <client_settings.h> // ui_client_settings_reload, client_settings_before_exec, client_settings_auto_restart
#include <console.h>         // ui_console_exec / ui_console_reconnect
#include <gltf.h>
#include <html_ui.h>
#include <map_catalog.h>
#include <ui_logic.h>
#include <ui_mods.h>
#include <update_ui.h>       // update_ui_restart_via_launcher
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#define RELOAD_RESUME_ENV "KRUNKNATIVE_RELOAD_RESUME" // restart fallback: the session to come back to
#define RESUME_WAIT_SECONDS 30.0  // the new process: the menu and the map list must be up by then
#define RESUME_LOBBY_SECONDS 15.0 // a lobby id waits this long for the matchmaker (backend_available)
#define SCAN_DEPTH_MAX 8
// auto-restart for updates
#define UPDATE_SCREEN_SECONDS 1.5    // the Game Updating screen is shown this long before the restart
#define UPDATE_MENU_SECONDS 1.5      // a newer client, in the menu: the notice, then the restart
#define UPDATE_MATCH_END_SECONDS 5.0 // a newer client, the match ended: the results stay this long

typedef enum { RESUME_NONE, RESUME_START, RESUME_MODE } ResumeStage;

static struct {
    int requested;
    int relaunch; // F5 (mm-token): the restart itself, not the in-place reload
    char why[32];
    double epoch; // wall clock (s): files modified later are new to this process
    // restart fallback, the new process side: "offline <map key> <mode>", "connect <host:port>",
    // "game <lobby id>", "rejoin <lobby id>" (a server update: the restart rejoin's backoff)
    ResumeStage stage;
    char kind[16];
    char target[200];
    int mode;
    double since; // glfwGetTime when the stage began
} g_rl = {.mode = -1};

// auto-restart for updates (game_reload.h)
static struct {
    int server;       // the server of the session stopped for an update: resume below
    char resume[300]; // the new process's session line ("" = session_line at the restart)
    int newer;        // a newer client is ready: restart once out of the match
    char what[64];
    double due;       // glfwGetTime of the restart, 0 = none due
    int done;         // a restart started: never a second one
} g_ar;

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#elif !defined(__EMSCRIPTEN__)
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#else
    (void) name;
    (void) value;
#endif
}

static double wall_now(void) {
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
    return (double) time(NULL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
#endif
}

#ifdef __EMSCRIPTEN__
// The web's handover across a reload: sessionStorage (this tab only, survives location.reload()).
#define WEB_REJOIN_KEY "knRejoin"
EM_JS(void, kn_js_session_set, (const char *key, const char *value), {
    try {
        var k = UTF8ToString(key), v = UTF8ToString(value);
        if (v) sessionStorage.setItem(k, v);
        else sessionStorage.removeItem(k);
    } catch (e) {}
});
EM_JS(char *, kn_js_session_take, (const char *key), {
    var v = "";
    try {
        var k = UTF8ToString(key);
        v = sessionStorage.getItem(k) || "";
        sessionStorage.removeItem(k);
    } catch (e) {}
    return stringToNewUTF8(v);
});
#endif

// The session line a restart handed over: "<kind>\t<target>[\t<mode>]".
static void resume_parse(const char *resume) {
    char copy[300];
    snprintf(copy, sizeof(copy), "%s", resume);
    char *kind = strtok(copy, "\t"), *target = kind ? strtok(NULL, "\t") : NULL, *mode = target ? strtok(NULL, "\t") : NULL;
    if (!kind || !target) return;
    snprintf(g_rl.kind, sizeof(g_rl.kind), "%s", kind);
    snprintf(g_rl.target, sizeof(g_rl.target), "%s", target);
    g_rl.mode = mode ? atoi(mode) : -1;
    g_rl.stage = RESUME_START;
    g_rl.since = -1.0;
    printf("[reload] resume after the restart: %s %s%s%s\n", kind, target, mode ? " mode " : "", mode ? mode : "");
    if (!strcmp(kind, "game")) {
        // a lobby: the startup seek takes it, like the web reload's ?game= (servers_ui_query), so
        // no second seek races it (both landed in the lobby: two joins, one dropped)
        char query[220];
        snprintf(query, sizeof(query), "game=%s", target);
        servers_ui_query(query);
        g_rl.stage = RESUME_NONE;
    } else if (!strcmp(kind, "rejoin")) {
        // a server update: no startup seek; the restart rejoin starts once the matchmaker answers
        servers_ui_hold_startup_seek(1);
    }
}

void game_reload_startup(void) {
    g_rl.epoch = wall_now();
#ifdef __EMSCRIPTEN__
    char *rejoin = kn_js_session_take(WEB_REJOIN_KEY);
    if (rejoin && rejoin[0]) {
        char line[300];
        snprintf(line, sizeof(line), "rejoin\t%s", rejoin);
        resume_parse(line);
    }
    free(rejoin);
#else
    const char *resume = getenv(RELOAD_RESUME_ENV);
    if (!resume || !*resume) return;
    char copy[300];
    snprintf(copy, sizeof(copy), "%s", resume);
    set_env(RELOAD_RESUME_ENV, NULL); // this process only: a later restart writes its own
    resume_parse(copy);
#endif
}

void game_reload_request(const char *why) {
    g_rl.requested = 1;
    snprintf(g_rl.why, sizeof(g_rl.why), "%s", why && *why ? why : "reload");
}

void game_reload_relaunch_request(const char *why) {
    game_reload_request(why);
    g_rl.relaunch = 1;
}

// The session the restarted process comes back to (RELOAD_RESUME_ENV); 0 = none (the menu, or a
// map outside the catalogue: the command line decides).
static int session_line(const Client *client, char *out, const size_t n) {
    if (client->offline) {
        const Game *game = &client->game;
        const int index = game->ready && game->map && game->map->raw_data ? map_catalog_index_of_json(game->map->raw_data) : -1;
        const MapCatalogEntry *entry = index >= 0 ? map_catalog_at((size_t) index) : NULL;
        if (!entry) return 0;
        snprintf(out, n, "offline\t%s\t%d", entry->key, game_mode_id(game));
        return 1;
    }
    if (!client->net_connected) return 0;
    char target[200];
    const int kind = servers_ui_session_target(target, sizeof(target));
    if (!kind) return 0;
    snprintf(out, n, "%s\t%s", kind == 2 ? "game" : "connect", target);
    return 1;
}

#ifndef __EMSCRIPTEN__
static double file_mtime(const struct stat *st) {
#if defined(__linux__)
    return (double) st->st_mtim.tv_sec + (double) st->st_mtim.tv_nsec / 1e9;
#elif defined(__APPLE__)
    return (double) st->st_mtimespec.tv_sec + (double) st->st_mtimespec.tv_nsec / 1e9;
#else
    return (double) st->st_mtime;
#endif
}

// The first file under `dir` modified after the epoch (its path in `what`). At the page tree's
// root the window templates (read again in place) and other page trees (ui/: a directory
// with its own index.html) are skipped.
static int newer_in(const char *dir, const int depth, const int page_root, char *what, const size_t n) {
    if (depth > SCAN_DEPTH_MAX) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    const size_t len = strlen(dir);
    const char *sep = len && (dir[len - 1] == '/' || dir[len - 1] == '\\') ? "" : "/";
    int found = 0;
    for (struct dirent *e; !found && (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s%s%s", dir, sep, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (page_root) {
                if (!strcmp(e->d_name, "windows")) continue;
                char index[1100];
                struct stat ist;
                snprintf(index, sizeof(index), "%s/index.html", path);
                if (stat(index, &ist) == 0) continue;
            }
            found = newer_in(path, depth + 1, 0, what, n);
        } else if (S_ISREG(st.st_mode) && file_mtime(&st) > g_rl.epoch) {
            snprintf(what, n, "%s", path);
            found = 1;
        }
    }
    closedir(d);
    return found;
}

// What a running process cannot load again: the reason in `out`, 0 when nothing.
static int restart_reason(char *out, const size_t n) {
    char what[1024] = "";
    const char *ui_env = getenv("KRUNKNATIVE_UI_DIR"); // the page tree loaded (the user's, else ui/)
    if (newer_in(ui_env && *ui_env ? ui_env : ui_default_path(), 0, 1, what, sizeof(what))) {
        snprintf(out, n, "menu page file %s changed", what);
        return 1;
    }
    static const char *const DIRS[][2] = {{"shaders", "shader"}, {"maps", "map file"}};
    for (size_t i = 0; i < sizeof(DIRS) / sizeof(DIRS[0]); i++) {
        char *dir = concat(assets_path(), DIRS[i][0]);
        const int changed = dir && newer_in(dir, 0, 0, what, sizeof(what));
        free(dir);
        if (changed) {
            snprintf(out, n, "%s %s changed", DIRS[i][1], what);
            return 1;
        }
    }
    return 0;
}

// F4's restart (app_reload: the same command line, the window as it is) with the session handed
// over. Returns only when the exec failed.
static void restart(Client *client, const char *why) {
    char resume[300] = "";
    printf("[reload] restart: %s\n", why);
    if (session_line(client, resume, sizeof(resume))) set_env(RELOAD_RESUME_ENV, resume);
    client_settings_before_exec();
    app_reload(NULL);
    set_env(RELOAD_RESUME_ENV, NULL);
    fprintf(stderr, "[reload] restart failed: reloading in place\n");
}

// Back into what the player was doing: `reconnect` (the offline game restarts on its map and
// mode with the new assets; an online one rejoins the same server or lobby).
static void reenter(Client *client) {
    if (!client->offline && !client->net_connected) {
        printf("[reload] no game to rejoin\n");
        ui_notify("Reloaded");
        return;
    }
    const int offline = client->offline;
    if (ui_console_reconnect()) {
        printf("[reload] %s\n", offline ? "the offline game restarts" : "rejoining the server");
        ui_notify(offline ? "Reloaded" : "Reloaded: rejoining the server");
        return;
    }
    printf("[reload] could not rejoin: back to the menu\n");
    client_set_menu_open(client, 1);
    ui_notify("Reloaded. The game could not be rejoined");
}

static void reload_now(Client *client) {
    const double started = glfwGetTime();
    printf("[reload] %s: full reload\n", g_rl.why);
    char why[1200] = "";
    const int page_or_files = restart_reason(why, sizeof(why));
    // the Client tab first: its startup-only ("*") rows are a reason to restart too
    const char *startup_setting = ui_client_settings_reload();
    if (!page_or_files && startup_setting) snprintf(why, sizeof(why), "the setting \"%s\" applies at startup", startup_setting);
    if (why[0]) restart(client, why); // returns when the exec failed: the rest still reloads

    // settings and binds
    const int settings = ui_settings_reload();
    ui_controls_load();
    ui_mods_reload();
    ui_mod_keys_reload();
    // assets: the same GL objects get the files' new contents, so every mesh follows
    const int textures = texture_cache_reload_all();
    const int models = geometry_cache_reload_all();
    gltf_model_forget_all(); // read again at their next use (the re-entered map)
    audio_reload_all();      // likewise
    ui_windows_reload();
    // the page: an open window generated again, the HUD rewritten
    const MenuUi *ui = menu_ui();
    if (ui->dom_ready && menu_window_is_open()) menu_show_window(ui->active_window, 1);
    hud_ui_relayout();
    client->hud_html.valid = 0;
    printf("[reload] settings: %d changed; textures: %d, OBJ models: %d reloaded; GLTF models, sounds and window templates "
           "read again (%.0f ms)\n", settings, textures, models, (glfwGetTime() - started) * 1000.0);
    if (!page_or_files) g_rl.epoch = wall_now(); // nothing newer is left behind
    reenter(client);
}

static int same_server(const Client *client, const char *target) {
    char here[96];
    snprintf(here, sizeof(here), "%s:%u", client->net_address, (unsigned int) client->net_port);
    return (client->net_connected || client->net_thread_running) && !strcmp(here, target);
}
#endif

// The new process after a restart: into the session the old one was in.
static void resume_tick(Client *client) {
    if (g_rl.stage == RESUME_NONE) return;
    const double now = glfwGetTime();
    if (g_rl.since < 0.0) g_rl.since = now;
    const double waited = now - g_rl.since;
    if (g_rl.stage == RESUME_START) {
        const int lobby = !strcmp(g_rl.kind, "game") || !strcmp(g_rl.kind, "rejoin");
        const int ready = menu_ui()->dom_ready && client->game.map_count && (!lobby || backend_available() == 1);
        if (!ready) {
            if (waited > (lobby ? RESUME_LOBBY_SECONDS : RESUME_WAIT_SECONDS)) {
                printf("[reload] resume: gave up waiting for the %s\n", lobby ? "matchmaker" : "menu");
                if (!strcmp(g_rl.kind, "rejoin")) servers_ui_hold_startup_seek(0); // the ordinary startup seek, if any
                ui_notify("Reloaded. The game could not be rejoined");
                g_rl.stage = RESUME_NONE;
            }
            return;
        }
        if (!strcmp(g_rl.kind, "rejoin")) {
            // a server update: the same lobby again with the restart rejoin's backoff (servers_ui.c)
            g_rl.stage = RESUME_NONE;
            printf("[reload] resume: rejoining %s after the server's update\n", g_rl.target);
            servers_ui_resume_rejoin(g_rl.target);
            return;
        }
#ifdef __EMSCRIPTEN__
        g_rl.stage = RESUME_NONE; // the address bar brought the page back: nothing else to resume
#else
        char line[256];
        if (!strcmp(g_rl.kind, "offline")) {
            snprintf(line, sizeof(line), "offline %s", g_rl.target);
            g_rl.stage = RESUME_MODE;
            g_rl.since = now;
        } else {
            g_rl.stage = RESUME_NONE;
            if (!lobby && same_server(client, g_rl.target)) { // the command line joins it already
                printf("[reload] resume: already joining %s\n", g_rl.target);
                return;
            }
            snprintf(line, sizeof(line), "connect %s", g_rl.target);
        }
        printf("[reload] resume: %s\n", line);
        ui_console_exec(line);
#endif
        return;
    }
#ifndef __EMSCRIPTEN__
    // RESUME_MODE: the offline game is up on its map; its mode too when it was another one
    if (client->offline && client->game.ready) {
        if (g_rl.mode >= 0 && game_mode_id(&client->game) != g_rl.mode) {
            printf("[reload] resume: mode %d\n", g_rl.mode);
            local_game_host(client, client->game.current_map_index, g_rl.mode);
        }
        g_rl.stage = RESUME_NONE;
    } else if (waited > RESUME_WAIT_SECONDS) {
        g_rl.stage = RESUME_NONE;
    }
#else
    g_rl.stage = RESUME_NONE;
#endif
}

// ---------------------------------------------------------------- auto-restart for updates

int game_reload_server_updating(Client *client, const char *lobby_id) {
    if (!client_settings_auto_restart()) return 0;
    if (g_ar.done) return 1; // the restart is under way
    g_ar.server = 1;
    if (lobby_id && lobby_id[0]) snprintf(g_ar.resume, sizeof(g_ar.resume), "rejoin\t%s", lobby_id);
    else if (client && client->net_address[0]) snprintf(g_ar.resume, sizeof(g_ar.resume), "connect\t%s:%u", client->net_address, (unsigned int) client->net_port);
    else g_ar.resume[0] = 0;
    if (!g_ar.due) g_ar.due = glfwGetTime() + UPDATE_SCREEN_SECONDS;
    printf("[reload] the server is restarting for an update: restarting the client in %.1f s, then %s%s\n", UPDATE_SCREEN_SECONDS,
           lobby_id && lobby_id[0] ? "rejoining lobby " : client && client->net_address[0] ? "reconnecting to " : "back to the menu",
           lobby_id && lobby_id[0] ? lobby_id : client && client->net_address[0] ? client->net_address : "");
    return 1;
}

void game_reload_update_available(const char *what) {
    if (g_ar.newer || g_ar.done) return;
    g_ar.newer = 1;
    snprintf(g_ar.what, sizeof(g_ar.what), "%s", what ? what : "");
    printf("[reload] a newer client is ready (%s): %s\n", g_ar.what,
           client_settings_auto_restart() ? "restarting in the menu or when the match ends" : "Auto-restart for updates is off (Update now restarts)");
}

int game_reload_update_pending(void) {
    if (!g_ar.newer || !client_settings_auto_restart()) return 0;
    return g_ar.due > 0.0 || g_ar.done ? 2 : 1;
}

int game_reload_restart_pending(void) {
    return g_ar.due > 0.0 || g_ar.done;
}

// The restart itself: natively through the launcher when one started this client (its update
// check first: a newer build, installed and verified, comes back), else this executable again; on
// the web the page reload. The session to come back to goes ahead (RELOAD_RESUME_ENV; the web's
// sessionStorage for a lobby, its address bar for the rest).
static void auto_restart_now(Client *client) {
    if (g_ar.done) return;
    g_ar.done = 1;
    g_ar.due = 0.0;
    const char *why = g_ar.server ? "the server restarted for an update" : "a newer client is ready";
    char resume[300];
    snprintf(resume, sizeof(resume), "%s", g_ar.resume);
    if (!resume[0] && !g_ar.server) session_line(client, resume, sizeof(resume));
#ifdef __EMSCRIPTEN__
    if (!strncmp(resume, "rejoin\t", 7)) kn_js_session_set(WEB_REJOIN_KEY, resume + 7);
    printf("[reload] %s: reloading the page\n", why);
    fflush(stdout);
    app_reload(NULL); // location.reload(): the address bar keeps ?game= / ?ws=
#else
    printf("[reload] %s: relaunching%s\n", why, resume[0] ? " (the new process resumes the session)" : "");
    fflush(stdout);
    if (resume[0]) set_env(RELOAD_RESUME_ENV, resume);
    client_settings_before_exec();
    app_reload_prepare();
    if (update_ui_restart_via_launcher(client, app_exec_args())) return; // the launcher restarts us (this window closes)
    app_reload(NULL); // no launcher: this executable again; returns only when the exec failed
    set_env(RELOAD_RESUME_ENV, NULL);
    fprintf(stderr, "[reload] restart failed\n");
    g_ar.done = 0;
    if (g_ar.server) {
        // the session is gone either way: rejoin in place like a client without the setting
        g_ar.server = 0;
        if (!strncmp(resume, "rejoin\t", 7)) servers_ui_resume_rejoin(resume + 7);
        else ui_console_reconnect();
    }
    g_ar.newer = 0; // the update box stays: Update now / Download
#endif
}

static void auto_restart_tick(Client *client) {
    if (g_ar.done) return;
    const double now = glfwGetTime();
    if (g_ar.due > 0.0) {
        if (now >= g_ar.due) auto_restart_now(client);
        return;
    }
    if (!g_ar.newer || !client_settings_auto_restart()) return;
    // a newer client: never in the middle of a match. The menu outside a match (at startup, after
    // the session ended; in_game stays set through deaths and Esc until the round's next INIT) or
    // the end of the match (the results stay a moment) is the time.
    if (html_ui_available() && html_ui_is_menu_open() && !client->in_game) {
        g_ar.due = now + UPDATE_MENU_SECONDS;
        printf("[reload] newer client %s: in the menu, restarting in %.1f s\n", g_ar.what, UPDATE_MENU_SECONDS);
        ui_notify(I18N("kn.update.restartingNotice", "Restarting for an update..."));
    } else if (client_round_ended(client)) {
        g_ar.due = now + UPDATE_MATCH_END_SECONDS;
        printf("[reload] newer client %s: the match ended, restarting in %.1f s\n", g_ar.what, UPDATE_MATCH_END_SECONDS);
    }
}

void game_reload_tick(Client *client) {
    if (!client) return;
    if (g_rl.requested) {
        g_rl.requested = 0;
#ifdef __EMSCRIPTEN__
        g_rl.relaunch = 0;
        app_reload(NULL); // location.reload(): the browser reloads everything, the address bar keeps the game
#else
        if (g_rl.relaunch) {
            // F5: the client relaunches (the same command line and window, like F4 did) and the
            // new process comes back into this session, as the web's reload keeps ?game=
            g_rl.relaunch = 0;
            char why[64];
            snprintf(why, sizeof(why), "%s: relaunch", g_rl.why);
            restart(client, why); // returns only when the exec failed: then the in-place reload
        }
        reload_now(client);
#endif
    }
    resume_tick(client);
    auto_restart_tick(client);
}
