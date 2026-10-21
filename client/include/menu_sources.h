#ifndef KN_MENU_SOURCES_H
#define KN_MENU_SOURCES_H

// Which backend feeds the main menu's Popular Now (v10 fetchMatches, v10_menu.c / servers_ui.c) and its Live
// Streams overlay (v10 fetchTopStreams, v10_menu.c). Pure, so the rule is testable (client/tests/menu_sources_test.c).
//
//   server_type:       seek_prefs_servers() (Settings > Quickplay > Server Type)
//   krunker_active:    krunker.io is the active account backend (account_backend() == ACCOUNT_BACKEND_KRUNKER): an
//                      official krunker.io game is the session. The brain is gated then (backend.h)
//   krunker_signed_in: a krunker.io account is signed in AND an official game is the session (account_split_krunker_identity():
//                      on a Box Office server a krunker.io sign-in chooses no host and supplies no Bearer)
//   brain:             backend_available(): 1 = a brain answers, 0 = none, -1 = still probing
#include <seek_prefs.h> // SeekServers

// Popular Now is Box Office's: shown and filled only on the Box Office Server Type, from the brain's custom games.
// krunker.io's custom games are never in it, whatever the Server Type (the krunker.io and Both types hide it).
static inline int menu_popular_on(const SeekServers server_type) { return server_type == SEEK_SERVERS_KN; }

enum { MENU_SRC_KN = 1, MENU_SRC_KIO = 2 };

// Live Streams' host: MENU_SRC_KIO = api.krunker.io's /streams/top (v10's data client, krunker_kio_routes.h),
// MENU_SRC_KN = the brain's /streams/top (services/brain/streams.c), 0 = no host (the overlay stays hidden).
// krunker.io's host: a krunker.io Server Type, an official game being the session, or a signed-in krunker.io account.
// Otherwise the brain while it answers; Both without a brain asks krunker.io's; Box Office without a brain asks nobody
// (an offline client makes no krunker.io request).
static inline unsigned menu_streams_source(const SeekServers server_type, const int krunker_active, const int krunker_signed_in,
                                           const int brain) {
    if (krunker_active || krunker_signed_in || server_type == SEEK_SERVERS_OFFICIAL) return MENU_SRC_KIO;
    if (brain == 1) return MENU_SRC_KN;
    return server_type == SEEK_SERVERS_BOTH ? MENU_SRC_KIO : 0;
}

#endif
