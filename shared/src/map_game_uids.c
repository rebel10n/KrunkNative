// Krunker v10.0.3's GameObject uids of a map (shared/include/map_game_uids.h). The builder rules, from map.generate's
// dispatch (deob-10.0.3.js:188770-189760) and each MapManager builder's push:
//   one GameObject unless noCol (l || col):  CYLINDER :186723, PLANE / BILLBOARD / RUNE (addFixedPlane) :187819,
//                                            RAMP :185837, the models (addMesh, EVENT among them) :187059
//   one unless noCol and not dyn:            CUBE (addBlock) :186821, CUSTOM_ASSET (addAsset) :187197
//   always one:                              the zones (:185025-185268), CHECK_POINT, TELEPORTER, AI_REDIRECTION,
//                                            LADDER, ROPE_LADDER, TRIGGER, GATE, DEPOSIT_BOX, BOOST_PAD,
//                                            WEAPON_PICKUP, MYSTERY_BOX, PERK_MACHINE, KRUM_MACHINE, SERVER_NODE
//   two:                                     FLAG (the flag, then its trigger zone, :184685-184723)
//   none:                                    the lights, SPHERE, SPRAY, PRIZE_WHEEL, LIGHT_CONE, LIGHT_BAR,
//                                            SOUND_EMITTER, OBJECTIVE, DOMINATION_ZONE, BOMB_SITE, SHOWCASE,
//                                            PARTICLES, SPRITE, LIQUID, SIGN, LEADERBOARD, CAMERA, AI_SPAWNER,
//                                            BOT_SPAWNER, MAP_ANCHOR, ENVIRONMENT_ZONE, PATH_NODE, BOT_PATH_NODE /
//                                            TERMINAL, ENTITY, PARTNER_ZONE (:189494); and EVENT in a custom game (:189737)
#include <map_game_uids.h>
#include <prefabs.h>
#include <shared.h>
#include <map_catalog.h> // map_sanitize_json
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int mgu_truthy(const cJSON *v) { // JS truthiness of a parsed map value
    if (!v || cJSON_IsNull(v) || cJSON_IsFalse(v)) return 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0.0 && !isnan(v->valuedouble);
    if (cJSON_IsString(v)) return v->valuestring && v->valuestring[0];
    return 1;
}

static int mgu_named(const char *name, const char *const *list) {
    for (; *list; list++)
        if (!strcmp(name, *list)) return 1;
    return 0;
}

int map_game_object_count(const cJSON *raw_obj, const int custom) {
    const int prefab = map_raw_object_prefab(raw_obj);
    const char *name = prefab_name(prefab);
    if (!name) return 0;
    const int no_col = mgu_truthy(cJSON_GetObjectItem(raw_obj, "l")) || mgu_truthy(cJSON_GetObjectItem(raw_obj, "col"));
    const int dyn = mgu_truthy(cJSON_GetObjectItem(raw_obj, "dyn"));
    static const char *const always[] = {"PREMIUM_ZONE", "VERIFIED_ZONE", "TEAM_ZONE", "KR_ZONE", "NOPVP_ZONE", "PVP_ZONE", "KPD_ZONE",
                                         "SCORE_ZONE", "DEATH_ZONE", "CHECK_POINT", "TELEPORTER", "AI_REDIRECTION", "LADDER",
                                         "ROPE_LADDER", "TRIGGER", "GATE", "DEPOSIT_BOX", "BOOST_PAD", "WEAPON_PICKUP", "MYSTERY_BOX",
                                         "PERK_MACHINE", "KRUM_MACHINE", "SERVER_NODE", NULL};
    static const char *const none[] = {"POINT_LIGHT", "DIRECTIONAL_LIGHT", "SPOT_LIGHT", "RECTAREA_LIGHT", "SPHERE", "SPRAY", "PRIZE_WHEEL",
                                       "LIGHT_CONE", "LIGHT_BAR", "SOUND_EMITTER", "OBJECTIVE", "DOMINATION_ZONE", "BOMB_SITE",
                                       "SHOWCASE", "PARTICLES", "SPRITE", "LIQUID", "SIGN", "LEADERBOARD", "CAMERA", "AI_SPAWNER",
                                       "BOT_SPAWNER", "MAP_ANCHOR", "ENVIRONMENT_ZONE", "PATH_NODE", "BOT_PATH_NODE", "TERMINAL",
                                       "ENTITY", "PARTNER_ZONE", NULL};
    if (!strcmp(name, "FLAG")) return 2;
    if (mgu_named(name, always)) return 1;
    if (mgu_named(name, none)) return 0;
    if (!strcmp(name, "CUBE") || !strcmp(name, "CUSTOM_ASSET")) return !no_col || dyn;
    if (!strcmp(name, "EVENT") && custom) return 0;
    return !no_col; // CYLINDER, PLANE, BILLBOARD, RUNE, RAMP, the models (addMesh)
}

int map_game_uids_build(MapGameUids *out, const cJSON *map_json, const int custom) {
    memset(out, 0, sizeof(*out));
    if (!cJSON_IsObject(map_json)) return 0;
    // terrainSeed / Width / Height: generate's terrain objects come first (addTerrObj); not modelled
    if (cJSON_GetObjectItem(map_json, "terrainSeed") && cJSON_GetObjectItem(map_json, "terrainWidth") &&
        cJSON_GetObjectItem(map_json, "terrainHeight"))
        return 0;
    cJSON *copy = cJSON_Duplicate(map_json, 1);
    if (!copy) return 0;
    map_sanitize_json(copy); // the objects map_init keeps, in its order
    const cJSON *objects = cJSON_GetObjectItem(copy, "objects");
    const int n = cJSON_GetArraySize(objects);
    out->uid_of_index = n > 0 ? malloc(sizeof(int) * (size_t) n) : NULL;
    out->index_of_uid = n > 0 ? malloc(sizeof(int) * (size_t) n * 2) : NULL; // at most two uids an object
    if (n <= 0 || !out->uid_of_index || !out->index_of_uid) {
        cJSON_Delete(copy);
        map_game_uids_free(out);
        return 0;
    }
    const cJSON *raw;
    cJSON_ArrayForEach(raw, objects) {
        const int kept = map_raw_object_kept(raw);
        const int count = map_game_object_count(raw, custom);
        const int index = kept ? (int) out->index_count++ : -1;
        if (kept) out->uid_of_index[index] = count ? out->uid_count : -1;
        for (int k = 0; k < count; k++) out->index_of_uid[out->uid_count++] = k == 0 ? index : -1; // a flag's zone: none
    }
    cJSON_Delete(copy);
    return 1;
}
