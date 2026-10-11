#pragma once
// Streamer mode name scrambling (client/src/ui/streamer.c): Krunker v3.1.1's
// UTILS.scrambleS (src/libs/utils.js) and the `streamerModeEnabled && Filter ?
// UTILS.scrambleS(alias) : alias` pattern of src/index.js (Filter is always loaded, so
// it reduces to the streamMode setting). v3.1.1 applies it to the voice speaker key
// (voiceChat), the kill feed and actionFeed names, the kill card (#kCName), both
// scoreboards (updateLeaderboard), the end-screen tables (showEndTable, link text only),
// player chat senders (addChat / addTradeChat) and unboxing lines.
#include <stddef.h>

// UTILS.scrambleS(s): the odd-indexed characters followed by the even-indexed ones
// ("Guest_1" -> "us_Get1"). Counts UTF-8 code points (JS counts UTF-16 units; the same
// for every BMP character). `out` always ends up NUL-terminated (truncated to `size`).
void streamer_scramble(const char *in, char *out, size_t size);

// streamerModeEnabled ? scrambleS(name) : name. Returns `out` when it scrambled, else
// `name` itself (NULL stays NULL).
const char *streamer_name(const char *name, char *out, size_t size);

// settings streamMode (window.toggleStrm): 1 while streamer mode is on.
int streamer_mode(void);
