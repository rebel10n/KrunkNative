/* Unit test for services/strike/director_ticket.c (Krunker Strike director join tickets). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "director_ticket.h"
#include "knsvc/crypto.h"
#include "knsvc/loop.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

int main(void) {
    char key[65], other[65];
    CHECK(director_ticket_key("", key) == -1);
    CHECK(director_ticket_key(NULL, key) == -1);
    CHECK(director_ticket_key("a-shared-director-secret", key) == 0 && strlen(key) == 64);
    CHECK(director_ticket_key("another-secret", other) == 0 && strcmp(key, other) != 0);
    const int64_t now = kn_wall_ms() / 1000;
    char err[64], jti[40];
    DirectorTicket t;

    /* public ticket: verifies, carries gid / port / jti, no code */
    char *pub = director_ticket_issue(key, "eu:0a1b2c3d", 7777, NULL, 60, jti, sizeof jti);
    CHECK(pub && strlen(pub) <= DIRECTOR_TICKET_MAX && strlen(jti) == 32);
    CHECK(director_ticket_verify(key, pub, now, &t, err, sizeof err) == 0);
    CHECK(!strcmp(t.gid, "eu:0a1b2c3d") && t.port == 7777 && !strcmp(t.jti, jti) && t.code[0] == 0);
    CHECK(t.exp >= now + 59 && t.exp <= now + 61);

    /* private ticket: the code rides along */
    char *prv = director_ticket_issue(key, "eu:0a1b2c3d", 7777, "K7QX2M", 60, NULL, 0);
    CHECK(prv && director_ticket_verify(key, prv, now, &t, err, sizeof err) == 0 && !strcmp(t.code, "K7QX2M"));

    /* another secret, a tampered ticket, garbage, expiry */
    CHECK(director_ticket_verify(other, pub, now, &t, err, sizeof err) == -1);
    char *bad = strdup(pub);
    bad[strlen(bad) - 3] = bad[strlen(bad) - 3] == 'A' ? 'B' : 'A';
    CHECK(director_ticket_verify(key, bad, now, &t, err, sizeof err) == -1);
    free(bad);
    CHECK(director_ticket_verify(key, "garbage", now, &t, err, sizeof err) == -1);
    CHECK(director_ticket_verify(key, "", now, &t, err, sizeof err) == -1);
    CHECK(director_ticket_verify(key, pub, now + 61, &t, err, sizeof err) == -1 && !strcmp(err, "expired"));

    /* the same key signing something that is not a director ticket (typ) is refused */
    cJSON *claims = cJSON_Parse("{\"typ\":\"mmv\",\"gid\":\"x\",\"port\":7777,\"jti\":\"00\"}");
    char *foreign = kn_signed_token_create(key, claims, 60);
    CHECK(foreign && director_ticket_verify(key, foreign, now, &t, err, sizeof err) == -1 &&
          !strcmp(err, "not a director ticket"));
    cJSON_Delete(claims);
    free(foreign);

    /* a secret's raw value (no KDF) does not verify either: the label separates the keys */
    char *raw = NULL;
    claims = cJSON_Parse("{\"typ\":\"sdt\",\"gid\":\"x\",\"port\":7777,\"jti\":\"00\"}");
    raw = kn_signed_token_create("a-shared-director-secret", claims, 60);
    CHECK(raw && director_ticket_verify(key, raw, now, &t, err, sizeof err) == -1);
    cJSON_Delete(claims);
    free(raw);

    /* bad inputs */
    CHECK(director_ticket_issue(key, "g", 0, NULL, 60, NULL, 0) == NULL);
    CHECK(director_ticket_issue("", "g", 7777, NULL, 60, NULL, 0) == NULL);

    free(pub);
    free(prv);
    printf("director_ticket: %d checks, %s\n", g_checks, g_fail ? "FAILED" : "ok");
    return g_fail;
}
