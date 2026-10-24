// Box Office's captcha: the brain's ALTCHA challenge, fetched and solved here (client/include/kn_captcha.h,
// doc/v10/captcha.md). The search is krunker_altcha.c's (the same proof of work as official games' "cpt").
#include <kn_captcha.h>
#include <backend.h>
#include <krunker_altcha.h>
#include "klog.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STEP_NUMBERS 2048L // numbers tried between two looks at the clock

typedef enum { SLOT_FREE = 0, SLOT_FETCH, SLOT_SOLVE } SlotState;

typedef struct {
    SlotState state;
    unsigned id;
    char purpose[16];
    double started_ms; // the request's start: KN_CAPTCHA_DEADLINE_MS counts from it
    KnCaptchaDone done;
    void *user;
    // kn_captcha_send_frame: the frame waiting for its captcha
    cJSON *frame;
    int frame_slot;
    KnCaptchaSendFrame send;
    KrunkerAltcha a;
} Slot;

static Slot g_slots[KN_CAPTCHA_SLOTS];
static unsigned g_next_id = 1;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6;
}

static Slot *slot_of(const unsigned id) {
    for (int i = 0; i < KN_CAPTCHA_SLOTS; i++) {
        if (g_slots[i].state != SLOT_FREE && g_slots[i].id == id) return &g_slots[i];
    }
    return NULL;
}

// every string of j zeroed before it is freed (a sign-in frame carries the password)
static void wipe_strings(cJSON *j) {
    for (cJSON *c = j ? j->child : NULL; c; c = c->next) {
        if (cJSON_IsString(c) && c->valuestring) memset(c->valuestring, 0, strlen(c->valuestring));
        if (c->child) wipe_strings(c);
    }
}

static void free_frame(cJSON *frame) {
    wipe_strings(frame);
    cJSON_Delete(frame);
}

// frame[idx] = the payload, or null without one (nulls fill a shorter frame up to it)
static void put_captcha(cJSON *frame, const int idx, const char *payload) {
    while (cJSON_GetArraySize(frame) < idx) cJSON_AddItemToArray(frame, cJSON_CreateNull());
    cJSON *v = payload ? cJSON_CreateString(payload) : cJSON_CreateNull();
    if (cJSON_GetArraySize(frame) == idx) cJSON_AddItemToArray(frame, v);
    else cJSON_ReplaceItemInArray(frame, idx, v);
}

// Ends a captcha: its frame is sent, or done is called. The slot is free before either runs (they may start another).
static void finish(Slot *s, const char *payload) {
    Slot c = *s;
    memset(s, 0, sizeof(*s));
    if (c.frame) {
        put_captcha(c.frame, c.frame_slot, payload);
        c.send(c.frame);
        free_frame(c.frame);
    } else if (c.done) {
        c.done(c.user, payload);
    }
}

static void on_challenge(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    Slot *s = slot_of((unsigned) (uintptr_t) user);
    if (!s || s->state != SLOT_FETCH) return; // cancelled meanwhile
    cJSON *c = status == 200 && body ? cJSON_Parse(body) : NULL;
    const cJSON *max = cJSON_GetObjectItemCaseSensitive(c, "maxnumber");
    const int ok = c && krunker_altcha_begin(&s->a, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(c, "algorithm")),
                                             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(c, "challenge")),
                                             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(c, "salt")),
                                             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(c, "signature")),
                                             cJSON_IsNumber(max) && max->valuedouble >= 1 ? (long) max->valuedouble : 0) == 0;
    cJSON_Delete(c);
    if (!ok) {
        // 404: a brain from before the route; anything else: no usable challenge. The request goes without one
        KN_LOG_DEBUG1("[captcha] %s: no challenge (HTTP %d): sent without one\n", s->purpose, status);
        finish(s, NULL);
        return;
    }
    s->state = SLOT_SOLVE;
}

static unsigned start(const char *purpose, const KnCaptchaDone done, void *user, cJSON *frame, const int frame_slot,
                      const KnCaptchaSendFrame send) {
    if (!purpose || !purpose[0] || strlen(purpose) >= sizeof(g_slots[0].purpose) || !backend_enabled()) return 0;
    Slot *s = NULL;
    for (int i = 0; i < KN_CAPTCHA_SLOTS && !s; i++) {
        if (g_slots[i].state == SLOT_FREE) s = &g_slots[i];
    }
    if (!s) return 0;
    const unsigned id = g_next_id++;
    if (!g_next_id) g_next_id = 1;
    memset(s, 0, sizeof(*s));
    s->state = SLOT_FETCH;
    s->id = id;
    snprintf(s->purpose, sizeof(s->purpose), "%s", purpose);
    s->started_ms = now_ms();
    s->done = done;
    s->user = user;
    s->frame = frame;
    s->frame_slot = frame_slot;
    s->send = send;
    char path[64];
    snprintf(path, sizeof(path), "/mm/captcha?for=%s", purpose);
    if (!backend_http_get(path, on_challenge, (void *) (uintptr_t) id)) {
        memset(s, 0, sizeof(*s)); // not queued (the brain gate, out of memory): the caller sends without one
        return 0;
    }
    return id;
}

unsigned kn_captcha_request(const char *purpose, const KnCaptchaDone done, void *user) {
    return start(purpose, done, user, NULL, 0, NULL);
}

unsigned kn_captcha_solve(const char *purpose, const char *challenge_json, const KnCaptchaDone done, void *user) {
    if (!purpose || !purpose[0] || strlen(purpose) >= sizeof(g_slots[0].purpose) || !challenge_json) return 0;
    Slot *s = NULL;
    for (int i = 0; i < KN_CAPTCHA_SLOTS && !s; i++) {
        if (g_slots[i].state == SLOT_FREE) s = &g_slots[i];
    }
    if (!s) return 0;
    cJSON *c = cJSON_Parse(challenge_json);
    const cJSON *max = cJSON_GetObjectItemCaseSensitive(c, "maxnumber");
    memset(s, 0, sizeof(*s));
    const int ok = c && krunker_altcha_begin(&s->a, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(c, "algorithm")),
                                             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(c, "challenge")),
                                             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(c, "salt")),
                                             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(c, "signature")),
                                             cJSON_IsNumber(max) && max->valuedouble >= 1 ? (long) max->valuedouble : 0) == 0;
    cJSON_Delete(c);
    if (!ok) {
        memset(s, 0, sizeof(*s));
        return 0;
    }
    const unsigned id = g_next_id++;
    if (!g_next_id) g_next_id = 1;
    s->state = SLOT_SOLVE;
    s->id = id;
    snprintf(s->purpose, sizeof(s->purpose), "%s", purpose);
    s->started_ms = now_ms();
    s->done = done;
    s->user = user;
    return id;
}

unsigned kn_captcha_send_frame(const char *purpose, cJSON *frame, const int slot, const KnCaptchaSendFrame send) {
    if (!frame || !send || slot < 0) {
        if (frame) free_frame(frame);
        return 0;
    }
    const unsigned id = start(purpose, NULL, NULL, frame, slot, send);
    if (!id) {
        put_captcha(frame, slot, NULL);
        send(frame);
        free_frame(frame);
    }
    return id;
}

void kn_captcha_cancel(const unsigned id) {
    Slot *s = id ? slot_of(id) : NULL;
    if (!s) return;
    if (s->frame) free_frame(s->frame);
    memset(s, 0, sizeof(*s)); // a late challenge answer finds no slot of its id
}

int kn_captcha_pending(void) {
    int n = 0;
    for (int i = 0; i < KN_CAPTCHA_SLOTS; i++) n += g_slots[i].state != SLOT_FREE;
    return n;
}

void kn_captcha_tick(void) {
    const double t0 = now_ms();
    for (int i = 0; i < KN_CAPTCHA_SLOTS; i++) {
        Slot *s = &g_slots[i];
        if (s->state == SLOT_SOLVE && t0 - s->started_ms > KN_CAPTCHA_DEADLINE_MS) {
            KN_LOG_DEBUG1("[captcha] %s: not solved within %.0f s: sent without one\n", s->purpose, KN_CAPTCHA_DEADLINE_MS / 1000.0);
            finish(s, NULL);
        }
    }
    // the frame's slice, round robin over the captchas being solved
    for (int any = 1; any && now_ms() - t0 < KN_CAPTCHA_SLICE_MS;) {
        any = 0;
        for (int i = 0; i < KN_CAPTCHA_SLOTS; i++) {
            Slot *s = &g_slots[i];
            if (s->state != SLOT_SOLVE) continue;
            any = 1;
            char *payload = NULL;
            const int r = krunker_altcha_step(&s->a, STEP_NUMBERS, &payload);
            if (r == 0) continue;
            if (r == 1) KN_LOG_DEBUG1("[captcha] %s: solved (number %ld, %.0f ms)\n", s->purpose, s->a.next, now_ms() - s->started_ms);
            else KN_LOG_DEBUG1("[captcha] %s: no solution up to maxnumber: sent without one\n", s->purpose);
            finish(s, r == 1 ? payload : NULL);
            free(payload);
        }
    }
}
