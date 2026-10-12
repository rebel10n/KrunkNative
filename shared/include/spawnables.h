#pragma once
// WP-D (doc/v7/map-logic.md §4.1c, §7 WP-D): v7.1.4 spawnables (data/medals.js: 25 types) and the
// mode hooks other packages call. Declared by WP-A; the bodies in shared/src/spawnables.c are
// stubs until WP-D replaces them.
#include <shared.h>

// game.js spawnable drop ("crsp" type, sid, x, y, z, team, data): KR tags, ammo packs, weapon
// boxes, melee pickups, Krunkanium... Returns the spawnable sid, -1 when nothing spawned.
int spawnable_drop(Game*, int type, Player *owner, int data, vec3 position);
void spawnables_update(Game*, float delta);      // game.update #19

// mode.collisionCheck(player, obj) (king onZone, dom occupancy): called by the WP-B collision
// chain for every object the player touches. Returns 1 when the mode consumed the contact.
int mode_v7_collision_check(Game*, Player*, const Object*);
// trigger action 71 "Mode Specific" (zombies portal ...) and event 27 onObjectiveSwitch.
void mode_v7_on_trigger(Game*, Player*, int value);
