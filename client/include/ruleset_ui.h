#pragma once
// The client's Krunker version (version toggle, doc/v10/version-toggle.md; shared/include/ruleset.h).
// Settings > General "Game version" (v3.1.1 / v7.1.4 / v10.0.2) and the console `ruleset` save the
// choice in the `knRuleset` storage entry. It picks:
//   - the offline sandbox's ruleset (the in-process server plays it; client_ruleset_before_offline),
//   - quick join's ruleset (seek ?ruleset=, the brain only picks lobbies of that version),
//   - the menu page tree at startup: ui/<Ruleset.ui_page_dir>/ when its index.html exists (natively;
//     the web build keeps its page: dist/web serves ui/ only), else ui/ with a warning.
// Online the server decides: INIT carries its ruleset and the client adopts it for that session
// (client_ruleset_on_init); a ruleset this build does not have is refused with a message.
// v3.1.1 is listed as "coming soon" and cannot be picked (RULESET_V3_1_1 is not built yet).
#include <client.h>
#include <ruleset.h>

#define CLIENT_RULESET_STORAGE "knRuleset"

// The saved choice (RULESET_DEFAULT when nothing / an unbuilt one is saved). Never NULL.
const Ruleset *client_ruleset_saved(void);
// main.c, before game_configure: the process ruleset = the saved one; natively also the page tree
// (KRUNKNATIVE_UI_DIR, unless the user set it) when ui/<dir>/index.html exists.
void client_ruleset_startup(void);
// The ruleset whose page tree this process loaded (the saved one at startup).
const Ruleset *client_ruleset_page(void);
// Relaunch: drop the KRUNKNATIVE_UI_DIR this process set (the next one reads the setting again).
void client_ruleset_env_cleanup(void);
// 1 when the saved Game version wants another menu page tree than the one loaded and a restart
// would load it (natively; the user's KRUNKNATIVE_UI_DIR and a missing ui/<dir> keep the page).
// F5 (game_reload.c) restarts for it.
int client_ruleset_page_pending(void);

typedef enum {
    CLIENT_RULESET_UNKNOWN = 0,     // not a ruleset name
    CLIENT_RULESET_UNAVAILABLE = -1, // known but not built (v3.1.1)
    CLIENT_RULESET_SAVED = 1,       // saved; applies to the next offline game / quick join
    CLIENT_RULESET_SAVED_RELOAD = 2, // saved, and its menu page differs from the one shown: a restart loads it
} ClientRulesetResult;

// Save `name` ("v7", "10.0.2", ...). Offline (or not connected) the client's game adopts it now.
ClientRulesetResult client_ruleset_select(Client *client, const char *name);
// INIT: adopt the server's ruleset (net_ruleset_from_wire). 0 = this build lacks it: the client
// disconnects with a message and the INIT must not be applied.
int client_ruleset_on_init(Client *client, const NetInitPacket *init);
// local_game_start: the sandbox plays the saved ruleset.
void client_ruleset_before_offline(Client *client);

// Krunker v1.5.6: a menu page only (ui/v156/, client/src/ruleset_page_v156.c). knRuleset "v156"
// keeps v7.1.4 gameplay (v1.5.6's tables are not ported; no RulesetId, nothing on the wire).
#define CLIENT_PAGE_V156_KEY "v156"
int client_page_v156_is_name(const char *name); // "v156" / "1.5.6" / "v1.5.6"
int client_page_v156_saved(void);               // knRuleset is "v156"
int client_page_v156_loaded(void);              // this process shows ui/v156/
int client_page_v156_startup(void);             // client_ruleset_startup: 1 = v156 saved (ui/v156/ or ui/)
void client_page_v156_env_cleanup(void);        // relaunch: drop the KRUNKNATIVE_UI_DIR it set
int client_page_v156_pending(void);             // a restart would load or leave ui/v156/ (client_ruleset_page_pending)
ClientRulesetResult client_page_v156_select(Client *client); // save; UNKNOWN on the web
void client_page_v156_game_notice(const Ruleset *rules);     // first game under the v156 page: say what it plays
