// The v10.0.4 map loading screen (#loadingBg): the card a match shows while its map loads, with the
// map thumbnail, the map and mode names and the mode's accent. Krunker v10.0.4 (deob-10.0.4.js:223011-223041,
// the load in 223018-223041, the hide in 223011-223014). The timing is GL- and DOM-free so
// client/tests/v10_loading_test.c can check it; v10_loading.c writes the elements by id and does nothing
// for an id the page lacks (see the markup note there).
#pragma once

// loadInTime (deob-10.0.4.js:16549, 0x7d0 = 2000 ms): the card holds this long after a map load that is not the
// session's first (iíÌiiïî = loads++ > 0, :223024-223025). The first load shows no card (the boot curtain covers it).
#define V10_LOADING_HOLD_MS 2000.0
// the fade: the card gets class loadingOut, then display none after 300 ms (:223011-223014, 0x12c)
#define V10_LOADING_FADE_MS 300.0

typedef struct {
    const char *map_thumb; // "./img/maps/map_<n>.png"; NULL: no thumbnail (a custom map, or map 0 that is not Burg)
    const char *map_name;  // loadingMapName (map.name)
    const char *mode_name; // loadingMapMode (mode.name, uppercased by the page); NULL: hidden
    const char *accent;    // --load-accent (mode.tag[1]); NULL: "#fff"
} V10LoadingMap;

// The hold a map load gets: 0 for the session's first load, V10_LOADING_HOLD_MS after.
double v10_loading_hold_ms(int loads_before);

// --load-accent for a mode: mode.tag[1] when the mode has a tag and the game is not custom, else "#fff"
// (deob-10.0.4.js:223040). The tags of the mode list (deob-10.0.4.js:105931-106488): Party #ffac00 (Gun Game,
// Classic FFA, One in the Chamber, Sharp Shooter, Deposit FFA, Chaos Snipers, Bighead FFA, Impulsed), Event
// #d95487 (Stalker, Raid, All or Nothing, Mag Dump), Candy Heist #ff8a1a; KrunkNative's mode ids are that list's
// indices (shared/include/modes.h). An unknown or negative mode id: "#fff".
const char *v10_loading_mode_accent(int mode_id, int is_custom);

// A map load begins (the INIT of a match): counts it, shows the card when it is not the first, and schedules
// the hide at now + hold. A newer load replaces an older one's schedule (the generation check, :223011-223013).
void v10_loading_begin(const V10LoadingMap *map, double now_ms);

// Per frame: starts the fade when the hold is over, and removes the card after the fade.
void v10_loading_tick(double now_ms);

// Test hooks: the state the page would show.
int v10_loading_visible(void);
void v10_loading_reset(void);
