#pragma once
// Seek preferences: Settings > General > Quickplay (doc/v10/matchmaker-token.md "Seek
// preferences"). Krunker v10.0.2 seeks with the saved `seek_modes` / `seek_maps`: getSavedVal
// lists of mode / map indices (a "1,3" string or an array, v10.js:222011-222016) sent as the
// dataQuery arrays dsmo / dsma (v10.js:222051-222055). v10.0.2 has no page writing them; the rows
// here are KrunkNative's: one switch per map and one per mode (Settings > Quickplay, a folder each).
//
// The switches start on at Krunker's public rotation and off for everything else: the modes of v10's
// rotationModes ([0, 2, 3, 19, 25]: FFA, Hardpoint, CTF, Kill Confirmed, Domination; g_rotation_modes)
// and the maps of v10's rotationMaps (seek_filter_rotation_maps, v10.js:167210). The player's saved
// list replaces that default, switch by switch. A list with every switch off is empty: the seek then
// uses the rotation again, never an empty pool. Quick Play, the launch seek, Find New Game and our
// brain's seek send both lists (dsma, dsmo) as they are switched on, so nobody lands in Trade, a
// Parkour map or a custom game by default. The lobby-list picks (krunker_lobbies_pick, krunker_net's
// Find New Game list) and the server browser apply the same lists here.
//
// The Quickplay section starts folded (its header opens it; the state is not saved), with a row of presets
// above it (SeekPreset, seek_filter.h): each saves the lists its switches would save, then shows them.
//
// Plus the server type of the native client: Box Office servers, the official krunker.io lobbies
// (krunker_lobbies.h) or both, ours first. The server type only picks what Quick Match / F4 seek
// into: the native server browser always lists both. The web build has the same Server Type row (the
// saved choice, knSeekServers, and krunker.io's state from the bridge's relay flag, krunker_relay.h), but
// its seeks stay on Box Office servers until the krunker.io join over the relay is built.
#include <html_dom.h> // HtmlEvent / HtmlArg (seek_prefs_handle)
#include <stddef.h>

#define SEEK_PREFS_MAX 64 // entries kept of a saved list

typedef enum {
    SEEK_SERVERS_KN = 0,       // Box Office servers (the only choice on the web)
    SEEK_SERVERS_OFFICIAL = 1, // official krunker.io lobbies (joined in this client)
    SEEK_SERVERS_BOTH = 2,     // ours when one matches, else an official lobby
} SeekServers;

// Whether a seek is narrowed by a switch the player changed (the server browser shows only matching
// rows then; with the defaults every row is listed, as before).
int seek_prefs_filtering(void);
SeekServers seek_prefs_servers(void);
// Stores the Server Type (the Quickplay row's setting; the first-run choice writes it).
void seek_prefs_set_servers(SeekServers servers);
// Settings > Quickplay > Auto Seek on Launch (knSeekOnLaunch, default on; v10 has no such option):
// the launch seeks the way F4 does, once the menu is up, unless it names a lobby or starts offline.
int seek_prefs_on_launch(void);
// Settings > Quickplay > krunker.io Region (knKrunkerRegion, native): the Krunker matchmaker region
// (KRUNKER_REGIONS, krunker_build.h) official seeks ask for and Quick Match prefers, like v10's Default
// Region; "" = not chosen (the ping race's, krunker_lobbies.h krunker_seek_region).
const char *seek_prefs_krunker_region(void);

// v10's dataQuery fields, ,"dsma":[..],"dsmo":[..] (v10's order): the lists the switches seek, always
// both (the rotation when nothing is switched on). "" when out is too small.
void seek_prefs_dq_fields(char *out, size_t n);
// dataQuery JSON for our /mm/seek-game ({"dsma":[5],"dsmo":[0,2,3,19,25]}).
void seek_prefs_data_query(char *out, size_t n);
// Does a lobby (mode id, map name) match the switches? mode -1 / map NULL = unknown (no match then).
int seek_prefs_match(int mode, const char *map_name);
// Official lobby filter over krunker.io's game-list entry: mode, map name and the flags (SEEK_LOBBY_*).
int seek_prefs_lobby_ok(int mode, const char *map_name, unsigned flags);
int seek_prefs_flag(const void *value); // krunker.io's flag (true or 1)
// "Free for All, Hardpoint on Burg" / "public modes on public maps" for messages.
void seek_prefs_describe(char *out, size_t n);

// The General settings tab (advanced view) with the Quickplay section at its end (ui_windows.c);
// NULL = tab unchanged.
char *seek_prefs_extend_tab(const char *tab);
// The basic view's General tab: the Server Type / krunker.io Region rows (native) at the end of v10's
// own "Server Browser & Matchmaking" folder, then the Quickplay section; NULL = tab unchanged.
char *seek_prefs_extend_basic_tab(const char *tab);
// knSeekSwitch(key, index, checked): one mode or map switch; -1 = not ours.
int seek_prefs_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
