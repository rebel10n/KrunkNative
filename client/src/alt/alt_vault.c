// The alt manager's vault: the profile model, the sealed record and its file (client/include/alt_vault.h).
#include <alt_vault.h>

#include <cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef KRUNKNATIVE_HAVE_TLS
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define ALT_VAULT_VERSION 1
#define ALT_MAX_FILE (1 << 20) // a vault is ~40 KB at most: anything bigger is not one of ours
static const char ALT_MAGIC[8] = {'K', 'N', 'A', 'L', 'T', 'V', '0', '1'};

// ---------------------------------------------------------------- model

void alt_vault_init(AltVault *v) {
    if (v) memset(v, 0, sizeof(*v));
}

void alt_vault_wipe(AltVault *v) {
    if (v) alt_wipe(v, sizeof(*v));
}

int alt_vault_find(const AltVault *v, const char *id) {
    if (!v || !id || !*id) return -1;
    for (int i = 0; i < v->count; i++) {
        if (!strcmp(v->profiles[i].id, id)) return i;
    }
    return -1;
}

static int valid_kind(const int kind) { return kind == ALT_KIND_KN || kind == ALT_KIND_KRUNKER; }

static int new_id(const AltVault *v, char out[ALT_ID_LEN]) {
    static const char digits[] = "0123456789abcdef";
    for (int attempt = 0; attempt < 8; attempt++) {
        uint8_t raw[8];
        if (alt_random_bytes(raw, sizeof(raw)) != 0) return -1;
        out[0] = 'p';
        for (size_t i = 0; i < sizeof(raw); i++) {
            out[1 + 2 * i] = digits[raw[i] >> 4];
            out[2 + 2 * i] = digits[raw[i] & 15];
        }
        out[1 + 2 * sizeof(raw)] = 0;
        alt_wipe(raw, sizeof(raw));
        if (alt_vault_find(v, out) < 0) return 0;
    }
    return -1;
}

int alt_vault_put(AltVault *v, const int kind, const char *label, const char *secret, const int64_t now_ms, char *out_id) {
    if (!v || !valid_kind(kind) || !label || !*label || !secret || !*secret) return -1;
    if (strlen(label) >= ALT_LABEL_MAX || strlen(secret) >= ALT_SECRET_MAX) return -1;
    for (int i = 0; i < v->count; i++) {
        AltProfile *p = &v->profiles[i];
        if (p->kind == kind && !strcmp(p->label, label)) { // the same account signed in again: a fresh secret
            snprintf(p->secret, sizeof(p->secret), "%s", secret);
            if (out_id) snprintf(out_id, ALT_ID_LEN, "%s", p->id);
            return i;
        }
    }
    if (v->count >= ALT_MAX_PROFILES) return -1;
    AltProfile *p = &v->profiles[v->count];
    if (new_id(v, p->id) != 0) {
        alt_wipe(p, sizeof(*p));
        return -1;
    }
    p->kind = kind;
    snprintf(p->label, sizeof(p->label), "%s", label);
    snprintf(p->secret, sizeof(p->secret), "%s", secret);
    p->added_ms = now_ms;
    if (out_id) snprintf(out_id, ALT_ID_LEN, "%s", p->id);
    return v->count++;
}

int alt_vault_remove(AltVault *v, const char *id) {
    const int i = alt_vault_find(v, id);
    if (i < 0) return -1;
    if (!strcmp(v->active_kn, id)) v->active_kn[0] = 0;
    if (!strcmp(v->active_krunker, id)) v->active_krunker[0] = 0;
    for (int j = i; j + 1 < v->count; j++) v->profiles[j] = v->profiles[j + 1];
    v->count--;
    alt_wipe(&v->profiles[v->count], sizeof(v->profiles[v->count])); // the vacated slot keeps no secret
    return 0;
}

int alt_vault_switch(AltVault *v, const char *id) {
    const int i = alt_vault_find(v, id);
    if (i < 0) return -1;
    const AltProfile *p = &v->profiles[i];
    if (p->kind == ALT_KIND_KN) snprintf(v->active_kn, sizeof(v->active_kn), "%s", p->id);
    else snprintf(v->active_krunker, sizeof(v->active_krunker), "%s", p->id);
    return 0;
}

void alt_vault_sign_out(AltVault *v) {
    if (v) v->active_kn[0] = 0;
}

const AltProfile *alt_vault_active(const AltVault *v, const int kind) {
    if (!v) return NULL;
    const char *id = kind == ALT_KIND_KN ? v->active_kn : kind == ALT_KIND_KRUNKER ? v->active_krunker : "";
    const int i = alt_vault_find(v, id);
    return i < 0 ? NULL : &v->profiles[i];
}

// ---------------------------------------------------------------- JSON (plaintext, in memory only)

// Every string in a tree that held secrets is overwritten before the tree is freed.
static void wipe_tree(cJSON *node) {
    for (; node; node = node->next) {
        if (node->valuestring) alt_wipe(node->valuestring, strlen(node->valuestring));
        if (node->child) wipe_tree(node->child);
    }
}

static char *encode_json(const AltVault *v) {
    cJSON *root = cJSON_CreateObject();
    cJSON *profiles = cJSON_CreateArray();
    if (!root || !profiles) {
        cJSON_Delete(root);
        cJSON_Delete(profiles);
        return NULL;
    }
    cJSON_AddNumberToObject(root, "v", ALT_VAULT_VERSION);
    cJSON_AddStringToObject(root, "active_kn", v->active_kn);
    cJSON_AddStringToObject(root, "active_krunker", v->active_krunker);
    cJSON_AddItemToObject(root, "profiles", profiles);
    for (int i = 0; i < v->count; i++) {
        const AltProfile *p = &v->profiles[i];
        cJSON *o = cJSON_CreateObject();
        if (!o) continue;
        cJSON_AddStringToObject(o, "id", p->id);
        cJSON_AddNumberToObject(o, "kind", p->kind);
        cJSON_AddStringToObject(o, "label", p->label);
        cJSON_AddStringToObject(o, "secret", p->secret);
        cJSON_AddNumberToObject(o, "added", (double) p->added_ms);
        cJSON_AddItemToArray(profiles, o);
    }
    char *text = cJSON_PrintUnformatted(root);
    wipe_tree(root);
    cJSON_Delete(root);
    return text;
}

static void copy_field(char *dst, const size_t cap, const cJSON *item, const char *key, int *ok) {
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(item, key);
    if (!cJSON_IsString(s) || !s->valuestring || strlen(s->valuestring) >= cap) {
        *ok = 0;
        return;
    }
    snprintf(dst, cap, "%s", s->valuestring);
}

static int decode_json(AltVault *v, const char *text) {
    cJSON *root = cJSON_Parse(text);
    if (!root) return -1;
    int ok = 1;
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "v");
    if (!cJSON_IsNumber(ver) || ver->valueint != ALT_VAULT_VERSION) ok = 0;
    const cJSON *kn = cJSON_GetObjectItemCaseSensitive(root, "active_kn");
    const cJSON *kr = cJSON_GetObjectItemCaseSensitive(root, "active_krunker");
    const cJSON *profiles = cJSON_GetObjectItemCaseSensitive(root, "profiles");
    if (!cJSON_IsString(kn) || strlen(kn->valuestring) >= ALT_ID_LEN) ok = 0;
    if (!cJSON_IsString(kr) || strlen(kr->valuestring) >= ALT_ID_LEN) ok = 0;
    if (!cJSON_IsArray(profiles) || cJSON_GetArraySize(profiles) > ALT_MAX_PROFILES) ok = 0;
    if (ok) {
        snprintf(v->active_kn, sizeof(v->active_kn), "%s", kn->valuestring);
        snprintf(v->active_krunker, sizeof(v->active_krunker), "%s", kr->valuestring);
        const cJSON *item;
        cJSON_ArrayForEach(item, profiles) {
            AltProfile *p = &v->profiles[v->count];
            int fine = 1;
            copy_field(p->id, sizeof(p->id), item, "id", &fine);
            copy_field(p->label, sizeof(p->label), item, "label", &fine);
            copy_field(p->secret, sizeof(p->secret), item, "secret", &fine);
            const cJSON *kind = cJSON_GetObjectItemCaseSensitive(item, "kind");
            const cJSON *added = cJSON_GetObjectItemCaseSensitive(item, "added");
            if (!cJSON_IsNumber(kind) || !valid_kind(kind->valueint)) fine = 0;
            if (!cJSON_IsNumber(added)) fine = 0;
            if (!fine) {
                alt_wipe(p, sizeof(*p));
                ok = 0;
                break;
            }
            p->kind = kind->valueint;
            p->added_ms = (int64_t) added->valuedouble;
            v->count++;
        }
    }
    wipe_tree(root);
    cJSON_Delete(root);
    if (!ok) return -1;
    if (alt_vault_find(v, v->active_kn) < 0 && v->active_kn[0]) v->active_kn[0] = 0;
    if (alt_vault_find(v, v->active_krunker) < 0 && v->active_krunker[0]) v->active_krunker[0] = 0;
    return 0;
}

// ---------------------------------------------------------------- sealed record

static void put_u32(uint8_t *p, const uint32_t v) {
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) v;
}

static uint32_t get_u32(const uint8_t *p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

int alt_vault_seal(const AltVault *v, const uint8_t key[ALT_KEY_LEN], uint8_t **out, size_t *outlen) {
    if (!v || !key || !out || !outlen) return -1;
    *out = NULL;
    *outlen = 0;
    char *text = encode_json(v);
    if (!text) return -1;
    const size_t n = strlen(text);
    uint8_t *buf = NULL;
    int ok = -1;
    if (n > ALT_MAX_FILE) goto done;
    buf = malloc(ALT_HEADER_LEN + n);
    if (!buf) goto done;
    memcpy(buf, ALT_MAGIC, sizeof(ALT_MAGIC));
    buf[8] = (uint8_t) v->keystore;
    if (alt_random_bytes(buf + 9, ALT_IV_LEN) != 0) goto done;
    put_u32(buf + 21, (uint32_t) n);
    if (alt_aes_gcm_seal(key, buf + 9, buf, ALT_AAD_LEN, (const uint8_t *) text, n, buf + ALT_HEADER_LEN, buf + 25) != 0) goto done;
    *out = buf;
    *outlen = ALT_HEADER_LEN + n;
    buf = NULL;
    ok = 0;
done:
    if (buf) {
        alt_wipe(buf, ALT_HEADER_LEN + n);
        free(buf);
    }
    alt_wipe(text, n);
    free(text);
    return ok;
}

int alt_vault_peek_keystore(const uint8_t *in, const size_t inlen) {
    if (!in || inlen < ALT_HEADER_LEN || memcmp(in, ALT_MAGIC, sizeof(ALT_MAGIC)) != 0) return -1;
    return in[8];
}

int alt_vault_open_bytes(AltVault *v, const uint8_t *in, const size_t inlen, const uint8_t key[ALT_KEY_LEN], char *err, const size_t errn) {
    alt_vault_init(v);
    if (err && errn) err[0] = 0;
    if (!in || !key || inlen < ALT_HEADER_LEN || memcmp(in, ALT_MAGIC, sizeof(ALT_MAGIC)) != 0) {
        if (err && errn) snprintf(err, errn, "not a vault file");
        return -1;
    }
    const size_t n = get_u32(in + 21);
    if (n != inlen - ALT_HEADER_LEN || n > ALT_MAX_FILE) {
        if (err && errn) snprintf(err, errn, "the vault file is damaged");
        return -1;
    }
    char *plain = malloc(n + 1);
    if (!plain) return -1;
    int ok = -1;
    if (alt_aes_gcm_open(key, in + 9, in, ALT_AAD_LEN, in + ALT_HEADER_LEN, n, in + 25, (uint8_t *) plain) != 0) {
        if (err && errn) snprintf(err, errn, "the vault key is wrong, or the vault was changed");
        goto done;
    }
    plain[n] = 0;
    if (decode_json(v, plain) != 0) {
        if (err && errn) snprintf(err, errn, "the vault decrypted but its contents are not valid");
        goto done;
    }
    v->keystore = in[8];
    ok = 0;
done:
    alt_wipe(plain, n + 1);
    free(plain);
    if (ok != 0) alt_vault_wipe(v);
    return ok;
}

// ---------------------------------------------------------------- file

#ifdef KRUNKNATIVE_HAVE_TLS

// 0 read, 1 absent (ENOENT only: any other failure must never look like "no vault yet", or a
// save would replace a vault that is merely unreadable), -1 error
static int read_file(const char *path, uint8_t **out, size_t *outlen) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0 || (size_t) st.st_size > ALT_MAX_FILE + ALT_HEADER_LEN) {
        close(fd);
        return -1;
    }
    const size_t size = (size_t) st.st_size;
    uint8_t *buf = malloc(size ? size : 1);
    size_t got = 0;
    while (buf && got < size) {
        const ssize_t r = read(fd, buf + got, size - got);
        if (r <= 0) break;
        got += (size_t) r;
    }
    close(fd);
    if (!buf || got != size) {
        free(buf);
        return -1;
    }
    *out = buf;
    *outlen = size;
    return 0;
}

int alt_vault_peek_file(const char *path) {
    uint8_t head[ALT_HEADER_LEN];
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    const ssize_t got = read(fd, head, sizeof(head));
    close(fd);
    if (got != (ssize_t) sizeof(head)) return -1;
    return alt_vault_peek_keystore(head, sizeof(head));
}

int alt_vault_load_file(AltVault *v, const char *path, const uint8_t key[ALT_KEY_LEN], char *err, const size_t errn) {
    alt_vault_init(v);
    uint8_t *buf = NULL;
    size_t n = 0;
    const int r = read_file(path, &buf, &n);
    if (r == 1) return 1;
    if (r != 0) {
        if (err && errn) snprintf(err, errn, "cannot read the vault file");
        return -1;
    }
    const int ok = alt_vault_open_bytes(v, buf, n, key, err, errn);
    free(buf);
    return ok == 0 ? 0 : -1;
}

int alt_vault_save_file(const AltVault *v, const char *path, const uint8_t key[ALT_KEY_LEN]) {
    uint8_t *buf = NULL;
    size_t n = 0;
    if (alt_vault_seal(v, key, &buf, &n) != 0) return -1;
    char tmp[600];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) <= 0) goto fail;
    // 0600 from the first byte: the sealed vault is private to the player
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) goto fail;
    size_t put = 0;
    while (put < n) {
        const ssize_t w = write(fd, buf + put, n - put);
        if (w <= 0) break;
        put += (size_t) w;
    }
    const int closed = close(fd);
    if (put != n || closed != 0 || rename(tmp, path) != 0) {
        remove(tmp);
        goto fail;
    }
    alt_wipe(buf, n);
    free(buf);
    return 0;
fail:
    alt_wipe(buf, n);
    free(buf);
    return -1;
}

#elif defined(__EMSCRIPTEN__)

// The web build: the sealed record is one IndexedDB value (client/src/alt/alt_web.c). A put is atomic,
// so no temp file is needed; the path names the record. Only ciphertext is ever stored.
#include <alt_web.h>

int alt_vault_peek_file(const char *path) {
    uint8_t *buf = NULL;
    size_t n = 0;
    const int r = alt_web_blob_get(path, &buf, &n);
    if (r == 1) return 0;
    if (r != 0) return -1;
    const int kind = alt_vault_peek_keystore(buf, n);
    free(buf);
    return kind;
}

int alt_vault_load_file(AltVault *v, const char *path, const uint8_t key[ALT_KEY_LEN], char *err, const size_t errn) {
    alt_vault_init(v);
    uint8_t *buf = NULL;
    size_t n = 0;
    const int r = alt_web_blob_get(path, &buf, &n);
    if (r == 1) return 1;
    if (r != 0) {
        if (err && errn) snprintf(err, errn, "cannot read the vault");
        return -1;
    }
    const int ok = alt_vault_open_bytes(v, buf, n, key, err, errn);
    free(buf);
    return ok == 0 ? 0 : -1;
}

int alt_vault_save_file(const AltVault *v, const char *path, const uint8_t key[ALT_KEY_LEN]) {
    uint8_t *buf = NULL;
    size_t n = 0;
    if (alt_vault_seal(v, key, &buf, &n) != 0) return -1;
    const int ok = alt_web_blob_put(path, buf, n);
    alt_wipe(buf, n);
    free(buf);
    return ok;
}

#else // no cipher in this build: no vault file is ever read or written

int alt_vault_peek_file(const char *path) {
    (void) path;
    return 0;
}
int alt_vault_load_file(AltVault *v, const char *path, const uint8_t key[ALT_KEY_LEN], char *err, const size_t errn) {
    (void) key;
    alt_vault_init(v);
    (void) path;
    if (err && errn) snprintf(err, errn, "this build has no encryption");
    return -1;
}
int alt_vault_save_file(const AltVault *v, const char *path, const uint8_t key[ALT_KEY_LEN]) {
    (void) v;
    (void) path;
    (void) key;
    return -1;
}

#endif
