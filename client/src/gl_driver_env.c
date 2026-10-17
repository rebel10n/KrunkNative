// The V-Sync relaunch decision and the V-Sync text rules (gl_driver_env.h). Pure: the one dependency is
// cJSON (the knSettings text); client/tests/gl_driver_env_test.c links this file and cJSON.
#include <gl_driver_env.h>

#include <cJSON.h>
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

int gl_driver_vsync_from_settings(const char *json, int *found) {
    *found = 0;
    if (!json) return 1;
    cJSON *root = cJSON_Parse(json);
    const cJSON *v = cJSON_IsObject(root) ? cJSON_GetObjectItem(root, "vsync") : NULL;
    int on = 1;
    if (v && cJSON_IsBool(v)) {
        on = cJSON_IsTrue(v);
        *found = 1;
    } else if (v && cJSON_IsNumber(v)) {
        on = cJSON_GetNumberValue(v) != 0.0;
        *found = 1;
    } else if (v && cJSON_IsString(v)) {
        on = gl_driver_parse_vsync(cJSON_GetStringValue(v));
        *found = 1;
    }
    cJSON_Delete(root);
    return on;
}
