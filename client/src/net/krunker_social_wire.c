// The hub page's krunker.io social socket, its framing (client/include/krunker_social.h): the page's cJSON frames
// as the game socket's msgpack + ahNum frames, and back. No sockets here: krunker_social.c and the unit test
// (client/tests/krunker_social_test.c) drive it.
#include <krunker_social.h>
#include <krunker_build.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void krunker_social_wire_init(KrunkerSocialWire *w) {
    memset(w, 0, sizeof(*w));
    krunker_session_init(&w->codec, KRUNKER_AHKEY);
    w->socket_id = -1;
}

// ---------------------------------------------------------------- cJSON -> KrunkerValue

// The values of one frame, in one allocation list (freed together after the encode)
typedef struct {
    void **blocks;
    size_t n, cap;
} Pool;

static void *pool_alloc(Pool *p, const size_t size) {
    if (p->n == p->cap) {
        const size_t cap = p->cap ? p->cap * 2 : 16;
        void **grown = realloc(p->blocks, cap * sizeof(void *));
        if (!grown) return NULL;
        p->blocks = grown;
        p->cap = cap;
    }
    void *b = calloc(1, size ? size : 1);
    if (b) p->blocks[p->n++] = b;
    return b;
}

static void pool_free(Pool *p) {
    for (size_t i = 0; i < p->n; i++) free(p->blocks[i]);
    free(p->blocks);
}

// @msgpack/msgpack on a JS value: null (and undefined) nil, booleans, numbers (integers as ints, the rest float64:
// krunker_proto_encode), strings, arrays, plain objects as maps with string keys
static int to_value(const cJSON *j, KrunkerValue *out, Pool *p, const int depth) {
    if (depth > KRUNKER_PROTO_MAX_DEPTH) return 0;
    if (!j || cJSON_IsNull(j) || cJSON_IsInvalid(j)) {
        *out = KRUNKER_V_NIL;
    } else if (cJSON_IsBool(j)) {
        *out = KRUNKER_V_BOOL(cJSON_IsTrue(j));
    } else if (cJSON_IsNumber(j)) {
        *out = isfinite(j->valuedouble) ? KRUNKER_V_NUM(j->valuedouble) : KRUNKER_V_NIL; // JSON has no NaN / Infinity
    } else if (cJSON_IsString(j)) {
        *out = KRUNKER_V_STR(j->valuestring ? j->valuestring : "");
    } else if (cJSON_IsArray(j) || cJSON_IsObject(j)) {
        const int object = cJSON_IsObject(j);
        const int n = cJSON_GetArraySize(j);
        KrunkerValue *items = pool_alloc(p, (size_t) n * (object ? 2 : 1) * sizeof(KrunkerValue));
        if (!items) return 0;
        int i = 0;
        const cJSON *c;
        cJSON_ArrayForEach(c, j) {
            if (object) {
                items[2 * i] = KRUNKER_V_STR(c->string ? c->string : "");
                if (!to_value(c, &items[2 * i + 1], p, depth + 1)) return 0;
            } else if (!to_value(c, &items[i], p, depth + 1)) {
                return 0;
            }
            i++;
        }
        *out = object ? KRUNKER_V_MAP(items, (uint32_t) n) : KRUNKER_V_ARRAY(items, (uint32_t) n);
    } else {
        *out = KRUNKER_V_NIL;
    }
    return 1;
}

static int encode(KrunkerSocialWire *w, const char *type, const cJSON *first, const KrunkerValue *head, const size_t nhead,
                  KrunkerBuf *out) {
    Pool p = {0};
    size_t n = nhead;
    for (const cJSON *c = first; c; c = c->next) n++;
    KrunkerValue *args = pool_alloc(&p, n * sizeof(KrunkerValue));
    int rc = args ? KRUNKER_PROTO_OK : KRUNKER_PROTO_ENOMEM;
    size_t i = 0;
    for (; rc == KRUNKER_PROTO_OK && i < nhead; i++) args[i] = head[i];
    for (const cJSON *c = first; rc == KRUNKER_PROTO_OK && c; c = c->next, i++)
        if (!to_value(c, &args[i], &p, 2)) rc = KRUNKER_PROTO_EINVAL;
    if (rc == KRUNKER_PROTO_OK) rc = krunker_proto_encode(&w->codec, type, args, n, out);
    pool_free(&p);
    return rc;
}

int krunker_social_wire_encode(KrunkerSocialWire *w, const cJSON *frame, KrunkerBuf *out) {
    const cJSON *type = cJSON_GetArrayItem(frame, 0);
    if (!w || !out || !cJSON_IsArray(frame) || !cJSON_IsString(type) || !type->valuestring[0]) return KRUNKER_PROTO_EINVAL;
    return encode(w, type->valuestring, type->next, NULL, 0, out);
}

int krunker_social_wire_rpc(KrunkerSocialWire *w, const char *name, const cJSON *args, KrunkerBuf *out) {
    if (!w || !out || !name || !name[0] || (args && !cJSON_IsArray(args))) return -1;
    const int id = w->rpc_counter;
    const KrunkerValue head[2] = {KRUNKER_V_NUM(id), KRUNKER_V_STR(name)};
    if (encode(w, krunker_c2s_wire(KRUNKER_C2S_RPC), args ? args->child : NULL, head, 2, out) != KRUNKER_PROTO_OK) return -1;
    w->rpc_counter++; // rpcCounter++ (the id is taken even when no answer comes)
    snprintf(w->rpc_names[id % 8], sizeof(w->rpc_names[0]), "%s", name);
    return id;
}

// ---------------------------------------------------------------- KrunkerValue -> cJSON, the server's frames

static cJSON *to_json(const KrunkerValue *v) {
    KrunkerBuf buf = {0};
    cJSON *j = krunker_value_to_json(v, &buf) == KRUNKER_PROTO_OK && buf.data ? cJSON_ParseWithLength((const char *) buf.data, buf.len) : NULL;
    krunker_buf_free(&buf);
    return j ? j : cJSON_CreateNull();
}

static int is(const KrunkerFrame *f, const KrunkerS2C id) {
    const char *wire = krunker_s2c_wire(id);
    return wire && f->name_len == strlen(wire) && !memcmp(f->name, wire, f->name_len);
}

// the widget's challengejson: {algorithm, challenge, salt, signature, maxnumber}, or a JSON string of it
static int captcha_begin(KrunkerSocialWire *w, const KrunkerValue *c) {
    const KrunkerValue *obj = c;
    cJSON *j = NULL;
    char alg[32] = "", chal[160] = "", salt[512] = "", sig[512] = "";
    long max = 0;
    if (krunker_is(c, KRUNKER_STR)) j = cJSON_Parse(krunker_str(c));
    if (j) {
        const char *s;
        if ((s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "algorithm")))) snprintf(alg, sizeof(alg), "%s", s);
        if ((s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "challenge")))) snprintf(chal, sizeof(chal), "%s", s);
        if ((s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "salt")))) snprintf(salt, sizeof(salt), "%s", s);
        if ((s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "signature")))) snprintf(sig, sizeof(sig), "%s", s);
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(j, "maxnumber");
        if (cJSON_IsNumber(m)) max = (long) m->valuedouble;
        cJSON_Delete(j);
    } else if (krunker_is(obj, KRUNKER_MAP)) {
        const char *s;
        if ((s = krunker_str(krunker_get(obj, "algorithm")))) snprintf(alg, sizeof(alg), "%s", s);
        if ((s = krunker_str(krunker_get(obj, "challenge")))) snprintf(chal, sizeof(chal), "%s", s);
        if ((s = krunker_str(krunker_get(obj, "salt")))) snprintf(salt, sizeof(salt), "%s", s);
        if ((s = krunker_str(krunker_get(obj, "signature")))) snprintf(sig, sizeof(sig), "%s", s);
        max = (long) krunker_num(krunker_get(obj, "maxnumber"), krunker_num(krunker_get(obj, "maxNumber"), 0));
    }
    w->solving = krunker_altcha_begin(&w->altcha, alg, chal, salt, sig, max) == 0;
    w->captcha_shown = w->solving;
    return w->solving;
}

KrunkerSocialDecode krunker_social_wire_decode(KrunkerSocialWire *w, const void *data, const size_t len, cJSON **frame) {
    if (frame) *frame = NULL;
    KrunkerFrame f;
    if (!w || krunker_proto_decode(data, len, 0, &f) != KRUNKER_PROTO_OK) return KSW_BAD;
    KrunkerSocialDecode rc = KSW_NONE;
    if (is(&f, KRUNKER_S2C_IO_INIT)) {
        w->socket_id = krunker_num(krunker_frame_arg(&f, 0), -1);
    } else if (is(&f, KRUNKER_S2C_cpt)) {
        rc = captcha_begin(w, krunker_frame_arg(&f, 0)) ? KSW_CAPTCHA : KSW_CAPTCHA_FAIL;
    } else if (is(&f, KRUNKER_S2C_cptR)) {
        w->solving = w->captcha_shown = 0; // _hideCaptcha
    } else if (is(&f, KRUNKER_S2C_RPC_SUCCESS) || is(&f, KRUNKER_S2C_RPC_ERROR)) {
        // the rpc's promise settles; the hub awaits LOGIN for nothing but its order
    } else {
        if (is(&f, KRUNKER_S2C_ERROR)) w->solving = w->captcha_shown = 0; // _hideCaptcha, then the "error" event
        cJSON *out = cJSON_CreateArray();
        cJSON_AddItemToArray(out, cJSON_CreateString(f.name));
        for (uint32_t i = 0; i < f.argc; i++) cJSON_AddItemToArray(out, to_json(&f.args[i]));
        if (frame) *frame = out;
        else cJSON_Delete(out);
        rc = KSW_PAGE;
    }
    krunker_frame_free(&f);
    return rc;
}

int krunker_social_wire_captcha_step(KrunkerSocialWire *w, const long budget, KrunkerBuf *out) {
    if (!w || !w->solving) return -1;
    char *payload = NULL;
    const int rc = krunker_altcha_step(&w->altcha, budget, &payload);
    if (rc == 0) return 0;
    w->solving = 0;
    if (rc < 0 || !payload) {
        free(payload);
        w->captcha_shown = 0; // the failure branch: _hideCaptcha, then the "error" event
        return -1;
    }
    // the widget's input value, sent as cptR (no ahNum step: krunker_proto_ahnum_exempt); the modal stays up until
    // the server's own cptR (showCaptcha's third argument is false)
    const KrunkerValue arg = KRUNKER_V_STR(payload);
    const int ok = krunker_proto_encode(&w->codec, krunker_c2s_wire(KRUNKER_C2S_cptR), &arg, 1, out) == KRUNKER_PROTO_OK;
    free(payload);
    return ok ? 1 : -1;
}
