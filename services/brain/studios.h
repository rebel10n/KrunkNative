/*
 * services/brain/studios.h - Krunker v10.0.2's Studios: the gapi routes (studios.c) and their proc
 * (db_studios.c, migration 0330_studios).
 */
#ifndef KN_BRAIN_STUDIOS_H
#define KN_BRAIN_STUDIOS_H

#include "knsvc/store.h"

struct SocialSession;
struct GapiReq;

/* (op, actor, a, b, c) -> one JSON column; refusals are STUDIO_* / MONETIZATION_* codes */
extern const kn_proc P_STUDIO;

/* /studios/...: 1 = the request was ours (answered now or later) */
int studios_gapi(struct SocialSession *s, const struct GapiReq *r);

#endif
