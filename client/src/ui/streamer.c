// Streamer mode name scrambling: Krunker v3.1.1 UTILS.scrambleS. See client/include/streamer.h.
#include <streamer.h>
#include <player_names.h>
#include <render_settings.h>

#include <string.h>

// Byte length of the UTF-8 sequence starting at `s` (1 for stray continuation bytes).
static size_t streamer_cp_len(const unsigned char *s) {
    size_t n = 1;
    if (*s >= 0xF0) n = 4;
    else if (*s >= 0xE0) n = 3;
    else if (*s >= 0xC0) n = 2;
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return i;
    }
    return n;
}

// Appends every code point of `in` whose index has parity `odd`.
static void streamer_append(const char *in, const int odd, char *out, const size_t size, size_t *len) {
    int index = 0;
    for (const unsigned char *p = (const unsigned char *) in; *p; index++) {
        const size_t n = streamer_cp_len(p);
        if ((index & 1) == odd) {
            if (*len + n >= size) return;
            memcpy(out + *len, p, n);
            *len += n;
        }
        p += n;
    }
}

// l.replace(/.(.)?/g, "$1") keeps the odd-indexed characters; ("d" + l) shifted by one
// keeps the even-indexed ones of l.
void streamer_scramble(const char *in, char *out, const size_t size) {
    if (!out || !size) return;
    size_t len = 0;
    if (in) {
        streamer_append(in, 1, out, size, &len);
        streamer_append(in, 0, out, size, &len);
    }
    out[len] = 0;
}

int streamer_mode(void) {
    return client_render_settings()->stream_mode;
}

const char *streamer_name(const char *name, char *out, const size_t size) {
    // a name the Hide player names setting already replaced (player_names.h) is not scrambled again
    if (!name || !streamer_mode() || player_names_hidden()) return name;
    streamer_scramble(name, out, size);
    return out;
}
