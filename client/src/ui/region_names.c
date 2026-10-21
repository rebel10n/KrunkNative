// The region names and ids of the native client's server browser and pickers (region_names.h). The brain's
// lobby rows carry KrunkNative's edge codes; the pickers and seeks use the v10 region id each edge is listed
// under (doc/v10/regions.md).
#include <region_names.h>

#include <stdio.h>
#include <string.h>

// KrunkNative's edges. v10 ids: services/brain/registry.c k_v10_regions.
static const RegionEdge k_edges[] = {
    {"SV", "SV", "Santa Clara", "Santa Clara", "us-ca-sv"},
    {"LA", "LA", "Los Angeles", "Los Angeles", "us-ca-sv"},
    {"CH", "CH", "Chicago", "Chicago", "us-nj"},
    {"VA", "VA", "Ashburn, Virginia", "Ashburn", "us-nj"},
    {"local", "LO", "Local", "Local", "local"},
};

// v10 _7517 (v10.js:18672-18701): the region names of Krunker's server browser, and "local" (the dev host)
static const struct {
    const char *id, *name;
} k_v10_names[] = {
    {"us-nj", "New York"}, {"us-il", "Chicago"}, {"us-tx", "Dallas"}, {"us-wa", "Seattle"},
    {"us-ca-la", "Los Angeles"}, {"us-ga", "Atlanta"}, {"nl-ams", "Amsterdam"}, {"gb-lon", "London"},
    {"de-fra", "Frankfurt"}, {"us-ca-sv", "Silicon Valley"}, {"au-syd", "Sydney"}, {"fr-par", "Paris"},
    {"jb-hnd", "Tokyo"}, {"us-fl", "Miami"}, {"sgp", "Singapore"}, {"blr", "India"}, {"as-mb", "Mumbai"},
    {"brz", "Brazil"}, {"me-bhn", "Middle East"}, {"af-ct", "South Africa"}, {"as-seoul", "South Korea"},
    {"mx-cmx", "Mexico"}, {"eu-stm", "Sweden"}, {"tor", "Toronto"}, {"pol", "Poland"}, {"us-hi", "Hawaii"},
    {"sss", "EU Super Secret Servers"}, {"iow", "Iowa"}, {"local", "Local"},
};

size_t region_edge_count(void) { return sizeof(k_edges) / sizeof(k_edges[0]); }

const RegionEdge *region_edge_at(const size_t i) { return i < region_edge_count() ? &k_edges[i] : NULL; }

const RegionEdge *region_edge(const char *kn) {
    if (!kn) return NULL;
    for (size_t i = 0; i < region_edge_count(); i++) {
        if (!strcmp(kn, k_edges[i].kn)) return &k_edges[i];
    }
    return NULL;
}

const char *region_id_of(const char *region) {
    if (!region) return "";
    const RegionEdge *e = region_edge(region);
    return e ? e->v10 : region;
}

const char *region_v10_name(const char *id) {
    if (!id) return NULL;
    for (size_t i = 0; i < sizeof(k_v10_names) / sizeof(k_v10_names[0]); i++) {
        if (!strcmp(id, k_v10_names[i].id)) return k_v10_names[i].name;
    }
    return NULL;
}

void region_picker_label(const char *id, char *out, const size_t n) {
    if (!n) return;
    out[0] = 0;
    if (!id) return;
    const char *name = region_v10_name(id);
    if (!name) {
        snprintf(out, n, "%s", id);
        return;
    }
    snprintf(out, n, "%s", name);
    size_t used = strlen(out);
    int opened = 0;
    for (size_t i = 0; i < region_edge_count(); i++) {
        const RegionEdge *e = &k_edges[i];
        if (strcmp(e->v10, id) || !strcmp(e->name, name)) continue;
        int n_written = snprintf(out + used, n - used, "%s%s", opened ? ", " : " (", e->name);
        if (n_written < 0 || (size_t) n_written >= n - used) break;
        used += (size_t) n_written;
        opened = 1;
    }
    if (opened && used + 1 < n) {
        out[used] = ')';
        out[used + 1] = 0;
    }
}
