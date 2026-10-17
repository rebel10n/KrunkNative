// The V-Sync relaunch glue (gl_driver_env.h): the NVIDIA probe, the stored "vsync" and the re-exec.
// Linux only; elsewhere gl_driver_vsync_relaunch does nothing. The decision is gl_driver_env.c.
#include <gl_driver_env.h>

#if defined(__linux__) && !defined(__EMSCRIPTEN__)

#include <errno.h>
#include <headless.h> // headless_env_flag_on: the KRUNKNATIVE_HEADLESS rule (headless.c)
#include <html_dom.h> // html_storage_get: the knSettings entry (html_dom_core.c)
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define KN_SETTINGS_KEY "knSettings"      // ui_settings.c UI_SETTINGS_STORAGE_KEY
#define KN_SETTINGS_JSON_MAX (64 * 1024)  // ui_settings.c UI_SETTINGS_JSON_MAX

// The NVIDIA kernel module's version file exists while the NVIDIA driver is loaded.
static int nvidia_driver_present(void) {
    return access("/proc/driver/nvidia/version", F_OK) == 0;
}

// The stored "vsync" (knSettings), read the way the client reads it at startup (ui_settings_load: the same
// entry, the same 64 KiB buffer). `what` says what the store held, for the log line. No file, a file over
// the buffer (its JSON would be cut) or no usable key: on (.vsync = 1).
static int stored_vsync(char *what, const size_t n) {
    int found = 0;
    int on = 1;
    char *buf = malloc(KN_SETTINGS_JSON_MAX);
    const int size = buf ? html_storage_get(KN_SETTINGS_KEY, buf, KN_SETTINGS_JSON_MAX) : -1;
    if (size < 0) {
        snprintf(what, n, "no knSettings");
    } else if (size >= KN_SETTINGS_JSON_MAX) {
        snprintf(what, n, "knSettings over 64 KiB");
    } else {
        on = gl_driver_vsync_from_settings(buf, &found);
        if (found) snprintf(what, n, "vsync %s", on ? "true" : "false");
        else snprintf(what, n, "no usable vsync");
    }
    free(buf);
    return on;
}

// Tests, agents and tools never relaunch: headless (KRUNKNATIVE_HEADLESS or --headless), the render
// scenario, a debug capture (debug_capture.c) and the flags that exit before any GL is needed.
static int tool_run(const int argc, char **argv) {
    static const char *const flags[] = {"--headless", "--render-scenario", "--version", "--voice-info", "--help", "-h"};
    if (headless_env_flag_on(getenv("KRUNKNATIVE_HEADLESS"))) return 1;
    const char *camera = getenv("KRUNKNATIVE_CAMERA");
    const char *screenshot = getenv("KRUNKNATIVE_SCREENSHOT");
    if ((camera && *camera) || (screenshot && *screenshot)) return 1;
    for (int i = 1; i < argc; i++) {
        for (size_t f = 0; f < sizeof(flags) / sizeof(flags[0]); f++) {
            if (!strcmp(argv[i], flags[f])) return 1;
        }
    }
    return 0;
}

void gl_driver_vsync_relaunch(const int argc, char **argv) {
    // the store is read only where the driver reads the variable
    const int nvidia = nvidia_driver_present();
    if (!nvidia) return;
    char stored[48];
    const int stored_on = stored_vsync(stored, sizeof(stored));
    // KRUNKNATIVE_VSYNC beats the store for the whole run (ui_render_settings.c client_render_settings_init)
    const char *env_vsync = getenv("KRUNKNATIVE_VSYNC");
    const int overridden = env_vsync && *env_vsync;
    const int vsync = overridden ? gl_driver_parse_vsync(env_vsync) : stored_on;
    const char *sync_env = getenv(GL_DRIVER_SYNC_ENV);
    const int relaunched = getenv(GL_DRIVER_RELAUNCH_ENV) != NULL;
    const int tool = tool_run(argc, argv);
    const int wanted = gl_driver_relaunch_wanted(nvidia, vsync, sync_env != NULL, relaunched, tool);
    if (!tool) { // the decision, once per process: why it did or did not relaunch
        const char *reason = wanted ? "relaunching with __GL_SYNC_TO_VBLANK=0"
            : vsync ? "V-Sync is on"
            : sync_env ? "__GL_SYNC_TO_VBLANK is already set"
            : relaunched ? "already relaunched"
            : "no relaunch";
        char source[96];
        if (overridden) snprintf(source, sizeof(source), "KRUNKNATIVE_VSYNC=%s", env_vsync);
        else snprintf(source, sizeof(source), "store");
        char sync_state[64];
        if (sync_env) snprintf(sync_state, sizeof(sync_state), "set to %s", sync_env);
        else snprintf(sync_state, sizeof(sync_state), "unset");
        fprintf(stderr, "[gl] V-Sync relaunch: %s (%s); V-Sync %s from %s; stored: %s; __GL_SYNC_TO_VBLANK %s\n",
                wanted ? "yes" : "no", reason, vsync ? "on" : "off", source, stored, sync_state);
    }
    if (!wanted) return;

    // the same executable and the same argv (argv is the process's own, untouched by main so far)
    char exe[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return; // no /proc: start as before, without the variable
    exe[n] = 0;

    printf("[gl] relaunching with __GL_SYNC_TO_VBLANK=0 (V-Sync off)\n");
    fflush(stdout);
    fflush(stderr);
    setenv(GL_DRIVER_SYNC_ENV, "0", 1);
    setenv(GL_DRIVER_RELAUNCH_ENV, "1", 1);
    execv(exe, argv);
    // The exec failed: this process carries on, and the driver must not see a variable it was not meant to.
    fprintf(stderr, "[gl] relaunch failed: %s\n", strerror(errno));
    unsetenv(GL_DRIVER_SYNC_ENV);
    unsetenv(GL_DRIVER_RELAUNCH_ENV);
}

#else

void gl_driver_vsync_relaunch(const int argc, char **argv) {
    (void) argc;
    (void) argv;
}

#endif
