/*
 * services/brain/social_ws.h - the social WebSocket of the brain (Krunker
 * v3.1.1 matchmaker/socialWs.ts) at GET /api/ws and /social/ws.
 * Protocol: doc/backend.md "Accounts & social socket".
 */
#ifndef KN_BRAIN_SOCIAL_WS_H
#define KN_BRAIN_SOCIAL_WS_H

#include <stdint.h>

#include "cJSON.h"
#include "brain.h"

/* BrainModule hooks (main.c k_brain_modules), after accounts_init. */
int social_ws_init(Brain *b);
void social_ws_shutdown(Brain *b);

/* Push one frame ["label", ...] to every social socket logged in as account_id
 * (consumes frame). For other modules that want realtime notifications. */
void social_push_account(int64_t account_id, cJSON *frame);

/* The Junk balance changed (db_junk.c): ["uj", "12.34"] to the account's social sockets, so the navbar
 * and the Account window show it without a reply (doc/v10/junk.md). */
void social_push_junk(int64_t account_id, const char *junk);

/* Every signed-in JSON social socket of this brain gets the frame (consumed). */
void social_push_all_json(cJSON *frame);

/* Hub creation catalogue routes /api/maps, /api/mods, /api/assets, /api/search,
 * /api/streams, /api/prices (creations.c, v3.1.1 publicRoutes.ts). BrainModule hook. */
int creations_init(Brain *b);

/* Authenticated social sockets currently open. */
int social_ws_online(void);

#endif
