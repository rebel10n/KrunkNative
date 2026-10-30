// Map editor data model, GL-free: Krunker v7.1.4 libs/ObjectInstance.js (constructor +
// serialize, OI:372-800 / 1006-1963) and editor.js getMapExport / importMap / loadObjects
// (E:3363-3597, 3733-3760, 3904-4253). See editor_model.h for the API and the intentional
// differences from v7.1.4. The ObjectInstance port reads like the JavaScript: the constructor's
// parseInt / parseFloat / `== null` / `=== undefined` rules become the js_* helpers below, and
// serialize() writes the keys in v7's order with v7's gates and rounding.
#include "editor_model.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
// data (the generated editor_data.c blob)

static cJSON *g_data;

const cJSON *editor_data_json(void) {
    if (!g_data) g_data = cJSON_Parse(editor_data_text);
    return g_data;
}

const cJSON *editor_data_get(const char *key) {
    return cJSON_GetObjectItemCaseSensitive(editor_data_json(), key);
}

static double data_num(const char *section, const char *key, const double fallback) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(editor_data_get(section), key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

// ---------------------------------------------------------------------------
// JavaScript value semantics over cJSON

static const cJSON *G(const cJSON *o, const char *k) { return o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL; }
static int js_undef(const cJSON *o, const char *k) { return G(o, k) == NULL; }        // p.k === undefined
static int js_nullish(const cJSON *v) { return !v || cJSON_IsNull(v); }               // v == null
static int js_null(const cJSON *o, const char *k) { return js_nullish(G(o, k)); }     // p.k == null

static int js_truthy(const cJSON *v) {
    if (js_nullish(v) || cJSON_IsFalse(v)) return 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0.0 && !isnan(v->valuedouble);
    if (cJSON_IsString(v)) return v->valuestring[0] != 0;
    return 1;
}

static int num_truthy(const double v) { return v != 0.0 && !isnan(v); }

// JS String(v) of a number: integers exactly, others in the shortest form that round-trips
static void js_num_str(const double v, char *out, const size_t n) {
    if (isnan(v)) {
        snprintf(out, n, "NaN");
        return;
    }
    if (isinf(v)) {
        snprintf(out, n, v > 0 ? "Infinity" : "-Infinity");
        return;
    }
    if (v == floor(v) && fabs(v) < 1e21) {
        snprintf(out, n, "%.0f", v);
        return;
    }
    for (int p = 1; p <= 17; p++) {
        snprintf(out, n, "%.*g", p, v);
        if (strtod(out, NULL) == v) return;
    }
}

// String(v) for the toString() calls of the constructor (st, ldc, inm): NULL = not a string or number
static const char *js_str(const cJSON *v, char *buf, const size_t n) {
    if (cJSON_IsString(v)) return v->valuestring;
    if (cJSON_IsNumber(v)) {
        js_num_str(v->valuedouble, buf, n);
        return buf;
    }
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? "true" : "false";
    return NULL;
}

static double parse_int_str(const char *s) {
    while (isspace((unsigned char) *s)) s++;
    int neg = 0;
    if (*s == '+' || *s == '-') neg = *s++ == '-';
    int base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    double v = 0;
    int any = 0;
    for (;; s++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (base == 16 && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (base == 16 && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        v = v * base + d;
        any = 1;
    }
    return any ? (neg ? -v : v) : NAN;
}

static double parse_float_str(const char *s) {
    while (isspace((unsigned char) *s)) s++;
    const char *p = s;
    if (*p == '+' || *p == '-') p++;
    if (!strncmp(p, "Infinity", 8)) return *s == '-' ? -INFINITY : INFINITY;
    const char *digits = p;
    while (isdigit((unsigned char) *p)) p++;
    if (*p == '.') {
        p++;
        while (isdigit((unsigned char) *p)) p++;
    }
    if (p == digits || (p == digits + 1 && *digits == '.')) return NAN;
    if (*p == 'e' || *p == 'E') {
        const char *e = p + 1;
        if (*e == '+' || *e == '-') e++;
        if (isdigit((unsigned char) *e)) {
            while (isdigit((unsigned char) *e)) e++;
            p = e;
        }
    }
    char buf[64];
    const size_t len = (size_t) (p - s) < sizeof(buf) - 1 ? (size_t) (p - s) : sizeof(buf) - 1;
    memcpy(buf, s, len);
    buf[len] = 0;
    return strtod(buf, NULL);
}

// parseInt(v) / parseFloat(v): String(v) parsed; arrays use their first element (String([5]) == "5")
static double js_parse_int(const cJSON *v) {
    if (cJSON_IsArray(v)) v = cJSON_GetArrayItem(v, 0);
    if (cJSON_IsNumber(v)) return isfinite(v->valuedouble) ? trunc(v->valuedouble) : (isnan(v->valuedouble) ? NAN : NAN);
    if (cJSON_IsString(v)) return parse_int_str(v->valuestring);
    return NAN;
}

static double js_parse_float(const cJSON *v) {
    if (cJSON_IsArray(v)) v = cJSON_GetArrayItem(v, 0);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v)) return parse_float_str(v->valuestring);
    return NAN;
}

static double js_parse_int_d(const double v) { return isfinite(v) ? trunc(v) : NAN; } // parseInt(<number>)

// Number(v) (loose comparisons, Math.max coercion)
static double js_number(const cJSON *v) {
    if (js_nullish(v)) return cJSON_IsNull(v) ? 0.0 : NAN;
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? 1.0 : 0.0;
    if (cJSON_IsString(v)) {
        const char *s = v->valuestring;
        while (isspace((unsigned char) *s)) s++;
        if (!*s) return 0.0;
        char *end = NULL;
        const double d = strtod(s, &end);
        while (end && isspace((unsigned char) *end)) end++;
        return end && !*end ? d : NAN;
    }
    return NAN;
}

// parseInt(p.k || d) / parseFloat(p.k || d)
static double pi_or(const cJSON *o, const char *k, const double d) {
    const cJSON *v = G(o, k);
    return js_truthy(v) ? js_parse_int(v) : js_parse_int_d(d);
}

static double pf_or(const cJSON *o, const char *k, const double d) {
    const cJSON *v = G(o, k);
    return js_truthy(v) ? js_parse_float(v) : d;
}

// p.k == null ? d : parseInt(p.k || 0) / parseFloat(p.k || 0)
static double pi_null(const cJSON *o, const char *k, const double d) { return js_null(o, k) ? d : pi_or(o, k, 0); }
static double pf_null(const cJSON *o, const char *k, const double d) { return js_null(o, k) ? d : pf_or(o, k, 0); }

// Number.prototype.round
static double js_round(const double v, const int d) {
    const double p = pow(10.0, d);
    return floor(v * p + 0.5) / p;
}

// /[^<allowed>]+/.test(s) for the constructor's text checks
static const char *const K_TEXT_OK = ":;()?,.|[] -+*$@=^!{}";        // \w plus these
static const char *const K_TEXT_OK_BS = ":;()?,.|[] -+*$@=^!{}\\";   // ... and backslash (the replace() filters)

static int text_char_ok(const unsigned char c, const char *extra) {
    return isalnum(c) || c == '_' || (c && strchr(extra, c));
}

static int text_valid(const char *s, const char *extra) {
    for (; *s; s++) if (!text_char_ok((unsigned char) *s, extra)) return 0;
    return 1;
}

static char *text_filter(const char *s, const char *extra) {
    char *out = malloc(strlen(s) + 1);
    size_t j = 0;
    for (; out && *s; s++) if (text_char_ok((unsigned char) *s, extra)) out[j++] = *s;
    if (out) out[j] = 0;
    return out;
}

// p.k && !/[^<set>]+/i.test(p.k) ? p.k : "": an id value (asset ids, path lists). A number passes
// as its digits (String(n)); returns the value or NULL. Values serialize() checks with `.length`
// survive only as strings (id_string).
static const char *const K_ASSET_ID = "0123456789tlnesacdgxufqmrpbhv"; // models, sky textures, minimap (v10 E10:7130: /[^\dtlnesacdgxufqmrpbhv]+/i)
static const char *const K_ASSET_ID_V = "0123456789tlnesacvdgxufq"; // + video (projectile asset)

static const cJSON *id_value(const cJSON *v, const char *set) {
    char buf[64];
    const char *s = cJSON_IsString(v) || cJSON_IsNumber(v) ? js_str(v, buf, sizeof(buf)) : NULL;
    if (!s || !js_truthy(v)) return NULL;
    for (; *s; s++) if (!strchr(set, tolower((unsigned char) *s))) return NULL;
    return v;
}

static const char *id_string(const cJSON *v, const char *set) {
    v = id_value(v, set);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int is_hex_color(const cJSON *v) {
    if (!cJSON_IsString(v)) return 0;
    const char *s = v->valuestring;
    const size_t n = strlen(s);
    if (s[0] != '#' || (n != 4 && n != 7)) return 0;
    for (size_t i = 1; i < n; i++) if (!isxdigit((unsigned char) s[i])) return 0;
    return 1;
}

// the value written for a key: replace in place (JS keeps the first insertion position)
static void put(cJSON *o, const char *k, cJSON *v) {
    if (!v) return;
    if (cJSON_GetObjectItemCaseSensitive(o, k)) cJSON_ReplaceItemInObjectCaseSensitive(o, k, v);
    else cJSON_AddItemToObject(o, k, v);
}

static void put_num(cJSON *o, const char *k, const double v) { put(o, k, cJSON_CreateNumber(v)); } // NaN prints null
static void put_one(cJSON *o, const char *k) { put_num(o, k, 1); }
static void put_nz(cJSON *o, const char *k, const double v) { put_num(o, k, num_truthy(v) ? v : 0); } // parseX(this.k || 0)
static void put_dup(cJSON *o, const char *k, const cJSON *v) { put(o, k, cJSON_Duplicate(v, 1)); }

// ---------------------------------------------------------------------------
// prefabs

static int prefab_by_name(const char *name) {
    for (int i = 0; i < EDITOR_PREFAB_COUNT; i++) if (!strcasecmp(editor_prefabs[i].name, name)) return i;
    return -1;
}

int ed_model_prefab(const cJSON *obj) {
    // p.i = p.i || p.id || 0; prefabs[p.i] (an index, so a numeric string works too)
    const cJSON *i = G(obj, "i");
    if (!js_truthy(i)) i = G(obj, "id");
    if (!js_truthy(i)) return 0;
    if (cJSON_IsNumber(i)) {
        const double d = i->valuedouble;
        return d == floor(d) && d >= 0 && d < EDITOR_PREFAB_COUNT ? (int) d : -1;
    }
    if (cJSON_IsString(i)) {
        char *end = NULL;
        const long id = strtol(i->valuestring, &end, 10);
        if (end && !*end && end != i->valuestring) return id >= 0 && id < EDITOR_PREFAB_COUNT ? (int) id : -1;
        return prefab_by_name(i->valuestring); // KrunkNative: v3.1.1-style prefab names (importOldMap)
    }
    return -1;
}

static int texture_by_name(const char *name) {
    for (int i = 0; i < EDITOR_TEXTURE_COUNT; i++) if (!strcasecmp(editor_textures[i], name)) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// the keys ObjectInstance knows (constructor reads + serialize writes, legacy included)

static const char *const K_OBJECT_KEYS[] = {
    // core / transient
    "p", "s", "i", "id", "r", "objUUID", "meshUUID",
    // constructor + serialize
    "aid", "lt", "la", "lp", "f", "zp", "cp", "to", "ssp", "nf", "bb", "sf", "nsf", "st", "sc", "sb", "so", "sot", "bv",
    "sa", "fc", "tox", "toy", "tr", "tro", "txs", "tsr", "tsm", "fct", "fs", "ft", "rs", "ten", "tv", "alt", "at", "spt",
    "t", "col", "l", "pl", "ns", "nai", "wj", "gp", "ip", "pe", "b", "bm", "cr", "en", "h", "hp", "cdy", "sd", "rsc",
    "pr", "pc", "ps", "psp", "pg", "pd", "psc", "pb", "tm", "v", "ter", "c", "e", "da", "o", "d", "fd", "ts", "td", "sp",
    "su", "sup", "wid", "w", "wr", "rw", "rwg", "wpi", "bo", "smm", "ant", "spn", "spa", "ch", "tcp", "n", "m", "ab",
    "nm", "cl", "stc", "md", "ct", "te", "ta", "tac", "tam", "sfd", "tc", "tc2", "tce", "tct", "tcc", "tad", "cd", "cv0",
    "cv1", "cv2", "cha", "ci", "rt", "rr", "tsd", "rmt", "rmr", "aic", "vd", "igos", "dos", "ivos", "cs", "cnh", "xv",
    "tAi", "tPl", "aif", "exp", "spd", "fr", "aac", "hr", "shb", "sha", "shof", "shdy", "mldy", "mls", "ss", "se", "he",
    "crd", "mlr", "mld", "prg", "prgl", "prgt", "prsc", "prms", "prs", "pjs", "pjd", "bh", "rew", "rewm", "rewr", "rewn",
    "ain", "ail", "ains", "in", "tin", "wd", "dp", "obi", "obl", "ws", "wh", "tx", "tz", "es", "esa", "idl", "dth", "atk",
    "atkh", "mel", "melh", "agr", "ev", "ed", "edm", "ero", "er", "ef", "ms", "myo", "mro", "hbh", "hbw", "grv", "li",
    "lds", "ld", "opn", "rtp", "rbt", "thl", "ths", "sds", "skin", "cst", "rcv", "as", "ast", "pra", "ec", "nid", "phi",
    "pht", "bng", "bn", "bnn", "bnr", "vdg", "vad", "vts", "vtl", "vmp", "vpf", "vmr", "vrf", "vr", "sty", "krs", "ldt",
    "ldc", "ldo", "ldr", "sci", "fcr", "cstc", "inm", "fp", "camt", "frc", "msp", "sat", "cpx", "bbS", "bbD", "pmv", "nt",
    "ntx", "tas",
    // Krunker v10
    "dyn", "boss", "cnch", "ambFadeTime", "crt", "eid", "ar", "tag", "pn", "sfz", "mp",
    // ENVIRONMENT_ZONE
    "ambInd", "ambIndC", "skyDome", "skyDomeCol0", "skyDomeCol1", "skyDomeCol2", "skyDomeEmis", "skyDomeEmisTex",
    "skyDomeTex", "skyDomeTexA", "skyDomeMovD", "skyDomeMovT", "shadowR", "shadowD", "ambientC", "ambientI", "light",
    "lightD", "lightI", "sunAngX", "sunAngY", "sky", "fogC", "fogD", "correctLights", "toneMapping",
    "toneMappingExposure", "outputEncoding",
    // BOT_SPAWNER
    "btn", "btc", "btl", "btt", "bthm", "btdm", "btspm", "btrm", "btsm", "btfc", "bttc", "bttt", "btfcr", "btat", "btft",
    "btwt", "btmt", "btsh", "btcr", "btsd", "btrd", "btny", "btno", "btjm", "btlm", "btvc", "btgm", "btr", "btrs", "btrc",
    "btcpr", "btcd", "btcf", "btcht", "btch", "btcbk", "btcb", "btcw", "btcwr", "btcs", "btcp", "btcm",
    // the export pools (expanded by loadObjects before the constructor sees the object)
    "ci", "ei", "si",
    NULL,
};

int ed_model_known_key(const char *key) {
    for (int i = 0; K_OBJECT_KEYS[i]; i++) if (!strcmp(K_OBJECT_KEYS[i], key)) return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// ObjectInstance: constructor (OI:372-780) + update() size rules (OI:809-908) + serialize()
// (OI:1006-1963), for one object. `p` is the constructor argument, `out` the serialized object.

#define HAS(f) editor_prefab_has(prefab, f)
#define IS(n) (!strcmp(pname, n))

typedef struct {
    double action, constant, constant2, constant_type, delay, snd, asset_sound, vol, rate, emitfrom, condition;
    const char *txt, *cust;
    const cJSON *cv[3];
} OiAction;

// ObjectInstance.addAction(p): one entry of trigActions
static void oi_action_load(OiAction *a, const cJSON *p) {
    memset(a, 0, sizeof(*a));
    a->action = pi_or(p, "ta", 0);
    a->constant = pi_or(p, "tc", 0);
    a->constant2 = pi_or(p, "tc2", 0);
    a->constant_type = pi_or(p, "tce", 0);
    const cJSON *tct = G(p, "tct"), *tcc = G(p, "tcc");
    a->txt = cJSON_IsString(tct) && tct->valuestring[0] && text_valid(tct->valuestring, K_TEXT_OK) ? tct->valuestring : "";
    a->cust = cJSON_IsString(tcc) && tcc->valuestring[0] && text_valid(tcc->valuestring, K_TEXT_OK) ? tcc->valuestring : "";
    a->delay = pf_or(p, "tad", 0);
    a->snd = pi_or(p, "es", 0);
    a->asset_sound = pi_or(p, "esa", 0);
    a->vol = pf_or(p, "ev", 1);
    a->rate = pf_or(p, "er", 1);
    a->emitfrom = pi_or(p, "ef", 0);
    a->condition = js_parse_int(G(p, "cd"));
    if (!num_truthy(a->condition)) a->condition = 0; // parseInt(p.cd) || 0
    a->cv[0] = G(p, "cv0");
    a->cv[1] = G(p, "cv1");
    a->cv[2] = G(p, "cv2");
}

static cJSON *oi_action_serialize(const OiAction *a) {
    cJSON *o = cJSON_CreateObject();
    const int sound = a->action == 14 || a->action == 34 || a->action == 54;
    if (sound) {
        if (num_truthy(a->asset_sound)) put_num(o, "esa", js_parse_int_d(a->asset_sound));
        else put_num(o, "es", js_parse_int_d(a->snd));
        put_num(o, "ev", js_round(a->vol, 2));
        put_num(o, "er", js_round(a->rate, 1));
        if (num_truthy(a->emitfrom)) put_num(o, "ef", js_parse_int_d(a->emitfrom));
    }
    if (num_truthy(a->action)) put_num(o, "ta", js_parse_int_d(a->action));
    if (num_truthy(a->constant)) put_num(o, "tc", js_parse_int_d(a->constant));
    if (num_truthy(a->constant2)) put_num(o, "tc2", js_parse_int_d(a->constant2));
    if (num_truthy(a->constant_type)) put_num(o, "tce", js_parse_int_d(a->constant_type));
    if (a->txt[0]) put(o, "tct", cJSON_CreateString(a->txt));
    if (a->cust[0]) put(o, "tcc", cJSON_CreateString(a->cust));
    if (num_truthy(a->delay)) put_num(o, "tad", js_round(a->delay, 1));
    if (num_truthy(a->condition)) {
        put_num(o, "cd", js_parse_int_d(a->condition));
        static const char *const keys[3] = {"cv0", "cv1", "cv2"};
        for (int i = 0; i < 3; i++) if (!js_nullish(a->cv[i])) put_dup(o, keys[i], a->cv[i]);
    }
    return o;
}

// UTILS.convertFaces(hex): the face switches of a string "f" (Right Left Top Bottom Back Front).
// Returns the bit count (6, more for out-of-range values); bits[0] is the first face.
static int faces_from(const cJSON *f, unsigned char bits[32]) {
    memset(bits, 1, 32);
    if (cJSON_IsString(f) && f->valuestring[0]) {
        char *end = NULL;
        const unsigned long n = strtoul(f->valuestring, &end, 16); // parseInt(f, 16)
        if (end == f->valuestring) {
            memset(bits, 0, 6); // "NaN".padStart(6, "0").split("").map(Number): no face is on
            return 6;
        }
        int len = 6;
        while (len < 32 && (n >> len)) len++;
        for (int i = 0; i < len; i++) bits[i] = (unsigned char) ((n >> (len - 1 - i)) & 1);
        return len;
    }
    if (cJSON_IsArray(f) && cJSON_GetArraySize(f) > 0) {
        int len = 0;
        const cJSON *it;
        cJSON_ArrayForEach(it, f) if (len < 32) bits[len++] = (unsigned char) js_truthy(it);
        return len;
    }
    return 6;
}

static int texture_index_of(const cJSON *t, const int ladder) {
    // isString(p.t) ? p.t || (ladder ? "FLOOR" : "DEFAULT") : textures[p.t == null ? (ladder ? 2 : 0) : p.t || 0].name
    if (cJSON_IsString(t) && t->valuestring[0]) {
        const int i = texture_by_name(t->valuestring);
        return i >= 0 ? i : 5; // KrunkNative: an unknown name is DEFAULT (v7 throws "Invalid texture id.")
    }
    if (js_nullish(t)) return ladder ? 2 : 0;
    const double d = js_truthy(t) ? js_number(t) : 0;
    if (d == floor(d) && d >= 0 && d < EDITOR_TEXTURE_COUNT) return (int) d;
    return 5; // KrunkNative: an invalid index is DEFAULT (v7 throws reading textures[t].name)
}

cJSON *ed_model_object(const cJSON *p, const int flags) {
    const int keep_uuid = flags & ED_MODEL_KEEP_UUID, exact = flags & ED_MODEL_EXACT_TRANSFORM;
    const int prefab = ed_model_prefab(p);
    if (prefab < 0) return NULL;
    const EditorPrefab *pf = &editor_prefabs[prefab];
    const char *pname = pf->name;
    const int ai = HAS(EP_AI), bot = HAS(EP_BOT);
    const int trigger = IS("TRIGGER");
    const int ladder = IS("LADDER") || IS("ROPE_LADDER");
    char buf[64];
    cJSON *out = cJSON_CreateObject();

    // ---- constructor
    const cJSON *aid = id_value(G(p, "aid"), K_ASSET_ID);
    // defaultSize: prefab.defaultSize unless an asset model (or no prefab size): then the model's
    // bounds, which only the loaded model knows (see the header: the imported "s" is kept)
    // (a genGeo prefab's computed size is its generated geometry's: SPAWN_POINT's 7x11x7)
    const int real_default = (pf->has_default_size == 1 || (pf->has_default_size == 2 && HAS(EP_GENGEO))) && !aid;
    double default_size[3] = {pf->default_size[0], pf->default_size[1], pf->default_size[2]};
    if (IS("SERVER_NODE") || IS("SPAWN_POINT")) { // generateServerNode / generateSpawn: defaultSize = [5 | 7 + spread, 5 | 11, 5 | 7 + spread]
        double spread = pf_or(p, "ssp", 0);
        if (!num_truthy(spread)) spread = 0;
        default_size[0] += spread;
        default_size[2] += spread;
    }
    // v10.0.5 COLLISION_MESH (editor-10.0.5.js:7394-7408): collTris = p.cmt when it is an array of whole triangles
    // (parseFloat || 0 each), else []; collBase = its extent per axis (at least 0.1); without an "s" array the
    // size is collBase
    cJSON *coll_tris = NULL;
    if (IS("COLLISION_MESH")) {
        const cJSON *cmt = G(p, "cmt");
        coll_tris = cJSON_CreateArray();
        if (cJSON_IsArray(cmt) && cJSON_GetArraySize(cmt) % 9 == 0) {
            const cJSON *e;
            cJSON_ArrayForEach(e, cmt) {
                const double v = js_parse_float(e);
                cJSON_AddItemToArray(coll_tris, cJSON_CreateNumber(num_truthy(v) ? v : 0));
            }
        }
        double base[3];
        ed_model_collision_base(coll_tris, base);
        for (int k = 0; k < 3; k++) default_size[k] = base[k];
    }

    // ---- serialize: p, s, i
    const cJSON *pos = G(p, "p");
    cJSON *pa = cJSON_CreateArray();
    for (int k = 0; k < 3; k++) {
        const double v = js_parse_float(cJSON_GetArrayItem(pos, k));
        const double x = num_truthy(v) ? v : 0;
        cJSON_AddItemToArray(pa, cJSON_CreateNumber(exact ? x : js_round(x, 2))); // (parseFloat(x) || 0).round(2)
    }
    cJSON_AddItemToObject(out, "p", pa);

    double size[3];
    const cJSON *s = G(p, "s");
    const int has_s = js_truthy(s) && cJSON_IsArray(s);
    for (int k = 0; k < 3; k++) {
        double v = has_s ? js_number(cJSON_GetArrayItem(s, k)) : default_size[k];
        if (HAS(EP_DONTROUND)) v = isnan(v) ? NAN : fmax(0.00001, v); // update(): the scale clamp
        else v = isnan(v) ? NAN : js_round(fmax(0.01, v), 2);
        if (!HAS(EP_SCALABLE) || aid) {
            // update() ends with size = defaultSize (unclamped): the prefab's, else the model's
            // bounds, i.e. the size the map holds (see the header)
            if (real_default || !has_s) v = default_size[k];
            else v = js_number(cJSON_GetArrayItem(s, k));
        }
        if (k == 0 && HAS(EP_NOSCALEX)) v = pf->default_size[0];
        size[k] = v;
    }
    cJSON *sa = cJSON_CreateArray();
    for (int k = 0; k < 3; k++) {
        const double v = js_round(num_truthy(size[k]) ? size[k] : 0, 2);
        cJSON_AddItemToArray(sa, cJSON_CreateNumber(isfinite(v) ? v : 0));
    }
    cJSON_AddItemToObject(out, "s", sa);
    if (keep_uuid && G(p, "objUUID")) put_dup(out, "objUUID", G(p, "objUUID")); // serialize(true): objUUID (meshUUID is three.js state)
    if (prefab) put_num(out, "i", prefab);

    // collision / movement flags
    const int collidable = HAS(EP_NOCOLLISION) ? 0 : HAS(EP_FORCECOLLISION) ? 1 : js_undef(p, "col") && js_undef(p, "l"); // (the setter applies noCollision last)
    if (!collidable) put_one(out, "l");
    if (!js_undef(p, "pl")) put_one(out, "pl");
    if (js_truthy(G(p, "dyn"))) put_one(out, "dyn"); // v10 (E10:7236, 7814): this.dyn = !!p.dyn
    if (!js_undef(p, "ns")) put_one(out, "ns");
    if (!js_undef(p, "nai")) put_one(out, "nai");
    if (HAS(EP_CANWALLJUMP) && !js_undef(p, "wj")) put_one(out, "wj");
    if (HAS(EP_CANGRAPPLE) && !js_undef(p, "gp")) put_one(out, "gp");
    if (HAS(EP_CANIMPULSE) && !js_undef(p, "ip")) put_one(out, "ip");
    const double pen = pf_or(p, "pe", 0);
    if (num_truthy(pen) && HAS(EP_EDITPEN)) put_num(out, "pe", js_round(pen, 2));
    if (!js_undef(p, "v")) put_one(out, "v");
    const double boost = cJSON_IsTrue(G(p, "b")) ? NAN : pf_or(p, "b", 0); // parseFloat(true) is NaN
    if (num_truthy(boost)) put_num(out, "b", js_round(boost, 2));
    const double bounce = pf_or(p, "bm", 1);
    if (bounce != 1) put_num(out, "bm", js_round(bounce, 2));
    if (!js_null(p, "cr")) put_one(out, "cr");
    const double edge = pf_or(p, "en", 0);
    if (num_truthy(edge)) put_num(out, "en", js_round(edge, 1));
    double health;
    {
        const cJSON *h = G(p, "h");
        if (!js_truthy(h)) h = G(p, "hp");
        health = js_truthy(h) ? js_parse_int(h) : 0; // parseInt(p.h || p.hp || 0)
    }
    if (num_truthy(health)) put_num(out, "h", health);
    const int can_dest = !js_null(p, "cdy");
    if (can_dest) put_one(out, "cdy");
    if (IS("PARTICLES")) {
        const double part = pi_or(p, "pr", 0), part_c = pi_or(p, "pc", 0), part_s = pf_or(p, "ps", 0), part_sp = pf_or(p, "psp", 0),
                     part_g = pf_or(p, "pg", 0), part_d = pf_or(p, "pd", 0), part_sc = pf_or(p, "psc", 0), part_b = pi_or(p, "pb", 0);
        // the particle values are written against a default of 1 but load as 0 (doc/v7/editor.md 1.13: match on write)
        if (num_truthy(part) && part != 0) put_num(out, "pr", part);
        if (num_truthy(part_c) && part_c > 0) put_num(out, "pc", part_c);
        if (num_truthy(part_s) && part_s != 1) put_num(out, "ps", js_round(part_s, 1));
        if (num_truthy(part_sc) && part_sc != 1) put_num(out, "psc", js_round(part_sc, 1));
        if (num_truthy(part_sp) && part_sp != 1) put_num(out, "psp", js_round(part_sp, 1));
        if (num_truthy(part_g) && part_g != 1) put_num(out, "pg", js_round(part_g, 1));
        if (num_truthy(part_d) && part_d != 1) put_num(out, "pd", js_round(part_d, 1));
        if (num_truthy(part_b) && part_b != 0) put_num(out, "pb", part_b);
    }
    // rotation: written when any loaded component is non-zero, rounded (a tiny rotation writes [0,0,0])
    if (!HAS(EP_NOROTATION) && js_truthy(G(p, "r"))) {
        const cJSON *r = G(p, "r");
        double rv[3];
        int any = 0;
        for (int k = 0; k < 3; k++) {
            const cJSON *c = cJSON_GetArrayItem(r, k);
            rv[k] = c ? js_number(c) : 0; // new THREE.Euler(...r): a missing component is 0
            any |= num_truthy(rv[k]);
        }
        if (any) {
            cJSON *ra = cJSON_CreateArray();
            for (int k = 0; k < 3; k++) cJSON_AddItemToArray(ra, cJSON_CreateNumber(exact ? (isnan(rv[k]) ? 0 : rv[k]) : js_round(rv[k], 2)));
            put(out, "r", ra);
        }
    }
    if (HAS(EP_EDITFACES)) {
        unsigned char bits[32];
        const int len = faces_from(G(p, "f"), bits);
        int all = 1;
        unsigned long n = 0;
        for (int k = 0; k < len; k++) {
            all &= bits[k];
            n = (n << 1) | bits[k];
        }
        if (!all) {
            snprintf(buf, sizeof(buf), "%lx", n); // convertFaces(faces, 1)
            put(out, "f", cJSON_CreateString(buf));
        }
    }
    // colour: the setter only stores it for editColor prefabs; written unless the default string
    {
        const cJSON *c = G(p, "c"), *e = G(p, "e");
        if (HAS(EP_EDITCOLOR) && !js_nullish(c) && js_truthy(c) && !(cJSON_IsString(c) && !strcmp(c->valuestring, "#FFFFFF"))) put_dup(out, "c", c);
        if (HAS(EP_EDITEMISSIVE) && !js_nullish(e) && js_truthy(e) && !(cJSON_IsString(e) && !strcmp(e->valuestring, "#000000"))) put_dup(out, "e", e);
    }
    {
        const double darken = js_truthy(G(p, "da")) ? js_parse_float(G(p, "da")) : 0;
        if (num_truthy(darken)) put_num(out, "da", darken);
    }
    const double opacity = js_null(p, "o") ? 1 : js_parse_float(G(p, "o"));
    if (HAS(EP_EDITOPAC) && opacity != 1) put_num(out, "o", js_round(opacity, 1));
    // texture
    const int no_texture = HAS(EP_NOTEXTURE);
    const int tex = no_texture ? -1 : texture_index_of(G(p, "t"), ladder);
    const double tex_var = pi_or(p, "tv", 0);
    const double spray = num_truthy(js_parse_int(G(p, "spt"))) ? js_parse_int(G(p, "spt")) : 2482;
    const cJSON *asset_tex = id_value(G(p, "at"), "0123456789v");
    if (HAS(EP_TEXTURABLE)) {
        if (HAS(EP_SPRAYTEXTURE)) put_num(out, "spt", spray);
        else if (asset_tex) put_dup(out, "at", asset_tex);
        else {
            const int id = tex < 0 ? 0 : tex;
            if (id || ladder) put_num(out, "t", id);
            if (tex >= 0 && editor_texture_variants[tex] && num_truthy(tex_var)) put_num(out, "tv", tex_var);
        }
        const double tox = pf_or(p, "tox", 0), toy = pf_or(p, "toy", 0);
        if (tox > 0) put_num(out, "tox", js_round(tox, 2));
        if (toy > 0) put_num(out, "toy", js_round(toy, 2));
        // parseInt((p.tr == null ? p.tro || 0 : toDeg(parseFloat(p.tr))) || 0): "tr" is the legacy radians
        double tro;
        if (js_null(p, "tr")) tro = pi_or(p, "tro", 0);
        else {
            tro = js_parse_float(G(p, "tr")) * 180.0 / M_PI;
            tro = num_truthy(tro) ? js_parse_int_d(tro) : 0;
        }
        if (tro != 0) put_num(out, "tro", tro);
    }
    const double direction = pi_or(p, "d", 0);
    if (HAS(EP_CUSTOMDIRECTION) && direction > 0) put_num(out, "d", direction);
    if (HAS(EP_OPTIONALDIRECTION) && !js_null(p, "fd")) put_one(out, "fd");
    if (HAS(EP_CANSTRETCH) && !js_null(p, "tsr")) put_one(out, "tsr");
    {
        const double tsm = js_parse_float(G(p, "tsm"));
        if (HAS(EP_CANSTRETCH) && num_truthy(tsm)) put_num(out, "tsm", js_round(tsm, 2));
    }
    if (HAS(EP_TEAMABLE)) put_num(out, "tm", pi_or(p, "tm", 0));
    const double moving = pf_or(p, "ts", 0), axis = pf_or(p, "td", 0);
    if (HAS(EP_MOVINGTEXTURE) && moving != 0) {
        put_num(out, "ts", js_round(moving, 1));
        if (axis != 0) put_num(out, "td", js_parse_int_d(axis));
    }
    const double frames = pi_or(p, "fct", 0), frame_speed = pf_or(p, "fs", 0);
    if (HAS(EP_FRAMETEXTURE) && frames != 0) {
        put_num(out, "fct", frames);
        put_num(out, "fs", js_round(frame_speed, 1));
    }
    const double score_def = HAS(EP_INTERACT) || ai ? 0 : 1;
    const double score = pi_or(p, "sp", score_def);
    if (HAS(EP_CUSTOMSCORE) && score > score_def) put_num(out, "sp", score);
    if ((IS("CHECK_POINT") || trigger || ai) && !js_undef(p, "su")) put_one(out, "su");
    if (trigger && !js_undef(p, "sup")) put_one(out, "sup");
    const double event = pi_or(p, "te", ai ? 15 : 0);
    const double action_cost = pi_or(p, "tac", 0), action_method = pi_or(p, "tam", 0);
    if (trigger && event == 16 && action_cost > 0) put_num(out, "tac", action_cost);
    if (trigger && event == 16 && action_method > 0) put_num(out, "tam", action_method);
    if (IS("WEAPON_PICKUP")) {
        const cJSON *w = G(p, "wid");
        if (!js_truthy(w)) w = G(p, "w");
        const double weapon = js_truthy(w) ? js_parse_int(w) : 0;
        if (weapon > 0) put_num(out, "w", weapon);
        if (!js_undef(p, "wr")) put_one(out, "wr");
        if (!js_null(p, "rw")) put_one(out, "rw");
        double group = pi_or(p, "rwg", 0);
        if (!num_truthy(group)) group = 0;
        if (group > 0) put_num(out, "rwg", group);
        if (!js_undef(p, "wpi")) put_one(out, "wpi");
        if (!js_null(p, "ntx")) put_one(out, "ntx");
    }
    {
        const double poster = pi_or(p, "bb", 0);
        if (IS("BILLBOARD") && poster > 0) put_num(out, "bb", poster);
    }
    if (HAS(EP_HASBORDER) && !js_undef(p, "bo")) put_one(out, "bo");
    if (HAS(EP_SHOWMINIMAP) && !js_undef(p, "smm")) put_one(out, "smm");
    {
        const double anchor = pi_or(p, "ant", 0);
        if (IS("MAP_ANCHOR") && num_truthy(anchor)) put_num(out, "ant", anchor);
    }
    if (IS("TELEPORTER")) {
        const double channel = pi_or(p, "ch", 0), node = pi_or(p, "n", 0);
        if (channel > 0) put_num(out, "ch", channel);
        if (node > 0) put_num(out, "n", node);
        if (!js_null(p, "m")) put_one(out, "m");
        if (!js_null(p, "tcp")) put_one(out, "tcp");
    }
    if (!HAS(EP_COMPLEX) && !HAS(EP_TOOL) && !js_undef(p, "ab")) put_one(out, "ab");
    if (HAS(EP_EDITFOG) && !js_undef(p, "nf")) put_one(out, "nf");
    if (HAS(EP_INTERACT)) {
        if (!js_null(p, "nm")) put_one(out, "nm");
        const double method = pi_or(p, "md", 0);
        if (num_truthy(method)) put_num(out, "md", method);
        if (HAS(EP_CANUNDO) && !js_null(p, "cl")) {
            put_one(out, "cl");
            if (!js_null(p, "stc")) put_one(out, "stc");
        }
    }
    {
        const double node_id = pi_or(p, "nid", 0);
        if (IS("PATH_NODE") && num_truthy(node_id)) put_num(out, "nid", node_id);
    }
    if (IS("BOT_PATH_NODE")) { // v10 (E10:7409-7412, 8038-8043)
        put_nz(out, "bng", pi_or(p, "bng", 0));
        put_nz(out, "bn", pi_or(p, "bn", 0));
        put_nz(out, "bnn", js_null(p, "bnn") ? 1 : pi_or(p, "bnn", 0));
        put_num(out, "bnr", pi_or(p, "bnr", 0) == 1 ? 1 : 0);
    }
    // the object-level (legacy single-action) action, used for the sound block below
    const double obj_action = pi_or(p, "ta", 0);
    if (trigger || ai) {
        if (!js_null(p, "ct") && ai) put_one(out, "ct");
        if (num_truthy(event)) put_num(out, "te", event);
        const double const_e = pi_or(p, "tce", 0);
        if (num_truthy(const_e)) put_num(out, "tce", const_e);
        if (!js_null(p, "sfd")) put_one(out, "sfd");
        const double chance = pi_or(p, "cha", 0);
        if (num_truthy(chance)) put_num(out, "cha", chance);
        // trigActions: tas entries (at most triggerActionLimit), else the legacy object-level action
        cJSON *tas = cJSON_CreateArray();
        const int limit = (int) data_num("limits", "triggerActionLimit", 5);
        const cJSON *list = G(p, "tas");
        int count = 0;
        OiAction a;
        if (cJSON_IsArray(list)) {
            const cJSON *it;
            cJSON_ArrayForEach(it, list) {
                if (!js_truthy(it) || count >= limit) continue;
                oi_action_load(&a, cJSON_IsObject(it) ? it : NULL);
                cJSON_AddItemToArray(tas, oi_action_serialize(&a));
                count++;
            }
        } else {
            oi_action_load(&a, p);
            cJSON_AddItemToArray(tas, oi_action_serialize(&a));
        }
        put(out, "tas", tas);
    }
    if (HAS(EP_HASTEXT)) {
        const int lb = IS("LEADERBOARD");
        if (!lb) {
            const cJSON *st = G(p, "st");
            const char *raw = js_truthy(st) ? js_str(st, buf, sizeof(buf)) : "";
            char *text = text_filter(raw ? raw : "", K_TEXT_OK_BS);
            put(out, "st", cJSON_CreateString(text ? text : ""));
            free(text);
        }
        const cJSON *sc = G(p, "sc"), *so = G(p, "so"), *sb = G(p, "sb");
        if (!js_nullish(sc) && !(cJSON_IsString(sc) && !strcmp(sc->valuestring, "#000000"))) put_dup(out, "sc", sc);
        if (!js_nullish(so) && !(cJSON_IsString(so) && !strcmp(so->valuestring, "#111111"))) put_dup(out, "so", so);
        if (!js_nullish(sb) && !(cJSON_IsString(sb) && !strcmp(sb->valuestring, "#FFFFFF"))) put_dup(out, "sb", sb);
        const double othick = pi_or(p, "sot", 1);
        if (othick != 1) put_num(out, "sot", othick);
        if (!js_undef(p, "bv")) put_one(out, "bv");
        const double fsize = pi_or(p, "sf", 10);
        if (num_truthy(fsize) && fsize != 10) put_num(out, "sf", fsize);
        if (!js_null(p, "nsf")) put_one(out, "nsf");
        const double align = pi_or(p, "sa", 0);
        if (!lb && num_truthy(align) && align != 0) put_num(out, "sa", align);
    }
    if (IS("LEADERBOARD")) {
        const double type = pi_or(p, "ldt", 0), order = pi_or(p, "ldo", 0), results = pi_or(p, "ldr", 25);
        if (num_truthy(type)) put_num(out, "ldt", type);
        const cJSON *ldc = G(p, "ldc");
        const char *raw = js_truthy(ldc) ? js_str(ldc, buf, sizeof(buf)) : "";
        char *cust = text_filter(raw ? raw : "", K_TEXT_OK_BS);
        if (cust && cust[0]) put(out, "ldc", cJSON_CreateString(cust));
        free(cust);
        if (num_truthy(order)) put_num(out, "ldo", order);
        if (num_truthy(results)) put_num(out, "ldr", results);
    }
    if (num_truthy(health) || can_dest) {
        const double respawn = pf_or(p, "rt", 0);
        if (num_truthy(respawn)) put_num(out, "rt", respawn);
        if (!js_null(p, "rr")) put_one(out, "rr");
        if (!js_null(p, "sd")) {
            const double chance = pf_or(p, "rsc", 0);
            if (num_truthy(chance)) put_num(out, "rsc", js_round(chance, 2));
            put_one(out, "sd");
        }
    }
    {
        const double iface = pi_or(p, "in", 0), target = pi_or(p, "tin", 0);
        if (HAS(EP_CANINTERFACE) && num_truthy(iface)) put_num(out, "in", iface);
        if (HAS(EP_CANTARGETINTERFACE) && num_truthy(target)) put_num(out, "tin", target);
    }
    if (IS("DEPOSIT_BOX")) {
        const double wd = js_null(p, "wd") ? 1000 : js_parse_int(G(p, "wd")), dp = js_null(p, "dp") ? 1000 : js_parse_int(G(p, "dp"));
        if (wd != 1000) put_num(out, "wd", wd);
        if (dp != 1000) put_num(out, "dp", dp);
    }
    {
        const double obi = pi_or(p, "obi", 0), obl = pi_or(p, "obl", 0);
        if (HAS(EP_CANSETOBJECTIVE) && num_truthy(obi)) put_num(out, "obi", obi);
        if (HAS(EP_OBJECTIVELINK) && num_truthy(obl)) put_num(out, "obl", obl);
    }
    if (IS("CAMERA")) {
        const double cam_type = pi_or(p, "camt", 0);
        if (num_truthy(cam_type)) put_num(out, "camt", cam_type);
        if (!js_null(p, "frc")) put_one(out, "frc");
        const char *fp = id_string(G(p, "fp"), "0123456789,");
        if (fp) put(out, "fp", cJSON_CreateString(fp));
    }
    if (IS("LIQUID")) {
        const double wh = pf_or(p, "wh", 0.5), ws = pf_or(p, "ws", 10), tx = pf_or(p, "tx", 10), tz = pf_or(p, "tz", 10);
        if (wh != 0.5) put_num(out, "wh", wh);
        if (ws != 10) put_num(out, "ws", ws);
        if (tx != 10) put_num(out, "tx", js_parse_int_d(tx));
        if (tz != 10) put_num(out, "tz", js_parse_int_d(tz));
    }
    const double theta_length = js_null(p, "thl") ? 360 : pf_or(p, "thl", 0.1);
    const double theta_start = js_null(p, "ths") ? 0 : pf_or(p, "ths", 1); // (a "ths" of 0 loads as 1, as in v7)
    if (IS("LIGHT_CONE")) {
        if (!js_null(p, "fc")) put_one(out, "fc");
        if (theta_length != 360) put_num(out, "thl", js_parse_int_d(theta_length));
    }
    if (IS("CYLINDER")) {
        if (theta_length != 360) put_num(out, "thl", theta_length);
        if (theta_start != 0) put_num(out, "ths", theta_start);
    }
    if (IS("SOUND_EMITTER") || ((trigger || ai) && (obj_action == 14 || obj_action == 34 || obj_action == 54))) {
        const double asset_sound = pi_or(p, "esa", 0);
        if (num_truthy(asset_sound)) put_num(out, "esa", asset_sound);
        else put_num(out, "es", pi_or(p, "es", 0));
        put_num(out, "ev", js_round(pf_or(p, "ev", 1), 2));
        put_num(out, "er", js_round(pf_or(p, "er", 1), 1));
        if (IS("SOUND_EMITTER")) {
            put_num(out, "ed", js_parse_int_d(pf_or(p, "ed", 0)));
            put_num(out, "edm", pi_or(p, "edm", 0));
            put_num(out, "ero", js_round(pf_or(p, "ero", 0), 2));
        } else {
            const double from = pi_or(p, "ef", 0);
            if (num_truthy(from)) put_num(out, "ef", from);
        }
    }
    const double msize = js_round(pf_or(p, "ms", pf->scale), 2);
    if (HAS(EP_GEN) && (ai || msize != (double) pf->scale)) put_num(out, "ms", js_round(msize, 2));
    const double spin = pf_or(p, "spn", 0);
    if ((HAS(EP_GEN) || HAS(EP_HASSPIN)) && spin != 0) {
        put_num(out, "spn", spin);
        if ((HAS(EP_GEN) || HAS(EP_HASSPINDIR)) && num_truthy(pi_or(p, "spa", 0))) put_one(out, "spa");
    }
    const int custom_asset = IS("CUSTOM_ASSET");
    if ((custom_asset || trigger || ai) && aid) {
        put_dup(out, "aid", aid);
        // v10 mapProps (E10:8258-8262): the map-slot channels, kept while the asset is set
        const cJSON *mp = G(p, "mp");
        if (cJSON_IsObject(mp) && cJSON_GetArraySize(mp) > 0) put_dup(out, "mp", mp);
    }
    if (custom_asset || trigger) {
        if (!js_null(p, "cst")) put_one(out, "cst");
        if (!js_null(p, "rcv")) put_one(out, "rcv");
        if (!js_null(p, "fcr")) put_one(out, "fcr");
    }
    if (custom_asset || trigger || ai) {
        const double anim_speed = pf_or(p, "as", 1);
        if (anim_speed != 1) put_num(out, "as", js_round(anim_speed, 2));
        const cJSON *ast = G(p, "ast");
        if (cJSON_IsString(ast) && ast->valuestring[0] && text_valid(ast->valuestring, K_TEXT_OK)) put_dup(out, "ast", ast);
    }
    if (IS("SPAWN_POINT") && js_truthy(G(p, "cp"))) put_one(out, "cp");
    if (IS("SPAWN_POINT") && js_truthy(G(p, "to"))) put_one(out, "to");
    {
        double spread = pf_or(p, "ssp", 0);
        if (!num_truthy(spread)) spread = 0;
        if ((IS("SPAWN_POINT") || IS("SERVER_NODE")) && num_truthy(spread)) put_num(out, "ssp", js_round(spread, 1));
    }
    if (!js_undef(p, "txs")) put_one(out, "txs");
    {
        const double item = pi_or(p, "skin", 0);
        if (IS("SHOWCASE") && num_truthy(item)) put_num(out, "skin", item);
    }
    if (coll_tris) put(out, "cmt", coll_tris); // v10.0.5 serialize: cmt = this.collTris (editor-10.0.5.js:8318-8320)
    const double intensity = pf_or(p, "li", 1);
    const double distance = js_null(p, "lds") ? 50 : pf_or(p, "lds", 0);
    const double decay = pf_or(p, "ld", 1);
    if (HAS(EP_LIGHT)) {
        if (intensity != 1) put_num(out, "li", js_round(intensity, 1));
        if (num_truthy(distance)) put_num(out, "lds", js_round(distance, 1));
        if (decay != 1) put_num(out, "ld", js_round(decay, 1));
    }
    if (IS("CYLINDER")) {
        if (!js_null(p, "opn")) put_one(out, "opn");
        const double top = js_null(p, "rtp") ? 1 : pf_or(p, "rtp", 0), bottom = js_null(p, "rbt") ? 1 : pf_or(p, "rbt", 0);
        if (top != 1) put_num(out, "rtp", js_round(top, 2));
        if (bottom != 1) put_num(out, "rbt", js_round(bottom, 2));
    }
    {
        const double sides = pi_or(p, "sds", 12);
        if ((IS("LIGHT_CONE") || IS("CYLINDER")) && sides != 12) put_num(out, "sds", sides);
    }
    if (ai) {
        // value, default; written when != default (as parseInt / parseFloat / rounded)
        const double roam_time = pi_or(p, "rmt", 1000), roam_rad = pi_or(p, "rmr", 0), chase = pi_or(p, "aic", 10);
        const double turn = js_null(p, "tsd") ? 1 : pf_or(p, "tsd", 0), speed = js_null(p, "spd") ? 1 : pf_or(p, "spd", 0);
        const double proj_s = js_null(p, "prs") ? 1 : js_parse_float(G(p, "prs")), proj_g = pf_or(p, "prg", 0);
        const double proj_sc = pf_or(p, "prsc", 1), proj_ms = pf_or(p, "prms", 1), proj_spd = pf_or(p, "pjs", 1), proj_dmg = pi_or(p, "pjd", 100);
        const double vis = pi_or(p, "vd", 120);
        if (roam_time != 1000) put_num(out, "rmt", num_truthy(roam_time) ? roam_time : 1000);
        if (num_truthy(roam_rad)) put_num(out, "rmr", roam_rad);
        if (chase != 10) put_num(out, "aic", num_truthy(chase) ? chase : 10);
        if (turn != 1) put_num(out, "tsd", js_round(turn, 1));
        if (speed != 1) put_num(out, "spd", js_round(speed, 1));
        if (proj_s != 1) put_num(out, "prs", js_round(proj_s, 2));
        if (num_truthy(proj_g)) put_num(out, "prg", js_round(proj_g, 2));
        if (!js_null(p, "prgl")) put_one(out, "prgl");
        if (!js_null(p, "prgt")) put_one(out, "prgt");
        if (proj_sc != 1) put_num(out, "prsc", js_round(proj_sc, 2));
        if (proj_ms != 1) put_num(out, "prms", js_round(proj_ms, 2));
        if (proj_spd != 1) put_num(out, "pjs", num_truthy(proj_spd) ? proj_spd : 1);
        if (proj_dmg != 100) put_num(out, "pjd", num_truthy(proj_dmg) ? proj_dmg : 100);
        if (vis != 120) put_num(out, "vd", vis);
        static const char *const sounds[7] = {"idl", "dth", "atk", "atkh", "mel", "melh", "agr"};
        for (int k = 0; k < 7; k++) {
            const double v = pi_or(p, sounds[k], 0);
            if (num_truthy(v)) put_num(out, sounds[k], v);
        }
        const double frate = pi_or(p, "fr", 2000), aicur = pf_or(p, "aac", 0), shb = pi_or(p, "shb", 400), sha = pi_or(p, "sha", 0);
        const double shdy = pi_or(p, "shdy", 0), mldy = pi_or(p, "mldy", 400), mls = pf_or(p, "mls", 0), ss = pf_or(p, "ss", 0);
        const double crd = pf_or(p, "crd", 0), hrate = pi_or(p, "hr", 800), mlr = pi_or(p, "mlr", 10), mld = pi_or(p, "mld", 10);
        const double grv = js_null(p, "grv") ? 1 : pf_or(p, "grv", 0);
        if (frate != 2000) put_num(out, "fr", num_truthy(frate) ? frate : 0);
        if (num_truthy(aicur)) put_num(out, "aac", aicur);
        if (shb != 400) put_num(out, "shb", num_truthy(shb) ? shb : 400);
        if (num_truthy(sha)) put_num(out, "sha", sha);
        if (!js_null(p, "shof")) put_one(out, "shof");
        if (num_truthy(shdy)) put_num(out, "shdy", shdy);
        if (mldy != 400) put_num(out, "mldy", num_truthy(mldy) ? mldy : 0);
        if (num_truthy(mls)) put_num(out, "mls", mls);
        if (num_truthy(ss)) put_num(out, "ss", ss);
        if (num_truthy(crd)) put_num(out, "crd", crd);
        if (hrate != 800) put_num(out, "hr", num_truthy(hrate) ? hrate : 800);
        if (mlr != 10) put_num(out, "mlr", num_truthy(mlr) ? mlr : 10);
        if (mld != 10) put_num(out, "mld", num_truthy(mld) ? mld : 10);
        if (grv != 1) put_num(out, "grv", js_round(grv, 2));
        const char *rew = id_string(G(p, "rew"), "0123456789,-");
        if (rew) put(out, "rew", cJSON_CreateString(rew));
        const double rewm = pi_or(p, "rewm", 0), rewr = pi_or(p, "rewr", 0), rewn = pi_or(p, "rewn", 0);
        if (rewm > 0) put_num(out, "rewm", rewm);
        if (rewr > 0) put_num(out, "rewr", rewr);
        if (rewn > 0) put_num(out, "rewn", rewn);
        const double bh = pi_or(p, "bh", 0), pht = pi_or(p, "pht", 0);
        if (num_truthy(bh)) put_num(out, "bh", bh);
        if (num_truthy(pht)) put_num(out, "pht", pht);
        const char *phi = id_string(G(p, "phi"), "0123456789,");
        if (phi) put(out, "phi", cJSON_CreateString(phi));
        if (!js_null(p, "xv")) put_one(out, "xv");
        if (!js_null(p, "tAi")) put_one(out, "tAi");
        if (!js_null(p, "tPl")) put_one(out, "tPl");
        if (ai && pi_or(p, "boss", 0) == 1) put_one(out, "boss"); // v10 isBoss (E10:7350, 8459)
        if (js_truthy(G(p, "aif"))) put_one(out, "aif");
        if (!js_null(p, "cs")) put_one(out, "cs");
        if (!js_null(p, "igos")) put_one(out, "igos");
        if (!js_null(p, "dos")) put_one(out, "dos");
        if (!js_null(p, "ivos")) put_one(out, "ivos");
        if (!js_null(p, "cnh")) put_one(out, "cnh");
        if (ai && !js_null(p, "cnch")) put_one(out, "cnch"); // v10 canContactHit (E10:7320, 8480)
        const cJSON *ain = G(p, "ain");
        if (cJSON_IsString(ain) && ain->valuestring[0] && text_valid(ain->valuestring, K_TEXT_OK)) put_dup(out, "ain", ain);
        const char *pra = id_string(G(p, "pra"), K_ASSET_ID_V);
        if (pra) put(out, "pra", cJSON_CreateString(pra));
        const double ail = pi_or(p, "ail", 0), ains = pf_or(p, "ains", 1), myo = pf_or(p, "myo", 0), mro = pf_or(p, "mro", 0);
        if (num_truthy(ail)) put_num(out, "ail", ail);
        if (ains != 1) put_num(out, "ains", ains);
        if (num_truthy(myo)) put_num(out, "myo", js_parse_int_d(myo));
        if (num_truthy(mro)) put_num(out, "mro", js_parse_int_d(mro));
        if (!js_null(p, "exp")) put_one(out, "exp");
        const double se = js_null(p, "se") ? -1 : js_parse_int(G(p, "se")), he = js_null(p, "he") ? -1 : js_parse_int(G(p, "he"));
        if (se >= 0) put_num(out, "se", se);
        if (he >= 0) put_num(out, "he", he);
        const double bbs = pf_or(p, "bbS", 0), bbd = pf_or(p, "bbD", 0);
        if (num_truthy(bbs)) put_num(out, "bbS", js_round(bbs, 2));
        if (num_truthy(bbd)) put_num(out, "bbD", js_round(bbd, 2));
    }
    if (bot) {
        const cJSON *btn = G(p, "btn");
        if (cJSON_IsString(btn) && btn->valuestring[0] && text_valid(btn->valuestring, K_TEXT_OK)) put_dup(out, "btn", btn);
        const double btc = pi_null(p, "btc", 0);
        put_num(out, "btc", num_truthy(btc) ? btc : 0);
        const double btl = pi_null(p, "btl", 0), btt = pi_null(p, "btt", 0);
        if (num_truthy(btl)) put_num(out, "btl", btl);
        if (num_truthy(btt)) put_num(out, "btt", btt);
        // the multipliers: (p.k == null ? 1 : parseFloat(p.k || 1)), written when != 1 (speedMlt "btspm" is
        // read but never written by v7; BOT_SPAWNER is localOnly, doc/v7/editor.md 1.13 keeps it)
        static const char *const mlt[4] = {"bthm", "btdm", "btrm", "btsm"};
        for (int k = 0; k < 4; k++) {
            const double v = js_null(p, mlt[k]) ? 1 : pf_or(p, mlt[k], 1);
            if (v != 1) put_num(out, mlt[k], num_truthy(v) ? v : 1);
        }
        // switches: present (default off) and absent (default on) flags, in v7's order
        static const char *const sw[20] = {"btfc", "bttc", "bttt", "btfcr", "!btat", "!btft", "!btwt", "btmt", "btsh", "!btcr",
                                           "!btsd", "!btrd", "btny", "btno", "btjm", "btlm", "!btvc", "btgm", "!btr", "btrs"};
        for (int k = 0; k < 20; k++) {
            const char *key = sw[k][0] == '!' ? sw[k] + 1 : sw[k];
            if (!js_null(p, key)) put_one(out, key); // either way the key is written when present
        }
        if (!js_null(p, "btrc")) put_one(out, "btrc");
        static const char *const cosmetics[12] = {"btcpr", "btcd", "btcf", "btcht", "btch", "btcbk", "btcb", "btcw", "btcwr", "btcs", "btcp", "btcm"};
        for (int k = 0; k < 12; k++) {
            double v = js_null(p, cosmetics[k]) ? -1 : pi_or(p, cosmetics[k], 0);
            if (!num_truthy(v)) v = -1; // parseInt(..) || -1
            if (v != -1) put_num(out, cosmetics[k], v);
        }
    }
    if (HAS(EP_ALTERHITBOX)) {
        const double hbh = pf_or(p, "hbh", 1), hbw = pf_or(p, "hbw", 1);
        if (hbh != 1) put_num(out, "hbh", js_round(hbh, 1));
        if (hbw != 1) put_num(out, "hbw", js_round(hbw, 1));
    }
    if (HAS(EP_CANTRANS) && !js_null(p, "ft")) put_one(out, "ft");
    {
        const double rs = js_null(p, "rs") ? -1 : pi_or(p, "rs", 0);
        if (HAS(EP_EDITRENDERSIDE) && rs >= 0) put_num(out, "rs", num_truthy(rs) ? rs : 0);
        const double alt = pf_or(p, "alt", 0);
        if (HAS(EP_EDITALPHATEST) && num_truthy(alt)) put_num(out, "alt", js_round(alt, 2));
    }
    if (!js_null(p, "ec")) put_one(out, "ec");
    const int directional = IS("DIRECTIONAL_LIGHT"), spot = IS("SPOT_LIGHT");
    if (directional || spot) {
        // lightTarget = p.lt || [0, 0, 0], written mapped element by element
        const cJSON *lt = G(p, "lt");
        cJSON *la = cJSON_CreateArray();
        const int n = js_truthy(lt) && cJSON_IsArray(lt) ? cJSON_GetArraySize(lt) : 3;
        for (int k = 0; k < n; k++) {
            const double v = js_truthy(lt) ? js_parse_float(cJSON_GetArrayItem(lt, k)) : 0;
            cJSON_AddItemToArray(la, cJSON_CreateNumber(js_round(num_truthy(v) ? v : 0, 2)));
        }
        put(out, "lt", la);
        if (intensity != 1) put_num(out, "li", js_round(intensity, 1));
    }
    if (spot) {
        if (num_truthy(distance)) put_num(out, "lds", js_round(distance, 1));
        if (decay != 1) put_num(out, "ld", js_round(decay, 1));
        const double penumbra = pf_or(p, "lp", 0);
        if (num_truthy(penumbra)) put_num(out, "lp", js_round(penumbra, 1));
        // "la" is written against 15 but loads with a default of 45 (doc/v7/editor.md 1.13: match on write)
        const double angle = pi_or(p, "la", 45);
        if (angle != 15) put_num(out, "la", angle);
    }
    if (IS("RECTAREA_LIGHT") && intensity != 1) put_num(out, "li", js_round(intensity, 1));
    {
        const double sty = pi_or(p, "sty", 0), krs = pi_or(p, "krs", 0);
        if (IS("PRIZE_WHEEL") && num_truthy(sty)) put_num(out, "sty", sty);
        if (IS("KR_ZONE") && num_truthy(krs)) put_num(out, "krs", krs);
    }
    if (HAS(EP_CANSCRIPT)) {
        // intentional difference from v7.1.4: v7 writes parseInt(this.sci) (undefined, so null);
        // the script id itself is written (doc/v7/editor.md 1.13)
        const double sci = js_parse_int(G(p, "sci"));
        if (num_truthy(sci) && isfinite(sci)) put_num(out, "sci", sci);
    }
    {
        const double ten = pi_or(p, "ten", 0);
        if (num_truthy(ten)) put_num(out, "ten", ten);
    }
    if (IS("CAMERA_POSITION") && js_truthy(G(p, "cstc"))) put_one(out, "cstc");
    if (HAS(EP_INTERACTMSG) || trigger) {
        const cJSON *inm = G(p, "inm");
        const char *raw = js_truthy(inm) ? js_str(inm, buf, sizeof(buf)) : "";
        char *msg = text_filter(raw ? raw : "", K_TEXT_OK_BS);
        if (msg && msg[0]) put(out, "inm", cJSON_CreateString(msg));
        free(msg);
    }
    if (pf->variants && num_truthy(tex_var)) put_num(out, "tv", tex_var);
    if (HAS(EP_CANSPRITE) && !js_null(p, "msp")) put_one(out, "msp");
    {
        const double sat = pi_or(p, "sat", 0);
        if (HAS(EP_STEPSRC) && num_truthy(sat)) put_num(out, "sat", sat);
    }
    if (HAS(EP_CANCOMPLEXCOLL) && !js_null(p, "cpx")) put_one(out, "cpx");
    if (IS("ENVIRONMENT_ZONE")) {
        const cJSON *sky = cJSON_GetObjectItemCaseSensitive(editor_data_get("zoneColors"), "sky");
        static const char *const dome[3] = {"skyDomeCol0", "skyDomeCol1", "skyDomeCol2"};
        put_nz(out, "ambInd", js_null(p, "ambInd") ? 1 : pi_or(p, "ambInd", 0));
        put_nz(out, "ambIndC", pi_or(p, "ambIndC", 0));
        put_nz(out, "ambFadeTime", pi_or(p, "ambFadeTime", 0)); // v10 (E10:7472, 8720)
        put(out, "skyDome", cJSON_CreateBool(js_truthy(G(p, "skyDome"))));
        for (int k = 0; k < 3; k++) {
            const cJSON *c = G(p, dome[k]);
            put(out, dome[k], is_hex_color(c) ? cJSON_Duplicate(c, 1) : cJSON_Duplicate(cJSON_GetArrayItem(sky, k), 1));
        }
        // intentional difference from v7.1.4: v7 tests isHexColor on the output object (never set), so
        // it always writes "#000000"; the zone's own sky emissive is written
        put(out, "skyDomeEmis", is_hex_color(G(p, "skyDomeEmis")) ? cJSON_Duplicate(G(p, "skyDomeEmis"), 1) : cJSON_CreateString("#000000"));
        const cJSON *emis_tex = id_value(G(p, "skyDomeEmisTex"), K_ASSET_ID);
        put(out, "skyDomeEmisTex", emis_tex ? cJSON_Duplicate(emis_tex, 1) : cJSON_CreateNumber(0));
        put(out, "skyDomeTex", cJSON_CreateBool(js_truthy(G(p, "skyDomeTex"))));
        const cJSON *tex_a = id_value(G(p, "skyDomeTexA"), K_ASSET_ID);
        put(out, "skyDomeTexA", tex_a ? cJSON_Duplicate(tex_a, 1) : cJSON_CreateNumber(0));
        put_nz(out, "skyDomeMovD", pf_or(p, "skyDomeMovD", 0));
        put_nz(out, "skyDomeMovT", pf_or(p, "skyDomeMovT", 0));
        put_nz(out, "shadowR", pi_null(p, "shadowR", data_num("defaults", "shadowRes", 1024)));
        put_nz(out, "shadowD", pi_null(p, "shadowD", data_num("defaults", "shadowDst", 1200)));
        put(out, "ambientC", is_hex_color(G(p, "ambientC")) ? cJSON_Duplicate(G(p, "ambientC"), 1) : cJSON_CreateString("#97a0a8"));
        put_nz(out, "ambientI", pf_null(p, "ambientI", data_num("defaults", "ambIntensity", 1)));
        put(out, "light", is_hex_color(G(p, "light")) ? cJSON_Duplicate(G(p, "light"), 1) : cJSON_CreateString("#f2f8fc"));
        put_nz(out, "lightD", pi_null(p, "lightD", data_num("defaults", "lightDistance", 500)));
        put_nz(out, "lightI", pf_null(p, "lightI", data_num("defaults", "lightIntensity", 1.3)));
        put_nz(out, "sunAngX", pi_null(p, "sunAngX", data_num("defaults", "sunAngleX", 90)));
        put_nz(out, "sunAngY", pi_null(p, "sunAngY", data_num("defaults", "sunAngleY", 54)));
        put(out, "sky", is_hex_color(G(p, "sky")) ? cJSON_Duplicate(G(p, "sky"), 1) : cJSON_CreateString("#dce8ed"));
        put(out, "fogC", is_hex_color(G(p, "fogC")) ? cJSON_Duplicate(G(p, "fogC"), 1) : cJSON_CreateString("#8d9aa0"));
        put_nz(out, "fogD", pi_null(p, "fogD", 2000));
        put(out, "correctLights", cJSON_CreateBool(js_truthy(G(p, "correctLights"))));
        put_nz(out, "toneMapping", pf_or(p, "toneMapping", 0));
        put_nz(out, "toneMappingExposure", pf_null(p, "toneMappingExposure", 1));
        put_nz(out, "outputEncoding", pi_or(p, "outputEncoding", 0));
    }
    {
        const double perk = pi_or(p, "zp", 0);
        if (IS("PERK_MACHINE") && num_truthy(perk)) put_num(out, "zp", perk);
    }
    if (IS("SERVER_NODE")) {
        const cJSON *nt = G(p, "nt");
        if (cJSON_IsString(nt) && nt->valuestring[0] && text_valid(nt->valuestring, K_TEXT_OK)) put_dup(out, "nt", nt);
    }
    // Krunker v10 (E10:7443-7457 constructor, 8750-8788 serialize): crate types, entities, archetype parts
    if (IS("WEAPONBOX") || IS("SCRAPCRATE")) {
        const double crt = pi_or(p, "crt", 0);
        if (num_truthy(crt)) put_num(out, "crt", crt);
    }
    if (IS("ENTITY")) {
        put_nz(out, "eid", pi_or(p, "eid", 0));
        put_nz(out, "ar", pi_or(p, "ar", 0));
        const cJSON *tag = G(p, "tag");
        char tbuf[64];
        const char *t = tag ? js_str(tag, tbuf, sizeof(tbuf)) : NULL;
        int tag_ok = t && *t;
        for (const char *c = t; tag_ok && *c; c++) tag_ok = isalnum((unsigned char) *c) || *c == '_' || *c == '-';
        if (tag_ok) {
            char cut[33];
            snprintf(cut, sizeof(cut), "%s", t);
            put(out, "tag", cJSON_CreateString(cut));
        }
        if (!js_null(p, "as") && !num_truthy(pi_or(p, "as", 0))) put_num(out, "as", 0); // autoSpawn off
        const cJSON *sp = G(p, "sp");
        if (cJSON_IsObject(sp) && sp->child) put_dup(out, "sp", sp);
    }
    {
        const cJSON *pn = G(p, "pn");
        char pbuf[32];
        const char *n = pn ? js_str(pn, pbuf, sizeof(pbuf)) : NULL;
        int ok = n && *n && strlen(n) <= 12;
        for (const char *c = n; ok && *c; c++) ok = isalnum((unsigned char) *c) || *c == '_';
        if (ok) put(out, "pn", cJSON_CreateString(n));
        if (!js_null(p, "sfz")) put_one(out, "sfz");
    }

    // KrunkNative: keys ObjectInstance does not know are kept (see the header)
    const cJSON *it;
    cJSON_ArrayForEach(it, p) {
        if (!it->string || ed_model_known_key(it->string) || cJSON_GetObjectItemCaseSensitive(out, it->string)) continue;
        cJSON_AddItemToObject(out, it->string, cJSON_Duplicate(it, 1));
    }
    return out;
}

#undef HAS
#undef IS

// ---------------------------------------------------------------------------
// complex collision: the generated geometry of a "cpx" object in world space (getMapExport f967 /
// f968), as three.js r150 builds it: Float32 buffer geometry, non-indexed, every vertex through
// the object's matrixWorld (position + quaternion of the bounding mesh, the geometry offset up by
// half the height), rounded to integers (UTILS.toRoundArray).

typedef struct {
    float *v;   // xyz per vertex, triangles in order
    int n, cap; // vertices
} Tris;

static void tris_push(Tris *t, const double x, const double y, const double z) {
    if (t->n == t->cap) {
        const int cap = t->cap ? t->cap * 2 : 96;
        float *nv = realloc(t->v, (size_t) cap * 3 * sizeof(float));
        if (!nv) return;
        t->v = nv;
        t->cap = cap;
    }
    t->v[t->n * 3] = (float) x; // Float32BufferAttribute
    t->v[t->n * 3 + 1] = (float) y;
    t->v[t->n * 3 + 2] = (float) z;
    t->n++;
}

// geometry.applyMatrix4 for the rotate / scale / translate helpers (doubles in, Float32 stored)
static void tris_rotate(Tris *t, const int from, const int axis, const double a) {
    const double c = cos(a), s = sin(a);
    for (int i = from; i < t->n; i++) {
        float *v = &t->v[i * 3];
        const double x = v[0], y = v[1], z = v[2];
        if (axis == 0) { v[1] = (float) (c * y - s * z); v[2] = (float) (s * y + c * z); }
        else if (axis == 1) { v[0] = (float) (c * x + s * z); v[2] = (float) (-s * x + c * z); }
        else { v[0] = (float) (c * x - s * y); v[1] = (float) (s * x + c * y); }
    }
}

static void tris_scale_translate(Tris *t, const int from, const double sx, const double sy, const double sz, const double tx, const double ty, const double tz) {
    for (int i = from; i < t->n; i++) {
        float *v = &t->v[i * 3];
        v[0] = (float) ((double) v[0] * sx + tx);
        v[1] = (float) ((double) v[1] * sy + ty);
        v[2] = (float) ((double) v[2] * sz + tz);
    }
}

// indexed (positions, indices) -> non-indexed triangles (toNonIndexed)
static void tris_indexed(Tris *t, const float *pos, const int *idx, const int count) {
    for (int i = 0; i < count; i++) tris_push(t, pos[idx[i] * 3], pos[idx[i] * 3 + 1], pos[idx[i] * 3 + 2]);
}

// new THREE.PlaneGeometry(1, 1, 1, 1), non-indexed: (a, b, d), (b, c, d)
static void plane_unit(Tris *t) {
    static const float pos[12] = {-0.5f, 0.5f, 0, 0.5f, 0.5f, 0, -0.5f, -0.5f, 0, 0.5f, -0.5f, 0};
    static const int idx[6] = {0, 2, 1, 2, 3, 1};
    tris_indexed(t, pos, idx, 6);
}

// GEOS.generateCube(faces, w, h, d) + prefabs generateCube: the unit faces, merged, scaled, centred
static void geo_cube(Tris *t, const unsigned char faces[6], const double w, const double h, const double d) {
    const int start = t->n;
    for (int f = 0; f < 6; f++) {
        if (!faces[f]) continue;
        const int from = t->n;
        plane_unit(t);
        switch (f) {
            case 0: tris_rotate(t, from, 1, M_PI / 2); tris_scale_translate(t, from, 1, 1, 1, 0.5, 0.5, 0); break;
            case 1: tris_rotate(t, from, 1, -M_PI / 2); tris_scale_translate(t, from, 1, 1, 1, -0.5, 0.5, 0); break;
            case 2: tris_rotate(t, from, 0, -M_PI / 2); tris_scale_translate(t, from, 1, 1, 1, 0, 1, 0); break;
            case 3: tris_rotate(t, from, 0, M_PI / 2); break;
            case 4: tris_scale_translate(t, from, 1, 1, 1, 0, 0.5, 0.5); break;
            default: tris_rotate(t, from, 1, M_PI); tris_scale_translate(t, from, 1, 1, 1, 0, 0.5, -0.5); break;
        }
    }
    tris_scale_translate(t, start, w, h, d, 0, 0, 0);
    tris_scale_translate(t, start, 1, 1, 1, 0, -h / 2, 0);
}

// THREE.CylinderGeometry(rTop, rBot, 1, sides, 1, open, thetaStart, thetaLength) scaled (r, h, r)
static void geo_cylinder(Tris *t, const double r_top, const double r_bot, const int sides, const int open, const double t0, const double tl,
                         const double r, const double h) {
    const int nv = (sides + 1) * 2 + 2 * (sides + sides + 1);
    float *pos = calloc((size_t) nv * 3, sizeof(float));
    int *idx = calloc((size_t) sides * 12 + 6, sizeof(int));
    if (!pos || !idx) {
        free(pos);
        free(idx);
        return;
    }
    int index = 0, ni = 0;
    for (int y = 0; y <= 1; y++) {
        const double radius = y * (r_bot - r_top) + r_top;
        for (int x = 0; x <= sides; x++) {
            const double theta = (double) x / sides * tl + t0;
            pos[index * 3] = (float) (radius * sin(theta));
            pos[index * 3 + 1] = (float) (-y * 1.0 + 0.5);
            pos[index * 3 + 2] = (float) (radius * cos(theta));
            index++;
        }
    }
    for (int x = 0; x < sides; x++) {
        const int a = x, b = (sides + 1) + x, c = (sides + 1) + x + 1, d = x + 1;
        idx[ni++] = a; idx[ni++] = b; idx[ni++] = d;
        idx[ni++] = b; idx[ni++] = c; idx[ni++] = d;
    }
    for (int cap = 0; cap < 2 && !open; cap++) {
        const int top = cap == 0;
        const double radius = top ? r_top : r_bot;
        if (!(radius > 0)) continue;
        const double sign = top ? 1 : -1;
        const int center = index;
        for (int x = 1; x <= sides; x++) {
            pos[index * 3] = 0;
            pos[index * 3 + 1] = (float) (0.5 * sign);
            pos[index * 3 + 2] = 0;
            index++;
        }
        const int end = index;
        for (int x = 0; x <= sides; x++) {
            const double theta = (double) x / sides * tl + t0;
            pos[index * 3] = (float) (radius * sin(theta));
            pos[index * 3 + 1] = (float) (0.5 * sign);
            pos[index * 3 + 2] = (float) (radius * cos(theta));
            index++;
        }
        for (int x = 0; x < sides; x++) {
            const int c = center + x, i = end + x;
            if (top) { idx[ni++] = i; idx[ni++] = i + 1; idx[ni++] = c; }
            else { idx[ni++] = i + 1; idx[ni++] = i; idx[ni++] = c; }
        }
    }
    for (int i = 0; i < index; i++) { // updateGeoFromMesh: geometry.scale(mesh.scale)
        pos[i * 3] = (float) ((double) pos[i * 3] * r);
        pos[i * 3 + 1] = (float) ((double) pos[i * 3 + 1] * h);
        pos[i * 3 + 2] = (float) ((double) pos[i * 3 + 2] * r);
    }
    tris_indexed(t, pos, idx, ni);
    free(pos);
    free(idx);
}

// THREE.SphereGeometry(1, 12, 8) scaled to the size (GEOS.generateSphere useScale)
static void geo_sphere(Tris *t, const double w, const double h, const double d) {
    enum { WS = 12, HS = 8 };
    float pos[(WS + 1) * (HS + 1) * 3];
    int idx[WS * HS * 6], ni = 0, index = 0;
    for (int iy = 0; iy <= HS; iy++) {
        const double v = (double) iy / HS;
        for (int ix = 0; ix <= WS; ix++) {
            const double u = (double) ix / WS;
            pos[index * 3] = (float) (-cos(u * 2 * M_PI) * sin(v * M_PI));
            pos[index * 3 + 1] = (float) cos(v * M_PI);
            pos[index * 3 + 2] = (float) (sin(u * 2 * M_PI) * sin(v * M_PI));
            index++;
        }
    }
    for (int i = 0; i < index; i++) {
        pos[i * 3] = (float) ((double) pos[i * 3] * w / 2);
        pos[i * 3 + 1] = (float) ((double) pos[i * 3 + 1] * h / 2);
        pos[i * 3 + 2] = (float) ((double) pos[i * 3 + 2] * d / 2);
    }
    for (int iy = 0; iy < HS; iy++) {
        for (int ix = 0; ix < WS; ix++) {
            const int a = iy * (WS + 1) + ix + 1, b = iy * (WS + 1) + ix, c = (iy + 1) * (WS + 1) + ix, dd = (iy + 1) * (WS + 1) + ix + 1;
            if (iy != 0) { idx[ni++] = a; idx[ni++] = b; idx[ni++] = dd; }
            if (iy != HS - 1) { idx[ni++] = b; idx[ni++] = c; idx[ni++] = dd; }
        }
    }
    tris_indexed(t, pos, idx, ni);
}

// prefabs generatePlane: PlaneGeometry(1, 1, 1, 1) scaled (w, d, 1), rotated -90 degrees about X
static void geo_plane(Tris *t, const double w, const double d) {
    const int from = t->n;
    plane_unit(t);
    tris_scale_translate(t, from, w, d, 1, 0, 0, 0);
    tris_rotate(t, from, 0, -M_PI / 2);
}

static double vec_at(const cJSON *a, const int k, const double fallback) {
    const cJSON *v = cJSON_GetArrayItem(a, k);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

// The generated geometry of obj in world space, unrounded: 9 doubles per triangle in *out (malloc'd; NULL when none).
static int object_world_tris(const cJSON *obj, const int prefab, double **out) {
    *out = NULL;
    if (prefab < 0 || prefab >= EDITOR_PREFAB_COUNT) return 0;
    const char *name = editor_prefabs[prefab].name;
    const cJSON *s = G(obj, "s");
    const double w = vec_at(s, 0, 1), h = vec_at(s, 1, 1), d = vec_at(s, 2, 1);
    Tris t = {0};
    if (!strcmp(name, "CUBE")) {
        unsigned char bits[32];
        faces_from(G(obj, "f"), bits);
        geo_cube(&t, bits, w, h, d);
    } else if (!strcmp(name, "CYLINDER")) {
        const double r = fmin(w, d) / 2;
        const double top = js_null(obj, "rtp") ? 1 : pf_or(obj, "rtp", 0), bottom = js_null(obj, "rbt") ? 1 : pf_or(obj, "rbt", 0);
        const double ts = js_null(obj, "ths") ? 0 : pf_or(obj, "ths", 1), tl = js_null(obj, "thl") ? 360 : pf_or(obj, "thl", 0.1);
        double sides = pi_or(obj, "sds", 12);
        if (!num_truthy(sides) || sides < 1) sides = 12;
        geo_cylinder(&t, top, bottom, (int) sides, !js_null(obj, "opn"), ts * M_PI / 180, tl * M_PI / 180, r, h);
    } else if (!strcmp(name, "SPHERE")) {
        geo_sphere(&t, w, h, d);
    } else if (!strcmp(name, "PLANE")) {
        geo_plane(&t, w, d);
    } else {
        return 0; // CUSTOM_ASSET: the model's triangles need the model
    }
    // matrixWorld: quaternion from the Euler XYZ rotation, position = p + q * (0, h / 2, 0)
    const cJSON *p = G(obj, "p"), *r = G(obj, "r");
    const double rx = vec_at(r, 0, 0), ry = vec_at(r, 1, 0), rz = vec_at(r, 2, 0);
    const double c1 = cos(rx / 2), c2 = cos(ry / 2), c3 = cos(rz / 2), s1 = sin(rx / 2), s2 = sin(ry / 2), s3 = sin(rz / 2);
    const double qx = s1 * c2 * c3 + c1 * s2 * s3, qy = c1 * s2 * c3 - s1 * c2 * s3, qz = c1 * c2 * s3 + s1 * s2 * c3, qw = c1 * c2 * c3 - s1 * s2 * s3;
    const double x2 = qx + qx, y2 = qy + qy, z2 = qz + qz;
    const double xx = qx * x2, xy = qx * y2, xz = qx * z2, yy = qy * y2, yz = qy * z2, zz = qz * z2, wx = qw * x2, wy = qw * y2, wz = qw * z2;
    const double m[9] = {1 - (yy + zz), xy - wz, xz + wy, xy + wz, 1 - (xx + zz), yz - wx, xz - wy, yz + wx, 1 - (xx + yy)};
    const double oy = h / 2;
    const double px = vec_at(p, 0, 0) + m[1] * oy, py = vec_at(p, 1, 0) + m[4] * oy, pz = vec_at(p, 2, 0) + m[7] * oy;
    double *world = t.n ? malloc((size_t) t.n * 3 * sizeof(double)) : NULL;
    if (!world) {
        free(t.v);
        return 0;
    }
    for (int i = 0; i < t.n; i++) {
        const double x = t.v[i * 3], y = t.v[i * 3 + 1], z = t.v[i * 3 + 2];
        world[i * 3] = m[0] * x + m[1] * y + m[2] * z + px;
        world[i * 3 + 1] = m[3] * x + m[4] * y + m[5] * z + py;
        world[i * 3 + 2] = m[6] * x + m[7] * y + m[8] * z + pz;
    }
    free(t.v);
    *out = world;
    return t.n / 3;
}

int ed_model_object_tris(const cJSON *obj, const int prefab, cJSON *tris) {
    if (!tris) return 0;
    double *w;
    const int n = object_world_tris(obj, prefab, &w);
    for (int i = 0; i < n * 9; i++) cJSON_AddItemToArray(tris, cJSON_CreateNumber(isfinite(w[i]) ? floor(w[i] + 0.5) : 0));
    free(w);
    return n;
}

int ed_model_object_world_tris(const cJSON *obj, const int prefab, double **out) { return object_world_tris(obj, prefab, out); }

// ---------------------------------------------------------------------------
// v10.0.5 COLLISION_MESH: collision-only triangles, local to "p" (the bottom centre), stretched by s / collBase

static double round2(const double v) { return isfinite(v) ? floor(v * 100 + 0.5) / 100 : 0; } // Math.round(v * 100) / 100

void ed_model_collision_base(const cJSON *cmt, double base[3]) {
    // collBase: max - min of collTris per axis, at least 0.1 (`Math.max(max - min, 0.1) || 0.1`; no triangles: -inf -> 0.1)
    for (int k = 0; k < 3; k++) {
        double lo = INFINITY, hi = -INFINITY;
        int i = 0;
        const cJSON *e;
        cJSON_ArrayForEach(e, cmt) {
            if (i++ % 3 != k) continue;
            const double v = cJSON_IsNumber(e) ? e->valuedouble : 0;
            lo = fmin(lo, v);
            hi = fmax(hi, v);
        }
        const double d = fmax(hi - lo, 0.1);
        base[k] = isfinite(d) && d != 0 ? d : 0.1;
    }
}

int ed_model_collision_mesh_tris(const cJSON *obj, cJSON *tris) {
    // getMapExport's iiïîïií (editor-10.0.5.js:16347-16356): every cmt vertex scaled by size / collBase, turned by the
    // bounding mesh's quaternion, moved by p, rounded to 2 decimals
    const cJSON *cmt = G(obj, "cmt");
    const int n = cJSON_GetArraySize(cmt);
    if (!tris || n < 9 || n % 9) return 0;
    double base[3];
    ed_model_collision_base(cmt, base);
    const cJSON *p = G(obj, "p"), *s = G(obj, "s"), *r = G(obj, "r");
    double sc[3];
    for (int k = 0; k < 3; k++) sc[k] = vec_at(s, k, base[k]) / base[k];
    // the Euler XYZ rotation as a matrix (THREE.Matrix4.makeRotationFromEuler, rows; applyQuaternion of the same turn)
    const double rx = vec_at(r, 0, 0), ry = vec_at(r, 1, 0), rz = vec_at(r, 2, 0);
    const double a = cos(rx), b = sin(rx), c = cos(ry), d = sin(ry), ce = cos(rz), f = sin(rz);
    const double ae = a * ce, af = a * f, be = b * ce, bf = b * f;
    const double rm[9] = {c * ce, -c * f, d, af + be * d, ae - bf * d, -b * c, bf - ae * d, be + af * d, a * c};
    const double px = vec_at(p, 0, 0), py = vec_at(p, 1, 0), pz = vec_at(p, 2, 0);
    double v[3];
    int k = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, cmt) {
        v[k] = (cJSON_IsNumber(e) ? e->valuedouble : 0) * sc[k];
        if (++k < 3) continue;
        k = 0;
        cJSON_AddItemToArray(tris, cJSON_CreateNumber(round2(rm[0] * v[0] + rm[1] * v[1] + rm[2] * v[2] + px)));
        cJSON_AddItemToArray(tris, cJSON_CreateNumber(round2(rm[3] * v[0] + rm[4] * v[1] + rm[5] * v[2] + py)));
        cJSON_AddItemToArray(tris, cJSON_CreateNumber(round2(rm[6] * v[0] + rm[7] * v[1] + rm[8] * v[2] + pz)));
    }
    return n / 9;
}

cJSON *ed_model_collision_mesh_new(const double *tris, const int *starts, const int count) {
    // iïiìíïî (editor-10.0.5.js:16771-16792): the triangles at `starts` (offsets into tris, 9 values each) as one
    // COLLISION_MESH: p = (centre x, bottom y, centre z) of their bounds, cmt = each vertex - p, both rounded to 2 decimals
    if (!tris || !starts || count <= 0) return NULL;
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (int t = 0; t < count; t++) {
        for (int k = 0; k < 9; k++) {
            lo[k % 3] = fmin(lo[k % 3], tris[starts[t] + k]);
            hi[k % 3] = fmax(hi[k % 3], tris[starts[t] + k]);
        }
    }
    const double p[3] = {round2((lo[0] + hi[0]) / 2), round2(lo[1]), round2((lo[2] + hi[2]) / 2)};
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "i", prefab_by_name("COLLISION_MESH"));
    cJSON *pa = cJSON_AddArrayToObject(d, "p");
    for (int k = 0; k < 3; k++) cJSON_AddItemToArray(pa, cJSON_CreateNumber(p[k]));
    cJSON *cmt = cJSON_AddArrayToObject(d, "cmt");
    for (int t = 0; t < count; t++) {
        for (int k = 0; k < 9; k++) cJSON_AddItemToArray(cmt, cJSON_CreateNumber(round2(tris[starts[t] + k] - p[k % 3])));
    }
    cJSON *obj = ed_model_object(d, ED_MODEL_EXACT_TRANSFORM); // ObjectInstance.deserialize({i, p, cmt})
    cJSON_Delete(d);
    return obj;
}

// String.join of one value as importOrphanTris keys it (a number prints as JS does)
static void tri_value_key(const cJSON *v, char *out, const size_t n) {
    char buf[64];
    const char *s = cJSON_IsNumber(v) ? NULL : js_str(v, buf, sizeof(buf));
    if (cJSON_IsNumber(v)) js_num_str(v->valuedouble, out, n);
    else snprintf(out, n, "%s", s ? s : "");
}

// a string -> int map (open addressing, FNV-1a) for importOrphanTris' vertex keys
typedef struct {
    char **keys;
    int *vals;
    size_t cap;
} KeyMap;

static size_t key_hash(const char *s) {
    size_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char) *s) * 16777619u;
    return h;
}

static int *keymap_slot(KeyMap *m, const char *key, int *found) {
    size_t i = key_hash(key) & (m->cap - 1);
    while (m->keys[i] && strcmp(m->keys[i], key)) i = (i + 1) & (m->cap - 1);
    *found = m->keys[i] != NULL;
    if (!*found) m->keys[i] = (char *) key; // the caller owns `key` from here
    return &m->vals[i];
}

static int str_cmp(const void *a, const void *b) { return strcmp(*(char *const *) a, *(char *const *) b); }

static char *tri_key(const cJSON *const *vals, const int from, const int len) {
    char buf[1024] = "", one[64];
    for (int k = 0; k < len; k++) {
        tri_value_key(vals[from + k], one, sizeof(one));
        if (k) strncat(buf, ",", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, one, sizeof(buf) - strlen(buf) - 1);
    }
    return strdup(buf);
}

static int sorted_has(char *const *keys, const int n, const char *key) {
    return n > 0 && bsearch(&key, keys, (size_t) n, sizeof(char *), str_cmp) != NULL;
}

int ed_model_import_orphan_tris(const cJSON *map_tris, cJSON *objects) {
    // importOrphanTris (editor-10.0.5.js:16713-16770): the map's "tris" that no complexColl object of the import
    // bakes again (getMapExport compares them as "a,b,...,i" strings) become COLLISION_MESH objects, one per group of
    // triangles that share a vertex ("x,y,z"; union-find), in the order their groups are first met. KrunkNative: the
    // known set is what ed_model_object_tris bakes, i.e. what ed_model_export writes back; a CUSTOM_ASSET's cpx
    // triangles (Krunker bakes them from the loaded model) are not known here, so they come back as collision meshes
    // and the export still holds them.
    const int n = cJSON_GetArraySize(map_tris);
    if (!objects || !n || n % 9) return 0;
    const cJSON **vals = malloc((size_t) n * sizeof(*vals));
    if (!vals) return 0;
    {
        int i = 0;
        const cJSON *e;
        cJSON_ArrayForEach(e, map_tris) vals[i++] = e;
    }
    // the triangles the complexColl objects bake
    cJSON *known_tris = cJSON_CreateArray();
    const cJSON *o;
    cJSON_ArrayForEach(o, objects) {
        const int prefab = ed_model_prefab(o);
        if (prefab >= 0 && editor_prefab_has(prefab, EP_CANCOMPLEXCOLL) && G(o, "cpx")) ed_model_object_tris(o, prefab, known_tris);
    }
    const int nk = cJSON_GetArraySize(known_tris) / 9;
    char **known = nk ? calloc((size_t) nk, sizeof(char *)) : NULL;
    {
        const cJSON **kv = nk ? malloc((size_t) nk * 9 * sizeof(*kv)) : NULL;
        int i = 0;
        const cJSON *e;
        if (kv) cJSON_ArrayForEach(e, known_tris) if (i < nk * 9) kv[i++] = e;
        for (int t = 0; kv && known && t < nk; t++) known[t] = tri_key(kv, t * 9, 9);
        free(kv);
    }
    if (known) qsort(known, (size_t) nk, sizeof(char *), str_cmp);
    cJSON_Delete(known_tris);
    // the orphans, in map order
    int *orphan = malloc((size_t) (n / 9) * sizeof(int)), no = 0;
    for (int t = 0; orphan && t < n / 9; t++) {
        char *key = tri_key(vals, t * 9, 9);
        if (!known || !sorted_has(known, nk, key)) orphan[no++] = t * 9;
        free(key);
    }
    for (int t = 0; known && t < nk; t++) free(known[t]);
    free(known);
    int made = 0;
    if (no) {
        // union-find over shared vertices: each vertex key remembers the first orphan that had it
        int *parent = malloc((size_t) no * sizeof(int));
        KeyMap vk = {0};
        vk.cap = 16;
        while (vk.cap < (size_t) no * 6) vk.cap <<= 1; // 3 vertices per triangle, at most half full
        vk.keys = calloc(vk.cap, sizeof(char *));
        vk.vals = calloc(vk.cap, sizeof(int));
        double *num = malloc((size_t) n * sizeof(double));
        if (parent && vk.keys && vk.vals && num) {
            for (int i = 0; i < n; i++) num[i] = js_number(vals[i]);
            for (int i = 0; i < no; i++) parent[i] = i;
            for (int i = 0; i < no; i++) {
                for (int v = 0; v < 9; v += 3) {
                    char *key = tri_key(vals, orphan[i] + v, 3);
                    int found;
                    int *owner = key ? keymap_slot(&vk, key, &found) : NULL;
                    if (!owner) continue;
                    if (found) {
                        // iîìiïïî[find(i)] = find(seen), find with path halving
                        int a = i, b = *owner;
                        while (parent[a] != a) a = parent[a] = parent[parent[a]];
                        while (parent[b] != b) b = parent[b] = parent[parent[b]];
                        parent[a] = b;
                        free(key);
                    } else {
                        *owner = i;
                    }
                }
            }
            // groups keyed by their root, in first-seen order
            int *root = malloc((size_t) no * sizeof(int)), *order = malloc((size_t) no * sizeof(int)), *starts = malloc((size_t) no * sizeof(int));
            int *group_of = malloc((size_t) no * sizeof(int)), *first = malloc((size_t) no * sizeof(int)), *next = malloc((size_t) no * sizeof(int));
            int ng = 0;
            const int ok = root && order && starts && group_of && first && next;
            for (int i = 0; ok && i < no; i++) group_of[i] = -1;
            for (int i = 0; ok && i < no; i++) {
                int a = i;
                while (parent[a] != a) a = parent[a] = parent[parent[a]];
                root[i] = a;
                if (group_of[a] < 0) {
                    group_of[a] = ng;
                    first[ng] = -1;
                    order[ng++] = a;
                }
            }
            // each group's members in map order (a linked list per group, built backwards)
            for (int i = no - 1; ok && i >= 0; i--) {
                const int g = group_of[root[i]];
                next[i] = first[g];
                first[g] = i;
            }
            for (int g = 0; ok && g < ng; g++) {
                int c = 0;
                for (int i = first[g]; i >= 0; i = next[i]) starts[c++] = orphan[i];
                cJSON *mesh = ed_model_collision_mesh_new(num, starts, c);
                if (mesh) {
                    cJSON_AddItemToArray(objects, mesh);
                    made++;
                }
            }
            free(root);
            free(order);
            free(starts);
            free(group_of);
            free(first);
            free(next);
        }
        for (size_t j = 0; vk.keys && j < vk.cap; j++) free(vk.keys[j]);
        free(parent);
        free(vk.keys);
        free(vk.vals);
        free(num);
    }
    free(orphan);
    free(vals);
    return made;
}

// ---------------------------------------------------------------------------
// text helpers: base64 (UTILS.b64e / b64d / b64Test), sanitizeStr, sanitizeHTML, isURL

static const char K_B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *ed_model_b64_encode(const char *text) {
    const size_t n = strlen(text);
    char *out = malloc((n + 2) / 3 * 4 + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i += 3) {
        const unsigned v = (unsigned) (unsigned char) text[i] << 16 | (i + 1 < n ? (unsigned) (unsigned char) text[i + 1] << 8 : 0) |
                           (i + 2 < n ? (unsigned char) text[i + 2] : 0);
        out[j++] = K_B64[(v >> 18) & 63];
        out[j++] = K_B64[(v >> 12) & 63];
        out[j++] = i + 1 < n ? K_B64[(v >> 6) & 63] : '=';
        out[j++] = i + 2 < n ? K_B64[v & 63] : '=';
    }
    out[j] = 0;
    return out;
}

char *ed_model_b64_decode(const char *b64) {
    // b64Test: /^([0-9a-zA-Z+/]{4})*(([0-9a-zA-Z+/]{2}==)|([0-9a-zA-Z+/]{3}=))?$/
    const size_t n = strlen(b64);
    if (n % 4) return NULL;
    for (size_t i = 0; i < n; i++) {
        const int pad_ok = b64[i] == '=' && (i == n - 1 || (i == n - 2 && b64[n - 1] == '='));
        if (!strchr(K_B64, b64[i]) && !pad_ok) return NULL;
        if (!b64[i]) return NULL;
    }
    char *out = malloc(n / 4 * 3 + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i += 4) {
        unsigned v = 0;
        int pad = 0;
        for (int k = 0; k < 4; k++) {
            const char c = b64[i + k];
            v <<= 6;
            if (c == '=') pad++;
            else v |= (unsigned) (strchr(K_B64, c) - K_B64);
        }
        out[j++] = (char) ((v >> 16) & 255);
        if (pad < 2) out[j++] = (char) ((v >> 8) & 255);
        if (pad < 1) out[j++] = (char) (v & 255);
    }
    out[j] = 0;
    // intentional difference from v7.1.4: v7 decodes as ASCII (b64d: Buffer.toString("ascii"),
    // lossy for non-ASCII code); the bytes are kept as UTF-8 (doc/v7/editor.md 1.13)
    return out;
}

static char *sanitize_str(const char *s) {
    // UTILS.sanitizeStr: drop < > & and non-ASCII
    char *out = malloc(strlen(s) + 1);
    size_t j = 0;
    for (; out && *s; s++) {
        const unsigned char c = (unsigned char) *s;
        if (c == '<' || c == '>' || c == '&' || c > 0x7f) continue;
        out[j++] = (char) c;
    }
    if (out) out[j] = 0;
    return out;
}

static char *sanitize_html(const char *s) {
    // UTILS.sanitizeHTML: a text node's innerHTML (& < > and U+00A0 escaped)
    size_t n = 1;
    for (const char *p = s; *p; p++) n += *p == '&' ? 5 : (*p == '<' || *p == '>') ? 4 : ((unsigned char) p[0] == 0xc2 && (unsigned char) p[1] == 0xa0) ? 6 : 1;
    char *out = malloc(n);
    size_t j = 0;
    for (const char *p = s; out && *p; p++) {
        if (*p == '&') { memcpy(out + j, "&amp;", 5); j += 5; }
        else if (*p == '<') { memcpy(out + j, "&lt;", 4); j += 4; }
        else if (*p == '>') { memcpy(out + j, "&gt;", 4); j += 4; }
        else if ((unsigned char) p[0] == 0xc2 && (unsigned char) p[1] == 0xa0) { memcpy(out + j, "&nbsp;", 6); j += 6; p++; }
        else out[j++] = *p;
    }
    if (out) out[j] = 0;
    return out;
}

static int all_of(const char *a, const char *b, const char *set) {
    for (; a < b; a++) if (!(isalnum((unsigned char) *a) || strchr(set, *a))) return 0;
    return 1;
}

static int is_url(const char *s) {
    // UTILS.isURL: /^(https?:\/\/)?((([a-z\d]([a-z\d-]*[a-z\d])*)\.)+[a-z]{2,}|((\d{1,3}\.){3}\d{1,3}))(\:\d+)?
    //               (\/[-a-z\d%@_.~+&:]*)*(\?[;&a-z\d%@_.,~+&:=-]*)?(\#[-a-z\d_]*)?$/i
    if (!strncasecmp(s, "https://", 8)) s += 8;
    else if (!strncasecmp(s, "http://", 7)) s += 7;
    const char *host = s;
    while (*s && *s != ':' && *s != '/' && *s != '?' && *s != '#') s++;
    const char *host_end = s;
    int ok_host = 0;
    {
        // IPv4
        int groups = 0, digits = 0, ok = 1;
        for (const char *p = host; p < host_end && ok; p++) {
            if (isdigit((unsigned char) *p)) ok = ++digits <= 3;
            else if (*p == '.' && digits) { groups++; digits = 0; }
            else ok = 0;
        }
        if (ok && groups == 3 && digits) ok_host = 1;
    }
    if (!ok_host) {
        // labels "x." (alphanumeric ends, hyphens inside) then a top level of 2+ letters
        const char *last = NULL;
        for (const char *p = host; p < host_end; p++) if (*p == '.') last = p;
        if (last && host_end - last - 1 >= 2) {
            int ok = 1;
            for (const char *p = last + 1; p < host_end && ok; p++) ok = isalpha((unsigned char) *p);
            const char *label = host;
            for (const char *p = host; p <= last && ok; p++) {
                if (*p != '.') continue;
                ok = p > label && isalnum((unsigned char) *label) && isalnum((unsigned char) p[-1]) && all_of(label, p, "-");
                label = p + 1;
            }
            ok_host = ok;
        }
    }
    if (!ok_host) return 0;
    if (*s == ':') {
        const char *p = ++s;
        while (isdigit((unsigned char) *s)) s++;
        if (s == p) return 0;
    }
    while (*s == '/') {
        s++;
        while (*s && (isalnum((unsigned char) *s) || strchr("-%@_.~+&:", *s))) s++;
    }
    if (*s == '?') {
        s++;
        while (*s && (isalnum((unsigned char) *s) || strchr(";&%@_.,~+&:=-", *s))) s++;
    }
    if (*s == '#') {
        s++;
        while (*s && (isalnum((unsigned char) *s) || strchr("-_", *s))) s++;
    }
    return *s == 0;
}

const char *ed_model_script_template(const char *key) {
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(editor_data_get("kss"), "templates"), key);
    return cJSON_IsString(t) ? t->valuestring : "";
}

static double limit_mm(const double v, const double lo, const double hi) { return v < lo ? lo : v > hi ? hi : v; }

cJSON *ed_model_parse_guis(const cJSON *list, const int keep_all) {
    // UTILS.parseGUIs: clamp every field, sanitise the strings; without keep_all (the export)
    // drop the fields the element type ignores and the defaults
    cJSON *out = cJSON_CreateArray();
    const int types = cJSON_GetArraySize(editor_data_get("custGUITypes")), vtypes = cJSON_GetArraySize(editor_data_get("inputTypesGUI"));
    const cJSON *e;
    cJSON_ArrayForEach(e, list) {
        if (!cJSON_IsObject(e)) continue;
        cJSON *g = cJSON_Duplicate(e, 1);
        char buf[64];
#define GI(k, lo, hi) put_num(g, k, limit_mm(num_truthy(js_parse_int(G(e, k))) ? js_parse_int(G(e, k)) : 0, lo, hi))
        GI("t", 0, types - 1);
        GI("vt", 0, vtypes - 1);
        GI("w", 0, 2000);
        GI("h", 0, 2000);
        GI("x", 0, 100);
        GI("y", 0, 100);
        GI("z", 0, 2000);
        GI("id", 0, 2000);
#undef GI
        const double f = js_parse_int(G(e, "f"));
        put_num(g, "f", limit_mm(num_truthy(f) ? f : 10, 1, 100));
        const double o = js_parse_float(G(e, "o"));
        put_num(g, "o", limit_mm(num_truthy(o) ? o : 1, 0, 1));
        put_num(g, "nv", num_truthy(js_parse_int(G(e, "nv"))) ? 1 : 0);
        const char *v = js_truthy(G(e, "v")) ? js_str(G(e, "v"), buf, sizeof(buf)) : "";
        char *vf = text_filter(v ? v : "", K_TEXT_OK_BS);
        put(g, "v", cJSON_CreateString(vf ? vf : "")); // (JS assigns v before s, lbl and c)
        const char *sv = js_truthy(G(e, "s")) ? js_str(G(e, "s"), buf, sizeof(buf)) : "";
        char *sf = text_filter(sv ? sv : "", K_TEXT_OK_BS);
        put(g, "s", cJSON_CreateString(sf ? sf : ""));
        free(sf);
        const char *lv = js_truthy(G(e, "lbl")) ? js_str(G(e, "lbl"), buf, sizeof(buf)) : "";
        char *lf = text_filter(lv ? lv : "", " \t\r\n");
        put(g, "lbl", cJSON_CreateString(lf ? lf : ""));
        free(lf);
        put(g, "c", is_hex_color(G(e, "c")) ? cJSON_Duplicate(G(e, "c"), 1) : cJSON_CreateString("#ff0000"));
        // v: Input values keep the text set, Player / Global values are keys [a-z0-9_]
        if (cJSON_GetNumberValue(G(g, "vt")) != 0 && vf) {
            size_t j = 0;
            for (const char *q = vf; *q; q++) if (isalnum((unsigned char) *q) || *q == '_') vf[j++] = *q;
            vf[j] = 0;
        }
        put(g, "v", cJSON_CreateString(vf ? vf : ""));
        free(vf);
        cJSON_DeleteItemFromObjectCaseSensitive(g, "pv");
        if (!keep_all) {
            const double t = cJSON_GetNumberValue(G(g, "t"));
            if (t == 0) { cJSON_DeleteItemFromObjectCaseSensitive(g, "f"); cJSON_DeleteItemFromObjectCaseSensitive(g, "v"); }
            else if (t == 1 || t == 3) cJSON_DeleteItemFromObjectCaseSensitive(g, "s");
            else if (t == 2) { cJSON_DeleteItemFromObjectCaseSensitive(g, "f"); cJSON_DeleteItemFromObjectCaseSensitive(g, "v"); cJSON_DeleteItemFromObjectCaseSensitive(g, "c"); }
            if (t == 0) cJSON_DeleteItemFromObjectCaseSensitive(g, "t");
            if (cJSON_GetNumberValue(G(g, "z")) == 0) cJSON_DeleteItemFromObjectCaseSensitive(g, "z");
            if (G(g, "f") && cJSON_GetNumberValue(G(g, "f")) == 10) cJSON_DeleteItemFromObjectCaseSensitive(g, "f");
            if (cJSON_GetNumberValue(G(g, "id")) == 0) cJSON_DeleteItemFromObjectCaseSensitive(g, "id");
            if (cJSON_GetNumberValue(G(g, "vt")) == 0) cJSON_DeleteItemFromObjectCaseSensitive(g, "vt");
            const cJSON *c = G(g, "c");
            if (cJSON_IsString(c) && !strcasecmp(c->valuestring, "#ff0000")) cJSON_DeleteItemFromObjectCaseSensitive(g, "c");
            static const char *const strs[3] = {"s", "v", "lbl"};
            for (int k = 0; k < 3; k++) {
                const cJSON *x = G(g, strs[k]);
                if (x && (!cJSON_IsString(x) || !x->valuestring[0])) cJSON_DeleteItemFromObjectCaseSensitive(g, strs[k]);
            }
        }
        cJSON_AddItemToArray(out, g);
    }
    return out;
}

// ---------------------------------------------------------------------------
// the map: Editor.defineDefaults mapConfig / serverConfig

void ed_model_map_init(EdMapState *s) {
    memset(s, 0, sizeof(*s));
    const cJSON *zc = editor_data_get("zoneColors");
    const cJSON *sky = cJSON_GetObjectItemCaseSensitive(zc, "sky"), *br = cJSON_GetObjectItemCaseSensitive(zc, "BR");
#define ZC(a, i, d) (cJSON_IsString(cJSON_GetArrayItem(a, i)) ? cJSON_GetArrayItem(a, i)->valuestring : d)
    cJSON *m = s->map_config = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "name", "New Krunker Map");
    cJSON_AddNumberToObject(m, "ambInd", 1);
    cJSON_AddNumberToObject(m, "ambIndC", 0);
    cJSON_AddNumberToObject(m, "ambFadeTime", 0); // v10 (E10:14071)
    cJSON_AddStringToObject(m, "modURL", "");
    cJSON_AddStringToObject(m, "welMsg", "");
    cJSON_AddStringToObject(m, "mmId", "");
    cJSON_AddBoolToObject(m, "ocean", 0);
    cJSON_AddStringToObject(m, "ocCol", "#001e0f");
    cJSON_AddNumberToObject(m, "ocOpac", 0.9);
    cJSON_AddNumberToObject(m, "ocDist", 3.7);
    cJSON_AddNumberToObject(m, "ocSpd", 1);
    cJSON_AddNumberToObject(m, "ocYOff", -1);
    cJSON_AddNumberToObject(m, "ocRefRes", 3);
    cJSON_AddNumberToObject(m, "ocW", 1000);
    cJSON_AddNumberToObject(m, "ocL", 1000);
    cJSON_AddStringToObject(m, "terrainSeed", "");
    cJSON_AddStringToObject(m, "terrainTSeed", "");
    cJSON_AddNumberToObject(m, "terrainWidth", 3000);
    cJSON_AddNumberToObject(m, "terrainHeight", 3000);
    cJSON_AddNumberToObject(m, "terrainMntMlt", 1);
    cJSON_AddStringToObject(m, "terrainMntCol", "#707070");
    cJSON_AddStringToObject(m, "terrainSnwCol", "#e8fdff");
    cJSON_AddStringToObject(m, "terrainGrsCol", "#5e692f");
    cJSON_AddStringToObject(m, "terrainDrtCol", "#7f6238");
    cJSON_AddStringToObject(m, "terrainSndCol", "#967f51");
    cJSON_AddNumberToObject(m, "terrainEase", 12);
    cJSON_AddBoolToObject(m, "skyDome", 0);
    cJSON_AddStringToObject(m, "skyDomeCol0", ZC(sky, 0, "#74a4b9"));
    cJSON_AddStringToObject(m, "skyDomeCol1", ZC(sky, 1, "#dce8ed"));
    cJSON_AddStringToObject(m, "skyDomeCol2", ZC(sky, 2, "#dce8ed"));
    cJSON_AddStringToObject(m, "skyDomeEmis", "#000000");
    cJSON_AddNumberToObject(m, "skyDomeEmisTex", 0);
    cJSON_AddBoolToObject(m, "skyDomeTex", 0);
    cJSON_AddNumberToObject(m, "skyDomeTexA", 0);
    cJSON_AddNumberToObject(m, "skyDomeMovD", 0);
    cJSON_AddNumberToObject(m, "skyDomeMovT", 0);
    cJSON_AddBoolToObject(m, "zone", 0);
    cJSON_AddNumberToObject(m, "zoneSize", 500);
    cJSON_AddNumberToObject(m, "zoneHeight", 1000);
    cJSON_AddNumberToObject(m, "zoneStages", 0);
    cJSON_AddNumberToObject(m, "zoneWait", 5);
    cJSON_AddNumberToObject(m, "zoneSpeed", 1);
    cJSON_AddBoolToObject(m, "zoneRnd", 0);
    cJSON_AddStringToObject(m, "zoneCol0", ZC(br, 0, "#c542d9"));
    cJSON_AddStringToObject(m, "zoneCol1", ZC(br, 1, "#c542d9"));
    cJSON_AddStringToObject(m, "zoneCol2", ZC(br, 2, "#c542d9"));
    cJSON_AddNumberToObject(m, "sizeMlt", 2);
    cJSON_AddNumberToObject(m, "shadowR", data_num("defaults", "shadowRes", 1024));
    cJSON_AddNumberToObject(m, "shadowD", data_num("defaults", "shadowDst", 1200));
    cJSON_AddStringToObject(m, "ambient", "#97a0a8");
    cJSON_AddNumberToObject(m, "ambientI", data_num("defaults", "ambIntensity", 1));
    cJSON_AddStringToObject(m, "light", "#f2f8fc");
    cJSON_AddNumberToObject(m, "lightD", data_num("defaults", "lightDistance", 500));
    cJSON_AddNumberToObject(m, "lightI", data_num("defaults", "lightIntensity", 1.3));
    cJSON_AddNumberToObject(m, "sunAngX", data_num("defaults", "sunAngleX", 90));
    cJSON_AddNumberToObject(m, "sunAngY", data_num("defaults", "sunAngleY", 54));
    cJSON_AddStringToObject(m, "sky", "#dce8ed");
    cJSON_AddStringToObject(m, "fog", "#8d9aa0");
    cJSON_AddNumberToObject(m, "fogD", 2000);
    cJSON_AddNumberToObject(m, "dthY", -100);
    cJSON_AddBoolToObject(m, "oldAO", 0);
    cJSON_AddBoolToObject(m, "rampFix", 0);
    cJSON_AddBoolToObject(m, "correctLights", 0);
    cJSON_AddNumberToObject(m, "toneMapping", 0);
    cJSON_AddNumberToObject(m, "toneMappingExposure", 1);
    cJSON_AddNumberToObject(m, "outputEncoding", 0);
#undef ZC
    s->extra = cJSON_CreateObject();
    s->server_config = cJSON_Parse("{\"modes\":{},\"classes\":{},\"settings\":{},\"weps\":{},\"cls\":{},\"prks\":{}}");
    s->gui = cJSON_CreateArray();
    s->scripts = cJSON_Parse("{\"client\":\"\",\"server\":\"\"}");
}

void ed_model_map_free(EdMapState *s) {
    cJSON_Delete(s->map_config);
    cJSON_Delete(s->extra);
    cJSON_Delete(s->server_config);
    cJSON_Delete(s->gui);
    cJSON_Delete(s->scripts);
    memset(s, 0, sizeof(*s));
}

int ed_model_map_key_known(const char *key) {
    static EdMapState defaults;
    if (!defaults.map_config) ed_model_map_init(&defaults);
    return cJSON_GetObjectItemCaseSensitive(defaults.map_config, key) != NULL;
}

// ---------------------------------------------------------------------------
// export (getMapExport)

static int js_loose_eq(const cJSON *a, const cJSON *b) {
    // a == b for the JSON values a config holds (numbers, booleans, strings, null)
    if (!a || !b) return (!a || cJSON_IsNull(a)) && (!b || cJSON_IsNull(b));
    if (cJSON_IsNull(a) || cJSON_IsNull(b)) return cJSON_IsNull(a) && cJSON_IsNull(b);
    if (cJSON_IsString(a) && cJSON_IsString(b)) return !strcmp(a->valuestring, b->valuestring);
    if ((cJSON_IsNumber(a) || cJSON_IsBool(a) || cJSON_IsString(a)) && (cJSON_IsNumber(b) || cJSON_IsBool(b) || cJSON_IsString(b))) {
        const double x = js_number(a), y = js_number(b);
        return x == y;
    }
    return cJSON_Compare(a, b, 1);
}

static int str_len(const cJSON *v) { return cJSON_IsString(v) ? (int) strlen(v->valuestring) : 0; } // .length of a string (0 otherwise)

// the mapConfig keys getMapExport writes (default values and disabled features dropped)
static int map_key_exported(const cJSON *mc, const char *k, const cJSON *v) {
    const double n = js_number(v);
    if (!strncmp(k, "oc", 2) && !js_truthy(G(mc, "ocean"))) return 0;
    if (!strncmp(k, "zone", 4) && !js_truthy(G(mc, "zone"))) return 0;
    if (!strcmp(k, "zoneHeight") && n == 1000) return 0;
    if (!strcmp(k, "zoneStages") && n == 0) return 0;
    if (!strcmp(k, "zoneWait") && n == 5) return 0;
    if ((!strncmp(k, "terrain", 7) || !strcmp(k, "sizeMlt")) && !str_len(G(mc, "terrainSeed"))) return 0;
    if (!strncmp(k, "skyDome", 7) && !js_truthy(G(mc, "skyDome"))) return 0;
    if (!strcmp(k, "skyDomeEmis") && cJSON_IsString(v) && !strcmp(v->valuestring, "#000000")) return 0;
    if ((!strcmp(k, "skyDomeMovT") || !strcmp(k, "skyDomeMovD")) && n == 0) return 0;
    if ((!strcmp(k, "modURL") || !strcmp(k, "welMsg") || !strcmp(k, "mmId")) && !str_len(v)) return 0;
    if (!strcmp(k, "shadowR") && n == data_num("defaults", "shadowRes", 1024)) return 0;
    if (!strcmp(k, "shadowD") && n == data_num("defaults", "shadowDst", 1200)) return 0;
    if (!strcmp(k, "ambInd") && n == 1) return 0;
    if (!strcmp(k, "ambIndC") && !js_truthy(v)) return 0;
    if (!strcmp(k, "ambFadeTime") && n == 0) return 0; // v10 (E10:16568)
    if (!strcmp(k, "dthY") && n == -100) return 0;
    if (!strcmp(k, "oldAO") && !js_truthy(v)) return 0;
    if (!strcmp(k, "rampFix") && !js_truthy(v)) return 0; // v10.0.3 Solid Ramp Sides: off is the default
    if (!strcmp(k, "lightD") && n == data_num("defaults", "lightDistance", 500)) return 0;
    if (!strcmp(k, "lightI") && n == data_num("defaults", "lightIntensity", 1.3)) return 0;
    if (!strcmp(k, "ambientI") && n == data_num("defaults", "ambIntensity", 1)) return 0;
    if (!strcmp(k, "sunAngX") && n == data_num("defaults", "sunAngleX", 90)) return 0;
    if (!strcmp(k, "sunAngY") && n == data_num("defaults", "sunAngleY", 54)) return 0;
    if (!strcmp(k, "toneMapping") && n == 0) return 0;
    if (!strcmp(k, "toneMappingExposure") && n == 1) return 0;
    if (!strcmp(k, "outputEncoding") && n == 0) return 0;
    if (!strcmp(k, "correctLights") && !js_truthy(v)) return 0;
    return 1;
}

static cJSON *stringify_color(const cJSON *v) {
    // UTILS.stringifyColor: "#" + ("000000" + v.toString(16)).substr(-6)
    char hex[32], buf[64];
    if (cJSON_IsNumber(v)) {
        const double d = v->valuedouble;
        if (d >= 0 && d == floor(d) && d < 4294967296.0) snprintf(buf, sizeof(buf), "000000%lx", (unsigned long) d);
        else snprintf(buf, sizeof(buf), "000000%s", "0");
    } else {
        const char *s = js_str(v, hex, sizeof(hex));
        snprintf(buf, sizeof(buf), "000000%s", s ? s : "");
    }
    const size_t n = strlen(buf);
    snprintf(hex, sizeof(hex), "#%s", buf + (n > 6 ? n - 6 : 0));
    return cJSON_CreateString(hex);
}

static int pool_index(cJSON *pool, const char *s) {
    int i = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, pool) {
        if (cJSON_IsString(it) && !strcmp(it->valuestring, s)) return i;
        i++;
    }
    cJSON_AddItemToArray(pool, cJSON_CreateString(s));
    return i;
}

// config.<list> entries: drop the values equal to the stock data (getMapExport weps / cls / prks)
static void export_stock_list(cJSON *cfg, const char *key, const char *stock_key, const char *fields_key) {
    cJSON *list = cJSON_GetObjectItemCaseSensitive(cfg, key);
    if (!list) return;
    const cJSON *stock = editor_data_get(stock_key), *fields = editor_data_get(fields_key);
    const int n = cJSON_GetArraySize(stock);
    for (int i = 0; i < n; i++) {
        char idx[16];
        snprintf(idx, sizeof(idx), "%d", i);
        cJSON *e = cJSON_GetObjectItemCaseSensitive(list, idx);
        if (!cJSON_IsObject(e)) continue;
        const cJSON *def = cJSON_GetArrayItem(stock, i), *f;
        cJSON_ArrayForEach(f, fields) {
            const char *var = cJSON_GetStringValue(G(f, "varN"));
            const cJSON *d = var ? G(def, var) : NULL;
            if (!js_nullish(d) && js_loose_eq(G(e, var), d)) cJSON_DeleteItemFromObjectCaseSensitive(e, var);
        }
        if (!cJSON_GetArraySize(e)) cJSON_DeleteItemFromObjectCaseSensitive(list, idx);
    }
    if (!cJSON_GetArraySize(list)) cJSON_DeleteItemFromObjectCaseSensitive(cfg, key);
}

static int key_index_cmp(const void *a, const void *b) {
    const int x = *(const int *) a, y = *(const int *) b;
    return (x > y) - (x < y);
}

// serverConfig.modes / .classes: {"i": bool} -> the ascending list of enabled indices
static cJSON *switches_list(const cJSON *map, const int count) {
    int *on = calloc((size_t) (count > 0 ? count : 1), sizeof(int)), n = 0;
    for (int i = 0; on && i < count; i++) {
        char idx[16];
        snprintf(idx, sizeof(idx), "%d", i);
        if (js_truthy(G(map, idx))) on[n++] = i;
    }
    qsort(on, (size_t) n, sizeof(int), key_index_cmp);
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(list, cJSON_CreateNumber(on[i]));
    free(on);
    return list;
}

static cJSON *export_config(const cJSON *server) {
    cJSON *cfg = cJSON_Duplicate(server, 1);
    if (!cfg) return NULL;
    // modes: dropped when none or only mode 0
    cJSON *modes = switches_list(G(server, "modes"), cJSON_GetArraySize(editor_data_get("modes")));
    const int nm = cJSON_GetArraySize(modes);
    if (!nm || (nm == 1 && cJSON_GetArrayItem(modes, 0)->valuedouble == 0)) {
        cJSON_Delete(modes);
        cJSON_DeleteItemFromObjectCaseSensitive(cfg, "modes");
    } else put(cfg, "modes", modes);
    // classes: dropped when none or all
    const int class_count = cJSON_GetArraySize(editor_data_get("classes"));
    cJSON *classes = switches_list(G(server, "classes"), class_count);
    const int nc = cJSON_GetArraySize(classes);
    if (!nc || nc == class_count) {
        cJSON_Delete(classes);
        cJSON_DeleteItemFromObjectCaseSensitive(cfg, "classes");
    } else put(cfg, "classes", classes);
    // settings: normalised, defaults dropped
    cJSON *settings = cJSON_GetObjectItemCaseSensitive(cfg, "settings");
    const cJSON *w;
    cJSON_ArrayForEach(w, editor_data_get("serverConfig")) {
        if (G(w, "cost") || js_truthy(G(w, "hideE")) || !settings) continue;
        const char *var = cJSON_GetStringValue(G(w, "varN"));
        if (!var) continue;
        const cJSON *v = G(settings, var), *def = G(w, "def");
        if (js_nullish(v)) continue; // unset: the panel's default (v7's panel fills every setting first)
        cJSON *nv;
        if (js_truthy(G(w, "bool"))) nv = cJSON_CreateNumber(js_truthy(v) ? 1 : 0);
        else if (js_truthy(G(w, "input"))) {
            if (!cJSON_IsString(v)) {
                cJSON_DeleteItemFromObjectCaseSensitive(settings, var); // sanitizeStr(undefined): not written
                continue;
            }
            char *s = sanitize_str(v->valuestring);
            nv = cJSON_CreateString(s ? s : "");
            free(s);
        } else if (js_truthy(G(w, "select"))) {
            // intentional difference from v7.1.4: v7 writes parseInt(<the config object>[varN]),
            // NaN, i.e. null; the option index is written (editor_model.h)
            const double idx = js_number(v);
            nv = idx == floor(idx) && idx >= 0 && idx < cJSON_GetArraySize(G(w, "options")) ? cJSON_CreateNumber(idx) : cJSON_Duplicate(def, 1);
        } else nv = cJSON_CreateNumber(js_parse_float(v));
        if (js_loose_eq(nv, def)) {
            cJSON_Delete(nv);
            cJSON_DeleteItemFromObjectCaseSensitive(settings, var);
        } else put(settings, var, nv);
    }
    if (settings && !cJSON_GetArraySize(settings)) cJSON_DeleteItemFromObjectCaseSensitive(cfg, "settings");
    export_stock_list(cfg, "weps", "weapons", "weaponConfig");
    export_stock_list(cfg, "cls", "classes", "classConfig");
    export_stock_list(cfg, "prks", "effects", "effectConfig");
    // anything else empty goes too (a v7 map always fills these lists)
    static const char *const lists[4] = {"weps", "cls", "prks", "settings"};
    for (int i = 0; i < 4; i++) {
        const cJSON *l = G(cfg, lists[i]);
        if (l && !cJSON_GetArraySize(l)) cJSON_DeleteItemFromObjectCaseSensitive(cfg, lists[i]);
    }
    if (!cJSON_GetArraySize(cfg)) {
        cJSON_Delete(cfg);
        return NULL;
    }
    return cfg;
}

// Krunker v10 botPaths: [{p: [[x, y, z]...], r: 0|1}] per botNodeGroup (bng) in first-seen
// order of the nodes sorted by botNode (bn, a stable sort like Array.sort); r = the bnr of the
// group's node 0 (default 1). NULL when the map has no BOT_PATH_NODE.
typedef struct { int group, node; size_t index; } BotNodeRef;

static int bot_node_cmp(const void *a, const void *b) {
    const BotNodeRef *x = a, *y = b;
    if (x->node != y->node) return x->node < y->node ? -1 : 1;
    return x->index < y->index ? -1 : x->index > y->index;
}

static cJSON *bot_paths_export(const EdModelObj *objs, const size_t n) {
    BotNodeRef *refs = NULL;
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        if (objs[i].prefab < 0 || objs[i].prefab >= EDITOR_PREFAB_COUNT || !objs[i].data) continue;
        if (strcmp(editor_prefabs[objs[i].prefab].name, "BOT_PATH_NODE")) continue;
        BotNodeRef *grown = realloc(refs, (count + 1) * sizeof(*refs));
        if (!grown) break;
        refs = grown;
        const double g = pi_or(objs[i].data, "bng", 0), b = pi_or(objs[i].data, "bn", 0);
        refs[count++] = (BotNodeRef) {isfinite(g) ? (int) g : 0, isfinite(b) ? (int) b : 0, i};
    }
    if (!count) {
        free(refs);
        return NULL;
    }
    qsort(refs, count, sizeof(*refs), bot_node_cmp);
    cJSON *groups = cJSON_CreateObject(); // keyed by group, in insertion order (Object.values)
    for (size_t k = 0; k < count; k++) {
        char key[16];
        snprintf(key, sizeof(key), "%d", refs[k].group);
        cJSON *g = cJSON_GetObjectItemCaseSensitive(groups, key);
        if (!g) {
            g = cJSON_AddObjectToObject(groups, key);
            cJSON_AddArrayToObject(g, "p");
            cJSON_AddNumberToObject(g, "r", 1);
        }
        const cJSON *data = objs[refs[k].index].data, *p = G(data, "p");
        cJSON *pos = cJSON_CreateArray();
        for (int c = 0; c < 3; c++) cJSON_AddItemToArray(pos, cJSON_CreateNumber(vec_at(p, c, 0)));
        cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(g, "p"), pos);
        if (refs[k].node == 0) cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(g, "r"), pi_or(data, "bnr", 0) == 1 ? 1 : 0);
    }
    free(refs);
    // Object.values: integer-like keys enumerate in ascending order, like JS
    cJSON *out = cJSON_CreateArray();
    for (;;) {
        cJSON *best = NULL;
        for (cJSON *g = groups->child; g; g = g->next)
            if (!best || atoi(g->string) < atoi(best->string)) best = g;
        if (!best) break;
        cJSON_AddItemToArray(out, cJSON_DetachItemViaPointer(groups, best));
    }
    cJSON_Delete(groups);
    return out;
}

cJSON *ed_model_export(const EdMapState *st, const EdModelObj *objs, const size_t n, const int flags) {
    const int raw = flags & ED_EXPORT_RAW;
    cJSON *objects = cJSON_CreateArray(), *colors = cJSON_CreateArray(), *xyz = cJSON_CreateArray(), *sizes = cJSON_CreateArray();
    cJSON *spawns = cJSON_CreateArray(), *tris = cJSON_CreateArray(), *cam = NULL;
    for (size_t i = 0; i < n; i++) {
        const EdModelObj *o = &objs[i];
        if (o->prefab < 0 || o->prefab >= EDITOR_PREFAB_COUNT || !o->data) continue;
        if (!raw && editor_prefab_has(o->prefab, EP_NOEXPORT)) continue;
        const char *name = editor_prefabs[o->prefab].name;
        if (!raw && !strcmp(name, "SPAWN_POINT")) {
            // [x, y, z, team, direction, cmpSpwn, teamOnly?, spread?] (a spread without teamOnly leaves null at 6)
            const cJSON *p = G(o->data, "p");
            cJSON *sp = cJSON_CreateArray();
            for (int k = 0; k < 3; k++) cJSON_AddItemToArray(sp, cJSON_CreateNumber(vec_at(p, k, 0)));
            cJSON_AddItemToArray(sp, cJSON_CreateNumber(pi_or(o->data, "tm", 0)));
            cJSON_AddItemToArray(sp, cJSON_CreateNumber(num_truthy(pi_or(o->data, "d", 0)) ? pi_or(o->data, "d", 0) : 0));
            cJSON_AddItemToArray(sp, cJSON_CreateNumber(js_truthy(G(o->data, "cp")) ? 1 : 0));
            const int team_only = js_truthy(G(o->data, "to"));
            const double spread = pf_or(o->data, "ssp", 0);
            if (team_only || num_truthy(spread)) cJSON_AddItemToArray(sp, team_only ? cJSON_CreateNumber(1) : cJSON_CreateNull());
            if (num_truthy(spread)) cJSON_AddItemToArray(sp, cJSON_CreateNumber(js_round(spread, 1)));
            cJSON_AddItemToArray(spawns, sp);
            continue;
        }
        if (!raw && !strcmp(name, "BOT_PATH_NODE")) continue; // botPaths below (v10 getMapExport, E10:16374)
        if (!raw && !strcmp(name, "CAMERA_POSITION")) {
            // [x, y, z, static, rx, ry, rz]
            const cJSON *p = G(o->data, "p"), *r = G(o->data, "r");
            cJSON_Delete(cam);
            cam = cJSON_CreateArray();
            for (int k = 0; k < 3; k++) cJSON_AddItemToArray(cam, cJSON_CreateNumber(vec_at(p, k, 0)));
            cJSON_AddItemToArray(cam, cJSON_CreateBool(js_truthy(G(o->data, "cstc"))));
            for (int k = 0; k < 3; k++) cJSON_AddItemToArray(cam, cJSON_CreateNumber(vec_at(r, k, 0)));
            continue;
        }
        // serialize(raw): p, s, objUUID (raw), the rest; p and r rounded to 2 decimals (the editor
        // holds the exact transform, as three.js does; camPos / spawns above keep it)
        cJSON *v = cJSON_CreateObject();
        const cJSON *it;
        cJSON_ArrayForEach(it, o->data) {
            if (!strcmp(it->string, "objUUID")) continue;
            cJSON *c = cJSON_Duplicate(it, 1);
            if ((!strcmp(it->string, "p") || !strcmp(it->string, "r")) && cJSON_IsArray(c)) {
                for (cJSON *e = c->child; e; e = e->next) if (cJSON_IsNumber(e)) cJSON_SetNumberValue(e, js_round(e->valuedouble, 2));
            }
            cJSON_AddItemToObject(v, it->string, c);
            if (raw && !strcmp(it->string, "s")) cJSON_AddNumberToObject(v, "objUUID", o->uuid);
        }
        static const char *const pooled[2] = {"e", "c"};
        for (int k = 0; k < 2; k++) {
            const cJSON *c = G(v, pooled[k]);
            if (!js_truthy(c)) continue;
            cJSON *hex = stringify_color(c);
            char ikey[4];
            snprintf(ikey, sizeof(ikey), "%si", pooled[k]);
            put_num(v, ikey, pool_index(colors, hex->valuestring));
            cJSON_Delete(hex);
            cJSON_DeleteItemFromObjectCaseSensitive(v, pooled[k]);
        }
        const cJSON *s = G(v, "s");
        if (!(flags & ED_EXPORT_NO_SIZE_POOL) && js_truthy(s)) {
            // xyz pool: "x,y,z" (Array.join) -> si
            char key[128] = "";
            const cJSON *e;
            cJSON_ArrayForEach(e, s) {
                char num[40];
                js_num_str(js_number(e), num, sizeof(num));
                if (key[0]) strncat(key, ",", sizeof(key) - strlen(key) - 1);
                strncat(key, num, sizeof(key) - strlen(key) - 1);
            }
            const int before = cJSON_GetArraySize(sizes);
            const int idx = pool_index(sizes, key);
            if (idx == before) cJSON_ArrayForEach(e, s) cJSON_AddItemToArray(xyz, cJSON_Duplicate(e, 1));
            put_num(v, "si", idx);
            cJSON_DeleteItemFromObjectCaseSensitive(v, "s");
        }
        if (!raw && !(flags & ED_EXPORT_NO_TRIS) && editor_prefab_has(o->prefab, EP_CANCOMPLEXCOLL) && G(o->data, "cpx"))
            ed_model_object_tris(o->data, o->prefab, tris);
        cJSON_AddItemToArray(objects, v);
    }
    cJSON_Delete(sizes);
    // v10.0.5: the COLLISION_MESHes (noExport, skipped above) as world triangles after the complexColl ones
    // (getMapExport, editor-10.0.5.js:16454-16460)
    for (size_t i = 0; !raw && !(flags & ED_EXPORT_NO_TRIS) && i < n; i++) {
        if (objs[i].prefab >= 0 && objs[i].prefab < EDITOR_PREFAB_COUNT && objs[i].data && !strcmp(editor_prefabs[objs[i].prefab].name, "COLLISION_MESH"))
            ed_model_collision_mesh_tris(objs[i].data, tris);
    }

    cJSON *out = cJSON_CreateObject();
    const cJSON *it;
    cJSON_ArrayForEach(it, st->map_config) {
        if (!map_key_exported(st->map_config, it->string, it)) continue;
        cJSON *v = cJSON_Duplicate(it, 1);
        if ((!strcmp(it->string, "fogD") || !strcmp(it->string, "shadowD")) && cJSON_IsNumber(v)) cJSON_SetNumberValue(v, floor(v->valuedouble + 0.5));
        cJSON_AddItemToObject(out, it->string, v);
    }
    cJSON_ArrayForEach(it, st->extra) { // the imported keys v7 keeps in mapConfig (after the defaults)
        if (cJSON_GetObjectItemCaseSensitive(out, it->string) || !map_key_exported(st->map_config, it->string, it)) continue;
        cJSON_AddItemToObject(out, it->string, cJSON_Duplicate(it, 1));
    }
    if (cJSON_GetArraySize(colors)) cJSON_AddItemToObject(out, "colors", colors);
    else cJSON_Delete(colors);
    if (cJSON_GetArraySize(xyz)) cJSON_AddItemToObject(out, "xyz", xyz);
    else cJSON_Delete(xyz);
    cJSON_AddItemToObject(out, "objects", objects);
    cJSON *cfg = export_config(st->server_config);
    if (cfg) cJSON_AddItemToObject(out, "config", cfg);
    cJSON *gui = ed_model_parse_guis(st->gui, 0);
    if (cJSON_GetArraySize(gui)) cJSON_AddItemToObject(out, "gui", gui);
    else cJSON_Delete(gui);
    // scripts: base64, omitted when empty or the template
    cJSON *scripts = cJSON_CreateObject();
    static const char *const keys[3] = {"client", "server", "shared"};
    for (int k = 0; k < 3; k++) {
        const cJSON *code = G(st->scripts, keys[k]);
        const char *tpl = ed_model_script_template(keys[k]);
        if (!cJSON_IsString(code) || !code->valuestring[0] || (tpl && !strcmp(code->valuestring, tpl))) continue;
        char *b64 = ed_model_b64_encode(code->valuestring);
        if (b64) cJSON_AddStringToObject(scripts, keys[k], b64);
        free(b64);
    }
    if (cJSON_GetArraySize(scripts)) cJSON_AddItemToObject(out, "scripts", scripts);
    else cJSON_Delete(scripts);
    if (!raw && cam) cJSON_AddItemToObject(out, "camPos", cam);
    else cJSON_Delete(cam);
    if (!raw) cJSON_AddItemToObject(out, "spawns", spawns);
    else cJSON_Delete(spawns);
    if (!raw) { // v10 botPaths (E10:16405-16420): BOT_PATH_NODEs by botNodeGroup, sorted by botNode
        cJSON *paths = bot_paths_export(objs, n);
        if (paths) cJSON_AddItemToObject(out, "botPaths", paths);
    }
    if (!raw && cJSON_GetArraySize(tris)) cJSON_AddItemToObject(out, "tris", tris);
    else cJSON_Delete(tris);
    // name / welMsg through sanitizeHTML
    static const char *const html[2] = {"name", "welMsg"};
    for (int k = 0; k < 2; k++) {
        const cJSON *v = G(out, html[k]);
        char buf[64];
        const char *s = v ? js_str(v, buf, sizeof(buf)) : NULL;
        if (!s) continue;
        char *e = sanitize_html(s);
        if (e) put(out, html[k], cJSON_CreateString(e));
        free(e);
    }
    return out;
}

// ---------------------------------------------------------------------------
// import (importMap / loadObjects)

static int js_includes_num(const cJSON *list, const int value) {
    // Array.prototype.includes (SameValueZero): only numbers match a number
    const cJSON *it;
    cJSON_ArrayForEach(it, list) if (cJSON_IsNumber(it) && it->valuedouble == value) return 1;
    return 0;
}

static int text_invalid_config(const cJSON *v) {
    // /[^\w \-\|]+/gim.test(v) (server / weapon / class / effect text values)
    char buf[64];
    const char *s = js_str(v, buf, sizeof(buf));
    if (!s) return 0; // String(undefined) never reaches here; objects pass in v7 as "[object Object]" (fails)
    for (; *s; s++) if (!(isalnum((unsigned char) *s) || *s == '_' || *s == ' ' || *s == '-' || *s == '|')) return 1;
    return 0;
}

static cJSON *child(cJSON *parent, const char *key) {
    cJSON *o = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsObject(o)) {
        cJSON_DeleteItemFromObjectCaseSensitive(parent, key);
        o = cJSON_AddObjectToObject(parent, key);
    }
    return o;
}

// config.weps / cls / prks: imported per index, text values that fail the check fall back to the stock value
static void import_stock_list(EdMapState *st, const cJSON *config, const char *key, const char *stock_key, const char *fields_key) {
    const cJSON *src = G(config, key);
    if (!cJSON_GetArraySize(src)) return;
    const cJSON *stock = editor_data_get(stock_key), *fields = editor_data_get(fields_key);
    cJSON *dst = child(st->server_config, key);
    const int n = cJSON_GetArraySize(stock);
    for (int i = 0; i < n; i++) {
        char idx[16];
        snprintf(idx, sizeof(idx), "%d", i);
        const cJSON *e = cJSON_IsArray(src) ? cJSON_GetArrayItem(src, i) : G(src, idx);
        if (!cJSON_IsObject(e) || !cJSON_GetArraySize(e)) continue;
        cJSON *copy = cJSON_Duplicate(e, 1);
        if (!strcmp(key, "cls") && js_truthy(G(copy, "asset"))) { // legacy class "asset" -> "aID"
            put_dup(copy, "aID", G(copy, "asset"));
            cJSON_DeleteItemFromObjectCaseSensitive(copy, "asset");
        }
        cJSON *d = child(dst, idx);
        const cJSON *f;
        cJSON_ArrayForEach(f, fields) {
            const char *var = cJSON_GetStringValue(G(f, "varN"));
            const cJSON *v = var ? G(copy, var) : NULL;
            if (js_nullish(v)) continue;
            if (js_truthy(G(f, "input")) && text_invalid_config(v)) v = G(cJSON_GetArrayItem(stock, i), var);
            if (v) put_dup(d, var, v);
            else cJSON_DeleteItemFromObjectCaseSensitive(d, var);
        }
        cJSON_Delete(copy);
    }
}

static void import_config(EdMapState *st, const cJSON *config) {
    const cJSON *modes = G(config, "modes");
    if (cJSON_GetArraySize(modes)) {
        cJSON *m = child(st->server_config, "modes");
        const int n = cJSON_GetArraySize(editor_data_get("modes"));
        for (int i = 0; i < n; i++) {
            char idx[16];
            snprintf(idx, sizeof(idx), "%d", i);
            put(m, idx, cJSON_CreateBool(js_includes_num(modes, i)));
        }
    }
    const cJSON *classes = G(config, "classes");
    if (cJSON_GetArraySize(classes)) {
        cJSON *c = child(st->server_config, "classes");
        const int n = cJSON_GetArraySize(editor_data_get("classes"));
        for (int i = 0; i < n; i++) {
            char idx[16];
            snprintf(idx, sizeof(idx), "%d", i);
            put(c, idx, cJSON_CreateBool(js_includes_num(classes, i)));
        }
    }
    const cJSON *settings = G(config, "settings");
    if (cJSON_GetArraySize(settings)) {
        cJSON *dst = child(st->server_config, "settings");
        const cJSON *w;
        cJSON_ArrayForEach(w, editor_data_get("serverConfig")) {
            const char *var = cJSON_GetStringValue(G(w, "varN"));
            const cJSON *v = var ? G(settings, var) : NULL;
            if (js_truthy(G(w, "hideE")) || js_nullish(v)) continue;
            if (js_truthy(G(w, "input"))) {
                char buf[64];
                const char *s = js_str(v, buf, sizeof(buf));
                char *clean = sanitize_str(s ? s : "");
                cJSON *sv = cJSON_CreateString(clean ? clean : "");
                free(clean);
                if (text_invalid_config(sv)) {
                    cJSON_Delete(sv);
                    put_dup(dst, var, G(w, "def"));
                } else put(dst, var, sv);
            } else put_dup(dst, var, v);
        }
    }
    import_stock_list(st, config, "weps", "weapons", "weaponConfig");
    import_stock_list(st, config, "cls", "classes", "classConfig");
    import_stock_list(st, config, "prks", "effects", "effectConfig");
}

// a legacy top-level "game" block (v3.1.1's map-wide player config) is copied into every class;
// "aID" only into classes without a model
static void import_game(EdMapState *st, const cJSON *game) {
    cJSON *cls = child(st->server_config, "cls");
    const cJSON *stock = editor_data_get("classes");
    const int n = cJSON_GetArraySize(stock);
    const cJSON *it;
    cJSON_ArrayForEach(it, game) {
        for (int i = 0; i < n; i++) {
            char idx[16];
            snprintf(idx, sizeof(idx), "%d", i);
            cJSON *c = child(cls, idx);
            if (!strcmp(it->string, "aID")) {
                const cJSON *cur = G(c, "aID");
                if (!cur) cur = G(cJSON_GetArrayItem(stock, i), "aID"); // the panel's default
                cJSON *empty = cJSON_CreateString("");
                const int blank = cur && js_loose_eq(cur, empty);
                cJSON_Delete(empty);
                if (blank) put_dup(c, "aID", it);
            } else put_dup(c, it->string, it);
        }
    }
}

static int is_number_value(const cJSON *v) { return cJSON_IsNumber(v) && isfinite(v->valuedouble); } // UTILS.isNumber

// importMap's checks of the mapConfig defaults (E:4101-4245)
static void validate_map_config(EdMapState *st) {
    EdMapState def;
    ed_model_map_init(&def);
    cJSON *mc = st->map_config;
    const cJSON *d;
    cJSON_ArrayForEach(d, def.map_config) {
        const char *k = d->string;
        cJSON *v = cJSON_GetObjectItemCaseSensitive(mc, k);
        if (js_nullish(v)) continue;
        char buf[64];
        const char *s = js_str(v, buf, sizeof(buf));
        int reset = 0;
        if ((!strcmp(k, "name") || !strcmp(k, "welMsg")) && s && !text_valid(s, K_TEXT_OK)) reset = 1;
        if (!strcmp(k, "modURL") && !(s && is_url(s))) reset = 1;
        if (!strcmp(k, "mmId") && s) {
            for (const char *q = s; *q; q++) if (!strchr(K_ASSET_ID, tolower((unsigned char) *q))) reset = 1;
        }
        if ((!strcmp(k, "terrainSeed") || !strcmp(k, "terrainTSeed")) && s && !text_valid(s, "_-.")) reset = 1;
        if (reset) {
            put_dup(mc, k, d);
            continue;
        }
        static const char *const ints[] = {"ambInd", "ambIndC", "terrainWidth", "terrainHeight", "terrainMntMlt", "terrainEase", "skyDomeMovD",
                                           "skyDomeMovT", "zoneSize", "zoneHeight", "zoneStages", "zoneWait", "fogD", "dthY", "toneMapping",
                                           "outputEncoding", "shadowR", "shadowD", "ambientI", "lightD", "lightI", "sunAngX", "sunAngY",
                                           "skyDomeTexA", "ocRefRes", "ocW", "ocL", NULL};
        static const char *const floats[] = {"zoneSpeed", "toneMappingExposure", "ocOpac", "ocDist", "ocSpd", "ocYOff", NULL};
        static const char *const bools[] = {"oldAO", "rampFix", "correctLights", "skyDome", "skyDomeTex", "zone", "zoneRnd", "ocean", NULL};
        for (int i = 0; ints[i]; i++) if (!strcmp(k, ints[i]) && !is_number_value(v)) put_num(mc, k, js_parse_int(v));
        for (int i = 0; floats[i]; i++) if (!strcmp(k, floats[i]) && !is_number_value(v)) put_num(mc, k, js_parse_float(v));
        for (int i = 0; bools[i]; i++) if (!strcmp(k, bools[i])) put(mc, k, cJSON_CreateBool(js_truthy(v)));
    }
    ed_model_map_free(&def);
}

int ed_model_import(const cJSON *doc, EdMapState *st, cJSON **objects_out, const cJSON **map_out) {
    if (objects_out) *objects_out = NULL;
    if (map_out) *map_out = NULL;
    if (!cJSON_IsObject(doc)) return 0;
    // `e.states || e.id || e.map ? e.map : e`
    const cJSON *map = js_truthy(G(doc, "states")) || js_truthy(G(doc, "id")) || js_truthy(G(doc, "map")) ? G(doc, "map") : doc;
    cJSON *parsed = NULL;
    if (cJSON_IsString(map)) map = parsed = cJSON_Parse(map->valuestring); // KrunkNative: a hub record's map string
    if (!cJSON_IsObject(map)) {
        cJSON_Delete(parsed);
        return 0;
    }
    if (map_out && !parsed) *map_out = map;
    ed_model_map_free(st);
    ed_model_map_init(st);

    // loadObjects: colours and sizes back inline, every object through ObjectInstance
    cJSON *objects = cJSON_CreateArray();
    const cJSON *colors = G(map, "colors"), *xyz = G(map, "xyz");
    const int xyz_n = cJSON_GetArraySize(xyz);
    const cJSON *o;
    cJSON_ArrayForEach(o, G(map, "objects")) {
        if (!cJSON_IsObject(o)) continue;
        cJSON *d = cJSON_Duplicate(o, 1);
        static const char *const pooled[2] = {"e", "c"};
        for (int k = 0; k < 2; k++) {
            char ikey[4];
            snprintf(ikey, sizeof(ikey), "%si", pooled[k]);
            const cJSON *idx = G(d, ikey);
            const double ix = js_number(idx);
            const cJSON *col = !js_nullish(idx) && ix == floor(ix) && ix >= 0 ? cJSON_GetArrayItem(colors, (int) ix) : NULL;
            if (js_truthy(col)) {
                put_dup(d, pooled[k], col);
                cJSON_DeleteItemFromObjectCaseSensitive(d, ikey);
            }
        }
        const cJSON *si = G(d, "si");
        const double sv = js_number(si) * 3;
        if (!js_nullish(si) && sv == floor(sv) && sv >= 0 && xyz_n > sv) {
            cJSON *s = cJSON_CreateArray();
            for (int k = 0; k < 3 && sv + k < xyz_n; k++) cJSON_AddItemToArray(s, cJSON_Duplicate(cJSON_GetArrayItem(xyz, (int) sv + k), 1));
            put(d, "s", s);
            cJSON_DeleteItemFromObjectCaseSensitive(d, "si");
        }
        // KrunkNative: an object with an invalid prefab id is skipped (v7 aborts the import)
        cJSON *obj = ed_model_object(d, ED_MODEL_KEEP_UUID | ED_MODEL_EXACT_TRANSFORM);
        cJSON_Delete(d);
        if (obj) cJSON_AddItemToArray(objects, obj);
    }
    const cJSON *cam = G(map, "camPos");
    if (js_truthy(cam) && cJSON_IsArray(cam)) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "id", 6);
        cJSON *p = cJSON_AddArrayToObject(c, "p");
        for (int k = 0; k < 3; k++) {
            const cJSON *e = cJSON_GetArrayItem(cam, k);
            cJSON_AddItemToArray(p, e ? cJSON_Duplicate(e, 1) : cJSON_CreateNull());
        }
        cJSON_AddBoolToObject(c, "cstc", js_truthy(cJSON_GetArrayItem(cam, 3)));
        cJSON *r = cJSON_AddArrayToObject(c, "r");
        for (int k = 4; k < 7; k++) {
            const cJSON *e = cJSON_GetArrayItem(cam, k);
            cJSON_AddItemToArray(r, e ? cJSON_Duplicate(e, 1) : cJSON_CreateNumber(0));
        }
        cJSON *obj = ed_model_object(c, ED_MODEL_EXACT_TRANSFORM);
        cJSON_Delete(c);
        if (obj) cJSON_AddItemToArray(objects, obj);
    }
    const cJSON *sp;
    cJSON_ArrayForEach(sp, G(map, "spawns")) {
        if (!cJSON_IsArray(sp)) continue;
        cJSON *c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "id", 5);
        cJSON *p = cJSON_AddArrayToObject(c, "p");
        for (int k = 0; k < 3; k++) {
            const cJSON *e = cJSON_GetArrayItem(sp, k);
            cJSON_AddItemToArray(p, e ? cJSON_Duplicate(e, 1) : cJSON_CreateNull());
        }
        static const char *const keys[5] = {"tm", "d", "cp", "to", "ssp"};
        for (int k = 0; k < 5; k++) {
            const cJSON *e = cJSON_GetArrayItem(sp, 3 + k);
            if (e) cJSON_AddItemToObject(c, keys[k], cJSON_Duplicate(e, 1));
        }
        cJSON *obj = ed_model_object(c, ED_MODEL_EXACT_TRANSFORM);
        cJSON_Delete(c);
        if (obj) cJSON_AddItemToArray(objects, obj);
    }
    // botPaths: v10 importMap (E10:17046-17064): a BOT_PATH_NODE per point, {bnr: path.r, bn: k,
    // bnn: k + 1, bng: path index} (v7 had no such prefab and made cubes of them)
    const cJSON *paths = G(map, "botPaths");
    if (cJSON_IsArray(paths)) {
        int group = 0;
        const cJSON *path;
        cJSON_ArrayForEach(path, paths) {
            const cJSON *pts = G(path, "p");
            int k = 0;
            const cJSON *pt;
            cJSON_ArrayForEach(pt, pts) {
                cJSON *c = cJSON_CreateObject();
                cJSON_AddNumberToObject(c, "id", 41);
                cJSON *pp = cJSON_AddArrayToObject(c, "p");
                for (int a = 0; a < 3; a++) cJSON_AddItemToArray(pp, cJSON_CreateNumber(vec_at(pt, a, 0)));
                const cJSON *r = G(path, "r");
                if (r) cJSON_AddItemToObject(c, "bnr", cJSON_Duplicate(r, 1));
                cJSON_AddNumberToObject(c, "bn", k);
                cJSON_AddNumberToObject(c, "bnn", k + 1);
                cJSON_AddNumberToObject(c, "bng", group);
                cJSON *obj = ed_model_object(c, ED_MODEL_EXACT_TRANSFORM);
                cJSON_Delete(c);
                if (obj) cJSON_AddItemToArray(objects, obj);
                k++;
            }
            group++;
        }
    }
    // v10.0.5 importMap: the "tris" no object bakes again become COLLISION_MESH objects once the objects are loaded
    // (editor-10.0.5.js:17190-17194, importOrphanTris)
    const cJSON *map_tris = G(map, "tris");
    if (cJSON_IsArray(map_tris)) ed_model_import_orphan_tris(map_tris, objects);
    const cJSON *config = G(map, "config");
    if (js_truthy(config)) import_config(st, config);
    const cJSON *game = G(map, "game");
    if (cJSON_GetArraySize(game)) import_game(st, game);
    const cJSON *gui = G(map, "gui");
    if (cJSON_GetArraySize(gui)) {
        cJSON_Delete(st->gui);
        st->gui = ed_model_parse_guis(gui, 1);
    }
    const cJSON *scripts = G(map, "scripts");
    if (js_truthy(scripts)) {
        // + v10's "shared" (E10:10253): kept and exported; its tab is the window's third
        static const char *const keys[3] = {"client", "server", "shared"};
        for (int k = 0; k < 3; k++) {
            const cJSON *code = G(scripts, keys[k]);
            if (k == 2 && !cJSON_IsString(code)) continue;
            const char *text = cJSON_IsString(code) ? code->valuestring : "";
            char *decoded = ed_model_b64_decode(text);
            put(st->scripts, keys[k], cJSON_CreateString(decoded ? decoded : text));
            free(decoded);
        }
    }
    // Object.assign(mapConfig, map) without what importMap consumed
    static const char *const consumed[] = {"objects", "tris", "camPos", "spawns", "botPaths", "config", "game", "gui", "scripts", "colors", "xyz", NULL};
    const cJSON *it;
    cJSON_ArrayForEach(it, map) {
        int skip = 0;
        for (int i = 0; consumed[i] && !skip; i++) skip = !strcmp(consumed[i], it->string);
        if (skip) continue;
        put_dup(ed_model_map_key_known(it->string) ? st->map_config : st->extra, it->string, it);
    }
    validate_map_config(st);
    cJSON_Delete(parsed);
    if (objects_out) *objects_out = objects;
    else cJSON_Delete(objects);
    return 1;
}

// ---------------------------------------------------------------------------
// object properties by ObjectInstance name (the inspector's binding): the constructor's load rule
// for reads, a raw write for sets; ed_model_object() then re-derives the serialized form, so the
// prefab gates, defaults and rounding of serialize() apply to every edit.

typedef enum {
    PK_NUM,      // parseFloat(p.k || def): absent / 0 -> def
    PK_NUMN,     // p.k == null ? def : parseFloat(p.k || 0)
    PK_INT,      // parseInt(p.k || def)
    PK_INTN,     // p.k == null ? def : parseInt(p.k || 0)
    PK_PRESENT,  // p.k != null (written as 1)
    PK_ABSENT,   // p.k == null (false writes 1)
    PK_TRUTHY,   // !!p.k (written as 1)
    PK_BOOL,     // !!p.k, written as a JSON boolean (ENVIRONMENT_ZONE)
    PK_STR,      // a string, "" when absent
    PK_COLOR,    // a colour string, sdef when absent
    PK_RAW,      // any value, as stored (asset ids, condition values); def when absent
} PropKind;

typedef struct {
    const char *prop, *key;
    unsigned char kind, dec, action; // action: stored in tas[0] on TRIGGER / AI prefabs
    double def;
    const char *sdef;
} PropDef;

#define P(prop, key, kind, def) {prop, key, kind, 2, 0, def, NULL}
#define PC(prop, key, sdef) {prop, key, PK_COLOR, 0, 0, 0, sdef}
#define PA(prop, key, kind, def) {prop, key, kind, 2, 1, def, NULL}

static const PropDef K_PROPS[] = {
    // collision, movement, health
    P("playerCollidable", "pl", PK_ABSENT, 0), P("bulletColl", "ns", PK_ABSENT, 0), P("aiColl", "nai", PK_ABSENT, 0),
    P("canWallJump", "wj", PK_ABSENT, 0), P("canGrapple", "gp", PK_ABSENT, 0), P("canImpulse", "ip", PK_ABSENT, 0),
    P("penetrable", "pe", PK_NUM, 0), P("visible", "v", PK_ABSENT, 0), P("boost", "b", PK_NUM, 0), P("crouch", "cr", PK_ABSENT, 0),
    P("bounce", "bm", PK_NUM, 1), P("edgeNoise", "en", PK_NUM, 0), P("canDest", "cdy", PK_PRESENT, 0), P("startDest", "sd", PK_PRESENT, 0),
    P("spawnChance", "rsc", PK_NUM, 0), P("respawnT", "rt", PK_NUM, 0), P("respawnR", "rr", PK_PRESENT, 0),
    P("complexColl", "cpx", PK_PRESENT, 0), P("hbHeight", "hbh", PK_NUM, 1), P("hbWidth", "hbw", PK_NUM, 1),
    // particles
    P("part", "pr", PK_INT, 0), P("partC", "pc", PK_INT, 0), P("partS", "ps", PK_NUM, 0), P("partSP", "psp", PK_NUM, 0),
    P("partG", "pg", PK_NUM, 0), P("partD", "pd", PK_NUM, 0), P("partSC", "psc", PK_NUM, 0), P("partB", "pb", PK_INT, 0),
    // style / material
    P("team", "tm", PK_INT, 0), PC("color", "c", "#FFFFFF"), PC("emissive", "e", "#000000"), P("darken", "da", PK_NUM, 0),
    P("opacity", "o", PK_NUMN, 1), P("direction", "d", PK_INT, 0), P("forceDir", "fd", PK_PRESENT, 0),
    P("texStretch", "tsr", PK_PRESENT, 0), P("texScale", "tsm", PK_NUM, 0), P("movingSpeed", "ts", PK_NUM, 0),
    P("movingAxis", "td", PK_NUM, 0), P("frameCount", "fct", PK_INT, 0), P("frameSpeed", "fs", PK_NUM, 0),
    P("texOffX", "tox", PK_NUM, 0), P("texOffY", "toy", PK_NUM, 0), P("texRot", "tro", PK_INT, 0), P("texSmooth", "txs", PK_ABSENT, 0),
    P("forceTrans", "ft", PK_PRESENT, 0), P("renderSide", "rs", PK_INTN, -1), P("texEncode", "ten", PK_INT, 0),
    P("texVar", "tv", PK_INT, 0), P("alphaTest", "alt", PK_NUM, 0), P("assetTexture", "at", PK_RAW, 0),
    P("sprayTexture", "spt", PK_INT, 2482), P("border", "bo", PK_PRESENT, 0), P("showMinimap", "smm", PK_ABSENT, 0),
    P("ambient", "ab", PK_ABSENT, 0), P("fog", "nf", PK_ABSENT, 0), P("cast", "cst", PK_ABSENT, 0), P("receive", "rcv", PK_ABSENT, 0),
    P("forceRender", "fcr", PK_PRESENT, 0), P("mimicSprite", "msp", PK_PRESENT, 0), P("noTexture", "ntx", PK_PRESENT, 0),
    // logic
    P("score", "sp", PK_INT, 1), P("single", "su", PK_PRESENT, 0), P("singleP", "sup", PK_PRESENT, 0),
    P("weaponReplace", "wr", PK_ABSENT, 0), P("randWep", "rw", PK_PRESENT, 0), P("randWepGroup", "rwg", PK_INT, 0),
    P("showPickupIcon", "wpi", PK_ABSENT, 0), P("poster", "bb", PK_INT, 0), P("anchorType", "ant", PK_INT, 0),
    P("spin", "spn", PK_NUM, 0), P("spinAxis", "spa", PK_INT, 0), P("channel", "ch", PK_INT, 0), P("telCheckPoint", "tcp", PK_PRESENT, 0),
    P("node", "n", PK_INT, 0), P("momentum", "m", PK_PRESENT, 0), P("hidden", "nm", PK_PRESENT, 0), P("closeable", "cl", PK_PRESENT, 0),
    P("startClosed", "stc", PK_PRESENT, 0), P("method", "md", PK_INT, 0), P("canTrigger", "ct", PK_PRESENT, 0),
    P("event", "te", PK_INT, 0), P("actionCost", "tac", PK_INT, 0), P("actionMethod", "tam", PK_INT, 0),
    P("selfDestruct", "sfd", PK_PRESENT, 0), P("constantEvent", "tce", PK_INT, 0), P("chance", "cha", PK_INT, 0),
    P("interfaceId", "in", PK_INT, 0), P("interfaceIdT", "tin", PK_INT, 0), P("withdraw", "wd", PK_INTN, 1000),
    P("deposit", "dp", PK_INTN, 1000), P("objectiveId", "obi", PK_INT, 0), P("objectiveLink", "obl", PK_INT, 0),
    P("nodeId", "nid", PK_INT, 0), P("perk", "zp", PK_INT, 0), P("scriptId", "sci", PK_INT, 0), P("interactM", "inm", PK_STR, 0),
    P("cmpSpwn", "cp", PK_TRUTHY, 0), P("teamOnly", "to", PK_TRUTHY, 0), P("spread", "ssp", PK_NUM, 0),
    P("nodeTag", "nt", PK_STR, 0), P("enclose", "ec", PK_PRESENT, 0), P("static", "cstc", PK_TRUTHY, 0),
    P("camType", "camt", PK_INT, 0), P("forceCam", "frc", PK_PRESENT, 0), P("followPath", "fp", PK_STR, 0),
    P("stepSrc", "sat", PK_INT, 0), P("stype", "sty", PK_INT, 0), P("minimum", "krs", PK_INT, 0),
    // the first trigger action (tas[0]); WP4's multi-action UI uses ed_model_action_*
    PA("action", "ta", PK_INT, 0), PA("constant", "tc", PK_INT, 0), PA("constant2", "tc2", PK_INT, 0),
    PA("constantType", "tce", PK_INT, 0), PA("constantTxt", "tct", PK_STR, 0), PA("constantCust", "tcc", PK_STR, 0),
    PA("delay", "tad", PK_NUM, 0), PA("condition", "cd", PK_INT, 0), PA("conditionVal0", "cv0", PK_RAW, 0),
    PA("conditionVal1", "cv1", PK_RAW, 0), PA("conditionVal2", "cv2", PK_RAW, 0),
    // sound (SOUND_EMITTER object; tas[0] on TRIGGER / AI)
    PA("snd", "es", PK_INT, 0), PA("assetSound", "esa", PK_INT, 0), PA("vol", "ev", PK_NUM, 1), PA("rate", "er", PK_NUM, 1),
    PA("emitfrom", "ef", PK_INT, 0), P("dist", "ed", PK_NUM, 0), P("distM", "edm", PK_INT, 0), P("roll", "ero", PK_NUM, 0),
    // text, leaderboard
    P("text", "st", PK_STR, 0), PC("fcolor", "sc", "#000000"), PC("bcolor", "sb", "#FFFFFF"), PC("ocolor", "so", "#111111"),
    P("othick", "sot", PK_INT, 1), P("backVisible", "bv", PK_ABSENT, 0), P("fsize", "sf", PK_INT, 10), P("noSCF", "nsf", PK_PRESENT, 0),
    P("tAlign", "sa", PK_INT, 0), P("leaderType", "ldt", PK_INT, 0), P("leaderTypeCust", "ldc", PK_STR, 0),
    P("leaderOrder", "ldo", PK_INT, 0), P("leaderResults", "ldr", PK_INT, 25),
    // liquid, cylinder / cone, lights, models
    P("wSpeed", "ws", PK_NUM, 10), P("wHeight", "wh", PK_NUM, 0.5), P("tilesX", "tx", PK_NUM, 10), P("tilesZ", "tz", PK_NUM, 10),
    P("fullcone", "fc", PK_PRESENT, 0), P("openEnded", "opn", PK_PRESENT, 0), P("radiusTop", "rtp", PK_NUMN, 1),
    P("radiusBottom", "rbt", PK_NUMN, 1), P("thetaLength", "thl", PK_NUMN, 360), P("thetaStart", "ths", PK_NUMN, 0),
    P("sides", "sds", PK_INT, 12), P("intensity", "li", PK_NUM, 1), P("distance", "lds", PK_NUMN, 50), P("decay", "ld", PK_NUM, 1),
    P("angle", "la", PK_INT, 45), P("penumbra", "lp", PK_NUM, 0), P("itemID", "skin", PK_INT, 0), P("assetID", "aid", PK_RAW, 0),
    P("animS", "as", PK_NUM, 1), P("startAnim", "ast", PK_STR, 0), P("mYOff", "myo", PK_NUM, 0), P("mROff", "mro", PK_NUM, 0),
    P("bobS", "bbS", PK_NUM, 0), P("bobD", "bbD", PK_NUM, 0),
    // AI_SPAWNER
    P("aiName", "ain", PK_STR, 0), P("aiLvl", "ail", PK_INT, 0), P("aiTagScale", "ains", PK_NUM, 1), P("turnSpd", "tsd", PK_NUMN, 1),
    P("roamTime", "rmt", PK_INT, 1000), P("roamRad", "rmr", PK_INT, 0), P("aiChasd", "aic", PK_INT, 10), P("visDis", "vd", PK_INT, 120),
    P("ignoreOnStuck", "igos", PK_PRESENT, 0), P("driftOnStuck", "dos", PK_PRESENT, 0), P("invincibleOnStuck", "ivos", PK_PRESENT, 0),
    P("canShoot", "cs", PK_PRESENT, 0), P("canHit", "cnh", PK_PRESENT, 0), P("xrayVis", "xv", PK_PRESENT, 0),
    P("targAI", "tAi", PK_PRESENT, 0), P("targPlr", "tPl", PK_ABSENT, 0), P("friendly", "aif", PK_TRUTHY, 0),
    // Krunker v10 (E10:7236, 7320, 7350, 7443)
    P("dyn", "dyn", PK_TRUTHY, 0), P("isBoss", "boss", PK_TRUTHY, 0), P("canContactHit", "cnch", PK_PRESENT, 0),
    P("crateType", "crt", PK_INT, 0),
    // BOT_PATH_NODE (E10:7409-7412; the inspector's GroupId / NodeId / Next NodeId / Reversible)
    P("botNodeGroup", "bng", PK_INT, 0), P("botNode", "bn", PK_INT, 0), P("botNodeNext", "bnn", PK_INTN, 1),
    P("botNodeRev", "bnr", PK_TRUTHY, 0),
    P("explode", "exp", PK_PRESENT, 0), P("speed", "spd", PK_NUMN, 1), P("frate", "fr", PK_INT, 2000), P("aicur", "aac", PK_NUM, 0),
    P("hrate", "hr", PK_INT, 800), P("shoBrk", "shb", PK_INT, 400), P("shoArY", "sha", PK_INT, 0), P("shoOffH", "shof", PK_ABSENT, 0),
    P("shoDel", "shdy", PK_INT, 0), P("melDel", "mldy", PK_INT, 400), P("melShake", "mls", PK_NUM, 0), P("shoShake", "ss", PK_NUM, 0),
    P("shotEff", "se", PK_INTN, -1), P("hitEff", "he", PK_INTN, -1), P("critDmg", "crd", PK_NUM, 0), P("melrng", "mlr", PK_INT, 10),
    P("meldmg", "mld", PK_INT, 10), P("projG", "prg", PK_NUM, 0), P("projGL", "prgl", PK_PRESENT, 0), P("projT", "prgt", PK_PRESENT, 0),
    P("projSC", "prsc", PK_NUM, 1), P("projMS", "prms", PK_NUM, 1), P("projS", "prs", PK_NUMN, 1), P("projSpd", "pjs", PK_NUM, 1),
    P("projDmg", "pjd", PK_INT, 100), P("projA", "pra", PK_STR, 0), P("behaviorType", "bh", PK_INT, 0), P("rewardType", "rew", PK_STR, 0),
    P("rewardMethod", "rewm", PK_INT, 0), P("rewardReq", "rewr", PK_INT, 0), P("rewardNode", "rewn", PK_INT, 0),
    P("pathInput", "phi", PK_STR, 0), P("pathType", "pht", PK_INT, 0), P("gravity", "grv", PK_NUMN, 1),
    P("idleSound", "idl", PK_INT, 0), P("deathSound", "dth", PK_INT, 0), P("shootSound", "atk", PK_INT, 0),
    P("shootHitSound", "atkh", PK_INT, 0), P("melSound", "mel", PK_INT, 0), P("melHitSound", "melh", PK_INT, 0),
    P("agroSound", "agr", PK_INT, 0),
    // BOT_SPAWNER
    P("botName", "btn", PK_STR, 0), P("botClass", "btc", PK_INTN, 0), P("botLives", "btl", PK_INTN, 0), P("botTeam", "btt", PK_INTN, 0),
    P("healthMlt", "bthm", PK_NUMN, 1), P("dmgMlt", "btdm", PK_NUMN, 1), P("speedMlt", "btspm", PK_NUMN, 1), P("rangeMlt", "btrm", PK_NUMN, 1),
    P("spreadMlt", "btsm", PK_NUMN, 1), P("botForceClass", "btfc", PK_PRESENT, 0), P("botTargetBots", "bttc", PK_PRESENT, 0),
    P("botTargetTeam", "bttt", PK_PRESENT, 0), P("botForceCrouch", "btfcr", PK_PRESENT, 0), P("botAttackTarget", "btat", PK_ABSENT, 0),
    P("botFollowTarget", "btft", PK_ABSENT, 0), P("botWatchTarget", "btwt", PK_ABSENT, 0), P("botMimicTarget", "btmt", PK_PRESENT, 0),
    P("botSlideHop", "btsh", PK_PRESENT, 0), P("botCanRespawn", "btcr", PK_ABSENT, 0), P("botUseSpawnDelay", "btsd", PK_ABSENT, 0),
    P("botRemoveOnDeath", "btrd", PK_ABSENT, 0), P("botNodYes", "btny", PK_PRESENT, 0), P("botNodNo", "btno", PK_PRESENT, 0),
    P("botJoinMessage", "btjm", PK_PRESENT, 0), P("botLeaveMessage", "btlm", PK_PRESENT, 0), P("botVoiceChat", "btvc", PK_ABSENT, 0),
    P("botGodMode", "btgm", PK_PRESENT, 0), P("botRoam", "btr", PK_ABSENT, 0), P("botRandomSpawn", "btrs", PK_PRESENT, 0),
    P("botRecoil", "btrc", PK_ABSENT, 0), P("botPrimSkin", "btcpr", PK_INTN, -1), P("botDye", "btcd", PK_INTN, -1),
    P("botFace", "btcf", PK_INTN, -1), P("botHat", "btcht", PK_INTN, -1), P("botHead", "btch", PK_INTN, -1),
    P("botBack", "btcbk", PK_INTN, -1), P("botBody", "btcb", PK_INTN, -1), P("botWaist", "btcw", PK_INTN, -1),
    P("botWrist", "btcwr", PK_INTN, -1), P("botShoes", "btcs", PK_INTN, -1), P("botPet", "btcp", PK_INTN, -1),
    P("botMelee", "btcm", PK_INTN, -1),
    // ENVIRONMENT_ZONE (every key is written; the defaults are the map's)
    P("ambInd", "ambInd", PK_INTN, 1), P("ambIndC", "ambIndC", PK_INT, 0), P("ambFadeTime", "ambFadeTime", PK_INT, 0), P("skyDome", "skyDome", PK_BOOL, 0),
    PC("skyDomeCol0", "skyDomeCol0", "#74a4b9"), PC("skyDomeCol1", "skyDomeCol1", "#dce8ed"), PC("skyDomeCol2", "skyDomeCol2", "#dce8ed"),
    PC("skyDomeEmis", "skyDomeEmis", "#000000"), P("skyDomeEmisTex", "skyDomeEmisTex", PK_RAW, 0), P("skyDomeTex", "skyDomeTex", PK_BOOL, 0),
    P("skyDomeTexA", "skyDomeTexA", PK_RAW, 0), P("skyDomeMovD", "skyDomeMovD", PK_NUM, 0), P("skyDomeMovT", "skyDomeMovT", PK_NUM, 0),
    P("shadowR", "shadowR", PK_INTN, 1024), P("shadowD", "shadowD", PK_INTN, 1200), PC("ambientC", "ambientC", "#97a0a8"),
    P("ambientI", "ambientI", PK_NUMN, 1), PC("lightC", "light", "#f2f8fc"), P("lightD", "lightD", PK_INTN, 500),
    P("lightI", "lightI", PK_NUMN, 1.3), P("sunAngX", "sunAngX", PK_INTN, 90), P("sunAngY", "sunAngY", PK_INTN, 54),
    PC("sky", "sky", "#dce8ed"), PC("fogC", "fogC", "#8d9aa0"), P("fogD", "fogD", PK_INTN, 2000),
    P("correctLights", "correctLights", PK_BOOL, 0), P("toneMapping", "toneMapping", PK_NUM, 0),
    P("toneMappingExposure", "toneMappingExposure", PK_NUMN, 1), P("outputEncoding", "outputEncoding", PK_INT, 0),
    {NULL, NULL, 0, 0, 0, 0, NULL},
};

#undef P
#undef PC
#undef PA

// A property name, optionally with a trigger action index: "constant@2" is tas[2]'s "constant"
// (the multi-action list of the logic UI, editor_triggers.c); a plain action prop is tas[0].
// *index = -1 without a suffix.
static const PropDef *prop_def_at(const char *prop, int *index) {
    *index = -1;
    const char *at = prop ? strchr(prop, '@') : NULL;
    const size_t n = at ? (size_t) (at - prop) : (prop ? strlen(prop) : 0);
    if (at) {
        char *end = NULL;
        const long i = strtol(at + 1, &end, 10);
        if (end == at + 1 || *end || i < 0 || i >= 64) return NULL;
        *index = (int) i;
    }
    for (int i = 0; prop && K_PROPS[i].prop; i++) {
        if (strlen(K_PROPS[i].prop) == n && !strncmp(K_PROPS[i].prop, prop, n)) return at && !K_PROPS[i].action ? NULL : &K_PROPS[i];
    }
    return NULL;
}

static const PropDef *prop_def(const char *prop) {
    int index;
    return prop_def_at(prop, &index);
}

const char *ed_model_prop_key(const char *prop) {
    const PropDef *d = prop_def(prop);
    return d ? d->key : NULL;
}

// where a property lives: the object, or trigger action `index` (-1 = the first; tas[0] is created
// on demand when `create`, a later index must exist)
static cJSON *prop_holder(cJSON *data, const int prefab, const PropDef *d, const int index, const int create) {
    if (!d->action || !(prefab >= 0 && prefab < EDITOR_PREFAB_COUNT) ||
        !(!strcmp(editor_prefabs[prefab].name, "TRIGGER") || editor_prefab_has(prefab, EP_AI))) return index > 0 ? NULL : data;
    cJSON *tas = cJSON_GetObjectItemCaseSensitive(data, "tas");
    if (index > 0) {
        cJSON *a = cJSON_GetArrayItem(tas, index);
        return cJSON_IsObject(a) ? a : NULL;
    }
    if (!cJSON_IsArray(tas)) {
        if (!create) return NULL;
        cJSON_DeleteItemFromObjectCaseSensitive(data, "tas");
        tas = cJSON_AddArrayToObject(data, "tas");
    }
    cJSON *a = cJSON_GetArrayItem(tas, 0);
    if (!cJSON_IsObject(a)) {
        if (!create) return NULL;
        a = cJSON_CreateObject();
        if (cJSON_GetArraySize(tas)) cJSON_ReplaceItemInArray(tas, 0, a);
        else cJSON_AddItemToArray(tas, a);
    }
    return a;
}

static double prop_default(const PropDef *d, const int prefab) {
    if (!strcmp(d->prop, "score")) return editor_prefab_has(prefab, EP_INTERACT) || editor_prefab_has(prefab, EP_AI) ? 0 : 1;
    if (!strcmp(d->prop, "event")) return editor_prefab_has(prefab, EP_AI) ? 15 : 0;
    return d->def;
}

cJSON *ed_model_prop_get(const cJSON *data, const int prefab, const char *prop) {
    int index;
    const PropDef *d = prop_def_at(prop, &index);
    if (!d) return NULL;
    const cJSON *h = prop_holder((cJSON *) data, prefab, d, index, 0);
    const cJSON *v = h ? G(h, d->key) : NULL;
    const double def = prop_default(d, prefab);
    char buf[64];
    switch (d->kind) {
        case PK_PRESENT: return cJSON_CreateBool(!js_nullish(v));
        case PK_ABSENT: return cJSON_CreateBool(js_nullish(v));
        case PK_TRUTHY:
        case PK_BOOL: return cJSON_CreateBool(js_truthy(v));
        case PK_STR: {
            const char *s = js_truthy(v) ? js_str(v, buf, sizeof(buf)) : NULL;
            return cJSON_CreateString(s ? s : "");
        }
        case PK_COLOR: {
            if (cJSON_IsNumber(v)) {
                snprintf(buf, sizeof(buf), "#%06x", (unsigned) v->valuedouble & 0xffffffu);
                return cJSON_CreateString(buf);
            }
            return cJSON_CreateString(cJSON_IsString(v) ? v->valuestring : d->sdef);
        }
        case PK_RAW: return js_nullish(v) ? cJSON_CreateNumber(def) : cJSON_Duplicate(v, 1);
        case PK_NUMN: return cJSON_CreateNumber(js_nullish(v) ? def : (js_truthy(v) ? js_parse_float(v) : 0));
        case PK_INTN: return cJSON_CreateNumber(js_nullish(v) ? def : (js_truthy(v) ? js_parse_int(v) : 0));
        case PK_INT: return cJSON_CreateNumber(js_truthy(v) ? js_parse_int(v) : def);
        default: return cJSON_CreateNumber(js_truthy(v) ? js_parse_float(v) : def);
    }
}

int ed_model_prop_set(cJSON **data, const int prefab, const char *prop, const cJSON *value) {
    int index;
    const PropDef *d = prop_def_at(prop, &index);
    if (!d || !data || !*data) return 0;
    cJSON *h = prop_holder(*data, prefab, d, index, 1);
    if (!h) return 0;
    const double def = prop_default(d, prefab);
    cJSON *nv = NULL;
    switch (d->kind) {
        case PK_PRESENT: if (js_truthy(value)) nv = cJSON_CreateNumber(1); break;
        case PK_ABSENT: if (!js_truthy(value)) nv = cJSON_CreateNumber(1); break;
        case PK_TRUTHY: if (js_truthy(value)) nv = cJSON_CreateNumber(1); break;
        case PK_BOOL: nv = cJSON_CreateBool(js_truthy(value)); break;
        case PK_STR: {
            char buf[64];
            const char *s = js_str(value, buf, sizeof(buf));
            if (s && s[0]) nv = cJSON_CreateString(s);
            break;
        }
        case PK_COLOR:
            if (cJSON_IsString(value) && strcmp(value->valuestring, d->sdef)) nv = cJSON_CreateString(value->valuestring);
            else if (cJSON_IsNumber(value)) nv = cJSON_Duplicate(value, 1);
            break;
        case PK_RAW:
            // an indexed condition value is stored as given, like v7's GUI.action (serialize writes
            // cv0..2 whenever they are not null, 0 included)
            if (!js_nullish(value) && (index >= 0 || !(cJSON_IsNumber(value) && value->valuedouble == def))) nv = cJSON_Duplicate(value, 1);
            break;
        default: {
            double v = cJSON_IsString(value) ? parse_float_str(value->valuestring) : js_number(value);
            if (d->kind == PK_INT || d->kind == PK_INTN) v = js_parse_int_d(v);
            if (isnan(v)) v = def;
            // a value that loads back as itself: the OR-default kinds cannot hold 0 when the default is not 0
            if (v != def) nv = cJSON_CreateNumber(v);
            break;
        }
    }
    cJSON_DeleteItemFromObjectCaseSensitive(h, d->key);
    if (nv) cJSON_AddItemToObject(h, d->key, nv);
    cJSON *norm = ed_model_object(*data, ED_MODEL_KEEP_UUID | ED_MODEL_EXACT_TRANSFORM);
    if (!norm) return 0;
    cJSON_Delete(*data);
    *data = norm;
    return 1;
}

// ---- trigger actions (tas): the multi-action list of TRIGGER / AI_SPAWNER (doc/v7/editor.md 1.7)
int ed_model_action_count(const cJSON *data) { return cJSON_GetArraySize(G(data, "tas")); }

int ed_model_action_add(cJSON **data) {
    cJSON *tas = cJSON_GetObjectItemCaseSensitive(*data, "tas");
    const int limit = (int) data_num("limits", "triggerActionLimit", 5);
    if (!cJSON_IsArray(tas) || cJSON_GetArraySize(tas) >= limit) return 0;
    cJSON_AddItemToArray(tas, cJSON_CreateObject());
    return 1;
}

int ed_model_action_remove(cJSON **data, const int index) {
    // removeAction: any entry but the first (the UI never offers removing action 0)
    cJSON *tas = cJSON_GetObjectItemCaseSensitive(*data, "tas");
    if (!cJSON_IsArray(tas) || index <= 0 || index >= cJSON_GetArraySize(tas)) return 0;
    cJSON_DeleteItemFromArray(tas, index);
    return 1;
}

cJSON *ed_model_action_get(const cJSON *data, const int index, const char *key) {
    const cJSON *a = cJSON_GetArrayItem(G(data, "tas"), index);
    const cJSON *v = a ? G(a, key) : NULL;
    return v ? cJSON_Duplicate(v, 1) : NULL;
}

int ed_model_action_set(cJSON **data, const int prefab, const int index, const char *key, const cJSON *value) {
    cJSON *a = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(*data, "tas"), index);
    if (!cJSON_IsObject(a)) return 0;
    cJSON_DeleteItemFromObjectCaseSensitive(a, key);
    if (value && !cJSON_IsNull(value)) cJSON_AddItemToObject(a, key, cJSON_Duplicate(value, 1));
    (void) prefab;
    cJSON *norm = ed_model_object(*data, ED_MODEL_KEEP_UUID | ED_MODEL_EXACT_TRANSFORM);
    if (!norm) return 0;
    cJSON_Delete(*data);
    *data = norm;
    return 1;
}

// ---------------------------------------------------------------------------
// JSON.stringify output: maps are written by JavaScript, so numbers print in Number#toString form
// (shortest round-trip digits; cJSON's 15-digit form loses the last bits of some doubles) and
// strings escape as JSON.stringify does. Key order is the document's.

typedef struct {
    char *buf;
    size_t len, cap;
    int oom;
} Out;

static void out_putn(Out *o, const char *s, const size_t n) {
    if (o->oom) return;
    if (o->len + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap : 4096;
        while (o->len + n + 1 > cap) cap *= 2;
        char *nb = realloc(o->buf, cap);
        if (!nb) {
            o->oom = 1;
            return;
        }
        o->buf = nb;
        o->cap = cap;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = 0;
}

static void out_puts(Out *o, const char *s) { out_putn(o, s, strlen(s)); }

void ed_model_number(const double v, char *out, const size_t size) {
    if (!isfinite(v)) {
        snprintf(out, size, "null");
        return;
    }
    if (v == 0) {
        snprintf(out, size, "0");
        return;
    }
    char sci[40];
    int p = 0;
    for (; p < 17; p++) {
        snprintf(sci, sizeof(sci), "%.*e", p, v);
        if (strtod(sci, NULL) == v) break;
    }
    // sci = [-]d[.ddd]e[+-]xx -> digits and the decimal point position k (value = 0.digits * 10^k)
    char digits[24];
    int n = 0;
    const char *c = sci;
    const int neg = *c == '-';
    if (neg) c++;
    for (; *c && *c != 'e'; c++) if (isdigit((unsigned char) *c)) digits[n++] = *c;
    digits[n] = 0;
    const int k = atoi(c + 1) + 1;
    char buf[64];
    size_t j = 0;
    if (neg) buf[j++] = '-';
    if (n <= k && k <= 21) { // integer: digits then zeros
        for (int i = 0; i < n; i++) buf[j++] = digits[i];
        for (int i = n; i < k; i++) buf[j++] = '0';
    } else if (0 < k && k <= 21) {
        for (int i = 0; i < n; i++) {
            if (i == k) buf[j++] = '.';
            buf[j++] = digits[i];
        }
    } else if (-6 < k && k <= 0) {
        buf[j++] = '0';
        buf[j++] = '.';
        for (int i = 0; i < -k; i++) buf[j++] = '0';
        for (int i = 0; i < n; i++) buf[j++] = digits[i];
    } else {
        buf[j++] = digits[0];
        if (n > 1) {
            buf[j++] = '.';
            for (int i = 1; i < n; i++) buf[j++] = digits[i];
        }
        j += (size_t) snprintf(buf + j, sizeof(buf) - j, "e%c%d", k - 1 >= 0 ? '+' : '-', abs(k - 1));
    }
    buf[j] = 0;
    snprintf(out, size, "%s", buf);
}

static void out_string(Out *o, const char *s) {
    out_putn(o, "\"", 1);
    for (const unsigned char *q = (const unsigned char *) s; *q; q++) {
        char esc[8];
        switch (*q) {
            case '"': out_puts(o, "\\\""); break;
            case '\\': out_puts(o, "\\\\"); break;
            case '\b': out_puts(o, "\\b"); break;
            case '\f': out_puts(o, "\\f"); break;
            case '\n': out_puts(o, "\\n"); break;
            case '\r': out_puts(o, "\\r"); break;
            case '\t': out_puts(o, "\\t"); break;
            default:
                if (*q < 0x20) {
                    snprintf(esc, sizeof(esc), "\\u%04x", *q);
                    out_puts(o, esc);
                } else out_putn(o, (const char *) q, 1);
        }
    }
    out_putn(o, "\"", 1);
}

static void out_value(Out *o, const cJSON *v) {
    if (cJSON_IsNumber(v)) {
        char num[64];
        ed_model_number(v->valuedouble, num, sizeof(num));
        out_puts(o, num);
    } else if (cJSON_IsString(v)) out_string(o, v->valuestring);
    else if (cJSON_IsTrue(v)) out_puts(o, "true");
    else if (cJSON_IsFalse(v)) out_puts(o, "false");
    else if (cJSON_IsArray(v)) {
        out_putn(o, "[", 1);
        for (const cJSON *c = v->child; c; c = c->next) {
            if (c != v->child) out_putn(o, ",", 1);
            out_value(o, c);
        }
        out_putn(o, "]", 1);
    } else if (cJSON_IsObject(v)) {
        out_putn(o, "{", 1);
        for (const cJSON *c = v->child; c; c = c->next) {
            if (c != v->child) out_putn(o, ",", 1);
            out_string(o, c->string ? c->string : "");
            out_putn(o, ":", 1);
            out_value(o, c);
        }
        out_putn(o, "}", 1);
    } else out_puts(o, "null");
}

char *ed_model_print(const cJSON *item) {
    Out o = {0};
    out_value(&o, item);
    if (o.oom) {
        free(o.buf);
        return NULL;
    }
    if (!o.buf) return calloc(1, 1);
    return o.buf;
}
