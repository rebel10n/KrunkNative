#pragma once
// v10.0.7's profanity Filter (the bad-words npm package, deob-10.0.7.js:202773-203293, one instance at :214532):
// what Settings > Chat "Chat Filter" (chatFilter) and streamer mode apply to the in-game chat (:228050) and to the
// kill-feed names (:228087): `(tmp.streamMode || tmp.chatFilter) && filter.clean(text)`.
#include <stddef.h>

// Filter.clean(text): text.split(/\b/), every piece the list calls profane (isProfane: some list word w with
// /\b<w>\b/gi matching the piece) rewritten by replaceWord (the characters outside [a-zA-Z0-9|$@] dropped, then
// every \w one turned into "*"), joined again. \b and \w are JS's ASCII ones. Returns 1 with the result in `out`
// (never longer than `in`, cut to `size`), 0 when `in` has no word character at all: v10's
// splitRegex.exec(text)[0] throws a TypeError there and the line is never shown.
int chat_filter_clean(const char *in, char *out, size_t size);

// Filter.isProfane(text) on one piece of `len` bytes.
int chat_filter_is_profane(const char *text, size_t len);
