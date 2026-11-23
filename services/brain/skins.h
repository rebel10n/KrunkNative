/*
 * services/brain/skins.h - Krunker v10.0.7's Skin Manager records (custom skins, migration 0394_custom_skins):
 * the proc (skins.c) behind the /skins gapi routes on the social socket (skins_gapi) and on the brain root over
 * HTTP (v10_catalog.c).
 */
#ifndef KN_BRAIN_SKINS_H
#define KN_BRAIN_SKINS_H

#include "knsvc/store.h"

struct SocialSession;
struct GapiReq;

/* (op, actor, a, b, c, d, e) -> one JSON column; refusals are SKIN_* codes (skins_error)
 *   list (page, page_size, type | -1, weapon | -1, search)  -> {skins, total, page, page_size}
 *   get (id)                                                -> {skin}
 *   create (body JSON)                                      -> {skin}
 *   update (id, body JSON)                                  -> {skin}
 *   delete (id)                                             -> {deleted, skinId}
 *   duplicate (id)                                          -> {skin} */
extern const kn_proc P_CUSTOM_SKIN;

/* a SKIN_* refusal code -> its HTTP status and the message the window shows (500 / the code when unknown) */
int skins_error(const char *code, const char **message);

/* /skins...: 1 = the request was ours (answered now or later) */
int skins_gapi(struct SocialSession *s, const struct GapiReq *r);

#endif
