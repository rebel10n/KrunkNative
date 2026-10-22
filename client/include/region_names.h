#ifndef KN_REGION_NAMES_H
#define KN_REGION_NAMES_H

#include <stddef.h>

// The regions of the native client's server browser, pickers and seeks (servers_ui.c).
//
// The brain lists each KrunkNative edge region under a v10.0.2 region id (services/brain/registry.c
// registry_v10_region; doc/v10/regions.md), and the client seeks with the id its list gave it. The lobby rows
// keep the edge's own code (tuple[1]: SV, LA, CH, VA, local), which region_id_of maps to that id. The pickers
// show the v10 name of the id (what v10 players see) with the KrunkNative places it holds in brackets.

typedef struct {
    const char *kn;   // the code the lobby rows carry (tuple[1]) and the server.cfg region announces
    const char *code; // the short code the browser shows ("LO" for local)
    const char *name; // the place name KrunkNative shows ("Santa Clara")
    const char *city; // its city, for server names ("Chicago 2")
    const char *v10;  // the v10 region id the brain lists it under
} RegionEdge;

size_t region_edge_count(void);
const RegionEdge *region_edge_at(size_t i);

// The edge of a lobby row's code ("SV"); NULL when the code is not an edge's.
const RegionEdge *region_edge(const char *kn);

// The v10 region id a region is known by: an edge code maps to its id ("SV" -> "us-ca-sv"), anything else
// (a v10 id, an unknown code) is itself. "" for NULL.
const char *region_id_of(const char *region);

// The v10 name of a region id ("New York", "Silicon Valley"); NULL when the table does not know the id
// (shared/include/krunker_regions.h has the table).
const char *region_v10_name(const char *id);

// 1 when a KrunkNative edge is listed under that v10 id (us-ca-sv, us-nj, local): the brain has a node there.
int region_hosted(const char *id);

// The <option> values of the region pickers (Settings > Default Region; the same list whatever the Server
// Type): every id of v10's Default Region dropdown (availableRegions, krunker_regions.h) in v10's order, then
// the ids of KrunkNative's edges it lacks (local), then any other id the matchmaker's ping list names
// (`listed`, listed_n ids), each once. Writes at most `max` ids to out; returns the count.
size_t region_picker_ids(const char *const *listed, size_t listed_n, const char **out, size_t max);

// The picker label of a v10 id: its v10 name, then the edge places it holds in brackets, the edge names that
// differ from it: "Silicon Valley (Santa Clara, Los Angeles)", "New York (Chicago, Ashburn, Virginia)", "Dallas".
// An id the table does not know is its own label. out is NUL-terminated.
void region_picker_label(const char *id, char *out, size_t n);

#endif
