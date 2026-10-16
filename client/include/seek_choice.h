#ifndef KN_SEEK_CHOICE_H
#define KN_SEEK_CHOICE_H

#include <stddef.h>

#include <cJSON.h>

// The decisions a matchmaker seek takes from its inputs, without a window (servers_ui.c runs them):
//   - which region the seek is made in (seek_region_choose: the ping list, the saved defaultRegion
//     setting, the cached pingRegion7);
//   - whether a seek answer names a lobby that is already full (seek_answer_full), which is never
//     joined.

typedef enum {
    SEEK_REGION_SAVED = 0,  // the saved Default Region, listed (or the list was not there to check it)
    SEEK_REGION_UNLISTED,   // the saved Default Region is not in the ping list: an available region is sought instead
    SEEK_REGION_CACHED,     // no saved Default Region: the cached region, listed
    SEEK_REGION_FIRST,      // no saved Default Region and no listed cache: the list's first region
    SEEK_REGION_NONE,       // nothing known: out is the cache or "local"
} SeekRegionChoice;

// list: the /mm/ping-list object, NULL when the matchmaker did not answer. saved / cached: the
// defaultRegion setting and the pingRegion7 cache, "" when unset. The chosen region id is written to
// out (NUL-terminated, n bytes). A saved region the list does not name is replaced quietly by an
// available one (the first listed; SEEK_REGION_UNLISTED): no error is shown, the seek goes on.
SeekRegionChoice seek_region_choose(const cJSON *list, const char *saved, const char *cached, char *out, size_t n);

// 1 when the seek answer's lobby is full: its "players" have reached its "maxPlayers" (both numbers,
// maxPlayers above 0). An answer without the counts (an older brain) is not full.
int seek_answer_full(const cJSON *answer);

#endif
