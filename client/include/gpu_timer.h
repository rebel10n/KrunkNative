#pragma once
// Per-pass GPU (and CPU) frame timing: KRUNKNATIVE_GPU_TIMING=1 prints, once a second, the
// average time each render pass of the frame took on the GPU (GL timestamp queries, read a few
// frames late so nothing stalls) and on the CPU, to stderr:
//   [gpu] 612 frames: gpu 1.21 ms (clear 0.02 pre 0.00 sky 0.04 world 0.71 ...) cpu 1.35 ms (...)
// Implementation: client/src/gpu_timer.c. Desktop GL only (GL_TIMESTAMP is not in OpenGL ES 3.0:
// on ANGLE and the web the GPU columns read 0 and only the CPU side is measured). Without the
// variable every call returns at once.

// Passes in frame order; a mark ends the pass and starts the next one.
typedef enum {
    GPU_PASS_CLEAR,      // the frame's clear (client_tick)
    GPU_PASS_PRE,        // render target clear, the static shadow map (after a map load), ocean mirror
    GPU_PASS_SKY,        // sky dome
    GPU_PASS_WORLD,      // the world scene
    GPU_PASS_VFX,        // tracers, impacts, particles, nav / Object Manager overlays
    GPU_PASS_VIEWMODEL,  // the first-person scene
    GPU_PASS_POST,       // the scene target's upscale / filters (render_target_end)
    GPU_PASS_HUD,        // native HUD and the overlay canvases
    GPU_PASS_PREVIEW,    // class preview, menu-side canvases
    GPU_PASS_PAGE,       // the HTML page and the console
    GPU_PASS_COUNT
} GpuPass;

int  gpu_timer_enabled(void);
void gpu_timer_frame_begin(void);     // before the frame's first GL call
void gpu_timer_mark(GpuPass pass);    // `pass` ends here
void gpu_timer_frame_end(void);       // after the frame's last GL call (before the swap)
