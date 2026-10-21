#pragma once
// Explosion debris (client/src/vfx.c, ExplosionManager): the pool's piece type and the slot a new
// piece takes. Header-only, so client/tests/vfx_debris_test.c checks the slot choice without the
// renderer.
#include <common.h>
#include <stddef.h>

typedef struct {
    unsigned char active;
    vec3 position;
    vec3 velocity;          // per second (ExplosionManager works in seconds)
    float life;
    float life_speed;
    float size;
} VfxDebris;

// A piece is drawn while life < VFX_DEBRIS_LIFE_END (vfx.c update_debris: the cube's edge is
// (2 - (life / sqrt 2)^2) * size, gone at zero).
#define VFX_DEBRIS_LIFE_END 2.0f

// The slot for a new piece: the first free one. A full pool gives up the piece that disappears
// first (fewest seconds left), so a new explosion always shows its debris; NULL only for count 0.
static inline VfxDebris *vfx_debris_slot(VfxDebris *pool, const int count) {
    VfxDebris *evict = NULL;
    float soonest = 0.0f;
    for (int i = 0; i < count; i++) {
        VfxDebris *piece = &pool[i];
        if (!piece->active) return piece;
        const float speed = piece->life_speed > 0.0f ? piece->life_speed : 1.0f;
        const float left = (VFX_DEBRIS_LIFE_END - piece->life) / speed;
        if (!evict || left < soonest) {
            evict = piece;
            soonest = left;
        }
    }
    return evict;
}
