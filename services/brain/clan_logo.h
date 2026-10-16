/*
 * services/brain/clan_logo.h - clan logos (clan_logo.c): the upload from the clan settings tab
 * (POST /clans/<id>/logo over the gapi tunnel, dispatched by clan_v10.c) and the approved logo's
 * route (GET /api/clans/<id>/logo).
 */
#ifndef KN_BRAIN_CLAN_LOGO_H
#define KN_BRAIN_CLAN_LOGO_H

#include <stdint.h>

#include "acct_internal.h"
#include "brain.h"

int clan_logo_init(Brain *b);
void clan_logo_shutdown(Brain *b);

/* POST /clans/<clan>/logo: body {file: base64 PNG}. me is the viewer's account id (text). Answers the
 * gapi request rid itself (200 {status, clan_logo_pending, clan_funds} or an error). */
void clan_logo_upload(SocialSession *s, int64_t rid, const char *me, int64_t clan, const cJSON *body);

#endif
