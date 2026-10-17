/*
 * services/brain/push_rules.c - the Web Push subscription rules (push_rules.h). Pure C.
 */
#include "push_rules.h"

#include <string.h>

int push_endpoint_ok(const char *endpoint) {
    if (!endpoint) return 0;
    const size_t n = strlen(endpoint);
    if (n > PUSH_ENDPOINT_MAX || strncmp(endpoint, "https://", 8) != 0) return 0;
    if (n <= 8 || endpoint[8] == '/') return 0; /* a host follows the scheme */
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char) endpoint[i];
        if (c <= 0x20 || c >= 0x7f) return 0; /* no space, no control byte, no non-ASCII */
    }
    return 1;
}

int push_b64url_ok(const char *s, const size_t bytes) {
    if (!s) return 0;
    const size_t want = (bytes * 4 + 2) / 3; /* unpadded base64 length: 65 bytes -> 87, 16 bytes -> 22 */
    if (strlen(s) != want) return 0;
    for (size_t i = 0; i < want; i++) {
        const char c = s[i];
        const int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return 0;
    }
    return 1;
}
