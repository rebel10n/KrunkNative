/*
 * services/brain/roles.h - which jobs this kn_brain process does (config `brain.roles`,
 * doc/backend.md "Coordinator roles"). A list (or "a,b" string) of:
 *
 *   auth       central identity: /auth(...) (register, login, sessions, join / hasJoined)   auth.c
 *   community  the community coordinator: matchmaker /mm(...), hub /api(...), social WS,
 *              internal WS for game servers, inventory/stats/ranks keyed by account id
 *   finder     public server list: /servers(...) (heartbeats + KNINFO polling)             finder.c
 *
 * Unset = all three (one local process does everything, as before). CLI shortcut:
 * `--role auth,finder` (same as --brain.roles=auth,finder), env KN_BRAIN_ROLES.
 */
#ifndef KN_BRAIN_ROLES_H
#define KN_BRAIN_ROLES_H

#include "brain.h"

enum {
    BRAIN_ROLE_AUTH = 1 << 0,
    BRAIN_ROLE_COMMUNITY = 1 << 1,
    BRAIN_ROLE_FINDER = 1 << 2,
    BRAIN_ROLE_ALL = 7,
};

/* Parse brain.roles (unknown names are logged and ignored). Returns the mask (0 = error:
 * nothing valid configured). Called once by main.c after the config is loaded. */
unsigned brain_roles_load(const struct kn_config *cfg);
unsigned brain_roles(void);
int brain_has_role(unsigned role);
/* "auth,community" text of the mask (static buffer). */
const char *brain_roles_text(void);
/* 1 when the k_brain_modules entry `name` runs with the configured roles. */
int brain_module_enabled(const char *name);

/* auth.c (BRAIN_ROLE_AUTH) */
int auth_init(Brain *b);
void auth_shutdown(Brain *b);

/* hasJoined for the community role (internal WS "auth" {hasJoined}): answered from this
 * process's join records when it also has the auth role, else GET <community.auth_url>
 * /auth/hasJoined on a worker thread (libcurl when built with it; http:// otherwise).
 * cb runs on the loop thread once: profile {id, username, ...} or NULL + err. */
typedef void (*AuthHasJoinedCb)(void *ud, const char *err, const cJSON *profile);
void auth_has_joined_async(Brain *b, const char *username, const char *server_hash, const char *ip,
                           AuthHasJoinedCb cb, void *ud);

/* The per-IP login limit of POST /auth/login (auth.c), shared with the v10 login routes (v10_auth.c):
 * 1 = go on; 0 = over the limit, the 429 is sent. */
int auth_login_limit(struct kn_http_req *req);

#endif
