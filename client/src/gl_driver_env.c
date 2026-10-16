// The V-Sync relaunch decision and the V-Sync text rules (gl_driver_env.h). No dependencies:
// client/tests/gl_driver_env_test.c links this file alone.
#include <gl_driver_env.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int gl_driver_relaunch_wanted(const int nvidia, const int vsync, const int sync_env_set, const int relaunched,
                              const int tool_run) {
    if (!nvidia || vsync || sync_env_set || relaunched || tool_run) return 0;
    return 1;
}

// ui_render_settings.c parse_number / parse_bool, unchanged
static int parse_number(const char *s, double *out) {
    char *end;
    const double d = strtod(s, &end);
    while (*end && isspace((unsigned char) *end)) end++;
    if (*end || end == s) return 0;
    *out = d;
    return 1;
}

int gl_driver_parse_vsync(const char *s) {
    if (!s || !*s || !strcmp(s, "false")) return 0;
    if (!strcmp(s, "true")) return 1;
    double d;
    return parse_number(s, &d) ? d != 0.0 : 1;
}
