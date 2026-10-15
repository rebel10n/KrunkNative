#pragma once
// Seek preferences: Settings > General > Quickplay (doc/v10/matchmaker-token.md "Seek
// preferences"). Krunker v10.0.2 seeks with the saved `seek_modes` / `seek_maps` (comma lists of
// mode / map indices, sent as dataQuery dsmo / dsma, v10.js:222051-222055; the matchmaker answers
// NoMatchingServers, "No Games Found / Try changing Quickplay options", when nothing matches). v10
// has no UI for them; KrunkNative stores one mode and one map (or Any) under those keys in the
// settings store, plus the server type of the native client: KrunkNative servers, the official
// krunker.io lobbies (opened in the browser, krunker_lobbies.h) or both, ours first. The server
// type only picks what Quick Match / F4 seek into: the native server browser always lists both.
// The web build always seeks KrunkNative servers and has no server type row.
#include <stddef.h>

typedef enum {
    SEEK_SERVERS_KN = 0,       // KrunkNative servers (the only choice on the web)
    SEEK_SERVERS_OFFICIAL = 1, // official krunker.io lobbies (opened in the system browser)
    SEEK_SERVERS_BOTH = 2,     // ours when one matches, else an official lobby
} SeekServers;

int seek_prefs_mode(void);          // mode id (modes.h, = Krunker's mode index), -1 = any
int seek_prefs_map(void);           // Krunker's official mapIndex (krunker_maps.h, 0..KRUNKER_OFFICIAL_MAP_COUNT-1), -1 = any
SeekServers seek_prefs_servers(void);
// Settings > Quickplay > Auto Seek on Launch (knSeekOnLaunch, default on; v10 has no such option):
// the launch seeks the way F4 does, once the menu is up, unless it names a lobby or starts offline.
int seek_prefs_on_launch(void);
const char *seek_prefs_map_name(void);  // the wanted map's name, NULL = any

// dataQuery JSON for /mm/seek-game ({"dsmo":[1],"dsma":[5]}), "" when both are Any.
void seek_prefs_data_query(char *out, size_t n);
// Does a lobby (mode id, map name) match the preferences? mode -1 / map NULL = unknown (no match
// when that preference is set).
int seek_prefs_match(int mode, const char *map_name);
// "TDM on Burg" / "any mode on any map" for messages.
void seek_prefs_describe(char *out, size_t n);

// The General settings tab (advanced view) with the Quickplay section at its end (ui_windows.c);
// NULL = tab unchanged.
char *seek_prefs_extend_tab(const char *tab);
// The basic view's General tab with the Server Type row (native) at the end of v10's own
// "Server Browser & Matchmaking" folder; NULL = tab unchanged (the web build, no such folder).
char *seek_prefs_extend_basic_tab(const char *tab);
