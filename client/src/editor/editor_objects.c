// Map editor objects: Krunker v3.1.1 libs/ObjectInstance.js (properties, serialize) and the
// object half of editor.js (addObject/removeObject/duplicate/copy/paste/groups/history).
//
// Every object is kept in its serialized form (EditorObject.data, the JSON ObjectInstance
// .serialize() writes, colours inline). Properties are read and written through the
// table below, which encodes the constructor defaults and the serialize() rules (e.g.
// `collidable` is `l: 1` when false, `visible` is `v: 1` when false), so an imported map
// round-trips unchanged, including keys the editor GUI does not know.
#include "editor_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Editor g_ed;

// ---------------------------------------------------------------------------
// JSON helpers

double ed_round(const double v, const int decimals) {
    const double p = pow(10.0, decimals);
    return round(v * p) / p; // Number.prototype.round
}

const cJSON *ed_json_path(const cJSON *root, const char *key) {
    return root ? cJSON_GetObjectItemCaseSensitive(root, key) : NULL;
}

void ed_json_set_num(cJSON *obj, const char *key, const double v) {
    if (!obj) return;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsNumber(item)) {
        cJSON_SetNumberValue(item, v);
        return;
    }
    if (item) cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    cJSON_AddNumberToObject(obj, key, v);
}

void ed_json_set_str(cJSON *obj, const char *key, const char *v) {
    if (!obj) return;
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    cJSON_AddStringToObject(obj, key, v ? v : "");
}

void ed_json_set_bool(cJSON *obj, const char *key, const int v) {
    if (!obj) return;
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    cJSON_AddBoolToObject(obj, key, v ? 1 : 0);
}

double ed_json_num(const cJSON *obj, const char *key, const double fallback) {
    const cJSON *v = ed_json_path(obj, key);
    if (cJSON_IsNumber(v)) return cJSON_GetNumberValue(v);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? 1.0 : 0.0;
    if (cJSON_IsString(v) && v->valuestring[0]) {
        char *end = NULL;
        const double d = strtod(v->valuestring, &end);
        if (end && end != v->valuestring) return d;
    }
    return fallback;
}

const char *ed_json_str(const cJSON *obj, const char *key, const char *fallback) {
    const cJSON *v = ed_json_path(obj, key);
    return cJSON_IsString(v) ? v->valuestring : fallback;
}

int ed_json_truthy(const cJSON *v) {
    if (!v || cJSON_IsNull(v) || cJSON_IsFalse(v)) return 0;
    if (cJSON_IsNumber(v)) return cJSON_GetNumberValue(v) != 0.0;
    if (cJSON_IsString(v)) return v->valuestring[0] != 0;
    return 1;
}

static double value_num(const cJSON *v) {
    if (cJSON_IsNumber(v)) return cJSON_GetNumberValue(v);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsString(v)) return strtod(v->valuestring, NULL);
    return 0.0;
}

static int present(const cJSON *obj, const char *key) {
    const cJSON *v = ed_json_path(obj, key);
    return v != NULL && !cJSON_IsNull(v); // `e.x !== undefined` / `!= null` (JSON has no undefined)
}

// ---------------------------------------------------------------------------
// ObjectInstance properties

typedef enum {
    PT_NUM,           // number; absent == def; written rounded to `dec` decimals, omitted when == def
    PT_INT,           // parseInt
    PT_BOOL_PRESENT,  // true <=> key present (written as 1)
    PT_BOOL_ABSENT,   // true <=> key absent (false writes 1)
    PT_STR,           // string, omitted when == sdef
    PT_COLOR,         // "#RRGGBB" string, omitted when == sdef (case-insensitive)
    PT_ALWAYS_INT,    // teamable "tm": always written
} PropType;

typedef struct {
    const char *prop;
    const char *key;
    PropType type;
    double def;
    int dec;
    const char *sdef;
    int rebuild;      // regen(): the mesh depends on it
} PropDef;

static const PropDef PROPS[] = {
    {"penetrable", "pe", PT_BOOL_PRESENT, 0, 0, NULL, 0},
    {"visible", "v", PT_BOOL_ABSENT, 0, 0, NULL, 1},
    {"boost", "b", PT_NUM, 0, 2, NULL, 0},
    {"bounce", "bm", PT_NUM, 1, 2, NULL, 0},
    {"crouch", "cr", PT_BOOL_ABSENT, 0, 0, NULL, 0},
    {"edgeNoise", "en", PT_NUM, 0, 1, NULL, 1},
    {"part", "pr", PT_INT, 0, 0, NULL, 0},
    {"partC", "pc", PT_INT, 0, 0, NULL, 0},
    {"partS", "ps", PT_NUM, 0, 1, NULL, 0},
    {"partSP", "psp", PT_NUM, 0, 1, NULL, 0},
    {"partG", "pg", PT_NUM, 0, 1, NULL, 0},
    {"partD", "pd", PT_NUM, 0, 1, NULL, 0},
    {"partSC", "psc", PT_NUM, 0, 1, NULL, 0},
    {"partB", "pb", PT_INT, 0, 0, NULL, 0},
    {"team", "tm", PT_ALWAYS_INT, 0, 0, NULL, 1},
    {"color", "c", PT_COLOR, 0, 0, "#FFFFFF", 1},
    {"emissive", "e", PT_COLOR, 0, 0, "#000000", 1},
    {"opacity", "o", PT_NUM, 1, 1, NULL, 1},
    {"direction", "d", PT_INT, 0, 0, NULL, 1},
    {"forceDir", "fd", PT_BOOL_PRESENT, 0, 0, NULL, 0},
    {"texStretch", "tsr", PT_BOOL_PRESENT, 0, 0, NULL, 1},
    {"movingSpeed", "ts", PT_NUM, 0, 1, NULL, 1},
    {"movingAxis", "td", PT_INT, 0, 0, NULL, 1},
    {"single", "su", PT_BOOL_PRESENT, 0, 0, NULL, 0},
    {"weaponReplace", "wr", PT_BOOL_ABSENT, 0, 0, NULL, 0},
    {"randWep", "rw", PT_BOOL_PRESENT, 0, 0, NULL, 0},
    {"poster", "bb", PT_INT, 0, 0, NULL, 1},
    {"showAd", "sad", PT_BOOL_PRESENT, 0, 0, NULL, 1},
    {"border", "bo", PT_BOOL_PRESENT, 0, 0, NULL, 1},
    {"spin", "spn", PT_INT, 0, 0, NULL, 0},
    {"channel", "ch", PT_INT, 0, 0, NULL, 0},
    {"node", "n", PT_INT, 0, 0, NULL, 0},
    {"momentum", "m", PT_BOOL_PRESENT, 0, 0, NULL, 0},
    {"ambient", "ab", PT_BOOL_ABSENT, 0, 0, NULL, 1},
    {"fog", "nf", PT_BOOL_ABSENT, 0, 0, NULL, 1},
    {"hidden", "nm", PT_BOOL_PRESENT, 0, 0, NULL, 0},
    {"method", "md", PT_INT, 0, 0, NULL, 0},
    {"closeable", "cl", PT_BOOL_PRESENT, 0, 0, NULL, 0},
    {"startClosed", "stc", PT_BOOL_PRESENT, 0, 0, NULL, 0},
    {"event", "te", PT_INT, 0, 0, NULL, 0},
    {"action", "ta", PT_INT, 0, 0, NULL, 0},
    {"constant", "tc", PT_INT, 0, 0, NULL, 0},
    {"constantEvent", "tce", PT_INT, 0, 0, NULL, 0},
    {"constantTxt", "tct", PT_STR, 0, 0, "", 0},
    {"text", "st", PT_STR, 0, 0, "", 1},
    {"fcolor", "sc", PT_COLOR, 0, 0, "#000000", 1},
    {"bcolor", "sb", PT_COLOR, 0, 0, "#FFFFFF", 1},
    {"backVisible", "bv", PT_BOOL_ABSENT, 0, 0, NULL, 1},
    {"fsize", "sf", PT_INT, 10, 0, NULL, 1},
    {"tAlign", "sa", PT_INT, 0, 0, NULL, 1},
    {"respawnT", "rt", PT_NUM, 0, 1, NULL, 0},
    {"respawnR", "rr", PT_BOOL_PRESENT, 0, 0, NULL, 0},
    {"classIndx", "ci", PT_INT, 0, 0, NULL, 0},
    {"skill", "sk", PT_INT, 0, 0, NULL, 0},
    {"hpMlt", "hm", PT_INT, 1, 0, NULL, 0},
    {"interfaceId", "in", PT_INT, 0, 0, NULL, 0},
    {"interfaceIdT", "tin", PT_INT, 0, 0, NULL, 0},
    {"withdraw", "wd", PT_INT, 1000, 0, NULL, 0},
    {"deposit", "dp", PT_INT, 1000, 0, NULL, 0},
    {"objectiveId", "obi", PT_INT, 0, 0, NULL, 0},
    {"wHeight", "wh", PT_INT, 0.5, 0, NULL, 1},
    {"wSpeed", "ws", PT_INT, 10, 0, NULL, 1},
    {"tilesX", "tx", PT_INT, 10, 0, NULL, 1},
    {"tilesZ", "tz", PT_INT, 10, 0, NULL, 1},
    {"fullcone", "fc", PT_BOOL_PRESENT, 0, 0, NULL, 1},
    {"snd", "es", PT_INT, 0, 0, NULL, 0},
    {"vol", "ev", PT_NUM, 1, 2, NULL, 0},
    {"dist", "ed", PT_INT, 0, 0, NULL, 0},
    {"roll", "ero", PT_NUM, 0, 2, NULL, 0},
    {"rate", "er", PT_NUM, 1, 1, NULL, 0},
    {"emitfrom", "ef", PT_INT, 0, 0, NULL, 0},
    {"assetID", "aid", PT_STR, 0, 0, "", 1},
    {"cmpSpwn", "cp", PT_BOOL_PRESENT, 0, 0, NULL, 1},
    {"texSmooth", "txs", PT_BOOL_ABSENT, 0, 0, NULL, 1},
    {"dynamic", "dyn", PT_BOOL_PRESENT, 0, 0, NULL, 0},
    {"gravity", "gy", PT_NUM, 0, 1, NULL, 0},
    {"itemID", "skin", PT_INT, 0, 0, NULL, 1},
    {"intensity", "li", PT_NUM, 1, 1, NULL, 1},
    {"distance", "lds", PT_NUM, 0, 1, NULL, 1},
    {"decay", "ld", PT_NUM, 1, 1, NULL, 1},
    {"openEnded", "opn", PT_BOOL_PRESENT, 0, 0, NULL, 1},
    {"assetTexture", "at", PT_INT, 0, 0, NULL, 1},
    {"texOffX", "tox", PT_NUM, 0, 1, NULL, 1},
    {"texOffY", "toy", PT_NUM, 0, 1, NULL, 1},
};

static const PropDef *prop_def(const char *prop) {
    for (size_t i = 0; i < sizeof(PROPS) / sizeof(PROPS[0]); i++) {
        if (!strcmp(PROPS[i].prop, prop)) return &PROPS[i];
    }
    return NULL;
}

static int ladder(const EditorObject *o) { return !strcmp(editor_prefabs[o->prefab].name, "LADDER"); }

static const char *texture_name(const EditorObject *o) {
    const cJSON *t = ed_json_path(o->data, "t");
    if (cJSON_IsString(t)) {
        if (t->valuestring[0]) return t->valuestring;
        return ladder(o) ? "FLOOR" : "DEFAULT";
    }
    int index = cJSON_IsNumber(t) && cJSON_GetNumberValue(t) != 0 ? (int) cJSON_GetNumberValue(t) : ladder(o) ? 2 : 0;
    if (index < 0 || index >= EDITOR_TEXTURE_COUNT) index = 0;
    return editor_textures[index];
}

static int texture_index(const char *name) {
    for (int i = 0; i < EDITOR_TEXTURE_COUNT; i++) {
        if (!strcasecmp(editor_textures[i], name)) return i;
    }
    return 0; // getTextureIdByName
}

static double prop_score_default(const EditorObject *o) {
    return editor_prefab_has(o->prefab, EP_INTERACT) ? 0.0 : 1.0;
}

vec3 ed_obj_vec(const EditorObject *o, const char *key, const vec3 fallback) {
    const cJSON *a = ed_json_path(o->data, key);
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) < 3) return fallback;
    vec3 v = {
        (float) value_num(cJSON_GetArrayItem(a, 0)),
        (float) value_num(cJSON_GetArrayItem(a, 1)),
        (float) value_num(cJSON_GetArrayItem(a, 2)),
    };
    if (isnan(v.x)) v.x = 0.0f;
    if (isnan(v.y)) v.y = 0.0f;
    if (isnan(v.z)) v.z = 0.0f;
    return v;
}

static cJSON *vec_json(const vec3 v, const int decimals) {
    cJSON *a = cJSON_CreateArray();
    cJSON_AddItemToArray(a, cJSON_CreateNumber(ed_round(v.x, decimals)));
    cJSON_AddItemToArray(a, cJSON_CreateNumber(ed_round(v.y, decimals)));
    cJSON_AddItemToArray(a, cJSON_CreateNumber(ed_round(v.z, decimals)));
    return a;
}

void ed_obj_set_vec(EditorObject *o, const char *key, const vec3 v) {
    cJSON_DeleteItemFromObjectCaseSensitive(o->data, key);
    if (!strcmp(key, "r") && !ed_round(v.x, 2) && !ed_round(v.y, 2) && !ed_round(v.z, 2)) return; // r only when rotated
    if (!strcmp(key, "s") && !editor_prefab_has(o->prefab, EP_DONTROUND)) {
        // ObjectInstance.update(): scale is max(1, s).roundToNearest(1) unless dontRound
        vec3 r = v;
        r.x = ceilf(fmaxf(1.0f, r.x) - 1e-6f);
        r.y = ceilf(fmaxf(1.0f, r.y) - 1e-6f);
        r.z = ceilf(fmaxf(1.0f, r.z) - 1e-6f);
        cJSON_AddItemToObject(o->data, key, vec_json(r, 2));
        return;
    }
    cJSON_AddItemToObject(o->data, key, vec_json(v, 2));
}

static cJSON *faces_json(const EditorObject *o) {
    // e.f: hex string of 6 bits (UTILS.convertFaces), Right Left Top Bottom Back Front
    cJSON *a = cJSON_CreateArray();
    const cJSON *f = ed_json_path(o->data, "f");
    int bits[6] = {1, 1, 1, 1, 1, 1};
    if (cJSON_IsString(f)) {
        const long v = strtol(f->valuestring, NULL, 16);
        for (int i = 0; i < 6; i++) bits[i] = (int) ((v >> (5 - i)) & 1);
    } else if (cJSON_IsArray(f)) {
        for (int i = 0; i < 6 && i < cJSON_GetArraySize(f); i++) bits[i] = ed_json_truthy(cJSON_GetArrayItem(f, i));
    }
    for (int i = 0; i < 6; i++) cJSON_AddItemToArray(a, cJSON_CreateBool(bits[i]));
    return a;
}

cJSON *ed_prop_get(const EditorObject *o, const char *prop) {
    const cJSON *d = o->data;
    if (!strcmp(prop, "pos")) return vec_json(ed_obj_vec(o, "p", (vec3) {0}), 2);
    if (!strcmp(prop, "size")) {
        const EditorPrefab *p = &editor_prefabs[o->prefab];
        return vec_json(ed_obj_vec(o, "s", (vec3) {p->default_size[0], p->default_size[1], p->default_size[2]}), 2);
    }
    if (!strcmp(prop, "rot")) {
        // GUI shows degrees unless settings.degToRad (editor.radToDeg)
        vec3 r = ed_obj_vec(o, "r", (vec3) {0});
        if (!editor_setting_bool("degToRad")) {
            r.x = r.x * 180.0f / (float) M_PI;
            r.y = r.y * 180.0f / (float) M_PI;
            r.z = r.z * 180.0f / (float) M_PI;
        }
        return vec_json(r, 2);
    }
    if (!strcmp(prop, "texture")) return cJSON_CreateString(texture_name(o));
    if (!strcmp(prop, "collidable")) {
        if (editor_prefab_has(o->prefab, EP_FORCECOLLISION)) return cJSON_CreateBool(1);
        if (editor_prefab_has(o->prefab, EP_NOCOLLISION)) return cJSON_CreateBool(0);
        return cJSON_CreateBool(!present(d, "col") && !present(d, "l"));
    }
    if (!strcmp(prop, "health")) return cJSON_CreateNumber(ed_json_num(d, "h", ed_json_num(d, "hp", 0.0)));
    if (!strcmp(prop, "weaponId")) return cJSON_CreateNumber(ed_json_num(d, "wid", ed_json_num(d, "w", 0.0)));
    if (!strcmp(prop, "score")) return cJSON_CreateNumber(ed_json_num(d, "sp", prop_score_default(o)));
    if (!strcmp(prop, "mSize")) return cJSON_CreateNumber(ed_json_num(d, "ms", editor_prefabs[o->prefab].scale));
    if (!strcmp(prop, "faces")) return faces_json(o);
    if (!strcmp(prop, "bounce")) {
        const double b = ed_json_num(d, "bm", 0.0);
        return cJSON_CreateNumber(b ? b : 1.0); // e.bm || 1
    }

    const PropDef *def = prop_def(prop);
    if (!def) return cJSON_CreateNull();
    switch (def->type) {
        case PT_BOOL_PRESENT: return cJSON_CreateBool(present(d, def->key));
        case PT_BOOL_ABSENT: return cJSON_CreateBool(!present(d, def->key));
        case PT_STR: return cJSON_CreateString(ed_json_str(d, def->key, def->sdef));
        case PT_COLOR: {
            const cJSON *v = ed_json_path(d, def->key);
            if (cJSON_IsNumber(v)) {
                char hex[16];
                snprintf(hex, sizeof(hex), "#%06x", (int) cJSON_GetNumberValue(v) & 0xffffff);
                return cJSON_CreateString(hex);
            }
            return cJSON_CreateString(cJSON_IsString(v) ? v->valuestring : def->sdef);
        }
        default: {
            const double v = ed_json_num(d, def->key, def->def);
            return cJSON_CreateNumber(v ? v : def->def); // `e.x || def`
        }
    }
}

int ed_prop_needs_rebuild(const char *prop) {
    if (!strcmp(prop, "pos") || !strcmp(prop, "rot") || !strcmp(prop, "size") || !strcmp(prop, "texture") ||
        !strcmp(prop, "faces") || !strcmp(prop, "mSize") || !strcmp(prop, "collidable")) return 1;
    const PropDef *def = prop_def(prop);
    return def ? def->rebuild : 0;
}

void ed_prop_set(EditorObject *o, const char *prop, const cJSON *value) {
    cJSON *d = o->data;
    const int prefab = o->prefab;

    if (!strcmp(prop, "pos") || !strcmp(prop, "size") || !strcmp(prop, "rot")) {
        if (!cJSON_IsArray(value) || cJSON_GetArraySize(value) < 3) return;
        vec3 v = {
            (float) value_num(cJSON_GetArrayItem(value, 0)),
            (float) value_num(cJSON_GetArrayItem(value, 1)),
            (float) value_num(cJSON_GetArrayItem(value, 2)),
        };
        if (isnan(v.x)) v.x = 0.0f;
        if (isnan(v.y)) v.y = 0.0f;
        if (isnan(v.z)) v.z = 0.0f;
        if (!strcmp(prop, "rot")) {
            if (!editor_setting_bool("degToRad")) {
                v.x = v.x * (float) M_PI / 180.0f;
                v.y = v.y * (float) M_PI / 180.0f;
                v.z = v.z * (float) M_PI / 180.0f;
            }
            ed_obj_set_vec(o, "r", v);
        } else if (!strcmp(prop, "size")) {
            if (!editor_prefab_has(prefab, EP_SCALABLE)) return;
            ed_obj_set_vec(o, "s", v);
        } else {
            ed_obj_set_vec(o, "p", v);
        }
        return;
    }
    if (!strcmp(prop, "texture")) {
        if (!editor_prefab_has(prefab, EP_TEXTURABLE)) return;
        const char *name = cJSON_IsString(value) ? value->valuestring : "DEFAULT";
        cJSON_DeleteItemFromObjectCaseSensitive(d, "t");
        cJSON_DeleteItemFromObjectCaseSensitive(d, "at"); // texture select resets assetTexture
        const int index = texture_index(name);
        if (index) cJSON_AddNumberToObject(d, "t", index);
        return;
    }
    if (!strcmp(prop, "collidable")) {
        cJSON_DeleteItemFromObjectCaseSensitive(d, "l");
        cJSON_DeleteItemFromObjectCaseSensitive(d, "col");
        int on = ed_json_truthy(value);
        if (editor_prefab_has(prefab, EP_FORCECOLLISION)) on = 1;
        if (editor_prefab_has(prefab, EP_NOCOLLISION)) on = 0;
        if (!on) cJSON_AddNumberToObject(d, "l", 1);
        return;
    }
    if (!strcmp(prop, "health")) {
        cJSON_DeleteItemFromObjectCaseSensitive(d, "h");
        cJSON_DeleteItemFromObjectCaseSensitive(d, "hp");
        const int v = (int) value_num(value);
        if (v) cJSON_AddNumberToObject(d, "h", v);
        return;
    }
    if (!strcmp(prop, "weaponId")) {
        cJSON_DeleteItemFromObjectCaseSensitive(d, "w");
        cJSON_DeleteItemFromObjectCaseSensitive(d, "wid");
        const int v = (int) value_num(value);
        if (v > 0) cJSON_AddNumberToObject(d, "w", v);
        return;
    }
    if (!strcmp(prop, "score")) {
        cJSON_DeleteItemFromObjectCaseSensitive(d, "sp");
        const int v = (int) value_num(value);
        if (v > (editor_prefab_has(prefab, EP_INTERACT) ? 0 : 1)) cJSON_AddNumberToObject(d, "sp", v);
        return;
    }
    if (!strcmp(prop, "mSize")) {
        cJSON_DeleteItemFromObjectCaseSensitive(d, "ms");
        const double v = value_num(value);
        if (v != editor_prefabs[prefab].scale) cJSON_AddNumberToObject(d, "ms", v);
        return;
    }
    if (!strcmp(prop, "bounce")) {
        cJSON_DeleteItemFromObjectCaseSensitive(d, "bm");
        const double v = ed_round(value_num(value), 2);
        if (v != 1.0) cJSON_AddNumberToObject(d, "bm", v);
        return;
    }
    if (!strcmp(prop, "faces")) {
        cJSON_DeleteItemFromObjectCaseSensitive(d, "f");
        if (!cJSON_IsArray(value)) return;
        int bits = 0, all = 1;
        for (int i = 0; i < 6; i++) {
            const int on = ed_json_truthy(cJSON_GetArrayItem(value, i));
            all &= on;
            bits = (bits << 1) | on;
        }
        if (!all && editor_prefab_has(prefab, EP_EDITFACES)) {
            char hex[8];
            snprintf(hex, sizeof(hex), "%x", bits);
            cJSON_AddStringToObject(d, "f", hex);
        }
        return;
    }
    if (!strcmp(prop, "penetrable") && !editor_prefab_has(prefab, EP_EDITPEN)) return;
    if ((!strcmp(prop, "color") || !strcmp(prop, "fcolor") || !strcmp(prop, "bcolor")) && !strcmp(prop, "color") && !editor_prefab_has(prefab, EP_EDITCOLOR)) return;
    if (!strcmp(prop, "emissive") && !editor_prefab_has(prefab, EP_EDITEMISSIVE)) return;
    if (!strcmp(prop, "opacity") && !editor_prefab_has(prefab, EP_EDITOPAC)) return;

    const PropDef *def = prop_def(prop);
    if (!def) return;
    cJSON_DeleteItemFromObjectCaseSensitive(d, def->key);
    switch (def->type) {
        case PT_BOOL_PRESENT:
            if (ed_json_truthy(value)) cJSON_AddNumberToObject(d, def->key, 1);
            break;
        case PT_BOOL_ABSENT:
            if (!ed_json_truthy(value)) cJSON_AddNumberToObject(d, def->key, 1);
            break;
        case PT_STR: {
            const char *s = cJSON_IsString(value) ? value->valuestring : "";
            if (strcmp(s, def->sdef)) cJSON_AddStringToObject(d, def->key, s);
            break;
        }
        case PT_COLOR: {
            const char *s = cJSON_IsString(value) ? value->valuestring : def->sdef;
            if (strcasecmp(s, def->sdef)) cJSON_AddStringToObject(d, def->key, s);
            break;
        }
        case PT_ALWAYS_INT:
            cJSON_AddNumberToObject(d, def->key, (int) value_num(value));
            break;
        case PT_INT: {
            const int v = (int) value_num(value);
            if (v && v != def->def) cJSON_AddNumberToObject(d, def->key, v);
            break;
        }
        case PT_NUM: {
            const double v = ed_round(value_num(value), def->dec);
            if (v != def->def) cJSON_AddNumberToObject(d, def->key, v);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// lifecycle

static int prefab_from_json(const cJSON *data) {
    const cJSON *i = ed_json_path(data, "i");
    if (!i || cJSON_IsNull(i)) i = ed_json_path(data, "id");
    if (cJSON_IsNumber(i)) {
        const int id = (int) cJSON_GetNumberValue(i);
        return id >= 0 && id < EDITOR_PREFAB_COUNT ? id : 0;
    }
    if (cJSON_IsString(i)) {
        char *end = NULL;
        const long id = strtol(i->valuestring, &end, 10);
        if (end && *end == 0 && end != i->valuestring) return id >= 0 && id < EDITOR_PREFAB_COUNT ? (int) id : 0;
        for (int p = 0; p < EDITOR_PREFAB_COUNT; p++) {
            if (!strcasecmp(editor_prefabs[p].name, i->valuestring)) return p;
        }
    }
    return 0; // "Invalid Object": falls back to a cube
}

EditorObject *ed_obj_new(const cJSON *serialized, const int keep_uuid) {
    EditorObject *o = calloc(1, sizeof(EditorObject));
    if (!o) return NULL;
    o->data = serialized ? cJSON_Duplicate(serialized, 1) : cJSON_CreateObject();
    if (!o->data) {
        free(o);
        return NULL;
    }
    o->prefab = prefab_from_json(o->data);
    const cJSON *uuid = ed_json_path(o->data, "objUUID");
    o->uuid = keep_uuid && cJSON_IsNumber(uuid) ? (uint32_t) cJSON_GetNumberValue(uuid) : ++g_ed.proj->next_uuid;
    if (o->uuid > g_ed.proj->next_uuid) g_ed.proj->next_uuid = o->uuid;
    cJSON_DeleteItemFromObjectCaseSensitive(o->data, "objUUID");
    cJSON_DeleteItemFromObjectCaseSensitive(o->data, "meshUUID");

    // normalise the prefab key to "i" (serialize: t.i = id when non-zero)
    cJSON_DeleteItemFromObjectCaseSensitive(o->data, "i");
    cJSON_DeleteItemFromObjectCaseSensitive(o->data, "id");
    if (o->prefab) cJSON_AddNumberToObject(o->data, "i", o->prefab);

    if (!cJSON_IsArray(ed_json_path(o->data, "p"))) ed_obj_set_vec(o, "p", (vec3) {0});
    const EditorPrefab *prefab = &editor_prefabs[o->prefab];
    if (!cJSON_IsArray(ed_json_path(o->data, "s")) || !editor_prefab_has(o->prefab, EP_SCALABLE)) {
        // this.size = e.s || this.defaultSize; non-scalable prefabs keep defaultSize (update())
        if (!cJSON_IsArray(ed_json_path(o->data, "s")) || prefab->has_default_size) {
            cJSON_DeleteItemFromObjectCaseSensitive(o->data, "s");
            cJSON_AddItemToObject(o->data, "s", vec_json((vec3) {prefab->default_size[0], prefab->default_size[1], prefab->default_size[2]}, 2));
        }
    }
    if (editor_prefab_has(o->prefab, EP_TEAMABLE) && !ed_json_path(o->data, "tm")) cJSON_AddNumberToObject(o->data, "tm", 0);
    return o;
}

static void obj_drop_render(EditorObject *o) {
    if (o->mesh && g_ed.proj->scene) scene_remove_mesh(g_ed.proj->scene, o->mesh);
    if (o->helper && g_ed.proj->scene) scene_remove_mesh(g_ed.proj->scene, o->helper);
    if (o->render_map) {
        map_fini(o->render_map);
        free(o->render_map);
    }
    if (o->helper) mesh_fini(o->helper);
    cJSON_Delete(o->render_doc);
    o->render_doc = NULL;
    o->render_map = NULL;
    o->mesh = NULL;
    o->helper = NULL;
}

void ed_obj_free(EditorObject *o) {
    if (!o) return;
    obj_drop_render(o);
    cJSON_Delete(o->data);
    cJSON_Delete(o->history);
    cJSON_Delete(o->scratch);
    free(o);
}

// Spawn points: generateSpawn loads models/spawn_0.obj, colour by team / competitive.
static Mesh *spawn_helper(const EditorObject *o) {
    static Geometry *geometry;
    static unsigned int texture;
    if (!geometry) {
        char *model = concat(assets_path(), "models/spawn_0.obj");
        char *tex = concat(assets_path(), "textures/spawn_0.png");
        if (model) {
            ensure_asset_file(model);
            geometry = load_obj_model(model, 0);
        }
        if (tex) {
            ensure_asset_file(tex);
            texture = load_texture(tex);
        }
        free(model);
        free(tex);
    }
    if (!geometry) return NULL;
    BasicMaterial *material = basic_material_init();
    if (!material) return NULL;
    Mesh *mesh = mesh_init(geometry, (Material *) material);
    if (!mesh) return NULL;
    const int team = (int) ed_json_num(o->data, "tm", 0);
    const int cmp = present(o->data, "cp");
    const int hex = cmp ? 0xffff00 : team == 1 ? 0x00ffff : team == 2 ? 0xff0000 : 0xffffff;
    material->texture = texture;
    material->color = hex_to_vec(hex);
    material->uv_mode = BASIC_UV_MESH;
    const vec3 p = ed_obj_vec(o, "p", (vec3) {0});
    const int dir = (int) ed_json_num(o->data, "d", 0);
    // loadObj(..., scale 1, colour, "spwnMesh", y -5.5, x 0, rotY -(dir + 1) * PI / 2) inside the
    // object whose origin is the bounding-box centre (p + s.y / 2; the spawn box is 1 high)
    mesh->transform.position = (vec3) {p.x, p.y + 0.5f - 5.5f + 5.5f, p.z};
    mesh->transform.rotation = (vec3) {0.0f, -((float) dir + 1.0f) * (float) M_PI / 2.0f, 0.0f};
    mesh->transform.scale = (vec3) {1.0f, 1.0f, 1.0f};
    mesh->visible = 1;
    return mesh;
}

void ed_obj_rebuild(EditorObject *o) {
    obj_drop_render(o);
    if (o->prefab == ED_PREFAB_SPAWN_POINT) {
        o->helper = spawn_helper(o);
        if (o->helper && g_ed.proj->scene) scene_add_mesh(g_ed.proj->scene, o->helper);
        return;
    }
    if (editor_prefab_has(o->prefab, EP_TOOL) && !editor_prefab_has(o->prefab, EP_BOUNCE)) return; // editor helpers only
    if (o->prefab == ED_PREFAB_PLACEHOLDER) return;

    o->render_doc = cJSON_CreateObject();
    cJSON *objects = cJSON_AddArrayToObject(o->render_doc, "objects");
    cJSON_AddArrayToObject(o->render_doc, "spawns");
    cJSON *copy = cJSON_Duplicate(o->data, 1);
    // the editor always draws invisible objects' boxes; their meshes stay hidden (opacity 0)
    cJSON_AddItemToArray(objects, copy);
    o->render_map = map_init(o->render_doc);
    if (o->render_map && o->render_map->object_count) {
        o->mesh = o->render_map->objects[0]->mesh;
        if (o->mesh && g_ed.proj->scene) scene_add_mesh(g_ed.proj->scene, o->mesh);
    }
}

// ---------------------------------------------------------------------------
// the object list

EditorObject *ed_obj_find(const uint32_t uuid) {
    for (size_t i = 0; i < g_ed.proj->count; i++) {
        if (g_ed.proj->objects[i]->uuid == uuid) return g_ed.proj->objects[i];
    }
    return NULL;
}

int ed_obj_count_class(const EditorObject *o) {
    // cntCheck: 0 non-collidable, 1 collidable, 2 spawn point
    if (editor_prefab_has(o->prefab, EP_NOCOL)) return 0;
    cJSON *c = ed_prop_get(o, "collidable");
    const int collidable = cJSON_IsTrue(c);
    cJSON_Delete(c);
    if (!collidable) return 0;
    return o->prefab == ED_PREFAB_SPAWN_POINT ? 2 : 1;
}

void ed_obj_add(EditorObject *o, const int no_select, const int history) {
    if (!o) return;
    if (g_ed.proj->count == g_ed.proj->cap) {
        const size_t cap = g_ed.proj->cap ? g_ed.proj->cap * 2 : 256;
        EditorObject **objects = realloc(g_ed.proj->objects, cap * sizeof(EditorObject *));
        if (!objects) return;
        g_ed.proj->objects = objects;
        g_ed.proj->cap = cap;
    }
    g_ed.proj->objects[g_ed.proj->count++] = o;
    g_ed.proj->obj_count[ed_obj_count_class(o)]++;
    g_ed.dirty_counts = 1;
    ed_mark_changed();
    ed_obj_rebuild(o);
    cJSON_Delete(o->history);
    o->history = cJSON_Duplicate(o->data, 1);
    if (history) ed_history_add('a', o, NULL);
    if (!no_select) ed_obj_select(o);
}

void ed_obj_remove(EditorObject *o, const int keep_selection, const int history) {
    if (!o) o = g_ed.proj->selected;
    if (!o) return;
    size_t index = g_ed.proj->count;
    for (size_t i = 0; i < g_ed.proj->count; i++) {
        if (g_ed.proj->objects[i] == o) index = i;
    }
    if (index == g_ed.proj->count) return;
    memmove(&g_ed.proj->objects[index], &g_ed.proj->objects[index + 1], (g_ed.proj->count - index - 1) * sizeof(EditorObject *));
    g_ed.proj->count--;
    g_ed.proj->obj_count[ed_obj_count_class(o)]--;
    g_ed.dirty_counts = 1;
    ed_mark_changed();
    obj_drop_render(o);
    if (g_ed.proj->selected == o) {
        g_ed.proj->selected = NULL;
        if (!keep_selection) g_ed.dirty_panel = 1;
    }
    for (size_t i = 0; i < g_ed.proj->pending_count; i++) {
        if (g_ed.proj->pending_group[i] == o->uuid) g_ed.proj->pending_group[i] = 0;
    }
    if (history) ed_history_add('r', o, NULL); // the entry owns the object now
    else ed_obj_free(o);
}

void ed_objects_clear(void) {
    while (g_ed.proj->count) ed_obj_remove(g_ed.proj->objects[g_ed.proj->count - 1], 1, 0);
    free(g_ed.proj->objects);
    g_ed.proj->objects = NULL;
    g_ed.proj->cap = 0;
    memset(g_ed.proj->obj_count, 0, sizeof(g_ed.proj->obj_count));
    g_ed.proj->selected = NULL;
    g_ed.proj->pending_count = 0;
    for (size_t i = 0; i < g_ed.proj->group_count; i++) free(g_ed.proj->groups[i].objects);
    free(g_ed.proj->groups);
    g_ed.proj->groups = NULL;
    g_ed.proj->group_count = 0;
    g_ed.dirty_counts = 1;
}

void ed_obj_select(EditorObject *o) {
    g_ed.proj->selected = o;
    g_ed.gizmo.active = 0;
    g_ed.dirty_panel = 1; // updateObjConfigGUI
}

cJSON *ed_obj_serialize(const EditorObject *o) {
    return cJSON_Duplicate(o->data, 1);
}

void ed_duplicate_object(EditorObject *e) {
    // duplicateObject(e): clone, add and select
    if (!(e = e ? e : g_ed.proj->selected)) return;
    EditorObject *copy = ed_obj_new(e->data, 0);
    ed_obj_add(copy, 0, 1);
}

void ed_duplicate_group(EditorObject *e) {
    // duplicateGroup(e): copies of the members, a copy of the placeholder, then
    // createGroup(<the new placeholder>, copies) fits a fresh placeholder around them
    if (!g_ed.proj->group_count) return;
    if (!(e = e ? e : g_ed.proj->selected) || !ed_group_of(e)) {
        editor_message("You cant duplicate a group that doesnt exist");
        return;
    }
    const EditorGroup *g = ed_group_of(e);
    EditorObject **copies = calloc(g->count ? g->count : 1, sizeof(EditorObject *));
    if (!copies) return;
    size_t n = 0;
    for (size_t i = 0; i < g_ed.proj->count; i++) {
        EditorObject *m = g_ed.proj->objects[i];
        int member = 0;
        for (size_t k = 0; k < g->count && !member; k++) member = g->objects[k] == m->uuid;
        if (member && n < g->count) copies[n++] = m;
    }
    for (size_t i = 0; i < n; i++) {
        copies[i] = ed_obj_new(copies[i]->data, 0);
        ed_obj_add(copies[i], 1, 1);
    }
    ed_duplicate_object(e);
    ed_create_group(g_ed.proj->selected, copies, n, 0, 1);
    free(copies);
}

void ed_obj_duplicate(void) {
    if (g_ed.proj->selected && ed_group_of(g_ed.proj->selected)) ed_duplicate_group(NULL);
    else ed_duplicate_object(NULL);
}

static void group_drop(EditorGroup *g) {
    // delete this.objGroups[uuid]
    free(g->objects);
    const size_t index = (size_t) (g - g_ed.proj->groups);
    memmove(&g_ed.proj->groups[index], &g_ed.proj->groups[index + 1], (g_ed.proj->group_count - index - 1) * sizeof(EditorGroup));
    g_ed.proj->group_count--;
}

void ed_remove_group(EditorObject *e) {
    // removeGroup(e): the members and the placeholder
    if (!g_ed.proj->group_count || !(e = e ? e : g_ed.proj->selected)) return;
    EditorGroup *group = ed_group_of(e);
    if (!group) return;
    for (size_t i = 0; i < group->count; i++) {
        EditorObject *m = ed_obj_find(group->objects[i]);
        if (m) ed_obj_remove(m, 1, 1);
    }
    group_drop(group);
    ed_obj_remove(e, 0, 1);
}

void ed_obj_delete_selected(void) {
    EditorObject *o = g_ed.proj->selected;
    if (!o) return;
    if (ed_group_of(o)) ed_remove_group(o);
    else ed_obj_remove(o, 0, 1);
}

void ed_obj_create(const int prefab) {
    if (prefab < 0 || prefab >= EDITOR_PREFAB_COUNT) return;
    if (prefab == ED_PREFAB_CUSTOM_ASSET) {
        ed_net_view_assets(0); // createObjects[CUSTOM_ASSET]: viewAssets() (editor_net.c)
        return;
    }
    const vec3 p = editor_setting_bool("createObjectNear") ? editor_near_position() : (vec3) {0};
    cJSON *d = cJSON_CreateObject();
    if (prefab) cJSON_AddNumberToObject(d, "i", prefab);
    cJSON_AddItemToObject(d, "p", vec_json(p, 2));
    EditorObject *o = ed_obj_new(d, 0); // ObjectInstance.defaultFromType
    cJSON_Delete(d);
    ed_obj_add(o, 0, 1);
}

void ed_obj_create_placeholder(void) {
    // createPlaceholder: p = camera position - 10 on Y, s = [10, 10, 10] (P key)
    const vec3 cam = g_ed.proj->camera.position;
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "i", ED_PREFAB_PLACEHOLDER);
    cJSON_AddItemToObject(d, "p", vec_json((vec3) {cam.x, cam.y - 10, cam.z}, 2));
    cJSON_AddItemToObject(d, "s", vec_json((vec3) {10, 10, 10}, 2));
    EditorObject *o = ed_obj_new(d, 0);
    cJSON_Delete(d);
    ed_obj_add(o, 0, 1);
}

void ed_obj_toggle_prop_group(const char *prop) {
    // objectSelected(true) ? togglePropGroup(prop) : toggleProp(prop)
    const EditorGroup *g = ed_group_of(g_ed.proj->selected);
    if (!g) {
        ed_obj_toggle_prop(prop);
        return;
    }
    EditorObject *keep = g_ed.proj->selected;
    for (size_t i = 0; i < g->count; i++) {
        EditorObject *m = ed_obj_find(g->objects[i]);
        if (!m) continue;
        g_ed.proj->selected = m;
        ed_obj_toggle_prop(prop);
    }
    g_ed.proj->selected = keep;
}

void ed_obj_toggle_prop(const char *prop) {
    EditorObject *o = g_ed.proj->selected;
    if (!o) return;
    cJSON *v = ed_prop_get(o, prop);
    cJSON *nv = cJSON_CreateBool(!cJSON_IsTrue(v));
    const int before = ed_obj_count_class(o);
    ed_prop_set(o, prop, nv);
    g_ed.proj->obj_count[before]--;
    g_ed.proj->obj_count[ed_obj_count_class(o)]++;
    cJSON_Delete(v);
    cJSON_Delete(nv);
    ed_obj_rebuild(o);
    g_ed.dirty_counts = 1;
    g_ed.dirty_panel = 1;
    ed_mark_changed();
}

void ed_obj_flip_xz(void) {
    EditorObject *o = g_ed.proj->selected;
    if (!o) return;
    const vec3 s = ed_obj_vec(o, "s", (vec3) {1, 1, 1});
    ed_obj_set_vec(o, "s", (vec3) {s.z, s.y, s.x});
    ed_obj_rebuild(o);
    g_ed.dirty_panel = 1;
    ed_mark_changed();
}

// ---------------------------------------------------------------------------
// copy / paste / groups (copyObjects / copyGroup / pasteObjects / stopGroup): v3.1.1
// works on a placeholder stretched over objects. copyObjects copies (or cuts, or groups)
// every object whose bounding box intersects the selected one; the copy keeps absolute
// positions and pasteObjects puts it in place of the selected object as a group
// (replaceObject(e, copy, true, false, true)).

static void world_aabb(const EditorObject *o, vec3 *lo, vec3 *hi) {
    // detectCollisionCubes: boundingBox.applyMatrix4(matrixWorld) (an AABB of the rotated box)
    float m[16];
    ed_obj_world_box(o, m);
    *lo = (vec3) {1e30f, 1e30f, 1e30f};
    *hi = (vec3) {-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < 8; i++) {
        const float x = (i & 1) ? 0.5f : -0.5f, y = (i & 2) ? 0.5f : -0.5f, z = (i & 4) ? 0.5f : -0.5f;
        const float w[3] = {m[0] * x + m[1] * y + m[2] * z + m[3], m[4] * x + m[5] * y + m[6] * z + m[7], m[8] * x + m[9] * y + m[10] * z + m[11]};
        lo->x = fminf(lo->x, w[0]); lo->y = fminf(lo->y, w[1]); lo->z = fminf(lo->z, w[2]);
        hi->x = fmaxf(hi->x, w[0]); hi->y = fmaxf(hi->y, w[1]); hi->z = fmaxf(hi->z, w[2]);
    }
}

static int boxes_intersect(const EditorObject *a, const EditorObject *b) {
    vec3 al, ah, bl, bh;
    world_aabb(a, &al, &ah);
    world_aabb(b, &bl, &bh);
    return !(ah.x < bl.x || al.x > bh.x || ah.y < bl.y || al.y > bh.y || ah.z < bl.z || al.z > bh.z);
}

int ed_is_grouped(const uint32_t uuid) {
    // isGrouped: a member or the owner of any group
    for (size_t i = 0; i < g_ed.proj->group_count; i++) {
        if (g_ed.proj->groups[i].owner == uuid) return 1;
        for (size_t k = 0; k < g_ed.proj->groups[i].count; k++) if (g_ed.proj->groups[i].objects[k] == uuid) return 1;
    }
    return 0;
}

static void set_copy(cJSON *arr) {
    cJSON_Delete(g_ed.copy);
    g_ed.copy = arr; // this.copy = JSON.stringify(r)
    ed_bus_clipboard_set(arr); // every project and editor window pastes it (editor_bus_link.c)
}

void ed_copy_objects(const int cut, const int group, EditorObject *sel) {
    if (!(sel = sel ? sel : g_ed.proj->selected)) {
        editor_message("Stretch a cube over your objects then try again");
        return;
    }
    if (group && ed_group_of(sel)) {
        editor_message("You cant combine groups");
        return;
    }
    EditorObject **hit = calloc(g_ed.proj->count ? g_ed.proj->count : 1, sizeof(EditorObject *));
    if (!hit) return;
    size_t n = 0;
    for (size_t i = 0; i < g_ed.proj->count; i++) {
        EditorObject *o = g_ed.proj->objects[i];
        if (o == sel || !boxes_intersect(o, sel)) continue;
        if (group && ed_is_grouped(o->uuid)) {
            editor_message("You cant combine groups");
            free(hit);
            return;
        }
        hit[n++] = o;
    }
    if (group) {
        ed_create_group(sel, hit, n, 0, 1); // createGroup(i, a)
    } else {
        cJSON *arr = cJSON_CreateArray();
        for (size_t i = 0; i < n; i++) cJSON_AddItemToArray(arr, ed_obj_serialize(hit[i]));
        if (cut) for (size_t i = 0; i < n; i++) ed_obj_remove(hit[i], 1, 1);
        set_copy(arr);
    }
    free(hit);
}

cJSON *ed_copy_group(EditorObject *e) {
    // copyGroup(e): the members of the selected group (returns the copy, borrowed)
    if (!g_ed.proj->group_count || !(e = e ? e : g_ed.proj->selected)) return NULL;
    const EditorGroup *g = ed_group_of(e);
    if (!g) return NULL;
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < g_ed.proj->count; i++) {
        for (size_t k = 0; k < g->count; k++) {
            if (g->objects[k] == g_ed.proj->objects[i]->uuid) {
                cJSON_AddItemToArray(arr, ed_obj_serialize(g_ed.proj->objects[i]));
                break;
            }
        }
    }
    set_copy(arr);
    return arr;
}

void ed_obj_copy(const int cut, const int as_group) {
    // ctrl+C / advancedOptions.copy: objectSelected(true) ? copyGroup() : copyObjects();
    // cut: copyObjects(true); create: copyObjects(false, true)
    if (!cut && !as_group && g_ed.proj->selected && ed_group_of(g_ed.proj->selected)) ed_copy_group(NULL);
    else ed_copy_objects(cut, as_group, NULL);
}

void ed_paste_objects(EditorObject *e) {
    e = e ? e : g_ed.proj->selected;
    if (!g_ed.copy) {
        editor_message("Please copy objects first");
        return;
    }
    if (!e) {
        editor_message("Select a object you would like to replace with your copied objects");
        return;
    }
    char *text = cJSON_PrintUnformatted(g_ed.copy);
    if (text) ed_obj_replace(e, text, NULL, 1);
    free(text);
}

void ed_obj_paste(void) { ed_paste_objects(NULL); }

EditorGroup *ed_group_of(const EditorObject *owner) {
    if (!owner) return NULL;
    for (size_t i = 0; i < g_ed.proj->group_count; i++) {
        if (g_ed.proj->groups[i].owner == owner->uuid) return &g_ed.proj->groups[i];
    }
    return NULL;
}

void ed_group_create(void) { ed_copy_objects(0, 1, NULL); } // advancedOptions.create

void ed_group_from_pending(void) {
    // updateGroupSelection (Group Selection turned off with picked objects):
    // createGroup(null, picked, 0, false)
    if (!g_ed.proj->pending_count) return;
    EditorObject **list = calloc(g_ed.proj->pending_count, sizeof(EditorObject *));
    if (!list) return;
    size_t n = 0;
    for (size_t i = 0; i < g_ed.proj->count; i++) {
        EditorObject *o = g_ed.proj->objects[i];
        int picked = 0;
        for (size_t k = 0; k < g_ed.proj->pending_count && !picked; k++) picked = g_ed.proj->pending_group[k] == o->uuid;
        if (!picked) continue;
        o->highlighted = 0;
        if (ed_is_grouped(o->uuid)) {
            editor_message("You cant combine groups");
            free(list);
            return;
        }
        list[n++] = o;
    }
    g_ed.proj->pending_count = 0;
    if (!n) editor_message("Nothing to Group");
    else ed_create_group(NULL, list, n, 0, 0);
    free(list);
}

void ed_group_stop(const int all) {
    // stopGroup(e): the selected group's placeholder goes, its members stay; all: every
    // placeholder that owns a group
    if (!g_ed.proj->group_count) {
        editor_message("You cant stop a group that doesnt exist");
        return;
    }
    if (!all) {
        EditorObject *sel = g_ed.proj->selected;
        EditorGroup *g = ed_group_of(sel);
        if (!g) {
            editor_message("You cant stop a group that doesnt exist");
            return;
        }
        group_drop(g);
        ed_obj_remove(sel, 0, 1);
        return;
    }
    while (g_ed.proj->group_count) {
        EditorObject *owner = ed_obj_find(g_ed.proj->groups[g_ed.proj->group_count - 1].owner);
        group_drop(&g_ed.proj->groups[g_ed.proj->group_count - 1]);
        if (owner) ed_obj_remove(owner, 0, 1);
    }
    free(g_ed.proj->groups);
    g_ed.proj->groups = NULL;
}

void ed_groups_sync(void) {
    // updateGroups: members follow the placeholder's translation, Y rotation and scale
    for (size_t gi = 0; gi < g_ed.proj->group_count; gi++) {
        EditorGroup *g = &g_ed.proj->groups[gi];
        EditorObject *owner = ed_obj_find(g->owner);
        if (!owner) continue;
        const vec3 p = ed_obj_vec(owner, "p", (vec3) {0});
        const vec3 s = ed_obj_vec(owner, "s", (vec3) {1, 1, 1});
        const vec3 r = ed_obj_vec(owner, "r", (vec3) {0});
        const vec3 d = {p.x - g->pos.x, p.y - g->pos.y, p.z - g->pos.z};
        const float dr = r.y - g->rot_y;
        const vec3 k = {g->scale.x ? s.x / g->scale.x : 1, g->scale.y ? s.y / g->scale.y : 1, g->scale.z ? s.z / g->scale.z : 1};
        if (!d.x && !d.y && !d.z && !dr && k.x == 1 && k.y == 1 && k.z == 1) continue;
        const float c = cosf(dr), sn = sinf(dr);
        for (size_t i = 0; i < g->count; i++) {
            EditorObject *m = ed_obj_find(g->objects[i]);
            if (!m) continue;
            vec3 mp = ed_obj_vec(m, "p", (vec3) {0});
            vec3 ms = ed_obj_vec(m, "s", (vec3) {1, 1, 1});
            vec3 mr = ed_obj_vec(m, "r", (vec3) {0});
            // relative to the old pivot, scaled, rotated about Y, moved to the new pivot
            float rx = (mp.x - g->pos.x) * k.x, ry = (mp.y - g->pos.y) * k.y, rz = (mp.z - g->pos.z) * k.z;
            const float nx = rx * c + rz * sn, nz = -rx * sn + rz * c;
            mp = (vec3) {p.x + nx, p.y + ry, p.z + nz};
            ms = (vec3) {ms.x * k.x, ms.y * k.y, ms.z * k.z};
            mr.y += dr;
            ed_obj_set_vec(m, "p", mp);
            if (editor_prefab_has(m->prefab, EP_SCALABLE)) ed_obj_set_vec(m, "s", ms);
            ed_obj_set_vec(m, "r", mr);
            ed_obj_rebuild(m);
        }
        g->pos = p;
        g->scale = s;
        g->rot_y = r.y;
        ed_mark_changed();
    }
}

// ---------------------------------------------------------------------------
// history: editor.js initHistory polls every object once a second and records an
// "update" with the previous serialization; add/remove are recorded immediately.

static void entry_free(EditorHistoryEntry *e) {
    cJSON_Delete(e->state);
    free(e->label);
    e->state = NULL;
    e->label = NULL;
}

static void history_push(EditorHistoryEntry **list, size_t *count, size_t *cap, const EditorHistoryEntry e) {
    const size_t limit = (size_t) editor_setting_num("historyLimit", 1000);
    if (*count == *cap) {
        const size_t ncap = *cap ? *cap * 2 : 64;
        EditorHistoryEntry *n = realloc(*list, ncap * sizeof(EditorHistoryEntry));
        if (!n) {
            EditorHistoryEntry lost = e;
            entry_free(&lost);
            return;
        }
        *list = n;
        *cap = ncap;
    }
    (*list)[(*count)++] = e;
    // Krunker drops the newest entry (undos.pop()) once over the limit; a bus batch is
    // never cut in half by it (the limit counts steps of the UI, batches are one step)
    if (limit && *count > limit && !e.batch) entry_free(&(*list)[--(*count)]);
}

// removed objects that undo can revive keep their uuid and meshes (per project)
static void graveyard_add(EditorObject *o) {
    EditorProject *p = g_ed.proj;
    EdGraveyard *n = realloc(p->graveyard, (p->graveyard_count + 1) * sizeof(EdGraveyard));
    if (!n) {
        ed_obj_free(o);
        return;
    }
    p->graveyard = n;
    p->graveyard[p->graveyard_count++] = (EdGraveyard) {o->uuid, o};
}

static EditorObject *graveyard_take(const uint32_t uuid) {
    EditorProject *p = g_ed.proj;
    for (size_t i = 0; i < p->graveyard_count; i++) {
        if (p->graveyard[i].uuid == uuid) {
            EditorObject *o = p->graveyard[i].object;
            p->graveyard[i] = p->graveyard[--p->graveyard_count];
            return o;
        }
    }
    return NULL;
}

static EditorHistoryEntry new_entry(const int action, const uint32_t uuid, cJSON *state) {
    EditorProject *p = g_ed.proj;
    EditorHistoryEntry e = {action, uuid, state, p->batch, NULL};
    if (p->batch && p->batch_label) e.label = strdup(p->batch_label);
    return e;
}

void ed_history_add(const int action, EditorObject *o, cJSON *state) {
    if (editor_setting_bool("disableHistory")) {
        if (action == 'r') ed_obj_free(o);
        cJSON_Delete(state);
        return;
    }
    EditorHistoryEntry e = new_entry(action, o->uuid, NULL);
    if (action == 'a' || action == 'r') e.state = cJSON_Duplicate(o->data, 1);
    else e.state = state;
    if (action == 'r') graveyard_add(o); // removed objects can come back with their uuid
    history_push(&g_ed.proj->undos, &g_ed.proj->undo_count, &g_ed.proj->undo_cap, e);
    // a new action invalidates the redo list only implicitly in Krunker (it keeps it); keep parity
}

// map config snapshot for 'm' entries (bus map_set / map_unset)
static cJSON *map_snapshot(void) {
    cJSON *s = cJSON_CreateObject();
    cJSON_AddItemToObject(s, "map", cJSON_Duplicate(g_ed.proj->map_config, 1));
    cJSON_AddItemToObject(s, "game", cJSON_Duplicate(g_ed.proj->game_config, 1));
    cJSON_AddItemToObject(s, "server", cJSON_Duplicate(g_ed.proj->server_config, 1));
    cJSON_AddItemToObject(s, "extra", cJSON_Duplicate(g_ed.proj->extra_map, 1));
    return s;
}

static void map_restore(const cJSON *s) {
    EditorProject *p = g_ed.proj;
    static const char *const keys[4] = {"map", "game", "server", "extra"};
    cJSON **dst[4] = {&p->map_config, &p->game_config, &p->server_config, &p->extra_map};
    for (int i = 0; i < 4; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(s, keys[i]);
        if (!v) continue;
        cJSON_Delete(*dst[i]);
        *dst[i] = cJSON_IsNull(v) ? NULL : cJSON_Duplicate(v, 1);
    }
    editor_apply_lighting();
    ed_terrain_regen();
    ed_zone_regen();
    ed_panel_refresh_all();
}

void ed_history_add_map(void) {
    if (editor_setting_bool("disableHistory")) return;
    history_push(&g_ed.proj->undos, &g_ed.proj->undo_count, &g_ed.proj->undo_cap, new_entry('m', 0, map_snapshot()));
}

uint32_t ed_history_batch_begin(const char *label) {
    EditorProject *p = g_ed.proj;
    if (p->batch) return 0; // nested: the outer batch owns the step
    p->batch = ++p->next_batch ? p->next_batch : ++p->next_batch;
    p->batch_label = label;
    return p->batch;
}

void ed_history_batch_end(const uint32_t id) {
    EditorProject *p = g_ed.proj;
    if (!id || p->batch != id) return;
    p->batch = 0;
    p->batch_label = NULL;
}

static EditorObject *revive(const EditorHistoryEntry *e) {
    EditorObject *o = graveyard_take(e->uuid);
    if (!o) {
        cJSON *d = cJSON_Duplicate(e->state, 1);
        cJSON_AddNumberToObject(d, "objUUID", e->uuid);
        o = ed_obj_new(d, 1);
        cJSON_Delete(d);
    }
    return o;
}

static void history_execute_one(const int redo, EditorHistoryEntry e) {
    EditorProject *p = g_ed.proj;
    EditorObject *o = e.action == 'm' ? NULL : ed_obj_find(e.uuid);
    switch (e.action) {
        case 'a':
            if (!redo) {
                if (o) ed_obj_remove(o, 0, 0);
                history_push(&p->redos, &p->redo_count, &p->redo_cap, e);
                ed_mark_changed();
                return;
            }
            ed_obj_add(revive(&e), 0, 0);
            break;
        case 'r':
            if (!redo) {
                ed_obj_add(revive(&e), 0, 0);
                history_push(&p->redos, &p->redo_count, &p->redo_cap, e);
                ed_mark_changed();
                return;
            }
            if (o) ed_obj_remove(o, 0, 0);
            break;
        case 'u':
            if (o && e.state) {
                if (!redo) {
                    // the redo entry restores the state we are leaving
                    EditorHistoryEntry back = {'u', o->uuid, cJSON_Duplicate(o->data, 1), e.batch, e.label ? strdup(e.label) : NULL};
                    history_push(&p->redos, &p->redo_count, &p->redo_cap, back);
                }
                const int before = ed_obj_count_class(o);
                cJSON_Delete(o->data);
                o->data = cJSON_Duplicate(e.state, 1);
                p->obj_count[before]--;
                p->obj_count[ed_obj_count_class(o)]++;
                ed_obj_rebuild(o);
                cJSON_Delete(o->history);
                o->history = cJSON_Duplicate(o->data, 1);
                g_ed.dirty_counts = 1;
                if (o == p->selected) g_ed.dirty_panel = 1;
            }
            break;
        case 'm':
            if (e.state) {
                if (!redo) {
                    EditorHistoryEntry back = {'m', 0, map_snapshot(), e.batch, e.label ? strdup(e.label) : NULL};
                    history_push(&p->redos, &p->redo_count, &p->redo_cap, back);
                }
                map_restore(e.state);
            }
            break;
    }
    entry_free(&e);
    ed_mark_changed();
}

int ed_history_step(const int redo) {
    // one step: a single entry, or every entry of the newest batch
    EditorProject *p = g_ed.proj;
    EditorHistoryEntry *list = redo ? p->redos : p->undos;
    size_t *count = redo ? &p->redo_count : &p->undo_count;
    if (!*count) return 0;
    const uint32_t batch = list[*count - 1].batch;
    do {
        // list may move (history_push into the other list never reallocates this one)
        list = redo ? p->redos : p->undos;
        const EditorHistoryEntry e = list[--(*count)];
        history_execute_one(redo, e);
    } while (batch && *count && (redo ? p->redos : p->undos)[*count - 1].batch == batch);
    return 1;
}

void ed_history_execute(const int redo) { ed_history_step(redo); }

void ed_history_poll(void) {
    if (editor_setting_bool("disableHistory") || !editor_setting_num("historyLimit", 1000)) return;
    for (size_t i = 0; i < g_ed.proj->count; i++) {
        EditorObject *o = g_ed.proj->objects[i];
        if (o->history && !cJSON_Compare(o->history, o->data, 1)) {
            ed_history_add('u', o, o->history);
            o->history = NULL;
            ed_mark_changed();
        }
        if (!o->history) o->history = cJSON_Duplicate(o->data, 1);
    }
}

// the label of the step ending at list[i] (newest first listing)
static void step_label(const EditorHistoryEntry *list, const size_t i, size_t first, char *out, const size_t n) {
    const EditorHistoryEntry *e = &list[i];
    if (e->label) {
        snprintf(out, n, "%s", e->label);
        return;
    }
    const size_t steps = i - first + 1;
    const char *verb = e->action == 'a' ? "add" : e->action == 'r' ? "remove" : e->action == 'm' ? "map settings" : "edit";
    if (e->action == 'm') {
        snprintf(out, n, "%s", verb);
        return;
    }
    int prefab = -1;
    const cJSON *pi = e->state ? cJSON_GetObjectItemCaseSensitive(e->state, "i") : NULL;
    prefab = cJSON_IsNumber(pi) ? (int) cJSON_GetNumberValue(pi) : 0;
    const char *name = prefab >= 0 && prefab < EDITOR_PREFAB_COUNT ? editor_prefabs[prefab].label : "object";
    if (steps > 1) snprintf(out, n, "%s %zu objects", verb, steps);
    else snprintf(out, n, "%s %s", verb, name);
}

cJSON *ed_history_labels(const int redo, const int limit) {
    const EditorProject *p = g_ed.proj;
    const EditorHistoryEntry *list = redo ? p->redos : p->undos;
    size_t i = redo ? p->redo_count : p->undo_count;
    cJSON *out = cJSON_CreateArray();
    while (i > 0 && (limit <= 0 || cJSON_GetArraySize(out) < limit)) {
        size_t first = i - 1;
        const uint32_t batch = list[i - 1].batch;
        while (batch && first > 0 && list[first - 1].batch == batch) first--;
        char label[160];
        step_label(list, i - 1, first, label, sizeof(label));
        cJSON_AddItemToArray(out, cJSON_CreateString(label));
        i = first;
    }
    return out;
}

size_t ed_history_steps(const int redo) {
    const EditorProject *p = g_ed.proj;
    const EditorHistoryEntry *list = redo ? p->redos : p->undos;
    size_t i = redo ? p->redo_count : p->undo_count, steps = 0;
    while (i > 0) {
        const uint32_t batch = list[--i].batch;
        while (batch && i > 0 && list[i - 1].batch == batch) i--;
        steps++;
    }
    return steps;
}

void ed_history_clear(void) {
    EditorProject *p = g_ed.proj;
    for (size_t i = 0; i < p->undo_count; i++) entry_free(&p->undos[i]);
    for (size_t i = 0; i < p->redo_count; i++) entry_free(&p->redos[i]);
    p->undo_count = p->redo_count = 0;
    for (size_t i = 0; i < p->graveyard_count; i++) ed_obj_free(p->graveyard[i].object);
    free(p->graveyard);
    p->graveyard = NULL;
    p->graveyard_count = 0;
}
