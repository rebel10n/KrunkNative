#pragma once
// V-Sync off on NVIDIA (Linux): the driver reads __GL_SYNC_TO_VBLANK when its GL library loads.
// A value set by the process after that is ignored, so V-Sync off in the client did not lift the
// display's 120 fps cap on the NVIDIA driver. main() therefore calls gl_driver_vsync_relaunch()
// before any GL library loads: when V-Sync is off it re-executes this client once with the variable
// set, so the driver reads it at load. main() calls it once the flavor is resolved (--flavor /
// KN_FLAVOR pick the knSettings it reads) and passes the argv as typed (main keeps a copy of it).
//
// - client/src/gl_driver_env.c: the decision and the text rules (pure; cJSON for the knSettings text;
//   client/tests/gl_driver_env_test.c).
// - client/src/gl_driver_relaunch.c: the glue (the NVIDIA probe, the knSettings read, the log line, execv).
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

// The stored "vsync" of a knSettings text (ui_settings.c's store): a bool, a number (0 is off) or a
// "true"/"false" string, read with gl_driver_parse_vsync. *found is 1 when the key holds one of those and
// the return value is it. Otherwise (NULL, not JSON, not an object, no usable key) *found is 0 and the
// return is on, the default (ui_render_settings.c .vsync = 1).
int gl_driver_vsync_from_settings(const char *json, int *found);

// main(), first thing: may re-execute the process (never returns from a successful relaunch). When
// it does not relaunch, or the exec fails, it returns and the client starts as before.
void gl_driver_vsync_relaunch(int argc, char **argv);
