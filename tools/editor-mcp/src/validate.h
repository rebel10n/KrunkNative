// Map validation (map_validate) and the native-map helpers validation, compile and playtest
// share: the document parsed by KrunkNative's own map code (shared/src/map.c map_init) so the
// checks see exactly what the game loads.
#pragma once
#include "ctx.h"
#include <shared.h>

typedef struct {
    cJSON *json;      // map form (doc_map_form), sanitised; map->raw_data points here
    Map *map;
    int *ids;         // document object id of map->objects[i]
    double radius, height, climb;
} NativeMap;

int native_map_load(const Doc *doc, NativeMap *out, char *err, size_t errsz);
void native_map_free(NativeMap *nm);
// Index of the first object a standing player at feet p would intersect, -1 none.
int native_spawn_blocker(const NativeMap *nm, const double p[3]);
// Top of the highest solid under the player's footprint at or below p.y + climb; -1e30 none.
double native_ground(const NativeMap *nm, const double p[3]);

// Runs every check; opts: {reachability: bool, max_diagnostics: int, checks: [names]}.
cJSON *validate_project(Ctx *ctx, Project *p, const cJSON *opts);

// KrunkScript bridge (ks_bridge.c): validates a map's scripts.{client,server}. Adds
// diagnostics {severity, code, message, side, line, col} to `diags`; returns the error count.
// *available = 0 when the compiler is not linked (the stub).
int ks_bridge_check_scripts(const cJSON *map_json, cJSON *diags, int *available);
// One script's source (decoded) on side server (1) / client (0): the same diagnostics; returns
// 1 on a compile error.
int ks_bridge_check_source(const char *src, size_t len, int server, cJSON *diags, int *available);
// Decodes a scripts.<side> value (base64 or raw) into malloc'd source text.
char *ks_bridge_decode(const char *text, size_t *len);
const char *ks_bridge_status(void);
