/*
 * services/strike/director_ticket.c - the Krunker Strike director's join ticket
 * (director_ticket.h).
 */
#include "director_ticket.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "knsvc/crypto.h"

int director_ticket_key(const char *secret, char key[65]) {
    key[0] = 0;
    if (!secret || !*secret) return -1;
    /* hashed like mm_token.c's key: any secret length works, and the label keeps director
     * tickets apart from brain join tickets / validation tokens signed with the same secret */
    return kn_hmac_sha256_hex(secret, DIRECTOR_TICKET_KDF_LABEL, strlen(DIRECTOR_TICKET_KDF_LABEL), key);
}

char *director_ticket_issue(const char *key, const char *gid, const int port, const char *code, const int ttl_s,
                            char *jti_out, const size_t jti_cap) {
    if (!key || !key[0] || !gid || port <= 0 || port > 65535) return NULL;
    char jti[33];
    if (kn_random_hex(jti, sizeof jti, 16) != 0) return NULL;
    cJSON *claims = cJSON_CreateObject();
    if (!claims) return NULL;
    cJSON_AddStringToObject(claims, "typ", DIRECTOR_TICKET_TYP);
    cJSON_AddStringToObject(claims, "gid", gid);
    cJSON_AddNumberToObject(claims, "port", port);
    cJSON_AddStringToObject(claims, "jti", jti);
    if (code && *code) cJSON_AddStringToObject(claims, "prv", code);
    char *ticket = kn_signed_token_create(key, claims, ttl_s > 0 ? ttl_s : 60);
    cJSON_Delete(claims);
    if (ticket && strlen(ticket) > DIRECTOR_TICKET_MAX) {
        free(ticket);
        return NULL;
    }
    if (ticket && jti_out && jti_cap) snprintf(jti_out, jti_cap, "%s", jti);
    return ticket;
}

int director_ticket_verify(const char *key, const char *ticket, const int64_t now_s, DirectorTicket *out, char *err,
                           const size_t errlen) {
    if (err && errlen) err[0] = 0;
    memset(out, 0, sizeof(*out));
    if (!key || !key[0] || !ticket || !ticket[0]) {
        if (err) snprintf(err, errlen, "no ticket");
        return -1;
    }
    char why[64] = "";
    cJSON *claims = kn_signed_token_verify(key, ticket, now_s, why, sizeof why);
    if (!claims) {
        if (err) snprintf(err, errlen, "%s", strstr(why, "exp") ? "expired" : why[0] ? why : "signature");
        return -1;
    }
    const cJSON *typ = cJSON_GetObjectItemCaseSensitive(claims, "typ");
    const cJSON *gid = cJSON_GetObjectItemCaseSensitive(claims, "gid");
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(claims, "port");
    const cJSON *jti = cJSON_GetObjectItemCaseSensitive(claims, "jti");
    const cJSON *prv = cJSON_GetObjectItemCaseSensitive(claims, "prv");
    const cJSON *exp = cJSON_GetObjectItemCaseSensitive(claims, "exp");
    int rc = 0;
    if (!cJSON_IsString(typ) || strcmp(typ->valuestring, DIRECTOR_TICKET_TYP) != 0 || !cJSON_IsString(gid) ||
        !cJSON_IsNumber(port) || !cJSON_IsString(jti)) {
        if (err) snprintf(err, errlen, "not a director ticket");
        rc = -1;
    } else {
        snprintf(out->gid, sizeof out->gid, "%s", gid->valuestring);
        out->port = port->valueint;
        snprintf(out->jti, sizeof out->jti, "%s", jti->valuestring);
        if (cJSON_IsString(prv)) snprintf(out->code, sizeof out->code, "%s", prv->valuestring);
        out->exp = cJSON_IsNumber(exp) ? (int64_t) exp->valuedouble : 0;
    }
    cJSON_Delete(claims);
    return rc;
}
