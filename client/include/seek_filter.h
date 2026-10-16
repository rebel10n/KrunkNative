#pragma once
// The seek filter's pure core (seek_prefs.h: what Quick Play, the launch seek and Find New Game may
// pick): no settings store, no page, so client/tests/seek_filter_test.c runs it alone.
#include <cJSON.h>
#include <stddef.h>

#define SEEK_FILTER_MAX 64

// What a seek looks for. The player switches each mode and map on or off (Settings > Quickplay);
// the lists here are the switched-on ones, or the public rotation when none is on (an empty choice
// never sends an empty pool).
typedef struct {
    int modes[SEEK_FILTER_MAX];
    int mode_count;
    int modes_chosen;                       // 1 = the player switched a mode on (hosted lobbies need one)
    int maps[SEEK_FILTER_MAX];              // official mapIndexes (dsma)
    const char *map_names[SEEK_FILTER_MAX]; // their names, parallel to maps (NULL = unknown)
    int map_count;
} SeekFilter;

// Lobby flags (krunker.io's game-list data; true or 1 both count, seek_filter_flag)
#define SEEK_LOBBY_CUSTOM 1u   // data.c / cs: a custom game
#define SEEK_LOBBY_PASSWORD 2u // data.pw
#define SEEK_LOBBY_HOSTED 4u   // data.oc (official custom) / data.ds (dedicated, rented 24/7)

// v10's _69484 (v10.js:222011-222016) over a saved value: a "3,5" string split at commas, an array,
// a number; each entry a whole number for which valid() holds (NULL = any >= 0), duplicates dropped.
int seek_filter_parse(const cJSON *saved, int *out, int max, int (*valid)(int));
int seek_filter_flag(const cJSON *value); // krunker.io sends its flags as true or 1
// Krunker's public map rotation (v10.0.2 rotationMaps, v10.js:167210): official mapIndexes, the maps a
// seek with no map switched on looks for. Returns the count; *out the list.
size_t seek_filter_rotation_maps(const int **out);
// The list a seek uses: the switched-on entries of `on` (the saved list, valid ids), or `defaults`
// when none is on. Returns the count written to out (at most max).
int seek_filter_effective(const int *on, int on_count, const int *defaults, int default_count, int *out, int max);
// One switch flipped (Settings > Quickplay): the effective list (above) with id added (checked) or
// removed, in ascending order, into out. Returns the count (0 = every switch off: the rotation is sought).
int seek_filter_toggle(const int *on, int on_count, const int *defaults, int default_count, int id, int checked, int *out, int max);
// The mode is one the seek looks for (the switched-on modes, or the rotation).
int seek_filter_mode_ok(const SeekFilter *f, int mode);
// The lobby may be picked: the mode (above), a picked map (names compared without case), never a
// custom game or a password lobby, a hosted one only for a mode the player switched on.
int seek_filter_lobby_ok(const SeekFilter *f, int mode, const char *map_name, unsigned flags);
// v10's dataQuery fields, ,"dsma":[..],"dsmo":[..] (v10's order): the effective lists, always both
// (an empty choice falls back to the rotation, so the fields are never empty lists). "" when f is NULL.
void seek_filter_dq_fields(const SeekFilter *f, char *out, size_t n);

// The list as the switches save it (Settings > Quickplay): "1,2,19", ascending as given; "" for an empty
// list, which the seek reads as the rotation. Returns the length written (0 when out is too small).
size_t seek_filter_list_text(const int *list, int count, char *out, size_t n);

// Quickplay presets (seek_prefs.c, Settings > Quickplay): fixed sets of switches. Applying one saves the
// lists the switches save (ascending, "" = the rotation), so the switches show it and each can still be
// changed. Keys (kn.quickplay.preset.<key>) and what each switches on:
//   rotation  Krunker's public rotation: both lists empty, the default (every switch off)
//   team      Team Deathmatch, Hardpoint, Capture the Flag, Kill Confirmed, Domination, Squad Deathmatch
//             (modes 1, 2, 3, 19, 25, 26) on the rotation maps
//   ffa       Free for All, Classic FFA, Gun Game, Last Man Standing, Kranked FFA, Bighead FFA
//             (modes 0, 13, 10, 8, 27, 34) on the rotation maps
//   ranked    Hardpoint (mode 2, the mode of Ranked) on the Ranked map pool (g_ranked_map_pool)
//   any       every mode and every official map
#define SEEK_PRESET_COUNT 5
typedef enum {
    SEEK_PRESET_ROTATION = 0,
    SEEK_PRESET_TEAM = 1,
    SEEK_PRESET_FFA = 2,
    SEEK_PRESET_RANKED = 3,
    SEEK_PRESET_ANY = 4,
} SeekPreset;

typedef struct {
    int modes[SEEK_FILTER_MAX];
    int mode_count; // 0 = the rotation's modes
    int maps[SEEK_FILTER_MAX];
    int map_count;  // 0 = the rotation's maps
} SeekPresetLists;

// The preset's switched-on lists, ascending. Returns 0 when preset is out of range.
int seek_preset_lists(int preset, SeekPresetLists *out);
// The preset's key ("rotation", "team", ...): its i18n key suffix; NULL when out of range.
const char *seek_preset_key(int preset);
