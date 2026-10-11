#pragma once
// Map render work that runs once every object mesh exists (map_post.c): AO planes
// (ambient.c), noise-plane bump geometry and the POINT_LIGHT list.
#include <client.h>

void map_post_load(Scene *scene, const Map *map);
void map_post_unload(Scene *scene);
