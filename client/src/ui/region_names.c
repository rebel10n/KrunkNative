// The region names and ids of the native client's server browser and pickers (region_names.h). The brain's
// lobby rows carry KrunkNative's edge codes; the pickers and seeks use the v10 region id each edge is listed
// under (doc/v10/regions.md). The v10 regions themselves (ids, names, the dropdown's order) are the shared
// table, shared/src/krunker_regions.c.
#include <region_names.h>

#include <krunker_regions.h>

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

const char *region_v10_name(const char *id) { return krunker_region_v10_name(id); }

int region_hosted(const char *id) {
    if (!id || !id[0]) return 0;
    for (size_t i = 0; i < region_edge_count(); i++) {
        if (!strcmp(k_edges[i].v10, id)) return 1;
    }
    return 0;
}

static void add_id(const char **out, size_t *count, const size_t max, const char *id) {
    if (!id || !id[0]) return;
    for (size_t i = 0; i < *count; i++) {
        if (!strcmp(out[i], id)) return;
    }
    if (*count < max) out[(*count)++] = id;
}

size_t region_picker_ids(const char *const *listed, const size_t listed_n, const char **out, const size_t max) {
    size_t count = 0;
    // 1. v10's Default Region dropdown: availableRegions in its order (the whole Krunker list, whatever the
    //    Server Type: krunker.io's regions and ours are the same places)
    const char *dropdown[32];
    const size_t n = krunker_region_dropdown(dropdown, sizeof(dropdown) / sizeof(dropdown[0]));
    for (size_t i = 0; i < n && i < sizeof(dropdown) / sizeof(dropdown[0]); i++) add_id(out, &count, max, dropdown[i]);
    // 2. the ids of KrunkNative's edges not in it (local: the dev host's list)
    for (size_t i = 0; i < region_edge_count(); i++) add_id(out, &count, max, k_edges[i].v10);
    // 3. anything else the matchmaker lists (a node in a region v10 does not list in its dropdown)
    for (size_t i = 0; listed && i < listed_n; i++) add_id(out, &count, max, listed[i]);
    return count;
}

// The label is v10's name alone (user, 2026-10-22: never anything in parentheses after a region name; the
// edge places made the labels too long). The edges a region holds stay in k_edges for the lobby rows.
void region_picker_label(const char *id, char *out, const size_t n) {
    if (!n) return;
    out[0] = 0;
    if (!id) return;
    const char *name = region_v10_name(id);
    snprintf(out, n, "%s", name ? name : id);
}
