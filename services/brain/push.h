/*
 * services/brain/push.h - Web Push subscriptions (push.c): the subscribe routes' module hooks.
 * The rules are push_rules.h; the SQL is db_push.c.
 */
#ifndef KN_BRAIN_PUSH_H
#define KN_BRAIN_PUSH_H

#include "brain.h"

/* BrainModule hook (main.c k_brain_modules): registers /api/push/vapid-key, /subscribe, /unsubscribe. */
int push_init(Brain *b);

#endif
