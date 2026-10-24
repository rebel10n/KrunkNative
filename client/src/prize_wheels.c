// v10.0.3 map prize wheels on the client (client/include/prize_wheels.h). Line numbers: deob-10.0.3.js.
//   the list   map generate (:189053-189066): a PRIZE_WHEEL whose store.wheels[limitMM(parseInt(sty) || 0, 0, 2)] is
//              missing, ticketOnly, hidden or locked is skipped; the others are MapManager.addSpinner (:186539) in
//              object order, uid = the spinner's place, the mesh at the object's bottom centre + sy / 2
//   "spinW"    game.updateSpinner(uid, randNum, itemID) (:219408): the pointer heads for 2 pi (randNum / 100 + 13) + pi / 2
//   the steps  game.updateSpinners(150) every 50 ms (setInterval(updateSpinners(iìiïìíi * 3), iìiïìíi), :262717):
//              the wheel grows in, the pointer eases 5 % of the way per step with a tick_0 (0.2) every pi / 1.5, and
//              once within 0.002: endAnimTarget = 100 - wheel.rarities[item.rarity], reward (0.3), cheer_1 (rarity >= 4)
//              or cheer_0 (>= 1) at 0.1, an explosion (1) above rarity 5 (:219470-219574)
// Not ported (KrunkNative draws no PRIZE_WHEEL, prefab.c): the canvas wheel, the won item's model above it (meshPrev and
// its bob), the screen shake and the explosion effect of an Unobtainable. A pool wheel (store data with pools and no
// rarities, like the Starter / Heroic wheels of the 2026-10-13 data, shared/include/store_spins.h) throws in v10 at
// `wheel.rarities[rarity]`, so its stop plays nothing: the same here.
#include <prize_wheels.h>
#include <audio.h>
#include <store_spins.h>
#include <klog.h>
#include "viewer/viewer_store.h" // store.skins: the item's rarity
#include <cJSON.h>
#include <math.h>
#include <stdlib.h>
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

static int raw_int(const cJSON *raw, const char *key) { // parseInt(raw[key]) || 0
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(raw, key);
    if (cJSON_IsNumber(v)) return isfinite(v->valuedouble) ? (int) v->valuedouble : 0;
    if (cJSON_IsString(v) && v->valuestring) return (int) strtol(v->valuestring, NULL, 10);
    return 0;
}

static void build(const Map *map) {
    g_pw.map = map;
    g_pw.count = 0;
    const StoreSpins *spins = store_spins_builtin();
    for (size_t i = 0; map && i < map->object_count && g_pw.count < PRIZE_WHEELS_MAX; i++) {
        const Object *o = map->objects[i];
        if (!o || o->prefab != PREFAB_PRIZE_WHEEL || o->spawned || o->dynamic) continue;
        int sty = raw_int(o->raw, "sty");
        sty = sty < 0 ? 0 : sty > 2 ? 2 : sty;
        const StoreWheel *w = sty < spins->wheel_count ? &spins->wheels[sty] : NULL;
        if (!w || w->ticket_only || w->hide || w->locked) continue;
        PrizeWheel *p = &g_pw.wheels[g_pw.count++];
        memset(p, 0, sizeof(*p));
        p->object = (int) i;
        p->wheel = sty;
        p->position = (vec3) {o->position.x, o->position.y + o->scale.y / 2.0f, o->position.z};
        p->item_id = -1;
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
    w->rand_num = rand;
    w->item_id = skin;
    KN_LOG_DEBUG1("[prize] wheel %d spins to %g: item %d\n", uid, (double) rand, skin);
    return 1;
}

// The stop (:219477-219572): the won slice widens, the reward sounds
static void stop(PrizeWheel *w) {
    w->spin_rotation = w->spin_val;
    const ViewerSkin *item = viewer_skin(w->item_id);
    const StoreSpins *spins = store_spins_builtin();
    const StoreWheel *wheel = w->wheel < spins->wheel_count ? &spins->wheels[w->wheel] : NULL;
    if (!item || !wheel || wheel->pool_count > 0) return; // v10 throws on a missing item / a wheel without rarities
    const int rarity = item->rarity;
    w->end_anim_target = 100.0f - (rarity >= 0 && rarity < wheel->slices ? (float) wheel->weights[rarity] : 0.0f);
    audio_play_3d("reward", w->position, 0.3f, 1.0f, 0.0f, 0.0f);
    if (rarity >= 4) audio_play_3d("cheer_1", w->position, 0.1f, 1.0f, 0.0f, 0.0f);
    else if (rarity >= 1) audio_play_3d("cheer_0", w->position, 0.1f, 1.0f, 0.0f, 0.0f);
    if (rarity > 5) audio_play_3d("explosion", w->position, 1.0f, 1.0f, 0.0f, 0.0f);
}

static void step(const float delta) {
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
        if (w->spin_rotation == w->spin_val) continue;
        w->spin_rotation += (w->spin_val - w->spin_rotation) * 0.05f;
        w->tick_counter -= (w->spin_val - w->spin_rotation) * 0.05f;
        if (w->tick_counter <= 0.0f) {
            w->tick_counter += (float) (M_PI / 1.5);
            audio_play_3d("tick_0", w->position, 0.2f, 1.0f, 0.0f, 0.0f);
        }
        if (w->spin_val - w->spin_rotation <= 0.002f) stop(w);
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
        step(PRIZE_WHEELS_STEP_DELTA);
    }
    if (g_pw.carry_s > PRIZE_WHEELS_STEP_S) g_pw.carry_s = 0.0; // dropped behind: the steps go on from now
}
