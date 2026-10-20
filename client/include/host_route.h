#ifndef KN_HOST_ROUTE_H
#define KN_HOST_ROUTE_H

// Where Start Game hosts the game (host_flow_ui.c): krunker.io's custom game (CUSTOM_SERVER over the official
// connection, v10's createPrivateRoom) or Box Office's brain (/mm/host-game). Pure, so the rule is testable.
//
//   tab_backend:          the backend the Host window was opened with from a Servers tab (1 krunker.io, 0 Box
//                         Office), -1 when it was opened from the chooser (no tab)
//   server_type_official: the Server Type setting is krunker.io (seek_prefs_servers() == SEEK_SERVERS_OFFICIAL)
//   krunker_active:       an official krunker.io game is the session (krunker_net_active())
//
// The Server Type decides a chooser-opened window: krunker.io's Server Type hosts on krunker.io, the Box Office
// setting on Box Office's brain. An official game the player is in still hosts there (the brain is gated while
// krunker.io is the session, so it could not host in that game anyway).
static inline int host_route_krunker(const int tab_backend, const int server_type_official, const int krunker_active) {
    if (tab_backend >= 0) return tab_backend != 0;
    return server_type_official != 0 || krunker_active != 0;
}

#endif
