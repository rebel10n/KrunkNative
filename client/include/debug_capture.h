#pragma once
// Camera-matched world captures for renderer parity work (doc/ref/krunker-v3.1.1/world/).
//
// Environment (all optional; without KRUNKNATIVE_CAMERA / KRUNKNATIVE_SCREENSHOT nothing changes):
//   KRUNKNATIVE_CAMERA="x,y,z,pitch,yaw"  pin the free camera (Krunker units, radians; the camera
//                                         uses Krunker's YXZ order: yaw about Y, then pitch about X)
//   KRUNKNATIVE_FOV=<degrees>             vertical field of view override
//   KRUNKNATIVE_HIDE_UI=1                 skip the HTML page, class preview and HUD
//   KRUNKNATIVE_ORTHO=<half height>       orthographic projection (with KRUNKNATIVE_CAMERA; editor views)
//   KRUNKNATIVE_NO_FOG=1                  map fog off
//   KRUNKNATIVE_CAMERA_FAR=<units>        camera far plane
//   KRUNKNATIVE_SCREENSHOT=<out.png>      after the map has rendered KRUNKNATIVE_CAPTURE_FRAMES
//                                         frames (default 30), write the framebuffer and exit
#include <client.h>

// First thing in main(): with a capture requested and no XDG_CONFIG_HOME given, points it
// at an empty directory so stored user settings cannot change the captured view.
void debug_capture_startup(void);
// Before the world scene renders: applies the pinned pose/FOV to client->camera.
void debug_capture_apply_camera(Client *client);
// 1 when KRUNKNATIVE_HIDE_UI=1 (or a pinned capture is running with it set).
int debug_capture_hide_ui(void);
// After everything for the frame is drawn, before buffers swap: counts frames and writes the PNG.
void debug_capture_frame_end(Client *client);
// "showWindow(3);KE.closeWindow()": inline-handler calls (name + literal args) dispatched on
// the shown page, as the KRUNKNATIVE_UI_CMDS `ui` command does (ui_scenario.c pageActions).
void debug_capture_dispatch(const char *calls);
