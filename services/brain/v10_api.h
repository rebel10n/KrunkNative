/*
 * services/brain/v10_api.h - the Krunker v10 REST surface on the brain's root paths (gapi
 * https://gapi.svc.krunker.io and api.krunker.io, which the harness and the brain answer at "/").
 * Shapes: v10.js (the REST client _10077 and _9997, doc/v10/api-inventory.md). Every gapi body
 * is {"data": ..., "error": null} on success and {"error": {"code", "message"}} on refusal.
 */
#ifndef KN_BRAIN_V10_API_H
#define KN_BRAIN_V10_API_H

#include "brain_internal.h"
#include "knsvc/http.h"

/* {"data": data} with the status (data is consumed) */
void v10_reply_data(kn_http_req *req, int status, cJSON *data);
/* {"error": {"code": code, "message": message}} with the status (v10's gapi error shape) */
void v10_reply_error(kn_http_req *req, int status, const char *code, const char *message);

/* Registers the root routes this module answers (maps, titles, the packed skins and spins). */
int v10_api_init(Brain *b);

#endif
