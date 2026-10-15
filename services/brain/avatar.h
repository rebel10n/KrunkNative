/*
 * services/brain/avatar.h - profile pictures (avatar.c): upload, AI review, storage, serving.
 * Routes and rules: the comment at the top of avatar.c.
 */
#ifndef KN_BRAIN_AVATAR_H
#define KN_BRAIN_AVATAR_H

#include "brain.h"

int avatar_init(Brain *b);
void avatar_shutdown(Brain *b);

#endif
