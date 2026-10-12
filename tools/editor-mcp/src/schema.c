#define _GNU_SOURCE
#include "schema.h"
#include "util.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static cJSON *g_schema;
static SchemaPrefab *g_prefabs;
static int g_prefab_count;
static SchemaProp *g_props;
static int g_prop_count;
static char g_source[512];

static PropType type_of(const char *s) {
    static const struct { const char *n; PropType t; } T[] = {
        {"vec3", PT_VEC3}, {"num", PT_NUM}, {"int", PT_INT}, {"bool_present", PT_BOOL_PRESENT},
        {"bool_absent", PT_BOOL_ABSENT}, {"str", PT_STR}, {"color", PT_COLOR}, {"texture", PT_TEXTURE},
        {"faces", PT_FACES}, {"any", PT_ANY},
    };
    for (size_t i = 0; s && i < sizeof(T) / sizeof(T[0]); i++) if (!strcmp(T[i].n, s)) return T[i].t;
    return PT_ANY;
}

const char *schema_prop_type_name(const PropType t) {
    static const char *names[] = {"vec3", "num", "int", "bool_present", "bool_absent", "str", "color", "texture", "faces", "any"};
    return t <= PT_ANY ? names[t] : "any";
}

static void schema_free(void) {
    free(g_prefabs);
    free(g_props);
    cJSON_Delete(g_schema);
    g_prefabs = NULL;
    g_props = NULL;
    g_schema = NULL;
    g_prefab_count = g_prop_count = 0;
}

static int schema_index(char *err, const size_t errsz) {
    const cJSON *prefabs = js_get(g_schema, "prefabs");
    const cJSON *props = js_get(g_schema, "properties");
    if (!cJSON_IsArray(prefabs) || !cJSON_IsArray(props)) {
        snprintf(err, errsz, "schema has no prefabs / properties arrays");
        return -1;
    }
    g_prefab_count = cJSON_GetArraySize(prefabs);
    g_prefabs = calloc((size_t) g_prefab_count + 1, sizeof(SchemaPrefab));
    g_prop_count = cJSON_GetArraySize(props);
    g_props = calloc((size_t) g_prop_count + 1, sizeof(SchemaProp));
    if (!g_prefabs || !g_props) {
        snprintf(err, errsz, "out of memory");
        return -1;
    }
    int i = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, prefabs) {
        SchemaPrefab *p = &g_prefabs[i];
        p->id = i;
        p->name = js_str(it, "name", "?");
        p->label = js_str(it, "label", p->name);
        p->category = js_str(it, "category", "props");
        p->since = js_str(it, "since", "v3");
        double s[3] = {10, 10, 10};
        js_vec3(js_get(it, "default_size"), s);
        memcpy(p->default_size, s, sizeof(s));
        p->scale = js_num(it, "scale", 1);
        p->native = js_bool(it, "native", 1);
        p->flags = js_get(it, "flags");
        p->aliases = js_get(it, "aliases");
        p->json = it;
        i++;
    }
    i = 0;
    cJSON_ArrayForEach(it, props) {
        SchemaProp *p = &g_props[i++];
        p->prop = js_str(it, "prop", "?");
        p->key = js_str(it, "key", "?");
        p->palette_key = js_str(it, "palette_key", NULL);
        p->since = js_str(it, "since", "v3");
        p->desc = js_str(it, "desc", "");
        p->type = type_of(js_str(it, "type", "any"));
        p->def = js_get(it, "default");
        p->alt_keys = js_get(it, "alt_keys");
        p->has_min = cJSON_IsNumber(js_get(it, "min"));
        p->has_max = cJSON_IsNumber(js_get(it, "max"));
        p->min = js_num(it, "min", 0);
        p->max = js_num(it, "max", 0);
        p->decimals = js_int(it, "decimals", -1);
        p->max_length = js_int(it, "max_length", -1);
        const cJSON *w = js_get(it, "when");
        p->when_flags = js_get(w, "flags");
        p->when_prefabs = js_get(w, "prefabs");
        p->when_not_flags = js_get(w, "not_flags");
        p->json = it;
    }
    return 0;
}

int schema_load(const char *path, const char *root, char *err, const size_t errsz) {
    char candidate[1024] = "";
    if (path && path[0]) snprintf(candidate, sizeof(candidate), "%s", path);
    else if (root && root[0]) {
        snprintf(candidate, sizeof(candidate), "%s/tools/editor-mcp/data/schema.json", root);
        if (!file_exists(candidate)) candidate[0] = 0;
    }
    schema_free();
    if (candidate[0]) {
        size_t n;
        char *text = file_read(candidate, &n);
        if (!text) {
            snprintf(err, errsz, "cannot read schema %s", candidate);
            return -1;
        }
        g_schema = cJSON_ParseWithLength(text, n);
        free(text);
        if (!g_schema) {
            snprintf(err, errsz, "schema %s is not valid JSON", candidate);
            return -1;
        }
        snprintf(g_source, sizeof(g_source), "%s", candidate);
    } else {
        g_schema = cJSON_ParseWithLength((const char *) kn_schema_json, kn_schema_json_len);
        if (!g_schema) {
            snprintf(err, errsz, "embedded schema is not valid JSON");
            return -1;
        }
        snprintf(g_source, sizeof(g_source), "embedded");
    }
    return schema_index(err, errsz);
}

const char *schema_source(void) { return g_source; }
const cJSON *schema_json(void) { return g_schema; }
int schema_prefab_count(void) { return g_prefab_count; }
const SchemaPrefab *schema_prefab(const int id) { return id >= 0 && id < g_prefab_count ? &g_prefabs[id] : NULL; }

// compare letters and digits only, case-insensitively ("Spawn Point" == "SPAWN_POINT")
static int loose_eq(const char *a, const char *b) {
    for (;;) {
        while (*a && !isalnum((unsigned char) *a)) a++;
        while (*b && !isalnum((unsigned char) *b)) b++;
        if (!*a || !*b) return !*a && !*b;
        if (tolower((unsigned char) *a) != tolower((unsigned char) *b)) return 0;
        a++;
        b++;
    }
}

int schema_prefab_find(const char *name) {
    if (!name || !name[0]) return -1;
    char *end;
    const long v = strtol(name, &end, 10);
    if (!*end) return v >= 0 && v < g_prefab_count ? (int) v : -1;
    for (int i = 0; i < g_prefab_count; i++) if (!strcmp(g_prefabs[i].name, name)) return i;
    for (int i = 0; i < g_prefab_count; i++) {
        if (loose_eq(g_prefabs[i].name, name) || loose_eq(g_prefabs[i].label, name)) return i;
        const cJSON *a;
        cJSON_ArrayForEach(a, g_prefabs[i].aliases) if (cJSON_IsString(a) && loose_eq(a->valuestring, name)) return i;
    }
    return -1;
}

int schema_prefab_from_json(const cJSON *v) {
    if (cJSON_IsNumber(v)) {
        const int id = (int) v->valuedouble;
        return (double) id == v->valuedouble && id >= 0 && id < g_prefab_count ? id : -1;
    }
    if (cJSON_IsString(v)) return schema_prefab_find(v->valuestring);
    return -1;
}

static int list_has(const cJSON *list, const char *s) {
    const cJSON *it;
    cJSON_ArrayForEach(it, list) if (cJSON_IsString(it) && !strcmp(it->valuestring, s)) return 1;
    return 0;
}

int schema_prefab_has_flag(const int id, const char *flag) {
    const SchemaPrefab *p = schema_prefab(id);
    return p && list_has(p->flags, flag);
}

int schema_prop_count(void) { return g_prop_count; }
const SchemaProp *schema_prop_at(const int index) { return index >= 0 && index < g_prop_count ? &g_props[index] : NULL; }

const SchemaProp *schema_prop_by_key(const char *key) {
    if (!key) return NULL;
    for (int i = 0; i < g_prop_count; i++) if (!strcmp(g_props[i].key, key)) return &g_props[i];
    for (int i = 0; i < g_prop_count; i++) if (list_has(g_props[i].alt_keys, key)) return &g_props[i];
    return NULL;
}

const SchemaProp *schema_prop_lookup(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_prop_count; i++) if (!strcmp(g_props[i].prop, name)) return &g_props[i];
    return schema_prop_by_key(name);
}

int schema_prop_allowed(const SchemaProp *p, const int prefab) {
    const SchemaPrefab *pf = schema_prefab(prefab);
    if (!p || !pf) return 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, p->when_not_flags) if (cJSON_IsString(it) && list_has(pf->flags, it->valuestring)) return 0;
    const int has_flags = cJSON_GetArraySize(p->when_flags) > 0, has_prefabs = cJSON_GetArraySize(p->when_prefabs) > 0;
    if (!has_flags && !has_prefabs) return 1;
    cJSON_ArrayForEach(it, p->when_prefabs) {
        if (!cJSON_IsString(it)) continue;
        if (!strcmp(it->valuestring, pf->name) || list_has(pf->aliases, it->valuestring)) return 1;
    }
    cJSON_ArrayForEach(it, p->when_flags) if (cJSON_IsString(it) && list_has(pf->flags, it->valuestring)) return 1;
    return 0;
}

int schema_core_key(const char *key) {
    static const char *core[] = {"p", "s", "si", "r", "i", "id", "objUUID", NULL};
    for (int i = 0; core[i]; i++) if (!strcmp(core[i], key)) return 1;
    return 0;
}

int schema_texture_count(void) { return cJSON_GetArraySize(js_get(g_schema, "textures")); }

const char *schema_texture_name(const int index) {
    const cJSON *t = cJSON_GetArrayItem(js_get(g_schema, "textures"), index);
    return index >= 0 ? js_str(t, "name", NULL) : NULL;
}

int schema_texture_find(const char *name) {
    if (!name || !name[0]) return -1;
    char *end;
    const long v = strtol(name, &end, 10);
    if (!*end) return v >= 0 && v < schema_texture_count() ? (int) v : -1;
    const int n = schema_texture_count();
    for (int i = 0; i < n; i++) {
        const cJSON *t = cJSON_GetArrayItem(js_get(g_schema, "textures"), i);
        if (!strcasecmp(js_str(t, "name", ""), name) || !strcasecmp(js_str(t, "src", ""), name)) return i;
    }
    if (!strcasecmp(name, "none") || !strcasecmp(name, "default")) return schema_texture_find("DEFAULT");
    if (!strcasecmp(name, "stone")) return schema_texture_find("WALL");
    if (!strcasecmp(name, "wood")) return schema_texture_find("FLOOR");
    return -1;
}

int schema_texture_default(const int prefab) {
    const SchemaPrefab *p = schema_prefab(prefab);
    return p && (!strcmp(p->name, "LADDER") || !strcmp(p->name, "ROPE_LADDER")) ? 2 : 0;
}

const cJSON *schema_limits(void) { return js_get(g_schema, "limits"); }
double schema_limit(const char *name, const double def) { return js_num(schema_limits(), name, def); }
const cJSON *schema_triggers(void) { return js_get(g_schema, "triggers"); }
int schema_event_count(void) { return cJSON_GetArraySize(js_get(schema_triggers(), "events")); }
int schema_action_count(void) { return cJSON_GetArraySize(js_get(schema_triggers(), "actions")); }

const char *schema_event_name(const int id) {
    return id >= 0 ? js_str(cJSON_GetArrayItem(js_get(schema_triggers(), "events"), id), "name", NULL) : NULL;
}

const char *schema_action_name(const int id) {
    return id >= 0 ? js_str(cJSON_GetArrayItem(js_get(schema_triggers(), "actions"), id), "name", NULL) : NULL;
}

static int find_named(const cJSON *list, const char *name) {
    if (!name || !name[0]) return -1;
    char *end;
    const long v = strtol(name, &end, 10);
    if (!*end) return v >= 0 && v < cJSON_GetArraySize(list) ? (int) v : -1;
    int i = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, list) {
        if (loose_eq(js_str(it, "name", ""), name) || loose_eq(js_str(it, "v3_name", "\x01"), name)) return i;
        i++;
    }
    return -1;
}

int schema_event_find(const char *name) { return find_named(js_get(schema_triggers(), "events"), name); }
int schema_action_find(const char *name) { return find_named(js_get(schema_triggers(), "actions"), name); }

const cJSON *schema_map_key(const char *key) {
    const cJSON *it;
    cJSON_ArrayForEach(it, js_get(g_schema, "map_keys")) if (!strcmp(js_str(it, "key", ""), key)) return it;
    return NULL;
}
