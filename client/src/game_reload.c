// F5 / console `reload`: the full reload (client/include/game_reload.h, doc/console.md "F5: full
// reload"). Krunker's F5 is the browser's page reload: everything (page, settings, textures,
// models, sounds, shaders) comes from the server / disk again and the address bar brings the
// player back into the same game. The web build keeps exactly that (app_reload). Natively the
// client reloads in place what can be swapped under a running game, and re-executes itself like
// F4 (app_reload, the window kept) only for what a running process cannot take again.
#include <game_reload.h>
#include <app_mode.h>        // app_reload
#include <audio.h>
#include <backend.h>         // backend_available: a lobby id needs the matchmaker
#include <backend_ui.h>      // servers_ui_session_target
#include <client_settings.h> // ui_client_settings_reload, client_settings_before_exec
#include <console.h>         // ui_console_exec / ui_console_reconnect
#include <gltf.h>
#include <html_ui.h>
#include <map_catalog.h>
#include <ruleset_ui.h>      // client_ruleset_page_pending
#include <ui_logic.h>
#include <ui_mods.h>
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef __EMSCRIPTEN__
#include <dirent.h>
#include <sys/stat.h>
#endif

#define RELOAD_RESUME_ENV "KRUNKNATIVE_RELOAD_RESUME" // restart fallback: the session to come back to
#define RESUME_WAIT_SECONDS 30.0  // the new process: the menu and the map list must be up by then
#define RESUME_LOBBY_SECONDS 15.0 // a lobby id waits this long for the matchmaker (backend_available)
#define SCAN_DEPTH_MAX 8

typedef enum { RESUME_NONE, RESUME_START, RESUME_MODE } ResumeStage;

static struct {
    int requested;
    char why[32];
    double epoch; // wall clock (s): files modified later are new to this process
    // restart fallback, the new process side: "offline <map key> <mode>", "connect <host:port>",
    // "game <lobby id>"
    ResumeStage stage;
    char kind[16];
    char target[200];
    int mode;
    double since; // glfwGetTime when the stage began
} g_rl = {.mode = -1};

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

void game_reload_startup(void) {
    g_rl.epoch = wall_now();
    const char *resume = getenv(RELOAD_RESUME_ENV);
    if (!resume || !*resume) return;
    char copy[300];
    snprintf(copy, sizeof(copy), "%s", resume);
    set_env(RELOAD_RESUME_ENV, NULL); // this process only: a later restart writes its own
    char *kind = strtok(copy, "\t"), *target = kind ? strtok(NULL, "\t") : NULL, *mode = target ? strtok(NULL, "\t") : NULL;
    if (!kind || !target) return;
    snprintf(g_rl.kind, sizeof(g_rl.kind), "%s", kind);
    snprintf(g_rl.target, sizeof(g_rl.target), "%s", target);
    g_rl.mode = mode ? atoi(mode) : -1;
    g_rl.stage = RESUME_START;
    g_rl.since = -1.0;
    printf("[reload] resume after the restart: %s %s%s%s\n", kind, target, mode ? " mode " : "", mode ? mode : "");
}

void game_reload_request(const char *why) {
    g_rl.requested = 1;
    snprintf(g_rl.why, sizeof(g_rl.why), "%s", why && *why ? why : "reload");
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
// root the window templates (read again in place) and other page trees (ui/v10: a directory
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
    if (client_ruleset_page_pending()) {
        snprintf(out, n, "the saved Game version has another menu page");
        return 1;
    }
    char what[1024] = "";
    const char *ui_env = getenv("KRUNKNATIVE_UI_DIR"); // the page tree loaded (ruleset_ui.c / the user's)
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

// The new process after a restart: into the session the old one was in.
static void resume_tick(Client *client) {
    if (g_rl.stage == RESUME_NONE) return;
    const double now = glfwGetTime();
    if (g_rl.since < 0.0) g_rl.since = now;
    const double waited = now - g_rl.since;
    if (g_rl.stage == RESUME_START) {
        const int lobby = !strcmp(g_rl.kind, "game");
        const int ready = menu_ui()->dom_ready && client->game.map_count && (!lobby || backend_available() == 1);
        if (!ready) {
            if (waited > (lobby ? RESUME_LOBBY_SECONDS : RESUME_WAIT_SECONDS)) {
                printf("[reload] resume: gave up waiting for the %s\n", lobby ? "matchmaker" : "menu");
                ui_notify("Reloaded. The game could not be rejoined");
                g_rl.stage = RESUME_NONE;
            }
            return;
        }
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
        return;
    }
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
}
#endif

void game_reload_tick(Client *client) {
    if (!client) return;
    if (g_rl.requested) {
        g_rl.requested = 0;
#ifdef __EMSCRIPTEN__
        app_reload(NULL); // location.reload(): the browser reloads everything, the address bar keeps the game
#else
        reload_now(client);
#endif
    }
#ifndef __EMSCRIPTEN__
    resume_tick(client);
#endif
}
