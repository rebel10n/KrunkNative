// The viewer's dev config editors (viewer.js f453 / f454, only with ?dev): a "<Slot> Config"
// folder under each Player slot dropdown and a "<Weapon> Config" folder under the Primary /
// Secondary skin and Melee dropdowns. v7 binds the controllers straight to the picked
// Game.store.skins entry (vA353[slot]) and mutates it: here every edited entry is a record of
// its JSON values (assets/skins.json, the same store.skins) plus a writable ViewerSkin /
// ViewerDye copy (viewer_skin_edit) that the player mesh reads, so the edits show on the next
// Z() and persist for the page's lifetime like v7's in-place edits. Export Config copies the
// entry's JSON as v7 writes it (store-only keys, defaults and template values stripped,
// JSON.stringify(.., null, 4) with unquoted keys).
#include "viewer_internal.h"
#include <cosmetics.h>
#include <html_dom.h>
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    F_SIT_OFF, F_SIT_OFF_Z, F_SCL_MLT, F_TRANS, F_ALPHA, F_FRAMES, F_FRAME_T, F_SIDE, F_GLOW, F_NO_EMIS,
    F_SAME_GLOW, F_PULS_T, F_NO_HEAD, F_NO_BODY, F_NO_LEGS, F_RANDOM_LEG, F_LEG_LEN, F_COUNT
};

// f453's controllers in order: key, name, default (false or 0), min / max / step, the only
// store type that gets it (0 = all)
typedef struct {
    const char *key, *label;
    int is_bool;
    double min, max, step;
    int only_type;
} Field;

static const Field k_fields[F_COUNT] = {
    [F_SIT_OFF] = {"sitOff", "sitOff", 0, NAN, NAN, 0.0001, 0},
    [F_SIT_OFF_Z] = {"sitOffZ", "sitOffZ", 0, NAN, NAN, 0.0001, 0},
    [F_SCL_MLT] = {"sclMlt", "sclMlt", 0, NAN, NAN, 0.0001, 0},
    [F_TRANS] = {"trans", "Transparency", 1, NAN, NAN, 0.0, 0},
    [F_ALPHA] = {"alpha", "AlphaTest", 0, 0.0, 1.0, 0.01, 0},
    [F_FRAMES] = {"frames", "Frames", 0, 0.0, 10.0, 1.0, 0},
    [F_FRAME_T] = {"frameT", "Frame Time", 0, 0.0, 1000.0, 50.0, 0},
    [F_SIDE] = {"side", "Side", 0, NAN, NAN, 0.0, 0},
    [F_GLOW] = {"glow", "Glow", 1, NAN, NAN, 0.0, 0},
    [F_NO_EMIS] = {"noEmis", "noEmis", 1, NAN, NAN, 0.0, 0},
    [F_SAME_GLOW] = {"sameGlow", "sameGlow", 1, NAN, NAN, 0.0, 0},
    [F_PULS_T] = {"pulsT", "Pulse", 0, 0.0, 0.0015, 0.0001, 0},
    [F_NO_HEAD] = {"noHead", "noHead", 1, NAN, NAN, 0.0, ITEM_HEAD},
    [F_NO_BODY] = {"noBody", "noBody", 1, NAN, NAN, 0.0, ITEM_BODY},
    [F_NO_LEGS] = {"noLegs", "noLegs", 1, NAN, NAN, 0.0, ITEM_WAIST},
    [F_RANDOM_LEG] = {"randomLeg", "randomLeg", 1, NAN, NAN, 0.0, ITEM_SHOE},
    [F_LEG_LEN] = {"legLen", "legLen", 0, 0.0, 1.0, 0.01, ITEM_SHOE},
};
// f454 (weapon skins): Transparency, AlphaTest, Side, Glow, Pulse
static const int k_weapon_fields[] = {F_TRANS, F_ALPHA, F_SIDE, F_GLOW, F_PULS_T};

// The Dye Config colours in f453 order, with the class colour each defaults to
// (Game.classes[i].colors: skin, shirt, pants, shoes, hair, sleeve)
enum { D_HAIR, D_SKIN, D_SHIRT, D_SLEEVE, D_WAIST, D_PANTS, D_SHOE, D_COUNT };
static const struct {
    const char *key, *label;
    int part;
} k_dye[D_COUNT] = {
    [D_HAIR] = {"hairCol", "Hair", VIEWER_DYE_HAIR},
    [D_SKIN] = {"skinCol", "Skin", VIEWER_DYE_SKIN},
    [D_SHIRT] = {"shirtCol", "Shirt", VIEWER_DYE_SHIRT},
    [D_SLEEVE] = {"sleeveCol", "Sleeves", VIEWER_DYE_SLEEVE},
    [D_WAIST] = {"waistCol", "Waist", VIEWER_DYE_WAIST},
    [D_PANTS] = {"pantsCol", "Pants", VIEWER_DYE_PANTS},
    [D_SHOE] = {"shoeCol", "Shoe", VIEWER_DYE_SHOE},
};

// One property of an edited skin: its JS value (dat.gui picks the controller by its type)
typedef struct {
    int set;     // != null on the object (from the JSON, or f453's default)
    int in_json; // the JSON entry has it
    int dirty;   // a controller changed it (Export writes the edited value, else the JSON's)
    int is_bool;
    int b;
    double num;
} CfgValue;

typedef struct {
    int index;
    CfgValue f[F_COUNT];
    VgValue side;            // the Side select (a string once picked; f453 parseInts it back)
    CfgValue col[D_COUNT];   // dye colours (num = 0xRRGGBB)
    int col_format[D_COUNT]; // VG_COLOR_*: a number, a "#rrggbb" or an "rgb()" string
    int col_format0[D_COUNT]; // ... as the JSON has it
    int hex[D_COUNT];        // what the colour controllers bind (synced into col on change)
} Record;

#define CFG_MAX_RECORDS 32
#define CFG_MAX_SLOT_CTRLS 24

// A config folder: f453(type) for a Player slot, f454(1..3) for the weapons
typedef struct {
    int type;           // store type, or -1 / -2 / -3: primary / secondary / melee (vA353[-n])
    const char *folder_name;
    VgValue *dropdown;  // the slot's dropdown property
    int target;         // vA353[type]: the edited store index, -1 = none (folder hidden)
    int seen;           // the dropdown's value at the last snapshot
    VgFolder *parent, *folder;
    VgCtrl *ctrls[CFG_MAX_SLOT_CTRLS];
    int ctrl_count;
} Slot;

static struct {
    Record records[CFG_MAX_RECORDS];
    int record_count;
    Slot slots[11];
    int slot_count;
    cJSON *skins;       // assets/skins.json, loaded on the first edit
    int skins_loaded;
} g_cfg;

// ---------------------------------------------------------------- store JSON

static const cJSON *skin_json(const int index) {
    if (!g_cfg.skins_loaded) {
        g_cfg.skins_loaded = 1;
        char *path = concat(assets_path(), "skins.json");
        unsigned char *buf = NULL;
        size_t len = 0;
        if (path && ensure_asset_file(path) && !read_file(path, &buf, &len)) g_cfg.skins = cJSON_ParseWithLength((const char *) buf, len);
        free(buf);
        free(path);
        if (!g_cfg.skins) fprintf(stderr, "[viewer] config: assets/skins.json unreadable\n");
    }
    return cJSON_GetArrayItem(g_cfg.skins, index);
}

static void value_from_json(CfgValue *v, const cJSON *item) {
    if (!item || cJSON_IsNull(item)) return;
    v->set = v->in_json = 1;
    if (cJSON_IsBool(item)) {
        v->is_bool = 1;
        v->b = cJSON_IsTrue(item);
    } else if (cJSON_IsNumber(item)) {
        v->num = item->valuedouble;
    } else if (cJSON_IsString(item)) {
        v->num = strtod(item->valuestring, NULL);
    } else if (cJSON_IsArray(item) && cJSON_IsNumber(cJSON_GetArrayItem(item, 0))) {
        v->num = cJSON_GetArrayItem(item, 0)->valuedouble; // a list of dye colours: the first
    }
}

static Record *record_for(const int index) {
    for (int i = 0; i < g_cfg.record_count; i++) {
        if (g_cfg.records[i].index == index) return &g_cfg.records[i];
    }
    const cJSON *json = skin_json(index);
    if (!json || g_cfg.record_count >= CFG_MAX_RECORDS) return NULL;

    Record *rec = &g_cfg.records[g_cfg.record_count++];
    memset(rec, 0, sizeof(*rec));
    rec->index = index;
    for (int i = 0; i < F_COUNT; i++) value_from_json(&rec->f[i], cJSON_GetObjectItemCaseSensitive(json, k_fields[i].key));
    for (int i = 0; i < D_COUNT; i++) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, k_dye[i].key);
        value_from_json(&rec->col[i], item);
        if (cJSON_IsString(item) && item->valuestring[0] == '#') {
            rec->col_format[i] = rec->col_format0[i] = VG_COLOR_CSS_HEX;
            rec->col[i].num = (double) strtol(item->valuestring + 1, NULL, 16);
        }
    }
    return rec;
}

static double cfg_num(const CfgValue *v) {
    return v->is_bool ? v->b : v->num;
}

static int cfg_truthy(const CfgValue *v) {
    return v->is_bool ? v->b : v->num != 0.0;
}

// ---------------------------------------------------------------- edits -> the mesh

static void set_flag(unsigned *flags, const unsigned bit, const CfgValue *v) {
    if (!v->set) return;
    if (cfg_truthy(v)) *flags |= bit;
    else *flags &= ~bit;
}

static void apply_record(const Record *rec) {
    ViewerSkin *s = viewer_skin_edit(rec->index);
    if (!s) return;
    if (s->type == ITEM_DYE) {
        ViewerDye *dye = viewer_dye_edit(rec->index);
        for (int i = 0; dye && i < D_COUNT; i++) {
            const CfgValue *v = &rec->col[i];
            if (!v->set || (v->in_json && !v->dirty)) continue; // a JSON list keeps its random pick
            dye->parts[k_dye[i].part] = (ViewerDyeColor) {1, {(int) cfg_num(v) & 0xFFFFFF, -1, -1, -1}};
        }
        return;
    }
    const CfgValue *f = rec->f;
    if (f[F_SIT_OFF].set) s->sit_off = (float) cfg_num(&f[F_SIT_OFF]);
    if (f[F_SIT_OFF_Z].set) s->sit_off_z = (float) cfg_num(&f[F_SIT_OFF_Z]);
    if (f[F_SCL_MLT].set) s->scl_mlt = (float) cfg_num(&f[F_SCL_MLT]);
    if (f[F_ALPHA].set) s->alpha = (float) cfg_num(&f[F_ALPHA]);
    if (f[F_FRAMES].set) s->frames = (int) cfg_num(&f[F_FRAMES]);
    if (f[F_FRAME_T].set) s->frame_t = (int) cfg_num(&f[F_FRAME_T]);
    if (f[F_SIDE].set) s->side = (int) cfg_num(&f[F_SIDE]);
    if (f[F_PULS_T].set) s->puls_t = (float) cfg_num(&f[F_PULS_T]);
    if (f[F_LEG_LEN].set) s->leg_len = (float) cfg_num(&f[F_LEG_LEN]);
    set_flag(&s->flags, VS_TRANS, &f[F_TRANS]);
    set_flag(&s->flags, VS_GLOW, &f[F_GLOW]);
    set_flag(&s->flags, VS_NO_EMIS, &f[F_NO_EMIS]);
    set_flag(&s->flags, VS_SAME_GLOW, &f[F_SAME_GLOW]);
    set_flag(&s->flags, VS_NO_HEAD, &f[F_NO_HEAD]);
    set_flag(&s->flags, VS_NO_LEGS, &f[F_NO_LEGS]);
    set_flag(&s->flags2, VS2_NO_BODY, &f[F_NO_BODY]);
    set_flag(&s->flags2, VS2_RANDOM_LEG, &f[F_RANDOM_LEG]);
}

// Every controller's onChange: mark what changed, push the records into the edit layer, f458()
static void on_edit(void) {
    for (int i = 0; i < g_cfg.record_count; i++) {
        Record *rec = &g_cfg.records[i];
        // the Side select writes the option's string: number it (f453's parseInt; f454 keeps
        // the string, which reads the same)
        const double side = rec->side.is_string ? strtod(rec->side.str, NULL) : rec->side.num;
        const ViewerSkin *skin = viewer_skin(rec->index);
        if (skin && skin->type != ITEM_WEAPON && skin->type != ITEM_MELEE) rec->side.is_string = 0;
        if (rec->f[F_SIDE].set && side != rec->f[F_SIDE].num) {
            rec->f[F_SIDE].num = side;
            rec->f[F_SIDE].dirty = 1;
        }
        apply_record(rec);
    }
    vscene_rebuild_player();
}

// A controller's onChange does not say which property it wrote: every bound value keeps its
// last seen state here, and the ones that differ are the edited (dirty) ones.
typedef struct {
    CfgValue *value;
    double num;
    int b;
} Watch;
#define CFG_MAX_WATCH 256
static Watch g_watch[CFG_MAX_WATCH];
static int g_watch_count;

static void on_watched_edit(void) {
    for (int i = 0; i < g_watch_count; i++) {
        Watch *w = &g_watch[i];
        if (!w->value) continue;
        if (w->value->num != w->num || w->value->b != w->b) {
            w->value->dirty = 1;
            w->num = w->value->num;
            w->b = w->value->b;
        }
    }
    on_edit();
}

static void watch(CfgValue *v) {
    for (int i = 0; i < g_watch_count; i++) {
        if (g_watch[i].value == v) {
            g_watch[i].num = v->num;
            g_watch[i].b = v->b;
            return;
        }
    }
    if (g_watch_count < CFG_MAX_WATCH) g_watch[g_watch_count++] = (Watch) {v, v->num, v->b};
}

// ---------------------------------------------------------------- Export Config

static const char *const k_export_strip[] = {
    "export", "thumbnail", "index", "noSale", "noSpin", "noQuickSell", "limT", "bundle", "itemShop", "mapReward",
    "package", "drop", "challenge", "craftable", "opensea", "ranked", "limited", "streamer", "streamers", "creator",
    "creators", "assetType", "local", "localM", "tex",
};
static const char *const k_export_zero[] = {"side", "alpha", "frameT", "frames", "pulsT", "sitOffZ", "sitOff", "sclMlt"}; // `x == 0`
static const char *const k_export_falsy[] = {"trans", "glow", "noEmis", "sameGlow", "noHead", "noBody", "noLegs", "randomLeg", "legLen"}; // `!x`

// store.js templates (Game.store.templates)
static const char k_templates[] =
    "[{\"mid\":0,\"scl\":0.9,\"mScl\":1,\"zOff\":0.28,\"yOff\":-0.5},"
    "{\"mid\":0,\"noMuz\":true,\"spnScl\":1.3,\"scl\":0.9,\"mScl\":0.84,\"zOff\":0,\"yOff\":0},"
    "{\"flipAnim\":true,\"id\":3,\"type\":3,\"scl\":2.6,\"sclMlt\":1,\"yOff\":-1,\"xOff\":-1.5,\"sxRot\":0.3,\"sxOff\":0.35,\"szOff\":-0.25},"
    "{\"id\":0,\"type\":7,\"sitOff\":1,\"sitOffZ\":0.005,\"trans\":true,\"yOff\":-0.7,\"xOff\":1.6,\"xRot\":-0.5}]";

static int loose_equals_zero(const cJSON *item) {
    if (cJSON_IsNumber(item)) return item->valuedouble == 0.0;
    if (cJSON_IsFalse(item)) return 1;
    if (cJSON_IsString(item)) {
        const char *s = item->valuestring;
        while (*s == ' ') s++;
        if (!*s) return 1;
        char *end = NULL;
        const double v = strtod(s, &end);
        while (end && *end == ' ') end++;
        return end && !*end && v == 0.0;
    }
    return 0;
}

static int js_falsy(const cJSON *item) {
    if (!item || cJSON_IsNull(item) || cJSON_IsFalse(item)) return 1;
    if (cJSON_IsNumber(item)) return item->valuedouble == 0.0 || isnan(item->valuedouble);
    if (cJSON_IsString(item)) return !item->valuestring[0];
    return 0;
}

typedef struct {
    char *buf;
    size_t len, cap;
} Out;

static void out_put(Out *o, const char *s, const size_t n) {
    if (o->len + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 1024;
        while (cap < o->len + n + 1) cap *= 2;
        char *grown = realloc(o->buf, cap);
        if (!grown) return;
        o->buf = grown;
        o->cap = cap;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = 0;
}

static void out_str(Out *o, const char *s) {
    out_put(o, s, strlen(s));
}

static void out_indent(Out *o, const int depth) {
    for (int i = 0; i < depth * 4; i++) out_put(o, " ", 1);
}

// JSON.stringify's quoting
static void out_quoted(Out *o, const char *s, const int quotes) {
    if (quotes) out_put(o, "\"", 1);
    for (const unsigned char *p = (const unsigned char *) s; *p; p++) {
        char esc[8];
        switch (*p) {
            case '"': out_str(o, "\\\""); break;
            case '\\': out_str(o, "\\\\"); break;
            case '\b': out_str(o, "\\b"); break;
            case '\f': out_str(o, "\\f"); break;
            case '\n': out_str(o, "\\n"); break;
            case '\r': out_str(o, "\\r"); break;
            case '\t': out_str(o, "\\t"); break;
            default:
                if (*p < 0x20) {
                    snprintf(esc, sizeof(esc), "\\u%04x", *p);
                    out_str(o, esc);
                } else {
                    out_put(o, (const char *) p, 1);
                }
        }
    }
    if (quotes) out_put(o, "\"", 1);
}

// A key line `"key":` loses its quotes (viewer.js's /^[\t ]*"[^:\n\r]+(?<!\\)":/gm replace
// drops every `"` of the match)
static void out_key(Out *o, const char *key) {
    Out escaped = {0};
    out_quoted(&escaped, key, 0);
    const char *e = escaped.buf ? escaped.buf : "";
    const size_t n = strlen(e);
    if (n && !strpbrk(e, ":\n\r") && e[n - 1] != '\\') {
        for (const char *p = e; *p; p++) {
            if (*p != '"') out_put(o, p, 1);
        }
    } else {
        out_put(o, "\"", 1);
        out_str(o, e);
        out_put(o, "\"", 1);
    }
    out_str(o, ": ");
    free(escaped.buf);
}

// JSON.stringify(value, null, 4)
static void out_value(Out *o, const cJSON *item, const int depth) {
    if (cJSON_IsObject(item) || cJSON_IsArray(item)) {
        const int object = cJSON_IsObject(item);
        if (!item->child) {
            out_str(o, object ? "{}" : "[]");
            return;
        }
        out_str(o, object ? "{\n" : "[\n");
        for (const cJSON *child = item->child; child; child = child->next) {
            out_indent(o, depth + 1);
            if (object) out_key(o, child->string ? child->string : "");
            out_value(o, child, depth + 1);
            if (child->next) out_put(o, ",", 1);
            out_put(o, "\n", 1);
        }
        out_indent(o, depth);
        out_str(o, object ? "}" : "]");
    } else if (cJSON_IsString(item)) {
        out_quoted(o, item->valuestring, 1);
    } else if (cJSON_IsNumber(item)) {
        char text[64];
        if (isfinite(item->valuedouble)) vg_js_number(item->valuedouble, text, sizeof(text));
        else snprintf(text, sizeof(text), "null");
        out_str(o, text);
    } else if (cJSON_IsTrue(item)) {
        out_str(o, "true");
    } else if (cJSON_IsFalse(item)) {
        out_str(o, "false");
    } else {
        out_str(o, "null");
    }
}

static void put_value(cJSON *obj, const char *key, cJSON *value) {
    if (!value) return;
    if (cJSON_GetObjectItemCaseSensitive(obj, key)) cJSON_ReplaceItemInObjectCaseSensitive(obj, key, value);
    else cJSON_AddItemToObject(obj, key, value);
}

static cJSON *cfg_json(const CfgValue *v) {
    return v->is_bool ? cJSON_CreateBool(v->b) : cJSON_CreateNumber(v->num);
}

static void export_record(const Slot *slot) {
    const Record *rec = slot->target >= 0 ? record_for(slot->target) : NULL;
    const cJSON *json = rec ? skin_json(rec->index) : NULL;
    if (!json) return;
    cJSON *obj = cJSON_Duplicate(json, 1);
    if (!obj) return;

    // the object as f453 left it: the JSON with every edited / defaulted property
    if (slot->type == ITEM_DYE) {
        for (int i = 0; i < D_COUNT; i++) {
            const CfgValue *v = &rec->col[i];
            if (!v->set || (v->in_json && !v->dirty)) continue;
            const int hex = (int) v->num & 0xFFFFFF;
            char css[48];
            if (rec->col_format[i] == VG_COLOR_CSS_RGB) snprintf(css, sizeof(css), "rgb(%d,%d,%d)", (hex >> 16) & 255, (hex >> 8) & 255, hex & 255);
            else snprintf(css, sizeof(css), "#%06x", hex);
            put_value(obj, k_dye[i].key, rec->col_format[i] != VG_COLOR_HEX ? cJSON_CreateString(css) : cJSON_CreateNumber(hex));
        }
    } else {
        for (int i = 0; i < F_COUNT; i++) {
            const CfgValue *v = &rec->f[i];
            if (!v->set || (v->in_json && !v->dirty)) continue;
            put_value(obj, k_fields[i].key, cfg_json(v));
        }
    }

    for (size_t i = 0; i < sizeof(k_export_strip) / sizeof(k_export_strip[0]); i++) cJSON_DeleteItemFromObjectCaseSensitive(obj, k_export_strip[i]);
    for (size_t i = 0; i < sizeof(k_export_zero) / sizeof(k_export_zero[0]); i++) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, k_export_zero[i]);
        if (item && loose_equals_zero(item)) cJSON_DeleteItemFromObjectCaseSensitive(obj, k_export_zero[i]);
    }
    for (size_t i = 0; i < sizeof(k_export_falsy) / sizeof(k_export_falsy[0]); i++) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, k_export_falsy[i]);
        if (item && js_falsy(item)) cJSON_DeleteItemFromObjectCaseSensitive(obj, k_export_falsy[i]);
    }
    // properties equal (===) to the item's template's are implied by it
    const cJSON *template_index = cJSON_GetObjectItemCaseSensitive(obj, "template");
    if (cJSON_IsNumber(template_index)) {
        cJSON *templates = cJSON_Parse(k_templates);
        const cJSON *tpl = cJSON_GetArrayItem(templates, (int) template_index->valuedouble);
        for (const cJSON *t = tpl ? tpl->child : NULL; t; t = t->next) {
            const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, t->string);
            if (item && cJSON_Compare(item, t, 1)) cJSON_DeleteItemFromObjectCaseSensitive(obj, t->string);
        }
        cJSON_Delete(templates);
    }

    Out o = {0};
    out_value(&o, obj, 0);
    cJSON_Delete(obj);
    if (o.buf) {
        html_clipboard_write(o.buf);
        if (getenv("KRUNKNATIVE_VIEWER_DEBUG")) fprintf(stderr, "[viewer] export config:\n%s\n", o.buf);
    }
    free(o.buf);
}

// Export Config buttons: one per slot (VgFn takes no arguments)
#define EXPORT_FN(n) static void export_##n(void) { if (n < g_cfg.slot_count) export_record(&g_cfg.slots[n]); }
EXPORT_FN(0) EXPORT_FN(1) EXPORT_FN(2) EXPORT_FN(3) EXPORT_FN(4) EXPORT_FN(5) EXPORT_FN(6) EXPORT_FN(7) EXPORT_FN(8) EXPORT_FN(9) EXPORT_FN(10)
static const VgFn k_export_fns[] = {export_0, export_1, export_2, export_3, export_4, export_5, export_6, export_7, export_8, export_9, export_10};

// ---------------------------------------------------------------- folders

static void slot_push(Slot *slot, VgCtrl *ctrl) {
    if (ctrl && slot->ctrl_count < CFG_MAX_SLOT_CTRLS) slot->ctrls[slot->ctrl_count++] = ctrl;
}

static void add_field(Slot *slot, Record *rec, const int field) {
    const Field *spec = &k_fields[field];
    CfgValue *v = &rec->f[field];
    if (!v->set) { // `if (x == null) x = false / 0`
        v->set = 1;
        v->is_bool = spec->is_bool;
        v->b = 0;
        v->num = 0.0;
    }
    watch(v);
    if (field == F_SIDE) {
        static const char *const labels[] = {"FrontSide", "BackSide", "DoubleSide"};
        static const char *const values[] = {"0", "1", "2"};
        if (!rec->side.is_string) rec->side.num = (int) cfg_num(v);
        slot_push(slot, vg_add_options(slot->folder, &rec->side, spec->label, 3, labels, values, on_edit));
    } else if (v->is_bool) {
        slot_push(slot, vg_add_bool(slot->folder, &v->b, spec->label, on_watched_edit));
    } else {
        slot_push(slot, vg_add_number(slot->folder, &v->num, spec->label, spec->min, spec->max, spec->step, on_watched_edit));
    }
}

// a Dye Config colour changed: the controllers' ints back into the records
static void on_dye_edit(void) {
    for (int i = 0; i < g_cfg.record_count; i++) {
        Record *rec = &g_cfg.records[i];
        for (int c = 0; c < D_COUNT; c++) {
            if (rec->col[c].set && (rec->hex[c] != ((int) rec->col[c].num & 0xFFFFFF) || rec->col_format[c] != rec->col_format0[c])) {
                rec->col[c].num = rec->hex[c];
                rec->col[c].dirty = 1;
            }
        }
    }
    on_edit();
}

static void build_dye(Slot *slot, Record *rec) {
    const int class_index = g_viewer.e.class_index.num;
    const ClassConfig *cls = &g_classes[class_index >= 0 && class_index < CLASS_COUNT ? class_index : 0];
    // config.skinColors[skinColIndex] (the array position the dropdown's id names) || colors[0]
    const int skin_color = g_viewer.e.skin_col.num;
    const int skin = skin_color >= 0 && skin_color < g_viewer_skin_color_count ? g_viewer_skin_colors[skin_color].col : cls->colors[0];
    for (int i = 0; i < D_COUNT; i++) {
        CfgValue *v = &rec->col[i];
        if (!v->set) { // `if (x == null) x = <class colour>`, in this order (waist takes pants)
            const int defaults[D_COUNT] = {
                [D_HAIR] = cls->colors[4], [D_SKIN] = skin, [D_SHIRT] = cls->colors[1], [D_SLEEVE] = cls->colors[5],
                [D_WAIST] = rec->col[D_PANTS].set ? (int) cfg_num(&rec->col[D_PANTS]) : cls->colors[2],
                [D_PANTS] = cls->colors[2], [D_SHOE] = cls->colors[3],
            };
            v->set = 1;
            v->num = defaults[i];
        }
        rec->hex[i] = (int) cfg_num(v) & 0xFFFFFF;
        slot_push(slot, vg_add_color(slot->folder, &rec->hex[i], &rec->col_format[i], k_dye[i].label, on_dye_edit));
    }
}

// f453(type) / f454(slot): rebuild the folder's controllers for vA353[type]
static void build(Slot *slot) {
    if (!slot->folder) return;
    for (int i = 0; i < slot->ctrl_count; i++) vg_remove(slot->folder, slot->ctrls[i]);
    slot->ctrl_count = 0;

    Record *rec = slot->target >= 0 ? record_for(slot->target) : NULL;
    if (!rec) {
        vg_folder_set_visible(slot->folder, 0);
        return;
    }

    if (slot->type < 0) {
        for (size_t i = 0; i < sizeof(k_weapon_fields) / sizeof(k_weapon_fields[0]); i++) add_field(slot, rec, k_weapon_fields[i]);
    } else if (slot->type == ITEM_DYE) {
        build_dye(slot, rec);
    } else {
        for (int i = 0; i < F_COUNT; i++) {
            if (!k_fields[i].only_type || k_fields[i].only_type == slot->type) add_field(slot, rec, i);
        }
    }
    if (slot->type >= 0) {
        const int n = (int) (slot - g_cfg.slots);
        if (n >= 0 && n < (int) (sizeof(k_export_fns) / sizeof(k_export_fns[0]))) slot_push(slot, vg_add_function(slot->folder, k_export_fns[n], "Export Config"));
    }
    vg_folder_set_visible(slot->folder, 1);
    if (slot->type < 0) vg_folder_open(slot->folder); // f454 opens its folder, f453 does not
    apply_record(rec); // f453's defaults are on the object now (a dye's colours show)
}

static VgValue *dropdown_of(const int type) {
    ViewerState *e = &g_viewer.e;
    switch (type) {
        case ITEM_FACE: return &e->face;
        case ITEM_HAT: return &e->hat;
        case ITEM_HEAD: return &e->head;
        case ITEM_BODY: return &e->body;
        case ITEM_BACK: return &e->back;
        case ITEM_WAIST: return &e->waist;
        case ITEM_SHOE: return &e->shoe;
        case ITEM_DYE: return &e->dye;
        case -1: return &e->skin_prim;
        case -2: return &e->skin_sec;
        case -3: return &e->melee;
        default: return NULL;
    }
}

static Slot *slot_of(const int type) {
    for (int i = 0; i < g_cfg.slot_count; i++) {
        if (g_cfg.slots[i].type == type) return &g_cfg.slots[i];
    }
    if (g_cfg.slot_count >= (int) (sizeof(g_cfg.slots) / sizeof(g_cfg.slots[0]))) return NULL;
    Slot *slot = &g_cfg.slots[g_cfg.slot_count++];
    memset(slot, 0, sizeof(*slot));
    slot->type = type;
    slot->target = -1;
    slot->dropdown = dropdown_of(type);
    slot->seen = slot->dropdown ? slot->dropdown->num : -1;
    return slot;
}

// ---------------------------------------------------------------- API

void vconfig_reset(void) {
    memset(&g_cfg.slots, 0, sizeof(g_cfg.slots));
    g_cfg.slot_count = 0;
    g_cfg.record_count = 0;
    g_watch_count = 0;
    viewer_skin_edits_clear();
}

void vconfig_folder(VgFolder *parent, const int type, const char *name) {
    Slot *slot = slot_of(type);
    if (!slot || !parent) return;
    slot->parent = parent;
    slot->folder = vg_add_folder(parent, name);
    slot->ctrl_count = 0;
    build(slot);
}

void vconfig_remove_folders(VgFolder *parent) {
    for (int i = 0; i < g_cfg.slot_count; i++) {
        Slot *slot = &g_cfg.slots[i];
        if (slot->parent != parent || !slot->folder) continue;
        vg_folder_remove(parent, slot->folder);
        slot->folder = NULL;
        slot->ctrl_count = 0;
    }
}

void vconfig_snapshot(void) {
    for (int i = 0; i < g_cfg.slot_count; i++) {
        Slot *slot = &g_cfg.slots[i];
        if (slot->dropdown) slot->seen = slot->dropdown->num;
    }
}

void vconfig_changed(void) {
    for (int i = 0; i < g_cfg.slot_count; i++) {
        Slot *slot = &g_cfg.slots[i];
        if (!slot->dropdown || slot->dropdown->num == slot->seen) continue;
        // `vA353[type] = Game.store.skins[value]; f453(type)` (Default -1: no entry, hidden)
        slot->seen = slot->dropdown->num;
        slot->target = viewer_skin(slot->seen) ? slot->seen : -1;
        build(slot);
    }
}
