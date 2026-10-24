#pragma once
// v10.0.3 map prize wheels on the client (client/src/prize_wheels.c): MapManager.spinners (the map's PRIZE_WHEEL objects,
// MapManager.addSpinner deob-10.0.3.js:186539, shared/include/map_spinners.h) and their spinInfo, which "spinW"
// (SPIN_WHEEL_RESPONSE, NET_EXT_SPIN_WHEEL_RESPONSE) starts (game.updateSpinner :219408) and game.updateSpinners
// (:219434) runs every 50 ms. The wheel canvas is client/src/prize_wheel_render.c (painted from this state); its ticks
// and the end's reward / cheer / explosion sounds play at the wheel; prize_wheel_render.c also draws the won item's
// model (meshPrev) from item_shown and bob_anim.
#include <client.h>

typedef struct {
    int object;           // map object index
    int wheel;            // store.wheels index (parseInt(sty) || 0, 0..2)
    vec3 position;        // meshRef.position: the object's bottom centre + half its height
    float spin_whl_mlt;   // spinInfo.spinWhlMlt: the wheel's grow-in, 0..1
    float end_anim_target, end_anim_val; // the won slice's widening once it stops
    float spin_val, spin_rotation, tick_counter; // the pointer's target, where it is, the tick sound's phase
    float rand_num;       // spinInfo.randNum
    int item_id;          // spinInfo.itemID: the item won, -1 none
    int won_rarity;       // store.skins[itemID].rarity at the stop, -1 none
    int item_shown;       // meshPrev: the stop put the won item up (a rarity wheel's; the canvas loses its SPIN label)
    float bob_anim;       // spinInfo.bobAnim: meshPrev's bob
    int needs_update;     // spinInfo.needsUpdate: the canvas is painted at each step while set
    unsigned paints;      // updateSpinners steps that painted the canvas (prize_wheel_render.c follows it)
} PrizeWheel;

// "spinW": spinners[uid] spins to rand and wins skin (updateSpinner). 0 = no such spinner.
int prize_wheels_spin(Client*, int uid, float rand, int skin);
// Per frame: the spinner list follows the client's map; 50 ms updateSpinners steps.
void prize_wheels_tick(Client*);
// INIT: the list is rebuilt from the next map.
void prize_wheels_reset(void);
// The spinner of uid (tests, the wheel renderer), NULL none.
const PrizeWheel *prize_wheels_get(const Client*, int uid);
int prize_wheels_count(const Client*);
