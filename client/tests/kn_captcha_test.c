// kn_captcha_test: Box Office's captcha in the client (client/src/net/kn_captcha.c, client/include/kn_captcha.h) and
// the ALTCHA search it uses (client/src/net/krunker_altcha.c), with a stand-in brain: backend_http_get is a double
// that holds the request, and the test answers it. Checked: the challenge request, the solution found over frames and
// its payload (the 1.0.6 widget's value, which services/brain/captcha.c verifies: the known answer below is the brain
// test's, services/brain/tests/test_captcha.c), a brain with no route (404) / no answer / a refused challenge (sent
// without one), cancel, the deadline, and the sign-in frame (its captcha slot, its strings wiped).
#include <kn_captcha.h>
#include <backend.h>
#include <krunker_altcha.h>
#include <auth_join.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_kn_debug_level = 0; // klog.h

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

// ---- the backend double: one request held at a time
static int g_enabled = 1, g_queue_ok = 1, g_requests;
static char g_path[256];
static BackendHttpFn g_fn;
static void *g_user;

int backend_enabled(void) { return g_enabled; }
int backend_http_get(const char *path, BackendHttpFn fn, void *user) {
    if (!g_queue_ok) return 0;
    g_requests++;
    snprintf(g_path, sizeof(g_path), "%s", path);
    g_fn = fn;
    g_user = user;
    return 1;
}
static void answer(const int status, const char *body) {
    BackendHttpFn fn = g_fn;
    g_fn = NULL;
    if (fn) fn(g_user, status, body, body ? strlen(body) : 0);
}

// ---- done callbacks
static int g_done;
static char g_payload[4096];
static int g_had_payload;
static void on_done(void *user, const char *payload) {
    (void) user;
    g_done++;
    g_had_payload = payload != NULL;
    snprintf(g_payload, sizeof(g_payload), "%s", payload ? payload : "");
}

static char g_sent[8192];
static int g_sends;
static void on_send(const cJSON *frame) {
    char *s = cJSON_PrintUnformatted(frame);
    snprintf(g_sent, sizeof(g_sent), "%s", s ? s : "");
    cJSON_free(s);
    g_sends++;
}

// ---- helpers
static void sha_hex(const char *s, char out[65]) {
    uint8_t d[32];
    kn_sha256_bytes(s, strlen(s), d);
    kn_sha256_hex(d, out);
}

static int b64v(const char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}
static size_t b64_decode(const char *in, char *out, const size_t cap) {
    size_t o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (; *in && *in != '='; in++) {
        const int v = b64v(*in);
        if (v < 0) return 0;
        acc = acc << 6 | (unsigned) v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o + 1 >= cap) return 0;
            out[o++] = (char) (acc >> bits & 0xff);
        }
    }
    out[o] = 0;
    return o;
}

// a challenge JSON the way services/brain/captcha.c issues one, for a number of our choosing
static void make_challenge(char *out, const size_t n, const char *salt, const long number, const long maxnumber) {
    char buf[256], hex[65];
    snprintf(buf, sizeof(buf), "%s%ld", salt, number);
    sha_hex(buf, hex);
    snprintf(out, n, "{\"algorithm\":\"SHA-256\",\"challenge\":\"%s\",\"maxnumber\":%ld,\"salt\":\"%s\",\"signature\":\"%064d\"}",
             hex, maxnumber, salt, 7);
}

static void run_frames(const int frames) {
    for (int i = 0; i < frames && kn_captcha_pending(); i++) kn_captcha_tick();
}

int main(void) {
    // 1. the brain test's known answer (services/brain/tests/test_captcha.c): the search finds 4242, the payload
    //    carries the challenge's fields as the 1.0.6 widget's value does
    {
        const char *salt = "00112233445566778899aabb?expires=2000000000&for=login";
        const char *ch = "fc558ed24b8107cbf70d47555129c1d8a2757230d9a534c65827b3c0e290e186";
        const char *sig = "8442973563c19892d2de170874cae31db1e6e9916fee28ff94e98b3ab1a94d64";
        char *payload = NULL;
        CHECK(krunker_altcha_solve("SHA-256", ch, salt, sig, 5000, &payload) == 0 && payload);
        char json[2048];
        CHECK(payload && b64_decode(payload, json, sizeof(json)) > 0);
        cJSON *o = cJSON_Parse(json);
        CHECK(o && cJSON_GetArraySize(o) == 6); // algorithm, challenge, number, salt, signature, took (no test)
        CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(o, "algorithm")), "SHA-256"));
        CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(o, "challenge")), ch));
        CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(o, "salt")), salt));
        CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(o, "signature")), sig));
        CHECK(cJSON_GetObjectItem(o, "number") && cJSON_GetObjectItem(o, "number")->valuedouble == 4242);
        CHECK(cJSON_IsNumber(cJSON_GetObjectItem(o, "took")));
        const char *first = json + 1; // the widget's key order (JSON.stringify of its object literal)
        CHECK(!strncmp(first, "\"algorithm\":", 12) && strstr(json, "\"challenge\":") < strstr(json, "\"number\":") &&
              strstr(json, "\"number\":") < strstr(json, "\"salt\":") && strstr(json, "\"signature\":") < strstr(json, "\"took\":"));
        cJSON_Delete(o);
        free(payload);
        // a number past maxnumber is not found; SHA-512 and a maxnumber over the cap are refused
        CHECK(krunker_altcha_solve("SHA-256", ch, salt, sig, 4000, &payload) == -1 && !payload);
        CHECK(krunker_altcha_solve("SHA-512", ch, salt, sig, 5000, &payload) == -1);
        CHECK(krunker_altcha_solve("SHA-256", ch, salt, sig, KRUNKER_ALTCHA_MAX_CAP + 1, &payload) == -1);
    }

    // 2. a captcha: the request, then the search over frames, then done with the payload
    {
        char challenge[1024];
        make_challenge(challenge, sizeof(challenge), "abcdefabcdefabcdefabcdef?expires=2000000000&for=login", 77777, 100000);
        g_done = 0;
        const unsigned id = kn_captcha_request("login", on_done, NULL);
        CHECK(id > 0 && kn_captcha_pending() == 1 && g_requests == 1);
        CHECK(!strcmp(g_path, "/mm/captcha?for=login"));
        CHECK(g_done == 0); // never from inside the request
        answer(200, challenge);
        CHECK(g_done == 0); // the search runs from the frames
        run_frames(2000);
        CHECK(g_done == 1 && g_had_payload && kn_captcha_pending() == 0);
        char json[2048];
        CHECK(b64_decode(g_payload, json, sizeof(json)) > 0 && strstr(json, "\"number\":77777,"));
    }

    // 3. no challenge to send: the brain has no route (404), no answer (0), a refused challenge: done(NULL) at once
    {
        const struct {
            int status;
            const char *body;
        } cases[] = {
            {404, "{\"error\":\"not_found\",\"path\":\"/mm/captcha?for=login\"}"},
            {0, "cannot connect"},
            {503, "{\"success\":false,\"error\":\"Unavailable\"}"},
            {200, "{\"algorithm\":\"SHA-512\",\"challenge\":\"00\",\"salt\":\"x\",\"signature\":\"y\"}"},
            {200, "not json"},
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            g_done = 0;
            CHECK(kn_captcha_request("register", on_done, NULL) > 0);
            CHECK(!strcmp(g_path, "/mm/captcha?for=register"));
            answer(cases[i].status, cases[i].body);
            CHECK(g_done == 1 && !g_had_payload && kn_captcha_pending() == 0);
        }
        // a challenge whose number is past its maxnumber: searched to the end, then sent without one
        char challenge[1024];
        make_challenge(challenge, sizeof(challenge), "1234?expires=2000000000&for=hosting", 9000, 5000);
        g_done = 0;
        CHECK(kn_captcha_request("hosting", on_done, NULL) > 0);
        answer(200, challenge);
        run_frames(2000);
        CHECK(g_done == 1 && !g_had_payload);
    }

    // 4. none can start: no brain URL, a request the backend did not queue, every slot busy -> 0, done never called
    {
        g_done = 0;
        g_enabled = 0;
        CHECK(kn_captcha_request("login", on_done, NULL) == 0);
        g_enabled = 1;
        g_queue_ok = 0;
        CHECK(kn_captcha_request("login", on_done, NULL) == 0);
        g_queue_ok = 1;
        CHECK(kn_captcha_request(NULL, on_done, NULL) == 0 && kn_captcha_request("", on_done, NULL) == 0);
        unsigned ids[KN_CAPTCHA_SLOTS];
        for (int i = 0; i < KN_CAPTCHA_SLOTS; i++) ids[i] = kn_captcha_request("login", on_done, NULL);
        CHECK(ids[KN_CAPTCHA_SLOTS - 1] > 0 && kn_captcha_request("login", on_done, NULL) == 0);
        for (int i = 0; i < KN_CAPTCHA_SLOTS; i++) kn_captcha_cancel(ids[i]);
        CHECK(kn_captcha_pending() == 0 && g_done == 0);
    }

    // 5. cancel: a late challenge answer is dropped, done is not called
    {
        g_done = 0;
        const unsigned id = kn_captcha_request("login", on_done, NULL);
        kn_captcha_cancel(id);
        char challenge[1024];
        make_challenge(challenge, sizeof(challenge), "aa?expires=2000000000&for=login", 3, 1000);
        answer(200, challenge);
        run_frames(10);
        CHECK(g_done == 0 && kn_captcha_pending() == 0);
        kn_captcha_cancel(id); // twice, and unknown ids: ignored
        kn_captcha_cancel(0);
    }

    // 6. the sign-in frame: sent once solved, the payload in its slot (index 3), its strings wiped after
    {
        char challenge[1024];
        make_challenge(challenge, sizeof(challenge), "bb?expires=2000000000&for=login", 4321, 10000);
        cJSON *f = cJSON_Parse("[\"a\",1,[\"player\",\"secret-pass\",null]]");
        cJSON *pass = cJSON_GetArrayItem(cJSON_GetArrayItem(f, 2), 1);
        (void) pass;
        g_sends = 0;
        const unsigned id = kn_captcha_send_frame("login", f, 3, on_send);
        CHECK(id > 0 && g_sends == 0);
        answer(200, challenge);
        run_frames(2000);
        CHECK(g_sends == 1);
        cJSON *sent = cJSON_Parse(g_sent);
        CHECK(sent && cJSON_GetArraySize(sent) == 4 && cJSON_IsString(cJSON_GetArrayItem(sent, 3)));
        CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetArrayItem(cJSON_GetArrayItem(sent, 2), 1)), "secret-pass"));
        char json[2048];
        CHECK(b64_decode(cJSON_GetStringValue(cJSON_GetArrayItem(sent, 3)), json, sizeof(json)) > 0 && strstr(json, "\"number\":4321,"));
        cJSON_Delete(sent);
        // a brain from before the route: the frame goes with null in the slot
        f = cJSON_Parse("[\"a\",0,[\"player\",\"secret-pass\",null,\"p@example.org\"]]");
        g_sends = 0;
        CHECK(kn_captcha_send_frame("register", f, 3, on_send) > 0);
        answer(404, "{}");
        CHECK(g_sends == 1 && !strcmp(g_sent, "[\"a\",0,[\"player\",\"secret-pass\",null,\"p@example.org\"],null]"));
        // no captcha can start: sent at once with null (a shorter frame is filled up to the slot)
        g_enabled = 0;
        g_sends = 0;
        f = cJSON_Parse("[\"a\",1]");
        CHECK(kn_captcha_send_frame("login", f, 3, on_send) == 0 && g_sends == 1 && !strcmp(g_sent, "[\"a\",1,null,null]"));
        g_enabled = 1;
        // cancelled: never sent
        g_sends = 0;
        f = cJSON_Parse("[\"a\",1,[\"x\",\"y\",null]]");
        const unsigned id2 = kn_captcha_send_frame("login", f, 3, on_send);
        kn_captcha_cancel(id2);
        answer(404, "{}");
        CHECK(g_sends == 0 && kn_captcha_pending() == 0);
    }

    // 7. a challenge a game server gave (its in-game "cpt", kn_captcha_game.c): no request, solved over frames
    {
        char challenge[1024];
        make_challenge(challenge, sizeof(challenge), "cc?expires=2000000000&for=game", 31337, 50000);
        g_done = 0;
        const int requests = g_requests;
        const unsigned id = kn_captcha_solve("game", challenge, on_done, NULL);
        CHECK(id > 0 && g_requests == requests && g_done == 0 && kn_captcha_pending() == 1);
        run_frames(2000);
        char json[2048];
        CHECK(g_done == 1 && g_had_payload && b64_decode(g_payload, json, sizeof(json)) > 0 && strstr(json, "\"number\":31337,"));
        // one it cannot solve here: refused at once (the caller closes with CAPTCHA FAILED), done never called
        g_done = 0;
        CHECK(kn_captcha_solve("game", "{\"algorithm\":\"SHA-512\",\"challenge\":\"00\",\"salt\":\"x\"}", on_done, NULL) == 0);
        CHECK(kn_captcha_solve("game", "not json", on_done, NULL) == 0 && kn_captcha_solve("game", NULL, on_done, NULL) == 0);
        make_challenge(challenge, sizeof(challenge), "dd", 1, KRUNKER_ALTCHA_MAX_CAP + 1);
        CHECK(kn_captcha_solve("game", challenge, on_done, NULL) == 0 && g_done == 0 && kn_captcha_pending() == 0);
        // cancelled while solving: done never called
        make_challenge(challenge, sizeof(challenge), "ee", 999999, 1000000);
        const unsigned id2 = kn_captcha_solve("game", challenge, on_done, NULL);
        kn_captcha_cancel(id2);
        run_frames(10);
        CHECK(g_done == 0 && kn_captcha_pending() == 0);
    }

    if (g_fail) return 1;
    printf("kn_captcha_test: ok\n");
    return 0;
}
