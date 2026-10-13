#pragma once
// KrunkNative Object Manager world overlay (client/src/om_gizmo.c): the btools' selection box and
// handles and the Physics Gun's beam, as GL_LINES over the world (LineMaterial, no depth test),
// rebuilt every frame by client/src/ui/om_build.c. Same drawing style as nav_overlay.c.
#include <client.h>

typedef enum {
    OM_GIZMO_WHITE = 0, // hovered handle
    OM_GIZMO_YELLOW,    // the selected object
    OM_GIZMO_RED,       // x
    OM_GIZMO_GREEN,     // y
    OM_GIZMO_BLUE,      // z
    OM_GIZMO_CYAN,      // the Physics Gun's beam and the carried object
    OM_GIZMO_COLORS
} OmGizmoColor;

void om_gizmo_clear(void);                                   // start a frame's lines
void om_gizmo_line(int color, vec3 a, vec3 b);
void om_gizmo_box(int color, vec3 feet, vec3 size);          // a box's 12 edges (feet: bottom centre)
void om_gizmo_render(Camera *camera);                        // after the world, before the first-person pass
void om_gizmo_fini(void);                                    // renderer shutdown: GL buffers, the line program
