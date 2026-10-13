// The client's Krunker version: saved choice, startup page tree, INIT adoption, the offline
// sandbox's ruleset (client/include/ruleset_ui.h, doc/v10/version-toggle.md).
#include <ruleset_ui.h>
#include <html_dom.h>
#include <html_ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const Ruleset *g_page_rules; // the ruleset whose page tree is loaded
static int g_ui_env_set;            // KRUNKNATIVE_UI_DIR came from here, not from the user

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

const Ruleset *client_ruleset_saved(void) {
    char saved[16] = "";
    if (html_storage_get(CLIENT_RULESET_STORAGE, saved, sizeof(saved)) < 0 || !saved[0]) return ruleset_get(RULESET_DEFAULT);
    const Ruleset *r = ruleset_find(saved);
    return r ? r : ruleset_get(RULESET_DEFAULT);
}

const Ruleset *client_ruleset_page(void) {
    return g_page_rules ? g_page_rules : ruleset_get(RULESET_DEFAULT);
}

#ifndef __EMSCRIPTEN__
static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}
#endif

void client_ruleset_startup(void) {
    const Ruleset *rules = client_ruleset_saved();
    ruleset_set_current(rules->id);
    g_page_rules = ruleset_get(RULESET_DEFAULT);
    printf("[ruleset] Krunker v%s (%s)\n", rules->version, rules->key);
#ifndef __EMSCRIPTEN__
    if (!rules->ui_page_dir || !rules->ui_page_dir[0]) {
        g_page_rules = rules; // ui/ itself
        return;
    }
    const char *user_dir = getenv("KRUNKNATIVE_UI_DIR");
    if (user_dir && *user_dir) return; // the user's page tree wins (tests / dev)
    char dir[512], index[600];
    snprintf(dir, sizeof(dir), "%s%s", ui_default_path(), rules->ui_page_dir);
    snprintf(index, sizeof(index), "%s/index.html", dir);
    if (!file_exists(index)) {
        fprintf(stderr, "[ruleset] warning: %s missing, the v%s page is not available: showing ui/\n", index, rules->version);
        return;
    }
    // ui/v10 is the live krunker.io page without window templates yet (ui/README.md "Krunker v10
    // page tree"): it loads, and its windows are ui/'s v7 ones (ui_windows.c) until the live-v10
    // work adds its own. Dev-facing until then.
    snprintf(index, sizeof(index), "%s/windows/windows.json", dir);
    if (!file_exists(index))
        fprintf(stderr, "[ruleset] warning: %s has no window templates: its menu windows are ui/'s (v7.1.4)\n", dir);
    set_env("KRUNKNATIVE_UI_DIR", dir);
    g_ui_env_set = 1;
    g_page_rules = rules;
#else
    // the web build serves ui/ only (dist/web leaves ui/v10 out): the page stays, the ruleset applies
    if (rules->ui_page_dir && rules->ui_page_dir[0])
        fprintf(stderr, "[ruleset] warning: the web build has no ui/%s page: showing ui/\n", rules->ui_page_dir);
    else
        g_page_rules = rules;
#endif
}

void client_ruleset_env_cleanup(void) {
#ifndef __EMSCRIPTEN__
    if (g_ui_env_set) set_env("KRUNKNATIVE_UI_DIR", NULL);
    g_ui_env_set = 0;
#endif
}

ClientRulesetResult client_ruleset_select(Client *client, const char *name) {
    if (!name) return CLIENT_RULESET_UNKNOWN;
    const char *n = name[0] == 'v' || name[0] == 'V' ? name + 1 : name;
    if (!strcmp(n, "3") || !strcmp(n, "3.1.1")) return CLIENT_RULESET_UNAVAILABLE; // TODO(v3 ruleset)
    const Ruleset *rules = ruleset_find(name);
    if (!rules) return CLIENT_RULESET_UNKNOWN;
    html_storage_set(CLIENT_RULESET_STORAGE, rules->key);
    printf("[ruleset] saved Krunker v%s (%s)\n", rules->version, rules->key);
    // between games the client's own tables follow at once; a session keeps the server's
    if (client && !client->offline && !client->net_session_connected) game_adopt_rules(&client->game, rules);
    const Ruleset *page = client_ruleset_page();
#ifdef __EMSCRIPTEN__
    (void) page;
    return CLIENT_RULESET_SAVED; // one page on the web
#else
    return page != rules && (page->ui_page_dir[0] || rules->ui_page_dir[0]) ? CLIENT_RULESET_SAVED_RELOAD : CLIENT_RULESET_SAVED;
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
    return 1;
}

void client_ruleset_before_offline(Client *client) {
    game_adopt_rules(&client->game, client_ruleset_saved()); // the sandbox's game_configure takes the current one
}
