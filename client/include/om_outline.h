#pragma once
// KrunkNative Object Manager outline (client/src/om_outline.c): the silhouette of the Tool Gun's
// selection and of what the Physics Gun carries, a ring a few pixels wide around the meshes as they
// are drawn (stencil mask + eight screen-shifted flat copies), seen through walls. Replaces the yellow /
// cyan boxes; only GL calls of both GL 4.5 core and GLES 3.0 (the stencil comes with every target
// the world draws into: GLFW's default framebuffer, the EGL pbuffer, render_target / frame_target).
#include <client.h>

#define OM_OUTLINE_SELECT 0xffd23f // the Tool Gun's selection
#define OM_OUTLINE_CARRY 0x46e6ff  // the Physics Gun's carry

void om_outline_add(Mesh *mesh, int rgb);                       // this frame (cleared by om_outline_render)
void om_outline_add_player(const Player *player, int rgb);      // every mesh of a player's model
int om_outline_add_object(const Object *object, int rgb);       // its mesh; 0 when it has none drawn (a zone: draw a box)
void om_outline_render(Camera *camera);                         // after the world pass, before the gizmo lines
void om_outline_fini(void);                                     // renderer shutdown: the program
