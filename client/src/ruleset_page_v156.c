// Krunker v1.5.6 in the "Game version" picker (ruleset_ui.h): a menu page only. Intentional
// difference from a ruleset: v1.5.6's gameplay tables (constants, weapons, classes, modes, maps)
// are not ported, so there is no RULESET_V1_5_6 and nothing on the wire changes. Picking it saves
// knRuleset "v156": client_ruleset_saved() does not know that key and answers v7.1.4, so offline
// games and quick join play Krunker v7.1.4 (online the server's version, as always), and this
// module loads ui/v156/ (tools/ui-import --ref v156) natively instead of ui/. The player is told
// at the pick and at the first game (client_page_v156_game_notice); never silently v7.
// v1.5.6 has no window templates: its windows (Settings included, so the picker can switch back)
// are ui/'s v7.1.4 ones (ui_windows.c falls back). The web build serves ui/ only and does not list
// it (dist/web leaves ui/v156 out).
#include <ruleset_ui.h>
#include <html_dom.h>
#include <html_ui.h>
#include <ui_logic.h> // ui_notify
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_loaded;  // this process shows ui/v156/
static int g_env_set; // KRUNKNATIVE_UI_DIR came from here
static int g_noticed; // the first game's notice was shown

int client_page_v156_is_name(const char *name) {
    if (!name) return 0;
    if (name[0] == 'v' || name[0] == 'V') name++;
    return !strcmp(name, "156") || !strcmp(name, "1.5.6");
}

int client_page_v156_saved(void) {
    char saved[16] = "";
    return html_storage_get(CLIENT_RULESET_STORAGE, saved, sizeof(saved)) >= 0 && !strcmp(saved, CLIENT_PAGE_V156_KEY);
}

int client_page_v156_loaded(void) {
    return g_loaded;
}

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

int client_page_v156_startup(void) {
    if (!client_page_v156_saved()) return 0;
    printf("[ruleset] Krunker v1.5.6 menu (%s); v1.5.6 gameplay is not ported: games play Krunker v%s\n", CLIENT_PAGE_V156_KEY,
           ruleset_current()->version);
#ifdef __EMSCRIPTEN__
    fprintf(stderr, "[ruleset] warning: the web build has no ui/%s page: showing ui/\n", CLIENT_PAGE_V156_KEY);
#else
    const char *user_dir = getenv("KRUNKNATIVE_UI_DIR");
    if (user_dir && *user_dir) return 1; // the user's page tree wins (tests / dev)
    char dir[512], index[600];
    snprintf(dir, sizeof(dir), "%s%s", ui_default_path(), CLIENT_PAGE_V156_KEY);
    snprintf(index, sizeof(index), "%s/index.html", dir);
    FILE *f = fopen(index, "rb");
    if (!f) {
        fprintf(stderr, "[ruleset] warning: %s missing, the v1.5.6 page is not available: showing ui/\n", index);
        return 1;
    }
    fclose(f);
    fprintf(stderr, "[ruleset] warning: %s has no window templates: its menu windows are ui/'s (v7.1.4)\n", dir);
    set_env("KRUNKNATIVE_UI_DIR", dir);
    g_env_set = g_loaded = 1;
#endif
    return 1;
}

void client_page_v156_env_cleanup(void) {
    if (g_env_set) set_env("KRUNKNATIVE_UI_DIR", NULL);
    g_env_set = 0;
}

int client_page_v156_pending(void) {
#ifdef __EMSCRIPTEN__
    return 0; // one page on the web
#else
    const int saved = client_page_v156_saved();
    if (saved == g_loaded) return 0; // nothing to load, or nothing of v156's to leave
    const char *user_dir = getenv("KRUNKNATIVE_UI_DIR");
    if (user_dir && *user_dir && !g_env_set && !client_ruleset_page()->ui_page_dir[0]) return 0; // the user's page tree wins
    if (saved) {
        char index[600];
        snprintf(index, sizeof(index), "%s%s/index.html", ui_default_path(), CLIENT_PAGE_V156_KEY);
        FILE *f = fopen(index, "rb");
        if (!f) return 0; // startup would show ui/ again
        fclose(f);
    }
    return 1;
#endif
}

ClientRulesetResult client_page_v156_select(Client *client) {
#ifdef __EMSCRIPTEN__
    (void) client;
    return CLIENT_RULESET_UNKNOWN; // one page on the web: not offered there
#else
    html_storage_set(CLIENT_RULESET_STORAGE, CLIENT_PAGE_V156_KEY);
    printf("[ruleset] saved the Krunker v1.5.6 menu (games play v7.1.4)\n");
    const Ruleset *v7 = ruleset_get(RULESET_V7_1_4);
    if (client && !client->offline && !client->net_session_connected) game_adopt_rules(&client->game, v7);
    return g_loaded ? CLIENT_RULESET_SAVED : CLIENT_RULESET_SAVED_RELOAD;
#endif
}

void client_page_v156_game_notice(const Ruleset *rules) {
    if (!g_loaded || g_noticed || !rules) return;
    g_noticed = 1;
    char text[160];
    snprintf(text, sizeof(text), "Krunker v1.5.6 gameplay is not ported: this game plays Krunker v%s", rules->version);
    printf("[ruleset] %s\n", text);
    ui_notify(text);
}
