// Startup glue between main.c and the brain client (client/include/backend_ui.h):
// command line (--brain, --connect), the brain-or-direct decision and the per-frame
// pump. Kept out of main.c so the backend stays one include + three calls there.
#include <remote_images.h>
#include "klog.h"
#include <v10_social.h>
#include <backend.h>
#include "backend_internal.h" // backend_probe_tick (backend_common.c)
#include <kn_captcha.h>        // Box Office's captcha: kn_captcha_tick
#include <backend_ui.h>
#include <strike_director.h> // Krunker Strike: the director matchmakes
#include <strike_econ.h>     // Krunker Strike: the account's Strike economy state
#include <online_mode.h>
#include <krunker_net.h>
#include <krunker_account.h>
#include <krunker_ranked.h>
#include <krunker_lobbies.h>
#include <krunker_relay.h>   // the web build reads the bridge's krunker.io relay flag once
#include <v10_menu.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static const char *g_cli_brain;
static const char *g_cli_connect;
static const char *g_cli_mode, *g_cli_finder; // --mode online|offline, --finder <url> (online_mode.h)
static int g_hub_page; // --hub / --social / social.html: the hub needs no game server with a brain

void backend_parse_args(int *argc, char **argv) {
    int out = 1;
    for (int i = 1; i < *argc; i++) {
        if (!strcmp(argv[i], "--hub") || !strcmp(argv[i], "--social")) g_hub_page = 1;
        if ((!strcmp(argv[i], "--mode") || !strcmp(argv[i], "--finder")) && i + 1 < *argc) {
            if (argv[i][2] == 'm') g_cli_mode = argv[i + 1];
            else g_cli_finder = argv[i + 1];
            i++;
            continue;
        }
        if ((!strcmp(argv[i], "--brain") || !strcmp(argv[i], "--connect")) && i + 1 < *argc) {
            if (argv[i][2] == 'b') g_cli_brain = argv[i + 1];
            else g_cli_connect = argv[i + 1];
            i++;
            continue;
        }
        argv[out++] = argv[i];
    }
    *argc = out;
    argv[out] = NULL;
}

int backend_startup(Client *client, const int offline, const int scenario) {
    krunker_net_attach(client); // KN_KRUNKER_OFFICIAL (official krunker.io games, krunker_net.h)
    krunker_relay_probe();      // web: the bridge's krunker.io relay flag (krunker_relay.h); native: nothing
    krunker_lobbies_attach(client);
    remote_images_init(); // the HTML engine's http(s) images (remote_images.h)
    // --connect host[:port]: a direct server, no matchmaking (the brain still serves
    // the account and the hub when configured)
    int direct = 0;
    if (g_cli_connect && g_cli_connect[0]) {
        char host[sizeof(client->net_address)];
        snprintf(host, sizeof(host), "%s", g_cli_connect);
        char *colon = strrchr(host, ':');
        if (colon) {
            const long port = strtol(colon + 1, NULL, 10);
            if (port > 0 && port < 65536) client->net_port = (unsigned short) port;
            *colon = 0;
        }
        if (host[0]) snprintf(client->net_address, sizeof(client->net_address), "%s", host);
        direct = 1;
    }
    if (getenv("KRUNKNATIVE_SERVER_PORT")) direct = 1;

    if (scenario) {
        // --render-scenario (uidiff) renders the guest page: never touch a backend
        servers_ui_init(client, 0);
        return 0;
    }
    online_mode_init(g_cli_mode, g_cli_finder); // offline mode: no account service at all
    backend_init(online_mode_online() ? g_cli_brain : "");
#ifdef __EMSCRIPTEN__
    direct |= EM_ASM_INT({
        try { return new URLSearchParams(location.search).get('ws') ? 1 : 0; } catch (e) { return 0; }
    });
    g_hub_page |= EM_ASM_INT({ return /social([.]html)?$/.test(location.pathname) ? 1 : 0; });
#else
    // the hub / editor windows are separate processes (app_window.c): they inherit an
    // explicit URL; a default one (local config) is probed by each process itself
    if (backend_enabled() && !backend_url_is_default()) {
#ifdef _WIN32
        _putenv_s("KRUNKNATIVE_BRAIN_URL", backend_url());
#else
        setenv("KRUNKNATIVE_BRAIN_URL", backend_url(), 1);
#endif
    }
#endif
    backend_probe_start(); // a default URL (page origin / local config) needs a kn_brain answer first
    account_ui_init(client);
    if (backend_enabled() && !backend_url_is_default() && g_hub_page && !direct) {
        // the hub talks to the brain's social socket: no game server, no lobby slot (a
        // default URL may have no brain behind it: the guest hub needs the game connection)
        servers_ui_init(client, 0);
        return 1;
    }
    // the hub never takes a lobby slot; with a default URL it keeps the classic connection for
    // the guest stubs in case no brain answers the probe
    const int brain_mode = backend_enabled() && !offline && !direct && !g_hub_page;
    servers_ui_init(client, brain_mode);
    if (brain_mode) KN_LOG_DEBUG1("[mm] matchmaking through %s\n", backend_url());
    // Krunker Strike with a director (strike_director.h): it matchmakes, also without a brain; like
    // brain_mode, the transport starts once it answers
    const int director_mode = strike_director_enabled() && !offline && !direct && !g_hub_page;
    servers_ui_set_director_mode(director_mode);
    return brain_mode || director_mode;
}

void backend_frame(Client *client) {
    backend_poll();
    kn_captcha_tick(); // Box Office's captcha: a slice of its proof of work (kn_captcha.h)
    backend_probe_tick(); // the brain probe: a timed-out one counts as failed, a due retry starts
    online_mode_tick(client);
    account_ui_tick(client);
    v10_rewards_tick(); // v10.0.2: the sidebar's Battle Pass item (v10_rewards.c)
    servers_ui_tick(client);
    krunker_account_tick();   // the krunker.io account (krunker_account.h): sign-in / refresh answers
    krunker_ranked_tick();    // the Ranked queue socket on krunker.io (krunker_ranked.h): its frames
    v10_menu_account_tick();   // the menu header: the active account (backend_ui.h), at once after a switch
    krunker_net_tick(client); // official krunker.io games: notices, captcha
    v10_social_tick(); // the v10 page's Social hub: friends event bus, menu badge (v10_social.c)
    strike_econ_tick(); // Krunker Strike: the signed-in account's Strike wallet / items / loadout (strike_econ.c)
}
