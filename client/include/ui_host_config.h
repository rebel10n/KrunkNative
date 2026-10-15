#pragma once
// WP-E Host Game config fields (client/src/ui/ui_host_config.c): the v7.1.4 Host Custom Game
// window's (windows[7], ui/windows/8.html) game-config parts, built from the shared option
// tables (shared/include/game_config.h) instead of v7's config.js, and read back for
// createPrivateRoom. The window itself (tabs, map cards, Start Game) is ui-v7's
// (client/src/ui/servers_ui.c): it splices these bodies into its hostTb containers and routes the
// windows.7.* handlers here first.
#include <cJSON.h>
#include <html_dom.h>
#include <stddef.h>

typedef enum {
    HOST_CONFIG_TAB_MODES = 1,    // hostTb1: modes + classes (with Toggle), host types without restrict / comp
    HOST_CONFIG_TAB_ADVANCED = 2, // the hostTb2 bodies: Weapon / Class / Effect Config, then Settings (serverConfig)
    HOST_CONFIG_TAB_SERVER = 3,   // hostTb3: Privacy (private, password)
} HostConfigTab;

// Markup for a tab's bodies (malloc'd, the caller frees). `host_type` is the config.hostTypes index
// the window was opened with (openHostWindow(false, type)); `preset` (may be NULL) is a map's
// embedded "config" or a saved preset ({modes, classes, settings, weps, cls, prks}) whose values
// the rows start from, as v7 does for pasted map data.
char *ui_host_config_tab_html(HostConfigTab tab, int host_type, const cJSON *preset);
// The same bodies without the .hide class: the Host Game flow (host_flow_ui.c) shows its own steps.
char *ui_host_config_step_html(HostConfigTab tab, int host_type, const cJSON *preset);
// windows.7.toggle / changeWep / toggleWep / changeCls / toggleCls / changeEffects / toggleEffects
// and updateSliderLabel / updateSliderLabel2 of these rows. -1: not one of them.
int ui_host_config_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
// createPrivateRoom's request from the rows: {private, passCode, modes, classes, override, every
// serverConfig varN, weps / cls / prks for the values that differ from the base tables}. Maps,
// raw map data and the community map are the caller's. NULL with `err` set when a check fails
// (no mode / no class / long password), like v7's hostGameMsg errors.
cJSON *ui_host_config_collect(int host_type, char *err, size_t err_size);
