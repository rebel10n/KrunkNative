#pragma once
// WP-G (doc/v7/map-logic.md §1.8, §7 WP-G step 4): the client side of the v7.1.4 map environment:
// ENVIRONMENT_ZONE lighting swaps by camera position (Map.env_zones, world_environment_zone_at),
// tone mapping / output encoding / correct lights, sky dome assets, ocean. Declared by WP-A; the
// body in client/src/map_environment.c is a stub until WP-G replaces it.
#include <shared.h>

// Every frame with the camera position: switches RenderLighting / sky / fog / ambient sound to
// the environment zone the camera is in (-1 = the map's Map.env). Returns the active zone index.
int map_environment_update(const Map *map, vec3 camera_position);
