// KrunkScript GAME.* table (doc/v7/krunkscript.md 3.x, 6.5) built from
// KS_API_LIST in ks_api.h.
#include <string.h>

#include "ks_api.h"

#define KS_RT_ANY    {KS_T_ANY, 0}
#define KS_RT_NUM    {KS_T_NUM, 0}
#define KS_RT_STR    {KS_T_STR, 0}
#define KS_RT_BOOL   {KS_T_BOOL, 0}
#define KS_RT_OBJ    {KS_T_OBJ, 0}
#define KS_RT_OBJARR {KS_T_OBJ, 1}

#define KS_SIDE_C KS_API_SIDE_CLIENT
#define KS_SIDE_S KS_API_SIDE_SERVER
#define KS_SIDE_B KS_API_SIDE_BOTH

static const KsApiEntry ks_api_table[KS_API_COUNT] = {
#define KS_API_X_ROW(id, path, name, parent, kind, sides, ret) \
    {path, name, KS_API_KIND_##kind, KS_SIDE_##sides, KS_RT_##ret, parent},
    KS_API_LIST(KS_API_X_ROW)
#undef KS_API_X_ROW
};

const KsApiEntry *ks_api_entry(int id) {
    if (id < 0 || id >= KS_API_COUNT) return NULL;
    return &ks_api_table[id];
}

int ks_api_find(const char *path) {
    if (!path) return -1;
    if (strncmp(path, "GAME.", 5) == 0) path += 5;
    for (int i = 0; i < KS_API_COUNT; i++)
        if (strcmp(ks_api_table[i].path, path) == 0) return i;
    return -1;
}

int ks_api_child(int parent, const char *name) {
    for (int i = 0; i < KS_API_COUNT; i++)
        if (ks_api_table[i].parent == parent && strcmp(ks_api_table[i].name, name) == 0) return i;
    return -1;
}
