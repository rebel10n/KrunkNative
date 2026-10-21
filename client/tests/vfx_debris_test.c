// client/include/vfx_debris.h (vfx.c spawn_debris): a full debris pool still gives a new explosion
// its pieces. Before the fix a full pool dropped the explosion's debris silently ("explosions
// sometimes not showing", Krunker v10.0.3).
//   cmake-build-debug/vfx_debris_test
#include <vfx_debris.h>

#include <stdio.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define POOL 4

static void fill(VfxDebris *pool, const int n, const float life, const float life_speed) {
    for (int i = 0; i < n; i++) {
        pool[i] = (VfxDebris) {.active = 1, .life = life, .life_speed = life_speed, .size = 1.0f};
    }
}

int main(void) {
    // an empty pool: the first slot
    VfxDebris pool[POOL] = {0};
    CHECK(vfx_debris_slot(pool, POOL) == &pool[0], "empty pool gives slot 0");

    // a free slot is taken before any piece is given up
    pool[0] = (VfxDebris) {.active = 1, .life = 0.1f, .life_speed = 3.0f};
    pool[1] = (VfxDebris) {.active = 1, .life = 1.9f, .life_speed = 2.0f}; // nearly gone, but it is active
    pool[2] = (VfxDebris) {0};                                            // free
    pool[3] = (VfxDebris) {.active = 1, .life = 0.5f, .life_speed = 2.0f};
    CHECK(vfx_debris_slot(pool, POOL) == &pool[2], "a free slot is used first (got %d)", (int) (vfx_debris_slot(pool, POOL) - pool));

    // a full pool gives up the piece that disappears first: (2 - life) / life_speed seconds left
    // pool[0]: 1.9 / 3 = 0.63 s, pool[1]: 0.1 / 2 = 0.05 s, pool[2]: 1.5 / 2 = 0.75 s, pool[3]: 1.5 / 2 = 0.75 s
    fill(pool, POOL, 0.0f, 2.0f);
    pool[0] = (VfxDebris) {.active = 1, .life = 0.1f, .life_speed = 3.0f};
    pool[1] = (VfxDebris) {.active = 1, .life = 1.9f, .life_speed = 2.0f};
    pool[2] = (VfxDebris) {.active = 1, .life = 0.0f, .life_speed = 2.0f};
    pool[3] = (VfxDebris) {.active = 1, .life = 0.5f, .life_speed = 2.0f};
    VfxDebris *got = vfx_debris_slot(pool, POOL);
    CHECK(got == &pool[1], "a full pool gives up the piece closest to its end (got %d)", (int) (got - pool));

    // a full pool never returns NULL: a new explosion always gets a slot
    fill(pool, POOL, 0.0f, 2.0f);
    CHECK(vfx_debris_slot(pool, POOL) != NULL, "a full pool still gives a slot");

    // a piece with a zero life speed is treated as speed 1 (no division by zero)
    pool[0] = (VfxDebris) {.active = 1, .life = 1.5f, .life_speed = 0.0f};
    pool[1] = (VfxDebris) {.active = 1, .life = 0.2f, .life_speed = 1.0f};
    pool[2] = (VfxDebris) {.active = 1, .life = 0.2f, .life_speed = 1.0f};
    pool[3] = (VfxDebris) {.active = 1, .life = 0.2f, .life_speed = 1.0f};
    got = vfx_debris_slot(pool, POOL);
    CHECK(got == &pool[0], "zero life speed counts as 1 (got %d)", (int) (got - pool));

    // an empty pool (no slots at all) gives NULL, which spawn_debris returns on
    CHECK(vfx_debris_slot(pool, 0) == NULL, "count 0 gives NULL");

    printf("vfx_debris_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
