/*
 * services/brain/v10_catalog.h - the v10 catalog routes at the brain root (skins, wild offer, maps versions, data, leaderboard, votes, feature, delete)
 */
#ifndef KN_BRAIN_V10_CATALOG_H
#define KN_BRAIN_V10_CATALOG_H

#include "brain_internal.h"
#include "knsvc/http.h"

int v10_catalog_init(Brain *b);

/* GET /maps/by-name/:name for a published (community) map, after v10_api.c found no official map of that
 * name: answers the Map Detail record (deob-10.0.3.js:121106-121532), or 404 MAP_NOT_FOUND. */
void v10_catalog_map_by_name(kn_http_req *req, const char *name);

#endif
