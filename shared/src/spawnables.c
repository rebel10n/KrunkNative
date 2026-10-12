// STUB (WP-A): shared/include/spawnables.h bodies until WP-D implements the v7 spawnables and
// mode hooks (doc/v7/map-logic.md §7 WP-D). WP-D replaces this file.
#include <spawnables.h>

int spawnable_drop(Game *game, const int type, Player *owner, const int data, const vec3 position) {
    (void) game;
    (void) type;
    (void) owner;
    (void) data;
    (void) position;
    return -1;
}

void spawnables_update(Game *game, const float delta) {
    (void) game;
    (void) delta;
}

int mode_v7_collision_check(Game *game, Player *player, const Object *object) {
    (void) game;
    (void) player;
    (void) object;
    return 0;
}

void mode_v7_on_trigger(Game *game, Player *player, const int value) {
    (void) game;
    (void) player;
    (void) value;
}
