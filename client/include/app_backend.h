#pragma once
// The backend session across the windows of one client (client/src/app_window.c). Every window is its own
// process, and the backend it plays on (backend_ui.h account_backend()) follows the server the player is
// connected to: krunker.io while an official krunker.io game is joined or played, KrunkNative's otherwise.
// The window that holds the connection knows it (krunker_net_active()); the others learn it from their
// router (the window that opened them, app_window.c):
//   router -> child   "backend krunker" / "backend krunkio" whenever the router's session changes, and the
//                     KRUNKNATIVE_APP_BACKEND=krunker|krunkio environment variable at spawn
//   child  -> router  "backend krunker" / "backend krunkio" when the child's own connection changes
// This module only holds that state and the wire form; it does no I/O.
#define APP_BACKEND_ENV "KRUNKNATIVE_APP_BACKEND"

// A router's child: the router's session (1 = official, 0 = not). -1 (default) = no router, so nothing
// but this window's own connection counts.
void app_backend_set_router(int official);
// A router: how many of its child windows report an official krunker.io connection.
void app_backend_set_children_official(int count);
// 1 = a window of this client's tree is connected to an official krunker.io server through a router or a
// child. The caller adds its own connection (krunker_net_active()).
int app_backend_official_session(void);

// The wire form. app_backend_parse_line: 1 = "backend krunker", 0 = "backend krunkio", -1 = not one.
int app_backend_parse_line(const char *line);
const char *app_backend_line(int official);     // "backend krunker" / "backend krunkio"
// The environment value at spawn: "krunker" / "krunkio"; app_backend_parse_env: 1 / 0, -1 = not one.
const char *app_backend_env_value(int official);
int app_backend_parse_env(const char *value);
