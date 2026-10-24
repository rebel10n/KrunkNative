#pragma once
// Krunker v10.0.3 map prize wheels: MapManager.spinners (shared/src/map_spinners.c), the map's PRIZE_WHEEL objects that
// map.generate builds (deob-10.0.3.js:189053-189066) in object order; a spinner's uid is its place in that list
// (MapManager.addSpinner, :186539-186570). One list for the interact prompt (game_interact_scan, type 9), the client's
// spin state (client/src/prize_wheels.c) and the wheel meshes (client/src/prize_wheel_render.c).
//   skipped     store.wheels[limitMM(parseInt(sty) || 0, 0, 2)] missing, ticketOnly, hidden or locked (:189054-189057)
//   GameObject  {x, z, y: the object's bottom, w = l = h = sy / 4, d, tRadius 7, wheel} (:186543-186561); the player
//               collides with it through player.collides(spinner, tRadius) (:192203-192208)
//   the mesh    a sy x sy plane at the bottom centre + sy / 2, rotateY(PI / 2 - d * PI / 4) (:186562-186567, :232681-232688)
#include <shared.h>

#define MAP_SPINNER_TRADIUS 7.0f // tRadius: 7 (:189062)

struct StoreWheel;

// The store wheel (0..2) of a PRIZE_WHEEL that map.generate builds, -1 for any other object or a skipped wheel.
int map_spinner_wheel(const Object *object);
// The StoreWheel of map_spinner_wheel (store_spins_builtin), NULL when skipped.
const struct StoreWheel *map_spinner_store_wheel(const Object *object);
// MapManager.spinners.length of the map.
int map_spinner_count(const Map *map);
// The map object index of spinners[uid], -1 none.
int map_spinner_object(const Map *map, int uid);
// The spinner uid of the map object `index`, -1 when it is no spinner.
int map_spinner_uid(const Map *map, int index);
// IÍiìiiî.d || 0: the wheel's direction in eighths of a turn (the mesh turns PI / 2 - d * PI / 4).
float map_spinner_dir(const Object *object);
// The GameObject's box: centre x / z at the object, y the object's bottom; half extents sy / 4 on every axis.
void map_spinner_box(const Object *object, vec3 *centre, vec3 *half);
