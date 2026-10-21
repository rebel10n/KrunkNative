/*
 * services/brain/v10_auth.c - the v10 auth routes at the brain root (/auth/* of gapi: nonce, login, challenges, verification, recovery, kid consent), mapped onto the accounts of auth.c
 * Routes are added here; see doc/v10/api-inventory.md for each route's status and source.
 */
#include "v10_auth.h"

int v10_auth_init(Brain *b) {
    (void) b;
    return 0;
}
