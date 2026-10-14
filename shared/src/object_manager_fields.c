// KrunkNative Object Manager: the Properties tool's fields (shared/include/object_manager.h
// "the Properties tool's fields"). Not a Krunker v7.1.4 feature: the game's own cut of the map
// editor's object inspector (doc/v7/decisions.md "Building tools"). Every row names the editor's
// ObjectInstance property and its map key (client/src/editor/editor_model.c K_PROPS, or the special
// props of editor_objects.c: pos / rot / size / texture / collidable / health), the inspector's label,
// slider range and gate (client/src/editor/editor_inspector.c), and the editor's load rule; writes
// follow ed_model_prop_set (a value equal to the default drops the key, switches write 1 or nothing).
//
// Intentional differences from the editor:
// - Only keys the game rebuilds an object from: no assets, sounds, scripts, AI / bot / environment
//   settings, text or faces; the first trigger action only (tas[0], the editor's plain action props).
// - "bm" (Bounce) reads as the runtime does (map.c, v7 map.js: bm == null ? 1 : parseFloat(bm || 0)):
//   K_PROPS holds it as an OR-default number, which would show 0 as 1.
// - A legacy trigger (no "tas": its action keys on the object) gets its action keys moved into tas[0]
//   by the first action edit; "tce" stays the object's Event Value (v7.1.4 also copies it into
//   tas[0].tce, the action's value type: map.c drops that, doc/v7/decisions.md "v7 bugs").
#include <object_manager.h>
#include <prefabs.h>
#include <triggers.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    ObjectManagerField f;
    int (*gate)(int prefab);
} OmFieldDef;

static int has(const int prefab, const PrefabFlag flag) { return prefab_has_flag(prefab, flag); }

static int g_all(const int p) { return p >= 0; }
static int g_rot(const int p) { return !has(p, PF_NOROTATION); }
static int g_size(const int p) { return has(p, PF_SCALABLE); }
static int g_dir(const int p) { return has(p, PF_CUSTOMDIRECTION); }
static int g_styled(const int p) { return !has(p, PF_NOTSTYLEABLE); } // (the inspector's style block returns for these)
static int g_tex(const int p) { return g_styled(p) && has(p, PF_TEXTURABLE) && !has(p, PF_SPRAYTEXTURE); }
static int g_color(const int p) { return g_styled(p) && has(p, PF_EDITCOLOR); }
static int g_emissive(const int p) { return g_styled(p) && has(p, PF_EDITEMISSIVE); }
static int g_opacity(const int p) { return g_styled(p) && has(p, PF_EDITOPAC); }
static int g_shading(const int p) { return g_styled(p) && !has(p, PF_COMPLEX) && !has(p, PF_TOOL); }
static int g_fog(const int p) { return g_styled(p) && has(p, PF_EDITFOG); }
static int g_visible(const int p) { return !has(p, PF_TOOL) || has(p, PF_BOUNCE); }
static int g_collidable(const int p) { return !has(p, PF_TOOL) && !has(p, PF_FORCECOLLISION) && !has(p, PF_NOCOLLISION); }
static int g_trigger(const int p) { return p == PREFAB_TRIGGER; }
static int g_bullet(const int p) { return has(p, PF_EDITBULLETCOL); }
static int g_pen(const int p) { return has(p, PF_EDITPEN); }
static int g_walljump(const int p) { return has(p, PF_CANWALLJUMP); }
static int g_grapple(const int p) { return has(p, PF_CANGRAPPLE); }
static int g_boost(const int p) { return has(p, PF_BOOSTABLE); }
static int g_bounce(const int p) { return has(p, PF_BOUNCE); }
static int g_border(const int p) { return has(p, PF_HASBORDER); }
static int g_teleporter(const int p) { return p == PREFAB_TELEPORTER; }
static int g_single(const int p) { return p == PREFAB_CHECK_POINT || p == PREFAB_TRIGGER || has(p, PF_AI); }
static int g_health(const int p) { return has(p, PF_HASHEALTH); }
static int g_iface(const int p) { return p != PREFAB_WEAPON_PICKUP && has(p, PF_CANINTERFACE); }
static int g_iface_target(const int p) { return has(p, PF_CANTARGETINTERFACE); }
static int g_light(const int p) {
    return has(p, PF_LIGHT) || p == PREFAB_DIRECTIONAL_LIGHT || p == PREFAB_RECTAREA_LIGHT || p == PREFAB_SPOT_LIGHT;
}
static int g_light_range(const int p) { return has(p, PF_LIGHT) || p == PREFAB_SPOT_LIGHT; }
static int g_spot(const int p) { return p == PREFAB_SPOT_LIGHT; }

#define F(key, prop, label, section, kind, mn, mx, st, def, opts, gate) {{key, prop, label, section, kind, mn, mx, st, def, opts}, gate}

// In the inspector's order within each section (editor_inspector.c line numbers of the v7 controls)
static const OmFieldDef k_fields[] = {
    // Transform (build_transform, :535-538): radians, as the map has them (the editor's default)
    F("p", "pos", "Position", "Transform", OM_FIELD_VEC3, -100000.0f, 100000.0f, 0.01f, 0.0f, NULL, g_all),
    F("r", "rot", "Rotation", "Transform", OM_FIELD_VEC3, -7.0f, 7.0f, 0.01f, 0.0f, NULL, g_rot),
    F("s", "size", "Size", "Transform", OM_FIELD_VEC3, OM_EDIT_SIZE_MIN, OM_EDIT_SIZE_MAX, 0.01f, 0.0f, NULL, g_size),
    F("d", "direction", "Direction", "Transform", OM_FIELD_INT, 0.0f, 3.0f, 1.0f, 0.0f, NULL, g_dir), // :470 (0..3 / 5 / 7)
    // Style (build_style, :552-654)
    F("t", "texture", "Texture", "Style", OM_FIELD_TEXTURE, 0.0f, PREFAB_TEXTURE_COUNT - 1, 1.0f, 0.0f, NULL, g_tex),
    F("c", "color", "Color", "Style", OM_FIELD_COLOR, 0.0f, 0.0f, 0.0f, 0.0f, "#FFFFFF", g_color),
    F("e", "emissive", "Emissive", "Style", OM_FIELD_COLOR, 0.0f, 0.0f, 0.0f, 0.0f, "#000000", g_emissive),
    F("o", "opacity", "Opacity", "Style", OM_FIELD_NUMN, 0.0f, 1.0f, 0.1f, 1.0f, NULL, g_opacity),
    F("ab", "ambient", "Shading", "Style", OM_FIELD_OFF, 0.0f, 1.0f, 1.0f, 0.0f, NULL, g_shading),
    F("nf", "fog", "Fog", "Style", OM_FIELD_OFF, 0.0f, 1.0f, 1.0f, 0.0f, NULL, g_fog),
    // Collision (build_header, :426-443)
    F("v", "visible", "Visible", "Collision", OM_FIELD_OFF, 0.0f, 1.0f, 1.0f, 0.0f, NULL, g_visible),
    F("l", "collidable", "Collidable", "Collision", OM_FIELD_OFF, 0.0f, 1.0f, 1.0f, 0.0f, NULL, g_collidable),
    F("pl", "playerCollidable", "Player Collidable", "Collision", OM_FIELD_OFF, 0.0f, 1.0f, 1.0f, 0.0f, NULL, g_trigger),
    F("ns", "bulletColl", "Bullet Collisions", "Collision", OM_FIELD_OFF, 0.0f, 1.0f, 1.0f, 0.0f, NULL, g_bullet),
    F("pe", "penetrable", "Penetrable", "Collision", OM_FIELD_NUM, 0.0f, 1.0f, 0.01f, 0.0f, NULL, g_pen),
    F("wj", "canWallJump", "Wall Jumpable", "Collision", OM_FIELD_OFF, 0.0f, 1.0f, 1.0f, 0.0f, NULL, g_walljump),
    F("gp", "canGrapple", "Can Grapple", "Collision", OM_FIELD_OFF, 0.0f, 1.0f, 1.0f, 0.0f, NULL, g_grapple),
    F("b", "boost", "Boost", "Collision", OM_FIELD_NUM, -100.0f, 100.0f, 0.01f, 0.0f, NULL, g_boost), // BOOST_RANGE
    F("bm", "bounce", "Bounce", "Collision", OM_FIELD_NUMN, -3.0f, 3.0f, 0.1f, 1.0f, NULL, g_bounce),
    F("cr", "crouch", "Crouch Reduces", "Collision", OM_FIELD_OFF, 0.0f, 1.0f, 1.0f, 0.0f, NULL, g_bounce),
    F("bo", "border", "Border", "Collision", OM_FIELD_ON, 0.0f, 1.0f, 1.0f, 0.0f, NULL, g_border),
    // Teleporter (build_technical, :905-911)
    F("n", "node", "Node", "Teleporter", OM_FIELD_SELECT, 0.0f, 2.0f, 1.0f, 0.0f, "Sending|Receiving|Both", g_teleporter),
    F("ch", "channel", "Channel", "Teleporter", OM_FIELD_INT, 0.0f, 100.0f, 1.0f, 0.0f, NULL, g_teleporter),
    F("m", "momentum", "Stop Momentum", "Teleporter", OM_FIELD_ON, 0.0f, 1.0f, 1.0f, 0.0f, NULL, g_teleporter),
    F("tcp", "telCheckPoint", "Use Active Checkpoint", "Teleporter", OM_FIELD_ON, 0.0f, 1.0f, 1.0f, 0.0f, NULL, g_teleporter),
    // Logic (build_technical, :831-853)
    F("su", "single", "Single Use", "Logic", OM_FIELD_ON, 0.0f, 1.0f, 1.0f, 0.0f, NULL, g_single),
    F("sup", "singleP", "Once Per Player", "Logic", OM_FIELD_ON, 0.0f, 1.0f, 1.0f, 0.0f, NULL, g_trigger),
    F("cdy", "canDest", "Destructible", "Logic", OM_FIELD_ON, 0.0f, 1.0f, 1.0f, 0.0f, NULL, g_health),
    F("h", "health", "Health", "Logic", OM_FIELD_INT, 0.0f, 50000.0f, 10.0f, 0.0f, NULL, g_health),
    F("in", "interfaceId", "Interface ID", "Logic", OM_FIELD_INT, 0.0f, TRIGGER_INTERFACE_MAX, 1.0f, 0.0f, NULL, g_iface),
    F("tin", "interfaceIdT", "Interface Target", "Logic", OM_FIELD_INT, 0.0f, TRIGGER_INTERFACE_MAX, 1.0f, 0.0f, NULL, g_iface_target),
    // Trigger (ed_triggers_panel: the event and the first action, :1194-1216)
    F("te", "event", "Event", "Trigger", OM_FIELD_SELECT, 0.0f, TRIGGER_EVENT_COUNT - 1, 1.0f, 0.0f, "@events", g_trigger),
    F("tce", "constantEvent", "Event Value", "Trigger", OM_FIELD_INT, 0.0f, TRIGGER_CONSTANT_EVENT_MAX, 1.0f, 0.0f, NULL, g_trigger),
    F("cha", "chance", "Action Chance (%)", "Trigger", OM_FIELD_INT, 0.0f, 100.0f, 1.0f, 0.0f, NULL, g_trigger),
    F("tas.0.ta", "action", "Action", "Trigger", OM_FIELD_SELECT, 0.0f, TRIGGER_V10_ACTION_COUNT - 1, 1.0f, 0.0f, "@actions", g_trigger),
    F("tas.0.tc", "constant", "Action Value", "Trigger", OM_FIELD_INT, TRIGGER_CONSTANT_MIN, TRIGGER_CONSTANT_MAX, 1.0f, 0.0f, NULL, g_trigger),
    F("tas.0.tad", "delay", "Action Delay (s)", "Trigger", OM_FIELD_NUM, 0.0f, TRIGGER_V7_DELAY_MAX, 0.1f, 0.0f, NULL, g_trigger),
    // Lighting (build_type_folders, :1027-1037)
    F("li", "intensity", "Intensity", "Lighting", OM_FIELD_NUM, 0.0f, 15.0f, 0.1f, 1.0f, NULL, g_light),
    F("lds", "distance", "Distance", "Lighting", OM_FIELD_NUMN, 0.0f, 1000.0f, 0.1f, 50.0f, NULL, g_light_range),
    F("ld", "decay", "Decay", "Lighting", OM_FIELD_NUM, 0.0f, 2.0f, 0.1f, 1.0f, NULL, g_light_range),
    F("la", "angle", "Angle", "Lighting", OM_FIELD_INT, 1.0f, 90.0f, 1.0f, 45.0f, NULL, g_spot),
    F("lp", "penumbra", "Penumbra", "Lighting", OM_FIELD_NUM, 0.0f, 1.0f, 0.1f, 0.0f, NULL, g_spot),
};
#undef F

#define FIELD_COUNT ((int) (sizeof(k_fields) / sizeof(k_fields[0])))

// v7 map.js's legacy single-action keys (map.c map_parse_trigger_action reads them from tas[0], or
// from the object without a "tas"); "tce" is the object's Event Value there, never moved
static const char *const k_action_keys[] = {"ta", "tc", "tc2", "tct", "tcc", "tad", "cd", "cv0", "cv1", "cv2", "es", "esa", "ev", "er", "ef"};

int object_manager_field_count(void) { return FIELD_COUNT; }

const ObjectManagerField *object_manager_field(const int index) {
    return index >= 0 && index < FIELD_COUNT ? &k_fields[index].f : NULL;
}

int object_manager_field_find(const char *key) {
    for (int i = 0; key && i < FIELD_COUNT; i++) {
        if (!strcmp(k_fields[i].f.key, key)) return i;
    }
    return -1;
}

static const OmFieldDef *def_of(const ObjectManagerField *f) {
    return f >= &k_fields[0].f && f <= &k_fields[FIELD_COUNT - 1].f ? (const OmFieldDef *) f : NULL;
}

int object_manager_field_applies(const ObjectManagerField *f, const int prefab) {
    const OmFieldDef *d = def_of(f);
    return d && prefab >= 0 && prefab < PREFAB_COUNT && d->gate(prefab);
}

static int option_count(const ObjectManagerField *f) {
    if (!f->options || f->kind != OM_FIELD_SELECT) return 0;
    if (!strcmp(f->options, "@events")) return TRIGGER_EVENT_COUNT;
    if (!strcmp(f->options, "@actions")) return TRIGGER_V10_ACTION_COUNT;
    int n = 1;
    for (const char *c = f->options; *c; c++) n += *c == '|';
    return n;
}

const char *object_manager_field_option(const ObjectManagerField *f, const int index) {
    if (!f || index < 0 || index >= option_count(f)) return NULL;
    if (!strcmp(f->options, "@events")) return g_trigger_events[index];
    if (!strcmp(f->options, "@actions")) return g_trigger_actions[index].name;
    static char label[48];
    const char *c = f->options;
    for (int i = 0; i < index; i++) c = strchr(c, '|') + 1;
    const char *end = strchr(c, '|');
    const size_t n = end ? (size_t) (end - c) : strlen(c);
    snprintf(label, sizeof(label), "%.*s", (int) n, c);
    return label;
}

void object_manager_field_range(const ObjectManagerField *f, const int prefab, float *min, float *max) {
    *min = f->min;
    *max = f->max;
    if (!strcmp(f->key, "d")) *max = has(prefab, PF_EXTRADIRECTION) ? 7.0f : has(prefab, PF_ALTDIRECTION) ? 5.0f : 3.0f; // :470
    if (f->kind == OM_FIELD_SELECT) *max = (float) (option_count(f) - 1);
}

// ---------------------------------------------------------------- reads (the editor's load rules)

static int js_nullish(const cJSON *v) { return !v || cJSON_IsNull(v); }

static double js_number(const cJSON *v) {
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? 1.0 : 0.0;
    if (cJSON_IsString(v)) {
        char *end = NULL;
        const double d = strtod(v->valuestring, &end);
        return end != v->valuestring ? d : NAN;
    }
    return NAN;
}

static int js_truthy(const cJSON *v) {
    if (js_nullish(v) || cJSON_IsFalse(v)) return 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0.0 && !isnan(v->valuedouble);
    if (cJSON_IsString(v)) return v->valuestring[0] != 0;
    return 1;
}

// "tas.0.ta" -> "ta" (action 0's), else the key itself
static const char *sub_key(const char *key, int *action) {
    *action = !strncmp(key, "tas.0.", 6);
    return *action ? key + 6 : key;
}

// where a key lives: tas[0] when the record has a non-empty "tas", else the record (map.c's rule)
static const cJSON *read_holder(const cJSON *record, const int action) {
    if (!action) return record;
    const cJSON *tas = cJSON_GetObjectItemCaseSensitive(record, "tas");
    if (cJSON_IsArray(tas) && cJSON_GetArraySize(tas) > 0) {
        const cJSON *a = cJSON_GetArrayItem(tas, 0);
        return cJSON_IsObject(a) ? a : NULL;
    }
    return record;
}

static cJSON *vec_value(const cJSON *v, const double def[3]) {
    double out[3] = {def[0], def[1], def[2]};
    for (int k = 0; cJSON_IsArray(v) && cJSON_GetArraySize(v) >= 3 && k < 3; k++) {
        const double d = js_number(cJSON_GetArrayItem(v, k));
        if (isfinite(d)) out[k] = d;
    }
    return cJSON_CreateDoubleArray(out, 3);
}

cJSON *object_manager_field_value(const cJSON *record, const int prefab, const ObjectManagerField *f) {
    if (!record || !object_manager_field_applies(f, prefab)) return NULL;
    int action;
    const char *key = sub_key(f->key, &action);
    const cJSON *h = read_holder(record, action);
    const cJSON *v = h ? cJSON_GetObjectItemCaseSensitive(h, key) : NULL;
    const double def = f->def;
    switch (f->kind) {
        case OM_FIELD_ON: return cJSON_CreateBool(!js_nullish(v));
        case OM_FIELD_OFF: {
            if (!strcmp(f->key, "l")) return cJSON_CreateBool(js_nullish(v) && js_nullish(cJSON_GetObjectItemCaseSensitive(h, "col")));
            return cJSON_CreateBool(js_nullish(v));
        }
        case OM_FIELD_COLOR: {
            char buf[16];
            if (cJSON_IsNumber(v)) {
                snprintf(buf, sizeof(buf), "#%06x", (unsigned) v->valuedouble & 0xffffffu);
                return cJSON_CreateString(buf);
            }
            return cJSON_CreateString(cJSON_IsString(v) ? v->valuestring : f->options);
        }
        case OM_FIELD_VEC3: {
            const PrefabData *pd = prefab_data(prefab);
            const double zero[3] = {0.0, 0.0, 0.0};
            const double size[3] = {pd ? pd->default_size[0] : 10.0, pd ? pd->default_size[1] : 10.0, pd ? pd->default_size[2] : 10.0};
            return vec_value(v, !strcmp(f->key, "s") ? size : zero);
        }
        case OM_FIELD_TEXTURE: {
            const double t = js_number(v);
            return cJSON_CreateNumber(prefab_texture_resolve(prefab, !js_nullish(v), isfinite(t) ? (int) t : 0));
        }
        case OM_FIELD_NUMN: {
            const double d = js_number(v);
            return cJSON_CreateNumber(js_nullish(v) ? def : js_truthy(v) && !isnan(d) ? d : 0.0);
        }
        case OM_FIELD_INT:
        case OM_FIELD_SELECT: {
            if (!strcmp(f->key, "h") && js_nullish(v)) v = cJSON_GetObjectItemCaseSensitive(h, "hp"); // (editor_objects.c: h, else hp)
            const double d = js_number(v);
            return cJSON_CreateNumber(js_truthy(v) && isfinite(d) ? trunc(d) : def);
        }
        default: {
            const double d = js_number(v);
            return cJSON_CreateNumber(js_truthy(v) && isfinite(d) ? d : def);
        }
    }
}

// ---------------------------------------------------------------- writes (ed_model_prop_set's rules)

static void put(cJSON *h, const char *key, cJSON *value) {
    cJSON_DeleteItemFromObjectCaseSensitive(h, key);
    if (value) cJSON_AddItemToObject(h, key, value);
}

static double round_to(const double v, const double scale) { return round(v * scale) / scale; }

// tas[0] for writing: a legacy trigger's action keys move into a new tas[0]
static cJSON *write_holder(cJSON *record) {
    cJSON *tas = cJSON_GetObjectItemCaseSensitive(record, "tas");
    if (!cJSON_IsArray(tas) || !cJSON_GetArraySize(tas)) {
        cJSON *first = cJSON_CreateObject();
        if (!first) return NULL;
        for (size_t i = 0; i < sizeof(k_action_keys) / sizeof(k_action_keys[0]); i++) {
            cJSON *v = cJSON_DetachItemFromObjectCaseSensitive(record, k_action_keys[i]);
            if (v) cJSON_AddItemToObject(first, k_action_keys[i], v);
        }
        put(record, "tas", cJSON_CreateArray());
        tas = cJSON_GetObjectItemCaseSensitive(record, "tas");
        if (!tas) {
            cJSON_Delete(first);
            return NULL;
        }
        cJSON_AddItemToArray(tas, first);
        return first;
    }
    cJSON *a = cJSON_GetArrayItem(tas, 0);
    if (!cJSON_IsObject(a)) {
        a = cJSON_CreateObject();
        if (a) cJSON_ReplaceItemInArray(tas, 0, a);
    }
    return a;
}

static int hex_colour(const char *s) {
    if (!s || s[0] != '#' || strlen(s) != 7) return 0;
    for (int i = 1; i < 7; i++) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
    }
    return 1;
}

static int is_switch_value(const cJSON *v) {
    return cJSON_IsBool(v) || (cJSON_IsNumber(v) && (v->valuedouble == 0.0 || v->valuedouble == 1.0));
}

// One field's new value into the record (`size_had`: the record's size before the patch, for "s")
static int patch_one(cJSON *record, const int prefab, const ObjectManagerField *f, const cJSON *value, const double size_had[3]) {
    float mn, mx;
    object_manager_field_range(f, prefab, &mn, &mx);
    int action;
    const char *key = sub_key(f->key, &action);
    cJSON *h = action ? write_holder(record) : record;
    if (!h) return OM_STATUS_INVALID;
    switch (f->kind) {
        case OM_FIELD_ON:
        case OM_FIELD_OFF: {
            if (!is_switch_value(value)) return OM_STATUS_INVALID;
            const int on = cJSON_IsBool(value) ? cJSON_IsTrue(value) : value->valuedouble != 0.0;
            if (!strcmp(key, "l")) put(h, "col", NULL); // (editor_objects.c collidable: "col" is v3's "l")
            put(h, key, (f->kind == OM_FIELD_ON) == on ? cJSON_CreateNumber(1) : NULL);
            return OM_STATUS_OK;
        }
        case OM_FIELD_COLOR: {
            if (!cJSON_IsString(value) || !hex_colour(value->valuestring)) return OM_STATUS_INVALID;
            char lower[8];
            for (int i = 0; i < 8; i++) lower[i] = (char) (value->valuestring[i] >= 'A' && value->valuestring[i] <= 'F' ? value->valuestring[i] + 32 : value->valuestring[i]);
            if (!strcmp(key, "c")) put(h, "ci", NULL); // the map's palette index would win over "c"
            put(h, key, cJSON_CreateString(lower));
            return OM_STATUS_OK;
        }
        case OM_FIELD_VEC3: {
            if (!cJSON_IsArray(value) || cJSON_GetArraySize(value) != 3) return OM_STATUS_INVALID;
            double v[3];
            const int is_size = !strcmp(key, "s");
            for (int k = 0; k < 3; k++) {
                const cJSON *item = cJSON_GetArrayItem(value, k);
                if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) return OM_STATUS_INVALID;
                v[k] = round_to(item->valuedouble, 100.0); // map.js rounds p / s / r to 2 decimals
                // a size past the limits may stay as it was (a big map object), never grow
                const double lo = is_size ? fmin(mn, size_had[k]) : mn, hi = is_size ? fmax(mx, size_had[k]) : mx;
                if (v[k] < lo - 0.001 || v[k] > hi + 0.001) return OM_STATUS_BOUNDS;
            }
            put(h, key, cJSON_CreateDoubleArray(v, 3));
            return OM_STATUS_OK;
        }
        case OM_FIELD_TEXTURE: {
            if (!cJSON_IsNumber(value) || value->valuedouble != trunc(value->valuedouble)) return OM_STATUS_INVALID;
            if (value->valuedouble < mn || value->valuedouble > mx) return OM_STATUS_BOUNDS;
            put(h, "at", NULL); // the texture select resets the asset texture and the variant (editor_objects.c)
            put(h, "tv", NULL);
            put(h, "t", cJSON_CreateNumber(value->valuedouble));
            return OM_STATUS_OK;
        }
        default: {
            if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble)) return OM_STATUS_INVALID;
            const int integer = f->kind == OM_FIELD_INT || f->kind == OM_FIELD_SELECT;
            double v = integer ? value->valuedouble : round_to(value->valuedouble, 1000.0);
            if (integer && v != trunc(v)) return OM_STATUS_INVALID;
            if (v < mn - 1e-6 || v > mx + 1e-6) return OM_STATUS_BOUNDS;
            if (!strcmp(key, "h")) put(h, "hp", NULL);
            // a value that loads back as itself: the default drops the key (ed_model_prop_set)
            put(h, key, v != f->def ? cJSON_CreateNumber(v) : NULL);
            return OM_STATUS_OK;
        }
    }
}

int object_manager_fields_patch(cJSON *record, const int prefab, const cJSON *patch, int *pose) {
    if (pose) *pose = 0;
    if (!cJSON_IsObject(record) || !cJSON_IsObject(patch) || !patch->child) return OM_STATUS_INVALID;
    double size_had[3] = {0.0, 0.0, 0.0};
    const PrefabData *pd = prefab_data(prefab);
    for (int k = 0; k < 3; k++) size_had[k] = pd ? pd->default_size[k] : 0.0;
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(record, "s");
    for (int k = 0; cJSON_IsArray(s) && cJSON_GetArraySize(s) >= 3 && k < 3; k++) {
        const double d = js_number(cJSON_GetArrayItem(s, k));
        if (isfinite(d)) size_had[k] = d;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, patch) {
        const ObjectManagerField *f = object_manager_field(object_manager_field_find(item->string));
        if (!f) return OM_STATUS_INVALID;
        if (!object_manager_field_applies(f, prefab)) return OM_STATUS_UNSUPPORTED;
        const int status = patch_one(record, prefab, f, item, size_had);
        if (status != OM_STATUS_OK) return status;
        if (pose && (!strcmp(f->key, "p") || !strcmp(f->key, "s") || !strcmp(f->key, "r") || !strcmp(f->key, "d"))) *pose = 1;
    }
    return OM_STATUS_OK;
}
