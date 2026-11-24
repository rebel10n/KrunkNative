#pragma once
// Krunker v7.1.4 developer console `ent_bbox` (mod/devConsole.ts:1392, the entity bounding-box overlay): the
// movement boxes of the players and map objects as 3D lines over the world (client/src/ent_bbox.c; the geometry
// is client/src/ent_bbox_geom.c). Drawn after the world and its effects with depth testing off, like
// show_navmeshes (nav_overlay.c), and rebuilt every frame: players and dynamic objects move, gates and
// destructibles come and go.
#include <client.h>

// ent_bbox [on|off]: no argument toggles; "on" / "1" shows, "off" / "0" / "clear" hides. The console answers
// "ent_bbox: on (N player boxes, M object boxes)" / "ent_bbox: off".
void ent_bbox_command(Client*, int argc, char **argv);
int  ent_bbox_active(void);
void ent_bbox_render(Client*); // after the world scene, before the first-person pass
void ent_bbox_fini(void);      // renderer shutdown: GL buffers and vertex lists
