/*
 * krunker_cheat_test.c - the official servers' CHEAT_CHECK (`cc` -> `ccr`):
 * keystream, XOR, base64, the double-rounded FNV-1a, full cc -> ccr round trips
 * through the QuickJS sandbox, and the sandbox's limits.
 *
 * Vectors are derived from the algorithm written up in krunker_cheat.c (never by
 * running Krunker's JS). Hand checks for the first bytes / known values:
 *   keystream("abc12345")[0..3] = ('a'^'K')+0, ('b'^'R')+1, ('c'^'U')+2, ('1'^'N')+3
 *                               = 0x2a, 0x31, 0x38, 0x82
 *   keystream("é€😀")[1]        = (0x20ac ^ 'R') + 1 & 0xff = (0xac ^ 0x52) + 1 = 0xff
 *   fnv("") = offset 811c9dc5; fnv("a") = e40c292c (the textbook FNV-1a value:
 *   the first product is exact, its 2 dropped low bits are 0)
 * and the rest from an independent IEEE-double model of the same steps
 * (`h = toInt32(16777619.0 * toInt32(h ^ c))`, Python floats). Three strings
 * where the double multiply and Math.imul disagree pin the rounding.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* clock_gettime under -std=c11 */
#endif

#include "krunker_cheat.h"
#ifndef __EMSCRIPTEN__
#include "krunker_cheat_internal.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                   \
            fputc('\n', stderr);                            \
            g_fail++;                                       \
        }                                                   \
    } while (0)

static void hex(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02x", b[i]);
    out[2 * n] = '\0';
}

static void test_keystream(void)
{
    static const struct { const char *nonce; size_t len; const char *hex; } v[] = {
        { "abc12345", 24, "2a3138827d056c71282a326e738a8d15434e34858d767792" },
        { "n0nce", 12, "25633d30326268382a2f2f6d" },
        /* UTF-16 units e9, 20ac, d83d, de00: indexing by unit, low byte only */
        { "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80", 8, "a2ff6a51a6a47566" },
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        uint8_t ks[64];
        char got[129];
        krunker_cheat_keystream(v[i].nonce, ks, v[i].len);
        hex(ks, v[i].len, got);
        CHECK(strcmp(got, v[i].hex) == 0, "keystream(%s) = %s, want %s", v[i].nonce, got, v[i].hex);
    }
    /* the +i term wraps at 256 */
    /* the +i term wraps at 256: ks[256] = (a ^ salt[6]) = ks[6] - 6; the period is lcm(10, 256) */
    uint8_t ks[1300];
    krunker_cheat_keystream("a", ks, 1300);
    CHECK(ks[256] == (uint8_t)(ks[6] - 6) && ks[1280] == ks[0] && ks[1286] == ks[6],
          "keystream wraps mod 256, period 1280");
    CHECK(ks[0] == 0x2a && ks[10] == (uint8_t)(0x2a + 10), "keystream period = salt length");

    /* XOR is an involution and works in place */
    const char *msg = "{\"r\":1,\"s\":\"hello\"}";
    size_t n = strlen(msg);
    uint8_t a[64], b[64];
    krunker_cheat_xor("Zm9vYmFyYmF6", (const uint8_t *)msg, a, n);
    memcpy(b, a, n);
    krunker_cheat_xor("Zm9vYmFyYmF6", b, b, n);
    CHECK(memcmp(b, msg, n) == 0, "xor round trip");
    CHECK(memcmp(a, msg, n) != 0, "xor changes the bytes");
}

static void test_base64(void)
{
    static const struct { const char *raw, *b64; } v[] = {
        { "", "" }, { "f", "Zg==" }, { "fo", "Zm8=" }, { "foo", "Zm9v" },
        { "foob", "Zm9vYg==" }, { "fooba", "Zm9vYmE=" }, { "foobar", "Zm9vYmFy" },
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        char *e = krunker_cheat_b64_encode((const uint8_t *)v[i].raw, strlen(v[i].raw));
        CHECK(e && strcmp(e, v[i].b64) == 0, "b64 encode(%s) = %s", v[i].raw, e ? e : "(null)");
        free(e);
        size_t n = 99;
        uint8_t *d = krunker_cheat_b64_decode(v[i].b64, &n);
        CHECK(d && n == strlen(v[i].raw) && memcmp(d, v[i].raw, n) == 0, "b64 decode(%s)", v[i].b64);
        free(d);
    }
    /* atob's forgiving-base64: whitespace skipped, padding optional */
    size_t n = 0;
    uint8_t *d = krunker_cheat_b64_decode(" Zm9v\nYmE ", &n);
    CHECK(d && n == 5 && memcmp(d, "fooba", 5) == 0, "b64 decode with whitespace / no padding");
    free(d);
    d = krunker_cheat_b64_decode("/w==", &n);
    CHECK(d && n == 1 && d[0] == 0xff, "b64 decode /w==");
    free(d);
    /* ...and where atob throws (the game then sends no ccr) */
    static const char *bad[] = { "Z", "Zm9v=", "Zg=a", "Zm9v====", "Zm$v", "Zg===" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        d = krunker_cheat_b64_decode(bad[i], &n);
        CHECK(d == NULL, "b64 decode(%s) must fail", bad[i]);
        free(d);
    }
}

static void test_fnv(void)
{
    static const struct { const char *s, *want, *imul; } v[] = {
        { "", "811c9dc5", "811c9dc5" },
        { "a", "e40c292c", "e40c292c" },
        { "abc12345{\"r\":1}", "c3639e34", "69b94211" },
        { "1234567890{\"r\":null,\"e\":{\"_eval\":\"x\"}}", "cf36480f", "39516f43" },
        /* é € 😀 -> units e9 20ac d83d de00 */
        { "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80", "0c5e230c", "0c5e230d" },
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        char got[9];
        krunker_cheat_fnv1a_js(v[i].s, got);
        CHECK(strcmp(got, v[i].want) == 0, "fnv(%s) = %s, want %s (imul would be %s)", v[i].s,
              got, v[i].want, v[i].imul);
    }
}

/* One cc -> ccr. want_rc 0: compare the ccr; otherwise answer() must fail. */
static void cc_case(const char *name, const char *nonce, const char *cipher, int want_ok,
                    const char *want_b64, const char *want_fnv)
{
    KrunkerCcr ccr;
    int rc = krunker_cheat_answer(nonce, cipher, &ccr);
    if (!want_ok) {
        CHECK(rc != 0, "%s: expected no ccr, got rc %d", name, rc);
        if (rc == 0)
            krunker_ccr_free(&ccr);
        return;
    }
    CHECK(rc == 0, "%s: answer rc %d", name, rc);
    if (rc != 0)
        return;
    CHECK(strcmp(ccr.nonce, nonce) == 0, "%s: nonce echoed", name);
    CHECK(strcmp(ccr.payload_b64, want_b64) == 0, "%s: payload %s, want %s", name, ccr.payload_b64, want_b64);
    CHECK(strcmp(ccr.fnv, want_fnv) == 0, "%s: fnv %s, want %s", name, ccr.fnv, want_fnv);

    /* the server's side: decrypt the payload back and re-check the hash */
    size_t n = 0;
    uint8_t *plain = krunker_cheat_b64_decode(ccr.payload_b64, &n);
    CHECK(plain != NULL, "%s: payload decodes", name);
    if (plain) {
        krunker_cheat_xor(nonce, plain, plain, n);
        plain[n] = '\0';
        size_t cat_len = strlen(nonce) + (n ? n : 9) + 1;
        char *cat = malloc(cat_len);
        snprintf(cat, cat_len, "%s%s", nonce, n ? (char *)plain : "undefined");
        char h[9];
        krunker_cheat_fnv1a_js(cat, h);
        CHECK(strcmp(h, ccr.fnv) == 0, "%s: hash of decrypted payload", name);
        free(cat);
        free(plain);
    }
    krunker_ccr_free(&ccr);
}

static void test_round_trip(void)
{
    const char *N = "Zm9vYmFyYmF6";
    /* JSON "\"1+1\"" -> R = 2 */
    cc_case("1+1", N, "M3FFCjQ=", 1, "Iw==", "b6e0d580");
    /* "({a:[1,\"x\",true,null]})" -> object, key order and literals */
    cc_case("object", N, "M2gVWiw4KwF8FW8zORz0Jm4mNSNTOEXSVF6n", 1,
            "amIPGSw4KwECTzVDb0L1MTctbCFKCVQ=", "3272a81b");
    /* navigator.webdriver: undefined in the live page (the game redefines it), so
     * JSON.stringify gives undefined: payload "" and the hash covers "undefined" */
    cc_case("webdriver", N, "My4PTX8Ee1lPRTkYflLkJnI1fD8E", 1, "", "70e950a8");
    cc_case("undefined", N, "MzUAX3MFc0NFUzU=", 1, "", "70e950a8");
    /* throw new Error("boom") -> {r:null,e:{_eval:"boom"}} */
    cc_case("throw", N, "MzQGSXkUOkNFQDcqaULvJjMfOy9JO0TTC16n", 1,
            "amIcGSwNb0FMGzUKOQr7dkQmbyxKdhOtSxjqKRY2Vg==", "34a5eea0");
    /* throw 5 -> (5).message is undefined -> "eval_error" */
    cc_case("throw 5", N, "MzQGSXkUOhgC", 1,
            "amIcGSwNb0FMGzUKOQr7dkQmbyxKdhOtTAHkKGsuWThDNg1VUQ==", "b543ad17");
    /* throw null -> null.message throws in the catch: the game sends no ccr */
    cc_case("throw null", N, "MzQGSXkUOkNVW3tN", 0, NULL, NULL);
    /* non-ASCII: source "hé😀" -> "hé😀" (UTF-8 bytes; 5 UTF-16 units) */
    cc_case("unicode", N, "MxxMU0o/bx0QUi4zR0XkbCgnRRFTMEy/GSunZg==", 1, "Myitkub8gq0C", "f48744d0");
#ifndef __EMSCRIPTEN__
    /* the native sandbox has no require/std/os/process/module (node, which runs the
     * EM_JS path of this test, does have process) */
    cc_case("typeof", N,
            "MxsaQmYGdUsARXIeblnyMTdjbTRWMUbpCQTxIBhrXzNcIUBODDjtcBocRBJb5CS4Oy3LNCUrMK1jHzjAKBPMVz/wMhwwFg6FPQPV/n9Wdw==",
            1, "MzUAX3MFc0NFUzsadVTlMnItfCkKIUfrTBHsKlEvBz9CIEpORTn7OBYdUwZb7Sv2LjuG", "f56beb40");
#endif
    /* the document parses to a number: indirect eval returns it unchanged */
    cc_case("number doc", N, "JA==", 1, "JA==", "b9e0da38");
    /* TextDecoder drops a leading BOM: "﻿\"6*7\"" -> 42 */
    cc_case("bom", N, "/vvRGSBJLQ8=", 1, "JXI=", "9ff4f420");
    /* invalid UTF-8 0xff -> U+FFFD inside the source's string literal */
    cc_case("bad utf8", N, "M2eRHDQ=", 1, "M6/RhjQ=", "cdbf9cf0");
    /* an embedded NUL makes JSON.parse throw (no truncation to "\"1+1\"") */
    cc_case("nul", N, "M3FFCjRj", 0, NULL, NULL);
    /* not base64 / not JSON */
    cc_case("bad b64", N, "M3F$CjQ=", 0, NULL, NULL);
    cc_case("not json", N, "MzQGSXkUOkNVW3s=", 0, NULL, NULL);
    /* non-ASCII nonce: keystream indexes UTF-16 units, hash folds them */
    cc_case("unicode nonce", "n\xc3\xb6nce\xf0\x9f\x98\x80", "B4JSWxUx", 1, "B8pWEg==", "37084b2a");
}

/* ---- sandbox (native QuickJS only; on the web the page itself evals) ---- */
#ifndef __EMSCRIPTEN__

static double secs_since(struct timespec t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
}

/* Runs raw JS; returns the rc and copies the result (or "<undefined>") into out. */
static int run_js(const char *src, char *out, size_t cap)
{
    char *json = NULL;
    int rc = krunker_cheat_eval_source(src, strlen(src), &json);
    snprintf(out, cap, "%s", rc ? "<no ccr>" : json ? json : "<undefined>");
    free(json);
    return rc;
}

static void expect_js(const char *src, const char *want)
{
    char got[512];
    run_js(src, got, sizeof got);
    CHECK(strcmp(got, want) == 0, "js `%s` = %s, want %s", src, got, want);
}

static void test_sandbox(void)
{
    char got[512];
    struct timespec t0;

    /* an endless loop is interrupted (uncatchable): no ccr, promptly */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = run_js("for (;;) {}", got, sizeof got);
    double dt = secs_since(t0);
    CHECK(rc != 0, "infinite loop must not answer (%s)", got);
    CHECK(dt < 5.0, "infinite loop took %.2fs", dt);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    rc = run_js("try { while (true) {} } catch (e) { 'escaped' }", got, sizeof got);
    CHECK(rc != 0 && secs_since(t0) < 5.0, "the payload cannot catch the interrupt (%s)", got);

    /* a huge allocation is capped by the memory limit, not satisfied */
    rc = run_js("new Uint8Array(1024 * 1024 * 1024).length", got, sizeof got);
    CHECK(strstr(got, "1073741824") == NULL, "1 GiB typed array must fail (%s)", got);
    rc = run_js("var a = []; for (var i = 0; i < 1e8; i++) a.push('x' + i); a.length", got, sizeof got);
    CHECK(strstr(got, "100000000") == NULL, "unbounded array growth must fail (%s)", got);
    rc = run_js("'x'.repeat(1 << 29).length", got, sizeof got);
    CHECK(strstr(got, "536870912") == NULL, "512M-char string must fail (%s)", got);
    /* deep recursion hits the stack limit as a catchable error */
    expect_js("function f(){ return f() + 1; } try { f(); 'no' } catch (e) { e instanceof RangeError || e instanceof InternalError }", "true");

    /* no host modules, loader or Node globals */
    expect_js("[typeof require, typeof std, typeof os, typeof process, typeof module, typeof exports,"
              " typeof scriptArgs, typeof print, typeof __loadScript, typeof Worker, typeof fetch,"
              " typeof XMLHttpRequest, typeof WebSocket, typeof __KN_DOC__, typeof __KN_RAW__].join()",
              "\"undefined,undefined,undefined,undefined,undefined,undefined,undefined,undefined,undefined,"
              "undefined,undefined,undefined,undefined,undefined,undefined\"");
    expect_js("var p = 'n'; try { import('os').then(function(){ p = 'y'; }, function(){ p = 'r'; }); } catch (e) { p = 'c'; } p",
              "\"n\""); /* no job pump: the import never settles, and there is no loader anyway */

    /* fresh context per challenge: nothing leaks between payloads */
    expect_js("globalThis.leak = 41; leak + 1", "42");
    expect_js("typeof leak", "\"undefined\"");

    /* browser stand-ins */
    expect_js("window === globalThis && self === window && top === window", "true");
    expect_js("navigator.webdriver === undefined && 'webdriver' in navigator"
              " && !Object.getOwnPropertyDescriptor(navigator, 'webdriver').configurable", "true");
    expect_js("typeof navigator.userAgent === 'string' && navigator.userAgent.indexOf('Chrome/') > 0"
              " && navigator.hardwareConcurrency > 0 && navigator.languages[0]", "\"en-US\"");
    expect_js("Function.prototype.toString.call(document.createElement).indexOf('[native code]') > 0"
              " && Function.prototype.toString.call(Function.prototype.toString).indexOf('[native code]') > 0"
              " && Function.prototype.toString.call(function x(){ return 1 }).indexOf('return 1') > 0", "true");
    expect_js("document.readyState + ',' + location.host + ',' + (performance.now() >= 0) + ',' + screen.width",
              "\"complete,krunker.io,true,1920\"");
    expect_js("localStorage.setItem('k', 5); [localStorage.getItem('k'), localStorage.length, btoa('hi'), atob('aGk=')]",
              "[\"5\",1,\"aGk=\",\"hi\"]");
}

#endif /* __EMSCRIPTEN__ */

int main(void)
{
    test_keystream();
    test_base64();
    test_fnv();
    test_round_trip();
#ifndef __EMSCRIPTEN__
    test_sandbox();
#endif
    if (g_fail) {
        fprintf(stderr, "krunker_cheat_test: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("krunker_cheat_test: OK\n");
    return 0;
}
