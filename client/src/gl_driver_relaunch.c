// The V-Sync relaunch glue (gl_driver_env.h): the NVIDIA probe, the stored "vsync" and the re-exec.
// Linux only; elsewhere gl_driver_vsync_relaunch does nothing. The decision is gl_driver_env.c.
#include <gl_driver_env.h>

#if defined(__linux__) && !defined(__EMSCRIPTEN__)

#include <cJSON.h>
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

// The stored "vsync" (knSettings): ui_render_settings.c applies it, and a missing key is on
// (.vsync = 1). A string is read with the parse_bool rules, as the store's "true"/"false" strings are.
static int stored_vsync(void) {
    int on = 1;
    char *buf = malloc(KN_SETTINGS_JSON_MAX);
    if (!buf) return on;
    if (html_storage_get(KN_SETTINGS_KEY, buf, KN_SETTINGS_JSON_MAX) >= 0) {
        cJSON *root = cJSON_Parse(buf);
        const cJSON *v = cJSON_IsObject(root) ? cJSON_GetObjectItem(root, "vsync") : NULL;
        if (v && cJSON_IsBool(v)) on = cJSON_IsTrue(v);
        else if (v && cJSON_IsNumber(v)) on = cJSON_GetNumberValue(v) != 0.0;
        else if (v && cJSON_IsString(v)) on = gl_driver_parse_vsync(cJSON_GetStringValue(v));
        cJSON_Delete(root);
    }
    free(buf);
    return on;
}

// The V-Sync in effect: KRUNKNATIVE_VSYNC beats the store for the whole run
// (ui_render_settings.c client_render_settings_init), else the stored value.
static int effective_vsync(void) {
    const char *env = getenv("KRUNKNATIVE_VSYNC");
    if (env && *env) return gl_driver_parse_vsync(env);
    return stored_vsync();
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
    const int vsync = nvidia ? effective_vsync() : 1;
    const int wanted = gl_driver_relaunch_wanted(nvidia, vsync, getenv(GL_DRIVER_SYNC_ENV) != NULL,
                                                 getenv(GL_DRIVER_RELAUNCH_ENV) != NULL, tool_run(argc, argv));
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
