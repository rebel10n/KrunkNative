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
    o->uuid = keep_uuid && cJSON_IsNumber(uuid) ? (uint32_t) cJSON_GetNumberValue(uuid) : ++g_ed.next_uuid;
    if (o->uuid > g_ed.next_uuid) g_ed.next_uuid = o->uuid;
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
    if (o->mesh && g_ed.scene) scene_remove_mesh(g_ed.scene, o->mesh);
    if (o->helper && g_ed.scene) scene_remove_mesh(g_ed.scene, o->helper);
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
        if (o->helper && g_ed.scene) scene_add_mesh(g_ed.scene, o->helper);
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
        if (o->mesh && g_ed.scene) scene_add_mesh(g_ed.scene, o->mesh);
    }
}

// ---------------------------------------------------------------------------
// the object list

EditorObject *ed_obj_find(const uint32_t uuid) {
    for (size_t i = 0; i < g_ed.count; i++) {
        if (g_ed.objects[i]->uuid == uuid) return g_ed.objects[i];
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
    if (g_ed.count == g_ed.cap) {
        const size_t cap = g_ed.cap ? g_ed.cap * 2 : 256;
        EditorObject **objects = realloc(g_ed.objects, cap * sizeof(EditorObject *));
        if (!objects) return;
        g_ed.objects = objects;
        g_ed.cap = cap;
    }
    g_ed.objects[g_ed.count++] = o;
    g_ed.obj_count[ed_obj_count_class(o)]++;
    g_ed.dirty_counts = 1;
    g_ed.dirty_storage = 1;
    ed_obj_rebuild(o);
    cJSON_Delete(o->history);
    o->history = cJSON_Duplicate(o->data, 1);
    if (history) ed_history_add('a', o, NULL);
    if (!no_select) ed_obj_select(o);
}

void ed_obj_remove(EditorObject *o, const int keep_selection, const int history) {
    if (!o) o = g_ed.selected;
    if (!o) return;
    size_t index = g_ed.count;
    for (size_t i = 0; i < g_ed.count; i++) {
        if (g_ed.objects[i] == o) index = i;
    }
    if (index == g_ed.count) return;
    memmove(&g_ed.objects[index], &g_ed.objects[index + 1], (g_ed.count - index - 1) * sizeof(EditorObject *));
    g_ed.count--;
    g_ed.obj_count[ed_obj_count_class(o)]--;
    g_ed.dirty_counts = 1;
    g_ed.dirty_storage = 1;
    obj_drop_render(o);
    if (g_ed.selected == o) {
        g_ed.selected = NULL;
        if (!keep_selection) g_ed.dirty_panel = 1;
    }
    for (size_t i = 0; i < g_ed.pending_count; i++) {
        if (g_ed.pending_group[i] == o->uuid) g_ed.pending_group[i] = 0;
    }
    if (history) ed_history_add('r', o, NULL); // the entry owns the object now
    else ed_obj_free(o);
}

void ed_objects_clear(void) {
    while (g_ed.count) ed_obj_remove(g_ed.objects[g_ed.count - 1], 1, 0);
    free(g_ed.objects);
    g_ed.objects = NULL;
    g_ed.cap = 0;
    memset(g_ed.obj_count, 0, sizeof(g_ed.obj_count));
    g_ed.selected = NULL;
    g_ed.pending_count = 0;
    for (size_t i = 0; i < g_ed.group_count; i++) free(g_ed.groups[i].objects);
    free(g_ed.groups);
    g_ed.groups = NULL;
    g_ed.group_count = 0;
    g_ed.dirty_counts = 1;
}

void ed_obj_select(EditorObject *o) {
    g_ed.selected = o;
    g_ed.gizmo.active = 0;
    g_ed.dirty_panel = 1; // updateObjConfigGUI
}

cJSON *ed_obj_serialize(const EditorObject *o) {
    return cJSON_Duplicate(o->data, 1);
}

void ed_obj_duplicate(void) {
    EditorGroup *group = g_ed.selected ? ed_group_of(g_ed.selected) : NULL;
    if (group) {
        // duplicateGroup: copy the members and a new placeholder
        ed_obj_copy(0, 1);
        ed_obj_paste();
        return;
    }
    if (!g_ed.selected) return;
    EditorObject *copy = ed_obj_new(g_ed.selected->data, 0);
    ed_obj_add(copy, 0, 1);
}

void ed_obj_delete_selected(void) {
    EditorObject *o = g_ed.selected;
    if (!o) return;
    EditorGroup *group = ed_group_of(o);
    if (group) {
        // removeGroup: members first, then the placeholder
        for (size_t i = 0; i < group->count; i++) {
            EditorObject *m = ed_obj_find(group->objects[i]);
            if (m) ed_obj_remove(m, 1, 1);
        }
        const uint32_t owner = group->owner;
        ed_group_stop(0);
        EditorObject *p = ed_obj_find(owner);
        if (p) ed_obj_remove(p, 0, 1);
        return;
    }
    ed_obj_remove(o, 0, 1);
}

void ed_obj_create(const int prefab) {
    if (prefab < 0 || prefab >= EDITOR_PREFAB_COUNT) return;
    if (prefab == ED_PREFAB_CUSTOM_ASSET) {
        editor_message("Custom assets are loaded from the Krunker asset server, which KrunkNative does not use.");
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
    // createPlaceholder: a placeholder object at the near position (P key)
    const vec3 p = editor_near_position();
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "i", ED_PREFAB_PLACEHOLDER);
    cJSON_AddItemToObject(d, "p", vec_json(p, 2));
    EditorObject *o = ed_obj_new(d, 0);
    cJSON_Delete(d);
    ed_obj_add(o, 0, 1);
}

void ed_obj_toggle_prop(const char *prop) {
    EditorObject *o = g_ed.selected;
    if (!o) return;
    cJSON *v = ed_prop_get(o, prop);
    cJSON *nv = cJSON_CreateBool(!cJSON_IsTrue(v));
    const int before = ed_obj_count_class(o);
    ed_prop_set(o, prop, nv);
    g_ed.obj_count[before]--;
    g_ed.obj_count[ed_obj_count_class(o)]++;
    cJSON_Delete(v);
    cJSON_Delete(nv);
    ed_obj_rebuild(o);
    g_ed.dirty_counts = 1;
    g_ed.dirty_panel = 1;
    g_ed.dirty_storage = 1;
}

void ed_obj_flip_xz(void) {
    EditorObject *o = g_ed.selected;
    if (!o) return;
    const vec3 s = ed_obj_vec(o, "s", (vec3) {1, 1, 1});
    ed_obj_set_vec(o, "s", (vec3) {s.z, s.y, s.x});
    ed_obj_rebuild(o);
    g_ed.dirty_panel = 1;
    g_ed.dirty_storage = 1;
}

// ---------------------------------------------------------------------------
// copy / paste (copyObjects / pasteObjects): objects are stored relative to the
// selection centre and pasted around the near position.

static void bounds_center(cJSON *arr, vec3 *center) {
    vec3 lo = {1e9f, 1e9f, 1e9f}, hi = {-1e9f, -1e9f, -1e9f};
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        const cJSON *p = ed_json_path(it, "p");
        if (cJSON_GetArraySize(p) < 3) continue;
        const float x = (float) value_num(cJSON_GetArrayItem(p, 0)), y = (float) value_num(cJSON_GetArrayItem(p, 1)), z = (float) value_num(cJSON_GetArrayItem(p, 2));
        lo.x = fminf(lo.x, x); lo.y = fminf(lo.y, y); lo.z = fminf(lo.z, z);
        hi.x = fmaxf(hi.x, x); hi.y = fmaxf(hi.y, y); hi.z = fmaxf(hi.z, z);
    }
    *center = (vec3) {(lo.x + hi.x) / 2.0f, lo.y, (lo.z + hi.z) / 2.0f}; // findCenter: x/z centre, lowest y
}

void ed_obj_copy(const int cut, const int as_group) {
    cJSON *arr = cJSON_CreateArray();
    EditorObject *sel = g_ed.selected;
    EditorGroup *group = sel ? ed_group_of(sel) : NULL;
    if (as_group && !group && g_ed.pending_count) {
        ed_group_create();
        cJSON_Delete(arr);
        return;
    }
    if (group) {
        for (size_t i = 0; i < group->count; i++) {
            EditorObject *m = ed_obj_find(group->objects[i]);
            if (m) cJSON_AddItemToArray(arr, ed_obj_serialize(m));
        }
    } else if (g_ed.pending_count) {
        for (size_t i = 0; i < g_ed.pending_count; i++) {
            EditorObject *m = ed_obj_find(g_ed.pending_group[i]);
            if (m) cJSON_AddItemToArray(arr, ed_obj_serialize(m));
        }
    } else if (sel) {
        cJSON_AddItemToArray(arr, ed_obj_serialize(sel));
    }
    if (!cJSON_GetArraySize(arr)) {
        cJSON_Delete(arr);
        return;
    }
    vec3 c;
    bounds_center(arr, &c);
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(it, "p");
        cJSON_SetNumberValue(cJSON_GetArrayItem(p, 0), ed_round(value_num(cJSON_GetArrayItem(p, 0)) - c.x, 2));
        cJSON_SetNumberValue(cJSON_GetArrayItem(p, 1), ed_round(value_num(cJSON_GetArrayItem(p, 1)) - c.y, 2));
        cJSON_SetNumberValue(cJSON_GetArrayItem(p, 2), ed_round(value_num(cJSON_GetArrayItem(p, 2)) - c.z, 2));
    }
    cJSON_Delete(g_ed.copy);
    g_ed.copy = arr;
    char *text = cJSON_PrintUnformatted(arr);
    if (text) {
        ed_clipboard_copy(text); // Krunker copies the JSON to the system clipboard too
        free(text);
    }
    if (cut) {
        if (group) ed_obj_delete_selected();
        else if (g_ed.pending_count) {
            for (size_t i = 0; i < g_ed.pending_count; i++) {
                EditorObject *m = ed_obj_find(g_ed.pending_group[i]);
                if (m) ed_obj_remove(m, 1, 1);
            }
            g_ed.pending_count = 0;
        } else ed_obj_remove(sel, 0, 1);
    }
}

void ed_obj_paste(void) {
    if (!g_ed.copy) return;
    const vec3 at = g_ed.selected ? ed_obj_vec(g_ed.selected, "p", (vec3) {0}) : editor_near_position();
    EditorObject *last = NULL;
    const cJSON *it;
    cJSON_ArrayForEach(it, g_ed.copy) {
        EditorObject *o = ed_obj_new(it, 0);
        if (!o) continue;
        const vec3 p = ed_obj_vec(o, "p", (vec3) {0});
        ed_obj_set_vec(o, "p", (vec3) {p.x + at.x, p.y + at.y, p.z + at.z});
        ed_obj_add(o, 1, 1);
        last = o;
    }
    if (last && cJSON_GetArraySize(g_ed.copy) == 1) ed_obj_select(last);
}

// ---------------------------------------------------------------------------
// groups (placeholder owners, editor.js createGroup / updateGroups / stopGroup)

EditorGroup *ed_group_of(const EditorObject *owner) {
    if (!owner) return NULL;
    for (size_t i = 0; i < g_ed.group_count; i++) {
        if (g_ed.groups[i].owner == owner->uuid) return &g_ed.groups[i];
    }
    return NULL;
}

void ed_group_create(void) {
    if (!g_ed.pending_count) {
        editor_message("Select objects with Group Selection first (N).");
        return;
    }
    vec3 lo = {1e9f, 1e9f, 1e9f}, hi = {-1e9f, -1e9f, -1e9f};
    size_t members = 0;
    for (size_t i = 0; i < g_ed.pending_count; i++) {
        EditorObject *m = ed_obj_find(g_ed.pending_group[i]);
        if (!m) continue;
        const vec3 p = ed_obj_vec(m, "p", (vec3) {0});
        const vec3 s = ed_obj_vec(m, "s", (vec3) {1, 1, 1});
        lo.x = fminf(lo.x, p.x - s.x / 2); hi.x = fmaxf(hi.x, p.x + s.x / 2);
        lo.y = fminf(lo.y, p.y); hi.y = fmaxf(hi.y, p.y + s.y);
        lo.z = fminf(lo.z, p.z - s.z / 2); hi.z = fmaxf(hi.z, p.z + s.z / 2);
        members++;
    }
    if (!members) return;
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "i", ED_PREFAB_PLACEHOLDER);
    cJSON_AddItemToObject(d, "p", vec_json((vec3) {(lo.x + hi.x) / 2, lo.y, (lo.z + hi.z) / 2}, 2));
    cJSON_AddItemToObject(d, "s", vec_json((vec3) {hi.x - lo.x, hi.y - lo.y, hi.z - lo.z}, 2));
    EditorObject *owner = ed_obj_new(d, 0);
    cJSON_Delete(d);
    ed_obj_add(owner, 1, 1);

    EditorGroup *groups = realloc(g_ed.groups, (g_ed.group_count + 1) * sizeof(EditorGroup));
    if (!groups) return;
    g_ed.groups = groups;
    EditorGroup *g = &g_ed.groups[g_ed.group_count++];
    memset(g, 0, sizeof(*g));
    g->owner = owner->uuid;
    g->objects = calloc(members, sizeof(uint32_t));
    for (size_t i = 0; i < g_ed.pending_count && g->objects; i++) {
        EditorObject *m = ed_obj_find(g_ed.pending_group[i]);
        if (m) {
            g->objects[g->count++] = m->uuid;
            m->highlighted = 0;
        }
    }
    g->pos = ed_obj_vec(owner, "p", (vec3) {0});
    g->scale = ed_obj_vec(owner, "s", (vec3) {1, 1, 1});
    g->rot_y = 0.0f;
    g_ed.pending_count = 0;
    g_ed.group_selection = 0;
    editor_update_group_selection();
    ed_obj_select(owner);
}

void ed_group_stop(const int all) {
    if (all) {
        for (size_t i = 0; i < g_ed.group_count; i++) free(g_ed.groups[i].objects);
        free(g_ed.groups);
        g_ed.groups = NULL;
        g_ed.group_count = 0;
        return;
    }
    EditorGroup *g = ed_group_of(g_ed.selected);
    if (!g) return;
    free(g->objects);
    const size_t index = (size_t) (g - g_ed.groups);
    memmove(&g_ed.groups[index], &g_ed.groups[index + 1], (g_ed.group_count - index - 1) * sizeof(EditorGroup));
    g_ed.group_count--;
}

void ed_groups_sync(void) {
    // updateGroups: members follow the placeholder's translation, Y rotation and scale
    for (size_t gi = 0; gi < g_ed.group_count; gi++) {
        EditorGroup *g = &g_ed.groups[gi];
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
        g_ed.dirty_storage = 1;
    }
}

// ---------------------------------------------------------------------------
// history: editor.js initHistory polls every object once a second and records an
// "update" with the previous serialization; add/remove are recorded immediately.

static void history_push(EditorHistoryEntry **list, size_t *count, size_t *cap, const EditorHistoryEntry e) {
    const size_t limit = (size_t) editor_setting_num("historyLimit", 1000);
    if (*count == *cap) {
        const size_t ncap = *cap ? *cap * 2 : 64;
        EditorHistoryEntry *n = realloc(*list, ncap * sizeof(EditorHistoryEntry));
        if (!n) return;
        *list = n;
        *cap = ncap;
    }
    (*list)[(*count)++] = e;
    if (limit && *count > limit) {
        // Krunker drops the newest entry (undos.pop()) once over the limit
        EditorHistoryEntry *last = &(*list)[--(*count)];
        if (last->action == 'r' && last->state == NULL) {
            // removal entries own the removed object through state == NULL + uuid lookup (not reachable)
        }
        cJSON_Delete(last->state);
    }
}

static EditorObject *g_removed_objects_dummy;

typedef struct { uint32_t uuid; EditorObject *object; } Graveyard;
static Graveyard *g_graveyard;
static size_t g_graveyard_count;

static void graveyard_add(EditorObject *o) {
    Graveyard *n = realloc(g_graveyard, (g_graveyard_count + 1) * sizeof(Graveyard));
    if (!n) {
        ed_obj_free(o);
        return;
    }
    g_graveyard = n;
    g_graveyard[g_graveyard_count++] = (Graveyard) {o->uuid, o};
}

static EditorObject *graveyard_take(const uint32_t uuid) {
    for (size_t i = 0; i < g_graveyard_count; i++) {
        if (g_graveyard[i].uuid == uuid) {
            EditorObject *o = g_graveyard[i].object;
            g_graveyard[i] = g_graveyard[--g_graveyard_count];
            return o;
        }
    }
    return NULL;
}

void ed_history_add(const int action, EditorObject *o, cJSON *state) {
    (void) g_removed_objects_dummy;
    if (editor_setting_bool("disableHistory")) {
        if (action == 'r') ed_obj_free(o);
        cJSON_Delete(state);
        return;
    }
    EditorHistoryEntry e = {action, o->uuid, NULL};
    if (action == 'a' || action == 'r') e.state = cJSON_Duplicate(o->data, 1);
    else e.state = state;
    if (action == 'r') graveyard_add(o); // removed objects can come back with their uuid
    history_push(&g_ed.undos, &g_ed.undo_count, &g_ed.undo_cap, e);
    // a new action invalidates the redo list only implicitly in Krunker (it keeps it); keep parity
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

void ed_history_execute(const int redo) {
    EditorHistoryEntry *list = redo ? g_ed.redos : g_ed.undos;
    size_t *count = redo ? &g_ed.redo_count : &g_ed.undo_count;
    if (!*count) return;
    EditorHistoryEntry e = list[--(*count)];
    EditorObject *o = ed_obj_find(e.uuid);
    switch (e.action) {
        case 'a':
            if (!redo) {
                if (o) ed_obj_remove(o, 0, 0);
                history_push(&g_ed.redos, &g_ed.redo_count, &g_ed.redo_cap, e);
                return;
            }
            ed_obj_add(revive(&e), 0, 0);
            break;
        case 'r':
            if (!redo) {
                ed_obj_add(revive(&e), 0, 0);
                history_push(&g_ed.redos, &g_ed.redo_count, &g_ed.redo_cap, e);
                return;
            }
            if (o) ed_obj_remove(o, 0, 0);
            break;
        case 'u':
            if (o && e.state) {
                if (!redo) {
                    // the redo entry restores the state we are leaving
                    EditorHistoryEntry back = {'u', o->uuid, cJSON_Duplicate(o->data, 1)};
                    history_push(&g_ed.redos, &g_ed.redo_count, &g_ed.redo_cap, back);
                }
                const int before = ed_obj_count_class(o);
                cJSON_Delete(o->data);
                o->data = cJSON_Duplicate(e.state, 1);
                g_ed.obj_count[before]--;
                g_ed.obj_count[ed_obj_count_class(o)]++;
                ed_obj_rebuild(o);
                cJSON_Delete(o->history);
                o->history = cJSON_Duplicate(o->data, 1);
                g_ed.dirty_counts = 1;
                if (o == g_ed.selected) g_ed.dirty_panel = 1;
            }
            break;
    }
    cJSON_Delete(e.state);
    g_ed.dirty_storage = 1;
}

void ed_history_poll(void) {
    if (editor_setting_bool("disableHistory") || !editor_setting_num("historyLimit", 1000)) return;
    for (size_t i = 0; i < g_ed.count; i++) {
        EditorObject *o = g_ed.objects[i];
        if (o->history && !cJSON_Compare(o->history, o->data, 1)) {
            ed_history_add('u', o, o->history);
            o->history = NULL;
            g_ed.dirty_storage = 1;
        }
        if (!o->history) o->history = cJSON_Duplicate(o->data, 1);
    }
}

void ed_history_clear(void) {
    for (size_t i = 0; i < g_ed.undo_count; i++) cJSON_Delete(g_ed.undos[i].state);
    for (size_t i = 0; i < g_ed.redo_count; i++) cJSON_Delete(g_ed.redos[i].state);
    g_ed.undo_count = g_ed.redo_count = 0;
    for (size_t i = 0; i < g_graveyard_count; i++) ed_obj_free(g_graveyard[i].object);
    free(g_graveyard);
    g_graveyard = NULL;
    g_graveyard_count = 0;
}
