#pragma once
// GL backend of the HTML/CSS engine: submits a DisplayList (paint_types.h)
// through one shader program (assets/shaders/html_ui.{vert,frag}) and one
// streaming vertex format. Valid on OpenGL 4.5 core and OpenGL ES 3.0.
//
// Frame protocol (main thread, GL context current):
//   glb_begin(b, fb_w, fb_h, scale);   // records the bound FBO, sets GL state
//   glb_submit(b, &list);              // any number of lists
//   glb_end(b);                        // flushes, restores client blend state
//
// GL state after glb_end(): blend enabled with GL_SRC_ALPHA /
// GL_ONE_MINUS_SRC_ALPHA (the client's global blend function), depth test
// disabled, scissor and stencil tests disabled, VAO 0, texture unit 0 active
// with no 2D texture bound, the framebuffer that was bound at glb_begin().
// Cull face is restored to what it was. g_active_shader / g_active_texture
// (client.h) are updated so the client's shader cache stays coherent.
#include <stdint.h>
#include "paint_types.h"

typedef struct GlBackend GlBackend;

typedef struct {
    int draw_calls;     // glDrawElements calls in the last frame (incl. stencil/blur passes)
    int vertices;       // vertices uploaded in the last frame
    int fbo_passes;     // offscreen passes (group render, blur passes, group acquire)
    int culled;         // primitives skipped outside the scissor / target (paint culling)
} GlbStats;

// Compiles the program and creates the GL resources. NULL on failure
// (shader compile error is printed by shader_compile()).
GlBackend *glb_create(void);
void glb_destroy(GlBackend *b);

// Text gamma/contrast preblend table: row = luminance bucket of the text
// color (0 = black .. 7 = white), column = linear coverage 0..255, value =
// corrected coverage. Identity until set.
void glb_set_text_gamma(GlBackend *b, const uint8_t lut[8][256]);

// Vector images (DL_IMAGE with `vector` set, i.e. SVG): draw_image asks the
// resolver for a texture of the image covering dev_w x dev_h target pixels
// (fractional) whose top-left lies at (frac_x, frac_y) inside its first
// pixel. When the resolver reports *tex_w x *tex_h texels, the quad is drawn
// texel-aligned on the target grid from that pixel (like Chrome drawing the
// vector in place: crisp, no resampling); *tex_w = 0 means a plain texture
// stretched over dst. The resolver may create textures (binding of the
// active unit preserved); 0 keeps `tex`.
typedef unsigned (*GlbImageResolver)(void *user, const void *vector, int nearest, float dev_w, float dev_h, float frac_x,
                                     float frac_y, int *tex_w, int *tex_h);
void glb_set_image_resolver(GlBackend *b, GlbImageResolver fn, void *user);
// Lazy rasters (DL_IMAGE.lazy, image.h): the texture of `lazy` for this draw, decoding it
// first when the caller's per-frame budget allows; 0 skips the draw this frame (it is
// retried on the next submit). The binding of the active unit is preserved.
typedef unsigned (*GlbLazyImageResolver)(void *user, const void *lazy, int nearest);
void glb_set_lazy_image_resolver(GlBackend *b, GlbLazyImageResolver fn, void *user);

// fb_w/fb_h: size of the currently bound framebuffer in pixels.
// device_scale: device pixels per CSS px (list geometry is CSS px).
void glb_begin(GlBackend *b, int fb_w, int fb_h, float device_scale);
void glb_submit(GlBackend *b, const DisplayList *list);
void glb_end(GlBackend *b);

GlbStats glb_stats(const GlBackend *b);
