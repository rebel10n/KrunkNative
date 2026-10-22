// Krunker v10.0.3's matchmaker regions (krunker_regions.h): the table every reader shares, the
// nearest-region rule of the brain's seek routing and the ping URL rule of getRegion.
#include <krunker_regions.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

// Sources: deob-10.0.3.js 19306 (availableRegions, the dropdown's order), 19313 (dedicatedOnlyRegions),
// 19315-19344 (continents), 21566-21596 (names). Coordinates: the region's city (doc/v10/regions.md).
static const KrunkerRegion k_regions[] = {
    // availableRegions, production, in v10's order
    {"de-fra", "Frankfurt", "eu", 50.11, 8.68, KR_AVAILABLE},
    {"us-ca-sv", "Silicon Valley", "na", 37.35, -121.95, KR_AVAILABLE}, // Santa Clara
    {"au-syd", "Sydney", "oc", -33.87, 151.21, KR_AVAILABLE},
    {"jb-hnd", "Tokyo", "as", 35.68, 139.69, KR_AVAILABLE},
    {"sgp", "Singapore", "as", 1.35, 103.82, KR_AVAILABLE},
    {"us-nj", "New York", "na", 40.71, -74.01, KR_AVAILABLE},
    {"as-mb", "Mumbai", "as", 19.08, 72.88, KR_AVAILABLE},
    {"us-tx", "Dallas", "na", 32.78, -96.80, KR_AVAILABLE},
    {"brz", "Brazil", "sa", -23.55, -46.63, KR_AVAILABLE}, // Sao Paulo
    {"me-bhn", "Middle East", "as", 26.23, 50.59, KR_AVAILABLE}, // Bahrain (Manama)
    // pushed onto availableRegions on the beta host only
    {"sss", "EU Super Secret Servers", "eu", 50.11, 8.68, KR_BETA_ONLY}, // placed with Frankfurt (eu)
    // dedicatedOnlyRegions: custom games only, never pinged, not in the dropdown
    {"us-wa", "Seattle", "", 47.61, -122.33, KR_DEDICATED_ONLY},
    {"us-il", "Chicago", "", 41.88, -87.63, KR_DEDICATED_ONLY},
    {"gb-lon", "London", "eu", 51.51, -0.13, KR_DEDICATED_ONLY},
    {"mx-cmx", "Mexico", "sa", 19.43, -99.13, KR_DEDICATED_ONLY}, // Mexico City (v10 files it under 5, sa)
    {"as-seoul", "South Korea", "as", 37.57, 126.98, KR_DEDICATED_ONLY}, // Seoul
    // names only: in the name table (and some in the continent table), in no list
    {"us-ca-la", "Los Angeles", "", 34.05, -118.24, KR_NAME_ONLY},
    {"us-ga", "Atlanta", "", 33.75, -84.39, KR_NAME_ONLY},
    {"nl-ams", "Amsterdam", "", 52.37, 4.90, KR_NAME_ONLY},
    {"fr-par", "Paris", "", 48.86, 2.35, KR_NAME_ONLY},
    {"us-fl", "Miami", "na", 25.76, -80.19, KR_NAME_ONLY},
    {"blr", "India", "as", 12.97, 77.59, KR_NAME_ONLY}, // Bangalore
    {"af-ct", "South Africa", "eu", -33.92, 18.42, KR_NAME_ONLY}, // Cape Town (v10 files it under 3, eu)
    {"eu-stm", "Sweden", "eu", 59.33, 18.07, KR_NAME_ONLY}, // Stockholm
    {"tor", "Toronto", "na", 43.65, -79.38, KR_NAME_ONLY},
    {"pol", "Poland", "eu", 52.23, 21.01, KR_NAME_ONLY}, // Warsaw
    {"us-hi", "Hawaii", "na", 21.31, -157.86, KR_NAME_ONLY}, // Honolulu
    {"iow", "Iowa", "", 41.59, -93.62, KR_NAME_ONLY}, // Des Moines
    // the dev host's list
    {"local", "Local", "", 0, 0, KR_LOCAL},
};

size_t krunker_region_count(void) { return sizeof(k_regions) / sizeof(k_regions[0]); }

const KrunkerRegion *krunker_region_at(const size_t i) { return i < krunker_region_count() ? &k_regions[i] : NULL; }

const KrunkerRegion *krunker_region(const char *id) {
    if (!id || !id[0]) return NULL;
    for (size_t i = 0; i < krunker_region_count(); i++) {
        if (!strcmp(id, k_regions[i].id)) return &k_regions[i];
    }
    return NULL;
}

const char *krunker_region_v10_name(const char *id) {
    const KrunkerRegion *r = krunker_region(id);
    return r ? r->name : NULL;
}

size_t krunker_region_dropdown(const char **out, const size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < krunker_region_count(); i++) {
        if (!(k_regions[i].flags & KR_AVAILABLE)) continue;
        if (out && n < max) out[n] = k_regions[i].id;
        n++;
    }
    return n;
}

int krunker_region_pinged(const char *id) {
    const KrunkerRegion *r = krunker_region(id);
    if (!r || (r->flags & KR_DEDICATED_ONLY)) return 0;
    return (r->flags & (KR_AVAILABLE | KR_BETA_ONLY | KR_LOCAL)) != 0;
}

static int placed(const KrunkerRegion *r) { return r && !(r->flags & KR_LOCAL); }

static double rad(const double deg) { return deg * 3.14159265358979323846 / 180.0; }

double krunker_region_distance_km(const char *a, const char *b) {
    const KrunkerRegion *ra = krunker_region(a), *rb = krunker_region(b);
    if (!placed(ra) || !placed(rb)) return -1;
    // haversine on a 6371 km sphere
    const double dlat = rad(rb->lat - ra->lat), dlon = rad(rb->lon - ra->lon);
    const double h = sin(dlat / 2) * sin(dlat / 2) + cos(rad(ra->lat)) * cos(rad(rb->lat)) * sin(dlon / 2) * sin(dlon / 2);
    return 2 * 6371.0 * asin(sqrt(h < 0 ? 0 : h > 1 ? 1 : h));
}

int krunker_region_nearest(const char *id, const char *const *hosted, const size_t n, const double *pings) {
    if (!hosted || n == 0) return -1;
    for (size_t i = 0; i < n; i++) {
        if (id && hosted[i] && !strcmp(id, hosted[i])) return (int) i; // hosted as asked
    }
    // 1. the lowest measured ping
    if (pings) {
        int best = -1;
        for (size_t i = 0; i < n; i++) {
            if (pings[i] < 0) continue;
            if (best < 0 || pings[i] < pings[best]) best = (int) i;
        }
        if (best >= 0) return best;
    }
    // 2. the nearest city; regions without a place after every placed one
    int best = -1;
    double best_km = -1;
    for (size_t i = 0; i < n; i++) {
        const double km = krunker_region_distance_km(id, hosted[i]);
        if (km < 0) continue;
        if (best < 0 || km < best_km) {
            best = (int) i;
            best_km = km;
        }
    }
    // 3. nothing placed (the asked id or every hosted one has no place): the first hosted region
    return best >= 0 ? best : 0;
}

static int starts_with(const char *s, const char *prefix) { return !strncmp(s, prefix, strlen(prefix)); }

static int ends_with(const char *s, const char *suffix) {
    const size_t n = strlen(s), m = strlen(suffix);
    return n >= m && !strcmp(s + n - m, suffix);
}

void krunker_region_ping_url(const char *value, char *out, const size_t n) {
    if (!out || !n) return;
    const char *v = value ? value : "";
    // v10: proto = endsWith(":443") ? "https:" : "http:"; anything but ip_ / localhost / 127.0.0.1
    // is https: with the port dropped. KrunkNative: a brain's "host:6767" stays plain HTTP.
    if (starts_with(v, "ip_") || starts_with(v, "localhost") || starts_with(v, "127.0.0.1") || ends_with(v, ":6767")) {
        snprintf(out, n, "%s//%s/ping", ends_with(v, ":443") ? "https:" : "http:", v);
    } else {
        snprintf(out, n, "https://%.*s/ping", (int) strcspn(v, ":"), v);
    }
}
