/*
 * services/brain/v10_auth.h - the v10 auth routes at the brain root (/auth/* of gapi: nonce, login, challenges, verification, recovery, kid consent), mapped onto the accounts of auth.c
 */
#ifndef KN_BRAIN_V10_AUTH_H
#define KN_BRAIN_V10_AUTH_H

#include "brain_internal.h"

int v10_auth_init(Brain *b);

#endif
