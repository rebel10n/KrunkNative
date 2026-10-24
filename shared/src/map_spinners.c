// Krunker v10.0.3 MapManager.spinners (shared/include/map_spinners.h). Line numbers: deob-10.0.3.js.
#include <map_spinners.h>
#include <store_spins.h>
#include <cJSON.h>
#include <math.h>
#include <stdlib.h>

// parseInt(raw[key]) || 0
static int raw_int(const cJSON *raw, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(raw, key);
    if (cJSON_IsNumber(v)) return isfinite(v->valuedouble) ? (int) v->valuedouble : 0;
    if (cJSON_IsString(v) && v->valuestring) return (int) strtol(v->valuestring, NULL, 10);
    return 0;
}

int map_spinner_wheel(const Object *object) {
    // map.generate walks the map JSON: run-time objects (builds, Object Manager spawns) are none of its spinners
    if (!object || object->prefab != PREFAB_PRIZE_WHEEL || object->spawned || object->dynamic) return -1;
    int sty = raw_int(object->raw, "sty"); // limitMM(parseInt(sty) || 0, 0, 2) (:189054)
    sty = sty < 0 ? 0 : sty > 2 ? 2 : sty;
    const StoreSpins *spins = store_spins_builtin();
    const StoreWheel *w = sty < spins->wheel_count ? &spins->wheels[sty] : NULL;
    if (!w || w->ticket_only || w->hide || w->locked) return -1; // `continue` (:189055-189057)
    return sty;
}

const StoreWheel *map_spinner_store_wheel(const Object *object) {
    const int wheel = map_spinner_wheel(object);
    return wheel >= 0 ? &store_spins_builtin()->wheels[wheel] : NULL;
}

int map_spinner_count(const Map *map) {
    int count = 0;
    for (size_t i = 0; map && i < map->object_count; i++) count += map_spinner_wheel(map->objects[i]) >= 0;
    return count;
}

int map_spinner_object(const Map *map, const int uid) {
    if (uid < 0) return -1;
    for (size_t i = 0, n = 0; map && i < map->object_count; i++) {
        if (map_spinner_wheel(map->objects[i]) < 0) continue;
        if ((int) n++ == uid) return (int) i;
    }
    return -1;
}

int map_spinner_uid(const Map *map, const int index) {
    if (!map || index < 0 || (size_t) index >= map->object_count || map_spinner_wheel(map->objects[index]) < 0) return -1;
    int uid = 0;
    for (int i = 0; i < index; i++) uid += map_spinner_wheel(map->objects[i]) >= 0;
    return uid; // uid: this.spinners.length (:186551)
}

float map_spinner_dir(const Object *object) {
    const cJSON *d = object ? cJSON_GetObjectItemCaseSensitive(object->raw, "d") : NULL; // IÍiìiiî.d || 0
    if (cJSON_IsNumber(d)) return isfinite(d->valuedouble) ? (float) d->valuedouble : 0.0f;
    if (cJSON_IsString(d) && d->valuestring && d->valuestring[0]) return (float) atof(d->valuestring); // "d" * PI: a number
    return 0.0f;
}

void map_spinner_box(const Object *object, vec3 *centre, vec3 *half) {
    // new GameObject({x, z, y, w: sy / 4, l: sy / 4, h: sy / 4}) with y the object's p[1], before y += sy / 2 (:186543-186560)
    const float h = object->scale.y / 4.0f;
    if (centre) *centre = object->position;
    if (half) *half = (vec3) {h, h, h};
}
