// v10.0.3 map prize wheels on the client (client/include/prize_wheels.h). Line numbers: deob-10.0.3.js.
//   the list   map generate (:189053-189066): a PRIZE_WHEEL whose store.wheels[limitMM(parseInt(sty) || 0, 0, 2)] is
//              missing, ticketOnly, hidden or locked is skipped; the others are MapManager.addSpinner (:186539) in
//              object order, uid = the spinner's place (shared/include/map_spinners.h), the mesh at the object's
//              bottom centre + sy / 2
//   "spinW"    game.updateSpinner(uid, randNum, itemID) (:219408): the pointer heads for 2 pi (randNum / 100 + 13) + pi / 2
//   the steps  game.updateSpinners(150) every 50 ms (setInterval(updateSpinners(iìiïìíi * 3), iìiïìíi), :262717):
//              the wheel grows in, the pointer eases 5 % of the way per step with a tick_0 (0.2) every pi / 1.5, and
//              once within 0.002: endAnimTarget = 100 - wheel.rarities[item.rarity], reward (0.3), cheer_1 (rarity >= 4)
//              or cheer_0 (>= 1) at 0.1, an explosion (1) above rarity 5, meshPrev (the won item) put up (:219470-219574);
//              then, while needsUpdate, the canvas is painted (client/src/prize_wheel_render.c) until the won slice
//              is wide (meshPrev) or the idle wheel has grown in (:219576-219675)
// meshPrev's model is drawn from item_shown / bob_anim by prize_wheel_render.c. Above rarity 5 (an Unobtainable) the
// stop also shakes the screen (render.shake(0.14)) and explodes at meshPrev (the explosion sound, ExplosionManager
// explodeAt(.., 80) with its default 15 small / 8 large pieces, :219566-219574, :225603-225604). A pool wheel (store data with pools and no rarities, like the Starter /
// Heroic wheels of the 2026-10-13 data, shared/include/store_spins.h) throws in v10 at `wheel.rarities[rarity]`: that
// step stops there (no sound, no paint, the spinners after it wait for the next step), and the wheel goes on painting
// its stopped pointer with the SPIN label: the same here.
#include <prize_wheels.h>
#include <map_spinners.h>
#include <audio.h>
#include <store_spins.h>
#include <klog.h>
#include "viewer/viewer_store.h" // store.skins: the item's rarity
#include <prize_wheel_geom.h> // meshPrev's world position (an Unobtainable's explosion)
#include <camera_feel.h>      // render.shake
#include <vfx.h>              // ExplosionManager.explodeAt
#include <math.h>
#include <string.h>

#define PRIZE_WHEELS_MAX 64          // map_validate's MAP_V7_WHEEL_LIMIT is far below
#define PRIZE_WHEELS_STEP_S 0.05     // iìiïìíi = Math.round(50) ms (:19243)
#define PRIZE_WHEELS_STEP_DELTA 150.0f // iìiïìíi * 3
#define PRIZE_WHEELS_MAX_STEPS 10    // a long frame catches up this many steps at most

static struct {
    const Map *map; // the map the list was built for, NULL: build it
    int count;
    PrizeWheel wheels[PRIZE_WHEELS_MAX];
    double last_s;  // the step clock, < 0 not started
    double carry_s;
} g_pw = {.last_s = -1.0};

void prize_wheels_reset(void) {
    g_pw.map = NULL;
    g_pw.count = 0;
    g_pw.last_s = -1.0;
    g_pw.carry_s = 0.0;
}

static void build(const Map *map) {
    g_pw.map = map;
    g_pw.count = 0;
    for (size_t i = 0; map && i < map->object_count && g_pw.count < PRIZE_WHEELS_MAX; i++) {
        const Object *o = map->objects[i];
        const int sty = map_spinner_wheel(o);
        if (sty < 0) continue;
        PrizeWheel *p = &g_pw.wheels[g_pw.count++];
        memset(p, 0, sizeof(*p));
        p->object = (int) i;
        p->wheel = sty;
        p->position = (vec3) {o->position.x, o->position.y + o->scale.y / 2.0f, o->position.z};
        p->item_id = -1; // spinInfo ||= {..., itemID: -1, needsUpdate: true} (:219439-219451)
        p->won_rarity = -1;
        p->needs_update = 1;
    }
    if (g_pw.count) KN_LOG_DEBUG1("[prize] %d map prize wheel(s)\n", g_pw.count);
}

static void ensure(const Client *client) {
    const Map *map = client && client->game.ready ? client->game.map : NULL;
    if (map != g_pw.map) build(map);
}

int prize_wheels_count(const Client *client) {
    ensure(client);
    return g_pw.count;
}

const PrizeWheel *prize_wheels_get(const Client *client, const int uid) {
    ensure(client);
    return uid >= 0 && uid < g_pw.count ? &g_pw.wheels[uid] : NULL;
}

int prize_wheels_spin(Client *client, const int uid, const float rand, const int skin) {
    ensure(client);
    if (uid < 0 || uid >= g_pw.count) return 0;
    PrizeWheel *w = &g_pw.wheels[uid];
    w->end_anim_target = 0.0f;
    w->end_anim_val = 0.0f;
    w->spin_val = (float) (M_PI * 2.0 * (rand / 100.0 + 13.0) + M_PI / 2.0);
    w->tick_counter = 0.0f;
    w->spin_rotation = 0.0f;
    w->bob_anim = 0.0f;
    w->rand_num = rand;
    w->item_id = skin;
    w->won_rarity = -1;
    w->item_shown = 0; // meshPrev removed
    w->needs_update = 1;
    KN_LOG_DEBUG1("[prize] wheel %d spins to %g: item %d\n", uid, (double) rand, skin);
    return 1;
}

// The stop (:219477-219572): the won slice widens, the reward sounds, the won item goes up. 0 where v10 throws (no
// such item, a wheel without rarities): the rest of that step does not run.
// meshPrev.getWorldPosition() right after the stop put it up (no bob yet): the item's pose in the wheel's frame
static vec3 item_world(const PrizeWheel *w, const ViewerSkin *item) {
    const Object *o = g_pw.map && w->object >= 0 && (size_t) w->object < g_pw.map->object_count ? g_pw.map->objects[w->object] : NULL;
    if (!o) return w->position;
    const ViewerPreview *preview = viewer_preview(item); // iîIïîïì = item.type ? item : store.getPreview(item)
    PrizeWheelItemPose pose;
    prize_wheel_item_pose(o->scale.y, map_spinner_dir(o), item->type, item->type ? item->y_off : preview->y_off,
                          item->type ? item->x_off : preview->x_off, item->type ? item->scl : preview->scl, item->spn_scl, &pose);
    float at[3];
    prize_wheel_item_world((const float[3]) {w->position.x, w->position.y, w->position.z}, &pose, at);
    return (vec3) {at[0], at[1], at[2]};
}

static int stop(PrizeWheel *w, Client *client) {
    w->spin_rotation = w->spin_val;
    const ViewerSkin *item = viewer_skin(w->item_id);
    const StoreSpins *spins = store_spins_builtin();
    const StoreWheel *wheel = w->wheel < spins->wheel_count ? &spins->wheels[w->wheel] : NULL;
    if (!item || !wheel || wheel->pool_count > 0) return 0; // iiÌiiii.rarity / iìÏîìiì.rarities[...] of undefined
    const int rarity = item->rarity;
    w->end_anim_target = 100.0f - (rarity >= 0 && rarity < wheel->slices ? (float) wheel->weights[rarity] : 0.0f);
    w->won_rarity = rarity;
    audio_play_3d("reward", w->position, 0.3f, 1.0f, 0.0f, 0.0f);
    if (rarity >= 4) audio_play_3d("cheer_1", w->position, 0.1f, 1.0f, 0.0f, 0.0f);
    else if (rarity >= 1) audio_play_3d("cheer_0", w->position, 0.1f, 1.0f, 0.0f, 0.0f);
    w->item_shown = 1; // meshPrev = genObj3D(...) (:219494)
    if (rarity > 5) { // an Unobtainable (:219566-219574)
        camera_feel_shake(0.14f);                    // iíÍïîíí.shake(0.14)
        const vec3 at = item_world(w, item);         // meshPrev.getWorldPosition()
        audio_play_3d("explosion", at, 1.0f, 1.0f, 0.0f, 0.0f);
        vfx_explode_at(client, at, 80.0f, 15, 8);    // explodeAt(x, y, z, 80): smallSpawnCount 15, largeSpawnCount 8
        KN_LOG_DEBUG1("[prize] item %d: an Unobtainable explodes at %.1f %.1f %.1f\n", w->item_id, (double) at.x, (double) at.y, (double) at.z);
    }
    return 1;
}

static void step(const float delta, Client *client) {
    for (int i = 0; i < g_pw.count; i++) {
        PrizeWheel *w = &g_pw.wheels[i];
        if (w->spin_whl_mlt < 1.0f) {
            w->spin_whl_mlt += delta * 0.008f;
            if (w->spin_whl_mlt >= 1.0f) w->spin_whl_mlt = 1.0f;
        }
        if (w->end_anim_val < w->end_anim_target) {
            w->end_anim_val += delta * 0.4f;
            if (w->end_anim_val >= w->end_anim_target) w->end_anim_val = w->end_anim_target;
        }
        if (w->spin_rotation != w->spin_val) {
            w->spin_rotation += (w->spin_val - w->spin_rotation) * 0.05f;
            w->tick_counter -= (w->spin_val - w->spin_rotation) * 0.05f;
            if (w->tick_counter <= 0.0f) {
                w->tick_counter += (float) (M_PI / 1.5);
                audio_play_3d("tick_0", w->position, 0.2f, 1.0f, 0.0f, 0.0f);
            }
            if (w->spin_val - w->spin_rotation <= 0.002f && !stop(w, client)) return; // the throw ends the step (:219482)
        }
        if (w->item_shown) w->bob_anim += delta * 0.0018f; // meshPrev.position.y = orgYP + sin(bobAnim) * 0.55 (:219576)
        if (!w->needs_update) continue;
        w->paints++; // the canvas paint (:219577-219668)
        if (w->item_shown && w->end_anim_val == w->end_anim_target) w->needs_update = 0;
        if (!w->item_shown && w->item_id == -1 && w->spin_whl_mlt == 1.0f) w->needs_update = 0;
    }
}

void prize_wheels_tick(Client *client) {
    ensure(client);
    const double now = client ? client_death_clock(client) : 0.0;
    if (!g_pw.count || g_pw.last_s < 0.0 || now < g_pw.last_s) {
        g_pw.last_s = now;
        g_pw.carry_s = 0.0;
        return;
    }
    g_pw.carry_s += now - g_pw.last_s;
    g_pw.last_s = now;
    int steps = 0;
    while (g_pw.carry_s >= PRIZE_WHEELS_STEP_S && steps++ < PRIZE_WHEELS_MAX_STEPS) {
        g_pw.carry_s -= PRIZE_WHEELS_STEP_S;
        step(PRIZE_WHEELS_STEP_DELTA, client);
    }
    if (g_pw.carry_s > PRIZE_WHEELS_STEP_S) g_pw.carry_s = 0.0; // dropped behind: the steps go on from now
}
