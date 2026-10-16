#pragma once
// V-Sync off on NVIDIA (Linux): the driver reads __GL_SYNC_TO_VBLANK when its GL library loads.
// A value set by the process after that is ignored, so V-Sync off in the client did not lift the
// display's 120 fps cap on the NVIDIA driver. main() therefore calls gl_driver_vsync_relaunch()
// before anything else: when V-Sync is off it re-executes this client once with the variable set,
// so the driver reads it at load.
//
// - client/src/gl_driver_env.c: the decision (pure, no dependencies; client/tests/gl_driver_env_test.c).
// - client/src/gl_driver_relaunch.c: the glue (the NVIDIA probe, the stored "vsync", execv).
//
// Linux only (the glue is empty elsewhere): the decision is the same on every platform.

#define GL_DRIVER_SYNC_ENV "__GL_SYNC_TO_VBLANK"
#define GL_DRIVER_RELAUNCH_ENV "KRUNKNATIVE_VSYNC_RELAUNCHED" // set in the relaunched process: no second relaunch

// 1 = relaunch with __GL_SYNC_TO_VBLANK=0. All of: the NVIDIA driver is present (nvidia), V-Sync is
// off (vsync 0), the user has not set __GL_SYNC_TO_VBLANK (sync_env_set: any value, which wins),
// this process is not the relaunch (relaunched), and it is not a tool run (tool_run: headless,
// a render scenario or a debug capture, or a flag that exits before GL).
int gl_driver_relaunch_wanted(int nvidia, int vsync, int sync_env_set, int relaunched, int tool_run);

// The V-Sync value of a text form (KRUNKNATIVE_VSYNC or the stored "vsync"): ui_render_settings.c's
// parse_bool rules, so the relaunch sees the same value the client applies. NULL / "" / "false" are
// off, "true" on, numbers by their value, other text on.
int gl_driver_parse_vsync(const char *text);

// main(), first thing: may re-execute the process (never returns from a successful relaunch). When
// it does not relaunch, or the exec fails, it returns and the client starts as before.
void gl_driver_vsync_relaunch(int argc, char **argv);
