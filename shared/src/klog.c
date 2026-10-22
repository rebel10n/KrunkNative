/* klog.h implementation: the client/server debug output level. */
#include "klog.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int g_kn_debug_level = 0;

// "0", "1" or "2" (surrounding spaces ignored). Returns 0 when s is a valid level.
static int parse_level(const char *s, int *out) {
    if (!s || !*s) return -1;
    char *end = NULL;
    const long v = strtol(s, &end, 10);
    if (end == s) return -1;
    while (*end && isspace((unsigned char) *end)) end++;
    if (*end || v < 0 || v > KN_DEBUG_LEVEL_MAX) return -1;
    *out = (int) v;
    return 0;
}

void kn_debug_init_env(void) {
    int level = 0;
    const char *env = getenv("KRUNKNATIVE_DEBUG");
    if (env && *env && parse_level(env, &level) == 0) g_kn_debug_level = level;
}

int kn_debug_parse_arg(const char *arg, const char *next) {
    if (!arg) return 0;
    int level = 0;
    if (!strcmp(arg, "--debug")) {
        g_kn_debug_level = 1;
        return 1;
    }
    if (!strncmp(arg, "--debug=", 8)) {
        if (parse_level(arg + 8, &level) != 0) {
            fprintf(stderr, "--debug: level must be 0, 1 or 2 (got \"%s\")\n", arg + 8);
            return -1;
        }
        g_kn_debug_level = level;
        return 1;
    }
    if (!strcmp(arg, "-d")) {
        if (parse_level(next, &level) != 0) {
            fprintf(stderr, "-d: level must be 0, 1 or 2 (got \"%s\")\n", next ? next : "");
            return -1;
        }
        g_kn_debug_level = level;
        return 2;
    }
    return 0;
}
