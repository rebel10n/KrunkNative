// The web vault's browser half (client/include/alt_web.h): SubtleCrypto AES-256-GCM under a
// non-extractable master key in IndexedDB, and the sealed vault record in the same database. The
// page's JavaScript here is crypto and storage glue only (no UI): the same EM_JS / EM_ASYNC_JS
// pattern as web_dom.c's storage glue. Emscripten only: the native build never compiles this.
#ifdef __EMSCRIPTEN__

#include <alt_web.h>

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

#define ALT_WEB_DB_DEFAULT "krunknative-alt"
#define ALT_WEB_MAX_CALL (1u << 30) // a vault is ~40 KB: anything near this is a bug, not a vault

// Page-side state, on Module.knAlt: the database name and handle, the master CryptoKey, the last
// result (a seal or open output, or a stored record) that C copies out right after the call.
EM_JS(void, kn_alt_js_install, (void), {
    if (Module.knAlt) return;
    var A = Module.knAlt = {dbName: 'krunknative-alt', db: null, key: null, result: null, blob: null};
    A.open = function () {
        if (A.db) return Promise.resolve(A.db);
        return new Promise(function (res, rej) {
            var r = indexedDB.open(A.dbName, 1);
            r.onupgradeneeded = function () { r.result.createObjectStore('kv'); };
            r.onsuccess = function () { A.db = r.result; res(A.db); };
            r.onerror = function () { rej(r.error); };
            r.onblocked = function () { rej(new Error('IndexedDB blocked')); };
        });
    };
    A.put = function (k, v) {
        return A.open().then(function (db) {
            return new Promise(function (res, rej) {
                var tx = db.transaction('kv', 'readwrite');
                tx.objectStore('kv').put(v, k);
                tx.oncomplete = function () { res(0); };
                tx.onerror = function () { rej(tx.error); };
                tx.onabort = function () { rej(tx.error); };
            });
        });
    };
    A.get = function (k) {
        return A.open().then(function (db) {
            return new Promise(function (res, rej) {
                var q = db.transaction('kv', 'readonly').objectStore('kv').get(k);
                q.onsuccess = function () { res(q.result); };
                q.onerror = function () { rej(q.error); };
            });
        });
    };
});

EM_JS(int, kn_alt_js_available, (void), {
    try {
        return typeof crypto !== 'undefined' && crypto.subtle && typeof indexedDB !== 'undefined' && indexedDB ? 1 : 0;
    } catch (e) { return 0; }
});

EM_JS(int, kn_alt_js_random, (uint8_t *dst, uint32_t n), {
    try { crypto.getRandomValues(HEAPU8.subarray(dst, dst + n)); return 0; } catch (e) { return -1; }
});

EM_JS(void, kn_alt_js_use_database, (const char *name), {
    var A = Module.knAlt;
    A.dbName = name ? UTF8ToString(name) : 'krunknative-alt';
    if (A.db) { A.db.close(); A.db = null; }
});

EM_ASYNC_JS(int, kn_alt_js_drop_database, (void), {
    var A = Module.knAlt;
    if (A.db) { A.db.close(); A.db = null; }
    A.key = null;
    // "blocked" only means another connection is still closing: the delete goes on and succeeds once it
    // has, so it is not a failure; a delete still pending after 10 s is
    return await new Promise(function (res) {
        var r = indexedDB.deleteDatabase(A.dbName);
        var t = setTimeout(function () { res(-1); }, 10000);
        r.onsuccess = function () { clearTimeout(t); res(0); };
        r.onerror = function () { clearTimeout(t); res(-1); };
    });
});

// A new non-extractable AES-256-GCM key, stored under "master" and kept as the page's master key.
EM_ASYNC_JS(int, kn_alt_js_key_make, (void), {
    var A = Module.knAlt;
    try {
        var key = await crypto.subtle.generateKey({name: 'AES-GCM', length: 256}, false, ['encrypt', 'decrypt']);
        await A.put('master', key);
        A.key = key;
        return 0;
    } catch (e) { return -1; }
});

EM_ASYNC_JS(int, kn_alt_js_key_load, (void), {
    var A = Module.knAlt;
    try {
        var key = await A.get('master');
        if (!key) return -1;
        A.key = key;
        return 0;
    } catch (e) { return -1; }
});

// 1 when the master key's bytes can be exported (never, for a non-extractable key), 0 when they cannot.
EM_ASYNC_JS(int, kn_alt_js_key_extractable, (void), {
    var A = Module.knAlt;
    if (!A.key) return -1;
    try { await crypto.subtle.exportKey('raw', A.key); return 1; } catch (e) { return 0; }
});

EM_JS(void, kn_alt_js_key_forget, (void), { Module.knAlt.key = null; });

EM_JS(int, kn_alt_js_key_loaded, (void), { return Module.knAlt.key ? 1 : 0; });

// seal: ct || tag into the result (length ptlen + 16); open: the plaintext (length ctlen).
EM_ASYNC_JS(int, kn_alt_js_seal, (const uint8_t *iv, const uint8_t *aad, uint32_t aadlen, const uint8_t *pt, uint32_t ptlen), {
    var A = Module.knAlt;
    var key = A.key;
    if (!key) return -1;
    try {
        var params = {name: 'AES-GCM', iv: HEAPU8.slice(iv, iv + 12), tagLength: 128};
        if (aadlen) params.additionalData = HEAPU8.slice(aad, aad + aadlen);
        var data = HEAPU8.slice(pt, pt + ptlen);
        A.result = new Uint8Array(await crypto.subtle.encrypt(params, key, data));
        return A.result.length;
    } catch (e) { return -1; }
});

EM_ASYNC_JS(int, kn_alt_js_open, (const uint8_t *iv, const uint8_t *aad, uint32_t aadlen, const uint8_t *ct, uint32_t ctlen,
                                  const uint8_t *tag), {
    var A = Module.knAlt;
    var key = A.key;
    if (!key) return -1;
    try {
        var params = {name: 'AES-GCM', iv: HEAPU8.slice(iv, iv + 12), tagLength: 128};
        if (aadlen) params.additionalData = HEAPU8.slice(aad, aad + aadlen);
        var both = new Uint8Array(ctlen + 16);
        both.set(HEAPU8.subarray(ct, ct + ctlen), 0);
        both.set(HEAPU8.subarray(tag, tag + 16), ctlen);
        // a failed tag check throws: no plaintext is kept
        A.result = new Uint8Array(await crypto.subtle.decrypt(params, key, both));
        return A.result.length;
    } catch (e) { A.result = null; return -1; }
});

EM_JS(void, kn_alt_js_result_copy, (uint8_t *dst, uint32_t off, uint32_t n), {
    HEAPU8.set(Module.knAlt.result.subarray(off, off + n), dst);
});

// The last result (a plaintext after an open) and the last record are zeroed and dropped once C has
// copied them, so no decrypted bytes stay reachable from the page's JavaScript.
EM_JS(void, kn_alt_js_result_clear, (void), {
    var A = Module.knAlt;
    if (A.result) { A.result.fill(0); A.result = null; }
    if (A.blob) { A.blob.fill(0); A.blob = null; }
});

// stored records: get returns the length (copied out by kn_alt_js_blob_copy), -1 absent, -2 error
EM_ASYNC_JS(int, kn_alt_js_blob_get, (const char *name), {
    var A = Module.knAlt;
    try {
        var v = await A.get(UTF8ToString(name));
        if (v === undefined) return -1;
        A.blob = new Uint8Array(v);
        return A.blob.length;
    } catch (e) { return -2; }
});

EM_JS(void, kn_alt_js_blob_copy, (uint8_t *dst), { HEAPU8.set(Module.knAlt.blob, dst); });

EM_ASYNC_JS(int, kn_alt_js_blob_put, (const char *name, const uint8_t *buf, uint32_t n), {
    var A = Module.knAlt;
    try {
        var copy = HEAPU8.slice(buf, buf + n);
        await A.put(UTF8ToString(name), copy);
        return 0;
    } catch (e) { return -1; }
});

// One browser call at a time: a second one returns busy at once instead of overlapping the first.
static int g_busy;

static int begin(void) {
    if (g_busy) return -1;
    g_busy = 1;
    kn_alt_js_install();
    return 0;
}

static void finish(void) {
    kn_alt_js_result_clear();
    g_busy = 0;
}

static void wipe_out(uint8_t *p, size_t n) {
    if (p && n) alt_wipe(p, n);
}

// ---------------------------------------------------------------- public

int alt_web_available(void) { return kn_alt_js_available(); }

int alt_web_random(void *out, size_t n) {
    if (!out && n) return -1;
    uint8_t *p = out;
    while (n) {
        const uint32_t chunk = n > 65536 ? 65536 : (uint32_t) n;
        if (kn_alt_js_random(p, chunk) != 0) return -1;
        p += chunk;
        n -= chunk;
    }
    return 0;
}

int alt_web_key_make(void) {
    if (begin() != 0) return -1;
    const int r = kn_alt_js_key_make();
    finish();
    return r == 0 ? 0 : -1;
}

int alt_web_key_load(void) {
    if (begin() != 0) return -1;
    const int r = kn_alt_js_key_load();
    finish();
    return r == 0 ? 0 : -1;
}

void alt_web_key_forget(void) {
    kn_alt_js_install();
    kn_alt_js_key_forget();
}

int alt_web_seal(const uint8_t iv[ALT_IV_LEN], const uint8_t *aad, size_t aadlen, const uint8_t *pt, size_t ptlen,
                 uint8_t *ct, uint8_t tag[ALT_TAG_LEN]) {
    if (!iv || !tag || (ptlen && (!pt || !ct)) || (aadlen && !aad) || aadlen > ALT_WEB_MAX_CALL || ptlen > ALT_WEB_MAX_CALL) return -1;
    if (begin() != 0) return -1;
    const int n = kn_alt_js_seal(iv, aad, (uint32_t) aadlen, pt, (uint32_t) ptlen);
    int ok = -1;
    if (n == (int) (ptlen + ALT_TAG_LEN)) {
        if (ptlen) kn_alt_js_result_copy(ct, 0, (uint32_t) ptlen);
        kn_alt_js_result_copy(tag, (uint32_t) ptlen, ALT_TAG_LEN);
        ok = 0;
    }
    finish();
    return ok;
}

int alt_web_open(const uint8_t iv[ALT_IV_LEN], const uint8_t *aad, size_t aadlen, const uint8_t *ct, size_t ctlen,
                 const uint8_t tag[ALT_TAG_LEN], uint8_t *pt) {
    if (!iv || !tag || (ctlen && (!ct || !pt)) || (aadlen && !aad) || aadlen > ALT_WEB_MAX_CALL || ctlen > ALT_WEB_MAX_CALL) {
        wipe_out(pt, ctlen);
        return -1;
    }
    if (begin() != 0) {
        wipe_out(pt, ctlen);
        return -1;
    }
    const int n = kn_alt_js_open(iv, aad, (uint32_t) aadlen, ct, (uint32_t) ctlen, tag);
    int ok = -1;
    if (n == (int) ctlen) {
        if (ctlen) kn_alt_js_result_copy(pt, 0, (uint32_t) ctlen);
        ok = 0;
    } else {
        wipe_out(pt, ctlen); // a refused open leaves no plaintext behind
    }
    finish();
    return ok;
}

int alt_web_blob_get(const char *name, uint8_t **out, size_t *outlen) {
    if (!name || !out || !outlen) return -1;
    *out = NULL;
    *outlen = 0;
    if (begin() != 0) return -1;
    const int n = kn_alt_js_blob_get(name);
    int r = -1;
    if (n == -1) {
        r = 1;
    } else if (n >= 0 && (uint32_t) n <= ALT_WEB_MAX_CALL) {
        uint8_t *buf = malloc(n ? (size_t) n : 1);
        if (buf) {
            if (n) kn_alt_js_blob_copy(buf);
            *out = buf;
            *outlen = (size_t) n;
            r = 0;
        }
    }
    finish();
    return r;
}

int alt_web_blob_put(const char *name, const uint8_t *buf, size_t n) {
    if (!name || (n && !buf) || n > ALT_WEB_MAX_CALL) return -1;
    if (begin() != 0) return -1;
    const int r = kn_alt_js_blob_put(name, buf, (uint32_t) n);
    finish();
    return r == 0 ? 0 : -1;
}

int alt_web_use_database(const char *name) {
    kn_alt_js_install();
    kn_alt_js_use_database(name);
    return 0;
}

int alt_web_drop_database(void) {
    if (begin() != 0) return -1;
    const int r = kn_alt_js_drop_database();
    finish();
    return r == 0 ? 0 : -1;
}

// Used by alt_web_test.c: 1 when the master key could be exported (a failure), 0 when it cannot.
int alt_web_key_exportable_for_test(void) {
    if (begin() != 0) return -1;
    const int r = kn_alt_js_key_extractable();
    finish();
    return r;
}

int alt_web_key_loaded_for_test(void) {
    kn_alt_js_install();
    return kn_alt_js_key_loaded();
}

#endif
