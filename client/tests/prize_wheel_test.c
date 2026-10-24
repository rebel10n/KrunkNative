// v10.0.3 map prize wheels: the spinner list and its GameObject (shared/src/map_spinners.c), the type 9 prompt
// (game_interact_scan, shared/src/interact.c) and the canvas layout of game.updateSpinners (client/src/prize_wheel_geom.c):
// the canvas size, the slices, the stop's widening, the pointer landing on the slice the roll picked, the shadow
// levels and the plane's pose. Run from the repository root: ./<build>/prize_wheel_test
#include <shared.h>
#include <map_spinners.h>
#include <store_spins.h>
#include <modes.h>
#include <prize_wheel_geom.h>
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures, g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

#define NEAR(a, b, eps) (fabs((double) (a) - (double) (b)) <= (eps))

// A floor, a cube, and PRIZE_WHEELs of store wheels 0 (Starter), 1 (Elite: ticketOnly, skipped), 2 (Heroic) and 7
// (limitMM to 2: Heroic again)
static const char *k_map =
    "{\"name\":\"wheel_test\",\"spawns\":[[0,1,0,0,0,0]],\"objects\":["
    "{\"p\":[0,0,0],\"s\":[400,1,400]},"
    "{\"p\":[30,1,0],\"s\":[4,4,4]},"
    "{\"p\":[50,1,0],\"s\":[0.01,10,10],\"i\":68},"
    "{\"p\":[50,1,40],\"s\":[0.01,10,10],\"i\":68,\"sty\":1},"
    "{\"p\":[-50,1,0],\"s\":[0.01,20,20],\"i\":68,\"sty\":2,\"d\":2},"
    "{\"p\":[0,1,-60],\"s\":[0.01,10,10],\"i\":68,\"sty\":\"7\",\"d\":\"4\"}"
    "]}";

static void test_canvas_size(void) {
    CHECK(prize_wheel_canvas_px(10.0f) == 768, "sy 10: 768 px (max(353.3, 768))");
    CHECK(prize_wheel_canvas_px(0.0f) == 768, "sy 0: 768 px");
    CHECK(prize_wheel_canvas_px(30.0f) == 1060, "sy 30: 30 / 7.5 * 265 = 1060 px, got %d", prize_wheel_canvas_px(30.0f));
    CHECK(prize_wheel_canvas_px(31.0f) == 1095, "sy 31: 1095.33 truncated, got %d", prize_wheel_canvas_px(31.0f));
    CHECK(prize_wheel_canvas_px(500.0f) == 4096, "sy 500: 4096 px at most");
}

static PrizeWheelState wheel_state(const StoreWheel *w, const int px) {
    PrizeWheelState s = {.canvas = px, .pool = w->pool_count > 0, .count = w->pool_count > 0 ? w->pool_count : 8,
                         .weight_count = w->slices, .won_rarity = -1, .spin_whl_mlt = 1.0f};
    for (int i = 0; i < w->slices && i < PRIZE_WHEEL_GEOM_SLICES; i++) s.weights[i] = w->weights[i];
    return s;
}

// The slice the pointer's tip points into: the tip (0, -L) turned by spinRotation, in canvas angles (y down)
static int slice_under_pointer(const PrizeWheelLayout *l) {
    const double x = -l->pointer[0][1] * sin(l->pointer_rotation), y = l->pointer[0][1] * cos(l->pointer_rotation);
    double a = atan2(y, x) - l->turn;
    a = fmod(a, M_PI * 2.0);
    if (a < 0.0) a += M_PI * 2.0;
    for (int i = 0; i < l->count; i++) {
        if (l->slices[i].sweep > 0.0 && a >= l->slices[i].start && a < l->slices[i].start + l->slices[i].sweep) return i;
    }
    return -1;
}

static void test_layout(void) {
    const StoreSpins *spins = store_spins_builtin();
    CHECK(spins->wheel_count >= 4, "the compiled-in wheels");
    if (spins->wheel_count < 4) return;
    const StoreWheel *heroic = &spins->wheels[2], *ultra = &spins->wheels[3];
    CHECK(heroic->pool_count == 6 && !strcmp(heroic->name, "Heroic"), "wheel 2 is the Heroic pool wheel");
    CHECK(ultra->pool_count == 0 && ultra->slices == 7, "wheel 3 (Ultra) draws by rarity");

    // the idle Heroic wheel: a white disc of 768 / 2 * 0.95, six pool slices in a full turn, the SPIN label
    PrizeWheelState s = wheel_state(heroic, 768);
    PrizeWheelLayout l;
    prize_wheel_layout(&s, &l);
    CHECK(NEAR(l.centre, 384.0, 1e-9) && NEAR(l.radius, 364.8, 1e-9) && NEAR(l.slice_radius, 364.8 * 0.95, 1e-9), "R = 364.8, slices R * 0.95");
    CHECK(l.count == 6 && l.turn == 0.0, "six slices, no turn while it spins");
    double total = 0.0;
    for (int i = 0; i < l.count; i++) total += l.slices[i].sweep;
    CHECK(NEAR(total, M_PI * 2.0, 1e-9), "the pool weights fill the wheel (%g)", total);
    CHECK(NEAR(l.slices[1].start, M_PI * 2.0 * 0.48, 1e-9), "slice 1 starts after 48 %%");
    CHECK(NEAR(l.pointer[0][1], -1.2 * 0.6 * 364.8, 1e-9) && NEAR(l.pointer[1][0], 0.6 * 364.8, 1e-9), "the pointer: 1.2 x the hub up, the hub wide");
    CHECK(l.label && NEAR(l.label_radius, 0.6 * 364.8 * 0.85, 1e-9) && NEAR(l.spin_font, 0.6 * 364.8 * 0.3, 1e-9) &&
          NEAR(l.price_y, 0.6 * 364.8 * 0.25, 1e-9), "the SPIN label");

    // the grow-in: spinWhlMlt scales every radius
    s.spin_whl_mlt = 0.5f;
    prize_wheel_layout(&s, &l);
    CHECK(NEAR(l.radius, 182.4, 1e-9) && NEAR(l.hub_radius, 0.6 * 182.4, 1e-9), "half grown: half the radius");
    s.spin_whl_mlt = 1.0f;

    // "spinW": the pointer stops at 2 pi (rand / 100 + 13) + pi / 2, over the slice the roll lands on (store_wheel_slice)
    const double rolls[] = {0.05, 10.0, 47.99, 48.01, 70.0, 82.99, 83.01, 96.5, 99.5, 99.995};
    for (size_t i = 0; i < sizeof(rolls) / sizeof(rolls[0]); i++) {
        s.spin_rotation = (float) (M_PI * 2.0 * (rolls[i] / 100.0 + 13.0) + M_PI / 2.0);
        prize_wheel_layout(&s, &l);
        const int got = slice_under_pointer(&l), want = store_wheel_slice(heroic, rolls[i]);
        CHECK(got == want, "roll %g: the pointer is over slice %d, the roll's slice is %d", rolls[i], got, want);
    }

    // a rarity wheel's stop (Ultra, an Epic): endAnimTarget = 100 - 53; while it widens the slices still fill the
    // wheel, the pointer shrinks; at the end the Epic slice is the whole wheel and the pointer is gone
    s = wheel_state(ultra, 768);
    CHECK(s.count == 8 && s.weight_count == 7, "store.rarities.length slices, seven of them weighted");
    s.won_rarity = 2;
    s.end_anim_target = 100.0f - (float) ultra->weights[2];
    s.end_anim_val = s.end_anim_target / 2.0f;
    prize_wheel_layout(&s, &l);
    total = 0.0;
    for (int i = 0; i < l.count; i++) total += l.slices[i].sweep;
    CHECK(NEAR(total, M_PI * 2.0, 1e-5), "half widened: still a full wheel (%g)", total);
    CHECK(l.slices[7].sweep == 0.0, "the 8th rarity (NFT) has no weight: not drawn");
    CHECK(NEAR(l.pointer[1][0], 0.6 * 364.8 * 0.5, 1e-4), "the pointer at half its width");
    const double mid = M_PI * 2.0 * (0.0 + 28.0 + 53.0 / 2.0) / 100.0;
    CHECK(NEAR(l.turn, fmod(mid * 0.5, M_PI * 2.0) - M_PI * 0.5, 1e-9), "the turn: 2 pi * the won slice's middle * done - pi * done");
    s.end_anim_val = s.end_anim_target;
    prize_wheel_layout(&s, &l);
    CHECK(NEAR(l.slices[2].sweep, M_PI * 2.0, 1e-5) && l.slices[1].sweep == 0.0 && l.slices[3].sweep == 0.0, "widened: the Epic slice is the wheel");
    CHECK(NEAR(l.pointer[0][1], 0.0, 1e-9) && NEAR(l.pointer[1][0], 0.0, 1e-9), "widened: the pointer is gone");
    s.item_shown = 1;
    prize_wheel_layout(&s, &l);
    CHECK(!l.label, "meshPrev up: no SPIN label");
}

static void test_shadow(void) {
    double grow[6], alpha[6];
    const int n = prize_wheel_shadow_levels(PRIZE_WHEEL_SHADOW_BLUR, 6, grow, alpha);
    CHECK(n == 6, "six levels");
    double clear = 1.0;
    for (int i = 0; i < n; i++) {
        clear *= 1.0 - alpha[i];
        const double cover = 0.5 * (i + 1) / 6.0;
        CHECK(NEAR(1.0 - clear, cover, 1e-9), "level %d: %g stacked", i, cover);
        CHECK(NEAR(0.5 * erfc(grow[i] / (10.0 * M_SQRT2)), cover, 1e-6), "level %d: grown to where the blur covers %g", i, cover);
        CHECK(alpha[i] > 0.0 && alpha[i] < 1.0 && (i == 0 || grow[i] < grow[i - 1]), "level %d: widest first", i);
    }
    CHECK(NEAR(grow[n - 1], 0.0, 1e-6), "the last level is the shape's own edge (0.5)");
}

static void test_plane(void) {
    float centre[3], yaw;
    prize_wheel_plane((const float[3]) {5.0f, 2.0f, -3.0f}, 10.0f, 0.0f, centre, &yaw);
    CHECK(centre[0] == 5.0f && centre[1] == 7.0f && centre[2] == -3.0f, "the plane's centre: bottom + sy / 2");
    CHECK(NEAR(yaw, M_PI / 2.0, 1e-6), "d 0: rotateY(pi / 2), the canvas faces +x");
    prize_wheel_plane((const float[3]) {0.0f, 0.0f, 0.0f}, 10.0f, 2.0f, centre, &yaw);
    CHECK(NEAR(yaw, 0.0, 1e-6), "d 2: faces +z");
}

// meshPrev: the won item in front of the hub (:219485-219574)
static void test_item_pose(void) {
    PrizeWheelItemPose p;
    // a weapon skin (type 0) on a 16 high wheel facing +x (d 0): the frame turns PI, so meshPrev's -x is the world's +x,
    // the canvas side; tipped PI / 11, scaled w / 4 = 1, the model PI / 2 and (scl 0.0014) * 2
    prize_wheel_item_pose(16.0f, 0.0f, 0, -0.6f, -2.0f, 0.0014f, 0.0f, &p);
    CHECK(NEAR(p.frame_yaw, M_PI, 1e-6), "the frame: rotateY(pi - 0)");
    CHECK(NEAR(p.position[0], -2.5, 1e-6) && NEAR(p.position[1], 0.4, 1e-6) && NEAR(p.position[2], -2.0, 1e-6),
          "meshPrev at (w / 8 * -5, 1 + yOff, xOff) = (-2.5, 0.4, -2)");
    CHECK(NEAR(p.rotation[0], M_PI / 11.0, 1e-6) && p.rotation[1] == 0.0f && NEAR(p.scale, 1.0, 1e-6), "a weapon tips pi / 11, scale w / 4");
    CHECK(NEAR(p.item_rot_y, M_PI / 2.0, 1e-6) && NEAR(p.item_scale, 0.0028, 1e-7), "the weapon model: pi / 2, scl * 2");
    // the frame turned: the item stands on the canvas side of the wheel (the plane's +z, rotateY(pi / 2 - d pi / 4))
    for (int d = 0; d < 8; d++) {
        float centre[3], yaw;
        prize_wheel_plane((const float[3]) {0.0f, 0.0f, 0.0f}, 16.0f, (float) d, centre, &yaw);
        prize_wheel_item_pose(16.0f, (float) d, 0, 0.0f, 0.0f, 0.0f, 0.0f, &p);
        const double nx = sin(yaw), nz = cos(yaw); // the canvas' normal: rotateY(yaw) of +z
        const double ix = cos(p.frame_yaw) * p.position[0], iz = -sin(p.frame_yaw) * p.position[0]; // rotateY of (x, 0, 0)
        CHECK(ix * nx + iz * nz > 0.0 && NEAR(ix * nx + iz * nz, 2.5, 1e-5), "d %d: the item is 5 / 32 sy in front of the canvas", d);
    }
    // a hat (type 1): its own offsets, upright, the model turned PI, spnScl first
    prize_wheel_item_pose(10.0f, 2.0f, 1, 0.5f, 0.25f, 1.5f, 2.0f, &p);
    CHECK(p.rotation[0] == 0.0f && p.rotation[1] == 0.0f && NEAR(p.item_rot_y, M_PI, 1e-6) && NEAR(p.item_scale, 4.0, 1e-6),
          "a cosmetic: upright, pi, spnScl * 2");
    CHECK(NEAR(p.position[1], 1.5, 1e-6) && NEAR(p.scale, 0.625, 1e-6), "a cosmetic: 1 + its yOff, scale sy / 16");
    prize_wheel_item_pose(10.0f, 0.0f, 3, 0.0f, 0.0f, 0.0f, 0.0f, &p);
    CHECK(NEAR(p.item_rot_y, M_PI / 2.0, 1e-6) && NEAR(p.item_scale, 2.0, 1e-6), "a melee: pi / 2, (1) * 2");
    prize_wheel_item_pose(10.0f, 0.0f, 4, 0.0f, 0.0f, 0.0f, 0.0f, &p);
    CHECK(NEAR(p.rotation[1], M_PI / 2.0, 1e-6) && NEAR(p.item_rot_y, M_PI, 1e-6), "a spray: meshPrev turned pi / 2, its plane pi");
    CHECK(NEAR(prize_wheel_item_bob(1.0f, (float) (M_PI / 2.0)), 1.55, 1e-6) && prize_wheel_item_bob(1.0f, 0.0f) == 1.0f, "the bob: sin * 0.55");
}

static void free_players(Game *game) {
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        free(player->loadout);
        free(player->ammo);
        free(player->reloads);
        free(player->input_queue);
        free(player);
    }
    free(game->players);
    game->players = NULL;
    game->player_count = 0;
}

static void test_map(void) {
    cJSON *json = cJSON_Parse(k_map);
    CHECK(json != NULL, "the test map parses");
    if (!json) return;
    static Game game;
    memset(&game, 0, sizeof(game));
    int *modes = calloc(1, sizeof(int));
    modes[0] = MODE_ID_FFA;
    const cJSON **maps = calloc(1, sizeof(cJSON *));
    maps[0] = json;
    game_configure(&game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game_init(&game, 0, 0, 1);
    const Map *map = game.map;
    CHECK(map && map->object_count == 6, "six objects");
    if (!map || map->object_count != 6) return;

    CHECK(map_spinner_count(map) == 3, "three spinners: Elite (ticketOnly) is skipped, got %d", map_spinner_count(map));
    CHECK(map_spinner_wheel(map->objects[0]) == -1 && map_spinner_wheel(map->objects[1]) == -1, "a cube is no spinner");
    CHECK(map_spinner_wheel(map->objects[2]) == 0 && map_spinner_wheel(map->objects[3]) == -1 && map_spinner_wheel(map->objects[4]) == 2 &&
          map_spinner_wheel(map->objects[5]) == 2, "wheels 0, skipped, 2 and limitMM(7) = 2");
    CHECK(map_spinner_object(map, 0) == 2 && map_spinner_object(map, 1) == 4 && map_spinner_object(map, 2) == 5 && map_spinner_object(map, 3) == -1,
          "uids in object order");
    CHECK(map_spinner_uid(map, 4) == 1 && map_spinner_uid(map, 3) == -1 && map_spinner_uid(map, 1) == -1, "object -> uid");
    CHECK(map_spinner_dir(map->objects[2]) == 0.0f && map_spinner_dir(map->objects[4]) == 2.0f && map_spinner_dir(map->objects[5]) == 4.0f,
          "d || 0 (a string too)");
    vec3 c, h;
    map_spinner_box(map->objects[4], &c, &h);
    CHECK(c.x == -50.0f && c.y == 1.0f && c.z == 0.0f && h.x == 5.0f && h.y == 5.0f && h.z == 5.0f, "the GameObject: at the bottom, sy / 4 each way");
    const StoreWheel *w = map_spinner_store_wheel(map->objects[2]);
    CHECK(w && !strcmp(w->name, "Starter") && w->price == 50, "spinner 0 spins the Starter wheel");

    // the prompt: changeInteract(spinner, 9) in reach (x / z: sy / 4 + tRadius 7; y: the bottom +- sy / 4)
    Player *p = player_init(&game);
    p->uid = 1;
    game_players_add(&game, p);
    player_spawn(p);
    GameInteractScan scan;
    p->position = (vec3) {50.0f - 2.5f - 6.9f, 1.0f, 0.0f}; // just inside x - (w + 7)
    game_interact_scan(&game, p, &scan);
    CHECK(scan.type == GAME_INTERACT_SPINNER && scan.index == 0 && scan.last_uid == 0 && scan.last_type == GAME_INTERACT_SPINNER,
          "next to spinner 0: type 9, uid 0 (got type %d index %d)", scan.type, scan.index);
    p->position = (vec3) {-50.0f, 1.0f, 5.0f + 6.9f};
    game_interact_scan(&game, p, &scan);
    CHECK(scan.type == GAME_INTERACT_SPINNER && scan.index == 1, "next to the big Heroic wheel: uid 1 (got %d)", scan.index);
    p->position = (vec3) {50.0f - 2.5f - 7.1f - p->scale, 1.0f, 0.0f};
    game_interact_scan(&game, p, &scan);
    CHECK(scan.type != GAME_INTERACT_SPINNER, "out of reach: no prompt");
    p->position = (vec3) {50.0f, 1.0f, 40.0f};
    game_interact_scan(&game, p, &scan);
    CHECK(scan.type != GAME_INTERACT_SPINNER, "the skipped Elite wheel: no prompt");
    p->position = (vec3) {-50.0f, 1.0f + 5.0f + 0.1f, 0.0f};
    game_interact_scan(&game, p, &scan);
    CHECK(scan.type != GAME_INTERACT_SPINNER, "feet above the bottom + sy / 4: no prompt (v10's box sits at the wheel's foot)");
    p->position = (vec3) {-50.0f, 1.0f + 4.9f, 0.0f};
    game_interact_scan(&game, p, &scan);
    CHECK(scan.type == GAME_INTERACT_SPINNER, "feet below the bottom + sy / 4: the prompt");
    p->noclip = 1;
    game_interact_scan(&game, p, &scan);
    CHECK(scan.type == GAME_INTERACT_NONE, "noclip: no prompt");
    p->noclip = 0;
    free_players(&game);
}

int main(void) {
    test_canvas_size();
    test_layout();
    test_shadow();
    test_plane();
    test_item_pose();
    test_map();
    printf("prize_wheel_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
