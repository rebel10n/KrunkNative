// Build identity (shared/include/build_info.h): the only reader of the generated kn_version.h.
#include <build_info.h>
#include <kn_version.h>
#include <shared.h>
#include <stdio.h>

static const KnBuildInfo k_info = {
    .describe = KN_VERSION_DESCRIBE,
    .commit = KN_VERSION_COMMIT,
    .commit_time = (int64_t) KN_VERSION_COMMIT_TIME,
    .dirty = KN_VERSION_DIRTY,
    .build_time = KN_VERSION_BUILD_TIME,
    .protocol_major = NET_PROTOCOL_MAJOR,
    .protocol_minor = NET_PROTOCOL_MINOR,
};

const KnBuildInfo *kn_build_info(void) { return &k_info; }

const char *kn_build_string(void) {
    static char s[192];
    if (!s[0]) {
        snprintf(s, sizeof(s), "%s (%s, built %s, protocol %d.%d)", k_info.describe, k_info.commit[0] ? k_info.commit : "no commit",
                 k_info.build_time, k_info.protocol_major, k_info.protocol_minor);
    }
    return s;
}

const char *kn_build_protocol(void) {
    static char s[16];
    if (!s[0]) snprintf(s, sizeof(s), "%d.%d", k_info.protocol_major, k_info.protocol_minor);
    return s;
}
