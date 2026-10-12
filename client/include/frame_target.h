#pragma once
// Whole-frame offscreen target (client/src/frame_target.c).
//
// The browser composites a WebGL canvas whenever control returns to its event loop after the
// drawing buffer changed. On the web every blocking asset load yields to it (ensure_asset_file:
// ASYNCIFY + emscripten_wget_data), and loads happen mid-frame: a spray texture
// (sprays.c), a joining player's models/textures/GLTF items (player_mesh.c, cosmetics.c,
// skin_fx.c, pets.c, gltf.c). Drawing straight into the canvas therefore showed the frame as
// far as it had got, i.e. only client_tick's sky-colour clear, no world (the white / fog-only
// flash), once per fetch. With the target on, the frame draws into an offscreen framebuffer
// and frame_target_present() copies it to the canvas once, after everything was drawn: a
// yield finds the canvas untouched, so the browser keeps showing the last complete frame.
//
// On by default on the web. Natively the window only shows a frame at glfwSwapBuffers, so it
// is off; KRUNKNATIVE_FRAME_TARGET=1 turns it on for tests. KRUNKNATIVE_YIELD_CHECK=1 emulates
// the browser natively: at every ensure_asset_file() of a running game it inspects framebuffer
// 0 (what a browser would composite at that yield) and reports a frame without the world.
//
// GL state: begin() binds the target as GL_FRAMEBUFFER (viewport untouched: the window size);
// present() blits it to framebuffer 0 with the scissor test off, then binds the target again.
// Code that means "the window" binds frame_target_framebuffer(), never 0.
#include <client.h>

void frame_target_begin(Client*);           // client_frame(): before client_tick()
void frame_target_present(Client*);         // client_frame(): before the swap
unsigned int frame_target_framebuffer(void); // the framebuffer standing for the window this frame (0 when off)
void frame_target_fini(void);
