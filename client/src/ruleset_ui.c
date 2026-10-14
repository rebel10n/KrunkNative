// The client's Krunker version: saved choice, startup page tree, INIT adoption, the offline
// sandbox's ruleset (client/include/ruleset_ui.h, doc/v10/version-toggle.md).
#include <ruleset_ui.h>
#include <html_dom.h>
#include <html_ui.h>
#include <ui_logic.h> // ui_notify: the v3.1.1 stand-in notice
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// v3.1.1 / v7.1.4 / v10.0.2. v3.1.1 is its page (ui/v3, branch v3.1.1's ui/) over the v7.1.4
// ruleset until RULESET_V3_1_1 is built (TODO(v3 ruleset), doc/v10/version-toggle.md).
static ClientGameVersion g_versions[RULESET_COUNT];
static int g_version_count;
static const ClientGameVersion *g_page; // the Game version whose page tree is loaded
static int g_ui_env_set;                // KRUNKNATIVE_UI_DIR came from here, not from the user

static void versions_init(void) {
    if (g_version_count) return;
    for (int id = 0; id < RULESET_COUNT; id++) {
        const Ruleset *r = ruleset_get((RulesetId) id);
        ClientGameVersion *v = &g_versions[g_version_count];
        if (r) {
            *v = (ClientGameVersion) {r->key, r->version, r->ui_page_dir ? r->ui_page_dir : "", r, 0};
        } else if (id == RULESET_V3_1_1) {
            *v = (ClientGameVersion) {"v3", "3.1.1", "v3", ruleset_get(RULESET_V7_1_4), 1};
        } else {
            continue;
        }
        g_version_count++;
    }
}

static const ClientGameVersion *version_default(void) {
    return client_game_version_find(ruleset_get(RULESET_DEFAULT)->key);
}

const ClientGameVersion *client_game_versions(int *count) {
    versions_init();
    if (count) *count = g_version_count;
    return g_versions;
}

const ClientGameVersion *client_game_version_find(const char *name) {
    if (!name || !name[0]) return NULL;
    versions_init();
    const char *n = name[0] == 'v' || name[0] == 'V' ? name + 1 : name;
    for (int i = 0; i < g_version_count; i++) {
        const ClientGameVersion *v = &g_versions[i];
        if (!strcmp(n, v->key + 1) || !strcmp(n, v->version)) return v; // "3" / "3.1.1"
    }
    return NULL;
}

int client_game_version_available(const ClientGameVersion *v) {
    if (!v) return 0;
#ifdef __EMSCRIPTEN__
    return !v->stand_in; // the web serves ui/ only: v3.1.1 would be v7.1.4 with v7.1.4's page
#else
    return 1;
#endif
}

const ClientGameVersion *client_game_version_saved(void) {
    char saved[16] = "";
    const ClientGameVersion *v = NULL;
    if (html_storage_get(CLIENT_RULESET_STORAGE, saved, sizeof(saved)) >= 0 && saved[0]) v = client_game_version_find(saved);
    return v && client_game_version_available(v) ? v : version_default();
}

const ClientGameVersion *client_game_version_page(void) {
    return g_page ? g_page : version_default();
}

const char *client_game_version_notice(const ClientGameVersion *v) {
    if (!v || !v->stand_in) return NULL;
    return "Krunker v3.1.1 gameplay is not ported yet: games play Krunker v7.1.4 rules";
}

const Ruleset *client_ruleset_saved(void) {
    return client_game_version_saved()->rules;
}

const Ruleset *client_ruleset_page(void) {
    return client_game_version_page()->rules;
}

#ifndef __EMSCRIPTEN__
static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}
#endif

void client_ruleset_startup(void) {
    const ClientGameVersion *v = client_game_version_saved();
    ruleset_set_current(v->rules->id);
    g_page = version_default();
    if (v->stand_in) printf("[ruleset] Krunker v%s (%s): plays the Krunker v%s ruleset (v%s gameplay is not ported yet)\n",
                            v->version, v->key, v->rules->version, v->version);
    else printf("[ruleset] Krunker v%s (%s)\n", v->version, v->key);
    if (client_page_v156_startup()) return; // knRuleset "v156": ui/v156/ over v7.1.4 gameplay
#ifndef __EMSCRIPTEN__
    if (!v->page_dir[0]) {
        g_page = v; // ui/ itself
        return;
    }
    const char *user_dir = getenv("KRUNKNATIVE_UI_DIR");
    if (user_dir && *user_dir) return; // the user's page tree wins (tests / dev)
    char dir[512], index[600];
    snprintf(dir, sizeof(dir), "%s%s", ui_default_path(), v->page_dir);
    snprintf(index, sizeof(index), "%s/index.html", dir);
    if (!file_exists(index)) {
        fprintf(stderr, "[ruleset] warning: %s missing, the v%s page is not available: showing ui/\n", index, v->version);
        return;
    }
    // a page tree has its own window templates (ui/v10/windows/, tools/ui-import/windows_v10.py);
    // without them ui_windows.c falls back to ui/'s v7.1.4 ones, which this warns about. ui/v3 keeps
    // v3.1.1's templates out on purpose (the C UI writes v7.1.4 window markup; ui/v3/README.md), and
    // the engine reads ui/'s copy of what a page tree lacks (its hub / editor / viewer, images).
    snprintf(index, sizeof(index), "%s/windows/windows.json", dir);
    if (!file_exists(index))
        fprintf(stderr, "[ruleset] warning: %s has no window templates: its menu windows are ui/'s (v7.1.4)\n", dir);
    set_env("KRUNKNATIVE_UI_DIR", dir);
    g_ui_env_set = 1;
    g_page = v;
    printf("[ruleset] menu page: ui/%s (Krunker v%s)\n", v->page_dir, v->version);
#else
    // the web build serves ui/ only (dist/web leaves ui/v10 and ui/v3 out): the page stays, the ruleset applies
    if (v->page_dir[0])
        fprintf(stderr, "[ruleset] warning: the web build has no ui/%s page: showing ui/\n", v->page_dir);
    else
        g_page = v;
#endif
}

void client_ruleset_env_cleanup(void) {
#ifndef __EMSCRIPTEN__
    if (g_ui_env_set) set_env("KRUNKNATIVE_UI_DIR", NULL);
    g_ui_env_set = 0;
    client_page_v156_env_cleanup();
#endif
}

int client_ruleset_page_pending(void) {
#ifdef __EMSCRIPTEN__
    return 0; // one page on the web
#else
    if (client_page_v156_pending()) return 1; // ui/v156/ to load or to leave (ruleset_page_v156.c)
    const ClientGameVersion *saved = client_game_version_saved(), *page = client_game_version_page();
    if (saved == page) return 0;
    const char *user_dir = getenv("KRUNKNATIVE_UI_DIR");
    if (user_dir && *user_dir && !g_ui_env_set) return 0; // the user's page tree wins
    const char *dir = saved->page_dir, *loaded = page->page_dir;
    if (!strcmp(dir, loaded)) return 0;
    if (dir[0]) {
        char index[600];
        snprintf(index, sizeof(index), "%s%s/index.html", ui_default_path(), dir);
        if (!file_exists(index)) return 0; // startup would show ui/ again
    }
    return 1;
#endif
}

ClientRulesetResult client_ruleset_select(Client *client, const char *name) {
    if (client_page_v156_is_name(name)) return client_page_v156_select(client); // menu page only
    const ClientGameVersion *v = client_game_version_find(name);
    if (!v) return CLIENT_RULESET_UNKNOWN;
    if (!client_game_version_available(v)) return CLIENT_RULESET_UNAVAILABLE;
    html_storage_set(CLIENT_RULESET_STORAGE, v->key);
    printf("[ruleset] saved Krunker v%s (%s)%s\n", v->version, v->key, v->stand_in ? ": plays the v7.1.4 ruleset until v3.1.1's is ported" : "");
    // between games the client's own tables follow at once; a session keeps the server's
    if (client && !client->offline && !client->net_session_connected) game_adopt_rules(&client->game, v->rules);
    const ClientGameVersion *page = client_game_version_page();
#ifdef __EMSCRIPTEN__
    (void) page;
    return CLIENT_RULESET_SAVED; // one page on the web
#else
    if (client_page_v156_loaded()) return CLIENT_RULESET_SAVED_RELOAD; // leaving ui/v156/
    return strcmp(page->page_dir, v->page_dir) ? CLIENT_RULESET_SAVED_RELOAD : CLIENT_RULESET_SAVED;
#endif
}

int client_ruleset_on_init(Client *client, const NetInitPacket *init) {
    const int id = init->major == NET_PROTOCOL_MAJOR ? net_ruleset_from_wire(init->ruleset) : RULESET_V7_1_4;
    const Ruleset *rules = ruleset_get((RulesetId) id);
    if (!rules) {
        char reason[96];
        snprintf(reason, sizeof(reason), "This server plays a Krunker version this client does not have (%d)", id);
        fprintf(stderr, "[ruleset] %s\n", reason);
        if (!client->offline) client_net_disconnect(client, reason, NET_ERROR_CODE_RULESET);
        return 0;
    }
    if (client->game.rules != rules) {
        printf("[ruleset] the server plays Krunker v%s: adopting it\n", rules->version);
        game_adopt_rules(&client->game, rules);
    }
    client_page_v156_game_notice(rules);
    return 1;
}

void client_ruleset_before_offline(Client *client) {
    const ClientGameVersion *v = client_game_version_saved();
    game_adopt_rules(&client->game, v->rules); // the sandbox's game_configure takes the current one
    const char *notice = client_game_version_notice(v);
    if (notice) {
        // honest about it: the sandbox server plays v7.1.4 (Game.rules, INIT and the console say v7)
        printf("[ruleset] offline: %s\n", notice);
        ui_notify(notice);
    }
}
