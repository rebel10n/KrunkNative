#ifndef KN_MENU_SOURCES_H
#define KN_MENU_SOURCES_H

// Which backend feeds the main menu's Popular Now (v10 fetchMatches, v10_menu.c / servers_ui.c) and its Live
// Streams overlay (v10 fetchTopStreams, v10_menu.c): the Server Type's (Settings > Quickplay, seek_prefs.h). A
// Box Office menu shows Box Office's games and asks the brain, never krunker.io; the krunker.io Server Type
// shows krunker.io's games and asks krunker.io's hosts, as v10 does. Pure, so the rule is testable
// (client/tests/menu_sources_test.c).
//
//   server_type:    seek_prefs_servers()
//   krunker_active: krunker.io is the active account backend (account_backend() == ACCOUNT_BACKEND_KRUNKER): an
//                   official krunker.io game is the session. The brain is gated then (backend.h), so Box Office's
//                   lists cannot be asked and krunker.io's are the only ones that answer, whatever the Server Type
//   brain:          backend_available(): 1 = a brain answers, 0 = none, -1 = still probing
#include <seek_prefs.h> // SeekServers

enum { MENU_SRC_KN = 1, MENU_SRC_KIO = 2 };

// Popular Now's lists, MENU_SRC_* bits: Box Office = the brain's custom games only, krunker.io = krunker.io's
// custom games only, Both = the two summed (a Both seek lands on either, so a map on both counts both).
static inline unsigned menu_popular_sources(const SeekServers server_type, const int krunker_active) {
    if (krunker_active || server_type == SEEK_SERVERS_OFFICIAL) return MENU_SRC_KIO;
    if (server_type == SEEK_SERVERS_BOTH) return MENU_SRC_KN | MENU_SRC_KIO;
    return MENU_SRC_KN;
}

// Live Streams' host: MENU_SRC_KIO = api.krunker.io's /streams/top (v10's data client, krunker_kio_routes.h),
// MENU_SRC_KN = the brain's /streams/top (services/brain/streams.c), 0 = nobody to ask (the overlay stays hidden).
// Box Office asks the brain; krunker.io (the Server Type, or an official game being the session) asks krunker.io;
// Both asks the brain while there is one, else krunker.io.
static inline unsigned menu_streams_source(const SeekServers server_type, const int krunker_active, const int brain) {
    if (krunker_active || server_type == SEEK_SERVERS_OFFICIAL) return MENU_SRC_KIO;
    if (brain == 1) return MENU_SRC_KN;
    return server_type == SEEK_SERVERS_BOTH ? MENU_SRC_KIO : 0;
}

#endif
