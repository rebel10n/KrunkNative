#ifndef KN_KRUNKER_REGIONS_H
#define KN_KRUNKER_REGIONS_H

#include <stddef.h>

/* Krunker v10.0.3's matchmaker regions, one table for every reader (shared/src/krunker_regions.c;
 * doc/v10/regions.md): the client's pickers and ping race (client/src/ui/region_names.c, servers_ui.c,
 * krunker_lobbies.c), the brain's /mm/ping-list keys and its seek routing (services/brain/registry.c).
 *
 * From the 10.0.3 bundle (~/.cache/kn-v103/deob-10.0.3.js; the same values in v10.0.2's
 * ~/.cache/kn-v10/v10.js at 16443-16469 and 18672-18701):
 *   - availableRegions, line 19306: the production list, in the order the Default Region dropdown
 *     shows it (settings list defaultRegion.options, v10.js:203294); "sss" is pushed on the beta host
 *     only (19308), ["local"] is the list of a dev host (19311)
 *   - dedicatedOnlyRegions, line 19313: regions that only host dedicated (custom) games; getRegion
 *     never pings them and the dropdown does not list them
 *   - the region -> continent table, lines 19315-19337 (1 oc, 2 as, 3 eu, 4 na, 5 sa: 19338-19344)
 *   - the display names, lines 21566-21596 (v10.0.2's _7517)
 * The city coordinates are KrunkNative's (doc/v10/regions.md), for the nearest-region rule. */

enum {
    KR_AVAILABLE = 1 << 0,      /* in availableRegions (production): listed by the dropdown and pinged by getRegion */
    KR_DEDICATED_ONLY = 1 << 1, /* in dedicatedOnlyRegions: never pinged, not in the dropdown */
    KR_BETA_ONLY = 1 << 2,      /* "sss": pushed onto availableRegions on the beta host only */
    KR_LOCAL = 1 << 3,          /* "local": the dev host's whole list; no place (no coordinates) */
    KR_NAME_ONLY = 1 << 4,      /* a name in the table with no list membership (an old or future region) */
};

typedef struct KrunkerRegion {
    const char *id;        /* the matchmaker id ("us-ca-sv") */
    const char *name;      /* v10's display name ("Silicon Valley") */
    const char *continent; /* "na", "eu", "as", "oc", "sa"; "" when v10 lists none */
    double lat, lon;       /* the region's city (degrees); 0,0 with KR_LOCAL */
    unsigned flags;        /* KR_* */
} KrunkerRegion;

/* The whole table in order: availableRegions (production order), then sss, the dedicated-only ids,
 * the names-only ids, and local last. */
size_t krunker_region_count(void);
const KrunkerRegion *krunker_region_at(size_t i);
/* By id; NULL when v10 has no such region. */
const KrunkerRegion *krunker_region(const char *id);
/* v10's display name of an id, NULL when unknown (the client's krunker_region_name, krunker_lobbies.h, wraps
 * it with a fallback). */
const char *krunker_region_v10_name(const char *id);

/* The ids of v10's Default Region dropdown (availableRegions, production), in its order: `out` gets at
 * most `max` ids; the count is returned (KRUNKER_REGION_COUNT, krunker_build.h). */
size_t krunker_region_dropdown(const char **out, size_t max);

/* Whether getRegion would ping a listed key of that id: an available (or beta / local) id that is not
 * dedicated-only. The brain lists a node under its id only when this holds (doc/v10/regions.md). */
int krunker_region_pinged(const char *id);

/* Great-circle distance between two regions' cities in km; -1 when either is unknown or has no place
 * (local). */
double krunker_region_distance_km(const char *a, const char *b);

/* The nearest of `hosted` (n ids, the regions some node serves) to the region `id` a player asked
 * for, as an index into `hosted`; -1 only when n == 0. The rule (doc/v10/regions.md "Routing"):
 *   1. with `pings` (n values, the player's measured round trip to each hosted region's ping host in
 *      ms, < 0 = not measured / failed): the hosted region with the lowest measured ping ("nearest"
 *      in Krunker is the lowest latency: getRegion's race)
 *   2. else the hosted region whose city is nearest to id's by great circle; hosted regions without
 *      a place (local, an unknown code) come after every placed one
 *   3. an id with no place (local, unknown) and no pings: the first hosted region
 * Ties keep the first in `hosted`. A hosted region equal to `id` is always it. */
int krunker_region_nearest(const char *id, const char *const *hosted, size_t n, const double *pings);

/* http(s)://<value>/ping of a /ping-list value, v10 getRegion's rule (v10.js:147489-147491): a value
 * starting "ip_", "localhost" or "127.0.0.1" keeps its port and is http:// unless that port is 443;
 * any other host is https://<host>/ping without its port. KrunkNative's brains list "host:6767" for
 * a plain-HTTP brain (doc/backend.md /mm/ping-list): kept as http://host:6767/ping. */
void krunker_region_ping_url(const char *value, char *out, size_t n);

#endif
